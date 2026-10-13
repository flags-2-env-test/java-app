# ActorUtils pipeline spelling

The bootstrap guest frontend accepts three equivalent topology statements in
zero-argument `main`:

```ores
ActorUtils.pipe(source, transform, sink);
ActorUtils::pipe(source, transform, sink);
source pipe transform pipe sink;
```

Both function spellings also accept inline zero-argument spawns:

```ores
ActorUtils::pipe(source, spawn Transform(), spawn Sink());
```

Inline actors are owned by the pipeline and stopped during teardown. Named
references retain their existing explicit `stop` requirement. All spellings
use the same 2–16 actor route, nominal mailbox, forward/backward role, unique
reference, terminal-result and cleanup checks. `::` is a single delimiter.
If a named actor reference shadows `ActorUtils`, use `ActorUtils::pipe`.

This change extends the existing restricted source bootstrap frontend. It does
not add general native class static functions, first-class `ActorStream`
values, or `return ActorUtils.pipe(...)` factories. Those remain unimplemented;
a topology statement must be in the owning `main` function.

The fifteen-stage tests execute all three spellings through compiled Oreslang
handlers, with fourteen forward and fourteen backward hops and terminal result
30. The inline-spawn tests execute three stages and verify terminal result 19.
No handwritten guest handler is substituted.

Message domains use `define match pattern Messages as ... end`. The old
`define pattern` spelling is rejected in this LLVM frontend. See
[Actor response contracts](ACTOR_RESPONSE_CONTRACTS.md) for the proposed
separation of unary returns, streaming callbacks and transport adapters.
