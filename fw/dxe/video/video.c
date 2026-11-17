/*
 * VideoDxe: the board's display controller (docs/platform.md "VGA": text
 * 80x25, 640x480 and 800x600 at 32 bpp, VRAM at F000_0000h) as
 *
 *  - a Graphics Output Protocol (UEFI 2.10 12.9) with two modes, 640x480
 *    and 800x600, PixelBlueGreenRedReserved8BitPerColor (the controller's
 *    0x00RRGGBB pixels). Until a SetMode the display stays in text mode and
 *    QueryMode answers EFI_NOT_STARTED, as 12.9 allows ("Video display is
 *    not initialized. Call SetMode()"), so the firmware's screens keep the
 *    fast text mode and a loader that wants pixels asks for them;
 *  - a Simple Text Output protocol (12.4), mode 0 = 80x25, drawing into the
 *    text cells, or, once the display is in a graphics mode, rendering the
 *    same 8x16 VGA font (sim/vgafont.h, SeaBIOS's public-domain vgafont16)
 *    into the frame buffer, centred.
 *
 * Unicode box drawing, block and arrow characters map to code page 437.
 */
#include "uefilib.h"
#include "x86.h"
#include "vgafont.h"

#define COLS		80
#define ROWS		25
#define VGA_GUID EFI_GUID_DEF(0x5c3a8e2d, 0x14f1, 0x4bb0, 0x8f, 0x3e, 0x76, 0x69, 0x62, 0x65, 0x56, 0x47)

static uint16_t cells[ROWS][COLS];	/* char | attr << 8, the shadow of the screen */
static int gfx;				/* the display is in a GOP mode */
static int started;
static uint32_t fb_w, fb_h, fb_x0, fb_y0;
static uint32_t palette[16];

static uint64_t
vreg(unsigned r)
{
	return mmio_r64(VGA_REGS + r);
}

static void
vset(unsigned r, uint64_t v)
{
	mmio_w64(VGA_REGS + r, v);
}

/* ---- Unicode to code page 437 */

static const struct {
	uint16_t u;
	uint8_t c;
} cp437[] = {
	{ 0x2500, 0xC4 }, { 0x2502, 0xB3 }, { 0x250C, 0xDA }, { 0x2510, 0xBF }, { 0x2514, 0xC0 },
	{ 0x2518, 0xD9 }, { 0x251C, 0xC3 }, { 0x2524, 0xB4 }, { 0x252C, 0xC2 }, { 0x2534, 0xC1 },
	{ 0x253C, 0xC5 }, { 0x2550, 0xCD }, { 0x2551, 0xBA }, { 0x2552, 0xD5 }, { 0x2553, 0xD6 },
	{ 0x2554, 0xC9 }, { 0x2555, 0xB8 }, { 0x2556, 0xB7 }, { 0x2557, 0xBB }, { 0x2558, 0xD4 },
	{ 0x2559, 0xD3 }, { 0x255A, 0xC8 }, { 0x255B, 0xBE }, { 0x255C, 0xBD }, { 0x255D, 0xBC },
	{ 0x255E, 0xC6 }, { 0x255F, 0xC7 }, { 0x2560, 0xCC }, { 0x2561, 0xB5 }, { 0x2562, 0xB6 },
	{ 0x2563, 0xB9 }, { 0x2564, 0xD1 }, { 0x2565, 0xD2 }, { 0x2566, 0xCB }, { 0x2567, 0xCF },
	{ 0x2568, 0xD0 }, { 0x2569, 0xCA }, { 0x256A, 0xD8 }, { 0x256B, 0xD7 }, { 0x256C, 0xCE },
	{ 0x2588, 0xDB }, { 0x2580, 0xDF }, { 0x2584, 0xDC }, { 0x2591, 0xB0 }, { 0x2592, 0xB1 },
	{ 0x2593, 0xB2 }, { 0x25B2, 0x1E }, { 0x25BC, 0x1F }, { 0x25BA, 0x10 }, { 0x25C4, 0x11 },
	{ 0x2191, 0x18 }, { 0x2193, 0x19 }, { 0x2192, 0x1A }, { 0x2190, 0x1B }, { 0x00B7, 0xFA },
	{ 0x00B0, 0xF8 }, { 0x00A9, 'c' }, { 0x00AE, 'R' }, { 0x2022, 0x07 },
};

static uint8_t
to_cp437(CHAR16 c)
{
	if (c >= 0x20 && c < 0x7F)
		return (uint8_t)c;
	for (unsigned i = 0; i < ARRAY_SIZE(cp437); i++)
		if (cp437[i].u == c)
			return cp437[i].c;
	return '?';
}

static int
glyph_known(CHAR16 c)
{
	if ((c >= 0x20 && c < 0x7F) || c == '\r' || c == '\n' || c == '\b')
		return 1;
	for (unsigned i = 0; i < ARRAY_SIZE(cp437); i++)
		if (cp437[i].u == c)
			return 1;
	return 0;
}

/* ---- drawing a cell */

static void
draw_cell(unsigned r, unsigned c)
{
	uint16_t v = cells[r][c];
	if (!gfx) {
		mmio_w16(VGA_VRAM + (uint64_t)r * 160 + c * 2, v);
		return;
	}
	const uint8_t *g = vgafont16[v & 0xFF];
	uint32_t fg = palette[(v >> 8) & 15], bg = palette[(v >> 12) & 15];
	volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(VGA_VRAM +
	    ((uint64_t)(fb_y0 + r * 16) * fb_w + fb_x0 + c * 8) * 4);
	for (unsigned y = 0; y < 16; y++, p += fb_w) {
		uint8_t bits = g[y];
		for (unsigned x = 0; x < 8; x++)
			p[x] = (bits & (0x80 >> x)) ? fg : bg;
	}
}

/* ---- Simple Text Output */

static SIMPLE_TEXT_OUTPUT_MODE sto_mode;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL sto;

static void
update_cursor(void)
{
	if (!gfx)
		vset(VGA_CURSOR, (uint64_t)sto_mode.CursorColumn | (uint64_t)sto_mode.CursorRow << 8 |
		    (sto_mode.CursorVisible ? 1u << 16 : 0));
}

static void
scroll(void)
{
	memmove(cells[0], cells[1], sizeof cells - sizeof cells[0]);
	uint16_t blank = (uint16_t)(' ' | (sto_mode.Attribute & 0x7F) << 8);
	for (unsigned c = 0; c < COLS; c++)
		cells[ROWS - 1][c] = blank;
	if (!gfx) {
		memcpy((void *)(uintptr_t)VGA_VRAM, cells, sizeof cells);
		return;
	}
	uint8_t *fb = (uint8_t *)(uintptr_t)VGA_VRAM;
	uint64_t line = (uint64_t)fb_w * 4;
	memmove(fb + fb_y0 * line, fb + (fb_y0 + 16) * line, (ROWS - 1) * 16 * line);
	for (unsigned c = 0; c < COLS; c++)
		draw_cell(ROWS - 1, c);
}

static EFI_STATUS EFIAPI
sto_output_string(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *s)
{
	EFI_STATUS st = EFI_SUCCESS;
	for (; *s; s++) {
		CHAR16 ch = *s;
		if (ch == '\r') {
			sto_mode.CursorColumn = 0;
		} else if (ch == '\n') {
			if (++sto_mode.CursorRow == ROWS) {
				scroll();
				sto_mode.CursorRow = ROWS - 1;
			}
		} else if (ch == '\b') {
			if (sto_mode.CursorColumn)
				sto_mode.CursorColumn--;
		} else {
			if (!glyph_known(ch))
				st = EFI_WARN_UNKNOWN_GLYPH;
			unsigned r = (unsigned)sto_mode.CursorRow, c = (unsigned)sto_mode.CursorColumn;
			cells[r][c] = (uint16_t)(to_cp437(ch) | (sto_mode.Attribute & 0x7F) << 8);
			draw_cell(r, c);
			if (++sto_mode.CursorColumn == COLS) {
				sto_mode.CursorColumn = 0;
				if (++sto_mode.CursorRow == ROWS) {
					scroll();
					sto_mode.CursorRow = ROWS - 1;
				}
			}
		}
	}
	update_cursor();
	return st;
}

static EFI_STATUS EFIAPI
sto_test_string(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *s)
{
	for (; *s; s++)
		if (!glyph_known(*s))
			return EFI_UNSUPPORTED;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
sto_query_mode(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Mode, UINTN *Cols, UINTN *Rows)
{
	if (Mode != 0)
		return EFI_UNSUPPORTED;
	*Cols = COLS;
	*Rows = ROWS;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
sto_clear_screen(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This)
{
	uint16_t blank = (uint16_t)(' ' | (sto_mode.Attribute & 0x7F) << 8);
	for (unsigned r = 0; r < ROWS; r++)
		for (unsigned c = 0; c < COLS; c++)
			cells[r][c] = blank;
	if (!gfx) {
		vset(VGA_CLEAR, (uint64_t)(sto_mode.Attribute & 0x7F));
	} else {
		vset(VGA_CLEAR, palette[(sto_mode.Attribute >> 4) & 7]);
	}
	sto_mode.CursorColumn = sto_mode.CursorRow = 0;
	update_cursor();
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
sto_set_mode(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Mode)
{
	if (Mode != 0)
		return EFI_UNSUPPORTED;
	return sto_clear_screen(This);
}

static EFI_STATUS EFIAPI
sto_set_attribute(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Attr)
{
	if (Attr & ~0x7Full)
		return EFI_UNSUPPORTED;
	sto_mode.Attribute = (INT32)Attr;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
sto_set_cursor_position(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Col, UINTN Row)
{
	if (Col >= COLS || Row >= ROWS)
		return EFI_UNSUPPORTED;
	sto_mode.CursorColumn = (INT32)Col;
	sto_mode.CursorRow = (INT32)Row;
	update_cursor();
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
sto_enable_cursor(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Visible)
{
	sto_mode.CursorVisible = Visible;
	update_cursor();
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
sto_reset(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Ext)
{
	sto_mode.Attribute = EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK);
	return sto_clear_screen(This);
}

/* ---- Graphics Output */

static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION modes[2] = {
	{ 0, 640, 480, PixelBlueGreenRedReserved8BitPerColor, { 0, 0, 0, 0 }, 640 },
	{ 0, 800, 600, PixelBlueGreenRedReserved8BitPerColor, { 0, 0, 0, 0 }, 800 },
};
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION cur_info;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE gop_mode;
static EFI_GRAPHICS_OUTPUT_PROTOCOL gop;

static EFI_STATUS EFIAPI
gop_query_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *This, UINT32 Mode, UINTN *SizeOfInfo,
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info)
{
	if (!SizeOfInfo || !Info || Mode >= ARRAY_SIZE(modes))
		return EFI_INVALID_PARAMETER;
	if (!started)
		return EFI_NOT_STARTED;
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *i = malloc(sizeof *i);
	if (!i)
		return EFI_OUT_OF_RESOURCES;
	*i = modes[Mode];
	*Info = i;
	*SizeOfInfo = sizeof *i;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
gop_set_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *This, UINT32 Mode)
{
	if (Mode >= ARRAY_SIZE(modes))
		return EFI_UNSUPPORTED;
	vset(VGA_MODE, Mode == 0 ? 2 : 3);
	vset(VGA_BASE, 0);
	vset(VGA_CLEAR, 0);
	started = 1;
	gfx = 1;
	fb_w = modes[Mode].HorizontalResolution;
	fb_h = modes[Mode].VerticalResolution;
	fb_x0 = (fb_w - COLS * 8) / 2;
	fb_y0 = (fb_h - ROWS * 16) / 2;
	cur_info = modes[Mode];
	gop_mode.Mode = Mode;
	gop_mode.FrameBufferSize = (UINTN)fb_w * fb_h * 4;
	/* the text console goes on in pixels: the cells are now blank */
	uint16_t blank = (uint16_t)' ' | (uint16_t)(EFI_LIGHTGRAY << 8);
	for (unsigned r = 0; r < ROWS; r++)
		for (unsigned c = 0; c < COLS; c++)
			cells[r][c] = blank;
	sto_mode.CursorColumn = sto_mode.CursorRow = 0;
	DEBUG("VideoDxe: mode %u: %ux%u\n", Mode, fb_w, fb_h);
	return EFI_SUCCESS;
}

static uint32_t
px_of(const EFI_GRAPHICS_OUTPUT_BLT_PIXEL *p)
{
	return (uint32_t)p->Red << 16 | (uint32_t)p->Green << 8 | p->Blue;
}

static EFI_STATUS EFIAPI
gop_blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *This, EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Buf,
    EFI_GRAPHICS_OUTPUT_BLT_OPERATION Op, UINTN SrcX, UINTN SrcY, UINTN DstX, UINTN DstY,
    UINTN W, UINTN H, UINTN Delta)
{
	if (!started)
		gop_set_mode(This, 0);
	if (Op >= EfiGraphicsOutputBltOperationMax || !W || !H)
		return EFI_INVALID_PARAMETER;
	if (!Delta)
		Delta = W * sizeof *Buf;
	volatile uint32_t *fb = (volatile uint32_t *)(uintptr_t)VGA_VRAM;
	switch (Op) {
	case EfiBltVideoFill:
		if (DstX + W > fb_w || DstY + H > fb_h)
			return EFI_INVALID_PARAMETER;
		for (UINTN y = 0; y < H; y++)
			for (UINTN x = 0; x < W; x++)
				fb[(DstY + y) * fb_w + DstX + x] = px_of(Buf);
		break;
	case EfiBltBufferToVideo:
		if (DstX + W > fb_w || DstY + H > fb_h)
			return EFI_INVALID_PARAMETER;
		for (UINTN y = 0; y < H; y++) {
			const EFI_GRAPHICS_OUTPUT_BLT_PIXEL *row = (const void *)((const UINT8 *)Buf +
			    (SrcY + y) * Delta);
			for (UINTN x = 0; x < W; x++)
				fb[(DstY + y) * fb_w + DstX + x] = px_of(&row[SrcX + x]);
		}
		break;
	case EfiBltVideoToBltBuffer:
		if (SrcX + W > fb_w || SrcY + H > fb_h)
			return EFI_INVALID_PARAMETER;
		for (UINTN y = 0; y < H; y++) {
			EFI_GRAPHICS_OUTPUT_BLT_PIXEL *row = (void *)((UINT8 *)Buf + (DstY + y) * Delta);
			for (UINTN x = 0; x < W; x++) {
				uint32_t v = fb[(SrcY + y) * fb_w + SrcX + x];
				row[DstX + x] = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL){ (UINT8)v, (UINT8)(v >> 8),
				    (UINT8)(v >> 16), 0 };
			}
		}
		break;
	case EfiBltVideoToVideo:
		if (SrcX + W > fb_w || SrcY + H > fb_h || DstX + W > fb_w || DstY + H > fb_h)
			return EFI_INVALID_PARAMETER;
		for (UINTN k = 0; k < H; k++) {
			UINTN y = DstY > SrcY ? H - 1 - k : k;
			memmove((void *)(uintptr_t)&fb[(DstY + y) * fb_w + DstX],
			    (void *)(uintptr_t)&fb[(SrcY + y) * fb_w + SrcX], W * 4);
		}
		break;
	default:
		return EFI_INVALID_PARAMETER;
	}
	return EFI_SUCCESS;
}

#pragma pack(push, 1)
static struct {
	VENDOR_DEVICE_PATH ven;
	EFI_DEVICE_PATH_PROTOCOL end;
} path;
#pragma pack(pop)

EFI_STATUS EFIAPI
efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
	uefi_init(image, st);
	if ((uint32_t)vreg(VGA_ID) != 0x30414756) {	/* "VGA0" */
		DEBUG("VideoDxe: no display controller\n");
		return EFI_NOT_FOUND;
	}
	for (unsigned i = 0; i < 16; i++)
		palette[i] = mmio_r32(VGA_REGS + VGA_PALETTE + 4 * i);
	vset(VGA_MODE, 1);
	vset(VGA_BASE, 0);
	sto_mode = (SIMPLE_TEXT_OUTPUT_MODE){ 1, 0, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK), 0, 0, TRUE };
	sto = (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL){ sto_reset, sto_output_string, sto_test_string,
	    sto_query_mode, sto_set_mode, sto_set_attribute, sto_clear_screen, sto_set_cursor_position,
	    sto_enable_cursor, &sto_mode };
	sto_clear_screen(&sto);

	cur_info = modes[0];
	gop_mode = (EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE){ ARRAY_SIZE(modes), 0, &cur_info, sizeof cur_info,
	    VGA_VRAM, 640 * 480 * 4 };
	gop = (EFI_GRAPHICS_OUTPUT_PROTOCOL){ gop_query_mode, gop_set_mode, gop_blt, &gop_mode };

	static const EFI_GUID vga_guid = VGA_GUID;
	dp_set_node(&path.ven.Header, HARDWARE_DEVICE_PATH, HW_VENDOR_DP, sizeof path.ven);
	path.ven.Guid = vga_guid;
	dp_set_end(&path.end);
	EFI_HANDLE h = 0;
	return gBS->InstallMultipleProtocolInterfaces(&h, &gEfiDevicePathProtocolGuid, &path,
	    &gEfiGraphicsOutputProtocolGuid, &gop, &gEfiSimpleTextOutProtocolGuid, &sto, (void *)0);
}
