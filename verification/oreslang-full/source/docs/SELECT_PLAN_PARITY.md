# SelectPlan / DynamicSelectPlan: native parity gate

**Status:** not implemented in the LLVM backend. This document is the
required behavior contract, not an assertion of native feature support.

The Java reference backend defines:

- **Static select**: reevaluate channel/value case expressions on every
  execution. Any future plan-hoisting optimization must prove expression
  purity, binding stability, actor ownership, and hot-reload invalidation.
- **Immutable SelectPlan**: reuse an immutable case descriptor list and its
  fairness cursor. Each operation must have fresh winner/cancellation state.
- **DynamicSelectPlan**: keyed insertion-ordered cases with atomic put/remove
  mutation and immutable, versioned snapshots. Replacement retains key
  position. Pending operations remain bound to the old snapshot. Results
  with a stable case key must also identify the snapshot version.
- **Fair** policy rotates among ready cases; explicit priority is ordered;
  random policy is opt-in. Close, rendezvous, cancellation, losing-arm
  detachment and default dispatch must share atomic arbitration.
- **Actor isolation**: plans and channels are actor-domain-local runtime
  capabilities; never serialize/share them through mailboxes.
- **Allocation/performance**: reusing case descriptions is not reusing a live
  channel waiter. Do not claim zero allocations or a speedup until native
  benchmarks demonstrate it.

## Native implementation acceptance requirements

1. Typed channel cases (read/write/default), copy/ownership rules, and
   source AST/HIR support for both `select { ... }` and `select from plan`.
2. Native actor-owned select registration with atomic exactly-one-winner,
   cancellation, close, wake-up, and stale-generation protection.
3. Immutable-plan and mutable-plan snapshot storage with versioned keyed
   results; race-safe add/remove/rebind and bounded allocation behavior.
4. Differential conformance against Java: fair/priority/random ordering,
   old/new snapshot isolation, default, two-sided rendezvous, pending
   removal, cancellation, channel close, actor teardown and hot reload.
5. Native memory/race sanitizer coverage and measured benchmarks versus
   ordinary static and dynamic selects, for constant and mutating case sets.

The current bootstrap parser supports only integer-returning functions.
The negative `unsupported-static-select` and
`unsupported-dynamic-select-plan` fixtures require **nonzero exit with
no LLVM IR emitted**, preventing premature claims of parity. Do not loosen
these tests until the relevant native feature and its conformance suite are
implemented.
