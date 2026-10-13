# Actor instance-data isolation contract (draft, 2026-10-12)

## Normative rule

Actor declarations may contain methods and one compiler-owned `state { ... }`
map. They may not contain ordinary instance `let`, `val`, `const`, or
user-defined class data properties. State map keys are fixed at compile time;
they are never reflected as ordinary actor methods or public fields. An actor
may also have **methods only**, with no state map.

```ores
define actor Worker as
    state {
        value: int = 0;
    }
    pub fnc helper(int value): int { return value + 1; }
end
```

The canonical sealed intrinsic is `self.handle_state`: no modifier means a
read-only state loan; `critical` grants exclusive mutable access. The equally
canonical scoped forms `using state.read(callback)` and
`using state.write(critical callback)` lower to that intrinsic.
The enclosing function must declare the applicable `with state.read` and/or
`with state.write` effect upper bound. See
[WITH_USING_STATE_CONTRACT.md](WITH_USING_STATE_CONTRACT.md). The
restricted LLVM compiler supports these forms inside synchronous scalar
ordinary methods and exhaustive tagged/boolean receive arms:

```ores
const current = self.handle_state(|state| -> {
    return state.value;
});

const next = self.handle_state(critical |state| -> {
    state.value = n + 1;
    return state.value;
});
self.forward(Signal.Data(next));
```

Its result is a **copied primitive**; it cannot be a state reference.
`self.state`, raw `value` access, raw class properties, duplicate maps,
unknown keys, and returning `state` itself are rejected. Callback-owned
state values cannot cross an `await`, `select`, or other suspension
boundary in this restricted profile. The native scheduler enforces
single-flight actor execution; the parser/lowerer does not implement a
general reader-writer lock or async state loans.

## Evidence

LLVM: [57/57 focused native CTests, with Clang ASan/UBSan](https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/38314809579).

Java: [27 exact-source JDK25 parser/compiler checks](https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/38314867754).
The Java `Ast.ActorStateSpec` records fixed-key state independently from
`ClassDecl.fields`; annotation expansion and tree shaking preserve it.
The Java checker **rejects** state execution until the scoped state-loan
runtime is built. A parsed declaration is not runnable.

## Unresolved release gates

The restricted native profile supports fixed `int` state keys and one
synchronous scoped callback per ordinary method or exhaustive tagged/boolean
receive arm; it does **not** implement arbitrary actor method bodies,
general reference-valued state, asynchronous loans, nested callbacks,
zero/many pipeline emissions, or multiple concurrent flows. Full repository
CTest, JVM/GraalVM guest parity and Native Image are not yet verified.

Older fixtures/PRs that declare actor instance data directly need migration
to the new syntax before either draft PR may merge.

Focused native guest evidence: [41/41 passing CTests with ASan/UBSan](https://github.com/flags-2-env-test/java-app/actions/runs/38343367152), including ordinary-method state execution, multi-key actor persistence and a compiled three-actor typed directional round trip. These results do not certify full-tree or cross-backend parity.

Updated evidence: [46/46 focused native CTests on exact pinned inputs](https://github.com/flags-2-env-test/java-app/actions/runs/38343656495). Includes boolean-loan effect validation and rejection of bool-to-integer use inside callbacks. Full LLVM repository tests and Java/GraalVM parity have not passed as part of this gate.

Updated effect audit: [68/68 focused native CTests](https://github.com/flags-2-env-test/java-app/actions/runs/38348172398). Every restricted LLVM state callback now needs explicit `with` effects; blind tagged writes emit no native field-read call. Typed `int` state initializers and copied scalar results reject boolean literal coercion. Native and sanitized 15-actor tests passed on pinned source inputs. Full CTest and Java parity remain unverified.
