# Bounded formal single-flow pipeline model

## Two-flow correlated admission model (new experimental API)

`ConcurrentPipelineFlows.tla` models two independently keyed flows across five
directional admission hops, including reservation, publication,
per-flow cancellation and completion. The safe config intentionally permits
terminal quiescence (`CHECK_DEADLOCK FALSE`). It checks no late admission,
no duplicate accounting, no pending reservation on terminal states and
no premature completion. Both late-publish and double-commit mutant configs
must produce counterexamples.

This is a bounded **host-state abstraction** only: it does not yet prove
`SourcePipelineFlows.c` refines TLA+, much less the production
`SourceActorBootstrap` callback/worker/credit integration. Physical
delivered-versus-admitted state, graph routing, fairness, futures and
incarnation restarts remain separate models/tests. CI must test exact source
blobs and use positive + deliberate-negative model checks.

\`LinearPipelineSingleFlow.tla\` is a TLC-executable safety model of the
fixed root → source → transform → sink → transform → source admission
protocol currently used by the initial Java/GraalVM and LLVM pipeline slices.

The model distinguishes a **reservation** from the subsequent mailbox
**admission**, models terminal rejection, and admits source completion only
after five ordered admissions. Every emitted directional hop has one owned
reservation. The intentionally unsafe \`DuplicateCompletion\` mutation must
violate \`NoDoubleAdmission\`.

This is **model checking**, not a machine-checked implementation refinement.
It does not cover cancellation continuations, actor incarnation ABA, partial
network delivery, branching graphs, Future admission backpressure, or
multiple concurrent pipeline flows. Those require larger models, extra
implementation invariants, and integration tests on both backends.

## Source actor capability lifetime

`SourceActorCapabilityLifetime.tla` models one scheduler slot across three
successive public handle identities, with at most two simultaneous producer
leases. The registry resolves a caller token before pinning admission; stop
closes the producer gate before waiting for leases to drain and retiring the
incarnation. Old tokens may continue to be submitted after retirement.

Checked invariants: `TypeOK`, `NoFreeWithProducerLease`,
`NoGenerationReissue`, and `NoStaleAdmission`. The
`SourceActorCapabilityLifetimeMutant.cfg` deliberately allows a slot-only
lookup to accept an already-retired token; TLC must find its
`NoStaleAdmission` counterexample.

[Funded exact-source verification #38215738454](https://github.com/zed-pkg-test/java-app/actions/runs/38215738454)
checked 82 distinct bounded states with zero safety violations and rejected
the deliberate ABA mutant. The follow-up
[guest-compatible run #38216044075](https://github.com/zed-pkg-test/java-app/actions/runs/38216044075)
repeated the same TLC checks alongside native GCC, ASan/UBSan and source-compiled
LLVM guest fixtures.

The public `void*` handle is a non-dereferenceable *identity* represented by a
monotonically-issued 64-bit integer, not a secret-bearing authorization token.
It is valid only under the restricted compiler-owned, trusted-main entrypoint
contract: untrusted FFI must not be able to fabricate/cast integer identities
into actor references. General untrusted cross-context capabilities need
runtime-minted authority and context binding, not merely guess-resistant IDs.
This model does not prove C11 memory-order refinement, general `Future<T>`
continuations or multi-flow routing.


## Cross-table publisher callback lock-order model

`FlowCallbackReentrancy.tla` models a bounded A→B→A cross-table
publisher callback cycle. The safe configuration
`FlowCallbackReentrancy.cfg` forbids *any* flow-table operation while a
publisher callback is executing, including an unrelated table. The unsafe
`FlowCallbackReentrancyMutant.cfg` permits nested table entry and must fail
`NoNestedRegistryEntry`.

This tests the stated callback contract, not a lock-level C11 implementation
refinement. The C executable additionally attempts all seven foreign-table
operations while holding the outer flow callback mutex and verifies that
none consumes the foreign reservation.
