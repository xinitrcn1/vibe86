/*
 * vsim xHCI host controller with a HID boot keyboard (port 1) and a HID boot
 * mouse (port 2).
 *
 * The controller follows Intel, "eXtensible Host Controller Interface for
 * Universal Serial Bus (xHCI)", revision 1.2, May 2019 ("xHCI 1.2" below;
 * section numbers refer to it) for the subset described in docs/platform.md
 * "USB (xHCI)". The devices follow USB-IF, "Universal Serial Bus
 * Specification" 2.0, chapter 9 (standard requests), and USB-IF, "Device
 * Class Definition for Human Interface Devices (HID)", version 1.11, 2001
 * ("HID 1.11": class requests §7.2, boot protocol Appendix B, boot report
 * descriptors Appendix E.6/E.10), with key codes from the USB-IF "HID Usage
 * Tables" (Keyboard/Keypad page 0x07).
 *
 * Every access to guest memory (contexts, rings, buffers, events) is a DMA
 * request through pcie_dma(), so bus mastering and the IOMMU apply. Work per
 * core cycle is bounded: at most TRB_BUDGET TRBs are fetched per tick.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xhci.h"

/* ------------------------------------------------------------------ layout */

#define NPORTS		4
#define NSLOTS		8
#define CAPLENGTH	0x20
#define OPB		CAPLENGTH		/* operational registers */
#define RTB		0x1000			/* runtime registers (RTSOFF) */
#define DBB		0x2000			/* doorbell array (DBOFF) */
#define XECP		0x800			/* the Supported Protocol capability */
#define ERST_MAX	2			/* HCSPARAMS2.ERST Max: 2^2 segments */

#define TRB_BUDGET	8		/* TRBs fetched per tick */
#define EVQ		64		/* events waiting for room in the event ring */
#define PORT_RESET_TICKS 2000		/* sim-scaled port reset (USB 2.0: 10-20 ms) */
#define CNR_TICKS	64		/* controller not ready after HCRST */
#define MFINDEX_TICKS	150000		/* 125 us microframes at 1.2 GHz */
#define IDLE_TICKS	4800000ull	/* HID idle unit, 4 ms at 1.2 GHz */
#define CTLBUF		1024

/* USBCMD / USBSTS (xHCI 1.2 §5.4.1, §5.4.2) */
#define CMD_RS		0x1
#define CMD_HCRST	0x2
#define CMD_INTE	0x4
#define CMD_RW		0x00003F0Du	/* R/S INTE HSEE LHCRST.. EU3S..: stored */
#define STS_HCH		0x1
#define STS_HSE		0x4
#define STS_EINT	0x8
#define STS_PCD		0x10
#define STS_CNR		0x800
#define STS_HCE		0x1000
#define STS_RW1C	(STS_HSE | STS_EINT | STS_PCD | 0x400)

/* PORTSC (§5.4.8) */
#define PS_CCS		(1u << 0)
#define PS_PED		(1u << 1)
#define PS_PR		(1u << 4)
#define PS_PLS(x)	((uint32_t)(x) << 5)
#define PS_PLS_MASK	(15u << 5)
#define PS_PP		(1u << 9)
#define PS_SPEED(x)	((uint32_t)(x) << 10)
#define PS_PIC		(3u << 14)
#define PS_LWS		(1u << 16)
#define PS_CSC		(1u << 17)
#define PS_PEC		(1u << 18)
#define PS_WRC		(1u << 19)
#define PS_OCC		(1u << 20)
#define PS_PRC		(1u << 21)
#define PS_PLC		(1u << 22)
#define PS_CEC		(1u << 23)
#define PS_WAKE		(7u << 25)
#define PS_CHANGE	(PS_CSC | PS_PEC | PS_WRC | PS_OCC | PS_PRC | PS_PLC | PS_CEC)
#define PLS_U0		0
#define PLS_RXDETECT	5
#define PLS_POLLING	7
#define SPEED_FULL	1
#define SPEED_LOW	2

/* TRB types (§6.4.6) */
enum {
	TRB_NORMAL = 1, TRB_SETUP, TRB_DATA, TRB_STATUS, TRB_ISOCH, TRB_LINK,
	TRB_EVDATA, TRB_NOOP, TRB_ENABLE_SLOT, TRB_DISABLE_SLOT, TRB_ADDRESS_DEV,
	TRB_CONFIG_EP, TRB_EVAL_CTX, TRB_RESET_EP, TRB_STOP_EP, TRB_SET_DEQ,
	TRB_RESET_DEV, TRB_NOOP_CMD = 23,
	TRB_EV_TRANSFER = 32, TRB_EV_CMD, TRB_EV_PORT,
};
#define C_CYCLE		(1u << 0)
#define C_ENT_TC	(1u << 1)	/* Link: toggle cycle */
#define C_ISP		(1u << 2)
#define C_CH		(1u << 4)
#define C_IOC		(1u << 5)
#define C_IDT		(1u << 6)
#define C_BSR		(1u << 9)	/* Address Device; Configure Endpoint: DC */
#define C_DIR_IN	(1u << 16)	/* Data / Status stage */
#define C_ED		(1u << 2)	/* Transfer Event: Event Data */
#define TRB_TYPE(c)	(((c) >> 10) & 63)

/* completion codes (§6.4.5) */
enum {
	CC_SUCCESS = 1, CC_DATA_BUFFER = 2, CC_BABBLE = 3, CC_TRANSACTION = 4,
	CC_TRB = 5, CC_STALL = 6, CC_RESOURCE = 7, CC_NO_SLOTS = 9,
	CC_SLOT_NOT_ENABLED = 11, CC_EP_NOT_ENABLED = 12, CC_SHORT = 13,
	CC_PARAMETER = 17, CC_CONTEXT_STATE = 19, CC_CMD_RING_STOPPED = 24,
	CC_CMD_ABORTED = 25, CC_STOPPED = 26,
};

/* slot and endpoint states (§4.5.3, §4.8.3) */
enum { SS_ENABLED = 0, SS_DEFAULT, SS_ADDRESSED, SS_CONFIGURED };
enum { EP_DISABLED = 0, EP_RUNNING, EP_HALTED, EP_STOPPED, EP_ERROR };
enum { EPT_BULK_OUT = 2, EPT_CONTROL = 4, EPT_BULK_IN = 6, EPT_INT_IN = 7 };

struct trb {
	uint64_t p;
	uint32_t s, c;
};

/* ------------------------------------------------------------------ devices */

enum { DEV_NONE, DEV_KBD, DEV_MOUSE, DEV_MSC };
#define MSC_RESP	64	/* the longest non-data response (MODE SENSE with page 05h) */
#define KQ	32		/* keyboard reports waiting */
#define MAXHELD	16

struct usbdev {
	int kind, speed;
	int addr, config, protocol, idle, rwake;
	uint8_t leds;
	/* keyboard */
	uint8_t mods, held[MAXHELD];
	int nheld;
	uint8_t q[KQ][8];
	unsigned qh, qn;
	uint8_t last[8];		/* the last report sent */
	uint64_t idle_at;
	/* mouse */
	int dx, dy;
	uint8_t btn, bq[8];
	unsigned bqh, bqn;
	/* mass storage: a USB floppy drive (Bulk-Only Transport) */
	uint8_t *img;			/* the medium, in memory; NULL: no disk */
	uint64_t img_size;
	int ro;				/* write-protected */
	int bot;			/* 0 expect a CBW, 1 data in, 2 data out, 3 CSW */
	uint32_t tag, dlen, done;	/* the CBW's tag and length, bytes moved */
	int rw;				/* data: 0 resp[], 1 read the medium, 2 write it */
	uint8_t resp[MSC_RESP];
	uint32_t rlen;
	uint64_t moff;			/* READ/WRITE: byte offset into the medium */
	uint32_t xlen;			/* bytes the command transfers */
	uint8_t status;			/* CSW status: 0 passed, 1 failed */
	uint8_t skey, asc, ascq;	/* sense data for REQUEST SENSE */
};

static const char *const dev_product[] = { "", "USB Keyboard", "USB Mouse", "USB Floppy" };

/* HID 1.11 Appendix B.1 (= E.6): the boot keyboard report descriptor */
static const uint8_t kbd_report_desc[] = {
	0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
	0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
	0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01,
	0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
	0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
	0x81, 0x00, 0xC0,
};

/* HID 1.11 Appendix B.2 (= E.10): the boot mouse report descriptor */
static const uint8_t mouse_report_desc[] = {
	0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09,
	0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01,
	0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30,
	0x09, 0x31, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
	0xC0, 0xC0,
};

static void
dev_reset(struct usbdev *u)
{
	/* a USB reset: default state, report protocol (HID 1.11 §7.2.6), the
	 * recommended idle rates (§7.2.4: keyboards 500 ms, mice infinite);
	 * the keys held stay held */
	u->addr = u->config = u->rwake = 0;
	u->protocol = 1;
	u->idle = u->kind == DEV_KBD ? 125 : 0;
	u->leds = 0;
	u->qh = u->qn = 0;
	u->bqh = u->bqn = 0;
	u->dx = u->dy = 0;
	memset(u->last, 0, sizeof u->last);
	u->idle_at = 0;
	u->bot = 0;			/* mass storage: wait for a CBW; the medium stays */
	u->rw = 0;
}

/*
 * The USB floppy drive: USB Mass Storage Class Bulk-Only Transport 1.0
 * (USB-IF, 1999, https://www.usb.org/document-library/mass-storage-bulk-only-10),
 * the SCSI transparent command set (subclass 06h; SeaBIOS and UEFI's
 * UsbMassStorageDxe drive it) answering the commands of the UFI command
 * specification (USB-IF, "Mass Storage Class UFI Command Specification" 1.0,
 * 1998) a USB floppy drive answers: READ FORMAT CAPACITIES and the Flexible
 * Disk mode page (05h: 80 cylinders, 2 heads, 18 sectors for a 1.44 MB disk).
 * Real USB floppy drives mostly use UFI over Control/Bulk/Interrupt; SeaBIOS
 * only drives BOT, so this one reports BOT with the UFI command set.
 *
 * The data phase is always as long as the host's CBW says (BOT 6.7.2, case
 * Hi > Di: the device pads, and reports the residue in the CSW), so the host
 * never sees a short packet.
 */
static void
msc_sense(struct usbdev *u, uint8_t key, uint8_t asc, uint8_t ascq)
{
	u->status = 1;
	u->skey = key;
	u->asc = asc;
	u->ascq = ascq;
	u->rw = 0;
	u->rlen = 0;
	u->xlen = 0;
}

static uint32_t
be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void
put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* the Flexible Disk page (UFI 4.5.4 / SBC-1 5.3.3.3) for the medium's geometry */
static unsigned
msc_page05(const struct usbdev *u, uint8_t *p)
{
	unsigned spt = 18, heads = 2;
	uint64_t secs = u->img_size / 512;
	if (secs == 1440)		/* 720 KB */
		spt = 9;
	else if (secs == 2400)		/* 1.2 MB */
		spt = 15;
	else if (secs == 5760)		/* 2.88 MB */
		spt = 36;
	unsigned cyl = (unsigned)(secs / (spt * heads));
	memset(p, 0, 32);
	p[0] = 0x05;
	p[1] = 0x1E;
	p[2] = 0x01;			/* transfer rate 500 kbit/s */
	p[3] = 0xF4;
	p[4] = (uint8_t)heads;
	p[5] = (uint8_t)spt;
	p[6] = 0x02;			/* 512 bytes per sector */
	p[8] = (uint8_t)(cyl >> 8);
	p[9] = (uint8_t)cyl;
	p[28] = 0x01;			/* medium rotation rate 300 rpm */
	p[29] = 0x2C;
	return 32;
}

/* run the command block of a CBW */
static void
msc_command(struct usbdev *u, const uint8_t *cb)
{
	uint64_t blocks = u->img_size / 512;
	u->status = 0;
	u->rw = 0;
	u->rlen = 0;
	u->xlen = 0;
	memset(u->resp, 0, sizeof u->resp);
	uint8_t op = cb[0];
	if (!u->img && op != 0x03 && op != 0x12) {
		msc_sense(u, 0x02, 0x3A, 0x00);		/* NOT READY: medium not present */
		return;
	}
	switch (op) {
	case 0x00:					/* TEST UNIT READY */
	case 0x1B:					/* START STOP UNIT */
	case 0x1E:					/* PREVENT ALLOW MEDIUM REMOVAL */
	case 0x2F:					/* VERIFY(10) */
	case 0x35:					/* SYNCHRONIZE CACHE(10) */
		break;
	case 0x03:					/* REQUEST SENSE */
		u->resp[0] = 0x70;
		u->resp[2] = u->skey;
		u->resp[7] = 10;
		u->resp[12] = u->asc;
		u->resp[13] = u->ascq;
		u->rlen = 18;
		u->skey = u->asc = u->ascq = 0;
		break;
	case 0x12:					/* INQUIRY */
		if (cb[1] & 1) {			/* EVPD: no vital product data */
			msc_sense(u, 0x05, 0x24, 0x00);
			return;
		}
		u->resp[0] = 0x00;			/* direct-access block device */
		u->resp[1] = 0x80;			/* removable medium */
		u->resp[3] = 0x01;			/* response data format (UFI) */
		u->resp[4] = 31;
		memcpy(u->resp + 8, "vibe86  USB Floppy      1.00", 28);
		u->rlen = 36;
		break;
	case 0x1A:					/* MODE SENSE(6) */
	case 0x5A: {					/* MODE SENSE(10) */
		unsigned page = cb[2] & 0x3F, h = op == 0x1A ? 4 : 8, n = h;
		if (page == 0x05 || page == 0x3F)
			n += msc_page05(u, u->resp + h);
		else if (page != 0) {
			msc_sense(u, 0x05, 0x24, 0x00);	/* invalid field in CDB */
			return;
		}
		if (op == 0x1A) {
			u->resp[0] = (uint8_t)(n - 1);
			u->resp[2] = u->ro ? 0x80 : 0;
		} else {
			u->resp[1] = (uint8_t)(n - 2);
			u->resp[2] = 0x94;		/* medium type: 1.44 MB (UFI) */
			u->resp[3] = u->ro ? 0x80 : 0;
		}
		u->rlen = n;
		break;
	}
	case 0x23:					/* READ FORMAT CAPACITIES (UFI 4.10) */
		u->resp[3] = 8;
		put_be32(u->resp + 4, (uint32_t)blocks);
		u->resp[8] = 0x02;			/* formatted medium */
		u->resp[10] = 0x02;			/* 512-byte blocks */
		u->rlen = 12;
		break;
	case 0x25:					/* READ CAPACITY(10) */
		put_be32(u->resp, (uint32_t)(blocks - 1));
		put_be32(u->resp + 4, 512);
		u->rlen = 8;
		break;
	case 0x28: case 0xA8:				/* READ(10), READ(12) */
	case 0x2A: case 0xAA: {				/* WRITE(10), WRITE(12) */
		uint64_t lba = be32(cb + 2);
		uint32_t n = (op & 0x80) ? be32(cb + 6) : (uint32_t)(cb[7] << 8 | cb[8]);
		if (lba + n > blocks) {
			msc_sense(u, 0x05, 0x21, 0x00);	/* LBA out of range */
			return;
		}
		if ((op & 0x0F) == 0x0A && u->ro) {
			msc_sense(u, 0x07, 0x27, 0x00);	/* DATA PROTECT: write protected */
			return;
		}
		u->rw = (op & 0x0F) == 0x0A ? 2 : 1;
		u->moff = lba * 512;
		u->xlen = n * 512;
		break;
	}
	default:
		msc_sense(u, 0x05, 0x20, 0x00);		/* invalid command operation code */
		break;
	}
}

/* a bulk OUT transfer to the device: a CBW, or WRITE data */
static void
msc_out(struct usbdev *u, const uint8_t *p, unsigned n)
{
	if (u->bot == 2) {
		for (unsigned i = 0; i < n && u->done < u->dlen; i++, u->done++)
			if (u->rw == 2 && u->done < u->xlen)
				u->img[u->moff + u->done] = p[i];
		if (u->done >= u->dlen)
			u->bot = 3;
		return;
	}
	if (u->bot != 0 || n != 31 || be32(p) != 0x55534243u)	/* "USBC" little-endian */
		return;					/* not a valid CBW: ignored */
	u->tag = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
	u->dlen = (uint32_t)p[8] | (uint32_t)p[9] << 8 | (uint32_t)p[10] << 16 | (uint32_t)p[11] << 24;
	u->done = 0;
	msc_command(u, p + 15);
	int din = (p[12] & 0x80) != 0;
	if ((u->rw == 2 && din) || (u->rw == 1 && !din))
		msc_sense(u, 0x05, 0x24, 0x00);		/* direction doesn't match the command */
	u->bot = u->dlen == 0 ? 3 : din ? 1 : 2;
}

/* bytes the device has for the next bulk IN transfer */
static unsigned
msc_avail(const struct usbdev *u)
{
	return u->bot == 1 ? u->dlen - u->done : u->bot == 3 ? 13 : 0;
}

/* the next bulk IN bytes (data, or the CSW); returns how many */
static unsigned
msc_in(struct usbdev *u, uint8_t *p, unsigned max)
{
	if (u->bot == 3) {
		uint32_t have = u->rw ? u->xlen : u->rlen;
		uint32_t resid = u->dlen > have ? u->dlen - have : 0;
		uint8_t c[13] = { 'U', 'S', 'B', 'S' };
		for (int i = 0; i < 4; i++) {
			c[4 + i] = (uint8_t)(u->tag >> (8 * i));
			c[8 + i] = (uint8_t)(resid >> (8 * i));
		}
		c[12] = u->status;
		unsigned n = max < 13 ? max : 13;
		memcpy(p, c, n);
		u->bot = 0;
		return n;
	}
	if (u->bot != 1)
		return 0;
	unsigned n = u->dlen - u->done < max ? u->dlen - u->done : max;
	for (unsigned i = 0; i < n; i++) {
		uint32_t at = u->done + i;
		p[i] = u->rw == 1 ? (at < u->xlen ? u->img[u->moff + at] : 0)
		     : (at < u->rlen ? u->resp[at] : 0);
	}
	u->done += n;
	if (u->done >= u->dlen)
		u->bot = 3;
	return n;
}

static void
kbd_report(const struct usbdev *u, uint8_t r[8])
{
	memset(r, 0, 8);
	r[0] = u->mods;
	if (u->nheld > 6)
		memset(r + 2, 0x01, 6);		/* ErrorRollOver */
	else
		for (int i = 0; i < u->nheld; i++)
			r[2 + i] = u->held[i];
}

static void
kbd_changed(struct usbdev *u)
{
	if (!u->config)
		return;			/* not configured: nothing is sent */
	uint8_t r[8];
	kbd_report(u, r);
	if (u->qn == KQ)
		u->qn--;		/* full: the newest state replaces the last */
	memcpy(u->q[(u->qh + u->qn) % KQ], r, 8);
	u->qn++;
}

static int
mouse_pending(const struct usbdev *u)
{
	return u->config && (u->dx || u->dy || u->bqn);
}

static int
dev_has_data(const struct usbdev *u, uint64_t now)
{
	if (u->kind == DEV_KBD)
		return u->config && (u->qn || (u->idle && now >= u->idle_at));
	if (u->kind == DEV_MOUSE)
		return mouse_pending(u);
	if (u->kind == DEV_MSC)
		return u->config && msc_avail(u) != 0;
	return 0;
}

static int
clamp127(int v)
{
	return v > 127 ? 127 : v < -127 ? -127 : v;
}

/* the next interrupt IN packet; returns its length */
static unsigned
dev_fetch(struct usbdev *u, uint8_t *p, uint64_t now)
{
	if (u->kind == DEV_KBD) {
		if (u->qn) {
			memcpy(p, u->q[u->qh], 8);
			u->qh = (u->qh + 1) % KQ;
			u->qn--;
		} else
			memcpy(p, u->last, 8);	/* idle period expired: resend */
		memcpy(u->last, p, 8);
		u->idle_at = now + (uint64_t)u->idle * IDLE_TICKS;
		return 8;
	}
	int dx = clamp127(u->dx), dy = clamp127(u->dy);
	u->dx -= dx;
	u->dy -= dy;
	uint8_t b = u->btn;
	if (u->bqn) {
		b = u->bq[u->bqh];
		u->bqh = (u->bqh + 1) % 8;
		u->bqn--;
	}
	p[0] = b;
	p[1] = (uint8_t)(int8_t)dx;
	p[2] = (uint8_t)(int8_t)dy;
	memcpy(u->last, p, 3);
	return 3;
}

/* descriptors (USB 2.0 §9.6, HID 1.11 §6.2.1) */
static unsigned
desc_device(const struct usbdev *u, uint8_t *b)
{
	static const uint8_t d[18] = {
		18, 1, 0x10, 0x01,		/* USB 1.1 */
		0, 0, 0, 8,			/* class per interface; bMaxPacketSize0 8 */
		0x63, 0x76, 0, 0,		/* idVendor 0x7663 (placeholder), idProduct */
		0x00, 0x01, 1, 2, 0, 1,		/* bcdDevice 1.00, strings 1 2, no serial, 1 config */
	};
	memcpy(b, d, 18);
	b[10] = (uint8_t)u->kind;
	return 18;
}

static unsigned
report_desc(const struct usbdev *u, const uint8_t **p)
{
	if (u->kind == DEV_KBD) {
		*p = kbd_report_desc;
		return sizeof kbd_report_desc;
	}
	*p = mouse_report_desc;
	return sizeof mouse_report_desc;
}

static unsigned
desc_hid(const struct usbdev *u, uint8_t *b)
{
	const uint8_t *r;
	unsigned n = report_desc(u, &r);
	const uint8_t d[9] = { 9, 0x21, 0x11, 0x01, 0, 1, 0x22, (uint8_t)n, (uint8_t)(n >> 8) };
	memcpy(b, d, 9);
	return 9;
}

static unsigned
desc_config(const struct usbdev *u, uint8_t *b)
{
	if (u->kind == DEV_MSC) {
		static const uint8_t m[32] = {
			9, 2, 32, 0, 1, 1, 0, 0x80, 250,	/* bus powered, 500 mA */
			9, 4, 0, 0, 2, 8, 6, 0x50, 0,		/* mass storage, SCSI, Bulk-Only */
			7, 5, 0x81, 2, 64, 0, 0,		/* EP1 IN, bulk, 64 bytes */
			7, 5, 0x02, 2, 64, 0, 0,		/* EP2 OUT, bulk, 64 bytes */
		};
		memcpy(b, m, 32);
		return 32;
	}
	static const uint8_t c[9] = { 9, 2, 34, 0, 1, 1, 0, 0xA0, 50 };	/* bus powered, remote wakeup, 100 mA */
	uint8_t i[9] = { 9, 4, 0, 0, 1, 3, 1, (uint8_t)u->kind, 0 };	/* HID, boot, kbd 1 / mouse 2 */
	static const uint8_t e[7] = { 7, 5, 0x81, 3, 8, 0, 10 };	/* EP1 IN, interrupt, 8 bytes, 10 ms */
	memcpy(b, c, 9);
	memcpy(b + 9, i, 9);
	desc_hid(u, b + 18);
	memcpy(b + 27, e, 7);
	return 34;
}

static unsigned
desc_string(const struct usbdev *u, unsigned idx, uint8_t *b)
{
	const char *s;
	if (idx == 0) {
		const uint8_t l[4] = { 4, 3, 0x09, 0x04 };	/* English (US) */
		memcpy(b, l, 4);
		return 4;
	}
	if (idx == 1)
		s = "vibe86";
	else if (idx == 2)
		s = dev_product[u->kind];
	else
		return 0;
	unsigned n = 0;
	b[1] = 3;
	for (; s[n]; n++) {
		b[2 + 2 * n] = (uint8_t)s[n];
		b[3 + 2 * n] = 0;
	}
	b[0] = (uint8_t)(2 + 2 * n);
	return b[0];
}

/*
 * A control request (USB 2.0 §9.4, HID 1.11 §7.2). For IN requests the data
 * goes to buf (at most wLength bytes, *len = bytes); OUT data is in buf
 * (*len bytes). Returns 0, or -1 = STALL.
 */
static int
dev_request(struct usbdev *u, const uint8_t s[8], uint8_t *buf, unsigned *len)
{
	unsigned rt = s[0], req = s[1], val = s[2] | s[3] << 8, idx = s[4] | s[5] << 8,
	    wlen = s[6] | s[7] << 8, n = 0;
	uint8_t d[64];
	const uint8_t *src = d;

	switch (rt << 8 | req) {
	case 0x8000:					/* GET_STATUS device */
		d[0] = 0;
		d[1] = 0;
		if (u->rwake)
			d[0] |= 2;
		n = 2;
		break;
	case 0x8100:					/* GET_STATUS interface */
		if (idx != 0)
			return -1;
		d[0] = d[1] = 0;
		n = 2;
		break;
	case 0x8200:					/* GET_STATUS endpoint */
		if ((idx & 0xFF) != 0 && (idx & 0xFF) != 0x81 &&
		    !(u->kind == DEV_MSC && (idx & 0xFF) == 0x02))
			return -1;
		d[0] = d[1] = 0;
		n = 2;
		break;
	case 0x0001:					/* CLEAR_FEATURE device */
	case 0x0003:					/* SET_FEATURE device */
		if (val != 1)				/* DEVICE_REMOTE_WAKEUP */
			return -1;
		u->rwake = req == 3;
		break;
	case 0x0201:					/* CLEAR_FEATURE endpoint */
		if (val != 0 || ((idx & 0xFF) != 0 && (idx & 0xFF) != 0x81 &&
		    !(u->kind == DEV_MSC && (idx & 0xFF) == 0x02)))
			return -1;			/* ENDPOINT_HALT: never halted */
		break;
	case 0xA1FE:					/* BOT: Get Max LUN */
		if (u->kind != DEV_MSC || idx != 0)
			return -1;
		d[0] = 0;
		n = 1;
		break;
	case 0x21FF:					/* BOT: Bulk-Only Mass Storage Reset */
		if (u->kind != DEV_MSC || idx != 0)
			return -1;
		u->bot = 0;
		break;
	case 0x8006:					/* GET_DESCRIPTOR */
		switch (val >> 8) {
		case 1: n = desc_device(u, d); break;
		case 2:
			if ((val & 0xFF) != 0)
				return -1;
			n = desc_config(u, d);
			break;
		case 3:
			if (!(n = desc_string(u, val & 0xFF, d)))
				return -1;
			break;
		default: return -1;
		}
		break;
	case 0x8106:					/* GET_DESCRIPTOR interface: HID class */
		if (idx != 0 || u->kind == DEV_MSC)
			return -1;
		if (val >> 8 == 0x21)
			n = desc_hid(u, d);
		else if (val >> 8 == 0x22)
			n = report_desc(u, &src);
		else
			return -1;
		break;
	case 0x8008:					/* GET_CONFIGURATION */
		d[0] = (uint8_t)u->config;
		n = 1;
		break;
	case 0x0009:					/* SET_CONFIGURATION */
		if (val > 1 || !u->addr)
			return -1;
		u->config = (int)val;
		u->qh = u->qn = 0;
		u->bqh = u->bqn = 0;
		u->dx = u->dy = 0;
		break;
	case 0x810A:					/* GET_INTERFACE */
		if (idx != 0 || !u->config)
			return -1;
		d[0] = 0;
		n = 1;
		break;
	case 0x010B:					/* SET_INTERFACE */
		if (idx != 0 || val != 0 || !u->config)
			return -1;
		break;
	case 0xA101:					/* GET_REPORT (input) */
		if (idx != 0 || val >> 8 != 1 || u->kind == DEV_MSC)
			return -1;
		if (u->kind == DEV_KBD) {
			kbd_report(u, d);
			n = 8;
		} else {
			d[0] = u->btn;
			d[1] = d[2] = 0;
			n = 3;
		}
		break;
	case 0xA102:					/* GET_IDLE */
		if (idx != 0)
			return -1;
		d[0] = (uint8_t)u->idle;
		n = 1;
		break;
	case 0xA103:					/* GET_PROTOCOL */
		if (idx != 0)
			return -1;
		d[0] = (uint8_t)u->protocol;
		n = 1;
		break;
	case 0x2109:					/* SET_REPORT: keyboard LEDs */
		if (idx != 0 || u->kind != DEV_KBD || val != 0x0200 || *len < 1)
			return -1;
		u->leds = buf[0] & 0x1F;
		return 0;
	case 0x210A:					/* SET_IDLE */
		if (idx != 0)
			return -1;
		u->idle = (int)(val >> 8);
		u->idle_at = 0;
		break;
	case 0x210B:					/* SET_PROTOCOL */
		if (idx != 0 || val > 1)
			return -1;
		u->protocol = (int)val;
		break;
	default:
		return -1;
	}
	if (rt & 0x80) {
		if (n > wlen)
			n = wlen;
		memcpy(buf, src, n);
		*len = n;
	}
	return 0;
}

/* ------------------------------------------------------------------ controller */

struct ep {
	int state, type;
	unsigned mps;
	uint64_t deq;
	int ccs;
	uint32_t off;		/* bytes already done in the TRB at deq */
	uint32_t edtla;		/* Event Data transfer length accumulator */
	int skip;		/* short packet: skip to the end of the TD */
	int pending;		/* an interrupt TRB is waiting for data */
	/* control endpoint */
	int phase;		/* 0 setup, 1 data/status */
	int stall, in;
	uint8_t setup[8];
	uint8_t buf[CTLBUF];
	unsigned len, pos;
};

struct slot {
	int en, state, port;
	uint32_t act;		/* endpoints to service (doorbell rung) */
	struct ep ep[32];
};

struct xhci {
	struct pcidev d;	/* first */
	uint32_t usbcmd, usbsts, dnctrl, config;
	uint64_t crp, dcbaap;
	int rcs, crr;
	int cmd_wait;		/* the command ring was empty: wait for DB0 */
	uint32_t portsc[NPORTS];
	unsigned preset[NPORTS];
	uint32_t iman, imod, erstsz;
	uint64_t erstba, erdp;
	/* event ring producer */
	int er_ok;
	unsigned er_seg;
	uint64_t er_enq;
	uint32_t er_left;
	int pcs;
	struct trb evq[EVQ];
	unsigned evh, evn;
	uint64_t ev_lost;
	unsigned cnr;
	uint64_t ticks, mf0;
	struct slot slot[NSLOTS + 1];
	struct usbdev dev[NPORTS];
};

static void halt_hse(struct xhci *x);

static int
dma_rd(struct xhci *x, uint64_t a, void *b, unsigned n)
{
	if (pcie_dma(&x->d, 0, a, b, n, 0, 0, 0, 0) != IO_OK) {
		halt_hse(x);
		return -1;
	}
	return 0;
}

static int
dma_wr(struct xhci *x, uint64_t a, const void *b, unsigned n)
{
	if (pcie_dma(&x->d, 1, a, (uint8_t *)(uintptr_t)b, n, 0, 0, 0, 0) != IO_OK) {
		halt_hse(x);
		return -1;
	}
	return 0;
}

static int
running(const struct xhci *x)
{
	return (x->usbcmd & CMD_RS) && !(x->usbsts & STS_HCH);
}

/* Host System Error (§4.10.2.6): a DMA failed; the controller halts */
static void
halt_hse(struct xhci *x)
{
	x->usbsts |= STS_HSE | STS_HCH;
	x->usbcmd &= ~CMD_RS;
	x->crr = 0;
}

/* ---------------------------------------------------------------- events */

static void
ev_post(struct xhci *x, uint64_t p, uint32_t s, uint32_t c)
{
	if (x->evn == EVQ) {
		x->ev_lost++;
		return;
	}
	struct trb *t = &x->evq[(x->evh + x->evn) % EVQ];
	t->p = p;
	t->s = s;
	t->c = c & ~C_CYCLE;
	x->evn++;
}

/* load the current Event Ring Segment Table entry (§6.5) */
static int
er_load(struct xhci *x)
{
	uint8_t e[16];
	unsigned sz = x->erstsz & 0xFFFF;
	if (!sz || sz > (1u << ERST_MAX) || x->er_seg >= sz) {
		x->usbsts |= STS_HCE;
		return -1;
	}
	if (dma_rd(x, (x->erstba & ~0x3Full) + 16ull * x->er_seg, e, 16))
		return -1;
	uint64_t base;
	uint32_t n;
	memcpy(&base, e, 8);
	memcpy(&n, e + 8, 4);
	n &= 0xFFFF;
	if (n < 16 || n > 4096) {
		x->usbsts |= STS_HCE;	/* Host Controller Error (§4.24.1) */
		return -1;
	}
	x->er_enq = base & ~0x3Full;
	x->er_left = n;
	x->er_ok = 1;
	return 0;
}

static void
irq_check(struct xhci *x)
{
	/* §4.17: assert the interrupter while the Event Handler isn't busy
	 * and events are waiting behind the dequeue pointer */
	if (!x->er_ok || (x->erdp & 8) || (x->erdp & ~0xFull) == x->er_enq)
		return;
	x->erdp |= 8;			/* EHB */
	x->iman |= 1;			/* IP */
	x->usbsts |= STS_EINT;
	if ((x->iman & 2) && (x->usbcmd & CMD_INTE))
		pcie_msi(&x->d);
}

/* write queued events into the guest's event ring while there's room */
static void
ev_flush(struct xhci *x)
{
	while (x->evn && running(x)) {
		if (!x->er_ok && er_load(x))
			return;
		/* the position after this one, to check for a full ring (§4.9.4) */
		uint64_t next = x->er_enq + 16;
		if (x->er_left == 1) {
			unsigned sz = x->erstsz & 0xFFFF, seg = (x->er_seg + 1) % (sz ? sz : 1);
			uint8_t e[16];
			if (dma_rd(x, (x->erstba & ~0x3Full) + 16ull * seg, e, 16))
				return;
			memcpy(&next, e, 8);
			next &= ~0x3Full;
		}
		if (next == (x->erdp & ~0xFull))
			return;		/* full: keep the events until software catches up */
		struct trb t = x->evq[x->evh];
		t.c |= (uint32_t)x->pcs;
		if (dma_wr(x, x->er_enq, &t, 16))
			return;
		x->evh = (x->evh + 1) % EVQ;
		x->evn--;
		if (--x->er_left == 0) {
			x->er_seg++;
			if (x->er_seg >= (x->erstsz & 0xFFFF)) {
				x->er_seg = 0;
				x->pcs ^= 1;
			}
			if (er_load(x))
				return;
		} else
			x->er_enq += 16;
	}
	irq_check(x);
}

static void
ev_cmd(struct xhci *x, uint64_t trb, int cc, unsigned slot)
{
	ev_post(x, trb, (uint32_t)cc << 24, TRB_EV_CMD << 10 | slot << 24);
}

static void
ev_xfer(struct xhci *x, unsigned slot, unsigned dci, uint64_t p, uint32_t resid, int cc,
    int ed)
{
	ev_post(x, p, (resid & 0xFFFFFF) | (uint32_t)cc << 24,
	    TRB_EV_TRANSFER << 10 | (ed ? C_ED : 0) | dci << 16 | slot << 24);
}

/* ---------------------------------------------------------------- ports */

static void
port_change(struct xhci *x, int i, uint32_t bits)
{
	uint32_t old = x->portsc[i];
	x->portsc[i] |= bits;
	/* a Port Status Change Event when a change bit goes 0 -> 1 (§4.19.2) */
	if ((bits & ~old & PS_CHANGE) && running(x)) {
		x->usbsts |= STS_PCD;
		ev_post(x, (uint64_t)(i + 1) << 24, (uint32_t)CC_SUCCESS << 24, TRB_EV_PORT << 10);
	}
}

static void
port_init(struct xhci *x, int i)
{
	/* PPC = 0: ports are always powered. A USB 2 port with a device sits in
	 * the Disabled state (PLS = Polling) until software resets it */
	if (x->dev[i].kind != DEV_NONE)
		x->portsc[i] = PS_PP | PS_CCS | PS_CSC | PS_PLS(PLS_POLLING) |
		    PS_SPEED(x->dev[i].speed);
	else
		x->portsc[i] = PS_PP | PS_PLS(PLS_RXDETECT);
	x->preset[i] = 0;
}

static void
port_write(struct xhci *x, int i, uint32_t v)
{
	uint32_t p = x->portsc[i];
	p &= ~(v & PS_CHANGE);				/* RW1C */
	p = (p & ~(PS_WAKE | PS_PIC)) | (v & (PS_WAKE | PS_PIC));
	if (v & PS_PED)					/* writing 1 disables */
		p &= ~PS_PED;
	if ((v & PS_LWS) && (p & PS_PED))
		p = (p & ~PS_PLS_MASK) | (v & PS_PLS_MASK);
	if ((v & PS_PR) && (p & PS_CCS) && !(p & PS_PR)) {
		p |= PS_PR;
		p &= ~PS_PED;
		x->preset[i] = PORT_RESET_TICKS;
	}
	x->portsc[i] = p;
}

static void
port_tick(struct xhci *x, int i)
{
	if (!x->preset[i] || --x->preset[i])
		return;
	/* reset done (§4.19.1.1): enabled, U0, the device in the default state */
	uint32_t p = x->portsc[i] & ~(PS_PR | PS_PLS_MASK);
	x->portsc[i] = p | PS_PED | PS_PLS(PLS_U0);
	dev_reset(&x->dev[i]);
	port_change(x, i, PS_PRC);
}

/* ---------------------------------------------------------------- contexts */

static uint64_t
out_ctx(struct xhci *x, unsigned slot)
{
	uint64_t a = 0;
	if (dma_rd(x, (x->dcbaap & ~0x3Full) + 8ull * slot, &a, 8))
		return 0;
	return a & ~0x3Full;
}

static void
ep_ctx_write(struct xhci *x, unsigned slot, unsigned dci)
{
	struct ep *e = &x->slot[slot].ep[dci];
	uint64_t oc = out_ctx(x, slot);
	uint32_t c[4];
	if (!oc || dma_rd(x, oc + 32ull * dci, c, 16))
		return;
	c[0] = (c[0] & ~7u) | (uint32_t)e->state;
	uint64_t dq = (e->deq & ~0xFull) | (uint64_t)e->ccs;
	c[2] = (uint32_t)dq;
	c[3] = (uint32_t)(dq >> 32);
	dma_wr(x, oc + 32ull * dci, c, 16);
}

static void
slot_ctx_state(struct xhci *x, unsigned slot)
{
	struct slot *s = &x->slot[slot];
	uint64_t oc = out_ctx(x, slot);
	uint32_t c[4];
	if (!oc || dma_rd(x, oc, c, 16))
		return;
	unsigned last = 1;
	for (unsigned i = 2; i < 32; i++)
		if (s->ep[i].state != EP_DISABLED)
			last = i;
	c[0] = (c[0] & 0x07FFFFFFu) | last << 27;
	c[3] = (uint32_t)s->state << 27 | (s->state >= SS_ADDRESSED ? (uint32_t)x->dev[s->port - 1].addr : 0);
	dma_wr(x, oc, c, 16);
}

static void
ep_setup(struct ep *e, const uint32_t c[8])
{
	memset(e, 0, sizeof *e);
	e->state = EP_RUNNING;
	e->type = (c[1] >> 3) & 7;
	e->mps = c[1] >> 16;
	e->deq = ((uint64_t)c[3] << 32 | c[2]) & ~0xFull;
	e->ccs = c[2] & 1;
}

static struct usbdev *
slot_dev(struct xhci *x, unsigned slot)
{
	struct slot *s = &x->slot[slot];
	if (s->port < 1 || s->port > NPORTS || x->dev[s->port - 1].kind == DEV_NONE)
		return NULL;
	return &x->dev[s->port - 1];
}

/* ---------------------------------------------------------------- commands */

static int
cmd_address(struct xhci *x, unsigned sid, const struct trb *t)
{
	struct slot *s = &x->slot[sid];
	int bsr = (t->c & C_BSR) != 0;
	uint32_t in[24];
	if (s->state >= SS_ADDRESSED || (bsr && s->state == SS_DEFAULT))
		return CC_CONTEXT_STATE;
	if (dma_rd(x, t->p & ~0xFull, in, sizeof in))
		return CC_TRB;
	if ((in[1] & 3) != 3)			/* A0 and A1 */
		return CC_PARAMETER;
	uint32_t *sc = in + 8, *ec = in + 16;
	unsigned port = (sc[1] >> 16) & 0xFF;
	uint64_t oc = out_ctx(x, sid);
	if (!oc || ((ec[1] >> 3) & 7) != EPT_CONTROL)
		return CC_PARAMETER;
	if (port < 1 || port > NPORTS || x->dev[port - 1].kind == DEV_NONE ||
	    !(x->portsc[port - 1] & PS_PED))
		return CC_TRANSACTION;		/* no device answers */
	struct usbdev *u = &x->dev[port - 1];
	s->port = (int)port;
	s->state = bsr ? SS_DEFAULT : SS_ADDRESSED;
	if (!bsr)
		u->addr = (int)sid;		/* SET_ADDRESS to the device */
	ep_setup(&s->ep[1], ec);
	sc[0] = (sc[0] & 0x07FFFFFFu) | 1u << 27;
	sc[3] = (uint32_t)s->state << 27 | (bsr ? 0 : (uint32_t)u->addr);
	ec[0] = (ec[0] & ~7u) | EP_RUNNING;
	if (dma_wr(x, oc, sc, 32) || dma_wr(x, oc + 32, ec, 32))
		return CC_TRB;
	return CC_SUCCESS;
}

static void
ep_disable(struct xhci *x, unsigned sid, unsigned dci)
{
	struct ep *e = &x->slot[sid].ep[dci];
	memset(e, 0, sizeof *e);
	x->slot[sid].act &= ~(1u << dci);
	uint64_t oc = out_ctx(x, sid);
	uint32_t c0;
	if (oc && !dma_rd(x, oc + 32ull * dci, &c0, 4)) {
		c0 &= ~7u;
		dma_wr(x, oc + 32ull * dci, &c0, 4);
	}
}

static int
cmd_configure(struct xhci *x, unsigned sid, const struct trb *t)
{
	struct slot *s = &x->slot[sid];
	if (s->state != SS_ADDRESSED && s->state != SS_CONFIGURED)
		return CC_CONTEXT_STATE;
	if (t->c & C_BSR) {			/* DC: deconfigure */
		for (unsigned i = 2; i < 32; i++)
			if (s->ep[i].state != EP_DISABLED)
				ep_disable(x, sid, i);
		s->state = SS_ADDRESSED;
		slot_ctx_state(x, sid);
		return CC_SUCCESS;
	}
	uint64_t ic = t->p & ~0xFull, oc = out_ctx(x, sid);
	uint32_t icc[2];
	if (!oc || dma_rd(x, ic, icc, 8))
		return CC_TRB;
	if (icc[0] & 3)				/* D0, D1 reserved */
		return CC_PARAMETER;
	/* validate the endpoints to add first: nothing changes on an error */
	uint32_t c[32][8];
	for (unsigned i = 2; i < 32; i++) {
		if (!(icc[1] & (1u << i)))
			continue;
		if (dma_rd(x, ic + 32ull * (i + 1), c[i], 32))
			return CC_TRB;
		unsigned type = (c[i][1] >> 3) & 7;
		if (!type || !(c[i][1] >> 16) || !(c[i][2] & ~0xFu) )
			return CC_PARAMETER;
	}
	for (unsigned i = 2; i < 32; i++)
		if ((icc[0] & (1u << i)) && s->ep[i].state != EP_DISABLED)
			ep_disable(x, sid, i);
	for (unsigned i = 2; i < 32; i++) {
		if (!(icc[1] & (1u << i)))
			continue;
		ep_setup(&s->ep[i], c[i]);
		c[i][0] = (c[i][0] & ~7u) | EP_RUNNING;
		if (dma_wr(x, oc + 32ull * i, c[i], 32))
			return CC_TRB;
	}
	s->state = SS_ADDRESSED;
	for (unsigned i = 2; i < 32; i++)
		if (s->ep[i].state != EP_DISABLED)
			s->state = SS_CONFIGURED;
	slot_ctx_state(x, sid);
	return CC_SUCCESS;
}

static int
cmd_evaluate(struct xhci *x, unsigned sid, const struct trb *t)
{
	struct slot *s = &x->slot[sid];
	uint32_t in[24], out[16];
	if (s->state < SS_DEFAULT)
		return CC_CONTEXT_STATE;
	uint64_t oc = out_ctx(x, sid);
	if (!oc || dma_rd(x, t->p & ~0xFull, in, sizeof in) || dma_rd(x, oc, out, sizeof out))
		return CC_TRB;
	if (in[1] & 1) {			/* A0: max exit latency, interrupter target */
		out[1] = (out[1] & ~0xFFFFu) | (in[9] & 0xFFFF);
		out[2] = (out[2] & 0x003FFFFFu) | (in[10] & 0xFFC00000u);
	}
	if (in[1] & 2) {			/* A1: EP0 max packet size */
		out[9] = (out[9] & 0xFFFF) | (in[17] & 0xFFFF0000u);
		s->ep[1].mps = in[17] >> 16;
	}
	if (dma_wr(x, oc, out, sizeof out))
		return CC_TRB;
	return CC_SUCCESS;
}

static void
ep_reset_ctl(struct ep *e)
{
	e->phase = 0;
	e->off = 0;
	e->skip = 0;
	e->edtla = 0;
	e->pending = 0;
}

static int
cmd_ep(struct xhci *x, unsigned sid, const struct trb *t, int type)
{
	unsigned dci = (t->c >> 16) & 31;
	struct slot *s = &x->slot[sid];
	struct ep *e = &s->ep[dci];
	if (!dci || e->state == EP_DISABLED)
		return CC_EP_NOT_ENABLED;
	switch (type) {
	case TRB_RESET_EP:
		if (e->state != EP_HALTED)
			return CC_CONTEXT_STATE;
		e->state = EP_STOPPED;
		ep_reset_ctl(e);
		break;
	case TRB_STOP_EP:
		if (e->state != EP_RUNNING)
			return CC_CONTEXT_STATE;
		if (e->pending) {
			/* a TD in progress: a Stopped event for it (§4.6.9) */
			struct trb tr;
			if (!dma_rd(x, e->deq, &tr, 16))
				ev_xfer(x, sid, dci, e->deq, (tr.s & 0x1FFFF) - e->off, CC_STOPPED, 0);
		}
		e->state = EP_STOPPED;
		e->pending = 0;
		s->act &= ~(1u << dci);
		break;
	case TRB_SET_DEQ:
		if (e->state != EP_STOPPED && e->state != EP_ERROR)
			return CC_CONTEXT_STATE;
		e->deq = t->p & ~0xFull;
		e->ccs = t->p & 1;
		ep_reset_ctl(e);
		break;
	}
	ep_ctx_write(x, sid, dci);
	return CC_SUCCESS;
}

static void
do_cmd(struct xhci *x, uint64_t at, const struct trb *t)
{
	unsigned type = TRB_TYPE(t->c), sid = t->c >> 24;
	unsigned maxen = x->config & 0xFF;
	int cc;

	if (type == TRB_ENABLE_SLOT) {
		for (sid = 1; sid <= maxen && sid <= NSLOTS; sid++)
			if (!x->slot[sid].en)
				break;
		if (sid > maxen || sid > NSLOTS) {
			ev_cmd(x, at, CC_NO_SLOTS, 0);
			return;
		}
		memset(&x->slot[sid], 0, sizeof x->slot[sid]);
		x->slot[sid].en = 1;
		x->slot[sid].state = SS_ENABLED;
		ev_cmd(x, at, CC_SUCCESS, sid);
		return;
	}
	if (type == TRB_NOOP_CMD) {
		ev_cmd(x, at, CC_SUCCESS, 0);
		return;
	}
	if (type < TRB_DISABLE_SLOT || type > TRB_SET_DEQ) {
		ev_cmd(x, at, CC_TRB, 0);		/* not implemented */
		return;
	}
	if (!sid || sid > NSLOTS || !x->slot[sid].en) {
		ev_cmd(x, at, CC_SLOT_NOT_ENABLED, sid);
		return;
	}
	switch (type) {
	case TRB_DISABLE_SLOT: {
		uint64_t oc = out_ctx(x, sid);
		x->slot[sid].state = SS_ENABLED;
		x->slot[sid].en = 0;
		if (oc) {
			uint32_t c3 = 0;
			dma_wr(x, oc + 12, &c3, 4);	/* slot state: disabled */
		}
		memset(&x->slot[sid], 0, sizeof x->slot[sid]);
		cc = CC_SUCCESS;
		break;
	}
	case TRB_ADDRESS_DEV: cc = cmd_address(x, sid, t); break;
	case TRB_CONFIG_EP: cc = cmd_configure(x, sid, t); break;
	case TRB_EVAL_CTX: cc = cmd_evaluate(x, sid, t); break;
	default: cc = cmd_ep(x, sid, t, (int)type); break;
	}
	ev_cmd(x, at, cc, sid);
}

static void
cmd_run(struct xhci *x, int *budget)
{
	while (x->crr && *budget > 0 && running(x)) {
		struct trb t;
		if (dma_rd(x, x->crp, &t, 16))
			return;
		(*budget)--;
		if ((int)(t.c & C_CYCLE) != x->rcs) {
			x->cmd_wait = 1;	/* empty; CRR stays 1 until the next doorbell */
			return;
		}
		if (TRB_TYPE(t.c) == TRB_LINK) {
			if (t.c & C_ENT_TC)
				x->rcs ^= 1;
			x->crp = t.p & ~0x3Full;
			continue;
		}
		uint64_t at = x->crp;
		x->crp += 16;
		do_cmd(x, at, &t);
	}
}

/* ---------------------------------------------------------------- transfers */

static void
ep_halt(struct xhci *x, unsigned sid, unsigned dci)
{
	struct ep *e = &x->slot[sid].ep[dci];
	e->state = EP_HALTED;
	e->pending = 0;
	x->slot[sid].act &= ~(1u << dci);
	ep_ctx_write(x, sid, dci);
}

static void
ep_advance(struct ep *e)
{
	e->deq += 16;
	e->off = 0;
}

/*
 * The TRB at the endpoint's dequeue pointer, following Link TRBs.
 * Returns 1 with *t, 0 if the ring is empty, -1 on a DMA error.
 */
static int
ep_fetch(struct xhci *x, struct ep *e, struct trb *t, int *budget)
{
	while (*budget > 0) {
		(*budget)--;
		if (dma_rd(x, e->deq, t, 16))
			return -1;
		if ((int)(t->c & C_CYCLE) != e->ccs)
			return 0;
		if (TRB_TYPE(t->c) != TRB_LINK)
			return 1;
		if (t->c & C_ENT_TC)
			e->ccs ^= 1;
		e->deq = t->p & ~0xFull;
		e->off = 0;
	}
	return 0;
}

/* Event Data and No Op TRBs on a transfer ring (§4.11.5.2, §6.4.4.1) */
static int
ep_misc_trb(struct xhci *x, unsigned sid, unsigned dci, struct ep *e, const struct trb *t)
{
	if (TRB_TYPE(t->c) == TRB_EVDATA) {
		if (t->c & C_IOC)
			ev_xfer(x, sid, dci, t->p, e->edtla, e->skip ? CC_SHORT : CC_SUCCESS, 1);
		e->edtla = 0;
		if (!(t->c & C_CH))
			e->skip = 0;
		ep_advance(e);
		return 1;
	}
	if (TRB_TYPE(t->c) == TRB_NOOP) {
		if (t->c & C_IOC)
			ev_xfer(x, sid, dci, e->deq, 0, CC_SUCCESS, 0);
		if (!(t->c & C_CH))
			e->skip = 0;
		ep_advance(e);
		return 1;
	}
	return 0;
}

/* the default control endpoint (§4.11.2.2) */
static void
ep0_run(struct xhci *x, unsigned sid, int *budget)
{
	struct ep *e = &x->slot[sid].ep[1];
	struct usbdev *u = slot_dev(x, sid);
	struct trb t;

	while (e->state == EP_RUNNING && *budget > 0) {
		int r = ep_fetch(x, e, &t, budget);
		if (r <= 0) {
			if (r == 0)
				x->slot[sid].act &= ~2u;	/* empty: wait for a doorbell */
			return;
		}
		if (ep_misc_trb(x, sid, 1, e, &t))
			continue;
		unsigned type = TRB_TYPE(t.c), len = t.s & 0x1FFFF;
		if (type == TRB_SETUP) {
			if (!(t.c & C_IDT) || len != 8) {
				ev_xfer(x, sid, 1, e->deq, len, CC_TRB, 0);
				ep_halt(x, sid, 1);
				return;
			}
			if (!u || !(x->portsc[x->slot[sid].port - 1] & PS_PED)) {
				ev_xfer(x, sid, 1, e->deq, len, CC_TRANSACTION, 0);
				ep_halt(x, sid, 1);
				return;
			}
			memcpy(e->setup, &t.p, 8);
			e->in = (e->setup[0] & 0x80) != 0;
			e->phase = 1;
			e->skip = 0;
			e->pos = e->len = 0;
			e->edtla = 0;
			e->stall = 0;
			unsigned wlen = e->setup[6] | e->setup[7] << 8;
			if (e->in || wlen == 0) {
				/* run IN and no-data requests now; OUT data at the status stage */
				unsigned n = 0;
				e->stall = dev_request(u, e->setup, e->buf, &n) != 0;
				e->len = n;
			}
			if (t.c & C_IOC)
				ev_xfer(x, sid, 1, e->deq, 0, CC_SUCCESS, 0);
			ep_advance(e);
			continue;
		}
		if (type != TRB_DATA && type != TRB_NORMAL && type != TRB_STATUS) {
			ev_xfer(x, sid, 1, e->deq, len, CC_TRB, 0);
			ep_halt(x, sid, 1);
			return;
		}
		if (!e->phase) {			/* a data/status stage without a setup */
			ev_xfer(x, sid, 1, e->deq, len, CC_TRB, 0);
			ep_halt(x, sid, 1);
			return;
		}
		if (type != TRB_STATUS && e->skip) {	/* after a short packet */
			ep_advance(e);
			continue;
		}
		if (type == TRB_STATUS && !e->stall && !e->in &&
		    (e->setup[6] | e->setup[7] << 8)) {
			/* the OUT data stage is complete: the device takes it now */
			unsigned n = e->pos;
			e->stall = dev_request(u, e->setup, e->buf, &n) != 0;
		}
		if (e->stall) {
			/* the device returned STALL in the data or status stage */
			ev_xfer(x, sid, 1, e->deq, type == TRB_STATUS ? 0 : len, CC_STALL, 0);
			e->phase = 0;
			ep_halt(x, sid, 1);
			return;
		}
		if (type == TRB_STATUS) {
			if (t.c & C_IOC)
				ev_xfer(x, sid, 1, e->deq, 0, CC_SUCCESS, 0);
			e->phase = 0;
			e->skip = 0;
			ep_advance(e);
			continue;
		}
		/* data stage (or a Normal TRB chained to it) */
		int din = type == TRB_DATA ? (t.c & C_DIR_IN) != 0 : e->in;
		if (din != e->in) {
			ev_xfer(x, sid, 1, e->deq, len, CC_TRB, 0);
			ep_halt(x, sid, 1);
			return;
		}
		if (e->in) {
			unsigned n = e->len - e->pos < len ? e->len - e->pos : len;
			if (n && ((t.c & C_IDT) || dma_wr(x, t.p, e->buf + e->pos, n)))
				return;
			e->pos += n;
			e->edtla += n;
			if (n < len) {
				if (t.c & (C_ISP | C_IOC))
					ev_xfer(x, sid, 1, e->deq, len - n, CC_SHORT, 0);
				e->skip = 1;
			} else if (t.c & C_IOC)
				ev_xfer(x, sid, 1, e->deq, 0, CC_SUCCESS, 0);
		} else {
			unsigned n = len;
			if (e->pos + n > CTLBUF)
				n = CTLBUF - e->pos;
			if (t.c & C_IDT)
				memcpy(e->buf + e->pos, &t.p, n > 8 ? 8 : n);
			else if (n && dma_rd(x, t.p, e->buf + e->pos, n))
				return;
			e->pos += n;
			e->edtla += n;
			if (t.c & C_IOC)
				ev_xfer(x, sid, 1, e->deq, 0, CC_SUCCESS, 0);
		}
		ep_advance(e);
	}
}

/* an interrupt IN endpoint (§4.11.2.1, §4.14.2): one packet per service */
static void
epint_run(struct xhci *x, unsigned sid, unsigned dci, int *budget)
{
	struct ep *e = &x->slot[sid].ep[dci];
	struct usbdev *u = slot_dev(x, sid);
	struct trb t;
	int r;

	/* skip the rest of a TD that ended with a short packet */
	while (e->state == EP_RUNNING && e->skip && (r = ep_fetch(x, e, &t, budget)) > 0) {
		if (ep_misc_trb(x, sid, dci, e, &t))
			continue;
		if (!(t.c & C_CH))
			e->skip = 0;
		ep_advance(e);
	}
	if (e->state != EP_RUNNING || e->skip)
		return;
	r = ep_fetch(x, e, &t, budget);
	if (r <= 0) {
		if (r == 0) {
			x->slot[sid].act &= ~(1u << dci);
			e->pending = 0;
		}
		return;
	}
	if (ep_misc_trb(x, sid, dci, e, &t))
		return;
	if (TRB_TYPE(t.c) != TRB_NORMAL) {
		ev_xfer(x, sid, dci, e->deq, t.s & 0x1FFFF, CC_TRB, 0);
		ep_halt(x, sid, dci);
		return;
	}
	e->pending = 1;
	if (!u || !(x->portsc[x->slot[sid].port - 1] & PS_PED) || e->type != EPT_INT_IN)
		return;				/* nothing answers: keep the TRB */
	if (!dev_has_data(u, x->ticks))
		return;				/* NAK */
	uint8_t pkt[64];
	unsigned plen = dev_fetch(u, pkt, x->ticks), pos = 0;
	if (plen > e->mps)
		plen = e->mps;
	/* spread the packet over the TD's TRBs */
	for (;;) {
		unsigned len = t.s & 0x1FFFF, room = len - e->off;
		unsigned n = plen - pos < room ? plen - pos : room;
		if (n && ((t.c & C_IDT) || dma_wr(x, t.p + e->off, pkt + pos, n)))
			return;
		e->off += n;
		pos += n;
		e->edtla += n;
		if (e->off == len) {
			if (t.c & C_IOC)
				ev_xfer(x, sid, dci, e->deq, 0, CC_SUCCESS, 0);
			int ch = (t.c & C_CH) != 0;
			ep_advance(e);
			e->pending = 0;
			if (!ch || (pos == plen && plen == e->mps))
				return;		/* TD done, or wait for the next packet */
			if ((r = ep_fetch(x, e, &t, budget)) <= 0 || TRB_TYPE(t.c) != TRB_NORMAL)
				return;
			continue;
		}
		if (plen < e->mps) {		/* short packet: the TD ends here */
			if (t.c & (C_ISP | C_IOC))
				ev_xfer(x, sid, dci, e->deq, len - e->off, CC_SHORT, 0);
			e->skip = (t.c & C_CH) != 0;
			ep_advance(e);
			e->pending = 0;
		}
		return;
	}
}

/*
 * A bulk endpoint (§4.11.2.1): each Normal TRB is serviced whole (the
 * device's data phases are as long as the host asks, so packets are never
 * short except a CSW or data read into a longer buffer, which ends the TD).
 */
static uint8_t bulk_buf[0x20000];

static void
epbulk_run(struct xhci *x, unsigned sid, unsigned dci, int *budget)
{
	struct ep *e = &x->slot[sid].ep[dci];
	struct usbdev *u = slot_dev(x, sid);
	struct trb t;

	while (e->state == EP_RUNNING && *budget > 0) {
		int r = ep_fetch(x, e, &t, budget);
		if (r <= 0) {
			if (r == 0) {
				x->slot[sid].act &= ~(1u << dci);
				e->pending = 0;
			}
			return;
		}
		if (ep_misc_trb(x, sid, dci, e, &t))
			continue;
		if (TRB_TYPE(t.c) != TRB_NORMAL) {
			ev_xfer(x, sid, dci, e->deq, t.s & 0x1FFFF, CC_TRB, 0);
			ep_halt(x, sid, dci);
			return;
		}
		if (e->skip) {				/* the rest of a TD after a short packet */
			if (!(t.c & C_CH))
				e->skip = 0;
			ep_advance(e);
			continue;
		}
		unsigned len = t.s & 0x1FFFF;
		if (!u || u->kind != DEV_MSC || !(x->portsc[x->slot[sid].port - 1] & PS_PED)) {
			e->pending = 1;			/* nothing answers: keep the TRB */
			return;
		}
		if (e->type == EPT_BULK_OUT) {
			if (t.c & C_IDT)
				memcpy(bulk_buf, &t.p, len > 8 ? 8 : len);
			else if (len && dma_rd(x, t.p, bulk_buf, len))
				return;
			msc_out(u, bulk_buf, len);
			e->edtla += len;
			if (t.c & C_IOC)
				ev_xfer(x, sid, dci, e->deq, 0, CC_SUCCESS, 0);
			ep_advance(e);
			continue;
		}
		if (e->type != EPT_BULK_IN)
			return;
		if (!msc_avail(u)) {
			e->pending = 1;			/* NAK until the device has data */
			return;
		}
		unsigned n = msc_in(u, bulk_buf, len);
		if (n && ((t.c & C_IDT) || dma_wr(x, t.p, bulk_buf, n)))
			return;
		e->edtla += n;
		e->pending = 0;
		if (n < len) {
			if (t.c & (C_ISP | C_IOC))
				ev_xfer(x, sid, dci, e->deq, len - n, CC_SHORT, 0);
			e->skip = (t.c & C_CH) != 0;
		} else if (t.c & C_IOC)
			ev_xfer(x, sid, dci, e->deq, 0, CC_SUCCESS, 0);
		ep_advance(e);
	}
}

/* ---------------------------------------------------------------- registers */

static void
hc_reset(struct xhci *x)
{
	x->usbcmd = 0;
	x->usbsts = STS_HCH | STS_CNR;
	x->cnr = CNR_TICKS;
	x->dnctrl = x->config = 0;
	x->crp = x->dcbaap = 0;
	x->rcs = x->crr = x->cmd_wait = 0;
	x->iman = 0;
	x->imod = 4000;
	x->erstsz = 0;
	x->erstba = x->erdp = 0;
	x->er_ok = 0;
	x->er_seg = 0;
	x->pcs = 1;
	x->evh = x->evn = 0;
	memset(x->slot, 0, sizeof x->slot);
	for (int i = 0; i < NPORTS; i++) {
		dev_reset(&x->dev[i]);
		port_init(x, i);
	}
}

static uint32_t
reg_read(struct xhci *x, unsigned off)
{
	if (off < CAPLENGTH) {
		switch (off) {
		case 0x00: return CAPLENGTH | 0x0100u << 16;		/* HCIVERSION 1.0.0 */
		case 0x04: return NSLOTS | 1u << 8 | (uint32_t)NPORTS << 24;
		case 0x08: return ERST_MAX << 4;			/* IST 0, no scratchpads */
		case 0x0C: return 0;
		case 0x10: return 1u | (uint32_t)(XECP >> 2) << 16;	/* AC64, CSZ 0, PPC 0 */
		case 0x14: return DBB;
		case 0x18: return RTB;
		default: return 0;
		}
	}
	if (off >= XECP && off < XECP + 16) {
		/* Supported Protocol capability (§7.2): USB 2.0, ports 1-4 */
		switch (off - XECP) {
		case 0x0: return 2u | 0x0200u << 16;		/* ID 2, next 0, rev 2.00 */
		case 0x4: return 0x20425355;			/* "USB " */
		case 0x8: return 1u | (uint32_t)NPORTS << 8;	/* ports 1..4, PSIC 0 */
		default: return 0;				/* slot type 0 */
		}
	}
	if (off >= OPB && off < OPB + 0x400) {
		switch (off - OPB) {
		case 0x00: return x->usbcmd;
		case 0x04: return x->usbsts;
		case 0x08: return 1;				/* 4 KiB pages */
		case 0x14: return x->dnctrl;
		case 0x18: return x->crr ? 8 : 0;		/* CRR; the pointer reads 0 */
		case 0x30: return (uint32_t)x->dcbaap;
		case 0x34: return (uint32_t)(x->dcbaap >> 32);
		case 0x38: return x->config;
		default: return 0;
		}
	}
	if (off >= OPB + 0x400 && off < OPB + 0x400 + 0x10 * NPORTS) {
		unsigned p = (off - OPB - 0x400) >> 4;
		return (off & 0xF) == 0 ? x->portsc[p] : 0;
	}
	if (off >= RTB && off < RTB + 0x40) {
		switch (off - RTB) {
		case 0x00:
			return running(x) ? (uint32_t)((x->ticks - x->mf0) / MFINDEX_TICKS) & 0x3FFF : 0;
		case 0x20: return x->iman;
		case 0x24: return x->imod;
		case 0x28: return x->erstsz;
		case 0x30: return (uint32_t)x->erstba;
		case 0x34: return (uint32_t)(x->erstba >> 32);
		case 0x38: return (uint32_t)x->erdp;
		case 0x3C: return (uint32_t)(x->erdp >> 32);
		default: return 0;
		}
	}
	return 0;			/* doorbells and reserved space read 0 */
}

static void
doorbell(struct xhci *x, unsigned n, uint32_t v)
{
	if (!running(x))
		return;
	if (n == 0) {
		if ((v & 0xFF) == 0) {
			x->crr = 1;
			x->cmd_wait = 0;
		}
		return;
	}
	unsigned dci = v & 0xFF;
	if (n > NSLOTS || !x->slot[n].en || dci < 1 || dci > 31)
		return;
	struct ep *e = &x->slot[n].ep[dci];
	if (e->state == EP_DISABLED || e->state == EP_HALTED || e->state == EP_ERROR)
		return;
	if (e->state == EP_STOPPED) {
		e->state = EP_RUNNING;
		ep_ctx_write(x, n, dci);
	}
	x->slot[n].act |= 1u << dci;
}

static void
reg_write(struct xhci *x, unsigned off, uint32_t v)
{
	if (off >= OPB && off < OPB + 0x400) {
		switch (off - OPB) {
		case 0x00:
			if (v & CMD_HCRST) {
				hc_reset(x);
				return;
			}
			if ((v & CMD_RS) && !(x->usbcmd & CMD_RS) && !(x->usbsts & STS_HSE)) {
				x->usbcmd = v & CMD_RW;
				x->usbsts &= ~STS_HCH;
				x->mf0 = x->ticks;
				/* report the ports that changed while halted */
				for (int i = 0; i < NPORTS; i++)
					if (x->portsc[i] & PS_CHANGE) {
						x->usbsts |= STS_PCD;
						ev_post(x, (uint64_t)(i + 1) << 24,
						    (uint32_t)CC_SUCCESS << 24, TRB_EV_PORT << 10);
					}
				return;
			}
			if (!(v & CMD_RS) && (x->usbcmd & CMD_RS)) {
				x->usbsts |= STS_HCH;
				x->crr = 0;
			}
			x->usbcmd = v & CMD_RW;
			if (x->usbsts & STS_HSE)
				x->usbcmd &= ~CMD_RS;
			return;
		case 0x04: x->usbsts &= ~(v & STS_RW1C); return;
		case 0x14: x->dnctrl = v & 0xFFFF; return;
		case 0x18:
			if (x->crr) {
				/* CS / CA: stop the ring; commands finish at once, so an
				 * abort is a stop (§4.6.1.2) */
				if (v & 6) {
					x->crr = 0;
					ev_cmd(x, x->crp, CC_CMD_RING_STOPPED, 0);
				}
				return;
			}
			x->crp = (x->crp & ~0xFFFFFFFFull) | (v & ~0x3Fu);
			x->rcs = v & 1;
			return;
		case 0x1C:
			if (!x->crr)
				x->crp = (x->crp & 0xFFFFFFFFull) | (uint64_t)v << 32;
			return;
		case 0x30: x->dcbaap = (x->dcbaap & ~0xFFFFFFFFull) | (v & ~0x3Fu); return;
		case 0x34: x->dcbaap = (x->dcbaap & 0xFFFFFFFFull) | (uint64_t)v << 32; return;
		case 0x38: x->config = v & 0x3FF; return;
		default: return;
		}
	}
	if (off >= OPB + 0x400 && off < OPB + 0x400 + 0x10 * NPORTS) {
		if ((off & 0xF) == 0)
			port_write(x, (int)((off - OPB - 0x400) >> 4), v);
		return;
	}
	if (off >= RTB && off < RTB + 0x40) {
		switch (off - RTB) {
		case 0x20:
			x->iman = (x->iman & ~(v & 1)) & ~2u;	/* IP: RW1C */
			x->iman |= v & 2;
			return;
		case 0x24: x->imod = v; return;
		case 0x28: x->erstsz = v & 0xFFFF; return;
		case 0x30:
		case 0x34:
			if (off - RTB == 0x30)
				x->erstba = (x->erstba & ~0xFFFFFFFFull) | (v & ~0x3Fu);
			else
				x->erstba = (x->erstba & 0xFFFFFFFFull) | (uint64_t)v << 32;
			/* writing ERSTBA (re)initializes the event ring (§4.9.4) */
			x->er_ok = 0;
			x->er_seg = 0;
			x->pcs = 1;
			return;
		case 0x38:
			x->erdp = (x->erdp & ~0xFFFFFFFFull & ~8ull) | (v & ~0xFu) |
			    ((x->erdp & 8) & ~(uint64_t)(v & 8));	/* EHB: RW1C */
			irq_check(x);
			return;
		case 0x3C: x->erdp = (x->erdp & 0xFFFFFFFFull) | (uint64_t)v << 32; return;
		default: return;
		}
	}
	if (off >= DBB && off < DBB + 4 * (NSLOTS + 1))
		doorbell(x, (off - DBB) >> 2, v);
}

static int
xhci_bar(struct pcidev *d, int we, unsigned bar, uint64_t off, unsigned n, uint8_t *data)
{
	struct xhci *x = (struct xhci *)d;
	if (bar != 0 || (off & (n - 1)) || off + n > XHCI_BAR_SIZE)
		return -1;
	if (we) {
		if (n != 4 && n != 8)
			return -1;
		for (unsigned i = 0; i < n; i += 4) {	/* the low dword first */
			uint32_t v;
			memcpy(&v, data + i, 4);
			reg_write(x, (unsigned)off + i, v);
		}
		return 0;
	}
	if (n > 8)
		return -1;
	uint32_t w[2];
	unsigned base = (unsigned)off & ~3u;
	w[0] = reg_read(x, base);
	w[1] = n == 8 ? reg_read(x, base + 4) : 0;
	memcpy(data, (uint8_t *)w + (off & 3), n);
	return 0;
}

static void
xhci_tick(struct pcidev *d)
{
	struct xhci *x = (struct xhci *)d;
	x->ticks++;
	if (x->cnr && --x->cnr == 0)
		x->usbsts &= ~STS_CNR;
	for (int i = 0; i < NPORTS; i++)
		if (x->preset[i])
			port_tick(x, i);
	if (!running(x))
		return;
	int budget = TRB_BUDGET;
	if (x->crr && !x->cmd_wait)
		cmd_run(x, &budget);
	for (unsigned s = 1; s <= NSLOTS && budget > 0; s++) {
		struct slot *sl = &x->slot[s];
		if (!sl->en)
			continue;
		if (sl->act & 2)
			ep0_run(x, s, &budget);
		struct usbdev *u = sl->state == SS_CONFIGURED ? slot_dev(x, s) : NULL;
		int data = u && dev_has_data(u, x->ticks);
		for (unsigned i = 2; i < 32 && budget > 0; i++) {
			struct ep *e = &sl->ep[i];
			if (e->state != EP_RUNNING || !(sl->act & (1u << i)))
				continue;
			/* a waiting interrupt TRB only needs service when data came */
			if (e->pending && !e->skip && !data)
				continue;
			if (e->type == EPT_BULK_IN || e->type == EPT_BULK_OUT)
				epbulk_run(x, s, i, &budget);
			else
				epint_run(x, s, i, &budget);
		}
	}
	if (x->evn)
		ev_flush(x);
}

static void
xhci_reset(struct pcidev *d)
{
	struct xhci *x = (struct xhci *)d;
	memset(&d->f, 0, sizeof d->f);
	struct pcifn *f = &d->f;
	/* config space as the DMA test endpoint's (pcie.c), class 0C/03/30 */
#define PUT(o, n, v, w) do { for (unsigned i_ = 0; i_ < (n); i_++) { \
	f->cfg[(o) + i_] = (uint8_t)((uint32_t)(v) >> (8 * i_)); \
	f->wm[(o) + i_] = (uint8_t)((uint32_t)(w) >> (8 * i_)); } } while (0)
	PUT(0x00, 2, PCI_VENDOR, 0);
	PUT(0x02, 2, PCI_DEV_XHCI, 0);
	PUT(0x04, 2, 0x0000, 0x0546);
	PUT(0x06, 2, 0x0010, 0);
	PUT(0x08, 4, 0x0C033001, 0);			/* class 0C/03/30: xHCI, revision 1 */
	PUT(0x10, 4, 0x00000004, (uint32_t)~(XHCI_BAR_SIZE - 1));	/* 64-bit memory */
	PUT(0x14, 4, 0, 0xFFFFFFFF);
	PUT(0x2C, 4, (uint32_t)PCI_DEV_XHCI << 16 | PCI_VENDOR, 0);
	PUT(0x34, 1, 0x40, 0);
	PUT(0x40, 2, 0x7005, 0);			/* MSI, next 0x70 */
	PUT(0x42, 2, 0x0080, 0x0001);			/* 64-bit address; enable */
	PUT(0x44, 4, 0, 0xFFFFFFFC);
	PUT(0x48, 4, 0, 0xFFFFFFFF);
	PUT(0x4C, 2, 0, 0xFFFF);
	PUT(0x60, 1, 0x20, 0);				/* SBRN: USB 2.0 (xHCI 1.2 §5.2.3) */
	PUT(0x61, 1, 0x20, 0x3F);			/* FLADJ: default 0x20 (§5.2.4) */
	PUT(0x70, 2, 0x0010, 0);			/* PCIe capability */
	PUT(0x72, 2, 0x0002, 0);			/* v2, endpoint */
#undef PUT
	hc_reset(x);
	x->ticks = 0;
}

struct pcidev *
xhci_new(void)
{
	struct xhci *x = calloc(1, sizeof *x);
	if (!x)
		return NULL;
	x->d.bar = xhci_bar;
	x->d.tick = xhci_tick;
	x->d.reset = xhci_reset;
	x->d.bar_size = XHCI_BAR_SIZE;
	x->dev[0].kind = DEV_KBD;
	x->dev[0].speed = SPEED_FULL;
	x->dev[1].kind = DEV_MOUSE;
	x->dev[1].speed = SPEED_FULL;
	xhci_reset(&x->d);
	return &x->d;
}

/* ---------------------------------------------------------------- floppy */

int
xhci_msc_attach(struct pcidev *d, const char *path, int ro)
{
	struct xhci *x = (struct xhci *)d;
	struct usbdev *u = &x->dev[2];
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0 || n % 512) {
		fclose(f);
		return -1;
	}
	uint8_t *m = malloc((size_t)n);
	if (!m || fread(m, 1, (size_t)n, f) != (size_t)n) {
		free(m);
		fclose(f);
		return -1;
	}
	fclose(f);
	free(u->img);
	u->img = m;
	u->img_size = (uint64_t)n;
	u->ro = ro;
	u->kind = DEV_MSC;
	u->speed = SPEED_FULL;
	dev_reset(u);
	port_init(x, 2);
	return 0;
}

/* ---------------------------------------------------------------- input */

void
xhci_key(struct pcidev *d, unsigned usage, int down)
{
	struct usbdev *u = &((struct xhci *)d)->dev[0];
	if (usage >= 0xE0 && usage <= 0xE7) {
		uint8_t m = (uint8_t)(1u << (usage - 0xE0)), old = u->mods;
		u->mods = down ? u->mods | m : u->mods & ~m;
		if (u->mods != old)
			kbd_changed(u);
		return;
	}
	if (usage < 4 || usage > 0xFF)
		return;
	int i;
	for (i = 0; i < u->nheld && u->held[i] != usage; i++)
		;
	if (down) {
		if (i < u->nheld || u->nheld == MAXHELD)
			return;			/* already held (or too many) */
		u->held[u->nheld++] = (uint8_t)usage;
	} else {
		if (i == u->nheld)
			return;
		memmove(u->held + i, u->held + i + 1, (size_t)(u->nheld - i - 1));
		u->nheld--;
	}
	kbd_changed(u);
}

void
xhci_key_release_all(struct pcidev *d)
{
	struct usbdev *u = &((struct xhci *)d)->dev[0];
	if (!u->nheld && !u->mods)
		return;
	u->nheld = 0;
	u->mods = 0;
	kbd_changed(u);
}

void
xhci_mouse_move(struct pcidev *d, int dx, int dy)
{
	struct usbdev *u = &((struct xhci *)d)->dev[1];
	if (!u->config)
		return;
	/* keep the sum bounded: a host that never reads mustn't overflow it */
	u->dx = u->dx + dx > 32767 ? 32767 : u->dx + dx < -32767 ? -32767 : u->dx + dx;
	u->dy = u->dy + dy > 32767 ? 32767 : u->dy + dy < -32767 ? -32767 : u->dy + dy;
}

void
xhci_mouse_button(struct pcidev *d, unsigned bit, int down)
{
	struct usbdev *u = &((struct xhci *)d)->dev[1];
	uint8_t m = (uint8_t)(1u << bit), b = down ? u->btn | m : u->btn & ~m;
	if (bit > 2 || b == u->btn)
		return;
	u->btn = b;
	if (!u->config)
		return;
	if (u->bqn == 8)
		u->bqn--;
	u->bq[(u->bqh + u->bqn) % 8] = b;
	u->bqn++;
}
