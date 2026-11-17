# fw/ — vibe86 platform firmware (handoff)

This directory holds the platform's firmware data today (`acpi/`: the DSDT
in ASL and `mktables.py`, which builds the ACPI tables and QEMU's
bios-linker-loader script served over fw_cfg). The **firmware itself is
not written yet**: this file is the brief for the agent that writes it.

## The goal

A synthetic UEFI firmware for the vibe86 board, in the style of an AMI
Aptio image (SEC → PEI → DXE → BDS, setup screen, boot menu), that boots
**CSMWrap** (`work/csmwrapx64.efi`, the user's binary; git-ignored), which
provides a legacy BIOS (CSM) so that **MikeOS** (`work/mikeos.flp`, a
1.44 MB floppy image) boots **from a USB floppy drive** — a real-mode boot
through INT 13h on a USB mass-storage device behind the xHCI controller.

Milestones, each a ctest when it works:
1. reset → firmware banner on COM1 and the VGA console;
2. PCI enumeration, xHCI up, USB keyboard + mouse + floppy enumerated;
3. an EFI system table, boot services, the Block I/O and Simple File
   System protocols on the USB floppy, the GOP/console;
4. CSMWrap loaded and run from the EFI shell or boot manager;
5. MikeOS boots from the USB floppy (its menu on the screen; `-K` key
   scripts can drive it).

## What to borrow

`~/vercpu/fw` (the user's earlier firmware for a different ISA) has the
device-driver cruft in C under `dxe/`: `console.c` (VGA text + serial,
VT100 input), `usb.c`/`usbglue.c` (an xHCI driver with HID keyboard/mouse),
`pci.c`, `i2c.c`/`tpm.c`, `nvram.c`, `pmc.c`, `setup.c` (setup screen, F12),
`shell.c`, plus a boot-info hand-off. The board devices are the same models
(`sim/xhci.c`, `vga.c`, `i2c.c`, `tpm.c`, `pmc.c` were ported from it), so
the C drivers port with new MMIO bases (see below) and x86 port I/O where
the PC platform uses it. Its stage-1 assembly (`entry.s`, `dram.s`) is
vercpu ISA and does DRAM training: **don't port the training** — see
"Memory" below.

## The platform (read these first)

- `docs/platform.md` — the socket, boards, physical map, I/O ports, the PC
  compatibility layer (i440FX/PIIX identity, PAM, SMRAM, CF8/CFC, 8254,
  8259, RTC/CMOS, fw_cfg, ACPI PM, debugcon 402h).
- `rtl/common/platform.toml` — every address and port, generated into
  `build/rtl/common/vibe86_platform.h` (use that header from C: the
  firmware, the simulator and the RTL then agree by construction).
- `docs/decisions.md` — D-007 (the Venice ISA target), D-013 (rtl/common).
- Reset: the flash image's last byte is at `FFFF_FFFFh` (16 MiB EEPROM at
  `FF00_0000h`); the core starts in real mode at `F000:FFF0` (CS base
  `FFFF_0000h`), as every x86. The last 128 KiB of flash are also visible at
  `E_0000h`–`F_FFFFh` while the PAM registers say so.
- PCIe: ECAM at `E000_0000h`; root ports at 00:10.0–00:17.0, one per slot;
  xHCI in slot 1 by default (`vsim -X`). **The root ports enforce Bus
  Master Enable** (PCIe Base 7.5.1.1.3; the user chose strict behaviour):
  the firmware must set BME (and memory/IO enable) on each root port above
  a device that does DMA, or the xHCI halts with Host System Error on its
  first DMA. (This is exactly why QEMU's SeaBIOS can't use USB here.)
- xHCI ports: 1 keyboard, 2 mouse, 3 the USB floppy (`vsim -U image[,ro]`:
  Bulk-Only Transport, SCSI/UFI command set, 512-byte blocks, Flexible
  Disk mode page 05h with 80/2/18 geometry), 4 free. `VSIM_XHCI_TRACE=1`
  traces commands, control requests, SCSI commands and events.
- SMM exists (SMI# via APMC `B2h` when PIIX4 DEVACTB.APMC_EN is set; SMRAM
  at `A_0000h` under the i440FX SMRAM register; AMD64 save-state map;
  SMBASE resets to `3_0000h`); `tests/system/smm.S` shows the protocol.
- ACPI: `fw/acpi` builds FACS/FADT/MADT/MCFG/RSDT/XSDT/RSDP and the DSDT;
  `build/fw/acpi/{tables,rsdp,table-loader}` can be linked into the image
  or served by fw_cfg (`vsim -f name=file`).

## The CPU you're running on (limits that matter)

`docs/decisions.md` and `rtl/common/cpuid.toml` say what v1 implements; as
of this writing:

- **No x87 and no SSE/AVX yet** (being added). UEFI's x64 calling
  convention assumes SSE2; build the firmware with `-mgeneral-regs-only`
  (clang) / no floating point, and check what `csmwrapx64.efi` executes
  (`llvm-objdump -d`): if it uses SSE, CSMWrap waits for the vector work —
  report that rather than working around it.
- No local APIC / IOAPIC model yet (MADT describes them; interrupts reach
  the core from the 8259 or as MSIs). If CSMWrap or the firmware needs the
  LAPIC (timer, IPIs), that's a platform task to flag, not to fake.
- One core. Paging (4/5-level), long mode, SYSCALL, NX, SMEP/SMAP, PCID-
  less TLB, debug registers, SMM, RDRAND, FSGSBASE all work.
- `vsim -t` traces every instruction; `-g file` captures debugcon (402h);
  COM1 goes to stdout.

## Memory

The die is getting a real on-die DDR5 controller with pin-level DIMMs,
brought up by an on-die init engine that stands in for AMD's PSP/ABL
(the user's choice): **DRAM is trained before the x86 core leaves reset**,
as on EPYC. The firmware never trains memory; it reads the memory map
(e820-style) the platform reports (today: fw_cfg `etc/e820` and CMOS on
the test board; the hand-off from the init engine will be documented in
`docs/platform.md` when it lands). Until then, run on the test board
(`-b test -M 128`), whose RAM works from reset.

## Rules of the repository

- Cite papers/specs where a technique comes from (UEFI spec 2.10, PI spec
  1.8, ACPI 6.5, xHCI 1.2, USB MSC BOT 1.0, CSM spec, etc.) in comments,
  docs and commit messages.
- Commit in logical steps; messages end with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Generated files never go in git (build them under `build/`); `work/` is
  ignored and holds the user's binaries.
- Firmware build: CMake targets under `fw/` (clang + ld.lld, PE/COFF for
  EFI images via `-target x86_64-unknown-windows` / lld-link, or an ELF →
  flat flash image), and ctests under `tests/firmware/` using
  `tests/run.py`'s `expect-out`/`expect-exit`/`vsim-args`.
- Don't edit `rtl/` or `sim/` device semantics to make firmware work: if
  the platform is wrong or missing something, fix it properly (with a
  test) or report it.
