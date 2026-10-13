# Source-level actor fields to native persistent heap (strict LLVM no-GC)

The LLVM compiler accepts this intentionally restricted actor profile:

```oreslang
define actor Counter as {
  const base: int = 40;
  val delta: int = 2;
  let total: int = 0;
  pub fnc add(int n): int {
    total = total + base + delta + n;
    return total;
  }
}
```

The parser statically checks unique actor/field/method symbols, scalar
field types, known names, no actor-field parameter/local shadowing and
closed field-initializer arithmetic. `const` and `val` fields are
immutable; `let` i64 fields can be updated inside synchronous actor
methods. Unknown and immutable write targets fail before IR emission.
Unsupported constructors, async, non-i64 layouts and arbitrary FFI fail compilation.

LLVM emits `oreslang_actor_Counter_init(heap, domain, epoch)` and
`oreslang_actor_Counter_read(heap, domain, epoch)` entrypoints. The
caller is the trusted native supervisor/scheduler, which allocates a
persistent `OresActorI64Heap` for an ActorId and incarnation, admits
one turn, calls init once, and invokes read-only methods only while an
authenticated turn remains active. The compiler does **not** expose
heap pointers to guest-language expressions.

Every actor field is initialized in a fixed slot by native
`ores_actor_i64_heap_field_init`, which rejects a second initialization.
Methods call `ores_actor_i64_heap_field_read`, which rejects missing
fields and invalid/retired turns. Compiled mutable writes use the
separately guarded `ores_actor_i64_heap_field_write`: non-mutable fields,
loaned fields and stale turn capabilities fail closed. Fixed actor fields
cannot be dropped or overwritten through the generic heap slot API, and
a generic slot cannot impersonate an initialized actor field. Method-local snapshots
are updated after writes, so later expressions see the new field value. Explicitly `owned int` method locals retain their
separately bounded native lexical arena with reverse-order drops.

This compiler path has no tracing GC statepoints, GC metadata, or
managed-memory fallback. Native `calloc/free` remain possible and
are not guaranteed constant-time. The native test harness executes
compiled source initialization and methods over successive actor turns
and retires the owned heap.

**Remaining restrictions:** actor invocation through source `spawn`,
class/struct object layout, multi-handler scheduling,
full borrow/move lowering, source-level async/trap, FFI and cancellation
are unsupported. A host ABI with an opaque pointer is not by itself
safe against an adversarial native extension; direct access must remain
confined to the trusted scheduler. The full compiler-generated actor
lifecycle and quiescence gate are separate release milestones.

Funded exact-blob CI: 51/51 Release CTest, including actual compiled
actor source execution and negative field/ownership fixtures, plus
an emitted-IR no-tracing-GC check.
