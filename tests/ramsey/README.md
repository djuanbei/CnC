# 39-vertex Ramsey regression example

This example uses the requested partition:

`39 = 24 + 1 + 4 + 3 + 3 + 2 + 2`.

The confirmed target is R(5,5,39), and each Hk is the clique Kk.
The current singleton convention makes vertex 24 adjacent to every seed vertex
and nonadjacent to all H vertices.
Edges between different H blocks and between the seed and H blocks remain free.
The singleton convention is stated explicitly because it fixes additional edges.

The automated example tests only this decomposition formula. The generator also
supports a seed-only extension formula for separate experiments, but the test
runner does not use it. Preserving R(4,5) on all 39 vertices would be impossible:
R(4,5) = 25.

## Seed and provenance

`r45_24.g6` is the first graph in
[Brendan McKay's R(4,5,24) catalogue](https://users.cecs.anu.edu.au/~bdm/data/ramsey.html),
downloaded from [r45_24.g6](https://users.cecs.anu.edu.au/~bdm/data/r45_24.g6).
Only one graph is included; no network access is needed to reproduce this test.
Its SHA-256, including the newline, is
`b96cc9d212d4e3be872f4ae88bac87b7d49f0a615525b3829b6cd614b7a9ce2d`.
The generator checks all four- and five-vertex subsets before using the seed.

Vertices are numbered 0–38. Blocks are 0–23, {24}, 25–28, 29–31, 32–34,
35–36, and 37–38. Edge variables are numbered 1–741 in lexicographic `(u,v)`
order, with `u < v`. Positive means an edge is present.
For each five-vertex subset, a negative clause forbids a clique and a positive
clause forbids an independent set. Fixed edges are substituted and retained as
unit clauses; satisfied and duplicate clauses are removed. This preserves the
full encoding's models, without renumbering edge variables.

## Run

From the repository root, after `./build.sh orchestrator`:

```sh
python3 tests/ramsey/test_encoding.py
python3 tests/ramsey/run_example.py --output /tmp/ramsey39-new-run
```

Choose a new output directory. The runner preserves the decomposition CNF,
metadata, stdout, diagnostics, resumable checkpoints, and a JSON report. It tests
both real backends with features off and with symmetry/deduplication enabled. Each attempt
gets 0.5 seconds; analysis gets 1 second per operation, look-ahead 5 seconds,
and at most two conquer workers run. The first run allows three total cubes;
resume increases the limit to five. A 30-second outer deadline requests a graceful
pause. UNKNOWN and PAUSED are recorded as unsolved, never as SAT or UNSAT.

Every 39-vertex SAT model, if found, is checked against all forbidden subsets
and fixed edges. The unit tests check the decomposition's fixed edges, clause
count, and CNF evaluations against the independent graph checker.

There is also a completed **39-vertex SAT control** from the first graph in
[McKay's R(5,5,42) sample](https://users.cecs.anu.edu.au/~bdm/data/r55_42some.g6).
The file `r55_42_first.g6` contains that single 42-vertex graph. Its first 39
vertices form a graph with neither K5 nor an independent set of size 5. The
file SHA-256, including its newline, is
`ab0b10364ac62ab07f662c0ed4e8b47a44956f63a78356b64ae64ccb6520ecb0`.
The `known-39-control` CNF fixes 561 edges and leaves 180 open. Both real conquer
backends solve it with the symmetry/deduplication flags off and on; every model
is checked independently as a graph. All four runs solve at the root, so this
control verifies the SAT path but does not exercise orbital branching or
duplicate-cube detection. Those paths are exercised by the harder selected-seed
runs below. This control uses a different source graph and does not decide
whether the chosen R(4,5,24) seed extends to 39.

```sh
python3 tests/ramsey/test_known_control.py
python3 tests/ramsey/ramsey.py /tmp/known-39-control.cnf --mode known-39-control
```

Generate the decomposition CNF, or change the explicit conventions:

```sh
python3 tests/ramsey/ramsey.py /tmp/decomposition.cnf --mode decomposition
# Leave singleton adjacency free instead:
python3 tests/ramsey/ramsey.py /tmp/free-singleton.cnf --mode decomposition --free-singleton
```

`--clique`, `--independent`, `--seed`, `--vertices`, and `--blocks` expose the other
parameters. Blocks are complete graphs; arbitrary H graphs require their actual
adjacency data and a corresponding generator change.

See [measured results](../../doc/ramsey-example.md). The selected-seed runs have
not reached SAT or UNSAT. Add `--require-decision` to either runner to make that
requirement an explicit failing acceptance gate while preserving checkpoints.
The completed 39-vertex SAT control verifies the solver path on a different graph.

## Continue the saved search

Increase the cube limit and reuse the existing run identities and completed work:

```sh
python3 tests/ramsey/continue_example.py /tmp/ramsey39-new-run --max-cubes 127 --deadline 60
```

This continues the two symmetry/deduplication-enabled decomposition cases, one
at a time.
Use `--features both` to include the plain cases. The deadline applies to each
case and requests a graceful pause, preserving a checkpoint. The report records
actual outcomes, state counts, resource counters, and preservation checks. A SAT
result is independently verified as a graph. Repeating this command resumes
paused work; increase `--max-cubes` again when a run reaches that limit.
Conquer and analysis budgets stay fixed by the original checkpoint configuration.

For longer fixed-seed experiments, `cadical_worker.py` converts each CnC
incremental attempt to an ordinary DIMACS CNF and invokes an installed CaDiCaL.
It is an optional solver override; the two built-in backends remain the standard
test matrix. For example, after generating `decomposition.cnf`:

```sh
./orchestrator/cnc /tmp/decomposition.cnf --backend iglucose \
  --solver "$PWD/tests/ramsey/cadical_worker.py" \
  --symmetry --deduplicate --workers 2 --attempt-seconds 20 \
  --checkpoint-dir /tmp/decomposition-cadical-cnc
```

The solver executable and its contents are part of checkpoint compatibility.
