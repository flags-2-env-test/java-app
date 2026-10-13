# LLVM native actor mailbox and real HTTP admission

This draft introduces `NativeActorMailbox`, a preallocated, capacity-bounded
logical admission lane for a single actor incarnation. The live HTTP matrix
uses **five independent lanes and actor heaps** in both native host profiles.

## Guarantees exercised in CI

- Admission returns a FIFO ticket. Exactly one admitted handler executes per
  actor; actor turns may run on different pthread carriers on later turns.
- No guest code runs while the admission mutex is held. The lock protects
  queue metadata only; generated actor fields and native arena access happen
  *after* logical admission.
- A caller already holding an actor lane cannot recursively enter it or
  enter a different lane; host code must not hold an actor lease across await.
- Queue capacity includes the active handler **and** waiting callers. When
  full, `enter` returns false and the HTTP host returns **503** instead of
  panicking or allocating more waiter slots.
- Retirement cancels all waiting admissions. An in-flight admitted handler
  may finish and leave; later admissions fail closed.
- Physical mailbox destruction requires retirement, zero pending turns and an
  external supervisor fence/join of all callers. Destruction is not safe if
  an external thread may still attempt to enter.
- Native CTest verifies twelve queued FIFO callers in deterministic order,
  max logical concurrency of one, cross-lane reentrancy denial, capacity
  rejection, waking blocked waiters on retirement, and quiescent free.

## Shared Oreslang source executed by the server

The HTTP matrix compiles `tests/fixtures/actor-owned-fields.ores` and also
compiles `tests/fixtures/http-shared-counter-kernel.ores`. The live native
server invokes the LLVM-generated `counter_value(count)` function on each
counter request to check against the LLVM-generated `Counter.add(1)` actor
method. GraalVM independently parses and executes this exact fixture under
its Truffle guest runtime; this is **shared request computation**, not an
identical Oreslang HTTP server.

## Important limitations

This is a **blocking pthread-condvar host shim**, not the eventual fully
cooperative Oreslang actor scheduler. A waiting TCP carrier remains parked:
bounded admission does not mean backpressure-free or thread-free admission.
There are no compiler-issued actor envelopes, actor-group ownership, select,
async/await continuation lowering, remote proxy actor semantics, or production
`isoactor` source placement yet. The `private` HTTP profile still means a
distinct OS process per actor, not compiler-enforced Oreslang isolation
semantics. Native-owned state uses no tracing guest GC in this narrow compiled
fixture, but the whole LLVM language and runtime are not GC-certified.

Next backend gates: replace blocked carriers with a ready-task queue whose
only consumer executes each actor's admitted turn; implement source-level
`spawn`/actor group lifecycle and typed actor messages; lower source
`http.listen` against a common transport contract; then rerun independently
scheduled, order-balanced multi-minute benchmarks with peak RSS.
