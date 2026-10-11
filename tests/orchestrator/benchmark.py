#!/usr/bin/env python3
"""Reproducible bounded smoke benchmark; emits machine-readable measurements."""
import json
from pathlib import Path
import platform
import subprocess
import tempfile
import time
ROOT=Path(__file__).resolve().parents[2]

def run(args, limit=5):
    start=time.monotonic()
    try:
        p=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=limit)
        return p.returncode,p.stdout,p.stderr,time.monotonic()-start
    except subprocess.TimeoutExpired:
        return -1,'','external timeout',time.monotonic()-start

with tempfile.TemporaryDirectory(prefix='cnc-bench-') as tmp:
    d=Path(tmp)
    fixtures={
        'symmetric-sat':(3,[[1,2],[1,3],[2,3]]),
        'symmetric-unsat':(2,[[1,2],[1,-2],[-1,2],[-1,-2]]),
        'asymmetric-sat':(4,[[1,2,3],[-1,2],[-2,4],[-3,-4]]),
        'asymmetric-unsat':(3,[[1,2],[1,-2],[-1,2],[-1,-2],[1,3]]),
    }
    inputs=[]
    for name,(n,clauses) in fixtures.items():
        file=d/(name+'.cnf');file.write_text(f'p cnf {n} {len(clauses)}\n'+''.join(' '.join(map(str,c))+' 0\n' for c in clauses));inputs.append(file)
    inputs.append(ROOT/'tests/Rivest.cnf')
    rows=[]
    for backend in ['iglucose','ilingeling']:
        binary=ROOT/('iglucose/core/iglucose' if backend=='iglucose' else 'lingeling/ilingeling')
        for file in inputs:
            cubes=d/'cubes.icnf'
            code,stdout,stderr,elapsed=run([ROOT/'march_cu/march_cu',file,'-o',cubes])
            if code==10 or 's SATISFIABLE' in stdout:
                legacy_code=10;legacy_seconds=elapsed
            elif code==20 or 's UNSATISFIABLE' in stdout:
                legacy_code=20;legacy_seconds=elapsed
            elif cubes.exists():
                lines=file.read_text().splitlines()
                incremental=d/'input.icnf';incremental.write_text('p inccnf\n'+'\n'.join(x for x in lines if not x.startswith(('p','c')))+'\n'+cubes.read_text())
                c,o,e,t=run([binary,incremental,'-verb=0' if backend=='iglucose' else '1'])
                legacy_code=10 if 's SATISFIABLE' in o else 20 if 's UNSATISFIABLE' in o else c
                legacy_seconds=elapsed+t
            else:legacy_code=-1;legacy_seconds=elapsed
            rows.append(dict(backend=backend,input=file.stem,mode='legacy',exit=legacy_code,seconds=legacy_seconds))
            for mode,flags in [('plain',[]),('symmetry',['--symmetry']),('deduplicate',['--deduplicate']),('combined',['--symmetry','--deduplicate']),('checkpoint',['--symmetry','--deduplicate'])]:
                checkpoint=d/(backend+'-'+file.stem)
                args=[ROOT/'orchestrator/cnc',file,'--backend',backend,'--attempt-seconds','.1',*flags]
                if mode=='checkpoint':args+=['--checkpoint-dir',checkpoint]
                c,o,e,t=run(args)
                row=dict(backend=backend,input=file.stem,mode=mode,exit=c,seconds=t)
                for line in e.splitlines():
                    for key in ['counters','phase_seconds']:
                        prefix='c '+key+' '
                        if line.startswith(prefix):row[key]=json.loads(line[len(prefix):])
                if mode=='checkpoint' and checkpoint.exists():
                    row['checkpoint_bytes']=sum(p.stat().st_size for p in checkpoint.iterdir() if p.is_file())
                    _,_,_,rt=run([ROOT/'orchestrator/cnc','--resume',checkpoint]);row['restore_seconds']=rt
                rows.append(row)
    report=dict(platform=platform.platform(),machine=platform.machine(),attempt_seconds=.1,workers=1,seed=0,external_limit_seconds=5,results=rows)
    print(json.dumps(report,indent=2))
