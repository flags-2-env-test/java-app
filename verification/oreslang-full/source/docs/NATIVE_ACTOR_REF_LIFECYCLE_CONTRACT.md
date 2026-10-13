# LLVM actor scheduling and lifecycle integration contract

## Verified host substrate

`NativeActorScheduler` owns a fixed set of preallocated actor slots, bounded
ring mailboxes and a worker pool. Submitters never wait for a guest turn or
allocate a message envelope: successful `send(ref,payload)` transfers the
payload to the worker. The pool admits exactly one callback for each actor
slot. Other actor slots can execute concurrently. A callback runs outside
the scheduler mutex and should call the actor-owned native heap and arena
only during its admitted turn. Physical carrier identity is not actor
identity: a later message may run on another worker.

A host `spawn` activates a **one-shot** preallocated slot and returns
`OresNativeActorRef {scheduler_domain, actor_slot, generation}`. A ref from
another scheduler domain is rejected; stopping the ref refuses future sends.
`poll_ready` / `poll_done` are **nonblocking host state observations**.
`stop` stops new sends but drains already accepted messages; `poll_done`
is true only after the admitted turn and backlog have completed.
The group owner must fence all producers before physical destruction.

The native HTTP server registers five actor refs, dispatches accepted
connections using generation-checked send, and verifies each has finished
before releasing the scheduler, OS worker processes and native heaps.

## Required source compiler lowering (not implemented)

The Oreslang syntax is intended to remain:

```oreslang
const worker = spawn WorkerActor();
await worker.get_ready();  // Future, explicit initialization barrier
worker.send(message);      // Bounded enqueue; typed error on overload
await worker.get_done();   // Future, termination/finalization barrier
```

Do **not** lower `get_ready()` or `get_done()` to synchronous native
polling or wait on a physical actor carrier. The compiler must generate a
future whose completion is published to the actor's supervisor CONTROL
scheduler and resume the caller only on a newly admitted logical turn.

### Proposed compiler/runtime boundaries

1. Typed `ActorRef<T>` with actor ID, generation/incarnation, owner
   ActorGroup, and capability-bearing send rights; no raw actor object
   crosses an ownership boundary.
2. Host `spawn` reserves a slot and creates an actor-owned heap/arena.
   A constructor executes on that actor's admitted turn, then publishes
   `ready` through a native Future completion. Failed initialization is
   a typed terminal failure, never silent ready.
3. The actor mailbox accepts typed immutable/moved envelopes. A full
   mailbox returns a typed overload result, rather than waiting on a carrier.
4. `await` suspends the current logical turn, releases scratch borrows,
   and queues a continuation; resumption must be re-admitted on the same
   actor lane and may use a different physical worker.
5. `stop` rejects new normal mail, runs `defer/finally` and retirement
   fences, waits for loans/borrowers and publishes `done` to its parent.
6. A child panic publishes an owned failure event into a reserved parent
   CONTROL slot. Parent `on_panic` is never directly invoked on the
   failed child's carrier thread.
7. Trusted isolated actors require compiler-checked memory ownership
   and a distinct native isolation policy. The current OS-process HTTP
   benchmark placement is not proof of LLVM `isoactor` semantics.

## Release tests needed

- Cross-carrier await/resume and ready/done ordering; actor turn
  exclusivity under 1000+ concurrent sends.
- Cancellation during init, while mailbox full, across suspension, after
  actor retirement, and with outstanding FFI/GPU loans.
- Stale and foreign ref rejection, actor-incarnation rollover, typed panic
  escalation, bounded CONTROL delivery and deterministic cleanup.
- Shared-source `http.listen` conformance through native LLVM and GraalVM
  with real sockets, not just two host-specific HTTP transports.
- Peak RSS, ASan/UBSan/TSan where available, allocation budgets, GC/latency
  profile, full compiler/AOT tests and repeated sustained-load benchmarking.

This is a next-step implementation contract, not a claim of source-level
`ActorGroup`, future lifecycles, `isoactor` lowering or HTTP parity.
