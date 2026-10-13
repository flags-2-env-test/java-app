# Legacy actor-field regression migration audit

Status: **release blocker** (2026-10-12).

The new actor rule forbids ordinary class instance data in actors. Only
`state { key: Type = initializer; }` supplies actor-owned data, and state
access uses `self.handle_state(|state| -> {...})` for read-only
loans and `self.handle_state(critical |state| -> {...})` for
exclusive writes. The equally canonical `using state.read(callback)` and
`using state.write(critical callback)` forms are lexically scoped capability
acquisition, not prototype aliases. Enclosing methods declare their effect
bounds with `with state.read` / `with state.write`. User-defined actor methods
remain allowed. See [WITH_USING_STATE_CONTRACT.md](WITH_USING_STATE_CONTRACT.md).

The following historical fixtures were inspected on the LLVM draft branch.
Migration status is per-fixture; passing targeted gates does **not** establish
that the full legacy suite has been restored:

| Historical invariant / behavior | Fixture | Focused result |
|---|---|---|
| Three-key persistent heap and four methods | `actor-owned-fields.ores` | Native + ASan/UBSan passed |
| Duplicate fixed state key | `actor-duplicate-field.ores` | Exact diagnostic passed |
| Mutation attempted under immutable read grant | `actor-immutable-write.ores` | Reframed negative passed |
| Parameter versus state-key lexical resolution | `actor-field-shadow.ores` | Reframed positive passed |
| Unknown lexical variable | `actor-unknown-field.ores` | Exact diagnostic passed |
| Unknown state key write | `actor-unknown-write.ores` | Exact diagnostic passed |
| Unsupported fixed-key value type | `actor-unsupported-field.ores` | Exact diagnostic passed |
| Native async DATA send and STOP drain | `source-actor-send.ores` | Compiled guest passed |
| Five independent actors and mailboxes | `source-actor-five-workers.ores` | Compiled guest passed |
| Spawn and physical STOP | `source-actor-spawn-stop.ores` | Compiled guest passed |
| Nominal typed integer DATA dispatch | `source-tagged-int-payload-dispatch.ores` | Native + ASan/UBSan passed |
| Exhaustive boolean receive arms | `message-receive-bool-match.ores` | Native + ASan/UBSan passed |
| Three-actor forward/backward pipeline | `source-actor-pipeline-bidirectional.ores` | Typed guest + route evidence + ASan/UBSan passed |
| Generated actor symbol collision | `actor-abi-actor-collision.ores` | Exact diagnostic passed |

**Migration criteria:**

1. Positive fixtures must be rewritten to declare state, use legal
   scoped capability loans, and **still exercise their original runtime
   behavior**, not simply turn into compile-only checks.
2. Negative fixtures must fail for the originally intended invariant
   (e.g. duplicate key, shadowing or ABI collision), not for an unrelated
   early legacy-field syntax rejection.
3. The restricted native compiler now executes one synchronous scoped
   `int`-returning state callback in an ordinary actor method and in each
   exhaustive tagged/boolean receive arm. General function-body lowering,
   suspension, reference-bearing values and multiple outstanding flows
   remain outside this proven subset.
4. Keep Java/GraalVM fail-closed until equivalent guest `handle_state`
   capability semantics exist; do not rewrite source tests to weaken that
   safety boundary.
5. Re-run full relevant repository CMake/CTest, Java Maven/JUnit,
   Native Image and sanitizer suites on exact source trees **before**
   removing the draft restriction or merging.

This original inventory contains 14 historical fixtures now migrated to the
canonical fixed-key state model. This is **not** an exhaustive repository
inventory or a full-tree CTest result. Where the old contract was rendered
obsolete by the actor-state design (immutable actor fields and field-shadow
rejection), the corresponding tests are explicitly reinterpreted as
read-only loan rejection and lexical namespace acceptance.

Focused new-capability evidence:
https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/38314809579
(57 selected native CTests) and
https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/38314867754
(27 focused JDK25 compiler checks).

## First migrated historical regression — October 12, 2026

`actor-duplicate-field.ores` now declares two identical keys inside one
`state { ... }` map, rather than using prohibited legacy instance fields.
The LLVM source checker distinguishes `duplicate actor state key` from
older `duplicate actor field` diagnostics; the corresponding historical
CTest asserts this exact failure before emitting LLVM IR.

A real funded GitHub Actions runner compiled the updated LLVM frontend and
passed **27/27 focused CTests**, including the new duplicate-state admission
negative test, three compiled 15-actor guest pipelines, the optimized Python
safety canary, and Clang ASan/UBSan:
https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/38326659916

This was a **47-blob SHA-pinned focused native build**, not the complete
historical source tree or full CMake/CTest suite. Later focused gates migrate the remaining fixtures in this original inventory.
Other pre-1.0 source fixtures and full historical CTest remain unverified.

## Follow-up historical regressions (focused gate verified)

- The historical method-only `unsupported-actor.ores` is now an **acceptance**
  fixture. The old rejection was inconsistent with the canonical rule that
  actor instance state is optional.
- `actor-unsupported-field.ores` now uses `state { ... }` with an invalid
  state-value type, preserving the unsupported-type diagnostic without the
  obsolete `const` declaration.
- `actor-unknown-field.ores` now declares its valid state map and rejects
  unresolved lexical `phantom`, rather than rejecting legacy storage syntax.
- Both `actor-abi-actor-collision.ores` and `actor-abi-func-collision.ores`
  now use canonical maps while preserving generated-function symbol collisions.
- The historical negative CTests reject unrelated legacy-field diagnostics
  via a `--diagnostic-not` check in the regression harness, including under
  optimized Python where source `assert` statements would be disabled.

**Verification:** [funded exact-source Linux GitHub Actions #38340223003](https://github.com/flags-2-env-test/java-app/actions/runs/38340223003)
compiled the real LLVM frontend against 14 pinned Git blobs and passed **8/8**
focused canonical-regression CTests: duplicate state key, invalid state type,
unknown lexical variable, two ABI symbol collisions, two method-only actor
admission cases, and a Python `-O` forbidden-diagnostic mutation canary.
The intentionally failing canary passes only when CTest observes its expected
failure (`WILL_FAIL`); it is not a regular positive compiler test.

## Integrated focused migration gate — 2026-10-12

[Exact-source funded Linux Actions run #38343367152](https://github.com/flags-2-env-test/java-app/actions/runs/38343367152)
passed **41/41 focused CTests** with 47 pinned source/test blobs and the real
LLVM frontend, C actor runtime and Clang. The gate includes normal native
execution, ASan/UBSan, ordinary-method state callbacks, state persistence
between turns, read-only and effect-denial negatives, a full three-actor
typed forward/backward round trip with authenticated causal-route evidence,
five independently spawned workers, canonical tagged MessagePack i64
boundaries, and boolean state-match SSA/PHI merging.

The normal and sanitized three-actor guest test preserves the historical
typed result 19 and physical actor teardown evidence without handwritten
host-side actor callbacks. This is still a **focused mirrored CTest suite**,
not a full checkout of LLVM PR #244, not the complete historical CMake/CTest
suite, and not Java/GraalVM execution parity. Other legacy fixtures outside
this 14-file inventory may remain.

The restricted native compiler has concrete execution for one synchronous
state callback in ordinary methods and boolean/tagged receive arms. Full
general state loans, arbitrary user functions during loans, suspension,
reference-valued state snapshots, multi-flow scheduling, native full-tree
CTest and Java/GraalVM Native Image parity remain release blockers.
**Keep PR #244 draft.**

## Boolean match negative hardening (2026-10-12)

[Funded exact-source native Actions #38343656495](https://github.com/flags-2-env-test/java-app/actions/runs/38343656495): **46/46 selected CTests passed**, including the native three-actor typed guest, the multi-key historical actor, tag/MessagePack contracts, AOT-compatible LLVM emission, ASan/UBSan and five canonical boolean negative fixtures. This updated test mirror verifies **52 pinned blobs** (not a full source tree).

The compiler now explicitly prohibits treating the bool match subject as an i64 arithmetic operand inside a scoped state callback, including its write expressions and return expression. Nonexhaustive, duplicate, wrong-subject, wrong-domain and unnarrowed-payload negatives use legal fixed-key actor state; each CTest also guards against accidental unrelated legacy actor-field syntax rejection.

The original **14-file** historical migration inventory now has representative canonical tests across each listed behavior; this does **not** establish that all tests elsewhere in the repository have been migrated or executed. Full historical CTest, Java/GraalVM AOT/hybrid parity, general state loans and concurrent-flow routing are still unverified release gates. **Remain draft.**

## Post-migration actor effect audit (2026-10-12)

[Exact-source funded Actions #38348172398](https://github.com/flags-2-env-test/java-app/actions/runs/38348172398): **68/68 focused CTests passed**, 68 pinned blobs. Tagged actor handlers cannot silently infer a missing `with` effect row; four previously permissive 15-stage `handle_state`/`using` fixtures were migrated to explicit upper bounds and executed under Clang with and without ASan/UBSan. Blind tagged writes now emit no implicit native actor-state reads. Fixed `int` actor keys and scalar loan callback expressions reject boolean values instead of converting them to native i64. These changes preserve strict regression rejections and do not complete the repository-wide legacy inventory or cross-backend parity.
