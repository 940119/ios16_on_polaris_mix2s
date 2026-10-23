import copy
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import adt
import mkadt
import polaris_cpu_topology as topology


class PolarisCPUTopologyTests(unittest.TestCase):
    def setUp(self):
        self.tree = mkadt.build(polaris=True, ram=6)
        self.cmdline = 'debug=0xc cpus=4 rd=ufsroot0'

    def check(self, tree=None, cmdline=None):
        return topology.validate(adt.serialize(self.tree if tree is None else tree),
                                 self.cmdline if cmdline is None else cmdline)

    def test_contiguous_prefix(self):
        for count in range(1, 9):
            with self.subTest(count=count):
                self.assertEqual(self.check(cmdline=f'cpus={count}'), count)

    def test_rejects_missing_duplicate_or_noncontiguous_selection(self):
        for args in ('', 'rd=ufsroot0', 'cpus=4 cpus=8', 'cpus=8 cpumask=255',
                     'cpus=0', 'cpus=9', 'cpus=foo'):
            with self.subTest(args=args), self.assertRaises(topology.TopologyError):
                self.check(cmdline=args)

    def test_rejects_sweet_and_qemu(self):
        for tree in (mkadt.build(), mkadt.build(qemu=True)):
            with self.assertRaises(topology.TopologyError):
                self.check(tree=tree)

    def test_rejects_wrong_mpidr_and_missing_cpu(self):
        tree = copy.deepcopy(self.tree)
        cpus = next(n for n in tree.children if n.name == 'cpus')
        cpu = cpus.children[4]
        cpu.props = [(n, f, mkadt.u32(0x500) if n == 'reg' else v)
                     for n, f, v in cpu.props]
        with self.assertRaisesRegex(topology.TopologyError, 'CPU 4'):
            self.check(tree=tree)
        tree = copy.deepcopy(self.tree)
        cpus = next(n for n in tree.children if n.name == 'cpus')
        cpus.children.pop()
        with self.assertRaisesRegex(topology.TopologyError, 'eight CPUs'):
            self.check(tree=tree)

    def test_rejects_wrong_board_and_trailing_adt(self):
        tree = copy.deepcopy(self.tree)
        tree.props = [(n, f, v.replace(b'qcom,sdm845\0', b'qcom,sm7150\0')
                       if n == 'compatible' else v) for n, f, v in tree.props]
        with self.assertRaises(topology.TopologyError):
            self.check(tree=tree)
        with self.assertRaisesRegex(topology.TopologyError, 'trailing'):
            topology.validate(adt.serialize(self.tree) + b'\0', self.cmdline)


if __name__ == '__main__':
    unittest.main()
