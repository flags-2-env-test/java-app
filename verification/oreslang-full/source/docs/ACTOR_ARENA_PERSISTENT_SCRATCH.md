# ActorArena runtime substrate: persistent slabs and turn-local scratch

The first host-only `ActorArena` bridge deliberately exposes no native
pointer or actor turn capability to guest source. It is **not** automatic
Oreslang `ActorArena()` lowering, a production scheduler, or a complete
class/struct allocator.

## LLVM native
`ActorArena.h/.c` preallocates two distinct regions at actor incarnation
creation:

- Persistent: bounded fixed-size slots stored in a preallocated native slab.
  Allocation scans the slot bitmap/metadata, never calls malloc per object,
  and the total preallocated slab plus scratch bytes is hard capped at 64 MiB
  per arena in this prototype (the host must additionally enforce a runtime
  aggregate quota before creating multiple arenas),
  uses generation-checked actor-domain handles, and survives carrier migration.
- Scratch: bounded bump cursor with power-of-two logical offset alignment
  (1..64), reset and zeroized at the end of each admitted mailbox turn.
  Scratch handles bind to the current epoch and cannot be used after yield.

Persistent borrows carry unique loan nonces; writes/drop are denied until all
loans return. Retire revokes guest operations and fresh admissions immediately.
An already admitted turn can still leave. Physical free requires the last
turn **and** all persistent loans to finish. The trusted supervisor must
fence timers/wakers/FFI callbacks before the final destroy call.

The arena is a **separate native ABI** from `ActorOwnedI64Heap`: the runtime
does not yet route compiler-emitted actor-class fields into generic slots,
and no source-level `spawn`, `await`, `ActorGroup`, or FFI lifecycle
integration is claimed.

## Java/GraalVM
`ActorRuntime.ActorMemorySlice.newActorArena(persistentSlots, scratchSlots)`
returns an owner-checked `ActorArena`, currently **i64 scalar-only**.
It provisions two bounded FFM native regions charged against the same
actor and runtime budgets. Persistent slots survive turns. Each
`beginTurn()` returns an explicit `ScratchTurn` for scratch scalar
allocation, reads, and writes; closing it drops/zeroizes those slots in
reverse order and rejects old generations during subsequent turns.
No scratch loan is exposed.

Persistent loans may span turns but cannot be dereferenced off-actor.
A retained loan prevents physical FFM release and global native quota
reimbursement after actor shutdown. Java wrappers and scheduler objects
remain JVM-managed; the Java runtime is not zero-GC.

`ScratchTurn` currently relies on the handler closing its scope before
`await`/cooperate. Compiler-inserted scope unwinding on suspension,
cancellation, exceptions and FFI is a future compiler/scheduler task.

## Required next gates
1. Typed field layouts for arbitrary structs/classes and compiler-generated
   arena allocation, without exposing capabilities.
2. Actor supervision, cancellation, timers, and asynchronous I/O quiescence.
3. Differential Java/LLVM ownership and stale-handle tests; more extensive
   fuzz/property tests and native sanitizers.
4. Physical AOT/isolate execution and full integration performance tests.

## Hardened native fast path and turn admission
The per-actor native slab now maintains a preallocated free-slot stack:
persistent allocation/drop perform O(1) slot selection with no heap allocation
during an admitted actor turn. The turn capability is **carrier-bound** using
C11 thread-local admission state, so copying an epoch/domain token to another
native thread does not authorize concurrent access. Carrier migration is
supported only after leaving and readmitting a later turn. Supervisor retirement
still permits an admitted turn to unwind, and outstanding loan tokens remain
cross-carrier releasable.

Native malloc/calloc/free occur at actor arena open/physical destroy rather
than on the scalar allocation hot path. This is not proof that every LLVM
compiler-emitted guest operation avoids malloc: full typed source lowering and
whole-program allocation audits remain outstanding.
