#!/usr/bin/env python3
"""Assemble the Qualcomm appended-DTB tail for a Mi MIX 2S boot image.

The stock polaris kernel is followed by 14 concatenated DTBs; ABL scans them
and hands the matching board variant to the kernel (and to our shim in x0).
Reuse the stock tail verbatim so board-id/msm-id selection is unchanged.

  polaris_dtb.py FDT_DIR OUT
  polaris_dtb.py --from-boot BOOT_IMG OUT

--from-boot parses the Android boot image, decompresses the gzip kernel if
needed and keeps everything after it. Feed it your own stock boot.img when
the prebuilt probe image's DTB tail does not match your firmware.
"""
import glob, os, struct, sys, zlib

FDT_MAGIC = b'\xd0\x0d\xfe\xed'


def be32(b, off):
    return struct.unpack_from('>I', b, off)[0]


def root_props(b):
    off = be32(b, 8)			# off_dt_struct
    strings = be32(b, 12)
    # FDT_BEGIN_NODE (root, empty name), then properties until FDT_END_NODE
    assert be32(b, off) == 1
    p = off + 4
    e = b.index(b'\0', p)			# root name (usually empty)
    p = (e + 1 + 3) & ~3
    props = {}
    while be32(b, p) == 3:
        ln, nameoff = struct.unpack_from('>II', b, p + 4)
        val = b[p + 12:p + 12 + ln]
        name = b[strings + nameoff:b.index(b'\0', strings + nameoff)].decode()
        props[name] = val
        p = (p + 12 + ln + 3) & ~3
    return props


def cells(val):
    return ' '.join(hex(x) for x in struct.unpack('>%dI' % (len(val) // 4), val)) if val else '-'


def dtb_chain(buf, start):
    """Number of whole DTBs from start to EOF, or None if it is not a chain."""
    p = start
    n = 0
    while p < len(buf):
        if buf[p:p + 4] != FDT_MAGIC or p + 8 > len(buf):
            return None
        total = be32(buf, p + 4)
        if total < 0x30 or p + total > len(buf):
            return None
        p += total
        n += 1
    return n if p == len(buf) else None


def split_dtbs(tail):
    if tail[:4] == FDT_MAGIC:
        n = dtb_chain(tail, 0)
        if n:
            starts, p = [], 0
            for _ in range(n):
                starts.append(p)
                p += be32(tail, p + 4)
            return starts
    # Not a plain tail (or an uncompressed kernel): find the first magic whose
    # chain of whole DTBs ends exactly at EOF.
    i = 0
    while True:
        i = tail.find(FDT_MAGIC, i)
        if i < 0:
            return None
        if dtb_chain(tail, i):
            return _starts(tail, i)
        i += 4


def _starts(buf, start):
    starts = []
    p = start
    while p < len(buf):
        starts.append(p)
        p += be32(buf, p + 4)
    return starts


def kernel_of_boot(path):
    b = open(path, 'rb').read()
    if b[:8] != b'ANDROID!':
        raise SystemExit(f'{path}: not an Android boot image')
    ks, ka, rs, ra, ss, sa, ta, ps = struct.unpack_from('<8I', b, 8)
    if ps < 512 or ps & (ps - 1) or ps > len(b):
        raise SystemExit(f'{path}: bad page size {ps}')
    return b[ps:ps + ks]


def tail_of_boot(path):
    k = kernel_of_boot(path)
    if k[:2] == b'\x1f\x8b':
        d = zlib.decompressobj(31)
        d.decompress(k)
        if not d.eof:
            raise SystemExit(f'{path}: truncated gzip kernel')
        tail = d.unused_data
        print(f'kernel: gzip, tail {len(tail)} bytes', file=sys.stderr)
    else:
        tail = k
        print(f'kernel: raw, scanning {len(tail)} bytes', file=sys.stderr)
    starts = split_dtbs(tail)
    if not starts:
        raise SystemExit(f'{path}: no appended DTB chain found')
    return tail, starts


def report_tail(tail, starts):
    for i, s in enumerate(starts):
        b = tail[s:s + be32(tail, s + 4)]
        props = root_props(b)
        model = props.get('model', b'').split(b'\0')[0].decode(errors='replace')
        print(f'{i:2d} {len(b):8d}  {model:24s} board-id={cells(props.get("qcom,board-id"))} '
              f'msm-id={cells(props.get("qcom,msm-id"))}')


def main():
    if len(sys.argv) == 4 and sys.argv[1] == '--from-boot':
        tail, starts = tail_of_boot(sys.argv[2])
        report_tail(tail, starts)
        tail = tail[starts[0]:]
        out = sys.argv[3]
        n = len(starts)
    elif len(sys.argv) == 3:
        src, out = sys.argv[1], sys.argv[2]
        files = sorted(glob.glob(os.path.join(src, 'dtb_*.dtb')))
        if not files:
            raise SystemExit(f'{src}: no dtb_*.dtb')
        tail = bytearray()
        for path in files:
            b = open(path, 'rb').read()
            if b[:4] != FDT_MAGIC:
                raise SystemExit(f'{path}: not a DTB')
            total = be32(b, 4)
            if total > len(b):
                raise SystemExit(f'{path}: truncated ({total} > {len(b)})')
            tail += b
        tail = bytes(tail)
        starts = []
        p = 0
        for path in files:
            starts.append(p)
            p += os.path.getsize(path)
        report_tail(tail, starts)
        n = len(files)
    else:
        raise SystemExit('usage: polaris_dtb.py FDT_DIR OUT | --from-boot BOOT_IMG OUT')

    open(out, 'wb').write(tail)
    print(f'{out}: {len(tail)} bytes, {n} DTBs', file=sys.stderr)


if __name__ == '__main__':
    main()
