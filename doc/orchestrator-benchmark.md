# Orchestrator smoke benchmark

Measured on the local development host, 2026-10-09. These are single-run smoke
measurements of five small fixtures per backend, not statistical performance
estimates. Exact commands/generation are in `tests/orchestrator/benchmark.py`;
raw measurements are in `orchestrator-benchmark.json`.

Environment: Apple Clang 21, macOS arm64, nauty 2.9.3 (64 bits), OpenSSL 3.6.4,
C++17 `-O2 -g`. One conquer worker, seed 0, 0.1-second attempt budget,
1-second analysis budgets, 5-second external limit per invocation.

Legacy means the existing march cubing plus conquer binary pipeline, invoked
directly. The legacy scripts now support other working directories and paths
with spaces; their SAT/UNSAT invocations are separately regression-tested. If march
solves directly, its decision completes that baseline run. Fixtures include
symmetric/asymmetric SAT/UNSAT cases and `tests/Rivest.cnf`.

| Backend | Mode | Solved | Total wall seconds |
| --- | --- | --- | --- |
| iglucose | legacy | 5/5 | 0.0156 |
| iglucose | plain | 5/5 | 0.0421 |
| iglucose | symmetry | 5/5 | 0.0433 |
| iglucose | deduplicate | 5/5 | 0.0600 |
| iglucose | combined | 5/5 | 0.0619 |
| iglucose | checkpoint | 5/5 | 0.0661 |
| ilingeling | legacy | 5/5 | 0.0147 |
| ilingeling | plain | 5/5 | 0.0473 |
| ilingeling | symmetry | 5/5 | 0.0486 |
| ilingeling | deduplicate | 5/5 | 0.0663 |
| ilingeling | combined | 5/5 | 0.0671 |
| ilingeling | checkpoint | 5/5 | 0.0724 |

All modes agreed on decisions. The new workflow is slower on these tiny inputs;
canonicalization and durable writes add visible startup cost. Most inputs finish
before a timeout, so this sample does not establish benefits from orbital splits
or duplicate suppression. Those paths are exercised by the forced-timeout tests.
No speedup threshold is claimed.

Raw rows include phase costs, generated cubes, attempts, peak scheduler RSS
(`maxrss_platform_units`: bytes on this macOS host), charged index memory,
checkpoint directory bytes, and completed-checkpoint restore latency. Peak RSS
is per coordinator process, not simultaneous aggregate worker memory. Results
with longer production instances and repeated statistical measurements remain
future performance evaluation, not grounds to weaken correctness gates.
