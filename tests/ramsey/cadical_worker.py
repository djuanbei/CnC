#!/usr/bin/env python3
"""Optional CaDiCaL conquer adapter for an orchestrator p inccnf attempt."""
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    if len(sys.argv) < 2:
        raise SystemExit('usage: cadical_worker.py attempt.icnf')
    lines = Path(sys.argv[1]).read_text().splitlines()
    base = []
    assumptions = []
    declared = False
    variables = 0
    for line in lines:
        row = line.strip()
        if not row or row.startswith('c'):
            continue
        if row.startswith('p '):
            if declared or row != 'p inccnf':
                raise ValueError('expected one incremental CNF header')
            declared = True
        elif row.startswith('a '):
            values = [int(x) for x in row.split()[1:]]
            if not values or values[-1] != 0:
                raise ValueError('unterminated assumptions')
            assumptions.extend(values[:-1])
        else:
            values = [int(x) for x in row.split()]
            if not values or values[-1] != 0:
                raise ValueError('unterminated clause')
            base.append(row)
            variables = max([variables, *(abs(x) for x in values[:-1])])
    if not declared:
        raise ValueError('missing incremental CNF header')
    variables = max([variables, *(abs(x) for x in assumptions)])
    filename = None
    try:
        with tempfile.NamedTemporaryFile('w', suffix='.cnf', prefix='cnc-cadical-',
                                         delete=False) as file:
            filename = file.name
            file.write(f'p cnf {variables} {len(base)+len(assumptions)}\n')
            for row in base:
                file.write(row+'\n')
            for literal in assumptions:
                file.write(f'{literal} 0\n')
        result = subprocess.run(['cadical', '-q', filename])
        return result.returncode
    finally:
        if filename is not None:
            Path(filename).unlink(missing_ok=True)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print('c cadical worker error:', error, file=sys.stderr)
        sys.exit(1)
