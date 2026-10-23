#!/usr/bin/env python3
"""Fail-closed Polaris CPU prefix check before building a temporary payload.

Validates the serialized Apple Device Tree, not a claim that CPU_ON succeeds.
The 4 A55 + 4 A75 topology must still be tested on the phone in order.
"""

import argparse
import struct

from adt import load, parse


class TopologyError(ValueError):
    pass


def _property(node, name):
    value = node.prop(name)
    if value is None or len(value) != 4:
        raise TopologyError(f"{node.name}: missing/invalid {name}")
    return struct.unpack('<I', value)[0]


def _child(parent, name):
    matches = [child for child in parent.children if child.name == name]
    if len(matches) != 1:
        raise TopologyError(f"expected exactly one {name} node")
    return matches[0]


def validate(adt_bytes, cmdline):
    """Return the selected contiguous prefix, refusing ambiguous CPU boot args."""
    tree, end = parse(adt_bytes)
    if end != len(adt_bytes):
        raise TopologyError('trailing ADT bytes')
    tokens = cmdline.split()
    selections = [t for t in tokens if t.startswith('cpus=')]
    if len(selections) != 1 or any(t.startswith('cpumask=') for t in tokens):
        raise TopologyError('supply exactly one cpus=N, without cpumask')
    try:
        count = int(selections[0][5:])
    except ValueError as exc:
        raise TopologyError('invalid cpus=N') from exc
    if not 1 <= count <= 8:
        raise TopologyError('Polaris cpus must be in 1..8')
    chosen = _child(tree, 'chosen')
    # A board name alone is insufficient: require the exact dual identity
    # populated by mkadt --polaris, checked here without performing MMIO.
    compatible = tree.prop('compatible')
    if not compatible or b'xiaomi,polaris\0' not in compatible or b'qcom,sdm845\0' not in compatible:
        raise TopologyError('not a Polaris/SDM845 ADT')
    cpus = _child(tree, 'cpus')
    if len(cpus.children) != 8:
        raise TopologyError('Polaris ADT must describe eight CPUs')
    for index, cpu in enumerate(cpus.children):
        if cpu.name != f'cpu{index}' or _property(cpu, 'reg') != index << 8 or _property(cpu, 'cpu-id') != index:
            raise TopologyError(f'CPU {index} ID/MPIDR mismatch')
        if cpu.prop('state') != (b'running\0' if index == 0 else b'stopped\0'):
            raise TopologyError(f'CPU {index} state mismatch')
    # Require expected chosen node to guard accidental QEMU/sweet tree use.
    if not chosen.prop('name'):
        raise TopologyError('missing chosen name')
    return count


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('adt')
    ap.add_argument('--cmdline', required=True)
    args = ap.parse_args()
    try:
        count = validate(load(args.adt), args.cmdline)
    except (TopologyError, ValueError, IndexError, struct.error) as exc:
        ap.error(str(exc))
    print(f'Polaris ADT prefix: {count}/8 selected; '
          f'{max(0, count - 4)} A75 requested (NOT phone-verified)')


if __name__ == '__main__':
    main()
