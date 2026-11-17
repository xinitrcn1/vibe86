#!/usr/bin/env python3
"""Build vibe86's ACPI tables, and QEMU's table-loader script for them.

    mktables.py dsdt.aml OUTDIR [--cpus N]

Writes OUTDIR/tables (the tables, concatenated), OUTDIR/rsdp and
OUTDIR/table-loader: what QEMU offers firmware as the fw_cfg files
"etc/acpi/tables", "etc/acpi/rsdp" and "etc/table-loader". Firmware that
speaks QEMU's BIOS linker/loader interface (SeaBIOS: src/fw/romfile_loader.c)
allocates the two blobs, patches the pointers between tables to where it put
them and fixes the checksums; the loader commands' format is QEMU's
hw/acpi/bios-linker-loader.c (128-byte entries: ALLOCATE, ADD_POINTER,
ADD_CHECKSUM).

Tables (ACPI Specification 6.5): RSDP (5.2.5), RSDT and XSDT (5.2.7,
5.2.8), FADT (5.2.9; revision 6, minor 5), FACS (5.2.10), DSDT (5.2.11,
compiled from dsdt.asl by iasl), MADT (5.2.12; local APICs, the IOAPIC, the
ISA overrides and LINT1 NMI), MCFG (PCI Firmware Specification 3.3, 4.1.2:
the ECAM). The fixed hardware they describe is the board's
(docs/platform.md): PM1a event/control/timer at 600h/604h/608h, the reset
register CF9h, SCI on IRQ 9, the IOAPIC at FEC00000h, the local APICs at
FEE00000h, the ECAM at E0000000h.
"""

import os
import struct
import sys

OEMID = b"VIBE86"
OEMTABLE = b"VIBE86V1"
OEMREV = 1
CREATOR = b"VB86"
CREATOR_REV = 1

PM1A_EVT = 0x600            # the defaults; --platform rtl/common/platform.toml sets them
PM1A_CNT = 0x604
PM_TMR = 0x608
SCI_IRQ = 9
IOAPIC_BASE = 0xFEC00000
LAPIC_BASE = 0xFEE00000
ECAM_BASE = 0xE0000000


def header(sig, length, rev):
    return struct.pack("<4sIBB6s8sI4sI", sig, length, rev, 0, OEMID, OEMTABLE, OEMREV,
                       CREATOR, CREATOR_REV)


def table(sig, rev, body):
    return header(sig, 36 + len(body), rev) + body


def gas(space, bits, offset, access, addr):
    """Generic Address Structure (5.2.3.2)."""
    return struct.pack("<BBBBQ", space, bits, offset, access, addr)


def facs():
    t = bytearray(64)
    t[0:4] = b"FACS"
    struct.pack_into("<I", t, 4, 64)
    t[32] = 2                               # version
    return bytes(t)


def fadt():
    b = bytearray(276 - 36)

    def put(off, fmt, *v):
        struct.pack_into(fmt, b, off - 36, *v)
    put(36, "<I", 0)                        # FIRMWARE_CTRL (patched)
    put(40, "<I", 0)                        # DSDT (patched)
    put(45, "<B", 4)                        # preferred PM profile: enterprise server
    put(46, "<H", SCI_IRQ)
    put(48, "<I", 0)                        # SMI_CMD: none, always in ACPI mode
    put(56, "<I", PM1A_EVT)
    put(64, "<I", PM1A_CNT)
    put(76, "<I", PM_TMR)
    put(88, "<B", 4)                        # PM1_EVT_LEN
    put(89, "<B", 2)                        # PM1_CNT_LEN
    put(91, "<B", 4)                        # PM_TMR_LEN
    put(96, "<H", 0x0FFF)                   # P_LVL2_LAT: C2 not supported
    put(98, "<H", 0x0FFF)                   # P_LVL3_LAT: C3 not supported
    put(108, "<B", 0x32)                    # CENTURY (CMOS index)
    # IAPC_BOOT_ARCH: legacy devices; no 8042; VGA not present (yet)
    put(109, "<H", (1 << 0) | (1 << 2))
    # flags: WBINVD, PROC_C1, PWR_BUTTON (none), SLP_BUTTON (none),
    # RESET_REG_SUP
    put(112, "<I", (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 10))
    b[116 - 36:128 - 36] = gas(1, 8, 0, 1, 0xCF9)     # RESET_REG: I/O CF9h
    put(128, "<B", 0x06)                    # RESET_VALUE: a hard reset
    put(131, "<B", 5)                       # FADT minor version
    put(132, "<Q", 0)                       # X_FIRMWARE_CTRL (patched)
    put(140, "<Q", 0)                       # X_DSDT (patched)
    b[148 - 36:160 - 36] = gas(1, 32, 0, 2, PM1A_EVT)
    b[172 - 36:184 - 36] = gas(1, 16, 0, 2, PM1A_CNT)
    b[208 - 36:220 - 36] = gas(1, 32, 0, 3, PM_TMR)
    return table(b"FACP", 6, bytes(b))


def madt(ncpus):
    body = struct.pack("<II", LAPIC_BASE, 1)            # PCAT_COMPAT: there are 8259s
    for i in range(ncpus):
        body += struct.pack("<BBBBI", 0, 8, i, i, 1)     # processor local APIC, enabled
    body += struct.pack("<BBBBII", 1, 12, ncpus, 0, IOAPIC_BASE, 0)   # IOAPIC, GSI base 0
    # ISA IRQ 0 is GSI 2 (the 8254 on the IOAPIC's pin 2); the SCI is level, active high
    body += struct.pack("<BBBBIH", 2, 10, 0, 0, 2, 0)
    body += struct.pack("<BBBBIH", 2, 10, 0, SCI_IRQ, SCI_IRQ, 0x000D)
    body += struct.pack("<BBBHB", 4, 6, 0xFF, 0, 1)      # NMI on LINT1, all processors
    return table(b"APIC", 5, body)


def mcfg():
    body = bytes(8) + struct.pack("<QHBBI", ECAM_BASE, 0, 0, 255, 0)
    return table(b"MCFG", 1, body)


# ---------------------------------------------------------------- the loader script

ALLOCATE, ADD_POINTER, ADD_CHECKSUM = 1, 2, 3
ZONE_HIGH, ZONE_FSEG = 1, 2
TABLES, RSDP = b"etc/acpi/tables", b"etc/acpi/rsdp"


def cmd_allocate(name, align, zone):
    return struct.pack("<I56sIB", ALLOCATE, name, align, zone).ljust(128, b"\0")


def cmd_pointer(dest, src, offset, size):
    return struct.pack("<I56s56sIB", ADD_POINTER, dest, src, offset, size).ljust(128, b"\0")


def cmd_checksum(name, offset, start, length):
    return struct.pack("<I56sIII", ADD_CHECKSUM, name, offset, start, length).ljust(128, b"\0")


def build(dsdt, ncpus):
    blob = bytearray()
    loader = [cmd_allocate(TABLES, 64, ZONE_HIGH), cmd_allocate(RSDP, 16, ZONE_FSEG)]
    pos = {}

    def add(name, data, align=8):
        while len(blob) % align:
            blob.append(0)
        pos[name] = len(blob)
        blob.extend(data)

    add("FACS", facs(), 64)
    add("DSDT", dsdt)
    add("FACP", fadt())
    add("APIC", madt(ncpus))
    add("MCFG", mcfg())
    entries = ["FACP", "APIC", "MCFG"]
    rsdt = table(b"RSDT", 1, bytes(4 * len(entries)))
    add("RSDT", rsdt)
    xsdt = table(b"XSDT", 1, bytes(8 * len(entries)))
    add("XSDT", xsdt)

    def ptr(table_name, field, size, target):
        off = pos[table_name] + field
        struct.pack_into("<Q" if size == 8 else "<I", blob, off, pos[target])
        loader.append(cmd_pointer(TABLES, TABLES, off, size))

    # pointers between the tables (offsets into the blob, made addresses by the loader)
    ptr("FACP", 36, 4, "FACS")
    ptr("FACP", 40, 4, "DSDT")
    ptr("FACP", 132, 8, "FACS")
    ptr("FACP", 140, 8, "DSDT")
    for i, e in enumerate(entries):
        ptr("RSDT", 36 + 4 * i, 4, e)
        ptr("XSDT", 36 + 8 * i, 8, e)
    # checksums, after the pointers they cover
    for name in ("DSDT", "FACP", "APIC", "MCFG", "RSDT", "XSDT"):
        length = struct.unpack_from("<I", blob, pos[name] + 4)[0]
        blob[pos[name] + 9] = 0
        loader.append(cmd_checksum(TABLES, pos[name] + 9, pos[name], length))

    # the RSDP (ACPI 2.0+: 36 bytes) in the F-segment, pointing at RSDT and XSDT
    rsdp = bytearray(struct.pack("<8sB6sBIIQB3s", b"RSD PTR ", 0, OEMID, 2,
                                 pos["RSDT"], 36, pos["XSDT"], 0, b"\0\0\0"))
    loader.append(cmd_pointer(RSDP, TABLES, 16, 4))
    loader.append(cmd_pointer(RSDP, TABLES, 24, 8))
    loader.append(cmd_checksum(RSDP, 8, 0, 20))
    loader.append(cmd_checksum(RSDP, 32, 0, 36))
    return bytes(blob), bytes(rsdp), b"".join(loader)


def main(argv):
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    ncpus = 1
    if "--cpus" in argv:
        ncpus = int(argv[argv.index("--cpus") + 1])
    if "--platform" in argv:
        # the platform description (rtl/common/platform.toml) the RTL and vsim share
        import tomllib
        global PM1A_EVT, PM1A_CNT, PM_TMR, IOAPIC_BASE, LAPIC_BASE, ECAM_BASE
        with open(argv[argv.index("--platform") + 1], "rb") as f:
            plat = tomllib.load(f)
        PM1A_EVT = plat["ports"]["pm1"]
        PM1A_CNT = PM1A_EVT + 4
        PM_TMR = PM1A_EVT + 8
        IOAPIC_BASE = plat["map"]["ioapic_base"]
        LAPIC_BASE = plat["map"]["lapic_base"]
        ECAM_BASE = plat["map"]["ecam_base"]
    dsdt = open(argv[1], "rb").read()
    tables, rsdp, loader = build(dsdt, ncpus)
    os.makedirs(argv[2], exist_ok=True)
    for name, data in (("tables", tables), ("rsdp", rsdp), ("table-loader", loader)):
        with open(os.path.join(argv[2], name), "wb") as f:
            f.write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
