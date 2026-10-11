# Development plan: symmetry-aware Cube-and-Conquer

## 1. Scope and completion rule

Implement [Improve_PRD.md](Improve_PRD.md), including symmetry-aware branching,
recursive splitting, exact duplicate suppression, and checkpoint/resume solving.
The PRD is authoritative. The implementation and local release acceptance checks
are complete, including the items reopened in the 2026-10-10 status audit.
The new executable is `orchestrator/cnc`; shared records,
solver adapters, and lifecycle transitions are consolidated into `core.hpp`,
`process.cpp`, and `scheduler.cpp` rather than every proposed file below.

Validation: both legacy and opt-in builds succeeded on macOS arm64; the unit
suite covers all 512 two-variable clause subsets plus 160 seeded cases; all 25
integration tests passed normally and under address/undefined-behavior sanitizers.
The smoke benchmark covers both backends and all feature modes. See
[usage and correctness notes](doc/orchestrator.md) and the
[benchmark report](doc/orchestrator-benchmark.md) for evidence and limits.

Operational limits: analysis and checkpoint work can delay deadline polling;
checkpoint durability assumes local POSIX filesystem semantics. Whole-run memory
is not capped by the canonical-index limit, and reported peak RSS covers the
coordinator. Large-instance performance, Linux execution, and physical power-loss
validation are not established by the local test results.

Use the PRD's notation throughout code documentation and tests: `F`, `C`,
`C_root`, `R(C)`, `G(C)`, `O`, `r`, `C_1`, `C_0`, `K(C)`, `h(C)`, `π_C`, and
representative cube `D`. Preserve the distinction between cube states, run
outcomes, scheduler commits, and durable checkpoint commits.

Complete phases in order and satisfy each validation gate before enabling the
next feature. Keep the legacy build and invocation paths available throughout.
The final gate is every acceptance criterion in PRD Section 10, exercised with
both conquer backends. No universal performance improvement is required.

## 2. Architecture and repository boundaries

Use a new C++ orchestration executable, proposed name `cnc`, with a single owner
of scheduler state and isolated solver processes. Start with one worker; add
bounded concurrency after the serial lifecycle passes its correctness gate.
Confirm the compiler standard against pinned dependencies in Phase 0.

The following new paths are proposed; they are not existing repository components:

| Proposed area | Responsibility |
| --- | --- |
| `orchestrator/problem.*` | Immutable `F`, DIMACS validation, input identity, residual construction, model reconstruction |
| `orchestrator/cube.*` | Cube records, typed IDs, lifecycle states, branch obligations |
| `orchestrator/scheduler.*` | Dispatch, atomic logical transitions, deadlines, retries, completion propagation |
| `orchestrator/adapters/` | iglucose, ilingeling, and isolated march look-ahead interfaces |
| `orchestrator/symmetry/` | CNF graph encoding, dejavu integration, generator validation, orbital branching |
| `orchestrator/canonical/` | Exact canonicalizer adapter, canonical bytes/mappings, bounded duplicate index |
| `orchestrator/checkpoint/` | Versioned serialization, integrity validation, durable publication, restoration |
| `orchestrator/main.*` | CLI, configuration validation, signals, outcomes, diagnostics |
| `tests/orchestrator/` | Small fixtures, brute-force oracle, fake workers, integration and fault-injection tests |
| `doc/` | Architecture decisions, correctness argument, checkpoint format, benchmark report |

Inspect and modify existing interfaces narrowly:

- `march_cu/march.c`, `lookahead.c`, `solver.c`, and `cube.c`: identify how to
  request a single look-ahead decision for `R(C)` while retaining the existing
  heuristic. Keep global solver state confined to a worker process if necessary.
- `iglucose/core/Main.cc` and `lingeling/ilingeling.c`: verify single-cube input,
  result/model output, exit codes, interruption, and internal threading behavior.
- `build.sh` and existing solve scripts: add an optional build/run path without
  changing legacy defaults. Avoid unrelated script cleanup.
- `run-test.sh` and `tests/`: retain the current smoke test and add a dedicated
  automated harness; existing benchmark files alone are not a correctness oracle.

Process boundaries must return explicit results and model data. Do not infer a
cube result from aggregate incremental-solver progress or assume that current
binaries expose every adapter operation required by this plan.

## 3. Phase 0 — Baseline and interface decisions

- [x] Record supported platforms, toolchains, legacy build behavior, and baseline
  SAT/UNSAT results on small validated fixtures and representative existing inputs.
- [x] Audit adapters and identify minimal additions needed for per-cube solving,
  full SAT models, single look-ahead selection, cancellation, and worker cleanup.
- [x] Define model, result, and error contracts before adding scheduler logic.
  Decide whether adapters submit `F ∧ C` or `R(C)`; either must retain enough
  mapping data to validate models against `F ∧ C`.
- [x] Pin dejavu; verify license, build requirements, and usable generator APIs.
  Separately select an exact canonicalizer after checking its supported graph
  colors, exactness, mappings, interruption, license, and build requirements.
  Do not substitute an orbit signature for canonicalization.
- [x] Finalize CLI names/defaults for controls left open by PRD Section 8, numeric
  limits, failure retry allowance, cancellation grace period, and exit codes.
  Preserve the required checkpoint/resume option names and 60-second default.
- [x] Define root depth as zero and generated-cube counting consistently, including
  terminal children. Decide/document the initial root branching policy using the
  same split contract as recursive timeout branching.
- [x] Document supported filesystem durability semantics and version-compatibility
  checks. Keep the optional dependency build separate from legacy builds.

**Deliverables:** interface contracts, dependency/version decisions, CLI table,
and reproducible baseline commands/results.

**Gate:** legacy baseline established; no unresolved dependency capability or
adapter issue prevents exact canonicalization, model validation, or cancellation.

## 4. Phase 1 — Root storage, lifecycle, and first backend

- [x] Implement immutable root storage and normalized cube records with stable
  original variable IDs. Separate decisions, propagated assignments, and reasons.
- [x] Implement deterministic residual construction and reconstruction trails.
  Handle conflicting assumptions, empty clauses, empty formulas, and unused
  variables before launching a worker.
- [x] Add the iglucose adapter first, with isolated temporary files and strict
  parsing of SAT, UNSAT, unknown results, malformed output, and execution errors.
- [x] Implement every PRD cube state explicitly, including `PENDING_SPLIT` and
  reason-bearing `UNRESOLVED`. Keep SAT/UNSAT/UNKNOWN/PAUSED/ERROR run outcomes
  separate from the cube state type.
- [x] Serialize scheduler events under one owner. Associate output with run,
  session, cube, and attempt IDs; commit each attempt outcome at most once.
- [x] Add monotonic attempt deadlines measured from dispatch. Cancel, terminate
  after the grace period, and reap workers; ignore output from cancelled attempts.
- [x] Persist the logical timeout decision before processing its pending split.
  Add ordinary look-ahead children `C_1 = C ∧ v` and `C_0 = C ∧ ¬v` atomically.
- [x] Enforce depth and generated-cube limits before creating children. Preserve
  a blocked split obligation rather than dropping work or declaring UNSAT.
- [x] Propagate terminal results through the tree; validate SAT models against
  `F ∧ C`. Root UNSAT requires every required branch to be discharged.
- [x] Add bounded failure retries, structured diagnostics, and cancellation cleanup.
  Introduce bounded worker concurrency only after serial tests pass; include
  backend-internal threads when defining the documented resource policy.

**Gate:** brute-force agreement on small CNFs, recursive timeout tests with fake
workers, race tests, limit tests, and no orphan workers after graceful cancellation.
A crash or timeout must never become UNSAT.

## 5. Phase 2 — Verified symmetry and orbital branching

- [x] Implement one versioned colored graph encoding shared by symmetry and exact
  canonicalization. Preserve clause/literal kinds, polarity, and variable pairs.
- [x] Add bounded dejavu analysis on each eligible split. Validate generators
  against `R(C)` before forming `G(C)`; incomplete discovery may use a verified
  subgroup but unverified generators must never justify pruning.
- [x] Select the largest nontrivial orbit, break ties by smallest original ID,
  and choose the smallest-ID orbit representative `r`.
- [x] Create `C_1 = C ∧ r` and `C_0 = C ∧ (⋀_{x ∈ O} ¬x)`, recording orbit and
  generator evidence. Propagate both children before queueing them.
- [x] Fall back to existing look-ahead selection when symmetry is disabled,
  unavailable, unusable, or times out without usable verified generators.
- [x] Document the satisfiability-preservation argument and why these children
  are not an exhaustive assignment partition. Reject unsupported proof-mode
  combinations before beginning a run.

**Gate:** exhaustive parent/child satisfiability checks, known orbit fixtures,
new residual-symmetry fixtures, invalid generator rejection, disabled-feature
checks, and bounded-analysis fallback tests.

## 6. Phase 3 — Exact canonicalization and result reuse

- [x] Implement `K(C)`, `h(C)`, and `π_C` with consistent normalization of clauses
  and literals, retaining reconstruction information for absent variables.
- [x] Bound canonicalization work; incomplete results remain unavailable and do
  not block ordinary solving. Save encoding/canonicalizer version metadata.
- [x] Use hashes for candidate lookup only. Require exact canonical-byte equality
  and verify `π_D⁻¹ ∘ π_C` before registering equivalence with representative `D`.
- [x] Transfer SAT assignments through the inverse variable mapping, restore the
  target cube's assignments, and validate against `F ∧ C`. Reuse UNSAT only from
  a completed equivalent subproblem.
- [x] Register lookup and dependency insertion atomically. Reject cycles in the
  combined parent-to-child and alias dependency graph, including ancestor matches.
- [x] Handle queued, running, pending-split, and split representatives without
  duplicate dispatch. Retain alias records and unresolved obligations when a
  representative cannot complete; avoid repeatedly aliasing to blocked work.
- [x] Reserve index capacity before accepting entries/dependencies. Include pinned
  canonical bytes/mappings in accounting; skip new reuse when capacity is exhausted.

**Gate:** variable-renaming and clause-order invariance, polarity distinctions,
forced hash collisions, correct SAT model transfer, representative splits/failures,
dependency-cycle rejection, and index-budget tests.

## 7. Phase 4 — Durable checkpoints and resume

### 7.1 Serialization and publication

- [x] Define a versioned manifest and logical snapshot schema covering every item
  in PRD Section 7.2. Serialize no pointers or reconnectable process identities.
- [x] Save original DIMACS bytes once, with a cryptographic digest. Include cube
  records, queue order, child/alias edges, split evidence, terminal models,
  canonical data, configuration, RNG state, counters, and ID allocation state.
- [x] Acquire an exclusive directory lock for the whole session. Reject existing
  runs on fresh creation; allow continuation only through `--resume`.
- [x] Commit an initial checkpoint before dispatch and periodic snapshots at the
  configured interval. Copy logical state at a scheduler event boundary; publish
  no partial split, alias registration, or result-propagation transaction.
- [x] Write and flush a temporary generation, verify checksums, then atomically
  publish its manifest and flush directory metadata as required. Preserve the
  newest and previous valid generations, plus their shared root data.
- [x] Keep checkpoint data outside disposable worker storage. Initial save failure
  prevents dispatch; periodic failure preserves the old snapshot and reports age.

### 7.2 Pause and restoration

- [x] Route SIGINT/SIGTERM to the scheduler: stop dispatch, cancel/reap workers,
  reconcile accepted results, and commit a final consistent snapshot. Report
  PAUSED and the resume command only on successful pause; retain a winning
  terminal decision and report save failures separately.
- [x] Validate integrity, versions, root digest, configuration, IDs, references,
  complete branch obligations, and graph acyclicity before launching any worker.
  Ignore unfinished generations; visibly recover a previous valid generation if
  the newest is corrupt. Fail without altering saved state if none is valid.
- [x] Rebuild canonical lookup structures and validate saved SAT models. Preserve
  terminal cube results and pending aliases. Complete parent/alias reconciliation
  before deciding which work to dispatch.
- [x] Assign a new session ID. Requeue interrupted non-timeout attempts with fresh
  attempt IDs/budgets; process recorded pending splits instead of restarting their
  timed-out attempts. Preserve retry counts and limit-blocked reasons.
- [x] Restore RNG state and accumulated recorded active time; exclude downtime.
  Document that crash recovery can lose work/timing after the restored snapshot.
- [x] Allow only PRD-approved overrides: worker count, checkpoint interval, and
  increases in depth, generated-cube, or index-memory limits. Reject backend,
  feature, seed, version, and budget changes.
- [x] Return saved terminal SAT/UNSAT outcomes without workers. Resume remaining
  obligations for UNKNOWN/PAUSED/ERROR when runnable; otherwise report their block.

**Gate:** repeated pause/resume with both ordinary and orbital splits and aliases;
forced termination at publication boundaries; disk-full/corrupt/truncated data;
concurrent resume rejection; stale output rejection; input/version mismatch;
resume without original input or temporary files; completed cubes never redispatched
when their results exist in the restored generation. Simulated failures do not
substitute for documenting platform-specific filesystem assumptions.

## 8. Phase 5 — Second backend, packaging, and release validation

- [x] Add the ilingeling adapter behind the same contracts; repeat lifecycle,
  model, cancellation, deadline, and resume tests. Avoid multiplying scheduler
  concurrency by unrestricted backend-internal worker counts.
- [x] Add optional build targets and new-workflow CLI help. Verify legacy builds
  without dejavu/canonicalizer dependencies and existing script invocations.
- [x] Document fresh solve, checkpointed solve, graceful pause, resume, permitted
  overrides, UNKNOWN/error diagnosis, and unsupported proof combinations.
- [x] Finish structured counters for branching, canonicalization, solver attempts,
  memory, checkpoint generations, resume sessions, and accepted overrides.
- [x] Run the correctness matrix across both backends, serial and bounded parallel
  workers, symmetry on/off, duplicate suppression on/off, and resume on/off.
- [x] Benchmark legacy, symmetry-only, duplicate-suppression-only, and combined
  modes on fixed symmetric/asymmetric SAT/UNSAT inputs. Measure checkpoint overhead
  separately. Record environment, versions, seeds, limits, timeouts, wall time,
  memory, phase costs, checkpoint size, and save/restore latency.

**Gate:** all PRD acceptance criteria pass; remaining limitations and performance
regressions are documented; no correctness failure is waived for speed.

## 9. Requirement-to-validation map

| PRD requirement | Implementation phase | Required evidence |
| --- | --- | --- |
| Section 2: compatibility and boundaries | 0, 5 | Legacy builds/runs; optional dependencies; proof-mode rejection |
| Section 3: root and cube representation | 1 | Reconstruction, propagation, boundary fixtures, shared-root storage |
| Section 4: branching | 1, 2 | Look-ahead reuse and exhaustive orbital satisfiability checks |
| Section 5: recursive splitting | 1 | Fake-worker timeouts, races, limits, retry/error propagation |
| Section 6: exact duplicate suppression | 3 | Collision, mapping, dependency, and memory-limit tests |
| Section 7: checkpoint/resume | 4, 5 | Crash injection, restore invariants, repeated resume with both backends |
| Section 8: controls and diagnostics | 0–5 | CLI validation, configuration records, resource and output checks |
| Sections 9–10: deliverables and acceptance | 5 | Completed test matrix, usage docs, reproducible benchmark report |

Release only after the implementation, automated tests, dependency records,
correctness argument, checkpoint format documentation, usage examples, and
benchmark report are present and agree with the PRD.


## 10. Release acceptance follow-up (completed 2026-10-10)

The earlier blanket completion marks were audited and the following gaps were
closed before restoring the completion status:

- [x] Propagate both children before publishing their queue records. Conflict
  and satisfied children are terminal immediately; split publication is atomic
  with respect to checkpoints.
- [x] Use march's look-ahead decision, or validate its terminal result. A failed,
  timed-out, or malformed query leaves the cube UNRESOLVED. The march branch
  record starts on its own line; no arbitrary-variable fallback remains.
- [x] Restore QUEUED, RUNNING, PENDING_SPLIT, SPLIT, WAITING_EQUIVALENT, and
  UNRESOLVED states on both real conquer backends for SAT and UNSAT fixtures.
  Test-only stop/snapshot hooks and a controlled branch query ensure the states
  are reached; separate tests cover the real march protocol.
- [x] Test analysis dependency failures, ancestor/alias dependency traversal,
  failed representative release, generated-cube limits and increased limits on
  resume, and measured worker/index limits.
- [x] Run both legacy scripts on SAT/UNSAT inputs outside the repository with
  spaces in input paths, verify temporary-file cleanup, and preserve option
  forwarding. The scripts resolve their own repository and no longer need a TTY.
- [x] Pass 25 integration tests normally and under address/undefined-behavior
  sanitizers, the exhaustive/seeded C++ unit suite, three Ramsey encoding tests,
  and two completed 39-vertex control tests on both real backends.

The requested [Ramsey example](tests/ramsey/README.md) now tests only the
`24 + 1 + 4 + 3 + 3 + 2 + 2` decomposition formula. A seed-only extension remains
available as a separate generator mode and in historical exploratory results.
Its confirmed target is R(5,5,39) with Hk = Kk; the fixture joins the singleton
to the seed. Both backends pass bounded fresh/resume workflow checks. The
selected-seed 39-vertex outcomes are UNKNOWN at the configured cube limits;
they do not establish that this seed extends to 39. Earlier 25-vertex SAT
controls and a separate known 39-vertex SAT control independently validated
graph models; these are not part of the decomposition-only run. See the
[results](doc/ramsey-example.md).

Larger-scale solve performance, Linux execution, and physical power-loss behavior
remain documented validation limits, not additional release requirements.

The subsequent continuation reached 127 cubes in each enabled-feature Ramsey
case. Decomposition runs ended UNKNOWN at the cube limit; seed-only extension
runs paused durably at the 60-second deadline. Preserved work and worker/index
bounds were verified. These deeper checks still do not resolve either 39-vertex
instance; see [continuation results](doc/ramsey-example.md#deeper-continuation).

The user subsequently required a final SAT/UNSAT decision for the selected-seed
39-vertex examples. This additional mathematical acceptance gate is still open:
both selected-seed formulas remain unresolved after bounded CnC and independent
SAT checks. The [decision status](doc/ramsey-example.md#final-decision-acceptance-status)
is reported separately from completed CnC implementation validation.
