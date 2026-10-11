#!/usr/bin/env python3
"""Check the 39-vertex decomposition CNF without a SAT solver."""
from pathlib import Path
import tempfile
import unittest
import ramsey


class EncodingTests(unittest.TestCase):
    def test_seed_and_decomposition(self):
        n,edges=ramsey.decode_graph6(ramsey.SEED.read_text())
        self.assertEqual(n,24)
        ramsey.verify_graph(n,edges,4,5)
        fixed,groups=ramsey.fixed_edges(39,mode='decomposition')
        self.assertEqual(list(map(len,groups)),[24,1,4,3,3,2,2])
        self.assertEqual(len(fixed),328)
        self.assertEqual(sorted(v for group in groups for v in group),list(range(39)))
        for v in range(24): self.assertTrue(fixed[v,24])
        for v in range(25,39): self.assertFalse(fixed[24,v])
        self.assertNotIn((25,29),fixed)

    def test_decomposition_formula_matches_graph_checker(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'decomposition.cnf'
            metadata=ramsey.generate(path,39,'decomposition')
            self.assertEqual((metadata['variables'],metadata['fixed_edges'],
                              metadata['free_edges'],metadata['clauses']),
                             (741,328,413,149994))
            clauses=[list(map(int,line.split()[:-1])) for line in path.read_text().splitlines()
                     if not line.startswith(('c','p'))]
            fixed,_=ramsey.fixed_edges(39,mode='decomposition')
            variables=ramsey.edge_variables(39)
            free=[e for e in variables if e not in fixed]
            for choice in [dict.fromkeys(free,True),dict.fromkeys(free,False)]:
                assignment=dict(fixed)
                assignment.update(choice)
                values={variables[e]:value for e,value in assignment.items()}
                cnf_sat=all(any(values[abs(l)]==(l>0) for l in clause) for clause in clauses)
                try:
                    ramsey.verify_graph(39,{e for e,value in assignment.items() if value})
                    graph_sat=True
                except ValueError: graph_sat=False
                self.assertEqual(cnf_sat,graph_sat)
                self.assertFalse(cnf_sat)

    def test_bad_graph6_rejected(self):
        for text in ['', 'W?', '~AAAA', 'W'+chr(200)*46]:
            with self.assertRaises(ValueError): ramsey.decode_graph6(text)

if __name__=='__main__': unittest.main(verbosity=2)
