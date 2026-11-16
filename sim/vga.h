/* vsim basic on-board VGA: text 80x25 and linear 32-bpp framebuffers */
#ifndef VGA_H
#define VGA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VGA_REGS	0xFE010000ull	/* 4 KiB of registers */
#define VGA_VRAM	0xF0000000ull	/* VRAM window */
#define VGA_VRAM_SIZE	(16ull << 20)
#define VGA_FRAME_CYCLES 1000000ull	/* one frame (vblank) per this many cycles */
#define VGA_MAXW	800
#define VGA_MAXH	600

enum { VGA_OFF, VGA_TEXT, VGA_640, VGA_800 };

struct vga {
	uint8_t *vram;
	uint64_t mode, base, cursor, ttyattr, frame;
	uint32_t pal[16];
};

int vga_init(struct vga *v);
void vga_free(struct vga *v);
void vga_reset(struct vga *v);
void vga_tick(struct vga *v, uint64_t now);
int vga_reg(struct vga *v, int we, unsigned off, unsigned n, uint8_t *data);
int vga_mem(struct vga *v, int we, uint64_t off, unsigned n, uint8_t *data);
/* the visible picture: fills pix (VGA_MAXW * VGA_MAXH ARGB8888), sets w, h */
void vga_render(const struct vga *v, uint32_t *pix, int *w, int *h);
/* text mode cells as 25 lines of 80 characters: box drawing as + - |,
   blocks as #, arrows as ^ v < >, other non-printables as '.' */
void vga_text(const struct vga *v, char out[25][81]);

#ifdef __cplusplus
}
#endif

#endif
