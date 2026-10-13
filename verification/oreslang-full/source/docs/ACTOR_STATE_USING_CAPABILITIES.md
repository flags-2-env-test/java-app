# Actor state access: canonical `self.handle_state` contract

> **Pre-1.0 design update (2026-10-12):** The normative contract is
> [WITH_USING_STATE_CONTRACT.md](WITH_USING_STATE_CONTRACT.md):
> `with` declares permitted effects, `using` acquires lexical capabilities,
> `self.handle_state(|state|...)` reads by default, and `critical`
> grants exclusive write access. Historical syntax discussions below
> are retained for audit history but are superseded where they disagree.


Status: **restricted LLVM compiled execution verified; full Java/LLVM guest-method parity pending**.
Updated October 12, 2026.

## Normative source syntax

An actor has methods and at most one fixed-key `state` map; ordinary
actor instance `let`, `val` and `const` properties are not allowed.

```ores
define actor Counter as
    state { value: int = 0; }

    // Callback is implicitly read-only if no "critical" modifier.
    read_value(): int with state.read {
        const value = self.handle_state(|state| -> {
            return state.value;
        });
        return value;
    }

    // The "critical" modifier grants exclusive write authority.
    increment(): int with state.read, state.write {
        const next = self.handle_state(critical |state| -> {
            state.value = state.value + 1;
            return state.value;
        });
        return next;
    }
end
```

The source above describes the canonical language-level API. The restricted
LLVM frontend currently requires explicit `fnc` on ordinary method declarations
and admits one synchronous copied-`int` callback; it has **not** verified the
shorthand declarations in this documentation verbatim. Equivalent explicit
`pub fnc` methods are compiled and executed against actor-owned native heaps
under trusted turn tokens. Suspension, arbitrary guest calls within loans,
general method bodies and Java/LLVM parity remain outside this evidence.
Java PR #738 must be reconciled with the separate effects/state work in #760.

For this API, the name is `self.handle_state` (dot access), not
`self_handle_state` as a standalone global function.

## Borrow/capability rules

| Contract | `self.handle_state(|state| -> {...})` | `self.handle_state(critical |state| -> {...})` |
|---|---|---|
| Borrow category | Immutable/read-only state loan | Exclusive/mutable state loan |
| Read declared fixed keys | Yes | Yes |
| Write declared fixed keys | **Compile error** | Yes |
| Copy an `int` result out | Yes | Yes |
| Return/retain the state reference | **Forbidden** | **Forbidden** |
| Suspend via `await` or `select` | **Forbidden** | **Forbidden** |
| Nest a state loan or invoke arbitrary guest code during a loan | **Forbidden in native subset** | **Forbidden in native subset** |

The permission applies **only** inside its callback. The result must
not contain a borrowed state object or pointer; the current executable
subset copies an `int` scalar. The actor's private state remains
unavailable in `self.state`, from an ordinary member field, or
outside the callback. The compiler must reject a write through a read
loan even if the surrounding actor method is otherwise mutable.

Oreslang actors remain **single-threaded at any instant**. The read-only
mode does not currently admit simultaneous executing readers; parallel
read snapshots would require a separate scheduler and isolation design.

## `using` and `critical`

Both `self.handle_state` and `using state.read/write` are **first-class,
canonical** actor-state source forms. They are not deprecated aliases.
`with` in the enclosing actor method declares an upper bound on effects;
it does not acquire a loan. `using` acquires a lexical state capability;
it is never an effect declaration in a function signature.

```ores
read_value_using(): int with state.read {
    return using state.read(|state| -> state.value);
}
increment_using(): int with state.read, state.write {
    return using state.write(critical |state| -> {
        state.value = state.value + 1;
        return state.value;
    });
}
```

Read is the default state callback. A bare `critical |state|` callback
selects exclusive write; `critical read` and `critical write` are
explicit mode spellings. Both high-level spellings lower to the same sealed
compiler-owned intrinsic with the same no-escape, effect-bound, and
no-live-loan-suspension rules. For the complete normative contract, see
[WITH_USING_STATE_CONTRACT.md](WITH_USING_STATE_CONTRACT.md).

## Verified implementation boundary

- **LLVM:** [funded exact-source Actions run #38314809579](https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/38314809579)
  passed **57/57 focused CTests**. It includes compiled 15-actor forward/
  backward pipelines with both canonical forms, LLVM IR checks proving
  read-only callbacks cannot emit actor-heap writes, and Clang
  AddressSanitizer/UndefinedBehaviorSanitizer execution.
- **Java/GraalVM:** [funded JDK 25 run #38314867754](https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/38314867754)
  passed **27 focused parser/compiler checks**. Both canonical forms
  explicitly fail closed because scoped guest state access isn't
  implemented; neither form runs on Java yet.

An additional [41/41 exact-source focused LLVM gate](https://github.com/flags-2-env-test/java-app/actions/runs/38343367152)
verified ordinary-method and exhaustive tagged/boolean state callback execution,
multi-key actor heaps and typed forward/backward pipeline behavior under
sanitizers. Full native repository regressions, other legacy `let` actor-field
fixtures, async/suspend-aware loans, general value types, multiple concurrent
flows, reader snapshot semantics and Java/LLVM executable parity are still
release blockers.
**Keep both PRs draft and do not claim full feature parity.**
