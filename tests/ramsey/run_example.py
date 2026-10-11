#!/usr/bin/env python3
"""Bounded real-backend Ramsey test; UNKNOWN is recorded, never counted as solved."""
import argparse
import hashlib
import itertools
import json
import platform
from pathlib import Path
import signal
import subprocess
import time
import ramsey

ROOT = Path(__file__).resolve().parents[2]
CNC = ROOT/'orchestrator/cnc'


def latest(cp):
    commit=max(cp.glob('commit-*.json'), key=lambda p:int(p.stem.split('-')[1]))
    manifest=json.loads(commit.read_text())
    data=(cp/manifest['file']).read_bytes()
    assert hashlib.sha256(data).hexdigest()==manifest['sha256']
    return json.loads(data)


def run(args, stem, deadline):
    started=time.monotonic()
    with stem.with_suffix('.out').open('w') as out, stem.with_suffix('.log').open('w') as err:
        process=subprocess.Popen([str(CNC),*map(str,args)],stdout=out,stderr=err)
        try: process.wait(timeout=deadline)
        except subprocess.TimeoutExpired:
            process.send_signal(signal.SIGINT)
            try: process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait()
                raise RuntimeError('solver did not pause within 30 seconds')
    if process.returncode not in (0,3,10,20):
        raise RuntimeError(stem.with_suffix('.log').read_text())
    return dict(exit_code=process.returncode,seconds=round(time.monotonic()-started,4),
                outcome={0:'UNKNOWN',3:'PAUSED',10:'SAT',20:'UNSAT'}[process.returncode])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True,help='new directory for CNFs, logs, checkpoints')
    parser.add_argument('--deadline',type=float,default=30)
    parser.add_argument('--require-decision',action='store_true',
                        help='fail unless every 39-vertex case returns SAT or UNSAT')
    parser.add_argument('--report',type=Path)
    args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=False)
    seed_n,edges=ramsey.decode_graph6(ramsey.SEED.read_text())
    ramsey.verify_graph(seed_n,edges,4,5)
    report=dict(platform=platform.platform(),machine=platform.machine(),
        limits=dict(attempt_seconds=.5,analysis_seconds=1,lookahead_seconds=5,workers=2,
                    initial_max_cubes=3,resume_max_cubes=5,outer_deadline_seconds=args.deadline),
        binaries={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in [CNC,ROOT/'march_cu/march_cu',ROOT/'iglucose/core/iglucose',ROOT/'lingeling/ilingeling']},
        seed_vertices=seed_n,seed_edges=len(edges),seed_verified=True,
        seed_source='https://users.cecs.anu.edu.au/~bdm/data/r45_24.g6',
        assumptions={'target':'R(5,5,39)','Hk':'Kk',
                     'singleton':'adjacent to seed; nonadjacent to all H vertices',
                     'cross_block_edges':'free'},cases=[])
    for mode in ['decomposition']:
        path=args.output/(mode+'.cnf')
        metadata=ramsey.generate(path,mode=mode)
        for backend,features in itertools.product(['iglucose','ilingeling'],['plain','combined']):
            name=mode+'-'+backend+'-'+features
            cp=args.output/(name+'-checkpoint')
            flags=['--symmetry','--deduplicate'] if features=='combined' else []
            row=run([path,'--backend',backend,'--workers',2,'--attempt-seconds','.5',
                     '--symmetry-seconds',1,'--canonical-seconds',1,'--lookahead-seconds',5,
                     '--max-cubes',3,'--checkpoint-dir',cp,*flags],args.output/name,args.deadline)
            before=latest(cp)
            row.update(mode=mode,backend=backend,features=features,cnf=metadata,
                       counters=before['counters'])
            if row['exit_code']==10:
                ramsey.verify_model((args.output/(name+'.out')).read_text(),39,mode)
                row['model_independently_verified']=True
            resumed=run(['--resume',cp,'--max-cubes',5],args.output/(name+'-resume'),args.deadline)
            after=latest(cp)
            assert len(after['cubes'])>=len(before['cubes'])
            assert after['counters']['attempts']>=before['counters']['attempts']
            assert after['counters']['peak_workers']<=2
            assert after['counters']['index_bytes_charged']<=67108864
            assert after['run']==before['run'] and after['session']!=before['session']
            for old,new in zip(before['cubes'],after['cubes']):
                assert old['id']==new['id'] and old['fixed']==new['fixed']
                if old['state'] in ('SAT','UNSAT'): assert new['state']==old['state']
            if resumed['exit_code']==10:
                ramsey.verify_model((args.output/(name+'-resume.out')).read_text(),39,mode)
                resumed['model_independently_verified']=True
            if row['exit_code'] in (10,20): assert row['exit_code']==resumed['exit_code']
            row['resume']=dict(**resumed,counters=after['counters'])
            report['cases'].append(row)
            print(name,row['outcome'],'->',resumed['outcome'],flush=True)
    report['workflow_checks_passed']=True
    report['solved_39_vertex_cases']=sum(c['resume']['exit_code'] in (10,20) for c in report['cases'])
    report['total_39_vertex_cases']=len(report['cases'])
    report['required_decision_met']=(report['solved_39_vertex_cases']==
                                     report['total_39_vertex_cases'])
    report_path=args.report or args.output/'report.json'
    report_path.parent.mkdir(parents=True,exist_ok=True)
    report_path.write_text(json.dumps(report,indent=2)+'\n')
    print('Report:',report_path)
    if args.require_decision and not report['required_decision_met']:
        raise SystemExit('39-vertex final-decision requirement unmet; checkpoints retained')

if __name__=='__main__': main()
