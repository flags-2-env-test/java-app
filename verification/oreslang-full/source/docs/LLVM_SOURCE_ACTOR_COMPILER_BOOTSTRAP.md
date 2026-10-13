# Source actor compiler bootstrap — restricted and executable

The LLVM compiler now parses a typed/linear actor reference binding within
a zero-argument `main`:

```oreslang
define actor Worker as {
  let total: int = 0;
  pub fnc on_message(int value): int {
    total = total + value;
    return total;
  }
}
pub fnc main(): int {
  const worker = spawn Worker();
  worker.send(1);
  worker.send(2);
  stop worker;
  return 43;
}
```

The compiler generates a native constructor function reference and a checked
native actor reference (opaque to guest arithmetic), and emits source
`spawn`/`send`/`stop` calls. A fixed-capacity native scheduler holds the
actor's mailbox and executes `on_message(int)` on one admitted logical lane.
Constructor initialization is separately submitted to reserved CONTROL
before any ordinary message. No actor code runs inside a scheduler lock.

A trusted top-level `stop` drains all accepted messages, verifies the
constructor completed, retires the actor scheduler, and releases the
actor-owned native heap only after quiescence. This blocking barrier runs on
the initial trusted `main` thread, **never on a child actor's physical
carrier**. It is not an implementation of Oreslang `await`.

## Supported today

- `const ref = spawn Actor();` before ordinary scalar locals in
  zero-argument `main()`. The actor must have a compiler-supported native
  scalar field layout.
- `ref.send(int_expression);` when that actor declares
  `pub fnc on_message(int n): int`. Submission is O(1) to a bounded
  ring, with no per-message allocation. Overload fails closed in this
  prototype; a typed overload result is required before production use.
- `stop ref;` exactly once per spawned ref before returning. Accepted
  messages complete before native heap retirement.
- Multiple actor refs sharing an actor declaration have independent heaps,
  scheduler domains, and mailboxes.
- Fail-closed type checks for undeclared actor types, missing handlers,
  unknown refs, scalar misuse of actor refs, shadowing, double stop,
  missing stop, and attempted `await` syntax.

## NOT implemented

- `await spawn Worker()`, `await ref.get_ready()`, or
  `await ref.get_done()`. These must return real futures and resume on a
  *new admitted actor/supervisor turn*, not block a carrier.
- Reply futures for methods, typed overload results, cancellation of a
  suspended actor, deferred cleanups, typed panic restart/escalation
  policies, `shared actor` vs trusted `isoactor` source lowering,
  and guest-source `http.listen`.
- General actor type parameters/field layouts, inter-actor transfer of
  owned references, or passing ActorRefs through functions.
- Automatic integration with the existing native supervisor CONTROL
  Future/terminal-failure ABI. Those runtime tests pass separately, but
  the compiler does not yet lower source futures to them.

The native source bootstrap validates actor initialization and finalization
but does not certify production scheduling, ABI portability, full no-GC
behavior, or all actor-lifecycle failure paths.

## Next compiler work

1. Add typed `ActorRef<T>`/Future representation independent of scalar
   `int`, and actor expression forms in the AST.
2. Compile `spawn` to a supervisor-owned ActorGroup reservation whose
   initialization completion creates a Future, rather than the current
   trusted-main-only bootstrap.
3. Generate continuation state machines for `await` and schedule their
   resumption through actor-exclusive admission; enforce no outstanding
   mutable loans across suspension.
4. Compiler-generated defer/finally on cancellation and panic, then publish
   failure in a reserved parent CONTROL envelope.
5. Trusted shared/private-isolated ownership modes; common guest HTTP
   listener and equivalent adapters in LLVM and GraalVM.
