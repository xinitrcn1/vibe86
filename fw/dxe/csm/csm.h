/* CsmDxe's internal interfaces (csm.c: loading and booting; bios.c: the services) */
#ifndef CSM_H
#define CSM_H

#include "uefilib.h"
#include "x86.h"
#include "vibe86csm.h"

/* fw/csm/csm16.S's header at F000:0000 */
#pragma pack(push, 1)
struct csm16_header {
	char sig[8];			/* "VIBE86CS" */
	UINT16 vectors[32];		/* INT 00h-1Fh handler offsets, 0 = default */
	UINT16 irq_slave;		/* INT 70h-77h */
	UINT16 iret_only;		/* every other vector */
	UINT16 boot64;			/* 64-bit entry of the legacy boot */
	UINT16 config_table;		/* INT 15h AH=C0h */
	UINT64 cr3;
	UINT64 dispatch;
	UINT64 rsp64;
	UINT8 idtr64[16];
	UINT8 retry;
	UINT8 boot_dl;
	UINT8 boot_flag;
	UINT8 pad;
	UINT16 save_sp, save_ss;
	UINT32 frame_lin;
	UINT32 pad2;
};

/* the caller's registers as the thunk saved them */
struct rm_frame {
	UINT32 edi, esi, ebp, esp, ebx, edx, ecx, eax;
	UINT16 gs, fs, es, ds;
	UINT16 vector;
	UINT16 ip, cs, flags;
};
#pragma pack(pop)
_Static_assert(sizeof(struct csm16_header) == 0x88, "csm16.S header layout");

#define CSM_BASE	0xF0000u
#define CSM_SIZE	0x10000u
#define EBDA_SEG	0x9FC0u
#define BDA(off)	((volatile UINT8 *)(uintptr_t)(0x400 + (off)))
#define BDA16(off)	(*(volatile UINT16 *)(uintptr_t)(0x400 + (off)))
#define BDA32(off)	(*(volatile UINT32 *)(uintptr_t)(0x400 + (off)))

#define F_CF	0x0001
#define F_ZF	0x0040
#define F_IF	0x0200

/* register access in a frame */
#define AL(f)	((UINT8)(f)->eax)
#define AH(f)	((UINT8)((f)->eax >> 8))
#define AX(f)	((UINT16)(f)->eax)
#define BL(f)	((UINT8)(f)->ebx)
#define BH(f)	((UINT8)((f)->ebx >> 8))
#define BX(f)	((UINT16)(f)->ebx)
#define CL(f)	((UINT8)(f)->ecx)
#define CH(f)	((UINT8)((f)->ecx >> 8))
#define CX(f)	((UINT16)(f)->ecx)
#define DL(f)	((UINT8)(f)->edx)
#define DH(f)	((UINT8)((f)->edx >> 8))
#define DX(f)	((UINT16)(f)->edx)
static inline void set_al(struct rm_frame *f, UINT8 v) { f->eax = (f->eax & ~0xFFu) | v; }
static inline void set_ah(struct rm_frame *f, UINT8 v) { f->eax = (f->eax & ~0xFF00u) | (UINT32)v << 8; }
static inline void set_ax(struct rm_frame *f, UINT16 v) { f->eax = (f->eax & ~0xFFFFu) | v; }
static inline void set_bx(struct rm_frame *f, UINT16 v) { f->ebx = (f->ebx & ~0xFFFFu) | v; }
static inline void set_cx(struct rm_frame *f, UINT16 v) { f->ecx = (f->ecx & ~0xFFFFu) | v; }
static inline void set_dx(struct rm_frame *f, UINT16 v) { f->edx = (f->edx & ~0xFFFFu) | v; }
static inline void set_bl(struct rm_frame *f, UINT8 v) { f->ebx = (f->ebx & ~0xFFu) | v; }
static inline void set_dl(struct rm_frame *f, UINT8 v) { f->edx = (f->edx & ~0xFFu) | v; }
static inline void set_dh(struct rm_frame *f, UINT8 v) { f->edx = (f->edx & ~0xFF00u) | (UINT32)v << 8; }
static inline void set_ch(struct rm_frame *f, UINT8 v) { f->ecx = (f->ecx & ~0xFF00u) | (UINT32)v << 8; }
static inline void set_cl(struct rm_frame *f, UINT8 v) { f->ecx = (f->ecx & ~0xFFu) | v; }
static inline void cf(struct rm_frame *f, int on) { f->flags = (UINT16)(on ? f->flags | F_CF : f->flags & ~F_CF); }
static inline void zf(struct rm_frame *f, int on) { f->flags = (UINT16)(on ? f->flags | F_ZF : f->flags & ~F_ZF); }
static inline void *lin(UINT16 seg, UINT32 off) { return (void *)(uintptr_t)((UINT32)seg * 16 + off); }

/* csm.c */
extern struct csm16_header *csm16;
extern EFI_BLOCK_IO_PROTOCOL *boot_bio;
extern UINT8 boot_drive;		/* 00h floppy or 80h hard disk */
extern unsigned geo_cyl, geo_heads, geo_spt;

/* bios.c */
void bios_init(void);
void EFIAPI csm_dispatch(UINT32 frame);
void bios_load_boot_sector(void);	/* to 0:7C00 */

#endif
