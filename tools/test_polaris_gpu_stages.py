"""Host tests: GPU bring-up cannot progress on guessed evidence/ownership."""
import sys
import unittest
from dataclasses import replace
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from polaris_gpu_stages import (A630V2_KGSL_HZ, BringupLedger, Evidence,
                                GateError, Stage, STAGES, check_evidence)


def valid_evidence():
    return Evidence(True, True, True, A630V2_KGSL_HZ, True, True,
                    True, True, True, True)


class PolarisStagesTests(unittest.TestCase):
    def test_requires_exact_revision_and_frequencies(self):
        check_evidence(valid_evidence())
        for changed in (dict(polaris=False), dict(sdm845=False),
                        dict(a630v2=False),
                        dict(frequencies_hz=(210000000, 280000000, 600000000)),
                        dict(frequencies_hz=A630V2_KGSL_HZ + (710000000,))):
            with self.assertRaises(GateError):
                check_evidence(replace(valid_evidence(), **changed))

    def test_every_unproven_prerequisite_refuses(self):
        for name in ('firmware_ok', 'zap_reserved', 'command_db_owned',
                     'bcm_votes_valid', 'pas13_ready', 'smmu_contexts_owned'):
            with self.subTest(name=name), self.assertRaisesRegex(GateError, name):
                check_evidence(replace(valid_evidence(), **{name: False}))

    def test_strict_order_and_owned_only_reverse(self):
        ledger = BringupLedger()
        with self.assertRaisesRegex(GateError, 'out-of-order'):
            ledger.enter(Stage.CLOCKS)
        for stage in STAGES:
            ledger.enter(stage, acquired=stage in (Stage.RAILS, Stage.BUS, Stage.SMMU))
        self.assertEqual(ledger.teardown_order(idle_confirmed=True),
                         (Stage.SMMU, Stage.BUS, Stage.RAILS))
        with self.assertRaisesRegex(GateError, 'out-of-order'):
            ledger.enter(Stage.FENCE)

    def test_failed_gpu_idle_quarantines(self):
        ledger = BringupLedger()
        for stage in STAGES[:6]:
            ledger.enter(stage, acquired=stage in (Stage.RAILS, Stage.GMU))
        with self.assertRaisesRegex(GateError, 'quarantine'):
            ledger.teardown_order(idle_confirmed=False)
        self.assertTrue(ledger.quarantined)
        with self.assertRaises(GateError):
            ledger.enter(Stage.HFI)


if __name__ == '__main__':
    unittest.main()
