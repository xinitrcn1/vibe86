/*
 * layout.h: where the firmware's pieces sit in the flash image and in
 * memory during SEC and PEI. Included by C, assembly and (parsed by)
 * fw/tools/mkfd.py, so keep it to plain #defines of numbers.
 *
 * The flash device image (FD) is 4 MiB at the top of the 16 MiB EEPROM:
 *
 *   FFC0_0000  FV_MAIN   firmware volume: DXE core, DXE drivers, data (3 MiB)
 *   FFF0_0000  PEI       the PEI image, a fixed-address PE32+ run in place
 *   FFFF_0000  SEC       reset vector code (the last 64 KiB)
 */
#ifndef LAYOUT_H
#define LAYOUT_H

#define FD_SIZE		0x00400000
#define FD_BASE		0xFFC00000
#define FV_MAIN_BASE	0xFFC00000
#define FV_MAIN_SIZE	0x00300000
#define PEI_BASE	0xFFF00000
#define PEI_SIZE	0x000F0000
#define SEC_BASE	0xFFFF0000
#define SEC_SIZE	0x00010000

/*
 * Temporary RAM for SEC and PEI: on this platform DRAM works from reset
 * (docs/platform.md; the on-die init engine trains it before the core runs,
 * fw/README.md "Memory"), so SEC uses ordinary low memory as an x86 SEC
 * would use cache-as-RAM. PEI moves everything it hands to DXE to permanent
 * memory, and DXE reports this range as free.
 */
#define TEMP_RAM_BASE	0x00010000
#define TEMP_RAM_SIZE	0x00070000	/* up to 8_0000h; SEC's page tables first */
#define SEC_PML4	0x00010000
#define SEC_PDPT	0x00011000
#define TEMP_STACK_TOP	0x00080000

/* selectors of the GDTs SEC and DXE use (the same layout) */
#define SEL_CODE32	0x08
#define SEL_DATA	0x10
#define SEL_CODE64	0x18

#endif
