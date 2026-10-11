#!/usr/bin/env python3
"""Continue existing Ramsey checkpoints with larger limits and verify saved work."""
import argparse
import json
from pathlib import Path
import time
import ramsey
from run_example import latest, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path, help='output directory from run_example.py')
    parser.add_argument('--max-cubes', type=int, default=127)
    parser.add_argument('--deadline', type=float, default=60, help='seconds per case before graceful pause')
    parser.add_argument('--features', choices=['plain', 'combined', 'both'], default='combined')
    parser.add_argument('--require-decision', action='store_true',
                        help='fail unless every selected case returns SAT or UNSAT')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    if args.max_cubes < 1 or args.deadline <= 0:
        parser.error('limits must be positive')
    stamp = time.time_ns()
    report = dict(max_cubes=args.max_cubes, deadline_seconds=args.deadline, cases=[])
    report_path = args.report or args.directory / f'continuation-{stamp}.json'
    features = ['plain', 'combined'] if args.features == 'both' else [args.features]
    for mode in ['decomposition']:
        for backend in ['iglucose', 'ilingeling']:
            for feature in features:
                name = f'{mode}-{backend}-{feature}'
                cp = args.directory / (name + '-checkpoint')
                before = latest(cp)
                if args.max_cubes < before['config']['max-cubes']:
                    parser.error(f'{name}: max-cubes cannot decrease')
                stem = args.directory / f'{name}-continue-{stamp}'
                result = run(['--resume', cp, '--max-cubes', args.max_cubes], stem, args.deadline)
                after = latest(cp)
                assert before['run'] == after['run']
                assert before['session'] != after['session']
                assert len(after['cubes']) >= len(before['cubes'])
                assert after['counters']['attempts'] >= before['counters']['attempts']
                assert after['counters']['peak_workers'] <= after['config']['workers']
                assert after['counters']['index_bytes_charged'] <= after['config']['index-bytes']
                for old, new in zip(before['cubes'], after['cubes']):
                    assert old['id'] == new['id'] and old['fixed'] == new['fixed']
                    if old['state'] in ('SAT', 'UNSAT'):
                        assert old['state'] == new['state']
                if result['exit_code'] == 10:
                    ramsey.verify_model(stem.with_suffix('.out').read_text(), 39, mode)
                    result['model_independently_verified'] = True
                states = {}
                for cube in after['cubes']:
                    states[cube['state']] = states.get(cube['state'], 0) + 1
                report['cases'].append(dict(name=name, **result, checkpoint=str(cp),
                    log=str(stem.with_suffix('.log')), previous_cubes=len(before['cubes']),
                    cubes=len(after['cubes']), states=states, counters=after['counters'],
                    checkpoint_preservation_verified=True))
                report_path.parent.mkdir(parents=True, exist_ok=True)
                report_path.write_text(json.dumps(report, indent=2) + '\n')
                print(name, result['outcome'], len(after['cubes']), 'cubes', flush=True)
    print('Report:', report_path)
    report['required_decision_met'] = all(c['exit_code'] in (10, 20) for c in report['cases'])
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    if args.require_decision and not report['required_decision_met']:
        raise SystemExit('39-vertex final-decision requirement unmet; checkpoints retained')


if __name__ == '__main__':
    main()
