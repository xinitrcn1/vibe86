/*
 * vsim: the I/O APIC on the I/O die (D-017): Intel 82093AA I/O Advanced
 * Programmable Interrupt Controller (datasheet 290566-001, 1996) with 24
 * redirection entries, IOREGSEL (00h) / IOWIN (10h) at
 * rtl/common/platform.toml's ioapic_base, version 20h with the EOI register
 * at 40h that the ICH-family I/O APICs added (a write of a vector clears
 * Remote IRR of the level-triggered entries with that vector: the die's
 * local APICs send it for level-triggered EOIs). Its interrupts are MSIs:
 * memory writes to FEEx_xxxxh in Intel SDM Vol. 3A 12.11's format, sent
 * into the die over the I/O link.
 */
#ifndef IOAPIC_H
#define IOAPIC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IOAPIC_PINS	24

struct ioapic {
	uint8_t sel;			/* IOREGSEL */
	uint8_t id;			/* ID[27:24] */
	uint64_t redir[IOAPIC_PINS];
	uint32_t level;			/* the input pins' levels */
	void (*msi)(void *ctx, uint64_t addr, uint32_t data);
	void *ctx;
	uint64_t sent;
};

void ioapic_reset(struct ioapic *a, uint8_t id);
/* an input pin (a GSI 0..23) changed level (1 = high) */
void ioapic_set(struct ioapic *a, unsigned pin, int level);
/* a register access at off (00h IOREGSEL, 10h IOWIN, 40h EOI), 4 bytes;
   0, or -1 for a bad size */
int ioapic_mmio(struct ioapic *a, int we, unsigned off, unsigned n, uint8_t *data);

#ifdef __cplusplus
}
#endif

#endif
