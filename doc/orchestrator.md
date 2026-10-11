# Symmetry-aware Cube-and-Conquer

## Build and run

The new workflow is opt-in. Existing solve scripts remain available.
Use a C++17 compiler, nauty **2.9.3** development headers/library, and OpenSSL 3.x
development headers/library. dejavu and nlohmann/json headers are vendored with
licenses and exact pins in `orchestrator/vendor/README.md`.

From the repository root:

```sh
./build.sh orchestrator
./orchestrator/cnc tests/Rivest.cnf --backend iglucose
./orchestrator/cnc tests/Rivest.cnf --backend ilingeling --symmetry --deduplicate
make -C orchestrator test
```

The Makefile detects `/opt/homebrew` on Apple Silicon. Elsewhere pass `PREFIX`
for a common dependency prefix, or supply `CPPFLAGS` and `LDFLAGS` for separate
locations. The old `./build.sh` path does not require the new dependencies.
The march build uses `-fcommon` for its legacy global definitions; iglucose's
core and simplified Makefiles support `make -C`; simplified-glucose integer
format macros now compile with modern C++. An iglucose early simplification bug was fixed
so an immediate contradiction yields UNSAT rather than INDETERMINATE.

The new solver emits standard `s`/`v` records to stdout and diagnostics to stderr.
Exit codes: SAT **10**, UNSAT **20**, UNKNOWN **0**, PAUSED **3**, ERROR **2**.
A SAT/UNSAT exit code is not an operating-system failure. Unsupported options,
including proof flags, are rejected. This workflow does not produce proofs.

`--help` lists all controls and defaults. The main controls are:

| Option | Default | Meaning |
| --- | --- | --- |
| `--backend` | `iglucose` | `iglucose` or `ilingeling` |
| `--solver`, `--march` | Repository binaries | Explicit executable paths |
| `--symmetry` | Off | Verified dejavu orbital branching |
| `--deduplicate` | Off | Exact nauty canonicalization/result reuse |
| `--workers` | 1 | Maximum concurrent conquer processes; ilingeling uses one internal worker |
| `--attempt-seconds` | 10 | Wall-clock budget from dispatch |
| `--symmetry-seconds`, `--canonical-seconds` | 1 each | Independent analysis budgets |
| `--lookahead-seconds` | 1 | Bound the isolated march decision query |
| `--max-depth` | 100000 | Root depth is zero; zero prohibits splitting |
| `--max-cubes` | 1000000 | Total records, including root and terminal children |
| `--index-bytes` | 67108864 | Conservative charge for stored canonical data and hash-index entries; zero disables insertion |
| `--retries` | 1 | Additional attempts after a worker failure; pause is not failure |
| `--seed` | 0 | Deterministic dejavu seed, restored with configuration |

The root starts as one conquer job. A timeout requests a split; fast instances
need no branching. Each split makes progress by fixing an active residual variable.
The scheduler polls approximately every 2 ms. Analysis uses a separate short-lived
process, so at most `workers + 1` computation processes run concurrently. Analysis
and durable saves can delay scheduler polling; budgets are cooperative dispatch
budgets rather than hard real-time guarantees. Worker termination has a 100 ms
grace period before SIGKILL. Completed output observed before cancellation wins.

march has a new `-branch-only` adapter option: it prints `b <literal>` when its
existing look-ahead heuristic selects a branch. If preprocessing solves the
formula before that selection, a valid terminal response completes the cube
(SAT models are validated). If the query fails, exceeds its budget, or returns
no valid decision/result, the cube becomes UNRESOLVED with
`lookahead-unavailable`. No arbitrary variable is substituted. Both split
children are propagated before their records enter the queue.

## Pause and resume

```sh
./orchestrator/cnc problem.cnf --symmetry --deduplicate \
  --checkpoint-dir ./solve-progress --checkpoint-interval 60
# Send SIGINT (Ctrl-C) or SIGTERM to pause gracefully.
./orchestrator/cnc --resume ./solve-progress
./orchestrator/cnc --resume ./solve-progress --workers 4 --max-depth 200000
```

A fresh checkpoint directory must not already contain a run. Resume automatically
uses that directory for subsequent saves. Do not combine `--resume` with
`--checkpoint-dir`. The original input file is optional on resume; if supplied,
its exact bytes must match the saved SHA-256 identity.

Resume preserves cubes completed in the restored generation. Interrupted conquer
attempts restart from cube assumptions with fresh attempt IDs and budgets. There
is no heap or learned-clause restoration. A saved timeout remains an obligation
to split, and failure retry counts are not reset. Limit-blocked splits can become
runnable after an allowed limit increase. Unknown/error runs do not imply UNSAT.

Only worker count, checkpoint interval, and increases to depth/cube/index limits
may change on resume. Backend, binary contents, feature flags, analysis versions,
seed, and budgets must match. A changed solver executable is rejected even if its
filename is unchanged. Resume needs the recorded solver/march binary locations.

Graceful pause reaps active workers and saves before returning PAUSED. Initial
save failure prevents dispatch. Periodic save failure reports checkpoint age and
keeps solving; failed final saves report ERROR, never a successful pause. Forced
termination can lose work since the last committed generation. Old worker output
is never reattached to a new session. If the coordinator is forcibly killed,
external process supervision may be needed to terminate surviving workers;
graceful cancellation performs process-group cleanup automatically.

## Checkpoint format and integrity

A checkpoint directory contains:

- `root.cnf`: original DIMACS bytes, shared by all generations.
- `lock`: exclusive `flock` held for the whole invocation.
- `state-N.json`: schema-1 logical scheduler snapshot (adapter `cnc-2`).
- `commit-N.json`: generation, exact state filename, state SHA-256, root SHA-256.
- Temporary `.tmp` files from unfinished saves, ignored during restoration.

Snapshots contain configuration/version fingerprints, run/session IDs, cube
assumptions and propagation trails, results/models, split/generator evidence,
canonical bytes/hashes/mappings, alias edges, next attempt ID, counters and timing.
Cube ID equals its array index; queue priority is ascending ID. Thus the retained
array and states define the pending queue order without a separate duplicate list.
The next cube ID is the array length. Analysis restarts with the saved seed on each
cube, so no partially consumed random generator state crosses a checkpoint.

The single scheduler owner commits logical transitions before snapshots. A save
copies a consistent state, accounts for active attempt time through that snapshot,
writes and fsyncs a new file, renames it, fsyncs the directory, and publishes/fsyncs
the commit manifest last. The two newest **valid** generations are retained.
The root is written once and integrity-checked on every save/restore.

Restore checks checksums and schema before launching workers, then validates IDs,
parent/child structure, split assumptions and stored symmetry generators,
canonical mappings/hashes, model validity, and acyclic combined dependencies.
Corrupt newest-generation data can fall back to a previous intact generation with
a visible diagnostic. Logical inconsistencies and incompatible versions fail
closed. Restore does not silently migrate a checkpoint.

Durability assumes a local POSIX filesystem honoring atomic same-directory rename,
file fsync, directory fsync, and exclusive flock. Network filesystems and physical
power-loss behavior are not certified. `fsync` does not promise protection against
all storage-device/controller failures. Tests inject process/storage failures at
publication boundaries; they cannot reproduce every hardware failure.

## Correctness boundaries

A residual has one positive and one negative literal vertex per active variable,
connected as a pair. Literal signs and clause vertices have three different colors;
each clause is connected to its literal vertices. This admits exactly the supported
polarity-preserving CNF permutations. Clause order and duplicate literals/clauses
are normalized; tautological clauses are removed. Absent variables remain available
for completing models in original variable IDs.

Every dejavu generator is checked by permuting the residual clauses and comparing
the normalized result. Under the verified subgroup `G(C)`, an orbit `O` permits
`C_1 = C ∧ r` and `C_0 = C ∧ (⋀_{x ∈ O} ¬x)`. If a model makes some orbit variable
true, a verified permutation takes it to a model with `r` true; otherwise every
orbit variable is false. Thus these two children preserve decision satisfiability.
They do not represent an exhaustive model partition or support model counting.

nauty computes an exact canonical labeling of the same colored graph. `K(C)` is
the normalized CNF under canonical variable labels, and `π_C` maps original IDs to
those labels. Hash buckets only find candidates. Exact bytes and induced variable
mappings must match before reuse. A representative model is mapped back, augmented
with the target cube's propagation trail, and validated against `F ∧ C`.

Parent-to-child completion edges and alias-to-representative edges form a DAG.
A match that would create a cycle is rejected; neither hash collisions nor a
blocked representative can discharge a branch. UNSAT propagates through all
required children. SAT requires a validated original-CNF model. Crashes, unknown
results, timeouts, analysis failures, and exhausted limits never become UNSAT.

## Validation and practical limits

`make -C orchestrator test` runs exhaustive two-variable propagation/orbital checks,
seeded canonical-renaming tests, known-orbit/polarity fixtures, a forced hash
collision check, and the Python integration suite with both real solvers and
controllable fake workers. All 25 integration tests passed on macOS arm64 in
both normal and address/undefined-behavior sanitizer builds on 2026-10-10.
The interrupted-state matrix covers QUEUED, RUNNING, PENDING_SPLIT, SPLIT,
WAITING_EQUIVALENT, and UNRESOLVED on both real conquer backends with SAT and
UNSAT fixtures. Test-only stop/snapshot hooks and a deterministic branch-query
fixture make those states reachable; normal builds expose no such controls.
Separate tests exercise the real march branch and terminal-result protocol.
Dependency failures, representative failure, ancestor-alias checks, resource
limits, and legacy scripts invoked outside the repository are covered.
Three Ramsey encoding tests and two completed 39-vertex control tests run with
the normal test target.
The unit suite covers all 512 subsets of the nine normalized two-variable clauses
and 160 seeded fixtures. To repeat the instrumented integration run:

```sh
make -C orchestrator cnc_asan
CNC_BIN="$PWD/orchestrator/cnc_asan" python3 tests/orchestrator/test_cnc.py
```

`cnc_fault` is built only for testing; production `cnc`
does not expose environment-controlled fault injection.

See `orchestrator-benchmark.md` and `orchestrator-benchmark.json` for measured
smoke results. They are not a large-instance performance claim. Canonicalization
and symmetry are opt-in because startup and analysis can cost more than solving
small instances. Index memory accounting conservatively charges stored data;
it does not cap the whole run, transient analysis graphs, solver heaps, or copies
made while checkpointing. Every cube record remains retained until run completion.

The [39-vertex Ramsey example](../tests/ramsey/README.md) tests a larger real
input with both backends and checkpoint resumption. Its [results](ramsey-example.md)
are explicitly bounded UNKNOWN outcomes, alongside independently validated
25-vertex SAT controls.
