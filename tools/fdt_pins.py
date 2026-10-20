#!/usr/bin/env python3
"""Inspect stock polaris FDT phandles for an I2C controller and its pinctrl pins."""
import argparse
import pathlib
import struct


def u32(data, at):
    return struct.unpack_from('>I', data, at)[0]


def nodes(blob):
    st, names = u32(blob, 8), u32(blob, 12)
    p, stack = st, []
    while True:
        token = u32(blob, p)
        p += 4
        if token == 1:
            e = blob.index(0, p)
            name = blob[p:e].decode(errors='replace')
            stack.append(name)
            p = (e + 4) & ~3
            yield 'begin', '/' + '/'.join(x for x in stack if x), None, None
        elif token == 2:
            stack.pop()
        elif token == 3:
            length, index = struct.unpack_from('>II', blob, p)
            p += 8
            start = names + index
            end = blob.index(0, start)
            key = blob[start:end].decode()
            value = blob[p:p + length]
            p = (p + length + 3) & ~3
            yield 'property', '/' + '/'.join(x for x in stack if x), key, value
        elif token == 4:
            pass
        elif token == 9:
            break
        else:
            raise ValueError(f'FDT token {token} at {p-4:#x}')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('dtb', type=pathlib.Path)
    ap.add_argument('--match', default='a98000', help='path substring')
    opts = ap.parse_args()
    blob = opts.dtb.read_bytes()
    if blob[:4] != b'\xd0\x0d\xfe\xed':
        raise SystemExit('not an FDT')
    props = {}
    for typ, path, name, value in nodes(blob):
        if typ == 'begin':
            props[path] = {}
        else:
            props[path][name] = value
    phandles = {}
    for path, entries in props.items():
        if 'phandle' in entries and len(entries['phandle']) == 4:
            phandles[u32(entries['phandle'], 0)] = path
    for path, entries in props.items():
        if opts.match.lower() not in path.lower():
            continue
        print(path)
        for name, value in entries.items():
            if name.startswith('pinctrl-') and len(value) % 4 == 0:
                cells = struct.unpack('>' + 'I' * (len(value) // 4), value)
                print(f'  {name}:', ' '.join(f'{x:#x} => {phandles.get(x, "?")}' for x in cells))
                for x in cells:
                    base = phandles.get(x, '')
                    for pinpath, pin in props.items():
                        if pinpath != base and not pinpath.startswith(base + '/'):
                            continue
                        for key, raw in pin.items():
                            if key in ('pins', 'function', 'bias-pull-up', 'bias-disable', 'drive-strength'):
                                print(f'    {pinpath}: {key}={raw!r}')
            elif name in ('compatible', 'reg', 'status'):
                print(f'  {name}: {value!r}')


if __name__ == '__main__':
    main()
