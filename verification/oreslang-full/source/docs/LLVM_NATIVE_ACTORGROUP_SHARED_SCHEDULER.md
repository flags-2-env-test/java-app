# LLVM native compiler-generated ActorGroup boundary

The restricted source compiler's `spawn`/one-way `send`/`stop` now use a
**single process-local ActorGroup scheduler** with four shared native carriers,
64 preallocated one-shot actor slots, and 128 normal messages of fixed
capacity per actor. Initializers travel through reserved CONTROL slots.
Actor heaps are independently allocated and generation-fenced; ordinary guest
turns execute on only one logical lane per actor even when different actor
slots run concurrently.

The trusted zero-argument `main` remains the group owner. `stop` rejects
subsequent sends for that actor, waits for its own outstanding accepted work,
retires its heap, and leaves unrelated actors running. When the final actor
is gone, the group destroys its single worker pool and resets for a later
top-level batch. A retired slot cannot be reused within an active group.

The wait used for `stop` is a **trusted host-only** condition-variable barrier,
not a source `await`. True suspend/resume and guest Future<T> integration,
ActorGroup supervision control policy, cancellation/finalizers and isolation
remain separate release gates. Typed reserved CONTROL and panic runtime
foundations still exist but this restricted bootstrap does not wire failures
from guest code.
