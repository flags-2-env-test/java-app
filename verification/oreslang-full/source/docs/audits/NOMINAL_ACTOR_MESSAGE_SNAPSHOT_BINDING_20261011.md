# LLVM nominal actor message hardening audit — October 11, 2026

**Status:** focused exact-source native and parser CI; draft, not merge-ready. No
claim of complete LLVM guest-source tagged message code generation.

## Concrete findings and repairs

1. **Nested collection arity bypass.** `message_shape()` previously
   validated `Array[T]` and `List[T]` element cardinality only at the
   top level of enum payload declarations. Nested examples such as
   `Tuple[int, List[bool, bool]]` were incorrectly accepted as type
   declarations, although a canonical homogeneous variable sequence needs
   exactly one element descriptor. Enforce the rule at EVERY recursion depth.
   Carry one descriptor-node counter through recursive descent (100000
   nodes, nesting <=64, immediate slots <=4096). Regression tests include
   nested list/array failures and a >100000-node nested tuple attack.

2. **Variant routing read before immutable snapshot.** Previously
   `source_select_variant` parsed schema identity directly out of the
   caller's wire buffer and `ores_mp2_validate_copy` subsequently made a
   different read/copy to validate. Although the earlier code failed closed
   on mismatched pinned schema IDs, it unnecessarily inspected caller-owned
   mutable data twice. Now source send reserves a bounded DATA slot, creates
   exactly ONE owned wire snapshot, selects the compiler-pinned schema from
   the snapshot and performs recursive typed validation of THE SAME bytes.
   The `ores_mp2_validate_owned` helper validates a detached frame in
   place and does not transfer/release its ownership; every accept/failure
   path frees or moves the original owned frame exactly once.

3. **Tagged match binder scoping.** The LLVM checker treated repeated
   `_` placeholders as duplicate bindings and omitted a check for
   collisions with existing actor fields, locals and the receive parameter.
   Repeated ignored placeholders now remain unbound (e.g.
   `on E.Pair as (_, _) {}`), while repeated real names and shadowing
   existing names are diagnosed. Empty-arm static proofs STILL terminate
   in the explicit backend non-lowering diagnostic; passing static checks
   must never authorize unsupported guest execution.

## Verification

- [Exact-source native MessagePack and scheduler
  #38177517551](https://github.com/gha-indie-worker/gha-indie-worker-docs/actions/runs/38177517551)
  — trusted blob pins, GCC AddressSanitizer/UndefinedBehaviorSanitizer,
  multi-variant source actor routing, one per-actor DATA lane, legacy scalar
  compatibility, Java tagged wire vector parity, generic actor admission
  and lifecycle regressions, optimized Clang runs.
- [Nominal parser / typed match regression
  #38177517554](https://github.com/gha-indie-worker/gha-indie-worker-docs/actions/runs/38177517554)
  — SHA-verified parser/header/harness with nested-shape rejection, total
  descriptor quota, exhaustive nominal `match`, ignore-placeholder and
  binder-shadow tests under GCC sanitizer and Clang builds.

## Explicit remaining release blockers

- LLVM's source guest codegen for `define enum`, closed `define pattern`,
  typed `send`, executable typed `receive` arms, self.respond/forward/done
  control semantics and comprehensive typed actor matching.
- The native host API's in-flight producer lease only fences calls already
  entered. Callers still must prevent NEW calls and join/fence threads
  before an opaque actor/group handle is freed. This does not solve stale
  pointer use in arbitrary guest callers; a generation-fenced ref/lease
  compiler ABI is still needed.
- `Future<T>`/await, cancellation with exactly-once defer/recover cleanup,
  full LLVM CMake/CTest and Java GraalVM native-image validation, full
  source-level cross-compiler differential tests and external review.

Keep PR #180 DRAFT. Native C ABI validation and parser admission are not proof
that full Oreslang guest messaging is implemented.
