#!/usr/bin/env python3
"""End-to-end decision oracle and checkpoint/lifecycle regression tests."""
import hashlib
import itertools
import json
import os
from pathlib import Path
import random
import signal
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
CNC = Path(os.environ.get('CNC_BIN', ROOT / 'orchestrator/cnc'))


def call(*args):
    return subprocess.run([str(CNC), *map(str, args)], capture_output=True, text=True, timeout=30)


def latest(directory):
    commits = sorted(directory.glob('commit-*.json'), key=lambda p: int(p.stem.split('-')[1]))
    manifest = json.loads(commits[-1].read_text())
    return json.loads((directory / manifest['file']).read_text())


def sat(n, clauses):
    return any(all(any(values[abs(l)-1] == (l > 0) for l in c) for c in clauses)
               for values in itertools.product([False, True], repeat=n))


class SolverTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='cnc-tests-')
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)
        self.input = self.dir / 'input.cnf'
        self.cp = self.dir / 'checkpoint'
        self.fake = self.dir / 'slow-worker'
        self.fake.write_text('#!/usr/bin/env python3\nimport os, time\nparent=os.getppid()\nfor _ in range(1200):\n    if os.getppid()!=parent: break\n    time.sleep(.05)\n')
        self.fake.chmod(0o755)
        # Deterministic branch selection only for scheduler-state fixtures.
        self.branch = self.dir / 'decision-worker'
        self.branch.write_text('#!/usr/bin/env python3\nimport sys\na=[abs(int(x)) for l in open(sys.argv[1]) if not l.startswith(("p","c")) for x in l.split() if int(x)]\nprint("b",min(a))\n')
        self.branch.chmod(0o755)

    def formula(self, n, clauses):
        self.input.write_text(f'p cnf {n} {len(clauses)}\n' + ''.join(' '.join(map(str, c))+' 0\n' for c in clauses))

    def expect(self, result, code):
        self.assertEqual(result.returncode, code, result.stdout + result.stderr)

    def test_oracle_both_backends(self):
        rng = random.Random(42)
        fixtures = [(0, []), (1, [[]]), (3, [[1], [-1]]), (3, [[1, -1]])]
        for _ in range(25):
            n = rng.randint(2, 6)
            fixtures.append((n, [[rng.choice([-1, 1])*rng.randint(1,n) for _ in range(rng.randint(1,3))]
                                 for _ in range(rng.randint(1,20))]))
        for backend in ['iglucose', 'ilingeling']:
            for n, clauses in fixtures:
                self.formula(n, clauses)
                for flags in [[], ['--symmetry'], ['--deduplicate'], ['--symmetry', '--deduplicate']]:
                    result = call(self.input, '--backend', backend, *flags)
                    self.expect(result, 10 if sat(n, clauses) else 20)

    def test_recursive_orbital_and_ordinary(self):
        self.formula(3, [[1, 2], [1, 3], [2, 3]])
        for flags in [[], ['--symmetry']]:
            result = call(self.input, '--solver', self.fake, '--attempt-seconds', '.02', '--workers', 2, *flags)
            self.expect(result, 10)
            self.assertIn('"event":"split"', result.stderr)
            if flags:
                self.assertIn('"orbital_splits":', result.stderr)

    def test_deep_split_result_reaches_root(self):
        # Exclude every assignment except all-true. Unit propagation cannot
        # finish this formula until the branch path is several levels deep.
        n = 6
        clauses = [[-(i + 1) if mask & (1 << i) else i + 1 for i in range(n)]
                   for mask in range(1 << n) if mask != (1 << n) - 1]
        self.formula(n, clauses)
        result = call(self.input, '--solver', self.fake, '--march', self.branch,
                      '--lookahead-seconds', 5, '--attempt-seconds', '.02',
                      '--workers', 1)
        self.expect(result, 10)
        self.assertGreaterEqual(result.stderr.count('"event":"split"'), n - 2)

    def test_resume_matrix(self):
        self.formula(3,[[1,2],[-1,-2],[2,3],[-2,-3]])
        for backend in ['iglucose','ilingeling']:
            for workers in [1,2]:
                for index,flags in enumerate([[],['--symmetry'],['--deduplicate'],['--symmetry','--deduplicate']]):
                    cp=self.dir/f'{backend}-{workers}-{index}'
                    self.expect(call(self.input,'--backend',backend,'--workers',workers,
                                     '--checkpoint-dir',cp,*flags),10)
                    self.expect(call('--resume',cp),10)

    def test_completed_resume_without_input(self):
        self.formula(3, [[1,2], [-1,-2]])
        result = call(self.input, '--checkpoint-dir', self.cp, '--deduplicate')
        self.expect(result, 10)
        state = latest(self.cp)
        self.input.unlink()
        result = call('--resume', self.cp)
        self.expect(result, 10)
        self.assertNotIn('"event":"dispatch"', result.stderr)
        self.assertEqual(latest(self.cp)['counters']['attempts'], state['counters']['attempts'])

    def test_pause_resume_limits_and_lock(self):
        self.formula(3, [[1,2], [1,3], [2,3]])
        proc = subprocess.Popen([str(CNC), str(self.input), '--solver', str(self.fake),
            '--checkpoint-dir', str(self.cp), '--attempt-seconds', '.15', '--max-depth', '0',
            '--checkpoint-interval', '.02'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(lambda: proc.poll() is None and proc.kill())
        deadline = time.monotonic()+10
        while not list(self.cp.glob('commit-*.json')) and time.monotonic()<deadline:
            time.sleep(.01)
        self.expect(call('--resume', self.cp), 2)
        proc.send_signal(signal.SIGINT)
        stdout, stderr = proc.communicate(timeout=10)
        self.assertEqual(proc.returncode, 3, stdout+stderr)
        self.assertEqual(latest(self.cp)['outcome'], 'PAUSED')
        result = call('--resume', self.cp)
        self.expect(result, 0)
        self.assertEqual(latest(self.cp)['cubes'][0]['block'], 'split-limit')
        result = call('--resume', self.cp, '--max-depth', '10')
        self.expect(result, 10)

    def test_corrupt_latest_recovery_and_mismatch(self):
        self.formula(2, [[1,2], [-1,-2]])
        self.expect(call(self.input, '--checkpoint-dir', self.cp), 10)
        (self.cp/'state-2.json').write_text('truncated')
        result = call('--resume', self.cp)
        self.expect(result, 10)
        self.assertIn('trying previous generation', result.stderr)
        self.input.write_text('p cnf 0 0\n')
        self.expect(call(self.input, '--resume', self.cp), 2)
        self.expect(call('--resume', self.cp, '--attempt-seconds', '1'), 2)
        self.expect(call('--resume', self.cp, '--max-cubes', '1'), 2)

    def test_worker_errors_not_unsat(self):
        self.formula(2, [[1,2], [-1,-2]])
        self.fake.write_text('#!/usr/bin/env python3\nprint("s SATISFIABLE\\nv 1 2 0")\n')
        result = call(self.input, '--solver', self.fake, '--checkpoint-dir', self.cp)
        self.expect(result, 0)
        self.assertNotIn('s UNSATISFIABLE', result.stdout)
        self.assertEqual(latest(self.cp)['cubes'][0]['state'], 'UNRESOLVED')
        self.assertEqual(latest(self.cp)['counters']['attempts'], 2)

    def test_equivalent_siblings_sat_and_unsat(self):
        self.fake.write_text("""#!/usr/bin/env python3
import itertools, sys, time
clauses=[]; assumptions=[]
for row in open(sys.argv[1]):
    if row.startswith('p'): continue
    if row.startswith('a'):
        assumptions=list(map(int,row.split()[1:-1])); continue
    clauses.append(list(map(int,row.split()[:-1])))
if not assumptions: time.sleep(2)
else: time.sleep(.03)
n=max(abs(x) for c in clauses for x in c)
for bits in itertools.product([-1,1],repeat=n):
    if all(bits[abs(x)-1]==(1 if x>0 else -1) for x in assumptions) and all(any(bits[abs(x)-1]==(1 if x>0 else -1) for x in c) for c in clauses):
        print('s SATISFIABLE');print('v', *[(i+1)*v for i,v in enumerate(bits)],0);break
else: print('s UNSATISFIABLE')
""")
        for unsat in [False, True]:
            clauses=[[1,2,3],[-1,4,5]]
            if unsat:
                clauses=[[1,a,b] for a in [2,-2] for b in [3,-3]] + [[-1,a,b] for a in [4,-4] for b in [5,-5]]
            self.formula(5,clauses)
            cp=self.dir/('unsat-cp' if unsat else 'sat-cp')
            result=call(self.input,'--solver',self.fake,'--march',self.branch,'--lookahead-seconds',5,'--deduplicate','--workers',2,
                        '--attempt-seconds','.12','--checkpoint-dir',cp)
            self.expect(result,20 if unsat else 10)
            state=latest(cp)
            self.assertGreater(state['counters'].get('duplicates_avoided',0),0,result.stderr)
            self.expect(call('--resume',cp),20 if unsat else 10)

    def test_analysis_budget_and_zero_index(self):
        self.formula(3,[[1,2],[1,3],[2,3]])
        result=call(self.input,'--solver',self.fake,'--march',self.branch,'--lookahead-seconds',5,'--symmetry','--deduplicate',
                    '--attempt-seconds','.02','--symmetry-seconds','.00000001',
                    '--canonical-seconds','.00000001','--index-bytes',0)
        self.expect(result,10)
        self.assertNotIn('preprocessing',result.stdout)

    def test_structural_corruption_rejected(self):
        self.formula(2,[[1,2],[-1,-2]])
        self.expect(call(self.input,'--checkpoint-dir',self.cp),10)
        state=latest(self.cp)
        state['cubes'][0]['children']=[0,0]
        path=self.cp/'state-2.json'
        path.write_text(json.dumps(state))
        manifest=json.loads((self.cp/'commit-2.json').read_text())
        manifest['sha256']=hashlib.sha256(path.read_bytes()).hexdigest()
        (self.cp/'commit-2.json').write_text(json.dumps(manifest))
        self.expect(call('--resume',self.cp),2)

    def test_checkpoint_publication_faults(self):
        fault_binary=ROOT/'orchestrator/cnc_fault'
        self.formula(2,[[1,2],[-1,-2]])
        for point in ['before-file-flush','before-state-publish','after-state-publish',
                      'before-manifest-publish','after-manifest-publish','after-result']:
            cp=self.dir/point
            env=dict(os.environ,CNC_FAULT_POINT=point,CNC_FAULT_AT='1' if point=='after-result' else ('4' if point=='before-file-flush' else '2'))
            result=subprocess.run([str(fault_binary),str(self.input),'--checkpoint-dir',str(cp)],env=env,capture_output=True,text=True,timeout=10)
            self.expect(result,99)
            self.expect(call('--resume',cp),10)

    def test_split_and_alias_crash_boundaries(self):
        self.formula(5,[[1,2,3],[-1,4,5]])
        for point in ['during-split','after-alias']:
            cp=self.dir/point
            env=dict(os.environ,CNC_FAULT_POINT=point)
            result=subprocess.run([str(ROOT/'orchestrator/cnc_fault'),str(self.input),
                    '--solver',str(self.fake),'--march',str(self.branch),'--lookahead-seconds','5','--checkpoint-dir',str(cp),'--checkpoint-interval','.01',
                    '--attempt-seconds','.06','--deduplicate','--workers','2'],env=env,
                    capture_output=True,text=True,timeout=15)
            self.expect(result,99)
            self.expect(call('--resume',cp),10)

    def test_failed_pause_does_not_report_success(self):
        self.formula(2,[[1,2],[-1,-2]])
        env=dict(os.environ,CNC_FAULT_POINT='storage-write',CNC_FAULT_AT='4',CNC_FAULT_THROW='1')
        proc=subprocess.Popen([str(ROOT/'orchestrator/cnc_fault'),str(self.input),'--solver',str(self.fake),
                    '--checkpoint-dir',str(self.cp)],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        self.addCleanup(lambda: proc.poll() is None and proc.kill())
        deadline=time.monotonic()+10
        while not list(self.cp.glob('commit-*.json')) and time.monotonic()<deadline:time.sleep(.01)
        proc.send_signal(signal.SIGINT)
        out,err=proc.communicate(timeout=10)
        self.assertEqual(proc.returncode,2,out+err)
        self.assertNotIn('c PAUSED;',err)

    def test_initial_storage_failure(self):
        self.formula(2,[[1,2]])
        env=dict(os.environ,CNC_FAULT_POINT='storage-write',CNC_FAULT_THROW='1')
        result=subprocess.run([str(ROOT/'orchestrator/cnc_fault'),str(self.input),
                    '--checkpoint-dir',str(self.cp)],env=env,capture_output=True,text=True,timeout=10)
        self.expect(result,2)
        self.assertNotIn('"event":"dispatch"',result.stderr)
        self.assertFalse(list(self.cp.glob('commit-*.json')))

    def test_late_timeout_output_ignored(self):
        self.formula(3,[[1,2],[1,3],[2,3]])
        self.fake.write_text("""#!/usr/bin/env python3
import signal, time, sys
def cancelled(*args):
    print('s UNSATISFIABLE', flush=True)
    sys.exit(0)
signal.signal(signal.SIGTERM,cancelled)
time.sleep(60)
""")
        result=call(self.input,'--solver',self.fake,'--march',self.branch,'--lookahead-seconds',5,'--attempt-seconds','.05')
        self.expect(result,10)
        self.assertIn('"event":"timeout"',result.stderr)

    def test_malformed_unsat_output_rejected(self):
        self.formula(2,[[1,2],[-1,-2]])
        self.fake.write_text('#!/usr/bin/env python3\nprint("s UNSATISFIABLE garbage")\n')
        result=call(self.input,'--solver',self.fake)
        self.expect(result,0)
        self.assertNotIn('s UNSATISFIABLE',result.stdout)

    def test_saved_generator_rejected(self):
        self.formula(3,[[1,2],[1,3],[2,3]])
        self.expect(call(self.input,'--solver',self.fake,'--march',self.branch,'--lookahead-seconds',5,'--symmetry','--attempt-seconds','.03',
                         '--checkpoint-dir',self.cp),10)
        commits=sorted(self.cp.glob('commit-*.json'),key=lambda p:int(p.stem.split('-')[1]))
        manifest=json.loads(commits[-1].read_text());file=self.cp/manifest['file']
        state=json.loads(file.read_text())
        self.assertTrue(state['cubes'][0]['generators'])
        state['cubes'][0]['generators'][0]=[1,1,1]
        file.write_text(json.dumps(state));manifest['sha256']=hashlib.sha256(file.read_bytes()).hexdigest()
        commits[-1].write_text(json.dumps(manifest))
        self.expect(call('--resume',self.cp),2)

    def test_invalid_options(self):
        self.formula(0, [])
        for args in [ ['--workers','0'], ['--attempt-seconds','nan'], ['--checkpoint-interval','1'],
                     ['--proof','x'], ['--max-depth','-1'], ['--seed','-1'] ]:
            self.expect(call(self.input, *args), 2)

    def test_crash_running_resume(self):
        self.formula(3, [[1,2],[1,3],[2,3]])
        proc = subprocess.Popen([str(CNC), str(self.input), '--solver', str(self.fake),
            '--checkpoint-dir', str(self.cp), '--checkpoint-interval', '.01', '--attempt-seconds', '.2'],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        self.addCleanup(lambda: proc.poll() is None and proc.kill())
        deadline = time.monotonic()+10
        while time.monotonic()<deadline:
            try:
                if latest(self.cp)['cubes'][0]['state']=='RUNNING': break
            except (IndexError, FileNotFoundError): pass
            time.sleep(.01)
        else: self.fail('no running checkpoint')
        proc.kill(); proc.wait()
        self.expect(call('--resume', self.cp), 10)


    def test_interrupted_real_backend_states(self):
        for unsat in [False, True]:
            clauses = ([[1,a,b] for a in [2,-2] for b in [3,-3]] +
                       [[-1,a,b] for a in [4,-4] for b in [5,-5]]) if unsat else [[1,2,3],[-1,4,5]]
            self.formula(5,clauses)
            for backend in ['iglucose', 'ilingeling']:
                for state in ['QUEUED','RUNNING','PENDING_SPLIT','SPLIT',
                              'WAITING_EQUIVALENT','UNRESOLVED']:
                    with self.subTest(backend=backend, state=state, unsat=unsat):
                        cp = self.dir / (backend + state + str(unsat))
                        args = [str(ROOT/'orchestrator/cnc_fault'), str(self.input),
                                '--backend', backend, '--march', str(self.branch), '--lookahead-seconds', '5',
                                '--checkpoint-dir', str(cp), '--attempt-seconds', '.15',
                                '--workers', '2', '--deduplicate']
                        if state == 'UNRESOLVED': args += ['--max-depth', '0']
                        env = dict(os.environ, CNC_TEST_SNAPSHOT_STATE=state,
                                   CNC_TEST_STOP_ATTEMPTS='2')
                        result = subprocess.run(args, env=env, capture_output=True,
                                                text=True, timeout=15)
                        self.expect(result, 99)
                        saved = latest(cp)
                        self.assertIn(state, [c['state'] for c in saved['cubes']])
                        if state == 'SPLIT':
                            for c in saved['cubes'][1:]:
                                self.assertEqual(len(c['propagated']), 6)
                                for literal in c['fixed']:
                                    self.assertEqual(c['propagated'][abs(literal)],
                                                     1 if literal > 0 else -1)
                        self.expect(call('--resume',cp,*(['--max-depth',10] if state=='UNRESOLVED' else [])),20 if unsat else 10)
                        final = latest(cp)
                        self.assertLessEqual(final['counters']['peak_workers'],2)
                        self.assertLessEqual(final['counters']['index_bytes_charged'],67108864)

    def test_real_march_terminal_and_branch_protocol(self):
        for n,clauses,expected in [(3,[[1,2],[1,3],[2,3]],10),
                 (2,[[1,2],[-1,2],[1,-2],[-1,-2]],20)]:
            self.formula(n,clauses)
            result=call(self.input,'--solver',self.fake,'--attempt-seconds','.03',
                        '--lookahead-seconds',5)
            self.expect(result,expected)
            self.assertNotIn('lookahead-unavailable',result.stderr)

    def test_failed_lookahead_stays_unresolved(self):
        self.formula(3, [[1,2],[1,3],[2,3]])
        for source in ['raise SystemExit(1)', 'print("b 999")',
                       'import time; time.sleep(2)']:
            self.branch.write_text('#!/usr/bin/env python3\n' + source + '\n')
            result = call(self.input,'--solver',self.fake,'--march',self.branch,
                          '--attempt-seconds','.02','--lookahead-seconds','.05')
            self.expect(result,0)
            self.assertIn('lookahead-unavailable', result.stderr)
            self.assertNotIn('"event":"split"', result.stderr)

    def test_dependency_failures_and_resource_limits(self):
        self.formula(3, [[1,2],[1,3],[2,3]])
        for dependency in ['symmetry','canonical']:
            cp = self.dir / dependency
            result = subprocess.run([str(ROOT/'orchestrator/cnc_fault'), str(self.input),
                    '--solver',str(self.fake),'--march',str(self.branch),'--lookahead-seconds','5','--symmetry',
                    '--deduplicate','--attempt-seconds','.05','--checkpoint-dir',str(cp)],
                    env=dict(os.environ,CNC_TEST_ANALYSIS_FAILURE=dependency),
                    capture_output=True,text=True,timeout=15)
            self.expect(result,10)
            counters=latest(cp)['counters']
            self.assertGreater(counters.get('lookahead_splits' if dependency=='symmetry'
                                            else 'canonical_unavailable',0),0)
        result=call(self.input,'--solver',self.fake,'--march',self.branch,'--lookahead-seconds',5,
                    '--attempt-seconds','.02','--max-cubes',1,'--checkpoint-dir',self.cp,
                    '--deduplicate','--index-bytes',1)
        self.expect(result,0)
        saved=latest(self.cp)
        self.assertEqual(len(saved['cubes']),1)
        self.assertEqual(saved['cubes'][0]['block'],'split-limit')
        self.assertEqual(saved['counters']['index_bytes_charged'],0)
        self.assertGreater(saved['counters']['index_full'],0)
        self.expect(call('--resume',self.cp,'--max-cubes',15),10)

    def test_representative_failure_releases_dependents(self):
        self.formula(5, [[1,2,3],[-1,4,5]])
        self.fake.write_text("""#!/usr/bin/env python3
import sys,time
row=[l for l in open(sys.argv[1]) if l.startswith('a')][0]
a=list(map(int,row.split()[1:-1]))
if not a: time.sleep(2)
elif a[0]>0: time.sleep(.08); sys.exit(2)
else: print('s SATISFIABLE'); print('v -1 2 3 4 5 0')
""")
        result=call(self.input,'--solver',self.fake,'--march',self.branch,'--lookahead-seconds',5,
                    '--attempt-seconds','.2','--workers',2,'--deduplicate',
                    '--retries',0,'--checkpoint-dir',self.cp)
        self.expect(result,10)
        self.assertGreater(latest(self.cp)['counters']['failed_representatives_released'],0)

    def test_legacy_scripts_outside_repository(self):
        path=self.dir/'formula with spaces.cnf'
        for clauses, expected in [([[1,2],[-1,-2]],'s SATISFIABLE'),
                                  ([[1,2],[-1,2],[1,-2],[-1,-2]],'s UNSATISFIABLE')]:
            self.formula(2,clauses)
            path.write_bytes(self.input.read_bytes())
            for script in ['cube-glucose.sh','cube-lingeling.sh']:
                result=subprocess.run([str(ROOT/script),str(path),'-d','1'],
                    cwd=self.dir, env=dict(os.environ,TMPDIR=str(self.dir)),
                    capture_output=True,text=True,timeout=15)
                self.expect(result,0)
                self.assertIn(expected,result.stdout)
                self.assertFalse(list(self.dir.glob('cnc-legacy.*')))


if __name__ == '__main__':
    unittest.main(verbosity=2)
