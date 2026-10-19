#!/usr/bin/env python3
"""Portable Android boot image packer (header v0-v2).

Replaces the host `mkbootimg` dependency so the shim can be packed on
Windows/macOS/Linux with only Python. The polaris probe mirrors the stock
Mi MIX 2S image: header v1, 4K pages, kernel at 0x8000, ramdisk at 0x1000000,
and the Qualcomm DTB tail appended to the kernel section.

  mkbootimg.py -o boot.img --kernel K --ramdisk R --header_version 1 \
      --pagesize 4096 --base 0 --kernel_offset 0x8000 \
      --ramdisk_offset 0x1000000 --tags_offset 0x100 \
      --os_version 8.1.0 --os_patch_level 2035-06 --cmdline "console=null"
"""
import argparse, hashlib, os, struct, sys

MAGIC = b'ANDROID!'
V0_SIZE, V1_SIZE, V2_SIZE = 1632, 1648, 1660


def u32(v):
    return struct.pack('<I', v)


def u64(v):
    return struct.pack('<Q', v)


def encode_os(s):
    if s is None:
        return 0
    if isinstance(s, int):
        return s
    try:
        return int(s, 0)		# raw header value, e.g. 0x200001a2
    except ValueError:
        pass
    a, b, c = (list(map(int, s.split('.'))) + [0, 0])[:3]
    return (a << 14) | (b << 7) | c


def encode_patch(s):
    if s is None:
        return 0
    if isinstance(s, int):
        return s
    try:
        return int(s, 0)
    except ValueError:
        pass
    y, m = map(int, s.split('-'))
    return ((y - 2000) << 4) | m


def parse_addr(s):
    return int(s, 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-o', '--output', required=True)
    ap.add_argument('--kernel', required=True)
    ap.add_argument('--ramdisk')
    ap.add_argument('--second')
    ap.add_argument('--dtb')
    ap.add_argument('--recovery_dtbo')
    ap.add_argument('--header_version', type=int, default=0)
    ap.add_argument('--pagesize', type=int, default=2048)
    ap.add_argument('--base', type=parse_addr, default=0)
    ap.add_argument('--kernel_offset', type=parse_addr, default=0x00008000)
    ap.add_argument('--ramdisk_offset', type=parse_addr, default=0x01000000)
    ap.add_argument('--second_offset', type=parse_addr, default=0x00f00000)
    ap.add_argument('--tags_offset', type=parse_addr, default=0x00000100)
    ap.add_argument('--dtb_offset', type=parse_addr, default=0x01f00000)
    ap.add_argument('--os_version')
    ap.add_argument('--os_patch_level')
    ap.add_argument('--cmdline', default='')
    ap.add_argument('--name', default='')
    a = ap.parse_args()

    if a.header_version not in (0, 1, 2):
        raise SystemExit('header_version must be 0, 1 or 2')
    page = a.pagesize
    if page < 512 or page & (page - 1):
        raise SystemExit('pagesize must be a power of two >= 512')

    def read(path):
        return open(path, 'rb').read() if path else b''

    kernel = read(a.kernel)
    ramdisk = read(a.ramdisk)
    second = read(a.second)
    dtbo = read(a.recovery_dtbo)
    dtb = read(a.dtb)
    if not kernel:
        raise SystemExit('empty kernel')

    header_size = (V0_SIZE, V1_SIZE, V2_SIZE)[a.header_version]
    hdr = bytearray(header_size)
    hdr[0:8] = MAGIC
    struct.pack_into('<10I', hdr, 8,
                     len(kernel), (a.base + a.kernel_offset) & 0xffffffff,
                     len(ramdisk), (a.base + a.ramdisk_offset) & 0xffffffff,
                     len(second), (a.base + a.second_offset) & 0xffffffff,
                     (a.base + a.tags_offset) & 0xffffffff,
                     page, a.header_version,
                     encode_os(a.os_version))
    name = a.name.encode()[:15]
    hdr[48:48 + len(name)] = name
    cmd = a.cmdline.encode()[:511]
    hdr[64:64 + len(cmd)] = cmd
    # id[8] stays zero: nothing on this boot path verifies it.
    if a.header_version >= 1:
        struct.pack_into('<I', hdr, 1632, len(dtbo))
        struct.pack_into('<Q', hdr, 1636, (a.base + a.dtb_offset) & 0xffffffffffffffff)
        struct.pack_into('<I', hdr, 1644, header_size)
    if a.header_version >= 2:
        struct.pack_into('<I', hdr, 1648, len(dtb))
        struct.pack_into('<Q', hdr, 1652, (a.base + a.dtb_offset) & 0xffffffffffffffff)

    out = bytearray(hdr)
    out += b'\0' * (-len(out) % page)
    sections = [('kernel', kernel), ('ramdisk', ramdisk), ('second', second)]
    if a.header_version >= 1:
        sections.append(('recovery_dtbo', dtbo))
    if a.header_version >= 2:
        sections.append(('dtb', dtb))
    for _, blob in sections:
        out += blob
        out += b'\0' * (-len(out) % page)
    open(a.output, 'wb').write(out)
    print(f'{a.output}: {len(out)} bytes  kernel={len(kernel)} ramdisk={len(ramdisk)} '
          f'header=v{a.header_version}', file=sys.stderr)
    print('sha256', hashlib.sha256(out).hexdigest(), file=sys.stderr)


if __name__ == '__main__':
    main()
