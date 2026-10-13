# Java Oreslang to LLVM port contract

Java source of truth: [oreslang-source.java](https://github.com/ores-truffle-oreslang/oreslang-source.java).
LLVM roadmap: [issue #1](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/1).

## Compiler fragment currently accepted

- Zero or one `define module M [as] ... end` wrapper.
- Multiple zero- or multi-argument `[pub] fnc`/`routine` declarations,
  with `int` parameters and `: int` return annotation.
- Exactly one `return <expr>;` statement per function.
- Decimal int literals, unary negative, binary add/subtract/multiply, grouping,
  integer parameters, same-module function calls (including forward calls).
- Whitespace, `//` comments and nested `/* ... */` comments.

Semantic check runs before LLVM lowering: rejects unknown variables/callees,
mismatched call arity and duplicate declarations/parameters. Unexpected tokens fail closed.
The LLVM verifier runs before IR is emitted.

## Native I/O naming contract (future binding admission)

All source-visible **synchronous-looking I/O** APIs must end in `_blocking`; the runtime must **transparently suspend the calling continuation and release the actor carrier** while I/O is in flight, then resume with a `T` result or an error. They must not block an OS scheduler carrier. **Asynchronous I/O** APIs may, but need not, end in `_async`, and return `Future<T>`. This includes filesystem metadata operations, sockets, DNS, terminal/stdio and HTTP operations that can block the caller. Pure functions and host-internal C++ methods do not acquire suffixes.

The canonical contract, effect-classification rules, alias checks and negative-test matrix live in [oreslang-common-source/docs/NATIVE_IO_NAMING.md](https://github.com/ores-truffle-oreslang/oreslang-common-source/blob/main/docs/NATIVE_IO_NAMING.md). Example future APIs: `fs.read_text_blocking(path)` and `await fs.read_text_async(path)`.

**Implementation gap:** The current LLVM fragment has no imported/native I/O declarations, async lowering, I/O effect metadata or runtime standard library. It must not pretend that spelling `*_blocking` alone makes a builtin supported. When an I/O binding registry is added, reject any binding classified as synchronously blocking I/O unless its source-visible name ends in `_blocking`; check methods and aliases too. Both forms require an actual non-carrier-blocking execution contract. `_blocking` calls must lower to I/O submission plus implicit continuation suspension, with no explicit source `await`; async calls return an explicit awaitable. LLVM must fail closed rather than compile `_blocking` into a host-blocking C ABI call. Add valid and invalid compiler fixtures, capability-denial tests and concurrent carrier tests before declaring enforcement.

## Ownership and borrow checker parity gate

The LLVM frontend currently parses a deliberately narrow i64-only subset and
does **not** lower Java's `rt borrow`, `rt borrow mut`, `rt take`,
`rt copy`, `rt share`, borrowed parameters, structural read loans, or
actor-owned memory. The native runtime's thread/arena helpers do not make this
subset safe for move-only data or concurrent actors.

Until the backend has explicit ownership provenance, lifetime scopes,
reborrow/alias exclusion, transitive copy contracts, projection tracking,
suspension and actor-boundary rules, these features must **fail closed** with a
nonzero compiler exit and **no LLVM IR**. The negative CTest fixtures enforce
that contract. Their passing status is evidence of unsupported-language
rejection, **not** implementation or semantic parity with Java.

A future positive ownership fixture must demonstrate full-stack type checking,
lowering, IR emission, and runtime behavior in addition to differential
Java/LLVM conformance; simply accepting the parser grammar is insufficient.

## Cross-implementation status

| Java capability | LLVM status |
| --- | --- |
| Lexer, modules, functions | Partial i64-only subset, not full Java parity |
| Function calls, expressions | i64 arithmetic and same-module calls |
| Type/ownership checking | Only parameter/name/arity checking |
| LLVM IR verification | Implemented |
| Compile and execute native guest | Clang can link i64 main on supported host |
| General target-specific ABI/object emission | Not implemented |
| Actors/supervisors, async frames, channels | Not implemented |
| Static select and reusable SelectPlan / DynamicSelectPlan | Not implemented; fail-closed fixtures and [parity contract](SELECT_PLAN_PARITY.md) |
| Cycles/Ref/WeakRef, borrow checking, arenas | Not implemented |
| HPC/HFT and isolation | Not implemented |

**Unreconciled:** Java `int` width and overflow, possible runtime faults,
name mangling/linkage, operator semantics, hot reload and actor isolation. Arithmetic
currently lowers to LLVM signed i64 operations **without** `nsw`/checked traps; resulting
wraparound is provisional. Division deliberately remains unsupported to avoid undefined
zero-divisor and signed-overflow behavior. Native `main` process codes only test small
positive results.

### Tests

`ctest --test-dir build --output-on-failure` checks valid/invalid syntax,
LLVM IR fragments and, if Clang is available, native compilation + execution.
A workflow failure with zero allocated runner steps is not an executed test.
Record the exact Java source SHA in future differential fixtures, rather than claim parity.

Next priorities: imported Java lexer golden cases, unambiguous checked integer semantics,
structured control flow, typed return analysis, precise ownership and actor ABI,
before introducing scheduler or pinned-frame execution.
