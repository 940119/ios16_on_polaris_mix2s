#!/usr/bin/env python3
"""Build a flat image from an ELF64 by placing each allocated section at its
virtual address.

objcopy -O binary derives the layout from program headers and can shift
allocated sections by a few bytes relative to their VMA. The shim executes
at an arbitrary load address and addresses data PC-relatively, so file
offset must equal VMA exactly.
"""
import struct
import sys

SHT_PROGBITS = 1
SHF_ALLOC = 0x2


def main():
    elf_path, out_path = sys.argv[1], sys.argv[2]
    b = open(elf_path, 'rb').read()
    if b[:4] != b'\x7fELF' or b[4] != 2 or b[5] != 1:
        sys.exit(f'{elf_path}: not a little-endian ELF64')
    (_, _, machine, _, entry, phoff, shoff, _, _, phent, phnum,
     shent, shnum, shstrndx) = struct.unpack_from('<16sHHIQQQIHHHHHH', b)
    sh = [struct.unpack_from('<IIQQQQIIQQ', b, shoff + i * shent)
          for i in range(shnum)]
    end = 0
    for s in sh:
        if s[2] & SHF_ALLOC:
            end = max(end, s[3] + s[5])
    img = bytearray(end)
    for s in sh:
        if not (s[2] & SHF_ALLOC) or s[1] != SHT_PROGBITS or s[5] == 0:
            continue
        img[s[3]:s[3] + s[5]] = b[s[4]:s[4] + s[5]]
    open(out_path, 'wb').write(img)
    print(f'{out_path}: {len(img)} bytes (max VMA end 0x{end:x})')


if __name__ == '__main__':
    main()
