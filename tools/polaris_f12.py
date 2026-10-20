#!/usr/bin/env python3
"""Reject unsupported F12 Query8 layouts and decode candidate Data1 frames.

Vendor DSX reads query_base+7 for Query8 length and query_base+8 for its bytes.
The observed value at +7 is 1, insufficient to establish a Data1 presence
bit under that parser. This guarded device-specific validator MUST reject it;
the raw frame decoder is an offline hypothesis, not proof of register layout.
"""
import argparse


def data1_address(query_base: int, data_base: int, query_bytes: bytes) -> int:
    if len(query_bytes) < 11:
        raise ValueError('need query bytes through query_base+10')
    size = query_bytes[7]
    if size < 2 or size > 5:
        raise ValueError(f'Query8 too short for Data1 presence: {size}')
    q8_size = query_bytes[8]
    presence = query_bytes[9]
    if q8_size < 2 or q8_size > 32 or not (presence & 2):
        raise ValueError(f'unsupported Data1 presence: size={q8_size} map={presence:#x}')
    if presence & 1:
        raise ValueError('Data0 present; its size must be parsed before Data1')
    if query_base != 0x49 or data_base != 0x08:
        raise ValueError('PDT bases differ from tested S3330 firmware')
    return data_base


def decode_finger(packet: bytes, max_x: int = 1079, max_y: int = 2159):
    if len(packet) != 8:
        raise ValueError('expected exactly one 8-byte F12 Data1 object')
    kind = packet[0]
    if kind == 0:
        return None
    if kind != 1:
        raise ValueError(f'unsupported object type {kind}')
    x = packet[1] | packet[2] << 8
    y = packet[3] | packet[4] << 8
    if x > max_x or y > max_y:
        raise ValueError(f'out-of-range contact ({x}, {y})')
    return {'x': x, 'y': y, 'pressure': packet[5], 'width_x': packet[6], 'width_y': packet[7]}


def selftest():
    q = bytes.fromhex('03 04 26 ff 0f 00 01 01 07 12 80 00 00 50 ff 07')
    try:
        data1_address(0x49, 0x08, q)
    except ValueError as exc:
        assert 'too short' in str(exc)
    else:
        raise AssertionError('truncated Query8 incorrectly accepted')
    assert decode_finger(bytes.fromhex('00 00 00 00 00 00 00 00')) is None
    assert decode_finger(bytes.fromhex('01 11 02 8c 04 81 07 07')) == {
        'x': 529, 'y': 1164, 'pressure': 129, 'width_x': 7, 'width_y': 7}
    assert decode_finger(bytes.fromhex('01 11 02 8c 04 7c 08 06')) == {
        'x': 529, 'y': 1164, 'pressure': 124, 'width_x': 8, 'width_y': 6}
    for bad in (q[:7], q[:9] + bytes([0]) + q[10:], q[:9] + bytes([0x13]) + q[10:]):
        try:
            data1_address(0x49, 0x08, bad)
        except ValueError:
            pass
        else:
            raise AssertionError('invalid query was accepted')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--selftest', action='store_true')
    parser.add_argument('--query', help='hex bytes from F12 Query 0x49')
    parser.add_argument('--frame', help='hex bytes from F12 Data1 0x08')
    args = parser.parse_args()
    if args.selftest:
        selftest()
        print('F12 guarded parser tests passed')
    if args.query:
        print('Data1 address:', hex(data1_address(0x49, 0x08, bytes.fromhex(args.query))))
    if args.frame:
        print('contact:', decode_finger(bytes.fromhex(args.frame)))
