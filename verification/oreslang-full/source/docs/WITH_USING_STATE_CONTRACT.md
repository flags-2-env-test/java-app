# Normative actor state effects and scoped capabilities (pre-1.0)

Updated October 12, 2026. This contract deliberately supersedes the
prototype interpretation of `using` as a function signature effect clause.

## Canonical syntax

```ores
define actor Foo as
    state { x: int = 0; }

    increment(): int with state.read, state.write {
        return self.handle_state(critical |s| -> {
            s.x = s.x + 1;
            return s.x;
        });
    }

    read_val(): int with state.read {
        return self.handle_state(|s| -> {
            return s.x;
        });
    }

    increment_using(): int with state.read, state.write {
        return using state.write(critical |s| -> {
            s.x = s.x + 1;
            return s.x;
        });
    }

    read_using(): int with state.read {
        return using state.read(|s| -> {
            return s.x;
        });
    }
end
```

This describes the **normative desired language**; no single compiler currently
executes every example above. General method-level Java/LLVM parity remains
a release gate.

## Distinct responsibilities

| Syntax | Meaning |
|---|---|
| `with state.read, state.write` | Upper bound on effects a method is permitted to execute; does not acquire state |
| `using state.read(callback)` | Compiler-recognized scoped read acquisition; lowers to sealed `handle_state` |
| `using state.write(critical callback)` | Compiler-recognized scoped write acquisition; lowers to sealed `handle_state` |
| `self.handle_state(callback)` | Sealed intrinsic, immutable read loan by default |
| `self.handle_state(critical callback)` | Sealed intrinsic, exclusive mutable write loan |
| `critical` | Exclusive logical capability/admission, not an ordinary user lock |

An unmarked callback never has permission to mutate state, including when
its enclosing function declares `with state.write`. Conversely, a critical
callback without `with state.write` is denied the write effect. The
`with` clause is an upper bound, not an exact usage list.

Actual effect examples: `s.x = 4` produces `state.write`;
`s.x = s.x + 1` produces `state.read` and `state.write`.
No default inference may silently elevate a declared read-only method.

Actor classes may declare methods and **one fixed-key `state { ... }` map**.
They may not declare ordinary instance `let`, `val`, `const`, or other
stored data fields. `self.state` and direct state keys outside a sealed
scope are never valid. State loan references may not escape, be stored, be
captured by another closure, or cross `await`, `select`, cancellation or
any suspension. Callback access must be lexical and authenticated by the
actor scheduler. Invalid read-to-write upgrades and nested loans are rejected
by the frontends (a native host-side stack may have separate internal
nesting semantics not exposed to guest code).

Actors remain **single-flight**: at most one native thread executes actor
code at any moment. Read-only capability access does **not** authorize
parallel actor execution; true parallel snapshots require additional
ownership/scheduler proofs.

## Implementation and evidence

- Java/GraalVM PR #760 now parses function `with` rows and compiler-owned
  `using state.read/write` scopes, normalizing both to the sealed
  `self.handle_state` callback. The compiler requires a `with` row for
  a state loan, enforces inferred effect upper bounds and rejects
  unauthorized mutation, nesting, suspension and reference escape.
  [Funded JDK25 exact-source verification](https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/38316111454):
  **17 focused compiler/ownership checks passed**, 15 Git blobs verified.
  This is **not** Java guest execution or full Maven/Native Image testing.
- LLVM PR #244 executable restricted native compiler recognizes `with`
  and bounds scalar state read/write permissions in compiled ordinary
  methods and exhaustive tagged/boolean receive arms; `using` acquires
  scoped callbacks and `self.handle_state` remains sealed. [Funded exact-source native
  verification](https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/38316632181):
  **67/67 targeted CTests passed**, including real 15-stage typed pipelines,
  under-declared effect rejection, and ASan/UBSan coverage.
- LLVM PR #257 standalone native state-loan policy now treats an
  unmarked callback as read-only; a write requires the explicit write mode
  provided by `critical`. It rejects suspension inside an active loan.
  [Funded exact-source C++ verification](https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/38317050114)
  passed normal C++ admission and loan-bridge tests and sanitizer gates.
- Java PR #738 contains independent actor-pipeline compiler work; its
  state runtime remains fail-closed. It must be reconciled with #760
  before the branches are integrated.

## Outstanding release gates

The LLVM restricted source parser now requires explicit `with` effect
upper bounds on every ordinary, boolean or tagged state-bearing callback;
omitting the row is a compile-time error. The positive 15-stage pipeline
fixtures using `using` or `self.handle_state` have been migrated to
explicit effect signatures. Other historical actor-field fixtures/PRs
outside the focused migration inventory may still need work. The Java and LLVM
branches are independently stacked and must be reconciled without dropping
actor-state AST metadata or bypassing effect checking. Full-tree
Maven/JUnit, LLVM CTest, Graal Native Image, actor-runtime guest parity,
multi-flow streaming/backpressure, and formal scheduling/ownership proof
are not complete. **Do not merge these draft branches on focused gates
alone.**

### Updated native execution milestone (October 12, 2026)

[Funded exact-source LLVM CI #38343367152](https://github.com/flags-2-env-test/java-app/actions/runs/38343367152): **41/41 focused CTests passed**. Restricted guest methods now execute synchronous read/write loans on actor-owned heaps across distinct trusted turns, and the migrated three-actor typed guest roundtrip exercises `self.forward`/`self.backward` with correlated runtime evidence and sanitizers. This does not replace full-tree LLVM CTest, Java/GraalVM Native Image, generalized borrow checking or multi-flow tests; keep draft.

### Native effect and scalar-type audit (2026-10-12)

[Funded exact-source LLVM Actions #38348172398](https://github.com/flags-2-env-test/java-app/actions/runs/38348172398) passed **68/68 selected CTests**, with **68 pinned source/test blobs**. This adds a zero-native-read LLVM IR contract for blind tagged `state.write`, real guest execution and ASan/UBSan for write-only DATA callbacks, negative omission-of-`with` tests, four explicit-effect 15-actor native/sanitizer suites, boolean-literal rejection in all scalar callback shapes, and typed-`int` actor state initializer validation. The previous 46-test migration gate is subsumed in the selected runner set.

**Scope boundary:** These are selected SHA-pinned builds of the native frontend/runtime, not the complete historical repository CTest matrix, LLVM optimized production builds, Java/GraalVM guest parity, AOT/hybrid, or multi-flow scheduling/ownership proofs. PR #244 remains draft.
