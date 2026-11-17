#!/usr/bin/env python3
"""Build the vibe86 firmware's flash device image (FD).

    mkfd.py -o vibe86.fd --layout fw/include/layout.h --sec sec.bin --pei pei.efi
            --core DxeCore.efi [--driver Name=file.efi]... [--app Name=file.efi]...
            [--raw GUID=file]...

The image follows fw/include/layout.h: FV_MAIN (a PI firmware volume, UEFI
PI 1.8 Vol. 3 ch. 3: the volume header, FFS2 files with PE32, user-interface
and raw sections), the PEI image (a PE32+ linked at PEI_BASE and run in
place, so its file layout must be its memory layout and it must have no
writable data), and SEC at the top. Drivers are dispatched in the order
given; their FFS names are UUIDv5 of their names (RFC 9562 5.5) so they stay
stable across builds.
"""
import argparse
import re
import struct
import sys
import uuid

FFS2 = uuid.UUID("8c8ce578-8a3d-4f1c-9935-896185c32dd3")
NAMESPACE = uuid.UUID("d6a2cb7f-6a18-4e2f-b43c-766238360000")
DXE_CORE = uuid.UUID("d6a2cb7f-6a18-4e2f-b43c-766238360001")

FT_RAW, FT_DXE_CORE, FT_DRIVER, FT_APPLICATION = 0x01, 0x05, 0x07, 0x09
SEC_PE32, SEC_UI, SEC_RAW = 0x10, 0x15, 0x19


def layout(path):
    vals = {}
    for m in re.finditer(r"#define\s+(\w+)\s+(0x[0-9A-Fa-f]+|\d+)", open(path).read()):
        vals[m.group(1)] = int(m.group(2), 0)
    return vals


def section(kind, data):
    size = 4 + len(data)
    return struct.pack("<I", size | kind << 24)[:3] + bytes([kind]) + data


def ffs_file(name, ftype, sections):
    body = b""
    for s in sections:
        body += s + b"\0" * (-len(body + s) % 4)
    size = 24 + len(body)
    hdr = bytearray(name.bytes_le + bytes([0, 0xAA, ftype, 0]) + struct.pack("<I", size)[:3] + b"\xf8")
    # IntegrityCheck.Header: the header's 8-bit sum is 0 with State and
    # IntegrityCheck.File taken as 0 (PI Vol. 3 2.2.3)
    s = (sum(hdr) - hdr[17] - hdr[23]) & 0xFF
    hdr[16] = (-s) & 0xFF
    return bytes(hdr) + body


def volume(files, size):
    hdr_len = 0x48
    body = b""
    for f in files:
        body += b"\xff" * (-(hdr_len + len(body)) % 8)
        body += f
    if hdr_len + len(body) > size:
        sys.exit(f"mkfd: FV_MAIN overflow: {hdr_len + len(body)} > {size}")
    hdr = bytearray(16) + FFS2.bytes_le + struct.pack("<QIIHHHBB", size, 0x4856465F, 0x0004FEFF,
                                                       hdr_len, 0, 0, 0, 2)
    hdr += struct.pack("<IIII", size // 4096, 4096, 0, 0)
    assert len(hdr) == hdr_len
    csum = sum(struct.unpack(f"<{hdr_len // 2}H", bytes(hdr))) & 0xFFFF
    struct.pack_into("<H", hdr, 0x32, (-csum) & 0xFFFF)
    return bytes(hdr) + body + b"\xff" * (size - hdr_len - len(body))


def ui(name):
    return section(SEC_UI, (name + "\0").encode("utf-16-le"))


def check_pei(data, base):
    """The PEI image runs in place: headers at base, file offset = RVA."""
    lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[lfanew:lfanew + 4] != b"PE\0\0":
        sys.exit("mkfd: PEI isn't a PE image")
    nsec = struct.unpack_from("<H", data, lfanew + 6)[0]
    optsz = struct.unpack_from("<H", data, lfanew + 20)[0]
    opt = lfanew + 24
    image_base = struct.unpack_from("<Q", data, opt + 24)[0]
    sec_align, file_align = struct.unpack_from("<II", data, opt + 32)
    size_of_image = struct.unpack_from("<I", data, opt + 56)[0]
    if image_base != base or sec_align != file_align:
        sys.exit(f"mkfd: PEI must be linked at {base:#x} with equal alignments")
    for i in range(nsec):
        s = opt + optsz + 40 * i
        name = data[s:s + 8].rstrip(b"\0").decode()
        vsize, rva, rawsize, rawptr = struct.unpack_from("<IIII", data, s + 8)
        chars = struct.unpack_from("<I", data, s + 36)[0]
        if rva != rawptr and rawsize:
            sys.exit(f"mkfd: PEI section {name}: file offset {rawptr:#x} != RVA {rva:#x}")
        if chars & 0x80000000 and vsize:
            sys.exit(f"mkfd: PEI section {name} is writable: PEI runs from flash")
    return data.ljust(size_of_image, b"\0")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", required=True)
    ap.add_argument("--layout", required=True)
    ap.add_argument("--sec", required=True)
    ap.add_argument("--pei", required=True)
    ap.add_argument("--core", required=True)
    ap.add_argument("--driver", action="append", default=[])
    ap.add_argument("--app", action="append", default=[])
    ap.add_argument("--raw", action="append", default=[])
    a = ap.parse_args()
    L = layout(a.layout)

    files = [ffs_file(DXE_CORE, FT_DXE_CORE, [section(SEC_PE32, open(a.core, "rb").read()), ui("DxeCore")])]
    for d in a.driver:
        name, path = d.split("=", 1)
        files.append(ffs_file(uuid.uuid5(NAMESPACE, name), FT_DRIVER,
                              [section(SEC_PE32, open(path, "rb").read()), ui(name)]))
    for d in a.app:
        name, path = d.split("=", 1)
        g = uuid.UUID(name.split(":")[1]) if ":" in name else uuid.uuid5(NAMESPACE, name)
        name = name.split(":")[0]
        files.append(ffs_file(g, FT_APPLICATION, [section(SEC_PE32, open(path, "rb").read()), ui(name)]))
    for r in a.raw:
        g, path = r.split("=", 1)
        files.append(ffs_file(uuid.UUID(g), FT_RAW, [section(SEC_RAW, open(path, "rb").read())]))

    fd = bytearray(b"\xff" * L["FD_SIZE"])
    fv_off = L["FV_MAIN_BASE"] - L["FD_BASE"]
    fd[fv_off:fv_off + L["FV_MAIN_SIZE"]] = volume(files, L["FV_MAIN_SIZE"])
    pei = check_pei(open(a.pei, "rb").read(), L["PEI_BASE"])
    if len(pei) > L["PEI_SIZE"]:
        sys.exit("mkfd: PEI too large")
    off = L["PEI_BASE"] - L["FD_BASE"]
    fd[off:off + len(pei)] = pei
    sec = open(a.sec, "rb").read()
    if len(sec) != L["SEC_SIZE"]:
        sys.exit(f"mkfd: SEC must be {L['SEC_SIZE']} bytes")
    off = L["SEC_BASE"] - L["FD_BASE"]
    fd[off:off + len(sec)] = sec
    open(a.o, "wb").write(fd)


if __name__ == "__main__":
    main()
