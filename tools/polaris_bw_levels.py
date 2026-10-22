#!/usr/bin/env python3
"""Offline SDM845v2 GPU/GMU bandwidth case alignment checks.

These are vendor-DTS case indices and KB/s vectors, not BCM votes or TCS
commands. Real HFI rows require the kernel bus graph/command-db query output.
"""
from dataclasses import dataclass

DDR_IB_KBPS = (0, 400000, 600000, 800000, 1200000, 1648000, 2188000,
               2724000, 3072000, 4068000, 5184000, 6220000, 7216000)
CNOC_IB_KBPS = (0, 100)
DDR_PATH = (26, 512)
CNOC_PATH = (26, 10036)
MAX_GX_LEVELS = 16
MAX_CNOC_LEVELS = 2
MAX_BW_CMDS = 8
MAX_CNOC_CMDS = 6
V2_CHIPID = 0x06030001
V2_INITIAL_LEVEL = 6


class BandwidthTableError(ValueError):
    pass


@dataclass(frozen=True)
class PwrLevel:
    frequency_hz: int
    bus_case: int
    minimum: int
    maximum: int


# MiCode sdm845-v2.dtsi &msm_gpu power levels in vendor order (fast -> off).
V2_GPU_LEVELS = (
    PwrLevel(710000000, 12, 12, 12), PwrLevel(675000000, 12, 10, 12),
    PwrLevel(596000000, 10, 9, 12), PwrLevel(520000000, 9, 8, 11),
    PwrLevel(414000000, 8, 7, 9), PwrLevel(342000000, 6, 5, 7),
    PwrLevel(257000000, 4, 3, 5), PwrLevel(0, 0, 0, 0),
)
V2_GMU_HZ = (0, 200000000, 500000000)


def validate(versions, initial_level, gpu_levels, ddr_cases, cnoc_cases,
             ddr_path=DDR_PATH, cnoc_path=CNOC_PATH):
    """Check exact known v2 mapping and HFI array bounds; never synthesize votes."""
    if versions != V2_CHIPID or initial_level != V2_INITIAL_LEVEL:
        raise BandwidthTableError('SDM845v2 GPU chipid or initial level mismatch')
    if tuple(gpu_levels) != V2_GPU_LEVELS:
        raise BandwidthTableError('GPU powerlevel frequencies or bus case mapping mismatch')
    if tuple(ddr_cases) != DDR_IB_KBPS or tuple(cnoc_cases) != CNOC_IB_KBPS or \
            ddr_path != DDR_PATH or cnoc_path != CNOC_PATH:
        raise BandwidthTableError('DDR/CNOC cases or path mismatch')
    if len(ddr_cases) > MAX_GX_LEVELS or len(cnoc_cases) > MAX_CNOC_LEVELS:
        raise BandwidthTableError('HFI level capacity exceeded')
    for level in gpu_levels:
        if not 0 <= level.minimum <= level.bus_case <= level.maximum < len(ddr_cases):
            raise BandwidthTableError('GPU bus case out of range')
    if gpu_levels[initial_level].frequency_hz != 257000000 or \
            gpu_levels[initial_level].bus_case != 4:
        raise BandwidthTableError('incorrect initial GPU bus case')


def validate_query_shape(ddr_rows, cnoc_rows):
    """Validate bounded *query output* shape, not contents or hardware ownership."""
    if len(ddr_rows) != len(DDR_IB_KBPS) or len(cnoc_rows) != len(CNOC_IB_KBPS):
        raise BandwidthTableError('query row count differs from vendor cases')
    for rows, max_cmds in ((ddr_rows, MAX_BW_CMDS), (cnoc_rows, MAX_CNOC_CMDS)):
        if not rows or not 0 < len(rows[0]) <= max_cmds or \
                any(len(row) != len(rows[0]) for row in rows):
            raise BandwidthTableError('nonuniform or oversized queried BCM command rows')
