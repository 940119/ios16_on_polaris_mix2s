#!/usr/bin/env python3
"""Link-safety checks for the freestanding shim (no binutils needed).

The shim is linked at address 0 and ABL loads it at 0x8000. Any absolute
address embedded by the compiler would be off by the load address, so C
objects must contain no absolute relocations; PC-relative ones are fine.

  elfcheck.py abs64 OBJ...        fail if any absolute relocation is present
  elfcheck.py symbol ELF NAME     print a symbol's value in decimal
  elfcheck.py relocs ELF          list relocation section count/types
  elfcheck.py image FILE          validate the arm64 Linux Image header
"""
import struct, sys

ABS_TYPES = {257: 'ABS64', 258: 'ABS32', 259: 'ABS16',
             263: 'MOVW_UABS_G0', 264: 'MOVW_UABS_G0_NC',
             265: 'MOVW_UABS_G1', 266: 'MOVW_UABS_G1_NC',
             267: 'MOVW_UABS_G2', 268: 'MOVW_UABS_G2_NC',
             269: 'MOVW_UABS_G3'}

SHT_SYMTAB, SHT_RELA, SHT_REL = 2, 4, 9
EM_AARCH64 = 183


def elf(path):
    b = open(path, 'rb').read()
    if b[:4] != b'\x7fELF' or b[4] != 2 or b[5] != 1:
        raise SystemExit(f'{path}: not a little-endian ELF64')
    (_, _, machine, _, entry, phoff, shoff, _, _, phentsize, phnum,
     shentsize, shnum, shstrndx) = struct.unpack_from('<16sHHIQQQIHHHHHH', b)
    if machine != EM_AARCH64:
        raise SystemExit(f'{path}: e_machine={machine}, not AArch64')
    sh = []
    for i in range(shnum):
        o = shoff + i * shentsize
        sh.append(struct.unpack_from('<IIQQQQIIQQ', b, o))
    strtab = sh[shstrndx]
    base = strtab[4]
    names = [b[base + s[0]:b.index(b'\0', base + s[0])].decode(errors='replace')
             for s in sh]
    return b, sh, names


def relocs(path):
    b, sh, names = elf(path)
    out = []
    for i, s in enumerate(sh):
        if s[1] not in (SHT_RELA, SHT_REL):
            continue
        target = sh[s[7]] if s[7] < len(sh) else None
        if target is not None and not (target[2] & 0x2):	# SHF_ALLOC
            continue			# .debug_* relocations never reach the image
        rela = s[1] == SHT_RELA
        size = 24 if rela else 16
        for o in range(s[4], s[4] + s[5], size):
            if rela:
                off, info, addend = struct.unpack_from('<QQq', b, o)
            else:
                off, info = struct.unpack_from('<QQ', b, o)
                addend = None
            out.append((names[i], info & 0xffffffff, info >> 32, off, addend))
    return out


def cmd_abs64(paths):
    bad = 0
    for path in paths:
        for sec, typ, sym, off, addend in relocs(path):
            if typ in ABS_TYPES:
                bad += 1
                print(f'{path}: {ABS_TYPES[typ]} in {sec} at +0x{off:x} '
                      f'sym={sym} addend={addend}', file=sys.stderr)
    if bad:
        raise SystemExit(f'{bad} absolute relocation(s): image must be position-independent')
    print(f'abs64: none in {len(paths)} object(s)')


def cmd_symbol(path, want):
    b, sh, names = elf(path)
    for s in sh:
        if s[1] != SHT_SYMTAB:
            continue
        strs = sh[s[6]]
        link = strs[4]
        estr = s[4] + s[5]
        for o in range(s[4], estr, 24):
            nameoff, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', b, o)
            e = b.index(b'\0', link + nameoff)
            if b[link + nameoff:e].decode() == want:
                print(value)
                return
    raise SystemExit(f'{path}: no symbol {want}')


def cmd_relocs(path):
    rs = relocs(path)
    kinds = {}
    for _, typ, _, _, _ in rs:
        kinds[typ] = kinds.get(typ, 0) + 1
    print(f'{path}: {len(rs)} relocation(s) {kinds}')
    if rs:
        raise SystemExit('linked image still has relocations')


def cmd_image(path):
    b = open(path, 'rb').read()
    if b[0x38:0x3c] != b'ARM\x64':
        raise SystemExit(f'{path}: bad arm64 Image magic at 0x38')
    code1, text_offset, image_size, flags = struct.unpack_from('<IQQQ', b, 4)
    if code1 != 0:
        raise SystemExit(f'{path}: code1={code1:#x} (reserved, must be 0)')
    if flags & 0xf != 0xa:
        raise SystemExit(f'{path}: flags={flags:#x}, want LE/4K/anywhere (0xa)')
    if image_size > len(b):
        raise SystemExit(f'{path}: image_size={image_size} > file size {len(b)}')
    print(f'{path}: image_size={image_size} file={len(b)} text_offset={text_offset} '
          f'flags={flags:#x} magic=ok')
    return image_size


if __name__ == '__main__':
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == 'abs64':
        cmd_abs64(args)
    elif cmd == 'symbol' and len(args) == 2:
        cmd_symbol(*args)
    elif cmd == 'relocs' and len(args) == 1:
        cmd_relocs(args[0])
    elif cmd == 'image' and len(args) == 1:
        cmd_image(args[0])
    else:
        raise SystemExit(__doc__)
