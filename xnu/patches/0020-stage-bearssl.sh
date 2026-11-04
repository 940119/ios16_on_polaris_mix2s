#!/bin/bash
# Pinned, portable constant-time AES for the SWEET legacy CBC/ECB API.
set -euo pipefail
[[ "${MACHINE_CONFIG:-}" == SWEET ]] || exit 0
[[ -f libkern/crypto/corecrypto_aes.c ]] || exit 1

# bearssl.org started answering 502 and two builds in a row died on the fetch,
# so the tree is vendored as a tarball beside this script.  The archive holds the
# exact files the code below used to produce -- it was committed from a build
# tree that staged them successfully -- so a build that unpacks it compiles the
# same bytes as one that cloned the repository.  The clone stays as a fallback
# for a checkout that has no vendor directory.
bearssl_sha=7bea48e5e850ab4cafbe68d3765cdaba13a86d6f
vendor_sha=74a1ec0c169fc8e092267bf06107620e2eb29f0cc546810bc470b52e5c008718
vendor=
for candidate in \
    "$(cd "$(dirname "$0")" && pwd)/../vendor/bearssl-sweet.tgz" \
    "$PWD/../../../src/xnu/vendor/bearssl-sweet.tgz" \
    "$PWD/../../../../xnu/vendor/bearssl-sweet.tgz"; do
    if [[ -f "$candidate" ]]; then
        vendor="$candidate"
        break
    fi
done

if [[ -n "$vendor" ]]; then
    got=$(python3 -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$vendor")
    if [[ "$got" != "$vendor_sha" ]]; then
        echo "vendor tarball is $got, expected $vendor_sha" >&2
        exit 1
    fi
    if [[ -e libkern/crypto/sweet_bearssl ]]; then
        echo "refusing to overwrite existing AES source: libkern/crypto/sweet_bearssl" >&2
        exit 1
    fi
    mkdir -p libkern/crypto
    tar -xzf "$vendor" -C .
    if [[ ! -f libkern/crypto/sweet_bearssl/src/inner.h ]]; then
        echo "vendor tarball did not unpack into libkern/crypto" >&2
        exit 1
    fi
    echo "Staged BearSSL portable AES, MD5, SHA-1 and SHA-384/512 ($bearssl_sha) for SWEET from the vendor tarball"
    exit 0
fi

echo "no vendor tarball found, cloning $bearssl_sha from bearssl.org" >&2
bearssl_tmp=$(mktemp -d)
trap 'rm -rf "$bearssl_tmp"' EXIT
git init -q "$bearssl_tmp"
git -C "$bearssl_tmp" fetch -q --depth 1 \
    https://www.bearssl.org/git/BearSSL "$bearssl_sha"
git -C "$bearssl_tmp" checkout -q FETCH_HEAD
[[ "$(git -C "$bearssl_tmp" rev-parse HEAD)" == "$bearssl_sha" ]]
python3 - "$bearssl_tmp" <<'PY'
from pathlib import Path
import shutil
import sys

source = Path(sys.argv[1])
target = Path('libkern/crypto/sweet_bearssl')
if target.exists():
    raise SystemExit(f'refusing to overwrite existing AES source: {target}')
shutil.copytree(source / 'inc', target / 'inc')
(target / 'src/symcipher').mkdir(parents=True)
for file in ('inner.h', 'config.h'):
    shutil.copy2(source / 'src' / file, target / 'src' / file)
# XNU exports machine/limits.h instead of the userspace limits.h.
header = target / 'src/inner.h'
text = header.read_text()
assert text.count('#include <limits.h>') == 1
header.write_text(text.replace('#include <limits.h>',
    '#if defined(KERNEL)\n#include <machine/limits.h>\n'
    '#else\n#include <limits.h>\n#endif'))
for name in ('aes_ct', 'aes_ct_enc', 'aes_ct_dec', 'aes_ct_cbcenc', 'aes_ct_cbcdec'):
    shutil.copy2(source / 'src/symcipher' / (name + '.c'),
                 target / 'src/symcipher' / (name + '.c'))
# MD5, SHA-1 and SHA-384/512 for the legacy hash APIs, with their codecs.
for name in ('hash/md5.c', 'codec/dec32le.c', 'codec/enc32le.c',
             'hash/sha1.c', 'codec/dec32be.c', 'codec/enc32be.c',
             'hash/sha2big.c', 'codec/dec64be.c', 'codec/enc64be.c'):
    (target / 'src' / name).parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source / 'src' / name, target / 'src' / name)
shutil.copy2(source / 'LICENSE.txt', target / 'LICENSE.txt')
PY
echo "Staged BearSSL portable AES, MD5, SHA-1 and SHA-384/512 ($bearssl_sha) for SWEET"
