# Persistent actor-owned native heap — initial OresVM ABI

This extends the strict no-tracing-GC LLVM memory work beyond a
single function activation. `ActorOwnedI64Heap` is allocated once per
native actor incarnation, keyed by host-owned `(actorId, incarnation)`,
and remains alive across admitted actor turns and physical carrier migration.

## API and exact guarantees

- `open(owner, incarnation, capacity)`: bounded 1–4096 scalar slots,
  C-native allocation, opaque heap; domain identity is monotonic and unique
  within a process until wrap (where admission aborts).
- `enter(heap, owner, incarnation)`: verifies owner and creates a distinct
  epoch token. Only one admitted turn per actor heap; a concurrent/reentrant
  turn is rejected. The next physical thread may enter after the old one leaves.
- `new/read/write/drop`: actor-turn-only access to i64 slots through
  generational non-pointer handles. Erase zeros the slot and invalidates the
  old generation. Outstanding loans prevent drop.
- `borrow`: creates a bounded unique loan token that survives
  `leave` and may represent a suspended continuation or FFI lease.
  A loan alone cannot dereference data; it must resume an admitted turn.
- `retire`: fences new turns/guest access immediately. In-flight turns
  can still `leave`, and outstanding loans may still release.
- `try_destroy`: returns pending while an admitted turn or lease exists.
  Once quiescent, clears the actor heap and physically frees native memory.

**No tracing collector** is linked to this ABI. Native `calloc/free`
may incur allocator/page-fault stalls, but the runtime has no guest
GC roots, statepoints, reference-count cycles, or collector pauses.

## Non-negotiable security and integration gates

The native `OresActorI64Heap*` is a **trusted supervisor capability**.
The actor ID/incarnation and turn credentials are not secrets: do not
pass this C ABI directly to untrusted source guest code, FFI, or messages.
Only the scheduler may mint valid turn epochs and control retirement.
Use compiler-generated typed accessors so source programs cannot forge
heap pointers or mutate metadata.

The ABI presently enforces **serial execution** only. An actor with
`requestConcurrency > 1` or `messageConcurrency > 1` needs a documented
execution barrier and ownership rules before using it; do not assume
multiple handlers can read/write an individual native slot concurrently.
A bounded native spinlock guards short ABI control-plane critical sections;
no guest-language callback executes with it held.

`try_destroy` requires the supervisor to have fenced stale callbacks,
timers, wakers, async resumes and external references to the opaque heap.
The runtime checks turn/loan counters, but it cannot recognize arbitrary
native pointers held by dishonest FFI code. Never call into an already
freed heap; a stale raw host pointer cannot be made safe by a generation ID.

This is a **tested native runtime substrate**, not full compiler lowering.
The restricted LLVM Oreslang parser still rejects source actor classes,
actor fields, async/await, `trap`, class/struct layouts and nontrivial
destructors. The earlier compiler path lowers `owned int` into a
*function-scoped* native region; a future compiler bridge must select this
actor-persistent domain through an unforgeable ActorId execution context.

## CI

`native_actor_owned_heap_quiescence` exercises multiple turns, cross-thread
carrier migration, suspended loans, retirement pending and eventual physical
free, as well as SIGABRT rejection of foreign actor identity, stale
turn epochs, forged slot domains, drop-with-loan, duplicate loan release,
concurrent reentry and access after retirement. The funded workflow verifies
exact source Git blobs before running the full CTest matrix.
