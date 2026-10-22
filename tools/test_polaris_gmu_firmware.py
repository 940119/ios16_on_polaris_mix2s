"""No-device regression checks for the A630 GMU flat image discriminator."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from polaris_gmu_firmware import (A630_GMU_SIZE, ITCM_IMAGE_BYTES, FirmwareFormatError,
                                  check_flat_a630_gmu)


def mock_flat(stack=0x43f80, entry=9):
    data = bytearray(A630_GMU_SIZE)
    struct.pack_into('<II', data, 0, stack, entry)
    return bytes(data)


class GmuFormatTests(unittest.TestCase):
    def test_flat_vector(self):
        self.assertEqual(check_flat_a630_gmu(mock_flat()), ITCM_IMAGE_BYTES)

    def test_wrong_size_and_stack(self):
        for data in (mock_flat()[:-1], mock_flat(stack=0x50000),
                     mock_flat(stack=0x43f81)):
            with self.assertRaises(FirmwareFormatError):
                check_flat_a630_gmu(data)

    def test_wrong_reset_vector(self):
        for entry in (0, 0x8001, 0xffffffff):
            with self.assertRaises(FirmwareFormatError):
                check_flat_a630_gmu(mock_flat(entry=entry))

    def test_redwood_typed_header_rejected(self):
        # Redwood's first record (address, size, type, value) is not an
        # A630 initial-stack/reset-vector pair.
        with self.assertRaises(FirmwareFormatError):
            check_flat_a630_gmu(mock_flat(stack=0, entry=16))


if __name__ == '__main__':
    unittest.main()
