"""Host-only tests for the fail-closed Polaris ARC command-db decoder."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from polaris_arc_db import CommandDbError, NAMES, parse_arc


def sample_db():
    data = bytearray(400)
    struct.pack_into('<II', data, 0, 1, 0x0c0330db)
    # ARC slave 3; entry and auxiliary offsets relative to 144-byte header.
    struct.pack_into('<5H', data, 8, 3, 0, 72, 3, 0x100)
    for i, name in enumerate(NAMES):
        p = 144 + 24 * i
        data[p:p + 8] = name.encode().ljust(8, b'\0')
        struct.pack_into('<IIIHH', data, p + 8, 4, 0, 0x30000 + i * 4, 8, i * 8)
        struct.pack_into('<4H', data, 144 + 72 + i * 8, 0x10, 0x20, 0x30, 0)
    return data


class ArcDbTests(unittest.TestCase):
    def test_valid(self):
        items = parse_arc(sample_db())
        self.assertEqual(set(items), set(NAMES))
        self.assertEqual(items['gfx.lvl']['levels'], [16, 32, 48])
        self.assertEqual(items['mx.lvl']['address'], 0x30008)

    def test_truncated_aux(self):
        data = sample_db()
        struct.pack_into('<H', data, 144 + 20, 200)
        with self.assertRaisesRegex(CommandDbError, 'invalid ARC'):
            parse_arc(data)

    def test_wrong_slave_and_duplicate(self):
        data = sample_db()
        struct.pack_into('<H', data, 8, 4)
        with self.assertRaisesRegex(CommandDbError, 'invalid ARC'):
            parse_arc(data)
        data = sample_db()
        data[144 + 24:144 + 32] = b'gfx.lvl\0'
        with self.assertRaisesRegex(CommandDbError, 'duplicate'):
            parse_arc(data)

    def test_bad_header_and_missing_resource(self):
        data = sample_db()
        struct.pack_into('<H', data, 8 + 2, 0xfff0)
        with self.assertRaisesRegex(CommandDbError, 'malformed'):
            parse_arc(data)
        data = sample_db()
        struct.pack_into('<H', data, 8 + 6, 2)
        with self.assertRaisesRegex(CommandDbError, 'missing ARC'):
            parse_arc(data)

    def test_standalone_and_version(self):
        data = sample_db()
        struct.pack_into('<I', data, 140, 1)
        with self.assertRaisesRegex(CommandDbError, 'standalone'):
            parse_arc(data)
        data = sample_db()
        struct.pack_into('<I', data, 0, 2)
        with self.assertRaisesRegex(CommandDbError, 'version'):
            parse_arc(data)


if __name__ == '__main__':
    unittest.main()
