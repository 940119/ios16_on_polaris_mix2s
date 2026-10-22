"""Host-only tests of MiCode SDM845v2 GPU bandwidth case contracts."""
import sys
import unittest
from dataclasses import replace
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from polaris_bw_levels import (BandwidthTableError, CNOC_IB_KBPS, DDR_IB_KBPS,
                               V2_CHIPID, V2_GPU_LEVELS, V2_INITIAL_LEVEL,
                               validate, validate_query_shape)


class BandwidthCasesTests(unittest.TestCase):
    def test_v2_cases_and_initial_level(self):
        validate(V2_CHIPID, V2_INITIAL_LEVEL, V2_GPU_LEVELS,
                 DDR_IB_KBPS, CNOC_IB_KBPS)
        self.assertEqual(V2_GPU_LEVELS[5].bus_case, 6)  # v1 would be 5
        self.assertEqual(V2_GPU_LEVELS[6].bus_case, 4)

    def test_rejects_v1_or_wrong_indices(self):
        for kwargs in (dict(versions=0x06030000), dict(initial_level=5),
                       dict(gpu_levels=V2_GPU_LEVELS[:5] +
                            (replace(V2_GPU_LEVELS[5], bus_case=5),) + V2_GPU_LEVELS[6:]),
                       dict(ddr_cases=DDR_IB_KBPS[:-1]),
                       dict(cnoc_cases=(0, 200))):
            args = dict(versions=V2_CHIPID, initial_level=V2_INITIAL_LEVEL,
                        gpu_levels=V2_GPU_LEVELS, ddr_cases=DDR_IB_KBPS,
                        cnoc_cases=CNOC_IB_KBPS)
            args.update(kwargs)
            with self.subTest(kwargs=kwargs), self.assertRaises(BandwidthTableError):
                validate(**args)

    def test_rejects_missing_oversized_or_nonuniform_query(self):
        validate_query_shape([[0]] * 13, [[0]] * 2)
        for ddr, cnoc in (([[0]] * 12, [[0]] * 2),
                          ([[0] * 9] * 13, [[0]] * 2),
                          ([[0], [0, 1]] + [[0]] * 11, [[0]] * 2),
                          ([[0]] * 13, [[]] * 2)):
            with self.assertRaises(BandwidthTableError):
                validate_query_shape(ddr, cnoc)


if __name__ == '__main__':
    unittest.main()
