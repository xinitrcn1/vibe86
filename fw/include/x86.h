/*
 * x86.h: port I/O, MSRs, CPUID, the TSC and the vibe86 board's addresses
 * for the firmware. The platform constants come from rtl/common/platform.toml
 * (generated vibe86_platform.h, D-013); the board's MMIO devices that the
 * description doesn't list yet are from docs/platform.md "Physical map".
 */
#ifndef X86_H
#define X86_H

#include <stdint.h>
#include "vibe86_platform.h"

static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p)); }
static inline void outw(uint16_t p, uint16_t v) { __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(p)); }
static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p) { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint16_t inw(uint16_t p) { uint16_t v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }

static inline uint64_t rdtsc(void)
{
	uint32_t lo, hi;
	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return (uint64_t)hi << 32 | lo;
}

static inline uint64_t rdmsr(uint32_t m)
{
	uint32_t lo, hi;
	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m));
	return (uint64_t)hi << 32 | lo;
}

static inline void wrmsr(uint32_t m, uint64_t v)
{
	__asm__ volatile("wrmsr" :: "c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4])
{
	__asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

static inline void cpu_halt(void) { __asm__ volatile("hlt"); }
static inline void cpu_cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void cpu_pause(void) { __asm__ volatile("pause"); }

static inline uint8_t mmio_r8(uint64_t a) { return *(volatile uint8_t *)a; }
static inline uint16_t mmio_r16(uint64_t a) { return *(volatile uint16_t *)a; }
static inline uint32_t mmio_r32(uint64_t a) { return *(volatile uint32_t *)a; }
static inline uint64_t mmio_r64(uint64_t a) { return *(volatile uint64_t *)a; }
static inline void mmio_w8(uint64_t a, uint8_t v) { *(volatile uint8_t *)a = v; }
static inline void mmio_w16(uint64_t a, uint16_t v) { *(volatile uint16_t *)a = v; }
static inline void mmio_w32(uint64_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void mmio_w64(uint64_t a, uint64_t v) { *(volatile uint64_t *)a = v; }

/* ---- the board (docs/platform.md) */
#define TSC_HZ		PLAT_CLOCK_TSC_HZ
#define COM1		((uint16_t)PLAT_PORTS_COM1)
#define DEBUGCON	((uint16_t)PLAT_PORTS_DEBUGCON)
#define POST_PORT	((uint16_t)PLAT_PORTS_POST)
#define FWCFG_PORT	((uint16_t)PLAT_PORTS_FWCFG)
#define FWCFG_DMA_PORT	((uint16_t)PLAT_PORTS_FWCFG_DMA)
#define PM1_PORT	((uint16_t)PLAT_PORTS_PM1)
#define RESET_PORT	((uint16_t)PLAT_PORTS_RESET)
#define RTC_PORT	((uint16_t)PLAT_PORTS_RTC)
#define PCI_CFG_PORT	((uint16_t)PLAT_PORTS_PCI_CFG)
#define ECAM_BASE	PLAT_MAP_ECAM_BASE
#define PCI_MEM32_BASE	PLAT_MAP_PCI_MEM32_BASE
#define PCI_MEM32_SIZE	PLAT_MAP_PCI_MEM32_SIZE
#define PCI_MEM64_BASE	PLAT_MAP_PCI_MEM64_BASE
#define PCI_MEM64_SIZE	PLAT_MAP_PCI_MEM64_SIZE
#define FLASH_BASE	PLAT_MAP_FLASH_BASE
#define FLASH_SIZE	PLAT_MAP_FLASH_SIZE
#define MMIO_BASE	PLAT_MAP_MMIO_BASE

#define SYSCON_BASE	0xFE001000ull	/* write 0x5555 off, 0x3333|code<<16 fail, 0x7777 reset */
#define I2C_BASE	0xFE008000ull
#define PMC_BASE	0xFE009000ull
#define NVRAM_BASE	0xFE00A000ull
#define NVRAM_SIZE	4096
#define VGA_REGS	0xFE010000ull
#define VGA_VRAM	0xF0000000ull
#define VGA_VRAM_SIZE	0x01000000ull

/* VGA registers (docs/platform.md "VGA") */
#define VGA_ID		0x00
#define VGA_MODE	0x08	/* 0 off, 1 text 80x25, 2 640x480, 3 800x600 */
#define VGA_WIDTH	0x10
#define VGA_HEIGHT	0x18
#define VGA_STRIDE	0x20
#define VGA_BASE	0x28
#define VGA_CURSOR	0x30
#define VGA_TTY		0x38
#define VGA_TTYATTR	0x40
#define VGA_CLEAR	0x48
#define VGA_FRAME	0x50
#define VGA_PALETTE	0x100

/* PCI config space through ECAM (PCIe Base 5.0 7.2.2) */
static inline uint64_t pci_ecam(unsigned bus, unsigned dev, unsigned fn, unsigned reg)
{
	return ECAM_BASE + ((uint64_t)bus << 20 | dev << 15 | fn << 12 | reg);
}
static inline uint32_t pci_r32(unsigned b, unsigned d, unsigned f, unsigned r) { return mmio_r32(pci_ecam(b, d, f, r)); }
static inline uint16_t pci_r16(unsigned b, unsigned d, unsigned f, unsigned r) { return mmio_r16(pci_ecam(b, d, f, r)); }
static inline uint8_t pci_r8(unsigned b, unsigned d, unsigned f, unsigned r) { return mmio_r8(pci_ecam(b, d, f, r)); }
static inline void pci_w32(unsigned b, unsigned d, unsigned f, unsigned r, uint32_t v) { mmio_w32(pci_ecam(b, d, f, r), v); }
static inline void pci_w16(unsigned b, unsigned d, unsigned f, unsigned r, uint16_t v) { mmio_w16(pci_ecam(b, d, f, r), v); }
static inline void pci_w8(unsigned b, unsigned d, unsigned f, unsigned r, uint8_t v) { mmio_w8(pci_ecam(b, d, f, r), v); }

static inline void post_code(uint8_t c) { outb(POST_PORT, c); }

#endif
