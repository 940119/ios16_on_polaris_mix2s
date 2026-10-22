#!/usr/bin/env python3
"""Offline-only Qualcomm BCM command-db metadata decoder.

Input is an explicitly supplied byte buffer from an authorized source; this
module never reads physical memory or sends RPMh votes. Parsing addresses and
aux is not enough to derive DDR/CNOC commands: the bus graph and aggregation
are separate required inputs.
"""
import struct
from dataclasses import dataclass
from polaris_arc_db import CommandDbError, ENTRY, HEADER, MAGIC, MAX_DB, _bounded

BCM_SLAVE = 5
BCM_AUX_SIZE = 8  # MiCode struct bcm_db: u32 unit_size, u16 width, u8 VCD, u8 reserved
MAX_BCM_ENTRIES = 256
MAX_VCD = 16  # conservative parser limit, not a hardware TCS capacity claim


@dataclass(frozen=True)
class BcmResource:
    name: str
    address: int
    unit_size: int
    width: int
    vcd: int


def parse_bcm(data: bytes) -> dict[str, BcmResource]:
    if not HEADER <= len(data) <= MAX_DB or struct.unpack_from('<I', data, 4)[0] != MAGIC:
        raise CommandDbError('invalid command-db length or magic')
    if struct.unpack_from('<I', data)[0] != 1 or struct.unpack_from('<I', data, 140)[0] & 1:
        raise CommandDbError('unsupported version or standalone database')
    resources = {}
    bcm_headers = 0
    for slot in range(8):
        slave, head_off, aux_off, count, version = struct.unpack_from('<5H', data, 8 + slot * 16)
        if slave == 0:
            break
        if slave != BCM_SLAVE:
            continue
        bcm_headers += 1
        if bcm_headers != 1 or version == 0 or not 0 < count <= MAX_BCM_ENTRIES:
            raise CommandDbError('duplicate or invalid BCM slave header')
        entries = HEADER + head_off
        aux = HEADER + aux_off
        if entries % 4 or not _bounded(entries, count * ENTRY, len(data)) or not _bounded(aux, 0, len(data)):
            raise CommandDbError('BCM entry array outside command-db')
        for index in range(count):
            p = entries + index * ENTRY
            raw = data[p:p + 8]
            name = raw.split(b'\0', 1)[0]
            if not name or not name.isascii() or not name.decode('ascii').isalnum() or \
                    raw != name.ljust(8, b'\0'):
                raise CommandDbError('invalid BCM resource ID')
            text = name.decode('ascii')
            address, length, offset = struct.unpack_from('<IHH', data, p + 16)
            start = aux + offset
            if text in resources or (address >> 16) != BCM_SLAVE or \
                    length != BCM_AUX_SIZE or not _bounded(start, length, len(data)):
                raise CommandDbError(f'invalid or duplicate BCM entry: {text}')
            unit, width, vcd, _reserved = struct.unpack_from('<IHBB', data, start)
            if not unit or not width or vcd >= MAX_VCD:
                raise CommandDbError(f'invalid BCM metadata: {text}')
            resources[text] = BcmResource(text, address, unit, width, vcd)
    if not resources:
        raise CommandDbError('missing BCM slave')
    return resources


def encode_vote(commit: bool, ab: int, ib: int) -> int:
    """MiCode TCS bit packing for already-aggregated vectors, NOT a vote API."""
    if type(commit) is not bool or type(ab) is not int or type(ib) is not int or ab < 0 or ib < 0:
        raise CommandDbError('invalid aggregated vectors')
    valid = bool(ab or ib)
    return (int(bool(commit)) << 30) | (int(valid) << 29) | \
           (min(ab, 0x3fff) << 14) | min(ib, 0x3fff)
