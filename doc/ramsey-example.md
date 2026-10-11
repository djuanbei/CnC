# Ramsey extension example results

The current automated example tests only the decomposition formula. Earlier
seed-only extension and positive-control runs below are retained as historical
measurements; the runner no longer executes them. A decomposition-only rerun on
2026-10-10 completed all four backend/feature combinations and checkpoint
resumes, each with outcome UNKNOWN. Its temporary report is
`/tmp/cnc-decomp-only-20261010-1140/report.json`.

The target R(5,5,39) and Hk = Kk are confirmed. The decomposition fixture also
fixes the singleton adjacent to all 24 seed vertices and nonadjacent to the
five H blocks; this singleton convention remains explicit in the generator.

Measured on macOS arm64, 2026-10-10. The explicit provisional target is R(5,5,39),
with Hk = Kk and the singleton joined to the seed. See the
[fixture definitions and commands](../tests/ramsey/README.md) and
[raw measurements](ramsey-example-results.json).

| Case | Variables | Fixed edges | Free edges | Clauses |
| --- | ---: | ---: | ---: | ---: |
| decomposition | 741 | 328 | 413 | 149994 |
| extension | 741 | 276 | 465 | 248157 |

All eight fresh runs and eight resumes passed the workflow checks. Both
25-vertex controls were SAT and their models passed the independent graph checker.
**The selected-seed final-decision requirement is unmet.** The initial 39-vertex
runs remained UNKNOWN at the cube limit. No selected-seed extension was found or disproved.

| Case | Backend | Features | Fresh + resume seconds | Orbital / ordinary splits | Outcome |
| --- | --- | --- | ---: | ---: | --- |
| decomposition | iglucose | plain | 2.91 | 0 / 2 | UNKNOWN |
| decomposition | iglucose | combined | 5.03 | 2 / 0 | UNKNOWN |
| decomposition | ilingeling | plain | 2.97 | 0 / 2 | UNKNOWN |
| decomposition | ilingeling | combined | 4.98 | 2 / 0 | UNKNOWN |
| extension | iglucose | plain | 4.12 | 0 / 2 | UNKNOWN |
| extension | iglucose | combined | 7.16 | 2 / 0 | UNKNOWN |
| extension | ilingeling | plain | 4.11 | 0 / 2 | UNKNOWN |
| extension | ilingeling | combined | 7.35 | 2 / 0 | UNKNOWN |

Each run used a 0.5-second conquer budget, two worker slots, one-second symmetry
and canonicalization budgets, and five-second look-ahead queries. The cube limit
increased from three to five on resume. The enabled-feature runs performed two
orbital splits each, and canonical indexing remained below its 64 MiB budget.
No equivalent cubes were encountered in this short Ramsey search; duplicate
reuse is covered by the separate integration fixtures. Worker counts stayed
within the configured limit, and resume preserved cube identities and completed
results. These small search budgets establish workflow behavior, not solve speed.

Checkpoint directories and detailed logs from this measurement remain under
`/tmp/cnc-ramsey-final`; they are temporary artifacts. The generator, seed,
runner, and this report are retained in the repository for reproduction.

## Deeper continuation

The four enabled-feature cases were resumed from five cubes with a limit of
127 cubes and 60 seconds per case. Original attempt/analysis budgets and graph
assumptions were retained. [Raw continuation measurements](ramsey-continuation-results.json)
include checkpoint paths, logs, state counts, and cumulative counters.

| Case | Backend | Seconds | Cubes | UNSAT subproblems | Outcome |
| --- | --- | ---: | ---: | ---: | --- |
| decomposition | iglucose | 51.49 | 127 | 7 | UNKNOWN |
| decomposition | ilingeling | 51.80 | 127 | 7 | UNKNOWN |
| extension | iglucose | 60.52 | 127 | 17 | PAUSED |
| extension | ilingeling | 60.52 | 127 | 17 | PAUSED |

Both decomposition runs reached the cube limit with matching branch counters:
58 orbital splits, five ordinary splits, and seven UNSAT subproblems. Both
seed-only extension runs reached 127 generated cubes and paused at the wall-clock
deadline with work still pending. **There is still no SAT/UNSAT decision for either
39-vertex instance.** An UNSAT subproblem does not establish whole-instance UNSAT.

All four runs preserved earlier cube identities and terminal results. Each stayed
within two conquer workers and the 64 MiB canonical-index allowance. The index
filled during these runs; solving continued with further index insertions skipped.
The maximum reported coordinator RSS was approximately 299 MiB on this host.
The index allowance does not bound total process memory.

To continue these exact checkpoints:

```sh
python3 tests/ramsey/continue_example.py /tmp/cnc-ramsey-final --max-cubes 255 --deadline 60
```

Use the same binaries recorded in the checkpoints. The continuation command saves
a report after each case and independently checks any SAT graph it finds.

## Final-decision acceptance status

The user confirmed the R(5,5,39) target, Hk = Kk, and a requirement for a final
SAT or UNSAT answer. The selected-seed decomposition and extension have not met
that requirement. CaDiCaL returned UNKNOWN after both 120-second and 600-second
runs on each fixed-seed CNF. A separate CnC run on the seed-only extension used 30-second conquer
attempts and paused after 180 seconds with 13 generated cubes; its checkpoint is
`/tmp/cnc-long-extension-cp`.

A separate 39-vertex SAT control, derived from McKay's 42-vertex sample, did
finish on both real CnC backends with feature flags both off and on. All four
models passed the independent K5/independent-five checker. Each solved at the
root, so this checks that the flags do not disrupt the SAT path; it does not
demonstrate symmetry detection, orbital branching, or duplicate-cube reuse.
The harder selected-seed runs did record orbital splits, but their final
decision remains open.

A 180-second CnC decomposition run using CaDiCaL as the conquer worker reached
31 cubes, marked some subproblems UNSAT, and paused with an intact checkpoint at
`/tmp/cnc-cadical-conquer-decomposition-cp`. The corresponding seed-only
extension run reached 25 cubes and 12 orbital splits before pausing, with its
checkpoint at `/tmp/cnc-cadical-conquer-extension-cp`. Both root decisions remain
open.

### One 1,800-second decomposition continuation

The symmetry/deduplication-enabled decomposition case was resumed from the
31-cube checkpoint using the CaDiCaL conquer adapter. It ran for 1,800.78
seconds and paused cleanly without a whole-formula decision. The checkpoint at
`/tmp/cnc-cadical-conquer-decomposition-cp` now holds 379 cubes: 189 split,
19 UNSAT, and 171 queued. Cumulative counters include 203 conquer attempts,
147 orbital splits, and 42 look-ahead splits, with at most two workers. Its log
and output are `/tmp/cnc-decomposition-1800-20261010.log` and `.out`. These
subproblem results do not establish SAT or UNSAT for the selected-seed formula.

Run `tests/ramsey/run_example.py` or `continue_example.py` with
`--require-decision` to make an unresolved result fail the acceptance gate.
