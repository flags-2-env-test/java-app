# Oreslang LLVM backend

Native companion to [oreslang-source.java](https://github.com/ores-truffle-oreslang/oreslang-source.java).
Tracks [LLVM port issue #1](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/1).

**Status: small experimental source-to-LLVM compiler, not Java language/runtime parity.**

Static select, channels, reusable `SelectPlan`, and mutable `DynamicSelectPlan`
are **not implemented** in the LLVM compiler. The CI regression fixtures
reject them without producing IR. See [the native select parity contract](docs/SELECT_PLAN_PARITY.md).

## Implemented slice (October 2026)

- Parse one file with one or more `fnc`/`routine` declarations returning `int`, with optional
  `pub` and one optional `define module M [as] ... end` wrapper.
- Typed `int` parameters, `return` expressions, integer literals with underscores,
  unary `-`, parentheses, `+`, `-`, `*`, parameter references and same-module function calls.
- Forward declarations enable calls before definitions and recursion (not bounded recursion).
- Reject undeclared names, duplicate function/parameter names, mismatched call arities,
  overflowed positive i64 literals, malformed syntax, and unsupported language features.
- Lower each function to i64 LLVM IR, verify the entire LLVM module before printing IR.
- CI covers positive/negative source-to-IR contracts and Clang compilation/execution of
  standalone `main` functions, when Clang is present.

The `int` to `i64` mapping, wraparound arithmetic, external linkage, and calling conventions
**are provisional** and are not a claim of Java semantic parity. In particular, this slice
does not support `-9223372036854775808` as one literal.

## Build and test

Requires LLVM development libraries 14+, CMake 3.20+, C++17, Python3 and ideally Clang.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
./build/oreslang-llvmc tests/fixtures/forward-call-main.ores > /tmp/guest.ll
clang -x ir /tmp/guest.ll -o /tmp/guest
/tmp/guest
echo $? # 42
```

The CLI currently prints LLVM IR; object output and versioned ABI are not implemented.

Unsupported actors, async/await, channels, traits/classes, imports, tuples, recursive/owned
objects, GC, HFT scheduling and native runtime services **fail closed**. They remain roadmap items.

Native I/O API naming: blocking I/O operations will require the `_blocking` suffix; async I/O may use `_async`. This is a documented target contract, **not yet an implemented compiler check**.

See [source parity contract](docs/SOURCE_PARITY.md), [LLVM roadmap](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/1),
and the distinct [Java cyclic graph work](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/474).
