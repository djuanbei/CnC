#!/usr/bin/env python3
"""Completed 39-vertex SAT control using both real CnC backends."""
from pathlib import Path
import subprocess
import tempfile
import unittest
import ramsey

ROOT = Path(__file__).resolve().parents[2]
CNC = ROOT / 'orchestrator' / 'cnc'


class KnownGraphTests(unittest.TestCase):
    def test_induced_39_vertex_witness(self):
        n, edges = ramsey.decode_graph6(ramsey.KNOWN_42.read_text())
        self.assertEqual(n, 42)
        ramsey.verify_graph(n, edges, 5, 5)
        ramsey.verify_graph(39, {e for e in edges if e[1] < 39}, 5, 5)

    def test_real_backends_find_and_validate_39_vertex_graph(self):
        with tempfile.TemporaryDirectory(prefix='cnc-r55-control-') as directory:
            path = Path(directory) / 'control.cnf'
            metadata = ramsey.generate(path, 39, 'known-39-control')
            self.assertEqual(metadata['free_edges'], 180)
            for backend in ['iglucose', 'ilingeling']:
                for flags in [[], ['--symmetry', '--deduplicate']]:
                    with self.subTest(backend=backend, flags=flags):
                        result = subprocess.run([str(CNC), str(path), '--backend', backend,
                            '--attempt-seconds', '10', *flags], capture_output=True,
                            text=True, timeout=30)
                        self.assertEqual(result.returncode, 10, result.stdout + result.stderr)
                        self.assertIn('"attempts":1', result.stderr)
                        ramsey.verify_model(result.stdout, 39, 'known-39-control')


if __name__ == '__main__':
    unittest.main(verbosity=2)
