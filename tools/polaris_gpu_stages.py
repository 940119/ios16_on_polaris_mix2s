#!/usr/bin/env python3
"""Fail-closed A630v2 headless bring-up contract (host-only, no MMIO).

This models required evidence and ownership transitions, not GPU power code.
An implementation must supply real board-specific backends with bounded readback;
no method here performs a register, RPMh, SMMU or SCM operation.
"""
from dataclasses import dataclass, field
from enum import Enum


class GateError(ValueError):
    pass


class Stage(Enum):
    BOARD = 'board'
    FIRMWARE = 'firmware'
    RAILS = 'rails'
    CLOCKS = 'clocks'
    BUS = 'bus'
    GMU = 'gmu'
    HFI = 'hfi'
    SMMU = 'smmu'
    PAS = 'pas'
    CP = 'cp'
    FENCE = 'fence'


STAGES = tuple(Stage)
# MiCode gpucc-sdm845.c v2 fixup, corroborated by recovery KGSL.
A630V2_KGSL_HZ = (257000000, 342000000, 414000000, 520000000,
                   596000000, 675000000, 710000000)
GPUCC = (0x05090000, 0x9000)
GPU = (0x05000000, 0x40000)
GMU = (0x0506a000, 0x30000)
PDC = (0x0b200000, 0x300000)
SMMU = (0x05040000, 0x10000)
# DT qcom,protect is an OFFSET relative to SMMU base, not a range within
# its 64KiB reg. It refers to CB5..CB7; do not map them from this contract.
HYP_PROTECT = (0x40000, 0xc000)
ZAP = (0x8c415000, 0x2000)
CX_GDSCR = 0x0509106c
GX_GDSCR = 0x0509100c
CX_HW_CTRL = 0x05091540


@dataclass(frozen=True)
class Evidence:
    polaris: bool
    sdm845: bool
    a630v2: bool
    frequencies_hz: tuple[int, ...]
    firmware_ok: bool
    zap_reserved: bool
    command_db_owned: bool
    bcm_votes_valid: bool
    pas13_ready: bool
    smmu_contexts_owned: bool


def check_evidence(evidence: Evidence) -> None:
    if not (evidence.polaris and evidence.sdm845 and evidence.a630v2):
        raise GateError('wrong board or GPU revision')
    if evidence.frequencies_hz != A630V2_KGSL_HZ:
        raise GateError('GPU frequency table is not device-observed A630v2')
    for name in ('firmware_ok', 'zap_reserved', 'command_db_owned',
                 'bcm_votes_valid', 'pas13_ready', 'smmu_contexts_owned'):
        if not getattr(evidence, name):
            raise GateError(f'unproven power/secure prerequisite: {name}')


@dataclass
class BringupLedger:
    """Track only resources actually acquired by this driver instance.

    On an unresponsive GMU/SMMU or unfinished CP fence, quarantine rather than
    force power-collapse shared rails or alter a HYP-protected context bank.
    """
    completed: list[Stage] = field(default_factory=list)
    owned: set[Stage] = field(default_factory=set)
    quarantined: bool = False

    def enter(self, stage: Stage, acquired: bool = False) -> None:
        if self.quarantined or len(self.completed) >= len(STAGES) or \
                stage != STAGES[len(self.completed)]:
            raise GateError('out-of-order stage or quarantined GPU')
        self.completed.append(stage)
        if acquired:
            self.owned.add(stage)

    def teardown_order(self, idle_confirmed: bool) -> tuple[Stage, ...]:
        if not idle_confirmed and any(stage in self.completed
                                      for stage in (Stage.GMU, Stage.HFI, Stage.CP)):
            self.quarantined = True
            raise GateError('GPU not idle: quarantine, do not collapse shared rails')
        return tuple(stage for stage in reversed(self.completed) if stage in self.owned)
