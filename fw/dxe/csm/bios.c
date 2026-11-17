/*
 * CsmDxe: the legacy BIOS services, called from csm16.S's thunk with the
 * caller's register frame. Interfaces after the IBM PC/AT BIOS (IBM,
 * "Technical Reference, Personal Computer AT", 1984), with the later
 * extensions OSes use (INT 13h AH=41h-48h: Phoenix "Enhanced Disk Drive
 * Specification" 3.0, 1998; INT 15h AX=E820h: ACPI 6.5 15.1; INT 10h as on
 * VGA, IBM "VGA Technical Reference", 1987).
 *
 *  INT 10h  video, text modes, on the display controller's text cells
 *           (VRAM at F000_0000h: there is no legacy VGA), with console
 *           redirection of the output to COM1 (VT100 cursor addressing);
 *  INT 13h  the boot device's Block I/O: drive 00h (a floppy, CHS from
 *           its size) or 80h (a disk, CHS 255/63 and the EDD calls);
 *  INT 16h  ConIn: the USB keyboard and the serial console, as BIOS
 *           scan code / ASCII pairs (set 1 make codes, US layout);
 *  INT 1Ah  the BDA tick count (IRQ0, csm16.S) and the RTC;
 *  INT 15h  memory size (88h, E801h, E820h), wait (86h), configuration
 *           table (C0h), A20 (2400h-2403h);
 *  INT 11h, 12h, 14h (COM1), 17h (no printer), 18h, 19h.
 */
#include "csm.h"

/* ---------------------------------------------------------------- video */

#define COLS	80
#define ROWS	25

static unsigned ser_row = ~0u, ser_col = ~0u;

static volatile UINT16 *
cell(unsigned r, unsigned c)
{
	return (volatile UINT16 *)(uintptr_t)(VGA_VRAM + r * 160 + c * 2);
}

static void
ser_goto(unsigned r, unsigned c)
{
	if (r == ser_row && c == ser_col)
		return;
	char b[16];
	snprintf(b, sizeof b, "\033[%u;%uH", r + 1, c + 1);
	for (char *p = b; *p; p++)
		com1_putc(*p);
	ser_row = r;
	ser_col = c;
}

static void
ser_char(unsigned r, unsigned c, UINT8 ch)
{
	ser_goto(r, c);
	com1_putc(ch >= 0x20 && ch < 0x7F ? ch : ch >= 0xB3 && ch <= 0xDA ? '+' : ' ');
	ser_col++;
}

static void
get_cursor(unsigned *r, unsigned *c)
{
	*c = *BDA(0x50);
	*r = *BDA(0x51);
	if (*c >= COLS)
		*c = COLS - 1;
	if (*r >= ROWS)
		*r = ROWS - 1;
}

static void
set_cursor(unsigned r, unsigned c)
{
	*BDA(0x50) = (UINT8)c;
	*BDA(0x51) = (UINT8)r;
	int visible = !(BDA16(0x60) & 0x2000);
	mmio_w64(VGA_REGS + VGA_CURSOR, c | r << 8 | (visible ? 1u << 16 : 0));
}

static void
put(unsigned r, unsigned c, UINT8 ch, UINT8 attr)
{
	*cell(r, c) = (UINT16)(ch | attr << 8);
	ser_char(r, c, ch);
}

static void
scroll(int up, unsigned n, UINT8 attr, unsigned top, unsigned left, unsigned bot, unsigned right)
{
	if (bot >= ROWS)
		bot = ROWS - 1;
	if (right >= COLS)
		right = COLS - 1;
	if (top > bot || left > right)
		return;
	unsigned h = bot - top + 1;
	if (!n || n > h)
		n = h;
	for (unsigned i = 0; i < h; i++) {
		unsigned r = up ? top + i : bot - i;
		for (unsigned c = left; c <= right; c++) {
			UINT16 v = (UINT16)(' ' | attr << 8);
			if (i + n < h)
				v = *cell(up ? r + n : r - n, c);
			*cell(r, c) = v;
		}
	}
	/* the serial terminal: a whole-screen clear clears it; other scrolls
	   redraw the window */
	if (n == h && top == 0 && left == 0 && bot == ROWS - 1 && right == COLS - 1) {
		const char *cls = "\033[2J";
		while (*cls)
			com1_putc(*cls++);
		ser_row = ser_col = ~0u;
	} else {
		for (unsigned r = top; r <= bot; r++) {
			ser_goto(r, left);
			for (unsigned c = left; c <= right; c++)
				ser_char(r, c, (UINT8)*cell(r, c));
		}
	}
}

static void
teletype(UINT8 ch)
{
	unsigned r, c;
	get_cursor(&r, &c);
	switch (ch) {
	case 7:
		return;
	case 8:
		if (c)
			c--;
		break;
	case 10:
		r++;
		break;
	case 13:
		c = 0;
		break;
	default:
		put(r, c, ch, (UINT8)(*cell(r, c) >> 8));
		c++;
	}
	if (c >= COLS) {
		c = 0;
		r++;
	}
	if (r >= ROWS) {
		scroll(1, 1, (UINT8)(*cell(ROWS - 1, 0) >> 8), 0, 0, ROWS - 1, COLS - 1);
		r = ROWS - 1;
	}
	set_cursor(r, c);
}

static void
video_clear(void)
{
	mmio_w64(VGA_REGS + VGA_MODE, 1);
	mmio_w64(VGA_REGS + VGA_BASE, 0);
	mmio_w64(VGA_REGS + VGA_CLEAR, 0x07);
	const char *cls = "\033[0m\033[2J";
	while (*cls)
		com1_putc(*cls++);
	ser_row = ser_col = ~0u;
	BDA16(0x60) = 0x0607;
	set_cursor(0, 0);
}

static void
int10(struct rm_frame *f)
{
	unsigned r, c;
	get_cursor(&r, &c);
	switch (AH(f)) {
	case 0x00:				/* set mode */
		*BDA(0x49) = AL(f) & 0x7F;
		if ((AL(f) & 0x7F) > 3 && (AL(f) & 0x7F) != 7)
			DEBUG("CSM: INT 10h mode %02x: graphics modes aren't supported\n", AL(f) & 0x7F);
		if (!(AL(f) & 0x80))
			video_clear();
		break;
	case 0x01:				/* cursor shape */
		BDA16(0x60) = CX(f);
		set_cursor(r, c);
		break;
	case 0x02:				/* set the cursor position */
		if (BH(f) == 0)
			set_cursor(DH(f), DL(f));
		break;
	case 0x03:				/* get it, and the shape */
		set_dx(f, (UINT16)(r << 8 | c));
		set_cx(f, BDA16(0x60));
		break;
	case 0x05:				/* active page: only 0 */
		break;
	case 0x06:
	case 0x07:				/* scroll a window */
		scroll(AH(f) == 6, AL(f), BH(f), CH(f), CL(f), DH(f), DL(f));
		break;
	case 0x08: {				/* read the character and attribute */
		UINT16 v = *cell(r, c);
		set_ax(f, v);
		break;
	}
	case 0x09:
	case 0x0A: {				/* write it CX times at the cursor */
		unsigned pos = r * COLS + c;
		for (unsigned i = 0; i < CX(f) && pos < ROWS * COLS; i++, pos++) {
			UINT8 attr = AH(f) == 9 ? BL(f) : (UINT8)(*cell(pos / COLS, pos % COLS) >> 8);
			put(pos / COLS, pos % COLS, AL(f), attr);
		}
		break;
	}
	case 0x0E:				/* teletype */
		teletype(AL(f));
		break;
	case 0x0F:				/* get the mode */
		set_ax(f, (UINT16)(COLS << 8 | *BDA(0x49)));
		f->ebx &= ~0xFF00u;
		break;
	case 0x10:				/* palette, blinking: ignored */
		break;
	case 0x11:
		if (AL(f) == 0x30) {		/* font information */
			set_cx(f, 16);
			set_dl(f, ROWS - 1);
			f->es = 0;
			f->ebp &= ~0xFFFFu;
		}
		break;
	case 0x12:
		if (BL(f) == 0x10) {		/* EGA/VGA information: colour, 256 KiB */
			set_bx(f, 0x0003);
			set_cx(f, 0x0009);
		}
		break;
	case 0x13: {				/* write a string */
		const UINT8 *s = lin(f->es, f->ebp & 0xFFFF);
		unsigned rr = DH(f), cc = DL(f), n = CX(f);
		int attrs = AL(f) & 2;
		set_cursor(rr, cc);
		for (unsigned i = 0; i < n; i++) {
			UINT8 ch = attrs ? s[2 * i] : s[i], at = attrs ? s[2 * i + 1] : BL(f);
			if (ch == 7 || ch == 8 || ch == 10 || ch == 13) {
				teletype(ch);
				continue;
			}
			get_cursor(&rr, &cc);
			put(rr, cc, ch, at);
			if (++cc >= COLS) {
				cc = 0;
				if (++rr >= ROWS) {
					scroll(1, 1, at, 0, 0, ROWS - 1, COLS - 1);
					rr = ROWS - 1;
				}
			}
			set_cursor(rr, cc);
		}
		if (!(AL(f) & 1))
			set_cursor(r, c);
		break;
	}
	case 0x1A:				/* display combination: VGA colour */
		if (AL(f) == 0) {
			set_al(f, 0x1A);
			set_bx(f, 0x0008);
		}
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- disk */

static UINT8 *bounce;			/* 64 KiB: one INT 13h transfer at most */

static int
rw_blocks(int write, UINT64 lba, unsigned n, void *buf)
{
	EFI_BLOCK_IO_PROTOCOL *b = boot_bio;
	if (!n)
		return 0;
	if (lba + n > b->Media->LastBlock + 1)
		return 0x04;			/* sector not found */
	EFI_STATUS s;
	if (write) {
		memcpy(bounce, buf, n * 512u);
		s = b->WriteBlocks(b, b->Media->MediaId, lba, n * 512u, bounce);
	} else {
		s = b->ReadBlocks(b, b->Media->MediaId, lba, n * 512u, bounce);
		if (!EFI_ERROR(s))
			memcpy(buf, bounce, n * 512u);
	}
	if (EFI_ERROR(s))
		DEBUG("CSM: INT 13h %s LBA %lu+%u: %r\n", write ? "write" : "read", lba, n, s);
	if (s == EFI_WRITE_PROTECTED)
		return 0x03;
	if (s == EFI_NO_MEDIA)
		return 0x80;
	return EFI_ERROR(s) ? 0x20 : 0;		/* controller failure */
}

static void
disk_status(struct rm_frame *f, int st)
{
	set_ah(f, (UINT8)st);
	cf(f, st != 0);
	if (DL(f) & 0x80)
		*BDA(0x74) = (UINT8)st;
	else
		*BDA(0x41) = (UINT8)st;
}

static void
int13(struct rm_frame *f)
{
	UINT8 fn = AH(f);
	if (DL(f) != boot_drive) {
		disk_status(f, fn == 0x15 ? 0 : 0x01);
		if (fn == 0x15)
			set_ah(f, 0);		/* no such drive */
		if (fn == 0x08)
			cf(f, 1);
		return;
	}
	int floppy = boot_drive == 0;
	switch (fn) {
	case 0x00:				/* reset */
	case 0x04:				/* verify */
	case 0x0D:
	case 0x10:
	case 0x11:
		disk_status(f, 0);
		break;
	case 0x01:				/* last status */
		set_ah(f, floppy ? *BDA(0x41) : *BDA(0x74));
		cf(f, AH(f) != 0);
		break;
	case 0x02:
	case 0x03: {				/* read, write (CHS) */
		unsigned cyl = CH(f) | (CL(f) & 0xC0u) << 2, sec = CL(f) & 0x3F, head = DH(f), n = AL(f);
		if (!sec || sec > geo_spt || head >= geo_heads || n > 128) {
			DEBUG("CSM: INT 13h AH=%02x CHS %u/%u/%u n %u: bad\n", fn, cyl, head, sec, n);
			set_al(f, 0);
			disk_status(f, 0x01);
			break;
		}
		UINT64 lba = ((UINT64)cyl * geo_heads + head) * geo_spt + sec - 1;
		int st = rw_blocks(fn == 3, lba, n, lin(f->es, BX(f)));
		set_al(f, st ? 0 : (UINT8)n);
		disk_status(f, st);
		break;
	}
	case 0x08: {				/* drive parameters */
		unsigned mc = geo_cyl - 1;
		set_ch(f, (UINT8)mc);
		set_cl(f, (UINT8)(geo_spt | (mc >> 2 & 0xC0)));
		set_dh(f, (UINT8)(geo_heads - 1));
		set_dl(f, 1);
		if (floppy) {
			set_bl(f, geo_spt == 18 ? 4 : geo_spt == 36 ? 6 : geo_spt == 15 ? 2 : 3);
			f->es = 0xF000;
			f->edi = (f->edi & ~0xFFFFu) | csm16->vectors[0x1E];
		}
		disk_status(f, 0);
		break;
	}
	case 0x15:				/* drive type */
		if (floppy)
			set_ah(f, 0x01);	/* a diskette, no change line */
		else {
			UINT64 n = boot_bio->Media->LastBlock + 1;
			set_ah(f, 0x03);
			set_cx(f, (UINT16)(n >> 16));
			set_dx(f, (UINT16)n);
		}
		cf(f, 0);
		break;
	case 0x16:				/* media change */
		disk_status(f, 0);
		break;
	case 0x41:				/* EDD installation check */
		if (floppy || BX(f) != 0x55AA) {
			disk_status(f, 0x01);
			break;
		}
		set_bx(f, 0xAA55);
		set_ah(f, 0x30);		/* EDD 3.0 */
		set_cx(f, 0x0001);		/* the packet calls */
		cf(f, 0);
		break;
	case 0x42:
	case 0x43: {				/* extended read, write */
		if (floppy) {
			disk_status(f, 0x01);
			break;
		}
		volatile UINT8 *dap = lin(f->ds, f->esi & 0xFFFF);
		UINT16 n = (UINT16)(dap[2] | dap[3] << 8);
		UINT16 off = (UINT16)(dap[4] | dap[5] << 8), seg = (UINT16)(dap[6] | dap[7] << 8);
		UINT64 lba;
		memcpy(&lba, (const void *)(dap + 8), 8);
		int st = n > 127 ? 0x01 : rw_blocks(fn == 0x43, lba, n, lin(seg, off));
		if (st) {
			dap[2] = dap[3] = 0;
		}
		disk_status(f, st);
		break;
	}
	case 0x48: {				/* extended parameters */
		if (floppy) {
			disk_status(f, 0x01);
			break;
		}
		volatile UINT8 *p = lin(f->ds, f->esi & 0xFFFF);
		UINT64 n = boot_bio->Media->LastBlock + 1;
		UINT32 cyl = geo_cyl, heads = geo_heads, spt = geo_spt;
		p[0] = 0x1A;
		p[1] = 0;
		p[2] = 0x02;			/* CHS valid */
		p[3] = 0;
		memcpy((void *)(p + 4), &cyl, 4);
		memcpy((void *)(p + 8), &heads, 4);
		memcpy((void *)(p + 12), &spt, 4);
		memcpy((void *)(p + 16), &n, 8);
		p[24] = 0x00;
		p[25] = 0x02;			/* 512 bytes per sector */
		disk_status(f, 0);
		break;
	}
	default:
		DEBUG("CSM: INT 13h AH=%02x: not supported\n", fn);
		disk_status(f, 0x01);
	}
}

void
bios_load_boot_sector(void)
{
	rw_blocks(0, 0, 1, (void *)(uintptr_t)0x7C00);
}

/* ---------------------------------------------------------------- keyboard */

static UINT16 kq[16];
static unsigned kh, kn;

static const char sc_plain[] = "\0\0" "1234567890-=" "\b\t" "qwertyuiop[]" "\r\0" "asdfghjkl;'`" "\0\\" "zxcvbnm,./";
static const char sc_shift[] = "\0\0" "!@#$%^&*()_+" "\b\t" "QWERTYUIOP{}" "\r\0" "ASDFGHJKL:\"~" "\0|" "ZXCVBNM<>?";

static UINT8
scan_of(UINT8 a)
{
	if (a == ' ')
		return 0x39;
	if (a == 0x1B)
		return 0x01;
	if (a >= 1 && a <= 26 && a != 8 && a != 9 && a != 13)
		a = (UINT8)(a + 'a' - 1);	/* Ctrl+letter */
	for (unsigned i = 2; i < sizeof sc_plain - 1; i++)
		if (sc_plain[i] == (char)a || sc_shift[i] == (char)a)
			return (UINT8)i;
	return 0;
}

/* an EFI key as a BIOS keystroke (scan code << 8 | ASCII), 0 if none */
static UINT16
bios_key(const EFI_INPUT_KEY *k)
{
	static const UINT8 scan[] = {
		[SCAN_UP] = 0x48, [SCAN_DOWN] = 0x50, [SCAN_RIGHT] = 0x4D, [SCAN_LEFT] = 0x4B,
		[SCAN_HOME] = 0x47, [SCAN_END] = 0x4F, [SCAN_INSERT] = 0x52, [SCAN_DELETE] = 0x53,
		[SCAN_PAGE_UP] = 0x49, [SCAN_PAGE_DOWN] = 0x51, [SCAN_F1] = 0x3B, [SCAN_F2] = 0x3C,
		[SCAN_F3] = 0x3D, [SCAN_F4] = 0x3E, [SCAN_F5] = 0x3F, [SCAN_F6] = 0x40, [SCAN_F7] = 0x41,
		[SCAN_F8] = 0x42, [SCAN_F9] = 0x43, [SCAN_F10] = 0x44, [SCAN_F11] = 0x85, [SCAN_F12] = 0x86,
		[SCAN_ESC] = 0x01,
	};
	if (k->ScanCode == SCAN_ESC)
		return 0x011B;
	if (k->ScanCode) {
		if (k->ScanCode < sizeof scan && scan[k->ScanCode])
			return (UINT16)(scan[k->ScanCode] << 8);
		return 0;
	}
	UINT8 a = k->UnicodeChar < 0x80 ? (UINT8)k->UnicodeChar : '?';
	return (UINT16)(scan_of(a) << 8 | a);
}

static void
key_fill(void)
{
	EFI_INPUT_KEY k;
	while (kn < ARRAY_SIZE(kq) && gST->ConIn && gST->ConIn->ReadKeyStroke(gST->ConIn, &k) == EFI_SUCCESS) {
		UINT16 v = bios_key(&k);
		if (v)
			kq[(kh + kn++) % ARRAY_SIZE(kq)] = v;
	}
}

static void
int16(struct rm_frame *f)
{
	switch (AH(f)) {
	case 0x00:
	case 0x10:				/* wait for a key */
		key_fill();
		if (!kn) {
			csm16->retry = 1;	/* csm16.S lets interrupts in, then asks again */
			return;
		}
		set_ax(f, kq[kh]);
		kh = (kh + 1) % ARRAY_SIZE(kq);
		kn--;
		break;
	case 0x01:
	case 0x11:				/* is there one? */
		key_fill();
		zf(f, !kn);
		if (kn)
			set_ax(f, kq[kh]);
		break;
	case 0x02:
	case 0x12:				/* shift flags */
		set_al(f, *BDA(0x17));
		break;
	case 0x03:				/* typematic rate: accepted */
		break;
	case 0x05:				/* store a keystroke */
		if (kn < ARRAY_SIZE(kq)) {
			kq[(kh + kn++) % ARRAY_SIZE(kq)] = CX(f);
			set_al(f, 0);
		} else
			set_al(f, 1);
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- time */

static UINT8
cmos(UINT8 r)
{
	outb(RTC_PORT, r | 0x80);
	return inb(RTC_PORT + 1);
}

static void
int1a(struct rm_frame *f)
{
	switch (AH(f)) {
	case 0x00:				/* the tick count */
		set_cx(f, (UINT16)(BDA32(0x6C) >> 16));
		set_dx(f, (UINT16)BDA32(0x6C));
		set_al(f, *BDA(0x70));
		*BDA(0x70) = 0;
		cf(f, 0);
		break;
	case 0x01:
		BDA32(0x6C) = (UINT32)CX(f) << 16 | DX(f);
		*BDA(0x70) = 0;
		cf(f, 0);
		break;
	case 0x02:				/* RTC time, BCD (the RTC runs in BCD) */
		set_ch(f, cmos(4));
		set_cl(f, cmos(2));
		set_dh(f, cmos(0));
		set_dl(f, 0);
		cf(f, 0);
		break;
	case 0x04:				/* RTC date, BCD */
		set_ch(f, cmos(0x32));
		set_cl(f, cmos(9));
		set_dh(f, cmos(8));
		set_dl(f, cmos(7));
		cf(f, 0);
		break;
	default:
		cf(f, 1);
	}
}

/* ---------------------------------------------------------------- system */

#define E820_MAX 32
struct e820_entry {
	UINT64 base, len;
	UINT32 type;
};
static struct e820_entry e820[E820_MAX];
static unsigned ne820;

static void
e820_add(UINT64 base, UINT64 len, UINT32 type)
{
	if (ne820 && e820[ne820 - 1].type == type && e820[ne820 - 1].base + e820[ne820 - 1].len == base) {
		e820[ne820 - 1].len += len;
		return;
	}
	if (ne820 < E820_MAX)
		e820[ne820++] = (struct e820_entry){ base, len, type };
}

/* the E820 map: the UEFI memory map with everything the firmware holds
   (it keeps running under the BIOS) reserved */
static void
build_e820(void)
{
	UINTN sz = 0, key, dsz;
	UINT32 ver;
	gBS->GetMemoryMap(&sz, 0, &key, &dsz, &ver);
	sz += 8 * dsz;
	UINT8 *m = malloc(sz);
	if (!m || gBS->GetMemoryMap(&sz, (void *)m, &key, &dsz, &ver))
		return;
	e820_add(0, EBDA_SEG * 16, 1);
	e820_add(EBDA_SEG * 16, 0xA0000 - EBDA_SEG * 16, 2);
	e820_add(0xE0000, 0x20000, 2);
	for (UINTN o = 0; o < sz; o += dsz) {
		EFI_MEMORY_DESCRIPTOR *d = (void *)(m + o);
		UINT64 b = d->PhysicalStart, l = d->NumberOfPages * EFI_PAGE_SIZE;
		if (b < 0x100000)
			continue;
		UINT32 t = d->Type == EfiConventionalMemory ? 1 : d->Type == EfiACPIReclaimMemory ? 3 :
		    d->Type == EfiACPIMemoryNVS ? 4 : d->Type == EfiUnusableMemory ? 5 : 2;
		e820_add(b, l, t);
	}
	free(m);
}

static void
int15(struct rm_frame *f)
{
	cf(f, 0);
	switch (AH(f)) {
	case 0x24:				/* A20: always on */
		set_ah(f, 0);
		if (AL(f) == 2)
			set_al(f, 1);
		else if (AL(f) == 3)
			set_bx(f, 0x0003);
		return;
	case 0x4F:				/* keyboard intercept: none */
		cf(f, 1);
		return;
	case 0x86: {				/* wait CX:DX microseconds */
		udelay((UINT64)CX(f) << 16 | DX(f));
		set_ah(f, 0);
		return;
	}
	case 0x88: {				/* KiB of memory above 1 MiB */
		UINT64 kb = 0;
		for (unsigned i = 0; i < ne820; i++)
			if (e820[i].base == 0x100000 && e820[i].type == 1)
				kb = e820[i].len >> 10;
		set_ax(f, (UINT16)MIN(kb, 0xFFFF));
		return;
	}
	case 0xC0:				/* the configuration table */
		f->es = 0xF000;
		set_bx(f, csm16->config_table);
		set_ah(f, 0);
		return;
	case 0xE8:
		if (AL(f) == 0x01) {		/* E801h: 1-16 MiB in KiB, above in 64 KiB */
			UINT64 lo = 0, hi = 0;
			for (unsigned i = 0; i < ne820; i++) {
				if (e820[i].type != 1 || e820[i].base < 0x100000)
					continue;
				UINT64 b = e820[i].base, e = b + e820[i].len;
				if (b < 0x1000000)
					lo += MIN(e, 0x1000000) - b;
				if (e > 0x1000000 && b < (1ull << 32))
					hi += MIN(e, 1ull << 32) - MAX(b, 0x1000000);
			}
			set_ax(f, (UINT16)(lo >> 10));
			set_cx(f, (UINT16)(lo >> 10));
			set_bx(f, (UINT16)(hi >> 16));
			set_dx(f, (UINT16)(hi >> 16));
			return;
		}
		if (AL(f) == 0x20 && f->edx == 0x534D4150) {	/* E820h, "SMAP" */
			UINT32 i = f->ebx;
			if (i >= ne820 || (f->ecx & 0xFFFF) < 20) {
				cf(f, 1);
				return;
			}
			volatile UINT8 *p = lin(f->es, f->edi & 0xFFFF);
			memcpy((void *)p, &e820[i].base, 8);
			memcpy((void *)(p + 8), &e820[i].len, 8);
			memcpy((void *)(p + 16), &e820[i].type, 4);
			f->eax = 0x534D4150;
			f->ecx = 20;
			f->ebx = i + 1 < ne820 ? i + 1 : 0;
			return;
		}
		break;
	}
	set_ah(f, 0x86);			/* not supported */
	cf(f, 1);
}

static void
int14(struct rm_frame *f)
{
	if (DX(f) != 0) {			/* COM1 only */
		set_ah(f, 0x80);
		return;
	}
	UINT8 lsr = inb(COM1 + 5), msr = inb(COM1 + 6);
	switch (AH(f)) {
	case 0x00:				/* initialise: our UART is already set up */
	case 0x03:				/* status */
		set_ax(f, (UINT16)(lsr << 8 | msr));
		break;
	case 0x01:				/* send */
		com1_putc(AL(f));
		set_ah(f, inb(COM1 + 5));
		break;
	case 0x02: {				/* receive (no wait: time-out at once) */
		int c = com1_getc();
		if (c < 0)
			set_ah(f, 0x80);
		else {
			set_al(f, (UINT8)c);
			set_ah(f, inb(COM1 + 5) & 0x1E);
		}
		break;
	}
	}
}

static void
message(const char *s)
{
	for (; *s; s++)
		teletype((UINT8)*s);
}

static void
int19(struct rm_frame *f)
{
	/* the bootstrap loader: the boot sector again */
	bios_load_boot_sector();
	f->cs = 0;
	f->ip = 0x7C00;
	f->flags = 0x0202;
	set_dl(f, boot_drive);
}

void EFIAPI
csm_dispatch(UINT32 frame)
{
	struct rm_frame *f = (struct rm_frame *)(uintptr_t)frame;
	switch (f->vector) {
	case 0x10: int10(f); break;
	case 0x11: set_ax(f, BDA16(0x10)); break;
	case 0x12: set_ax(f, BDA16(0x13)); break;
	case 0x13: int13(f); break;
	case 0x14: int14(f); break;
	case 0x15: int15(f); break;
	case 0x16: int16(f); break;
	case 0x17: set_ah(f, 0x30); break;	/* printer: not ready (time-out, no paper) */
	case 0x18: {
		static int said;
		if (!said++)
			message("\r\nNo bootable device -- insert a boot disk and press a key\r\n");
		csm16->retry = 1;		/* stays here, with interrupts on in between */
		break;
	}
	case 0x19: int19(f); break;
	case 0x1A: int1a(f); break;
	}
}

void
bios_init(void)
{
	bounce = dma_alloc_pages(16);
	ne820 = 0;
	build_e820();
	video_clear();
	kh = kn = 0;
}
