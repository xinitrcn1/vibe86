/* vsim basic on-board VGA (docs/platform.md, "VGA") */
#include <stdlib.h>
#include <string.h>

#include "vgafont.h"

#include "vga.h"

/* the 16 CGA colours */
static const uint32_t cga[16] = {
	0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
	0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

int
vga_init(struct vga *v)
{
	memset(v, 0, sizeof *v);
	if (!(v->vram = calloc(1, VGA_VRAM_SIZE)))
		return -1;
	vga_reset(v);
	return 0;
}

void
vga_free(struct vga *v)
{
	free(v->vram);
	v->vram = NULL;
}

void
vga_reset(struct vga *v)
{
	v->mode = VGA_OFF;
	v->base = 0;
	v->cursor = 0;
	v->ttyattr = 0x07;
	v->frame = 0;
	memcpy(v->pal, cga, sizeof cga);
}

void
vga_tick(struct vga *v, uint64_t now)
{
	v->frame = now / VGA_FRAME_CYCLES;
}

static void
geom(const struct vga *v, int *w, int *h, int *stride)
{
	switch (v->mode) {
	case VGA_TEXT: *w = 720; *h = 400; *stride = 160; break;	/* 80x25 cells, 9x16 dots */
	case VGA_640: *w = 640; *h = 480; *stride = 640 * 4; break;
	case VGA_800: *w = 800; *h = 600; *stride = 800 * 4; break;
	default: *w = 720; *h = 400; *stride = 0; break;
	}
}

static uint8_t *
cell(struct vga *v, unsigned col, unsigned row)
{
	return v->vram + ((v->base + (uint64_t)row * 160 + col * 2) & (VGA_VRAM_SIZE - 1));
}

static void
clear_text(struct vga *v, uint8_t attr)
{
	for (unsigned r = 0; r < 25; r++)
		for (unsigned c = 0; c < 80; c++) {
			uint8_t *p = cell(v, c, r);
			p[0] = ' ';
			p[1] = attr;
		}
}

/* teletype output at the cursor, with \r \n \b and scrolling */
static void
tty(struct vga *v, uint8_t ch)
{
	unsigned col = v->cursor & 0xFF, row = (v->cursor >> 8) & 0xFF;

	if (col > 79) col = 79;
	if (row > 24) row = 24;
	switch (ch) {
	case '\r': col = 0; break;
	case '\n': row++; break;
	case '\b': if (col) col--; break;
	default:
		if (ch < 0x20)
			break;
		{
			uint8_t *p = cell(v, col, row);
			p[0] = ch;
			p[1] = (uint8_t)v->ttyattr;
		}
		if (++col == 80) {
			col = 0;
			row++;
		}
		break;
	}
	if (row == 25) {
		for (unsigned r = 0; r < 24; r++)
			for (unsigned c = 0; c < 80; c++)
				memcpy(cell(v, c, r), cell(v, c, r + 1), 2);
		for (unsigned c = 0; c < 80; c++) {
			uint8_t *p = cell(v, c, 24);
			p[0] = ' ';
			p[1] = (uint8_t)v->ttyattr;
		}
		row = 24;
	}
	v->cursor = (v->cursor & ~0xFFFFull) | (uint64_t)row << 8 | col;
}

int
vga_reg(struct vga *v, int we, unsigned off, unsigned n, uint8_t *data)
{
	uint64_t x = 0;
	int w, h, stride;

	if (off >= 0x100 && off < 0x140) {	/* palette, 4 bytes per entry */
		if ((n != 4 && n != 8) || (off & (n - 1)))
			return -1;
		uint8_t *p = (uint8_t *)v->pal + (off - 0x100);
		if (we) {
			memcpy(p, data, n);
			for (int i = 0; i < 16; i++)
				v->pal[i] &= 0xFFFFFF;
		} else
			memcpy(data, p, n);
		return 0;
	}
	if ((n != 4 && n != 8) || (off & 7))
		return -1;
	if (we) {
		memcpy(&x, data, n);
		switch (off) {
		case 0x08: v->mode = x <= VGA_800 ? x : VGA_OFF; break;
		case 0x28: v->base = x & (VGA_VRAM_SIZE - 1) & ~15ull; break;
		case 0x30: v->cursor = x & 0x1FFFF; break;
		case 0x38: tty(v, (uint8_t)x); break;
		case 0x40: v->ttyattr = x & 0xFF; break;
		case 0x48:
			geom(v, &w, &h, &stride);
			if (v->mode == VGA_TEXT)
				clear_text(v, (uint8_t)x);
			else if (v->mode != VGA_OFF)
				for (int i = 0; i < w * h; i++)
					memcpy(v->vram + ((v->base + (uint64_t)i * 4) & (VGA_VRAM_SIZE - 1)), &x, 4);
			v->cursor &= ~0xFFFFull;
			break;
		default: break;		/* read-only registers ignore writes */
		}
		return 0;
	}
	geom(v, &w, &h, &stride);
	switch (off) {
	case 0x00: x = 0x30414756; break;	/* "VGA0" */
	case 0x08: x = v->mode; break;
	case 0x10: x = (uint64_t)w; break;
	case 0x18: x = (uint64_t)h; break;
	case 0x20: x = (uint64_t)stride; break;
	case 0x28: x = v->base; break;
	case 0x30: x = v->cursor; break;
	case 0x40: x = v->ttyattr; break;
	case 0x50: x = v->frame; break;
	case 0x58: x = VGA_VRAM_SIZE; break;
	default: x = 0; break;
	}
	memset(data, 0, n);
	memcpy(data, &x, n);
	return 0;
}

int
vga_mem(struct vga *v, int we, uint64_t off, unsigned n, uint8_t *data)
{
	if (off + n > VGA_VRAM_SIZE)
		return -1;
	if (we)
		memcpy(v->vram + off, data, n);
	else
		memcpy(data, v->vram + off, n);
	return 0;
}

void
vga_render(const struct vga *v, uint32_t *pix, int *w, int *h)
{
	int stride;

	geom(v, w, h, &stride);
	if (v->mode == VGA_OFF) {
		memset(pix, 0, sizeof *pix * (size_t)*w * (size_t)*h);
		return;
	}
	if (v->mode == VGA_TEXT) {
		unsigned ccol = v->cursor & 0xFF, crow = (v->cursor >> 8) & 0xFF;
		int cvis = (v->cursor >> 16) & 1;
		for (int r = 0; r < 25; r++)
			for (int c = 0; c < 80; c++) {
				const uint8_t *p = v->vram + ((v->base + (uint64_t)r * 160 + c * 2) &
				    (VGA_VRAM_SIZE - 1));
				uint8_t ch = p[0], at = p[1];
				uint32_t fg = 0xFF000000u | v->pal[at & 15];
				uint32_t bg = 0xFF000000u | v->pal[at >> 4];
				/* the 8-dot font in 9-dot cells: the hardware's ninth dot
				   repeats the eighth for the line-drawing characters
				   0xC0-0xDF (line graphics enable) and is blank otherwise */
				int lge = ch >= 0xC0 && ch <= 0xDF;
				for (int y = 0; y < 16; y++) {
					unsigned bits = vgafont16[ch][y];
					if (cvis && (unsigned)c == ccol && (unsigned)r == crow && y >= 14)
						bits = 0xFF;	/* underline cursor, scan lines 14-15 */
					bits = bits << 1 | (lge ? bits & 1 : 0);
					uint32_t *row = pix + (r * 16 + y) * *w + c * 9;
					for (int x = 0; x < 9; x++)
						row[x] = (bits & (0x100 >> x)) ? fg : bg;
				}
			}
		return;
	}
	for (int y = 0; y < *h; y++)
		for (int x = 0; x < *w; x++) {
			uint32_t px;
			memcpy(&px, v->vram + ((v->base + (uint64_t)y * stride + (uint64_t)x * 4) &
			    (VGA_VRAM_SIZE - 1)), 4);
			pix[y * *w + x] = 0xFF000000u | (px & 0xFFFFFF);
		}
}

/* the -Y dump: an ASCII approximation of a code page 437 cell */
static char
cp437_ascii(uint8_t ch)
{
	if (ch >= 0x20 && ch < 0x7F)
		return (char)ch;
	if (ch == 0)
		return ' ';
	if (ch == 0xB3 || ch == 0xBA)
		return '|';
	if (ch == 0xC4 || ch == 0xCD)
		return '-';
	if (ch >= 0xB4 && ch <= 0xDA)
		return '+';
	if ((ch >= 0xB0 && ch <= 0xB2) || (ch >= 0xDB && ch <= 0xDF) || ch == 0xFE)
		return '#';
	switch (ch) {
	case 0x10: case 0x1A: return '>';
	case 0x11: case 0x1B: return '<';
	case 0x18: case 0x1E: return '^';
	case 0x19: case 0x1F: return 'v';
	case 0xF8: return 'o';
	case 0xFA: return '.';
	}
	return '.';
}

void
vga_text(const struct vga *v, char out[25][81])
{
	for (int r = 0; r < 25; r++) {
		for (int c = 0; c < 80; c++) {
			uint8_t ch = v->vram[(v->base + (uint64_t)r * 160 + c * 2) & (VGA_VRAM_SIZE - 1)];
			out[r][c] = cp437_ascii(ch);
		}
		out[r][80] = 0;
	}
}
