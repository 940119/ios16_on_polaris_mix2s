#!/usr/bin/env python3
"""Make a gzip'd newc cpio ramdisk without host cpio/gzip binaries.

  mkcpio.py OUT [DIR]

With no DIR the archive contains only the newc trailer, which is a valid
(empty) ramdisk and keeps ABL happy when the shim just probes.
"""
import gzip, os, stat, sys


def entry(name, data=b'', mode=0o100644, ino=1):
    name_b = name.encode() + b'\0'
    fields = [ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name_b), 0]
    hdr = b'070701' + b''.join(b'%08X' % f for f in fields)
    out = hdr + name_b
    out += b'\0' * (-len(out) % 4)
    out += data
    out += b'\0' * (-len(data) % 4)
    return out


def main():
    out_path = sys.argv[1]
    src = sys.argv[2] if len(sys.argv) > 2 else None
    out = bytearray()
    if src:
        for root, dirs, files in os.walk(src):
            dirs.sort()
            for d in dirs:
                full = os.path.join(root, d)
                rel = '.' + full[len(src):].replace(os.sep, '/')
                out += entry(rel + '/', mode=0o040755)
            for f in sorted(files):
                full = os.path.join(root, f)
                rel = '.' + full[len(src):].replace(os.sep, '/')
                out += entry(rel, open(full, 'rb').read())
    out += entry('TRAILER!!!', ino=0)
    out += b'\0' * (-len(out) % 512)
    blob = gzip.compress(bytes(out), 9, mtime=0)
    open(out_path, 'wb').write(blob)
    print(f'{out_path}: {len(blob)} bytes gzip, {len(out)} bytes cpio', file=sys.stderr)


if __name__ == '__main__':
    main()
