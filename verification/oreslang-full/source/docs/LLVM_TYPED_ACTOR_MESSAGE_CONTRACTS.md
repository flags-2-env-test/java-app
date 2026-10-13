# Closed actor inbound message contracts (LLVM source subset)

## Intended source shape

An actor can declare a **named closed set of inbound message shapes**.
The same declaration works at top level or within a module:

```oreslang
define module Bar as
  declare type match Patterns as
    (bool, string)
    string
    Array[string, string, string]
    List[bool, bool]
  end

  define actor Foo<messages=Patterns> as
    receive(Message m) {
      // Match/pattern destructuring belongs here in the full compiler.
    }
  end
end
```

This syntax is **provisional**: the design request explicitly said "not
exactly" and no assumption about full syntax stability or `List` positional
semantics should enter the runtime ABI until confirmed. The parser preserves
the *ordered structural shapes* including their arity. Tuple, Array and List
are distinct AST kinds, as are bool, int and string. Nested shapes parse
recursively. The contract is module-scoped when inside a module.

The actor's entire inbound data API is `ref.send(message)`: no direct
`ref.receive(...)` or arbitrary actor-method invocations. A named
`messages=Patterns` actor requires one `receive(Message m)`. The compiler
rejects unknown/duplicate contracts, duplicate variants, missing receiver,
attempted legacy `on_message(int)` in a typed actor, and mismatched sends.

## What executes now

The LLVM bootstrap's bounded message scheduler transports a single i64
payload. Consequently **only single-alternative `bool` or `int` contracts**
may currently use `send`. Their literal payload is checked before IR
generation and the generated actor `receive` method executes exclusively
on its native ActorGroup lane, including module-scoped actors.

A structural/multi-alternative contract is admitted as a declaration,
but **sending it is rejected** until a proper typed tagged-envelope
transport, lifetime/ownership validation and match lowering exist.
The `receive` entrypoint may update actor-owned fields from closed scalar
expressions, but directly inspecting `Message m` is rejected until the
pattern matcher lowers proven variants. The legacy untyped
`pub fnc on_message(int n)` code path remains for compatibility.

## Next compiler/runtime gates

1. Finalize pattern grammar, structural and `List` shape semantics.
2. Introduce an owned tagged message payload ABI (no forgeable arbitrary
   pointers, explicit maximum sizes and deep ownership semantics).
3. Lower exhaustive `match m` and type-narrow each arm, with compile-time
   diagnostics for missing/overlapping variants.
4. Dispatch `receive` through one scheduler-admitted method, preserving
   exclusive actor state access and separating the root supervisor lane.
5. Run the exact same source positive/negative pattern fixtures on
   GraalVM and LLVM, including cancellation, destructuring and nested modules.
