# Native transform pipe actor: executable status (2026-10-12)

## Implemented restricted LLVM profile

`define transform pipe actor Stage<forward_messages=F,backward_messages=B> as ... end`
declares a compiler-enforced actor role. Both input domains must name
existing closed nominal match-pattern or enum domains. The actor must define
exactly one `receive_forward(F m): void` and one
`receive_backward(B m): void`; neither generic `receive(Message m)` nor
scalar returns satisfy the contract. Compiler checks exhaustive tagged arms
within the *individual direction's domain* and rejects output cases outside
the active handler's domain.

The compiler generates two separate methods and a single runtime-facing
callback wrapper. The wrapper dispatches using the trusted ingress direction
supplied by `SourceActorBootstrap` from the scheduler's admitted DATA turn:
1 forwards from upstream; 2 backwards from downstream. Guest payload and
caller-supplied flags cannot set ingress authority. The two hooks share the
actor's one scheduler lane and state heap; no second carrier or mailbox.

For the *restricted* linear single-flow native implementation, the compiler
constructs a sorted union of every linked pipeline stage's nominal enum
cases. The union is a single identical transport schema table for all
stages, avoiding ordinal collisions between F and B. Within each handler,
the compiler validates the narrower declared domain. This internal shared
pipeline fingerprint is not a claim of independently verified GraalVM wire
parity or public message-schema evolution.

## Actual execution

`tests/fixtures/source-actor-pipeline-fifteen-{dot,colon,operator}.ores`
now compile thirteen explicit transform middle stages and ordinary source/sink
actors. The initial input is 1; 14 forward and 14 backward hops produce 30.
The native compiler's IR is compiled and run by Clang on a real GitHub Actions
runner. Each of three pipe spellings is executed five times. Seven malformed
role/signature fixtures must fail before LLVM IR.

Exact-source private funded validation:
https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/38296833465

The job verified 34 Git blob identities, built the real LLVM frontend and
passed all ten selected CTests (3 execution gates x 5 native runs,
7 negative gates). The source organization's Actions are unable to allocate
a runner (runner_id=0), so no source-org green status is claimed.

## Not yet supported

- Fixed-key `state { value: int = 0; }` and synchronous scoped
  `self.handle_state(critical |state| -> { ... })` **now execute** in the
  restricted native tagged receive arm. The callback returns a copied int;
  state references cannot escape. All three 15-actor fixtures use this
  contract. Native compilation currently supports fixed int keys only,
  not general state maps, ordinary-method callbacks, read/write loan
  upgrades or suspension-aware state access.
- This implementation only supports one nominal ingress/output domain per
  direction, not distinct forward-in/forward-out/backward-in/backward-out
  ports; four-port transformations remain to be specified.
- The Java/GraalVM source actor scheduler still requires the ordinary
  `receive(ActorMail<T>)` entry point. Java guest directional dispatch and
  full source-level runtime parity have not been established.
- Bounded one-flow synchronous native output is not general guest async
  `Future<T>` backpressure, arbitrary streams, correlated responses, or
  a guarantee that state loans can cross an `await` boundary.
- Keep the PR in draft until Java runtime dispatch, general state loans,
  full release CI and cross-backend semantic conformance are verified.

## Final hardening audit: destination, ingress, evidence (2026-10-12)

- Each emitted `Enum.Case` must belong to the *recipient's* declared
  forward/backward ingress domain. A globally shared union wire table
  is transport metadata, not authorization for arbitrary message delivery.
- Transform actors are interior stages only. The trusted root DATA ingress
  uses direction 0 and cannot be dispatched to either transform hook.
- The LLVM callback checks both trusted ingress direction and the actual
  nominal case ordinal *before* entering user code. Invalid delivery
  commits an explicit terminal route failure instead of silently returning
  a scalar and later timing out.
- Generated `_receive` callback symbol is compiler reserved.
- Three new malformed compiled pipeline fixtures reject an incompatible
  recipient domain and transform use as source or sink.
- The Python compiler/native conformance harness uses explicit runtime
  checks instead of `assert` statements, so `python -O` cannot disable
  its required pass/fail checks.

Funded private exact-source audit: 
https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/38300016507

**17/17 selected CTests passed**, including the three 15-actor native
pipeline spellings (five runs each), 10 negative compilation tests,
strict-ingress IR inspection, optimized-Python negative canary and
native execution, and three Clang ASan/UBSan executions. Exact-source
Git blobs were verified before compiler/native builds.

This is a focused **subset** gate, not full head CMake/CTest or a
Java/GraalVM executable pipeline. The source repository still lacks
allocated runner minutes; the private funded repo carries test only.

## Canonical state milestone — October 12, 2026

The new actor-data rule is enforced by the restricted native compiler:
actors may declare methods and at most one fixed-key state map, **not** ordinary
class `let`/`val`/`const` instance fields. Method-only actors are valid.
Every compiled state update executes in a scoped critical callback; a primitive
i64 scalar is copied from the callback to the outer arm. The source root actor
can return that copied scalar from an exhaustive tagged match via an LLVM PHI,
without exposing the state loan. Direct class mutation, duplicate maps, unknown
state keys, unscoped reads, and state reference escapes have negative fixtures.

[Funded exact-source GitHub Actions #38312289067](https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/38312289067)
passed **26/26 focused CTests**. These include three complete compiled
15-actor forward/backward pipelines (five repeats each), native Clang
ASan/UBSan, optimized-Python negative canary, eight new state isolation
rejection gates, method-only actor acceptance, and prior directional domain
rejections. This is not yet full-tree native suite validation. See
`docs/ACTOR_STATE_INVARIANT.md` for the exact restriction and release limits.

## Canonical handle_state capability hardening

Oreslang's canonical actor-state API is `self.handle_state`. Without
`critical`, the callback is read-only; with `critical`, the callback
may mutate actor-owned fixed-key state. Both forms produce copied
scalar results and prohibit escapes, suspension, nesting, and arbitrary
guest callbacks. LLVM IR confirms that the read-only form has no
actor-heap field-write calls.

Prototype `using state.read/write` forms remain executable as
restricted aliases but are **not** the preferred language syntax. The
actor scheduler remains single-flight: the read mode does **not**
enable parallel actor execution.

[Latest funded GitHub Actions #38314809579](https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/38314809579)
passed **57/57 focused CTests**, including canonical read/write 15-actor
roundtrips and Clang ASan/UBSan. See
`docs/ACTOR_STATE_USING_CAPABILITIES.md` for normative source syntax.
The full historical suite and Java runtime parity remain unverified.
