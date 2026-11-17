# fw/ — the vibe86 platform firmware

A UEFI firmware for the vibe86 board in the style of an AMI Aptio image:
SEC → PEI → DXE → BDS, a POST screen, setup, a boot menu and a built-in EFI
shell. Its goal is to boot **CSMWrap** (`work/csmwrapx64.efi`, the user's
binary, git-ignored), which provides a legacy BIOS (CSM) so that **MikeOS**
(`work/mikeos.flp`, a 1.44 MB floppy image) boots from the **USB floppy
drive** behind the xHCI controller. `acpi/` holds the platform's ACPI tables
(the DSDT in ASL, `mktables.py` for the rest and QEMU's bios-linker-loader
script), which the firmware installs.

Everything is written from the specifications (UEFI 2.10, PI 1.8, xHCI
1.2, USB MSC BOT 1.0, HID 1.11, the FAT32 specification 1.03, ACPI 6.5,
SMBIOS 3.6, PCIe Base / PCI-to-PCI Bridge); the xHCI/HID driver and the
setup screen's layout are ported from the vercpu firmware
(`~/vercpu/fw/dxe`). No EDK2 code.

## Status

| # | Milestone | State | ctest |
|---|---|---|---|
| 1 | reset → banner on COM1 and the VGA console | works | `firmware/uefi-post` |
| 2 | PCI enumeration, xHCI, USB keyboard + mouse + floppy | works (keys drive the menus) | `firmware/uefi-usb` |
| 3 | system table, boot services, Block I/O and Simple File System on the USB floppy, GOP/console | works | `firmware/uefi-fs`, `firmware/uefi-setup` |
| 4 | CSMWrap loaded and run from the boot manager | runs until it needs a second CPU (below) | `firmware/uefi-csmwrap` (slow) |
| 5 | MikeOS boots from the USB floppy | **blocked by the platform** | — |

**Milestone 5 is blocked on SMP.** CSMWrap 2.0 and later (the user's
binary is 3.1.2) reserve one logical CPU as a "BIOS proxy" helper core,
started with INIT-SIPI-SIPI through the local APIC, and panic without one
(`bios_proxy.c`: "No AP available for BIOS proxy"; CSMWrap README, FAQ:
"does not support running on systems with only 1 logical processor"). v1
has one core and no LAPIC model, so the run stops after CSMWrap has
unlocked the legacy region through our Legacy Region 2 protocol and parsed
our ACPI tables with uACPI. This is the platform task on the roadmap
(README "SMP: two or more cores, LAPIC (xAPIC, timer, INIT/SIPI)");
CSMWrap 1.3.0, the last release without the proxy, would not need it.
Nothing is faked: the firmware reports the CPUs the MADT lists.

What CSMWrap does next with a second core (SeaBIOS's USB stack driving the
xHCI controller and the floppy through INT 13h) is untested here.

## Running

```
cmake --build build --target firmware          # build/fw/vibe86.fd (4 MiB)
build/sim/vsim -b test -M 128 -U work/mikeos.flp build/fw/vibe86.fd
```

The test board with at least 32 MiB (`-M`), since PEI uses its RAM from
reset (below). Useful options: `-g file` (the firmware's debug log on port
402h), `-N file` (keep the NVRAM: setup settings and variables), `-K` (keys
on the USB keyboard), `-Y`/`-V` (the screen at exit). A loader can be handed
to the firmware as a file system over fw_cfg:

```
build/sim/vsim -b test -M 128 -U work/mikeos.flp \
  -f opt/vibe86/fs/EFI/BOOT/BOOTX64.EFI=work/csmwrapx64.efi \
  -f opt/vibe86/fs/EFI/BOOT/csmwrap.ini=tests/firmware/csmwrap.ini build/fw/vibe86.fd
```

At the POST prompt: **DEL** or **F2** setup, **F11** or **F12** the boot
menu (also VT100+ `ESC 2` / `ESC @` etc. on the serial console). The
prompt waits 50 ms by default (Boot → Setup Prompt Timeout, in 10 ms
units): the simulator runs at about 1/150 of real time, so a real-time
second would be minutes. Without a key the boot manager tries, in the order
set in Boot → Boot Option Priorities: UEFI removable media
(`\EFI\BOOT\BOOTX64.EFI` on any FAT volume), the fw_cfg volume, the
built-in EFI shell; then "Reboot and Select proper Boot device".

The shell has `ls cd cat map memmap pci devices dmem gop date time echo
cls ver reset exit help`, runs `.efi` images and `startup.nsh`.

## How it is built

`fw/CMakeLists.txt` compiles C for `x86_64-unknown-windows` (PE32+ and the
Microsoft x64 convention that UEFI 2.10 2.3.4 prescribes) with
`-mgeneral-regs-only` (v1 has no SSE or x87 yet), `-nostdinc` with the
firmware's own `include/std`, and links with lld-link; SEC is assembled and
linked flat with ld.lld. `tools/mkfd.py` packs the flash image:

| Address | Size | |
|---|---|---|
| `FFC0_0000` | 3 MiB | FV_MAIN, a PI firmware volume (FFS2): the DXE core, the drivers in dispatch order, the shell application, the ACPI files as raw files |
| `FFF0_0000` | ≤ 960 KiB | PEI, a PE32+ linked at this address and run in place (mkfd checks it has no writable sections) |
| `FFFF_0000` | 64 KiB | SEC, the reset vector at `FFFF_FFF0` |

`include/layout.h` holds these numbers for C, assembly and mkfd.

## The phases

**SEC** (`sec/sec.S`): reset in real mode → protected mode → IA-32e mode
with 4 GiB identity-mapped by 1-GiB pages in temporary RAM
(`1_0000h`–`8_0000h`), GDT in flash with the accessed bits preset; calls
PEI with a stack in temporary RAM.

**PEI** (`pei/pei.c`): runs from flash with no writable data. Memory comes
from fw_cfg `etc/e820` (CMOS 30h/31h, 34h/35h as a fallback); the firmware
never trains DRAM (on this platform an init engine does that before reset).
It takes the top 8 MiB of RAM below 4 GiB as the hand-off range, builds the
HOB list (PHIT, CPU, resource descriptors for RAM, flash and the MMIO
windows, the FV, memory allocations, a vibe86 platform-information GUID
HOB), page tables identity-mapping 2.5 TiB with 1-GiB pages (the 64-bit PCI
window is at 2 TiB) and the DXE stack, loads and relocates the DXE core
and enters it.

**DXE core** (`dxe/core`): the UEFI boot services — memory map and pools
(top-down allocation below 4 GiB, so low memory stays free for legacy
users), the protocol database with notifications and open-protocol
records, events/timers/TPLs, the PE32+ image services (buffers, `Fv()/
FvFile()` paths, files on a Simple File System; `Exit` through a
setjmp-style context), ExitBootServices, the system table and
configuration tables, an IDT that prints exceptions with the image the RIP
is in, a GDT in runtime memory. It dispatches FV_MAIN's drivers in volume
order and calls the BDS architectural protocol.

There is no timer interrupt: the firmware runs with interrupts disabled
and checks timers against the TSC where UEFI code waits (WaitForEvent,
CheckEvent, RestoreTPL). Drivers connect their devices when dispatched,
so ConnectController/DisconnectController only validate their arguments.

**Drivers** (`dxe/*`, in dispatch order):

| Driver | Produces |
|---|---|
| RuntimeDxe (runtime) | GetTime/SetTime (MC146818 RTC), ResetSystem (CF9h, PM1a S5), variables (non-volatile ones in the board's NVRAM) |
| SerialDxe | Serial I/O on COM1 |
| VideoDxe | GOP (640×480, 800×600) and Simple Text Output on the display; text mode until a GOP SetMode, QueryMode says EFI_NOT_STARTED before that (UEFI 2.10 12.9) |
| ConsoleDxe | a VT100 terminal on Serial I/O; the ConIn/ConOut/StdErr splitters |
| PciDxe | bus numbers, BARs, bridge windows; PCI I/O. Enabling memory or bus mastering on a function enables it on every bridge above it: the root ports enforce BME |
| UsbDxe | xHCI; HID keyboards (Simple Text Input), mice (Simple Pointer), Bulk-Only mass storage (Block I/O); halts the controller at ExitBootServices |
| FatDxe | Simple File System on FAT12/16/32, whole devices and MBR partitions (read-only) |
| FwCfgFsDxe | a read-only Simple File System over fw_cfg files named `opt/vibe86/fs/...` |
| PlatformDxe | 8259 and PIIX4 PM setup, ACPI tables (QEMU table-loader from fw_cfg, else from FV_MAIN), SMBIOS 3.6, Legacy Region 2 on the i440FX PAM registers |
| BdsDxe | POST screen, setup, boot menu, boot manager |

**Shell** (`apps/shell`): an application in FV_MAIN, the boot option
"UEFI: Built-in EFI Shell".

## Limits

- The test board only: SEC and PEI use RAM from reset, and the server
  board's DRAM sits above 4 GiB with nothing below; when the memory init
  engine's hand-off is documented, PEI will take the map from it.
- Read-only FAT; no SetVirtualAddressMap (EFI_UNSUPPORTED); no Secure Boot,
  no TPM measurements; one xHCI controller; no USB hubs; no SMM set-up.
- The IOMMU stays in bypass (PCI I/O Map is 1:1).

## Platform findings (to fix in the platform, not here)

- **SMP and the local APIC** (above): CSMWrap needs an AP.
- Without a LAPIC model, CSMWrap's xAPIC programming (TPR, LVT masks,
  LINT0 ExtINT, LINT1 NMI, SVR = 10Fh) lands in the MSI window as 4-byte
  writes with vectors below 16, which vsim drops; harmless by accident.
- The MADT lists an I/O APIC at `FEC0_0000h` that nothing decodes (a bus
  error if CSMWrap's MP table code or an OS touches it).
- The legacy VGA window `A_0000h`–`B_FFFFh` reads as all ones: SeaVGABIOS
  in CSMWrap draws text into the GOP frame buffer instead, so programs
  that write `B_8000h` directly won't show.
- `docs/platform.md` doesn't describe the PC compatibility layer yet
  (root ports at 00:10.0–00:17.0, `-M`, PAM, SMRAM, fw_cfg, the CMOS sizes).

## Tests

`tests/firmware/uefi-*.test` run the flash image with `tests/run.py`
(`expect-out` on COM1, `expect-vga` on the text screen at exit, `requires`
skips a test when `work/` lacks the user's binaries; `.nsh` files are
startup scripts served through the fw_cfg volume):

```
ctest --test-dir build -L firmware -LE slow     # ~6 minutes
ctest --test-dir build -R uefi-csmwrap          # ~6 minutes
```
