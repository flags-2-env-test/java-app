# Correlated actor flow admission — implementation slice

Status: **host-owned state registry and native regression only; not enabled
in LLVM guest pipelines**. Stacks on generation-safe SourceActor ref PR #239,
which in turn stacks on MessagePack/pipeline PR #180.

## Invariants and trust boundary

- `SourcePipelineFlows.c` owns up to 128 independent flow slots. A slot may
  be reused, but the process-global atomic monotonically issued 64-bit flow
  and message IDs are not recycled even after table destroy/recreate. Exhaustion
  fails closed. They are identifiers, not unforgeable bearer capabilities.
- The tuple (pipeline ID, route generation, flow ID, causal parent, directional
  stage, destination scheduler domain + actor ID + generation) is bound before publication and
  compared against the outstanding host-issued reservation. Guest MessagePack
  frame bytes remain `[2,schema_id,sha256(shape),payload]` unchanged.
- Cancellation and **nonblocking** physical DATA enqueue are linearized under
  a **per-flow** mutex. A blocked publisher for flow A cannot hold the directory
  mutex and stall unrelated flow B; retirement of a busy publisher returns
  `ORES_FLOW_BUSY` instead of waiting under the directory lock. Direct
  same-table callback reentrancy fails closed with `ORES_FLOW_REENTRANT`.
  Publisher receives a detached copy of the validated ticket. Duplicate
  logical actor IDs across reincarnations in one scheduler domain are rejected
  during flow registration.
  The scheduler publish callback must itself validate
  current destination incarnation and reserve real bounded DATA capacity.
  It MUST NOT block, call registry APIs, run guest code or acquire the group
  mutex in the opposite lock order. Same-flow cancellation still serializes
  behind an executing publish callback: a blocking callback would violate this
  liveness contract. When integrating, establish a total lock order or fold
  the final admission/cancellation linearization point into the scheduler's
  existing group mutex; avoid a callback lock inversion.
- Cancellation affects one flow. Already admitted DATA does not retroactively
  disappear; late delivery must be fenced by a separate flow cancellation epoch
  when this table is integrated with scheduler-owned DATA turns.
- `finish` checks the expected number of admitted directional hops but **does
  not prove physical turn finalization**. The guest adapter MUST join finalizers,
  check reply ownership, and settle an actual typed Future before calling it.
- Flow IDs, source actor IDs, and route generations are **trusted host
  identities, not secret-bearing tokens**; never expose these C functions to
  untrusted FFI or infer authority from their numeric values alone.
- The table is not safe to destroy until all producers have been fenced and
  joined by the scheduler. Do not attempt stop-on-free from actor workers.

## Executable evidence

`tests/native_pipeline_correlated_flows_test.c` exercises shared routes with
80 concurrent active flows and 400 accepted typed-direction hop tickets, plus
200 cancellation-versus-publication races per run, wrong-direction/correlation/
scheduler-domain/generation/parent fencing, rejected DATA admission, full
16-stage 31-hop path and 128-slot table saturation. Additional tests stall a
callback deliberately while an unrelated flow opens, cancels, and retires;
verify `ORES_FLOW_BUSY` without global head-of-line blocking; reject same-table
callback reentry; reject duplicate logical actor IDs across generations and
missing scheduler domains; and prove ID non-reuse across table recreation. This is a C host state-machine test, not
compiled Oreslang guest execution or actual MessagePack scheduler admission.

The TLA+ `ConcurrentPipelineFlows.tla` models two concurrent flow keys with
independent reservation, commit, cancellation and completion. Its two mutants
must produce counterexamples: late commit after cancellation and duplicate
admission without reservation. It is a bounded model, **not an implementation
refinement proof**.

## Follow-on integration gates

1. Replace the global `pipeline_phase`, `pipeline_root_sent`,
   `pipeline_done`, `pipeline_result` fields with per-flow scheduler-owned
   state while retaining the current exact cancel-versus-DATA publication
   barrier. Carry the immutable context in `SourceWireDelivery`, not guest
   payload bytes; bind it to the actual incoming scheduler DATA turn.
2. Make reverse routing use per-flow causal parent and actual incoming edge,
   not the mutable actor-wide `upstream` alone. Keep `self.respond` distinct
   from `self.backward`.
3. Require generation-fenced producer leases across admission, stop, restart,
   cancellation, and all bounded credits. Reject stale stop and stale reply.
4. Compile a real 3-actor `.ores` fixture with 1000 independent requests,
   interleaved upstream/downstream messages, selected cancellations, and typed
   replies. Verify exactly one logical completion per request and zero leaks.
5. Expand TLC to model scheduler credits, finalizers, Future suspension,
   generation reuse, and implementation trace refinement. Run full LLVM
   CMake/CTest, Java Maven/JVM and Graal AOT/hybrid differential suites.
6. Maintain exact Git blob verification when executing CI on a funded mirror;
   do not claim full release parity or merge these drafts beforehand.

Schema: `contracts/v2/pipeline-flow-context.schema.json`.
Companion TypeSpec: `contracts/v2/pipeline-flow-context.tsp`.


## Follow-up audit and funded exact-source evidence (2026-10-12)

**Concrete repairs:** replaced callback-held global table mutex with per-flow
mutexes and nonblocking try-lock retirement; changed IDs to process-global
overflow-fenced atomic counters; bound publisher tickets to immutable
compiler-issued copy; made same-table callback reentry fail closed; added
scheduler-domain identity and rejected logical actor duplicates across
incarnations. The original bounded CTest and source-level guest pipeline
have not been reclassified as real multi-flow runtime integration.

- [Funded hardened race CI #38271623756](https://github.com/zed-pkg-test/java-app/actions/runs/38271623756):
  exact Git blobs, 10 repeated strict GCC ASan/UBSan tests, 20 optimized
  Clang repetitions, TLC and two expected counterexamples, JSON Schema and
  TypeSpec all green. Includes blocked publisher and recreated-table tests.
- [Final scheduler-domain conformance CI #38272018230](https://github.com/zed-pkg-test/java-app/actions/runs/38272018230):
  exact newer Git blobs, same sanitizer/Clang/formal gates plus
  cross-domain forgery and explicit `schedulerDomain` JSON Schema/TypeSpec.
  All executed jobs succeeded.
- TLC remains a **bounded abstract safety model** of two flow keys, not a
  machine-checked C implementation refinement. The C race test proves
  independence against a deliberately blocked publisher **on another flow**,
  not progress for the publishing flow during an indefinitely blocking
  callback.

**Release blockers:** publisher must never block on guest work or peer
completion; scheduler integration must own source and destination generation
leases, mailbox credit transfer, callback/lock ordering, cancellation epochs
in DATA deliveries, physically finalized turns, typed replies/Future<T>,
and safe producer fencing before destroy. The current C registry alone
cannot safely authenticate untrusted FFI, preserve live message state after
a cancelled flow, or prove cross-backend semantics. Do not merge this draft.

The final topology guard rejects distinct scheduler domains within the same
linear flow before reservation or DATA admission. This is the deliberately
restricted single-scheduler implementation; cross-scheduler routing requires a
future explicitly authorized bridge rather than matching only numeric IDs.

## First real LLVM guest DATA context bridge (2026-10-12)

PR #240 now carries **host-issued routing provenance on actual scheduler DATA
envelopes**, not inside application MessagePack bytes. The source compiler's
existing `pipeline(source, transform, sink)` fixture lowers genuine
`self.forward` and `self.backward` guest operations, and
`SourceActorBootstrap` binds the accepted message's flow ID, unique
process-lifetime message ID, causal parent, destination scheduler domain,
and physical actor generation at the same locked DATA publication point.

The root gets a flow identity only when bounded scheduler admission succeeds;
malformed/rejected root mail cannot burn the one-use admission credit. Worker
callbacks read their immutable ingress context, check current actor physical
identity, and thread flow/message ancestry through a worker-local scoped
context. The existing single-flow causal phase and cancellation/publication
barrier remain mandatory. Each accepted DATA emits a testing-only provenance
record **after** successful bounded admission.

The CTest `native_exec_three_actor_bidirectional_pipeline` now requires
`--pipeline-context-summary` as well as final state, three physical STOP
receipts, and group drain. It verifies exactly five directional DATA records:
root ingress → Source forward → Transform forward → Sink backward →
Transform backward. Each record must match one nonzero flow ID, a fresh message
ID, strict causal-parent continuity, the intended recipient, and a consistent
nonzero scheduler domain; actor incarnations must be nonzero. Mutation tests
reject missing, duplicate, foreign-flow, forged-parent, replayed, or
cross-domain records.

**Funded original project verification**:
[LLVM full CMake/CTest run #38275613367](https://github.com/zed-pkg-test/java-app/actions/runs/38275613367)
passed **163/163 CTests** with exact Git blob identity for **165** current
`CMakeLists.txt`, `src/main/`, and `tests/` files, including the real
compiler-generated guest execution and original TLC root-credit mutant.
CI mirror: [zed-pkg-test/java-app PR #203](https://github.com/zed-pkg-test/java-app/pull/203).
A later mirror-only change refreshed the capability-hardening workflow's
previously stale source blob pin; it did not change the tested LLVM runtime
or the source tree.

**Important unresolved scope:** the provenance bridge is currently a *single*
flow in one linear chain. It is NOT yet a call into `SourcePipelineFlows`,
does not permit a second independent root ingress, and does not establish
concurrent route correlation. The next runtime change must unify the new
envelope metadata with the already-tested per-flow registry while explicitly
maintaining group-mutex/flow-mutex lock order, actor turn finalization,
producer leases, and bounded credits. Then introduce an opt-in compiled
two-flow fixture and expand toward 1,000 interleaved requests.


### Parent-reconciled final verification

The complete parent-stack replay after #180's optimized-Python evidence fixes
passed on funded Actions:

- [Full CMake/CTest #38276259243](https://github.com/zed-pkg-test/java-app/actions/runs/38276259243):
  **165 exact compiler/runtime/test Git blobs, 164/164 original CTests**, including
  20 actual compiled three-actor guest runs with the strict causal-admission
  verifier; the ordinary and Python `-O` mutation suites both passed.
  The single-root-credit TLC safety proof and deliberately unsafe mutant also
  passed.
- [Capability/lifetime native and compiled-guest #38276258949](https://github.com/zed-pkg-test/java-app/actions/runs/38276258949):
  exact source fingerprints, 30 strict GCC race runs, 15 Clang ASan/UBSan
  race runs, bounded TLC stale-generation counterexample, and actual
  source-compiled signed-i64/pipeline/await-ready guest programs all passed.
- Verification-only [funded mirror PR #203](https://github.com/zed-pkg-test/java-app/pull/203)
  remains draft. All three PRs (#180 → #239 → #240) remain draft/unmerged;
  reconciliation used two-parent Git commits preserving each downstream head.

These checks establish the real scheduler-owned **single-flow** causal
provenance integration and no observed regressions in the bounded compiler
slice. They do **not** establish concurrent flows, route binding to the
standalone `SourcePipelineFlows` admission registry, implementation-level
TLA+ trace refinement, or Java/GraalVM Native Image parity.


## Guest causal predecessor runtime hardening — 2026-10-12

**Defect:** the compiled LLVM `SourceActorBootstrap` bridge previously required
only nonzero predecessor IDs for directional DATA messages. The strict
five-hop transcript verifier could detect an invalid lineage afterward,
but the runtime did not enforce that the recorded parent was the last
successfully admitted predecessor.

**Repair:** under the existing `group.mutex` cancellation/publication
linearization point, every pipeline DATA admission now compares the
host-issued causal parent with `pipeline_last_message_id`. Root ingress
requires an empty lineage and a trusted-main caller, while each directional
hop must come from the physical currently executing adjacent actor in the
same scheduler domain. A rejected, malformed, cancelled, or mailbox-full
candidate does not advance the committed predecessor. The successful
scheduler enqueue atomically commits the new message identity before
another carrier may validate the delivered context. MessagePack v2 guest
frames remain unmodified. This remains the compiler-trusted single-flow
linear subset, not an implementation of the separate concurrent-flow
registry.

**Executable negative:** `native_compiled_pipeline_forged_parent_rejected`
builds the real three-actor `.ores` program, deliberately corrupts the
first source-forward turn's host-local parent before acquiring
`group.mutex`, and checks recovery status 73
(`SOURCE_PIPELINE_REJECTED_MAIL=3`), exactly one admitted root DATA
envelope, no accepted descendant, and full three-actor physical teardown.
It repeats 20 times and never links the fault hook in production builds.

**Funded exact-source native evidence:**
[Actions #38279710980](https://github.com/zed-pkg-test/java-app/actions/runs/38279710980)
passed **165/165 original LLVM CTests** and matched **167/167 source/test
Git blobs**, including 20 real guest forged-ancestry negatives and the
existing safe/unsafe root-credit TLC check. The separately refreshed
[actor-lifetime run #38279801405](https://github.com/zed-pkg-test/java-app/actions/runs/38279801405)
also passed GCC race, Clang ASan/UBSan, bounded anti-ABA model and
actual guest-compiled fixtures.

**Formal follow-up:** `PipelineCausalParentAdmission.tla` models
a bounded two-key host causal admission state machine and its optional
forged-parent transition. A safe configuration should satisfy lineage,
duplicate, terminal-state and type invariants; the unsafe configuration
must exhibit `NoForgedCausalAdmission` counterexample. This bounded
abstraction is NOT a proof of C scheduler lock-order or implementation
trace refinement. The finalized funded result must be recorded separately
after the checker actually runs.


## Callback lock-order and checked teardown audit (2026-10-12)

### Reentrancy bug fixed

The original `callback_table == table` guard prevented only immediate
same-table callback recursion. It did not stop **A→B→A** recursion: a callback
holding a per-flow lock in table A could publish into B, and B's nested callback
would overwrite the thread-local callback marker before re-entering A. A
self-deadlock could then occur under A's still-held lock.

Every registry entrypoint now refuses entry while **any** publish callback is
active on its thread, irrespective of the table targeted. The pinned ticket
and flow lock remain held until the callback completes; unrelated callers on
other threads may still progress. A deterministic C regression attempts
open/reserve/publish/cancel/finish/snapshot/retire against a second table from
inside a callback and verifies `ORES_FLOW_REENTRANT` and preservation of its
unconsumed reservation. It then publishes the second flow normally.

The bounded two-table `FlowCallbackReentrancy.tla` contract model checks
`NoNestedRegistryEntry` and `NoCallbackLockCycle`; its deliberately unsafe
nested-entry mutant must be rejected. This **does not** establish C11
implementation trace refinement.

### Teardown lifecycle bug fixed

The former `ores_flow_table_destroy` could free a table while it still held
registered active, reserved or terminal-but-unretired flows, silently losing
correlation/terminal state. The new `ores_flow_table_try_destroy` is
nonblocking: it returns `ORES_FLOW_REENTRANT` inside callbacks,
`ORES_FLOW_BUSY` for currently locked flow slots, and
`ORES_FLOW_INCOMPLETE` for any unretired slot. Only a quiescent, empty
registry can return OK and free its storage. The historical void
`ores_flow_table_destroy` now fails closed on rejected teardown rather than
silently discarding the table.

**Critical contract:** the scheduler owner must still fence and join external
producers *before* trying destruction; the check cannot create a producer
shutdown barrier from thin air. This remains experimental host-side API,
not production-integrated guest cancellation/finalization.


## Final funded causal-ancestry verification

[Exact-source Actions #38280769953](https://github.com/zed-pkg-test/java-app/actions/runs/38280769953)
verified **167/167** current `CMakeLists.txt`, `src/main/` and `tests/`
Git blobs and passed **165/165 original LLVM CTests** with zero failures,
including the compiled three-actor forged-parent rejection (20 actual
native executions), positive guest causal chain, and Python `-O` evidence
checks. The original root-credit model reached 12 distinct states and
rejected its unsafe preclaim mutant.

The separate `PipelineCausalParentAdmission.tla` bounded model reached
**400 distinct states**, with no safe-model invariant violations, and the
deliberately unsafe `AllowForgedParent` variant produced the required
`NoForgedCausalAdmission` counterexample. The mutation initially exposed
a TLA+ successor-variable specification error; the corrected model and exact
SHA were rerun successfully. This is abstract bounded model checking, **not**
a compiler/runtime implementation-level refinement proof.

[Funded actor lifetime + native sanitizer run #38280769873](https://github.com/zed-pkg-test/java-app/actions/runs/38280769873)
also passed its exact-source, GCC, Clang ASan/UBSan, TLC, and
compiled-guest gates.

These are verification gates on the checked PR #240 source revision,
not a merge authorization. The stacked parent PR #180 advanced separately
with native event traces; PR #239 must reconcile those changes before the
full dependency stack is considered integration-mergeable.


### Final funded verification after checked teardown

- [Exact-source focused CI #38280292528](https://github.com/zed-pkg-test/java-app/actions/runs/38280292528):
  all steps green. **10 GCC ASan/UBSan repetitions**, **20 Clang optimized
  repetitions**; each C test pass covers 80 independently registered flows,
  400 correlated hops, 200 cancellation/publication races, held publisher
  isolation, cross-table callback reentry, and checked destruction. TLA+
  two-flow model: **324 distinct states, zero violations**, both unsafe mutants
  rejected. The callback lock-order model: **four distinct states** without
  violations, its unsafe nested-entry mutant rejected. JSON Schema valid
  and seven negative checks passed; TypeSpec compilation passed.
- [Full original LLVM CMake/CTest #38280769953](https://github.com/zed-pkg-test/java-app/actions/runs/38280769953):
  verified **167 exact source/test Git blobs** and passed **165/165 CTests**,
  including the compiled guest forward/backward causal-admission checks,
  native correlated-flow teardown regressions, and compiled guest evidence
  under ordinary and Python optimized modes. Root credit TLC:
  **12 distinct states**, unsafe preclaim rejected. Host causal-parent TLC:
  **400 distinct states**, unsafe forged-parent mutant rejected. An
  intermediate run failed solely because the new TLA+ mutant's `forged`
  state was incompletely defined on one transition; the model was corrected
  and the final run passed.
- [Actor lifetime/ASan/compiled guest #38280769873](https://github.com/zed-pkg-test/java-app/actions/runs/38280769873):
  all exact-source capabilities, producer-stop races, ASan/UBSan,
  generation-lifetime TLC, and native-compiled guest executions passed.

The executable native registry is **still a standalone host implementation**.
The compiled `.ores` pipeline's host-issued flow context remains one-flow
only and is not yet bound to that registry's per-flow DATA admissions. None of
the bounded model results constitute a C implementation-refinement proof.
Keep PR #240 draft and unmerged until actual multi-flow compiled execution,
backpressure cancellation and cross-backend parity have executable evidence.
