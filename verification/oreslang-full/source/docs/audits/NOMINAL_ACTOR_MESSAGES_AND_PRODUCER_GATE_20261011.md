# Nominal actor messages and producer admission — implementation audit (2026-10-11)

Status: **LLVM compiler declarations/match admission and native host transport tests green; full guest lowering NOT READY**.

## Compiler work

LLVM now records file-root `define enum` and `define pattern` as
nominal declarations separate from the older `declare type match`
structural shapes. This maintains identity across alternatives with the
same payload shape and preserves explicit positive monotonic tags,
default incremented tags, valueless cases, tuple and fixed/variable
collection payload types, pattern references and declaration order.

Limits: at most 4096 enum cases and pattern alternatives, at most 4096
immediate descriptor slots and nesting depth at most 64. Pattern
declarations can appear before their enums but all references must
resolve. Duplicate names, duplicate alternatives, invalid tags,
nonhomogeneous variable collections, excessive depth and undefined
enum variants are rejected at the compiler boundary.

The `receive(Message m)` parser now recognizes restricted
`do match m over { on Enum.Case as (...) {} }` arms. Static checks prove
exactly one arm per required closed alternative, source-subject identity,
variant membership, no duplicate/unreachable arms, and scalar versus
tuple/fixed-array binder arity and uniqueness. Nonempty tagged bodies
(including `self.respond`, state mutations, nested returns and effects)
are still rejected by this prototype. Even an exhaustive empty-body
match explicitly fails compile admission with
`LLVM tagged enum/pattern source actor requires canonical MessagePack guest lowering`.
**Do not claim source-level tagged send/receive execution.**

Existing scalar actor and boolean receive lowering remain supported and
covered by regression tests.

## Native transport and concurrency work

`MessagePackActorV2.c` now offers
`ores_mp2_group_close_admission`. A single atomic producer-gate word
contains a terminal close bit and the count of active producer calls;
CAS makes admission and close indivisible. Closure returns only after
every pre-close `ores_mp2_group_send` operation exits its
copy/validate/publish path. Afterwards new sends return rejection.

`ores_mp2_group_destroy` closes admission before scheduler retirement,
drain and final reclamation. The closure does **not** wait for accepted
actor callbacks; that is the scheduler's drain duty. Nor can an atomic
counter make a previously freed C pointer safe to call: callers MUST
fence/join all possible producer threads before releasing the group.

Concurrent work on this draft also introduced
`oreslang_source_actor_spawn_variants` and
`oreslang_source_actor_send_variant`. The native host can register
multiple trusted per-variant contracts and dispatch on one actor lane.
These C entrypoints are **not** LLVM guest-source lowering.

## Verified funded exact-source CI

- [Nominal parser and static match #38176791955](https://github.com/gha-indie-worker/gha-indie-worker-docs/actions/runs/38176791955):
  exact Git SHA pinning, GCC with ASan+UBSan and Clang optimized,
  repeat-run admission/exhaustiveness tests, legacy boolean matcher.
- [Native transport + producer gate #38176791954](https://github.com/gha-indie-worker/gha-indie-worker-docs/actions/runs/38176791954):
  SHA-checked native source and headers, GCC sanitizer loops, Clang
  optimized loops, closed multi-variant source-actor callback dispatch,
  legacy scalar actor parity, Java tagged MessagePack golden vector,
  adversarial generic actor group tests including admission closure,
  bounded queues and stale actor references.

These are focused native/standalone parser tests, not full LLVM CMake/CTest
or Java/GraalVM AOT certification of the latest integrated source trees.

## Next release gates

1. Generate canonical per-variant MessagePack bytes from LLVM-compiled
   `define enum`/`define pattern` guest source, with exact Java schema
   and shape digests. Preserve one scheduler and one DATA capacity budget.
2. Lower typed `receive` arms with verified narrowing, destructuring,
   state effects and explicit `self.respond`/`self.done` semantics;
   do not silently return expression values as actor replies.
3. Add caller-lifetime fencing to source actor stop/destroy and formal
   cancellation/cleanup ownership at pending Future/await boundaries.
4. Run exact-head full Maven/GraalVM native-image, LLVM CMake/CTest,
   source-level cross-backend semantic fixtures, fuzzing and stress.
5. Keep stacked PRs draft until full verification and review; no merges
   are authorized by these targeted results.
