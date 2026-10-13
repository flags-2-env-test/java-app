# LLVM closed source-actor MessagePack variants — October 11, 2026

Status: DRAFT. Native runtime conformance passes; general LLVM guest-language lowering is not complete.

## Implemented

The compiler-facing ABI now exposes oreslang_source_actor_spawn_variants and oreslang_source_actor_send_variant. The registry accepts 1–64 immutable MessagePack v2 contracts, copying and validating all schema IDs, shapes, and SHA-256 digests before actor registration. Duplicate schemas, malformed contracts, unsupported counts, and forged hashes fail closed.

Incoming DATA frames select an exact registered schema, then undergo full typed validation against a private owned snapshot before dispatch. The callback receives a compiler-local 0-based variant index; this index is NOT the source enum tag. The real Java-compatible tag remains inside the validated typed payload. All alternatives share the same NativeActorScheduler logical actor lane, mailbox capacity budget, and actor-owned state; scalar and one-schema entrypoints remain supported.

The source actor producer gate now closes admission on trusted stop and waits for already-entered send calls to finish validation/admission before releasing contracts. External callers MUST prevent new operations on opaque handles after stop begins. This producer lease is NOT a stale-pointer-safe or generation-checked public handle design.

## Verification

Funded exact-blob native workflow PASS:
https://github.com/gha-indie-worker/gha-indie-worker-docs/actions/runs/38176845866

The exact-source run verified source Git object hashes and passed GCC -Wall -Wextra -Werror with ASan/UBSan, 20 repetitions of source-actor execution, the four-variant source inbox test, eight concurrent sender threads with 196 total messages, preserved single-flight state access, sender-buffer ownership, malicious and duplicate contract rejection, legacy scalar actor compatibility, existing one-schema ABI, the production Java Status.Message2 MessagePack golden, generic actor group adversarial admission tests, and optimized Clang repetitions.

Implementation: src/main/cpp/oreslang/runtime/SourceActorBootstrap.c and .h.
Regression: tests/native_source_actor_variants_v2_test.c and registered native CTest.

## Additional nominal-identity security audit (same day)

**Severity: release-critical type-admission bypass, fixed in the new API.**
The original native variant envelope pinned only the SHA-256 *shape* of
`T[S,I,S]`. A sender could retain the legitimate Java `Status.Message2`
schema/digest but change the inner enum owner (`Status`) or declared
numeric tag (`2`). Both remained structurally valid strings/integers.
Java's `TaggedMessageCodec` rejects these combinations.

The native implementation now provides
`oreslang_source_actor_spawn_tagged_variants(..., contracts, identities, n)`.
Each `OresSourceNominalCase` pins a trusted enum owner and positive signed-32
tag alongside its schema. Contract metadata are copied before verification.
Once `ores_mp2_validate_owned` passes, the native decoder independently
checks the owner and positive canonical tag in the *same owned snapshot*;
incorrect values fail before actor callback admission.

The older `oreslang_source_actor_spawn_variants` is explicitly
structural-only. It now rejects `oreslang.enum.*` registrations outright:
a compiler cannot silently substitute an unverified shape-only variant
entrypoint for a typed source enum. This is an intentional API admission
restriction; existing scalar, one-schema, and structurally named native
message APIs remain valid for their documented purposes.

Latest exact-source native regression:
https://github.com/gha-indie-worker/gha-indie-worker-docs/actions/runs/38177885872

This green run covers:
- three structural variants using a common actor lane and mailbox capacity,
  with eight producers and 195 admitted messages, plus the rejection of a
  fourth Java enum schema through structural-only registration;
- a separately registered pinned `Status.Message2` nominal actor, genuine
  Java wire acceptance, and rejection of payloads mutated to a different
  owner or tag while preserving a valid schema, shape digest and value types;
- native GCC ASan/UBSan, scalar compatibility, Java golden and generic actor
  group compatibility tests, and optimized Clang repeats.

Java #645 has a matching independently compiled JUnit test for identical
invalid-owner and invalid-tag mutations. Source compiler parity is STILL NOT
claimed: LLVM must emit this new pinned nominal ABI itself, then execute
actual guest `.ores` programs before release.

## Outstanding release blockers

1. LLVM currently parses nominal enum/pattern declarations, but compilation of typed guest sends, receive match, typed tuple destructuring, and exhaustive matches into the native variant ABI is not yet demonstrated. Direct C callback conformance is not a guest-source parity claim.
2. Java allows up to 4096 declared tagged alternatives; the conservative LLVM ABI currently supports at most 64. Align the portable limit or extend native indexing and resource budgets.
3. Producer leases cover entered calls, not arbitrary stale pointer use after native destruction. Enforce generation-checked handles or compile-proved ownership and shutdown.
4. Typed Future<T>, await/reply correlation, cancellation, defer/recover cleanup, and finalizer lifecycle still need executable cross-backend tests.
5. Full LLVM CMake/CTest, full Java Maven plus Graal native-image and long fuzz/stress runs on exact current integrated heads remain merge gates.

Keep PR #180 draft. Do not merge until the remaining release blockers have test evidence.


## Compiler-generated guest progress: bounded tagged int (2026-10-11)

The earlier "not yet executable guest" limitation has been PARTIALLY
resolved. The LLVM compiler now executes a real Oreslang
`source-tagged-int-payload-dispatch.ores` fixture. `ref.send(Enum.Case(19))`
generates a Java-compatible pinned `T[S,I,I]` MessagePack frame.
`on Enum.Case as n` narrows its integer payload into an arm-local value.
Distinct Add/Sub variants with identical payload shape execute correctly
on the same actor serial lane, preserving field state and order.

Independent CI:
- LLVM real guest, negative compiler parser tests and GCC/Clang
  sanitizer checks: https://github.com/zed-pkg-test/java-app/actions/runs/38183268136
- Java `TaggedMessageCodec` exact byte vectors for Add(19)/Sub(4):
  https://github.com/zed-pkg-test/java-app/actions/runs/38183802496
  (59 targeted JUnit tests, 83 exact Git-blob inputs).

The previous merge blockers remain for nonliteral/non-uint31 payloads,
string and tuple binder lowering, actor reply/forward/done, Future<T>,
cancellation/defer/recover, stale actor pointer safety, variant scaling,
and full original-organization conformance. Do not infer broader guest
support from this first typed integer slice.
