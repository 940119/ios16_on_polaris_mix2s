"""Synthetic command-db tests; no recovered DB image or device access."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from polaris_arc_db import CommandDbError
from polaris_bcm_db import parse_bcm, encode_vote


def mock_db():
    data = bytearray(256)
    struct.pack_into('<II', data, 0, 1, 0x0c0330db)
    struct.pack_into('<5H', data, 8, 5, 0, 48, 2, 1)
    for i, name in enumerate(('SH0', 'MC0')):
        p = 144 + 24 * i
        data[p:p + 8] = name.encode().ljust(8, b'\0')
        struct.pack_into('<IHH', data, p + 16, 0x50000 + 4 * i, 8, i * 8)
        struct.pack_into('<IHBB', data, 144 + 48 + i * 8, 1000, 32, i, 0)
    return data


class BcmTests(unittest.TestCase):
    def test_valid_metadata_not_vote(self):
        entries = parse_bcm(mock_db())
        self.assertEqual(entries['SH0'].address, 0x50000)
        self.assertEqual(entries['MC0'].width, 32)
        self.assertEqual(entries['MC0'].vcd, 1)

    def test_rejects_wrong_slave_and_truncated_aux(self):
        data = mock_db()
        struct.pack_into('<I', data, 144 + 16, 0x30000)
        with self.assertRaisesRegex(CommandDbError, 'invalid or duplicate'):
            parse_bcm(data)
        data = mock_db()
        struct.pack_into('<H', data, 144 + 20, 2)
        with self.assertRaisesRegex(CommandDbError, 'invalid or duplicate'):
            parse_bcm(data)
        data = mock_db()
        struct.pack_into('<H', data, 144 + 24 + 22, 0xff00)
        with self.assertRaisesRegex(CommandDbError, 'invalid or duplicate'):
            parse_bcm(data)

    def test_rejects_duplicate_and_bad_metadata(self):
        data = mock_db()
        data[168:176] = b'SH0\0\0\0\0\0'
        with self.assertRaisesRegex(CommandDbError, 'duplicate'):
            parse_bcm(data)
        data = mock_db()
        struct.pack_into('<I', data, 192, 0)
        with self.assertRaisesRegex(CommandDbError, 'metadata'):
            parse_bcm(data)

    def test_vote_bits_not_bus_aggregation(self):
        self.assertEqual(encode_vote(False, 0, 0), 0)
        self.assertEqual(encode_vote(True, 0, 0), 0x40000000)
        self.assertEqual(encode_vote(True, 1, 2), 0x60004002)
        self.assertEqual(encode_vote(False, 0x4000, 0x5000), 0x2fffffff)
        with self.assertRaises(CommandDbError):
            encode_vote(False, -1, 0)


if __name__ == '__main__':
    unittest.main()
