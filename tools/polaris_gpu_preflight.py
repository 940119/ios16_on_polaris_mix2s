#!/usr/bin/env python3
"""Offline, fail-closed Polaris A630 firmware and payload preflight.

The reserved-memory addresses and ABL limit are from this handset's recovery
DT/0090 boot layout, not generic SDM845 constants. No firmware is executed.
"""
import argparse
import hashlib
import struct
from pathlib import Path

import adt
import mkpayload

PAGE = 0x4000
ZAP_BASE, ZAP_SIZE = 0x8c415000, 0x2000
SPLASH_BASE, SPLASH_SIZE = 0x9d400000, 0x2400000
MAX_PAYLOAD_SIZE = 0x1dea000 - 0x1000000
BLOBS = (
    ('GPU-SQE', 'a630_sqe.fw'), ('GPU-GMU', 'a630_gmu.bin'),
    ('GPU-ZAP-MDT', 'a630_zap.mdt'), ('GPU-ZAP-B00', 'a630_zap.b00'),
    ('GPU-ZAP-B01', 'a630_zap.b01'), ('GPU-ZAP-B02', 'a630_zap.b02'),
)
# Sizes observed from this phone; do not silently accept another firmware set.
BLOB_SIZES = dict(zip((name for name, _ in BLOBS),
                      (32136, 32768, 6684, 148, 6536, 1968)))


def check_firmware(folder):
    files = {}
    for name, filename in BLOBS:
        path = folder / filename
        if not path.is_file() or path.is_symlink():
            raise ValueError(f'missing or nonregular firmware: {filename}')
        data = path.read_bytes()
        if len(data) != BLOB_SIZES[name]:
            raise ValueError(f'unexpected device firmware size: {filename}')
        files[name] = (path, data)
    meta = files['GPU-ZAP-MDT'][1]
    if len(meta) < 52 or meta[:6] != b'\x7fELF\x01\x01':
        raise ValueError('zap metadata is not little-endian ELF32')
    phoff, = struct.unpack_from('<I', meta, 28)
    entsz, n = struct.unpack_from('<HH', meta, 42)
    if entsz != 32 or not 0 < n <= 32 or phoff + n * entsz > len(meta):
        raise ValueError('zap program headers out of bounds')
    loads = []
    for i in range(n):
        kind, _, _, addr, file_size, mem_size, flags, _ = struct.unpack_from(
            '<8I', meta, phoff + i * entsz)
        if kind == 1 and ((flags >> 24) & 7) != 2 and mem_size:
            loads.append((addr, file_size, mem_size))
    if len(loads) != 1:
        raise ValueError('expected one non-hash zap load segment')
    addr, file_size, mem_size = loads[0]
    if addr != 0x5000 or file_size != len(files['GPU-ZAP-B02'][1]) or \
            mem_size < file_size or mem_size > ZAP_SIZE:
        raise ValueError('zap load segment does not match this Polaris reservation')
    return files


def overlap(base, size, other, other_size):
    return base < other + other_size and other < base + size


def check_layout(kernel, tree, cmdline, files):
    raw = tree.read_bytes()
    root, end = adt.parse(raw)
    if end != len(raw):
        raise ValueError('ADT has trailing bytes')
    chosen = next(c for c in root.children if c.name == 'chosen')
    base, size = (struct.unpack('<Q', chosen.prop(n))[0]
                  for n in ('dram-base', 'dram-size'))
    if not size or base + size > 1 << 64:
        raise ValueError('invalid XNU managed RAM range')
    for label, addr, length in (('zap', ZAP_BASE, ZAP_SIZE),
                                ('splash', SPLASH_BASE, SPLASH_SIZE)):
        if overlap(base, size, addr, length):
            raise ValueError(f'XNU managed RAM overlaps {label} reserved memory')
    image, _ = mkpayload.build(kernel, tree, cmdline,
                                blobs=[(name, str(files[name][0])) for name, _ in BLOBS])
    if len(image) > MAX_PAYLOAD_SIZE:
        raise ValueError(f'payload {len(image)} exceeds ABL gap {MAX_PAYLOAD_SIZE}')
    adt_off, adt_size = struct.unpack_from('<2Q', image, 0x20)
    packed, _ = adt.parse(image[adt_off:adt_off + adt_size])
    packed_chosen = next(c for c in packed.children if c.name == 'chosen')
    memory_map = next(c for c in packed_chosen.children if c.name == 'memory-map')
    intervals = []
    for name, _ in BLOBS:
        value = memory_map.prop(name)
        if value is None or len(value) != 16:
            raise ValueError(f'missing ADT entry: {name}')
        addr, length = struct.unpack('<2Q', value)
        reserved = (length + PAGE - 1) & ~(PAGE - 1)
        if length != len(files[name][1]) or addr & (PAGE - 1) or \
                addr < base or addr + reserved > base + size:
            raise ValueError(f'invalid ADT firmware allocation: {name}')
        if any(overlap(addr, reserved, a, s) for a, s in intervals):
            raise ValueError(f'overlapping firmware allocations: {name}')
        intervals.append((addr, reserved))
    return len(image), MAX_PAYLOAD_SIZE - len(image)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('firmware_dir', type=Path)
    parser.add_argument('kernel', type=Path)
    parser.add_argument('adt', type=Path)
    parser.add_argument('--cmdline', required=True)
    args = parser.parse_args()
    try:
        files = check_firmware(args.firmware_dir)
        used, remaining = check_layout(args.kernel, args.adt, args.cmdline, files)
    except (ValueError, OSError, AssertionError, StopIteration, struct.error) as exc:
        parser.error(str(exc))
    for name, _ in BLOBS:
        print(f'{name}: {len(files[name][1])} bytes '
              f'sha256={hashlib.sha256(files[name][1]).hexdigest()}')
    print(f'layout verified: payload={used} bytes, ABL gap remaining={remaining} bytes')
    print('No GPU power, secure PAS, HFI, SMMU, rendering, or boot was tested.')


if __name__ == '__main__':
    main()
