# LLVM strict zero-tracing-GC compiler: first lexical storage slice

**Target:** LLVM execution runs ordinary Oreslang guest data without a
tracing collector, statepoints, or stop-the-world GC pauses.

## Implemented, compiler-executed subset

```oreslang
pub fnc main(): int {
  const base: int = 6;
  val scaled: int = base * 7;
  return scaled;
}
```

Both `const` and `val` are immutable `int` locals in this intentionally
restricted LLVM compiler. An initializer is checked before its binding enters
scope. Duplicate names, use-before-initialization, forward references and
unsupported object construction fail compilation **without producing IR**.

Native IR uses `alloca i64` and `llvm.lifetime.start/end` intrinsics with
reverse-order lifetime expiration before `ret`. The compiler verifies the
module and then admits calls only to **other defined guest functions** and
`llvm.lifetime.start/end`. Unknown external calls, indirect calls and GC
strategy metadata are rejected by default; this prevents an unsupported
runtime heap/collector from becoming an implicit fallback.

Scalar `int` locals are trivial to drop and no native heap destructor is
needed for them. For nontrivial objects, the compiler must generate one drop
for each initialized owner on every normal, exceptional, cancellation and
suspension exit, respecting move state, borrowed leases and actor quiescence.
Do not equate the `llvm.lifetime` optimizer hint with a destructor.

## Actor heap backend

The separate native `DeterministicGraphArena` introduced in PR #38 owns
inline C++ slots, allows cycles through **non-owning generational handles**
and gates destruction on outstanding borrow leases. It does not yet accept
source-level Oreslang class instances or plug into the LLVM actor scheduler.

Native AOT should allocate objects in actor-owned native regions. No
ordinary Oreslang heap object may be silently allocated on a tracing
collector in strict mode. Unsupported type layouts, owning cycles, escaped
borrows or cross-actor pointers should remain compile errors.

## What zero GC means

The generated guest code has no tracing GC and hence cannot pause to run
one. This does **not** mean no scheduler stalls, memory page faults, lock
contention, synchronous destructor latency, or OS allocator work. Host
process/platform runtime components may use their own memory systems; a
fully certified no-GC OresVM requires auditing every transitive native
boundary and measuring end-to-end latencies.

## Verification

The funded CI workflow mirrors exact LLVM source blobs, builds Release CMake,
runs native executable and negative parser/ownership CTest fixtures, then
inspects emitted IR for lifetime intrinsics and absence of GC statepoints,
`malloc` and `free` calls for the scalar example. Preserve these gates
when extending the compiler to arrays, structs, classes and actors.
