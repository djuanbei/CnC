#!/usr/bin/env python3
"""Reproducible Ramsey extension CNFs; standard library only, vertex IDs 0-based."""
import argparse
import hashlib
import itertools
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
SEED = HERE / 'r45_24.g6'
KNOWN_42 = HERE / 'r55_42_first.g6'


def decode_graph6(text):
    text = text.strip().removeprefix('>>graph6<<')
    codes = [ord(c) - 63 for c in text]
    if not codes or not 0 <= codes[0] <= 62 or any(not 0 <= c < 64 for c in codes):
        raise ValueError('expected small graph6 graph')
    n = codes[0]
    bits = [(c >> k) & 1 for c in codes[1:] for k in range(5, -1, -1)]
    if len(codes) != 1 + (n * (n - 1) // 2 + 5) // 6:
        raise ValueError('incorrect graph6 length')
    edges = set()
    for bit, pair in zip(bits, ((i, j) for j in range(1, n) for i in range(j))):
        if bit: edges.add(pair)
    return n, edges


def verify_graph(n, edges, clique=5, independent=5):
    for size, present in [(clique, True), (independent, False)]:
        for vertices in itertools.combinations(range(n), size):
            if all((edge in edges) == present for edge in itertools.combinations(vertices, 2)):
                raise ValueError(f'forbidden {"clique" if present else "independent set"}: {vertices}')


def edge_variables(n):
    return {edge: i + 1 for i, edge in enumerate(itertools.combinations(range(n), 2))}


def fixed_edges(n, seed_path=SEED, mode='extension', blocks=(4, 3, 3, 2, 2),
                singleton_joined=True):
    if mode == 'known-39-control':
        if n != 39: raise ValueError('known control has 39 vertices')
        known_n, known_edges = decode_graph6(KNOWN_42.read_text())
        if known_n != 42: raise ValueError('known witness must have 42 vertices')
        verify_graph(known_n, known_edges, 5, 5)
        # Leave 180 edge variables open so a backend must find a completion.
        free = {e for e in itertools.combinations(range(n), 2)
                if (e[0] >= 24 and e[1] >= 24) or (e[0] < 5 and e[1] >= 24)}
        fixed = {e: e in known_edges for e in itertools.combinations(range(n), 2)
                 if e not in free}
        return fixed, [list(range(n))]
    seed_n, edges = decode_graph6(seed_path.read_text())
    if seed_n != 24: raise ValueError('seed must have exactly 24 vertices')
    verify_graph(seed_n, edges, 4, 5)
    if n < seed_n: raise ValueError('target smaller than seed')
    fixed = {e: e in edges for e in itertools.combinations(range(seed_n), 2)}
    groups = [list(range(seed_n))]
    if mode == 'decomposition':
        if n != seed_n + 1 + sum(blocks): raise ValueError('decomposition does not sum to target')
        singleton = seed_n
        groups.append([singleton])
        if singleton_joined:
            for v in range(n):
                if v != singleton: fixed[tuple(sorted((v, singleton)))] = v < seed_n
        offset = singleton + 1
        for size in blocks:
            group = list(range(offset, offset + size))
            groups.append(group)
            # H_k = K_k is an explicit configurable fixture convention.
            fixed.update({e: True for e in itertools.combinations(group, 2)})
            offset += size
    elif mode == 'control':
        if n != 25: raise ValueError('positive control is the 25-vertex seed cone')
        fixed.update({(v, 24): True for v in range(24)})
        groups.append([24])
    elif mode != 'extension': raise ValueError('unknown mode')
    else: groups.append(list(range(seed_n, n)))
    return fixed, groups


def generate(path, n=39, mode='extension', seed_path=SEED, clique=5, independent=5,
             blocks=(4, 3, 3, 2, 2), singleton_joined=True):
    fixed, groups = fixed_edges(n, seed_path, mode, blocks, singleton_joined)
    variables = edge_variables(n)
    clauses = set()
    # Substitute fixed edges in every Ramsey constraint, retaining their unit
    # clauses and original edge IDs. This is equivalent to the full encoding.
    satisfied = 0
    for size, positive in [(clique, False), (independent, True)]:
        for vertices in itertools.combinations(range(n), size):
            clause = []
            for edge in itertools.combinations(vertices, 2):
                if edge in fixed:
                    if fixed[edge] == positive:
                        satisfied += 1
                        break
                else:
                    clause.append(variables[edge] if positive else -variables[edge])
            else:
                clauses.add(tuple(clause))
    clauses.update((variables[e] if value else -variables[e],) for e, value in fixed.items())
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('w') as out:
        source = 'r55_42_first.g6' if mode == 'known-39-control' else 'r45_24.g6'
        out.write(f'c R({clique},{independent},{n}) {mode}; source {source}\n')
        out.write(f'p cnf {len(variables)} {len(clauses)}\n')
        for clause in sorted(clauses): out.write(' '.join(map(str, clause)) + ' 0\n')
    metadata = dict(vertices=n, variables=len(variables), clauses=len(clauses), mode=mode,
                    forbidden_clique=clique, forbidden_independent_set=independent,
                    groups=groups, fixed_edges=len(fixed), free_edges=len(variables)-len(fixed),
                    satisfied_constraints=satisfied, h_convention='complete graphs' if mode=='decomposition' else None,
                    singleton_joined=singleton_joined if mode=='decomposition' else None,
                    seed_sha256=hashlib.sha256(seed_path.read_bytes()).hexdigest(),
                    cnf_sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    if mode == 'known-39-control':
        metadata['witness_sha256'] = hashlib.sha256(KNOWN_42.read_bytes()).hexdigest()
        metadata['seed_sha256'] = None
    path.with_suffix('.json').write_text(json.dumps(metadata, indent=2)+'\n')
    return metadata


def verify_model(output, n, mode, seed_path=SEED, clique=5, independent=5,
                 blocks=(4,3,3,2,2), singleton_joined=True):
    values = {}
    for line in output.splitlines():
        if line.startswith('v '):
            for literal in map(int, line.split()[1:]):
                if literal:
                    if abs(literal) in values and values[abs(literal)] != (literal > 0):
                        raise ValueError('inconsistent model')
                    values[abs(literal)] = literal > 0
    variables = edge_variables(n)
    if set(values) != set(variables.values()): raise ValueError('incomplete edge model')
    edges = {e for e, v in variables.items() if values[v]}
    fixed, _ = fixed_edges(n, seed_path, mode, blocks, singleton_joined)
    if any((e in edges) != value for e, value in fixed.items()): raise ValueError('fixed edge changed')
    verify_graph(n, edges, clique, independent)
    return edges


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output',type=Path)
    parser.add_argument('--mode',choices=['extension','decomposition','control',
                                        'known-39-control'],default='extension')
    parser.add_argument('--vertices',type=int,default=39)
    parser.add_argument('--seed',type=Path,default=SEED)
    parser.add_argument('--clique',type=int,default=5)
    parser.add_argument('--independent',type=int,default=5)
    parser.add_argument('--blocks',type=int,nargs='+',default=[4,3,3,2,2])
    parser.add_argument('--free-singleton',action='store_true')
    args=parser.parse_args()
    if args.clique < 2 or args.independent < 2 or any(k < 1 for k in args.blocks):
        parser.error('invalid graph sizes')
    print(json.dumps(generate(args.output,args.vertices,args.mode,args.seed,args.clique,
                              args.independent,args.blocks,not args.free_singleton),indent=2))

if __name__=='__main__': main()
