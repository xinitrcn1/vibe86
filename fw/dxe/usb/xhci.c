/*
 * UsbDxe: an xHCI host controller driver with HID boot keyboards and mice
 * and Bulk-Only mass storage, ported from the vercpu firmware's driver
 * (~/vercpu/fw/dxe/usb.c) and extended with bulk endpoints.
 *
 * References (section numbers in the comments):
 *   [xHCI]  Intel, "eXtensible Host Controller Interface for Universal Serial
 *           Bus (xHCI)", revision 1.2, May 2019.
 *   [USB2]  USB-IF, "Universal Serial Bus Specification", revision 2.0,
 *           April 2000, chapter 9 (requests, descriptors).
 *   [HID]   USB-IF, "Device Class Definition for Human Interface Devices
 *           (HID)", version 1.11, June 2001 (7.2 class requests, Appendix B).
 *   [HUT]   USB-IF, "HID Usage Tables", Keyboard/Keypad page (07h).
 *   [BOT]   USB-IF, "Universal Serial Bus Mass Storage Class Bulk-Only
 *           Transport", revision 1.0, 1999.
 *   [SPC/SBC] T10 SCSI Primary/Block Commands (INQUIRY, TEST UNIT READY,
 *           REQUEST SENSE, READ CAPACITY(10), READ(10), WRITE(10)).
 *
 * Polling only: the controller's events are consumed whenever a protocol
 * of the driver is called and from a 10 ms timer event. The controller is
 * halted at ExitBootServices, so the OS (or CSMWrap's SeaBIOS) takes over
 * a quiet controller.
 */
#include "usb.h"

struct hc hc;

/* ------------------------------------------------------------------ DMA memory */

static uint8_t *arena;
static uint64_t arena_left;

void *
dma(unsigned long size, unsigned long align)
{
	if (align < 16)
		align = 16;
	if (size + align > 0x8000) {
		void *p = dma_alloc_pages(EFI_SIZE_TO_PAGES(size));
		return ((uintptr_t)p & (align - 1)) ? 0 : p;
	}
	uint64_t pad = arena ? (align - ((uintptr_t)arena & (align - 1))) & (align - 1) : 0;
	if (!arena || pad + size > arena_left) {
		arena = dma_alloc_pages(16);
		if (!arena)
			return 0;
		arena_left = 16 * EFI_PAGE_SIZE;
		pad = 0;
	}
	void *p = arena + pad;
	arena += pad + size;
	arena_left -= pad + size;
	memset(p, 0, size);
	return p;
}

/* ------------------------------------------------------------------ helpers */

static uint32_t rd32(uintptr_t a) { return mmio_r32(a); }
static void wr32(uintptr_t a, uint32_t v) { mmio_w32(a, v); }

/* 64-bit registers as two 32-bit writes, low half first (xHCI 5.1) */
static void
wr64(uintptr_t a, uint64_t v)
{
	wr32(a, (uint32_t)v);
	wr32(a + 4, (uint32_t)(v >> 32));
}

static uint64_t
phys(const volatile void *p)
{
	return (uint64_t)(uintptr_t)p;
}

volatile uint32_t *
ctx(volatile uint32_t *base, unsigned idx)
{
	return (volatile uint32_t *)((volatile uint8_t *)base + idx * hc.ctxsz);
}

int
ring_init(struct ring *r, unsigned n)
{
	/* aligned to its size so it never crosses a 64 KiB boundary (6.5) */
	r->t = dma(n * sizeof(struct trb), n * sizeof(struct trb));
	if (!r->t)
		return -1;
	r->n = n;
	r->i = 0;
	r->cyc = 1;
	return 0;
}

/*
 * Queue a TRB. hold: write it with the wrong cycle bit, so the controller
 * doesn't start a TD before all of it is written; *cw = the real control word.
 */
vtrb *
ring_put(struct ring *r, uint64_t p, uint32_t s, uint32_t c, int hold, uint32_t *cw)
{
	vtrb *t = &r->t[r->i];
	t->p = p;
	t->s = s;
	c |= r->cyc;
	if (cw)
		*cw = c;
	t->c = hold ? c ^ 1 : c;
	if (++r->i == r->n - 1) {
		/* the Link TRB back to the start, toggling the cycle (4.11.5.1) */
		vtrb *l = &r->t[r->i];
		l->p = phys(r->t);
		l->s = 0;
		l->c = T_TYPE(TRB_LINK) | T_TC | (c & T_CH) | r->cyc;
		r->i = 0;
		r->cyc ^= 1;
	}
	return t;
}

uint64_t
ring_deq(const struct ring *r)
{
	return phys(&r->t[r->i]) | r->cyc;
}

void
doorbell(unsigned slot, unsigned target)
{
	wr32(hc.db + 4 * slot, target);
}

/* ------------------------------------------------------------------ events */

static struct udev *
dev_by_slot(unsigned slot)
{
	for (int i = 0; i < hc.ndev; i++)
		if (hc.dev[i].slot == (int)slot)
			return &hc.dev[i];
	return 0;
}

void
int_queue(struct udev *d)
{
	vtrb *t = ring_put(&d->ir, phys(d->ibuf), d->int_mps, T_TYPE(TRB_NORMAL) | T_IOC | T_ISP, 0, 0);
	d->i_trb = phys(t);
	d->i_busy = 1;
	doorbell((unsigned)d->slot, d->int_dci);
}

static void
handle_event(uint64_t p, uint32_t s, uint32_t c)
{
	unsigned type = (c >> 10) & 63, cc = s >> 24, dci = (c >> 16) & 31;
	struct udev *d;

	switch (type) {
	case EV_CMD:
		if (p == hc.cmd_ptr) {
			hc.cmd_cc = (int)cc;
			hc.cmd_slot = (int)(c >> 24);
			hc.cmd_done = 1;
		}
		break;
	case EV_TRANSFER:
		if (!(d = dev_by_slot(c >> 24)))
			break;
		if (dci == 1) {
			if (cc == CC_SUCCESS || cc == CC_SHORT) {
				if (p == d->c_data && cc == CC_SHORT)
					d->c_resid = s & 0xFFFFFF;
				if (p == d->c_status) {
					d->c_cc = CC_SUCCESS;
					d->c_done = 1;
				}
			} else if (p == d->c_data || p == d->c_status || cc != CC_STOPPED) {
				d->c_cc = (int)cc;
				d->c_done = 1;
			}
			break;
		}
		if (d->kind == K_MSC && (dci == d->bin_dci || dci == d->bout_dci)) {
			if (p == d->x_trb) {
				d->x_cc = (int)cc;
				d->x_resid = s & 0xFFFFFF;
				d->x_done = 1;
			}
			break;
		}
		if (dci != d->int_dci || p != d->i_trb)
			break;
		d->i_busy = 0;
		if (cc == CC_SUCCESS || cc == CC_SHORT) {
			unsigned resid = s & 0xFFFFFF, n = resid < d->int_mps ? d->int_mps - resid : 0;
			d->i_fail = 0;
			if (d->kind == K_KBD)
				key_report(d, d->ibuf, n);
			else if (d->kind == K_MOUSE)
				mouse_report(d, d->ibuf, n);
			int_queue(d);
		} else if (cc != CC_STOPPED && cc != CC_STOPPED_LEN)
			d->i_err = (int)cc;	/* usb_poll recovers the endpoint */
		break;
	case EV_PORT: {
		/* acknowledge the change bits (no hot-plug support) */
		unsigned port = (unsigned)(p >> 24);
		if (port >= 1 && port <= hc.maxports) {
			uint32_t ps = rd32(hc.op + PORTSC(port));
			wr32(hc.op + PORTSC(port), (ps & PS_KEEP) | (ps & PS_CHANGE & ~PS_PRC));
		}
		break;
	}
	default:
		break;
	}
}

/* consume the event ring (4.9.4) */
void
events(void)
{
	int any = 0;
	for (unsigned n = 0; n < EVT_TRBS; n++) {
		vtrb *e = &hc.er[hc.er_i];
		uint32_t c = e->c;
		if ((c & 1) != hc.er_cyc)
			break;
		handle_event(e->p, e->s, c);
		if (++hc.er_i == EVT_TRBS) {
			hc.er_i = 0;
			hc.er_cyc ^= 1;
		}
		any = 1;
	}
	if (any)
		wr64(hc.rt + ERDP, phys(&hc.er[hc.er_i]) | ERDP_EHB);
}

/* ------------------------------------------------------------------ waiting */

int
wait_flag(volatile int *flag, uint64_t us)
{
	uint64_t t0 = time_us();
	for (;;) {
		events();
		if (*flag)
			return 0;
		if (time_us() - t0 > us)
			break;
	}
	return events(), *flag ? 0 : -1;
}

static int
wait_reg(uintptr_t a, uint32_t mask, uint32_t want, uint64_t us)
{
	uint64_t t0 = time_us();
	for (;;) {
		if ((rd32(a) & mask) == want)
			return 0;
		if (time_us() - t0 > us)
			break;
	}
	return (rd32(a) & mask) == want ? 0 : -1;
}

/* ------------------------------------------------------------------ commands */

/* run one command (4.6); returns its completion code, or USB_ETIMEDOUT */
int
command(uint64_t p, uint32_t s, uint32_t c, int *slot)
{
	vtrb *t = ring_put(&hc.cmd, p, s, c, 0, 0);
	hc.cmd_ptr = phys(t);
	hc.cmd_done = 0;
	doorbell(0, 0);
	if (wait_flag(&hc.cmd_done, T_CMD_US)) {
		/* abort the ring (4.6.1.2); the controller can't be trusted now */
		wr32(hc.op + CRCR, CRCR_CA);
		wait_reg(hc.op + CRCR, CRCR_CRR, 0, T_CMD_US);
		DEBUG("USB: command %u timed out\n", (unsigned)(c >> 10) & 63);
		hc.ok = 0;
		return USB_ETIMEDOUT;
	}
	if (slot)
		*slot = hc.cmd_slot;
	return hc.cmd_cc;
}

/* get a halted or wedged endpoint going again at the ring's enqueue point */
int
ep_recover(struct udev *d, unsigned dci, const struct ring *r)
{
	unsigned st = ctx(d->out, dci)[0] & 7;
	int cc;
	if (st == EP_STATE_HALTED)
		cc = command(0, 0, T_TYPE(TRB_RESET_EP) | T_EP(dci) | T_SLOT(d->slot), 0);
	else if (st == EP_STATE_RUNNING)
		cc = command(0, 0, T_TYPE(TRB_STOP_EP) | T_EP(dci) | T_SLOT(d->slot), 0);
	else
		cc = CC_SUCCESS;
	if (cc != CC_SUCCESS)
		return cc < 0 ? cc : USB_EIO;
	cc = command(ring_deq(r), 0, T_TYPE(TRB_SET_DEQ) | T_EP(dci) | T_SLOT(d->slot), 0);
	return cc == CC_SUCCESS ? 0 : cc < 0 ? cc : USB_EIO;
}

/* ------------------------------------------------------------------ transfers */

/* a control transfer on the default endpoint (4.11.2.2, USB2 9.3) */
int
control(struct udev *d, int rt, int req, int val, int idx, void *data, int len)
{
	uint32_t fc;
	if (len < 0 || len > BOUNCE)
		return USB_EIO;
	if (!(rt & 0x80) && len)
		memcpy((void *)hc.bounce, data, (unsigned long)len);
	uint64_t setup = (uint64_t)(rt & 0xFF) | (uint64_t)(req & 0xFF) << 8 |
	    (uint64_t)(val & 0xFFFF) << 16 | (uint64_t)(idx & 0xFFFF) << 32 | (uint64_t)len << 48;
	uint32_t trt = len ? ((rt & 0x80) ? 3u : 2u) : 0u;	/* transfer type */
	d->c_done = 0;
	d->c_cc = 0;
	d->c_resid = 0;
	d->c_data = d->c_status = 0;
	vtrb *first = ring_put(&d->ep0, setup, 8, T_TYPE(TRB_SETUP) | T_IDT | trt << 16, 1, &fc);
	if (len)
		d->c_data = phys(ring_put(&d->ep0, phys(hc.bounce), (uint32_t)len,
		    T_TYPE(TRB_DATA) | T_ISP | ((rt & 0x80) ? T_DIR_IN : 0), 0, 0));
	/* the status stage goes the other way (IN when there is no data) */
	d->c_status = phys(ring_put(&d->ep0, 0, 0, T_TYPE(TRB_STATUS) | T_IOC |
	    ((len && (rt & 0x80)) ? 0 : T_DIR_IN), 0, 0));
	first->c = fc;
	doorbell((unsigned)d->slot, 1);
	if (wait_flag(&d->c_done, T_CTL_US)) {
		ep_recover(d, 1, &d->ep0);
		return USB_ETIMEDOUT;
	}
	if (d->c_cc != CC_SUCCESS) {
		int cc = d->c_cc;
		if (ep_recover(d, 1, &d->ep0) == USB_ETIMEDOUT)
			return USB_ETIMEDOUT;
		return cc == CC_STALL ? USB_ESTALL : USB_EIO;
	}
	int n = len - (int)d->c_resid;
	if (n < 0)
		n = 0;
	if ((rt & 0x80) && n)
		memcpy(data, (const void *)hc.bounce, (unsigned long)n);
	return n;
}

/*
 * A bulk transfer (4.11.2.1): one Normal TRB of len bytes at buf (DMA
 * memory, not crossing a 64 KiB boundary). Returns the bytes transferred,
 * or < 0; a STALL is recovered on the host side (Reset Endpoint, Set TR
 * Dequeue) and in the device (CLEAR_FEATURE ENDPOINT_HALT, USB2 9.4.1).
 */
int
bulk(struct udev *d, int in, volatile void *buf, uint32_t len)
{
	unsigned dci = in ? d->bin_dci : d->bout_dci;
	struct ring *r = in ? &d->bin : &d->bout;
	d->x_done = 0;
	d->x_cc = 0;
	d->x_resid = 0;
	vtrb *t = ring_put(r, phys(buf), len, T_TYPE(TRB_NORMAL) | T_IOC | (in ? T_ISP : 0), 0, 0);
	d->x_trb = phys(t);
	doorbell((unsigned)d->slot, dci);
	if (wait_flag(&d->x_done, T_BULK_US)) {
		ep_recover(d, dci, r);
		return USB_ETIMEDOUT;
	}
	if (d->x_cc == CC_SUCCESS || d->x_cc == CC_SHORT)
		return (int)(len - d->x_resid);
	int cc = d->x_cc;
	if (ep_recover(d, dci, r) == USB_ETIMEDOUT)
		return USB_ETIMEDOUT;
	if (cc == CC_STALL) {
		unsigned ep = in ? d->bin_addr : d->bout_addr;
		control(d, 0x02, 1, 0, (int)ep, 0, 0);		/* CLEAR_FEATURE(ENDPOINT_HALT) */
		return USB_ESTALL;
	}
	return USB_EIO;
}

/* ------------------------------------------------------------------ enumeration */

static void
desc_name(char *out, const volatile uint8_t *s, unsigned n)
{
	/* a string descriptor (UTF-16LE) as ASCII */
	unsigned j = 0;
	for (unsigned i = 2; i + 1 < n && i + 1 < s[0] && j < 47; i += 2)
		out[j++] = (s[i] >= 0x20 && s[i] < 0x7F && !s[i + 1]) ? (char)s[i] : '?';
	out[j] = 0;
}

/* the endpoint context Interval for an interrupt endpoint (6.2.3.6) */
static unsigned
ep_interval(int speed, unsigned binterval)
{
	if (speed == SPEED_FULL || speed == SPEED_LOW) {
		unsigned v = 3, f = binterval ? binterval * 8 : 8;
		while (v < 10 && (2u << v) <= f)
			v++;
		return v;
	}
	if (binterval < 1)
		binterval = 1;
	return binterval > 16 ? 15 : binterval - 1;
}

static void
ep_ctx(struct udev *d, unsigned dci, unsigned type, unsigned mps, unsigned interval, struct ring *r)
{
	volatile uint32_t *ep = ctx(d->in, dci + 1);
	ep[0] = interval << 16;
	ep[1] = 3u << 1 | type << 3 | mps << 16;	/* CErr 3 */
	ep[2] = (uint32_t)ring_deq(r);
	ep[3] = (uint32_t)(ring_deq(r) >> 32);
	ep[4] = type == 7 ? (mps | mps << 16) : 1024;	/* average TRB length, max ESIT payload */
}

static int
enumerate(unsigned port, int speed)
{
	struct udev *d;
	int slot, n, cc;
	uint8_t desc[18];

	if (hc.ndev == MAXDEV)
		return 0;
	d = &hc.dev[hc.ndev];
	memset(d, 0, sizeof *d);
	d->port = (int)port;
	d->speed = speed;

	/* a slot and its contexts (4.3.2, 4.3.3) */
	cc = command(0, 0, T_TYPE(TRB_ENABLE_SLOT), &slot);
	if (cc != CC_SUCCESS || slot < 1 || (unsigned)slot > hc.maxslots) {
		DEBUG("USB: port %u: no device slot (code %d)\n", port, cc);
		return cc < 0 ? cc : 0;
	}
	d->out = dma(32 * hc.ctxsz, 64);
	d->in = dma(33 * hc.ctxsz, 64);
	d->ibuf = dma(64, 64);
	if (!d->out || !d->in || !d->ibuf || ring_init(&d->ep0, EP0_TRBS) || ring_init(&d->ir, INT_TRBS))
		return USB_ENOMEM;
	hc.dcbaa[slot] = phys(d->out);
	d->slot = slot;
	hc.ndev++;

	/* Address Device (4.3.4): slot context + EP0 */
	d->mps0 = speed == SPEED_HIGH ? 64 : speed >= 4 ? 512 : 8;
	ctx(d->in, 0)[1] = 3;					/* A0, A1 */
	ctx(d->in, 1)[0] = 1u << 27 | (uint32_t)speed << 20;	/* 1 context entry */
	ctx(d->in, 1)[1] = port << 16;				/* root hub port */
	ctx(d->in, 2)[1] = 3u << 1 | 4u << 3 | d->mps0 << 16;	/* CErr 3, control */
	ctx(d->in, 2)[2] = (uint32_t)ring_deq(&d->ep0);
	ctx(d->in, 2)[3] = (uint32_t)(ring_deq(&d->ep0) >> 32);
	ctx(d->in, 2)[4] = 8;					/* average TRB length */
	cc = command(phys(d->in), 0, T_TYPE(TRB_ADDRESS) | T_SLOT(slot), 0);
	if (cc != CC_SUCCESS) {
		DEBUG("USB: port %u: Address Device failed (code %d)\n", port, cc);
		return cc < 0 ? cc : 0;
	}

	/* the device descriptor: bMaxPacketSize0 first (USB2 9.6.1) */
	n = control(d, 0x80, 6, 0x0100, 0, desc, 8);
	if (n < 8) {
		DEBUG("USB: port %u: no device descriptor (%d)\n", port, n);
		return n == USB_ETIMEDOUT ? n : 0;
	}
	if (desc[7] != d->mps0 && (desc[7] == 8 || desc[7] == 16 || desc[7] == 32 || desc[7] == 64)) {
		d->mps0 = desc[7];
		memset((void *)d->in, 0, 3 * hc.ctxsz);
		ctx(d->in, 0)[1] = 2;					/* A1 */
		ctx(d->in, 2)[1] = 3u << 1 | 4u << 3 | d->mps0 << 16;
		cc = command(phys(d->in), 0, T_TYPE(TRB_EVAL_CTX) | T_SLOT(slot), 0);
		if (cc != CC_SUCCESS)
			DEBUG("USB: port %u: Evaluate Context failed (code %d)\n", port, cc);
	}
	n = control(d, 0x80, 6, 0x0100, 0, desc, 18);
	if (n < 18) {
		DEBUG("USB: port %u: device descriptor: %d\n", port, n);
		return n == USB_ETIMEDOUT ? n : 0;
	}

	/* the configuration: its header, then all of it */
	uint8_t hdr[9];
	n = control(d, 0x80, 6, 0x0200, 0, hdr, 9);
	if (n < 9)
		return n == USB_ETIMEDOUT ? n : 0;
	unsigned total = hdr[2] | hdr[3] << 8, cfgval = hdr[5];
	if (total > 512)
		total = 512;
	static uint8_t cbuf[512];
	n = control(d, 0x80, 6, 0x0200, 0, cbuf, (int)total);
	if (n < 9)
		return 0;
	const uint8_t *cfg = cbuf;
	unsigned cls = 0, sub = 0, proto = 0, epaddr = 0, epmps = 0, epint = 0;
	unsigned bin = 0, bout = 0, bin_mps = 0, bout_mps = 0;
	int in_hid = 0, in_msc = 0;
	for (unsigned i = 0; i + 2 <= (unsigned)n && cfg[i] >= 2; i += cfg[i]) {
		if (cfg[i + 1] == 4 && i + 9 <= (unsigned)n) {		/* interface */
			in_hid = !epaddr && cfg[i + 5] == 3 && cfg[i + 6] == 1 &&
			    (cfg[i + 7] == 1 || cfg[i + 7] == 2);
			/* mass storage, SCSI transparent (06h) or UFI (04h), Bulk-Only (50h) */
			in_msc = !bin && cfg[i + 5] == 8 && (cfg[i + 6] == 6 || cfg[i + 6] == 4) &&
			    cfg[i + 7] == 0x50;
			if (!cls || in_hid || in_msc) {
				cls = cfg[i + 5];
				sub = cfg[i + 6];
				proto = cfg[i + 7];
				d->iface = cfg[i + 2];
			}
		} else if (cfg[i + 1] == 5 && i + 7 <= (unsigned)n) {	/* endpoint */
			unsigned a = cfg[i + 2], type = cfg[i + 3] & 3;
			unsigned mps = (cfg[i + 4] | cfg[i + 5] << 8) & 0x7FF;
			if (in_hid && !epaddr && (a & 0x80) && type == 3) {
				epaddr = a;
				epmps = mps;
				epint = cfg[i + 6];
			} else if (in_msc && type == 2) {
				if ((a & 0x80) && !bin)
					bin = a, bin_mps = mps;
				else if (!(a & 0x80) && !bout)
					bout = a, bout_mps = mps;
			}
		}
	}

	/* the product name */
	d->name[0] = 0;
	if (desc[15]) {
		static uint8_t sbuf[64];
		n = control(d, 0x80, 6, 0x0300 | desc[15], 0x0409, sbuf, 64);
		if (n >= 2)
			desc_name(d->name, sbuf, (unsigned)n);
	}
	if (!d->name[0])
		strncpy0(d->name, "USB device", sizeof d->name);

	int is_hid = cls == 3 && sub == 1 && epaddr && epmps;
	int is_msc = cls == 8 && bin && bout;
	if (!is_hid && !is_msc) {
		DEBUG("USB: port %u: %s (class %x/%x/%x, no driver)\n", port, d->name, cls, sub, proto);
		hc.info.other++;
		return 0;
	}

	/* Configure Endpoint (4.3.5) */
	memset((void *)d->in, 0, 33 * hc.ctxsz);
	unsigned last = 0;
	if (is_hid) {
		d->kind = proto == 1 ? K_KBD : K_MOUSE;
		d->int_dci = (epaddr & 15) * 2 + 1;
		d->int_mps = epmps > 64 ? 64 : epmps;
		ctx(d->in, 0)[1] = 1u | 1u << d->int_dci;
		ep_ctx(d, d->int_dci, 7, d->int_mps, ep_interval(speed, epint), &d->ir);	/* interrupt IN */
		last = d->int_dci;
	} else {
		d->kind = K_MSC;
		d->bin_addr = bin;
		d->bout_addr = bout;
		d->bin_dci = (bin & 15) * 2 + 1;
		d->bout_dci = (bout & 15) * 2;
		if (ring_init(&d->bin, BULK_TRBS) || ring_init(&d->bout, BULK_TRBS))
			return USB_ENOMEM;
		ctx(d->in, 0)[1] = 1u | 1u << d->bin_dci | 1u << d->bout_dci;
		ep_ctx(d, d->bin_dci, 6, bin_mps, 0, &d->bin);		/* bulk IN */
		ep_ctx(d, d->bout_dci, 2, bout_mps, 0, &d->bout);	/* bulk OUT */
		last = MAX(d->bin_dci, d->bout_dci);
	}
	ctx(d->in, 1)[0] = last << 27 | (uint32_t)speed << 20;
	ctx(d->in, 1)[1] = port << 16;
	cc = command(phys(d->in), 0, T_TYPE(TRB_CONFIG_EP) | T_SLOT(slot), 0);
	if (cc != CC_SUCCESS) {
		DEBUG("USB: port %u: Configure Endpoint failed (code %d)\n", port, cc);
		d->kind = K_OTHER;
		return cc < 0 ? cc : 0;
	}
	if ((n = control(d, 0x00, 9, (int)cfgval, 0, 0, 0)) < 0) {	/* SET_CONFIGURATION */
		DEBUG("USB: port %u: %s: SET_CONFIGURATION failed (%d)\n", port, d->name, n);
		d->kind = K_OTHER;
		return n == USB_ETIMEDOUT ? n : 0;
	}
	if (d->kind == K_MSC) {
		DEBUG("USB: port %u: %s (mass storage, %s)\n", port, d->name, sub == 6 ? "SCSI" : "UFI");
		return msc_start(d) == 0 ? 1 : 0;
	}
	/* the boot protocol and no idle reports (HID 7.2.6, 7.2.4) */
	if ((n = control(d, 0x21, 0x0B, 0, (int)d->iface, 0, 0)) < 0) {
		DEBUG("USB: port %u: %s: SET_PROTOCOL failed (%d)\n", port, d->name, n);
		d->kind = K_OTHER;
		return n == USB_ETIMEDOUT ? n : 0;
	}
	control(d, 0x21, 0x0A, 0, (int)d->iface, 0, 0);	/* SET_IDLE is optional for mice */
	int_queue(d);
	DEBUG("USB: port %u: %s (HID %s)\n", port, d->name, d->kind == K_KBD ? "keyboard" : "mouse");
	hid_start(d);
	return 1;
}

/* ------------------------------------------------------------------ init */

int
xhci_init(uint64_t bar0)
{
	uintptr_t cap = (uintptr_t)bar0;
	int found = 0;

	if (!bar0)
		return USB_ENODEV;
	uint32_t c0 = rd32(cap);
	if (c0 == 0xFFFFFFFFu || (c0 & 0xFF) < 0x20 || (c0 >> 16) < 0x0096)
		return USB_ENODEV;
	uint32_t hcs1 = rd32(cap + 0x04), hcs2 = rd32(cap + 0x08), hcc1 = rd32(cap + 0x10);
	hc.cap = cap;
	hc.op = cap + (c0 & 0xFF);
	hc.db = cap + (rd32(cap + 0x14) & ~3u);
	hc.rt = cap + (rd32(cap + 0x18) & ~0x1Fu);
	hc.maxslots = hcs1 & 0xFF;
	if (hc.maxslots > MAXSLOTS)
		hc.maxslots = MAXSLOTS;
	hc.maxports = hcs1 >> 24;
	if (hc.maxports > MAXPORTS)
		hc.maxports = MAXPORTS;
	hc.ctxsz = (hcc1 & 4) ? 64 : 32;

	/* which ports are USB 2 / USB 3: the Supported Protocol capabilities (7.2) */
	uintptr_t x = (hcc1 >> 16) ? cap + ((hcc1 >> 16) << 2) : 0;
	for (int i = 0; x && i < 64; i++) {
		uint32_t h = rd32(x);
		if ((h & 0xFF) == 2) {
			uint32_t r = rd32(x + 8);
			unsigned first = r & 0xFF, cnt = (r >> 8) & 0xFF;
			for (unsigned p = first; p < first + cnt && p <= hc.maxports; p++)
				if (p)
					hc.proto[p] = (uint8_t)(h >> 24);
		}
		if (!((h >> 8) & 0xFF))
			break;
		x += ((h >> 8) & 0xFF) << 2;
	}

	/* stop, then reset (4.2) */
	if (!(rd32(hc.op + USBSTS) & STS_HCH)) {
		wr32(hc.op + USBCMD, rd32(hc.op + USBCMD) & ~CMD_RS);
		if (wait_reg(hc.op + USBSTS, STS_HCH, STS_HCH, T_HALT_US)) {
			DEBUG("USB: controller does not halt\n");
			return USB_ETIMEDOUT;
		}
	}
	wr32(hc.op + USBCMD, CMD_HCRST);
	if (wait_reg(hc.op + USBCMD, CMD_HCRST, 0, T_RESET_US) ||
	    wait_reg(hc.op + USBSTS, STS_CNR, 0, T_RESET_US)) {
		DEBUG("USB: controller reset timed out\n");
		return USB_ETIMEDOUT;
	}
	if (!(rd32(hc.op + PAGESIZE) & 1))
		return USB_EIO;

	/* device context array, scratchpads, command ring, event ring */
	hc.dcbaa = dma(8 * (hc.maxslots + 1), 64);
	hc.bounce = dma(BOUNCE, 64);
	hc.er = dma(EVT_TRBS * sizeof(struct trb), EVT_TRBS * sizeof(struct trb));
	volatile uint64_t *erst = dma(16, 64);
	if (!hc.dcbaa || !hc.bounce || !hc.er || !erst || ring_init(&hc.cmd, CMD_TRBS))
		return USB_ENOMEM;
	unsigned nsp = ((hcs2 >> 21) & 0x1F) << 5 | (hcs2 >> 27);
	if (nsp) {
		volatile uint64_t *spa = dma(8 * nsp, 64);
		if (!spa)
			return USB_ENOMEM;
		for (unsigned i = 0; i < nsp; i++) {
			void *pg = dma(4096, 4096);
			if (!pg)
				return USB_ENOMEM;
			spa[i] = phys(pg);
		}
		hc.dcbaa[0] = phys(spa);
	}
	wr32(hc.op + CONFIG, hc.maxslots);
	wr64(hc.op + DCBAAP, phys(hc.dcbaa));
	wr64(hc.op + CRCR, ring_deq(&hc.cmd));
	erst[0] = phys(hc.er);
	erst[1] = EVT_TRBS;
	hc.er_cyc = 1;
	wr32(hc.rt + ERSTSZ, 1);
	wr64(hc.rt + ERDP, phys(hc.er));
	wr64(hc.rt + ERSTBA, phys(erst));
	wr32(hc.rt + IMAN, 1);			/* clear IP; polled, no interrupts */
	wr32(hc.op + USBCMD, CMD_RS);
	if (wait_reg(hc.op + USBSTS, STS_HCH, 0, T_HALT_US)) {
		DEBUG("USB: controller does not start (USBSTS %x)\n", rd32(hc.op + USBSTS));
		return USB_ETIMEDOUT;
	}
	hc.ok = 1;
	events();

	/* every connected port: reset it (USB 2), then enumerate the device */
	for (unsigned p = 1; p <= hc.maxports && hc.ok; p++) {
		uint32_t ps = rd32(hc.op + PORTSC(p));
		if (!(ps & PS_CCS))
			continue;
		if (hc.proto[p] != 3) {
			wr32(hc.op + PORTSC(p), (ps & PS_KEEP) | PS_PR);
			if (wait_reg(hc.op + PORTSC(p), PS_PRC | PS_PR, PS_PRC, T_PORT_US)) {
				DEBUG("USB: port %u: reset timed out\n", p);
				continue;
			}
			ps = rd32(hc.op + PORTSC(p));
			wr32(hc.op + PORTSC(p), (ps & PS_KEEP) | (ps & PS_CHANGE));
		}
		ps = rd32(hc.op + PORTSC(p));
		if (!(ps & PS_PED)) {
			DEBUG("USB: port %u: not enabled after reset\n", p);
			continue;
		}
		int r = enumerate(p, (int)PS_SPEED(ps));
		if (r == USB_ENOMEM || r == USB_ETIMEDOUT) {
			if (r == USB_ENOMEM)
				DEBUG("USB: out of DMA memory\n");
			if (!hc.ok)
				return r;
		}
		if (r > 0)
			found++;
	}
	return found;
}

void
xhci_halt(void)
{
	if (!hc.op)
		return;
	wr32(hc.op + USBCMD, rd32(hc.op + USBCMD) & ~CMD_RS);
	wait_reg(hc.op + USBSTS, STS_HCH, STS_HCH, T_HALT_US);
	hc.ok = 0;
}

/* ------------------------------------------------------------------ polling */

void
usb_poll(void)
{
	if (!hc.ok || hc.in_poll)
		return;
	hc.in_poll = 1;
	uint32_t sts = rd32(hc.op + USBSTS);
	if (sts & (STS_HCH | STS_HSE | STS_HCE)) {
		DEBUG("USB: controller stopped (USBSTS %x)\n", sts);
		hc.ok = 0;
		hc.in_poll = 0;
		return;
	}
	events();
	for (int i = 0; i < hc.ndev && hc.ok; i++) {
		struct udev *d = &hc.dev[i];
		if (!d->i_err || (d->kind != K_KBD && d->kind != K_MOUSE))
			continue;
		/* a failed interrupt transfer: recover the endpoint, retry a few times */
		d->i_err = 0;
		if (++d->i_fail > 3 || ep_recover(d, d->int_dci, &d->ir)) {
			DEBUG("USB: port %d: giving up on the device\n", d->port);
			d->kind = K_OTHER;
			continue;
		}
		int_queue(d);
	}
	hid_poll();
	hc.in_poll = 0;
}
