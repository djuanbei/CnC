# Refactor specification: symmetry-aware Cube-and-Conquer

## 1. Objective

Extend this repository's Cube-and-Conquer SAT solver with repeated symmetry
analysis, orbital branching, recursive splitting of slow cubes, and exact
recognition of equivalent subproblems. Support checkpointing and resuming an
interrupted solve without repeating work completed in the restored checkpoint.
Preserve SAT/UNSAT correctness and the existing look-ahead workflow when the new
features are disabled.

This document specifies implementation work; it does not claim these features
already exist.

References:

- Project: <https://github.com/djuanbei/CnC>
- Symmetry detection: [dejavu](https://github.com/markusa4/dejavu)
- Algorithm reference: [Ostrowski et al., Orbital Branching (2009)](https://jlinderoth.github.io/papers/Ostrowski-Et-Al-09-TR.pdf)

## 2. Existing integration points and scope

The repository builds `march_cu`, `iglucose`, and `ilingeling` through `build.sh`.
The scripts `cube-glucose.sh` and `cube-lingeling.sh` generate cubes with
`march_cu`, append them to an incremental CNF stream, and invoke a conquer solver.
`march_cu/march.c` exposes the current cubing options.

Add an opt-in orchestration layer with clear boundaries for root-formula storage,
cube scheduling, symmetry analysis, canonicalization, and solver adapters.
Reuse the existing look-ahead heuristic and conquer engines. The new layer must
support both conquer backends; introduce it incrementally, validating one adapter
before adding the second. Process isolation is acceptable where the existing
solver state cannot safely be reused.

Out of scope for the first implementation: distributed execution, persistent
caches shared between unrelated runs, model counting or enumeration, signed-variable
symmetry, and symmetry-aware proof certificates. Preserve the existing proof workflow; reject
combinations of proof mode and the new symmetry features until compatible proof
support exists. No unrelated solver rewrite is required.

## 3. Problem and cube representation

### 3.1 Terms and notation

| Symbol or term | Meaning |
| --- | --- |
| `F` | Immutable original CNF (the root problem) |
| `C` | Cube: a conjunction of fixed literals, stored in a cube record |
| `C_root = true` | Root cube, with no fixed literals; its subproblem is `F` |
| `F ∧ C` | Subproblem represented by cube `C` |
| `R(C)` | Residual CNF after applying `C` and deterministic unit propagation |
| `G(C)` | Verified subgroup of polarity-preserving automorphisms of `R(C)` |
| `O = {x_1, …, x_k}` | Orbit of unassigned residual variables under `G(C)` |
| `r` | Selected orbit representative variable |
| `C_1`, `C_0` | True-side and false-side child cubes of a split |
| `K(C)` | Exact canonical bytes for `R(C)` under supported permutations |
| `h(C) = H(K(C))` | Hash used only to locate candidate canonical matches |
| `π_C` | Bijection from variables present in `R(C)` to canonical variable labels |
| `D` | Representative cube whose result an equivalent cube may reuse |
| Run / session / attempt | One logical solve / one process invocation of that solve / one conquer dispatch |
| Committed result | Result accepted by the scheduler; durable only when included in a committed checkpoint |
| Committed checkpoint | Atomically published, durable snapshot generation |

Use `∧`, `¬`, and `⋀` for conjunction, negation, and indexed conjunction.
SAT/UNSAT describe decision results; cube lifecycle states and run outcomes are
specified separately in Section 5. An orbit representative `r` is a variable;
a representative cube `D` is a duplicate-suppression dependency.

### 3.2 Storage and reconstruction

- Store the original DIMACS CNF once as an immutable, globally readable root
  problem `F`, with stable original variable identifiers and a root identity.
- A cube `C` represents `F` under a conjunction of fixed literals. The root cube
  has no fixed literals. Each cube must remain reconstructible throughout the run.
- Store cube ID, parent ID, depth, normalized fixed literals, creation reason,
  lifecycle state, solve-attempt ID, timing information, and result. Store its
  canonicalization status, canonical form/hash when available, and any mapping
  to an equivalent representative.
- Keep decision literals distinguishable from propagated assignments. Remove
  repeated literals; conflicting literals immediately make the cube UNSAT.
- Construct residual CNF `R(C)` by applying the cube assignments and deterministic
  unit propagation to `F`. An empty clause means UNSAT; no remaining clauses means
  SAT, subject to reconstructing and validating a full model against `F ∧ C`.
- Preserve the assignment trail and original-variable mapping needed to lift a
  residual model back to `F`. Do not use model-changing preprocessing without a
  reconstruction mechanism.
- Residual formulas may be transient or bounded caches. Do not retain a complete
  copy of the root CNF for every cube. Retain cube records until run completion
  and persist them in checkpoints so that interrupted runs can resume.

## 4. Branching requirements

### 4.1 Recompute symmetry at each split

When symmetry branching is enabled, before splitting a cube, simplify its
residual formula and run dejavu on a
faithful colored graph encoding of that formula. Recompute at recursive splits:
assignments can remove symmetries or reveal new residual symmetries.

The first version permits only polarity-preserving variable permutations.
The encoding must distinguish clauses, positive literals, and negative literals,
and link each positive/negative pair so that graph automorphisms correspond to
valid CNF permutations. Document the encoding and test this correspondence.

Validate every generator used for pruning against the residual CNF. The verified
subgroup `G(C)` is sufficient for sound branching; finding the full group is not
required. Missing symmetries may reduce performance but must not affect results.
Use a separate bounded symmetry-analysis budget. Failure, timeout without usable
verified generators, or no nontrivial unassigned-variable orbit triggers the
existing look-ahead branch selection. The identity permutation alone is not a
usable symmetry.

### 4.2 Orbital branch rule

For an orbit `O = {x_1, …, x_k}`, `k > 1`, under `G(C)`, choose a
deterministic orbit representative `r` and create two children:

- `C_1 = C ∧ r`
- `C_0 = C ∧ (⋀_{x ∈ O} ¬x)`

These children preserve satisfiability **up to symmetry**. Any satisfying
assignment either sets all orbit variables false or has a symmetry-equivalent
assignment with `r` true. They do not enumerate every parent assignment; do not
treat them as an ordinary exhaustive partition for model counting or proof output.
A SAT model from either child must satisfy `F` and that child's fixed literals.

Select the largest usable orbit, breaking ties by smallest original variable ID;
choose its smallest-ID variable as representative. Record the orbit and verified
generators supporting the split. Alternative heuristics may be added later.

When symmetry branching is disabled or no usable orbit exists, use the existing
look-ahead heuristic to select an unassigned variable `v`, then create
`C_1 = C ∧ v` and `C_0 = C ∧ ¬v`. Propagate each
child before queuing it. Every split must fix at least one previously unassigned
variable; terminal cubes must never be split.

## 5. Recursive splitting of slow cubes

Expose a positive per-attempt conquer wall-clock budget. Measure it using a
monotonic clock from worker dispatch, including adapter startup; do not include
queue wait or symmetry/canonicalization work in that budget.

When an attempt exceeds its budget without SAT/UNSAT:

1. Request cancellation and terminate the worker after a documented grace period
   if needed. Reap the process and release its resources.
2. Serialize the result/timeout decision using the attempt ID. A result committed
   before cancellation wins; late output from a cancelled attempt is ignored.
3. Split the unresolved cube using Section 4 and schedule its children. Reapply
   the same timeout-and-split policy to each child.

Timeout, crash, malformed output, and cancellation are never UNSAT. An unexpected
worker failure must produce an explicit error or bounded retry, never silently
remove work. If splitting is prohibited by depth or resource limits, retain the
cube as unresolved and report UNKNOWN if the run cannot continue.

Use these persisted cube lifecycle states:

| State | Meaning |
| --- | --- |
| `QUEUED` | Ready for preprocessing or a conquer attempt |
| `RUNNING` | Owned by an active conquer attempt |
| `PENDING_SPLIT` | Attempt ended with a committed timeout; split obligation remains |
| `SPLIT` | Children exist; awaiting their results |
| `WAITING_EQUIVALENT` | Awaiting representative cube `D` |
| `SAT` / `UNSAT` | Committed terminal decision result |
| `UNRESOLVED` | Work blocked by limits or exhausted retries; reason retained |

A timeout transitions a cube to `PENDING_SPLIT`; creation of both children
transitions it atomically to `SPLIT`. If limits prevent the split, use `UNRESOLVED`
with a pending-split reason. Graceful-pause cancellation returns a non-timeout
`RUNNING` cube to `QUEUED`; it does not consume a failure retry. Symmetry analysis
and canonicalization run while a cube is `QUEUED` or `PENDING_SPLIT` and can be
restarted if interrupted; incomplete outputs must not be published.

A split parent becomes UNSAT only when every required child is UNSAT, including results resolved through equivalence. One
validated SAT model completes the root as SAT. Report root UNSAT only after every
required branch is discharged; otherwise report UNKNOWN or ERROR when execution ends. A graceful
checkpointed pause has outcome PAUSED. Run outcomes SAT, UNSAT, UNKNOWN, PAUSED,
and ERROR are distinct from cube states; only SAT and UNSAT are terminal decision
results. UNKNOWN, PAUSED, and ERROR may leave resumable obligations.

## 6. Canonicalization and duplicate suppression

When duplicate suppression is enabled, before dispatching a new nonterminal
cube, compute a canonical representation of its residual CNF, including polarity and variable-pair structure. Normalize
clause/literal order and duplicate clauses consistently. Ignore variables absent
from the residual for decision-SAT equivalence, while retaining information to
complete their values during model reconstruction.

Canonicalization must be exact under the supported variable permutations. A
sorted list of original variable IDs, orbit signature, or hash alone is not a
canonical form. Do not assume that dejavu's automorphism interface supplies a
canonical-labeling interface: confirm the pinned dependency's capabilities and
select/document an exact canonicalizer separately if required.

Store `K(C)`, `h(C)`, encoding/version metadata, and `π_C`. Scope the index to the root
problem and encoding version.

Lookup and reuse rules:

1. Use `h(C)` to find candidate representative cubes `D`.
2. Require `K(C) = K(D)`; hash collisions must not merge cubes.
3. Verify the induced mapping `π_D⁻¹ ∘ π_C` from `R(C)` to `R(D)` before
   sharing a result. Model transfer uses the inverse mapping.
4. If the representative is UNSAT, reuse UNSAT. If SAT, map its residual model
   through canonical labels to the new cube, restore that cube's assignments and
   eliminated variables, and validate the model against `F ∧ C`.
5. If `D` is `QUEUED`, `RUNNING`, `PENDING_SPLIT`, or `SPLIT`, register a dependency on
   its eventual result rather than starting another solve.
6. UNKNOWN, errors, and timeouts are not reusable proofs. Keep dependent cubes
   unresolved or reschedule them if their representative cannot complete.

Make lookup-and-registration atomic, even if the initial scheduler is serial.
Aliases must point to a stable representative and form an acyclic dependency
structure together with parent-to-child result dependencies. Reject a match
that would create a cycle (including a descendant waiting on its own ancestor);
solve that cube independently. Never delete the cube record or its branch
obligation when suppressing its execution. A model-mapping failure must prevent result reuse and be reported.

Bound canonicalization time and index memory. If canonicalization cannot finish,
record it as unavailable and solve the cube normally. If the index reaches its
limit, stop adding entries while preserving existing dependencies. Reserve capacity
for canonical bytes and mappings before registering an alias; pinned dependency
data counts toward the index limit and must not be evicted. Performance
limits may cause missed duplicates, never speculative equivalence.

## 7. Checkpoint and resume solving

### 7.1 User-visible behavior

Allow a run to save progress periodically, pause gracefully, and resume from its
last committed checkpoint after interruption, process failure, or machine restart.
Resume operates at the cube/scheduler level: completed cube results are retained,
while interrupted conquer attempts restart from their cube assumptions. Saving
solver heaps, learned clauses, or instruction-level execution state is out of
scope for both backends.

Provide these controls in the new workflow (names below are the required public
interface; the orchestration executable name is chosen during implementation):

- `--checkpoint-dir <path>`: enable durable checkpoints in a dedicated run
  directory. Refuse to overwrite an existing run; require `--resume` to continue it.
- `--checkpoint-interval <seconds>`: positive periodic interval, default 60 seconds
  when checkpointing is enabled. Commit an initial checkpoint before dispatch;
  also checkpoint on graceful pause and run exit, including UNKNOWN or ERROR when
  the scheduler can still save consistent state.
- `--resume <path>`: restore a checkpoint directory and continue unresolved work.
  The saved root CNF is sufficient; the original input path need not still exist.
  If an input is also supplied, require its content digest to match the saved input.
  Resume implies checkpointing in the same directory. Reject combining `--resume`
  with `--checkpoint-dir`, or using `--checkpoint-interval` without either.

Define input identity as a cryptographic digest of the original DIMACS file bytes;
formatting-only input changes therefore count as mismatches. Hold the directory
lock for the entire session, including a newly created run, not just during saves.

With checkpointing enabled, the first SIGINT or SIGTERM requests a graceful pause:
stop dispatch, cancel/reap active workers, resolve result/cancellation races using
Section 5, commit a checkpoint, then exit with an explicitly documented paused
status distinct from SAT/UNSAT and execution errors. Print the checkpoint location
and a usable resume command. If the root result wins the race, save that terminal
result instead. A forced termination may lose work since the last committed
checkpoint; never promise a final save after SIGKILL or power loss.

### 7.2 Durable state and atomicity

A checkpoint must be self-contained and contain:

- Schema version, run ID, generation number, input digest, original CNF, solver
  and dependency versions, configuration, encoding version, and seed/RNG state.
- All cube records, parent/child edges, pending queue order, split decisions and
  symmetry evidence, committed results, SAT models, and model-reconstruction data.
- Canonical forms and variable mappings needed by saved equivalence dependencies,
  representative/alias edges, and enough metadata to rebuild the lookup index.
- Cumulative counters and attempt durations, timeout/cancellation reasons, retry
  counts, pending split actions, and the next cube/attempt identifiers.

Serialize logical state rather than pointers, process IDs as worker identities,
or temporary solver-file paths. Store the root CNF once per checkpoint directory;
include it in integrity checks. Residual caches may be rebuilt deterministically.

Take a consistent scheduler snapshot while serializing state transitions; workers
may continue, but their results are either included in that snapshot or belong to
a later generation. Persist a split parent, all required children, and their
scheduling obligations atomically. Persist equivalence registration and result
propagation consistently so that restart cannot lose a branch obligation.

Write a new generation to temporary storage, checksum its contents, flush files,
and atomically publish a manifest only after all referenced data is durable. Flush
the containing directory where required by the supported filesystem. Retain the
newest and immediately previous valid generations after each successful commit
(only one exists after the initial save). Never overwrite the only valid
checkpoint in place. Document the filesystem/durability assumptions.

On write failure, preserve the last valid generation and report the failure with
its checkpoint age. If the initial checkpoint fails, fail before launching workers.
A periodic failure may leave solving active with a visible warning; a requested pause must not report a successful save if the final commit
failed. Checkpoint storage is separate from disposable worker files and must not
be deleted by normal cleanup.

### 7.3 Restore and reconciliation

1. Acquire exclusive ownership of the checkpoint directory. Reject concurrent
   resumes; use a lock released by process exit, not a stale PID alone.
2. Validate the manifest, checksums, root identity, supported schema, configuration,
   and version compatibility before launching workers. Ignore incomplete temporary
   generations. If the newest generation is corrupt, report it and recover the
   previous valid generation only if its integrity and compatibility checks pass;
   otherwise fail without modifying the saved state.
3. Validate graph invariants: unique IDs, existing child/representative references,
   acyclic dependencies, and complete obligations for every split parent. Reject
   inconsistent snapshots rather than treating missing work as completed.
4. Retain committed SAT/UNSAT results. Revalidate saved SAT models against `F` and
   the relevant cube assumptions. Rebuild canonical indexes from saved exact forms
   and mappings; never infer equality from saved hashes alone.
5. Requeue interrupted `RUNNING` cubes with new attempt IDs and a new session ID;
   never reconnect to old workers or accept their output. Preserve queued work and
   alias dependencies. Complete a durably recorded timeout's pending split before
   launching another conquer attempt for that cube.
6. Recompute parent completion and resolve aliases before dispatch. Keep permanently
   failed or limit-blocked cubes unresolved unless a permitted configuration change
   or remaining retry allowance makes them runnable. A completed checkpoint returns
   its saved SAT/UNSAT result without launching workers. A checkpoint saved with
   UNKNOWN, PAUSED, or ERROR must instead reconcile its remaining obligations.

An interrupted, non-timeout attempt receives a fresh per-attempt time budget.
Downtime is excluded from solver timing; retain accumulated active time through
the restored snapshot for reporting. Work and timing after that snapshot may be
lost after a crash; document this measurement limit. Persist timeout decisions and retry counts so repeated restarts cannot
silently reset recursive-split obligations or bounded failure retries. Randomized
heuristics restore their saved state; identical parallel execution order is not
required.

The first version requires matching solver/adapter, symmetry, canonicalization,
and encoding versions; unsupported versions must fail with a clear diagnostic.
Allow only operational overrides on resume: worker count, checkpoint interval,
and increases to depth, generated-cube, or index-memory limits. Preserve backend,
feature flags, seed, algorithm settings, and attempt/analysis budgets. Reject
other changes instead of silently invalidating saved results or dependencies.
Record every accepted override and resume session in diagnostics. Resuming a
checkpoint is continuation of that run, not a cache for unrelated input files.

## 8. Configuration and operational behavior

Provide documented controls for:

| Control | Required behavior |
| --- | --- |
| New workflow | Opt-in; legacy invocations retain their behavior |
| Symmetry branching | Independently enabled/disabled |
| Duplicate suppression | Independently enabled/disabled |
| Conquer backend | Select iglucose or ilingeling |
| Conquer attempt budget | Positive duration with explicit units |
| Symmetry and canonicalization budgets | Separate bounded durations |
| Maximum depth and generated cubes | Enforced without dropping unresolved work |
| Worker count | Positive bounded concurrency; default 1 |
| Canonical index memory limit | Bound cache growth without losing dependencies |
| Seed | Reproducible heuristic choices for a fixed configuration |
| Checkpoint directory and interval | Durable saves with the controls in Section 7 |
| Resume directory | Restore validated progress under exclusive ownership |

For controls not named in Section 7, choose and document option names and
defaults before implementing the public interface. Reject invalid values early. Use isolated per-run/per-attempt
temporary paths, clean up workers and temporary files on cancellation, and retain
final diagnostics and cube records for inspection.

Log root/cube/attempt IDs, split reasons, orbit sizes, symmetry/fallback counts,
canonicalization hits and failures, duplicate executions avoided, solver results,
timeouts, maximum depth, peak memory, and time spent in each phase. Keep normal
SAT result output machine-readable and diagnostics separate. Record dependency
versions, input identity, and configuration for reproducible experiments.

Pin dejavu and any canonicalizer to a reviewed version, document licenses and
build requirements, and keep the legacy build available without optional
symmetry dependencies. Specify a narrow C/C++ adapter boundary rather than
requiring wholesale conversion of the existing C solver.

## 9. Implementation sequence and deliverables

1. **Lifecycle and baseline:** root/cube records, scheduler, first conquer adapter,
   ordinary branching, timeouts, cancellation, and model validation.
2. **Symmetry:** graph encoding, validated dejavu generators, orbital branching,
   fallback behavior, and a written correctness argument for the SAT adaptation.
3. **Equivalence:** exact canonicalizer, model mappings, canonical index, alias
   dependencies, collision handling, and bounded-resource behavior.
4. **Checkpoint/resume:** versioned durable snapshots, graceful pause, atomic
   publication, restore validation, interrupted-attempt reconciliation, and
   dependency restoration.
5. **Integration:** second conquer adapter, documented options/build instructions,
   automated correctness tests, and benchmark report.

Each stage must pass its correctness checks before enabling the next feature.
Deliver source changes, tests and fixtures, updated usage documentation, dependency
information, checkpoint format and compatibility documentation, pause/resume
usage examples, and a report comparing legacy and new configurations.

## 10. Acceptance criteria

- **Baseline compatibility:** existing build and representative legacy solve
  invocations still work; feature-disabled results match the baseline.
- **Correctness oracle:** on exhaustively enumerable small CNFs and seeded random
  instances, every completed result agrees with brute force. Validate every SAT
  model against the original CNF. Exercise both backends and all feature toggles.
- **Orbital branching:** fixtures with a known nontrivial variable orbit produce
  the two specified children. Exhaustive checks establish parent satisfiability
  iff at least one child is satisfiable. Include residual symmetries revealed by
  assignments, trivial groups, and invalid generator rejection.
- **Fallback:** trivial symmetry, analysis timeout, and dependency failure use
  look-ahead branching without changing correctness.
- **Recursive splitting:** a controllable slow worker triggers repeated splitting;
  tests cover cancellation/result races, late output, depth/cube limits, crashes,
  and the rule that unresolved work cannot yield UNSAT.
- **Duplicate handling:** renamed isomorphic residuals share execution; distinct
  residuals with deliberately colliding hashes do not. Test clause-order changes,
  polarity distinctions, model lifting, in-flight aliases, representative splits,
  and representative failure. Include a descendant/ancestor match and assert no
  combined child/alias dependency cycles or lost branch obligations.
- **Resume correctness:** for both backends, pause/resume at `QUEUED`, `RUNNING`,
  `PENDING_SPLIT`, `SPLIT`, `WAITING_EQUIVALENT`, and `UNRESOLVED` states. Resumed results must match
  uninterrupted execution and the small-instance oracle; cubes completed in the restored checkpoint
  must not be dispatched again. Test saved SAT/UNSAT terminal runs and model checks.
- **Crash consistency:** inject failure during snapshot writing, flushing, manifest
  publication, split creation, and result/alias propagation. Restore the last valid
  generation without lost obligations or false UNSAT, including after forced
  termination. Test truncated files, corruption, missing root data, and disk-full
  behavior; a failed final save must not be reported as successful.
- **Resume validation:** reject input mismatch, unsupported versions, inconsistent
  graphs, forbidden overrides, and simultaneous resumes. Resume successfully after
  the original input and worker temporary files are removed. Test fallback to a
  previous valid generation with a visible diagnostic.
- **Resume timing and limits:** verify fresh budgets only for interrupted attempts,
  persisted timeout/split and retry obligations, exclusion of downtime, ignored
  stale worker output, and continuation after permitted limit increases. Repeat
  pause/resume cycles to check counters, IDs, dependencies, and cleanup.
- **Boundary inputs:** cover contradictory assumptions, empty clauses, empty CNF,
  unit propagation, unused variables, and fully assigned cubes.
- **Resource behavior:** configured worker/cache limits hold; cancellation leaves
  no orphan workers; root storage is shared and cube records remain reconstructible.
- **Performance evidence:** benchmark fixed symmetric and asymmetric SAT/UNSAT
  inputs with legacy, symmetry-only, deduplication-only, and combined modes. Record
  hardware, versions, seeds, limits, wall time, peak memory, solved count, and
  phase costs. Measure checkpoint size, save/restore latency, and overhead with
  checkpointing enabled. Report regressions and timeouts; a universal speedup is
  not an acceptance requirement, and correctness must never be traded for performance.
