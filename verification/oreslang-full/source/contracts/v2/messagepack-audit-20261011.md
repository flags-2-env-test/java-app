# Oreslang actor MessagePack v2 — audit and hardening, 2026-10-11

**Status:** targeted audited components green; **NOT release or merge admitted**.
The canonical production tagged actor codec is
`dev.oreslang.runtime.TaggedMessageCodec`; the older
`MessagePackActorContract` is a separate host/client primitive and must not
redefine source-level `define enum` / `define pattern` envelopes.
The canonical tagged format is documented in
`contracts/v2/tagged-enum-messagepack.md`.

## Findings fixed

1. **Java tagged-decoder computational amplification / inconsistent sender
   view.** Previously the decoder tried every compiled case, recursively
   attempting full MessagePack decoding of caller-owned bytes. At a maximum
   declared domain of 4096 cases, this caused avoidable unbounded repeated
   parsing relative to one envelope and let caller mutation influence different
   decoder attempts. Now it rejects frames larger than 1 MiB, clones exactly
   once, parses the canonical schema identity, looks up exactly one
   compiler-pinned case and performs one typed decode with pinned hash and tag
   checks. A missing case fails closed without fallback. The schema-peek parser
   is shared with the host actor codec to avoid separate acceptance rules.

2. **Generic native group contract time-of-check/time-of-copy gap.** Previously
   it checked grammar/digest using borrowed host strings and only then copied
   the contract into group storage. It now first snapshots schema bytes, shape
   bytes and the 32-byte digest and validates the copied values before
   registering. Failed validation frees the temporary copies. Registration
   remains trusted single-threaded construction; caller-provided pointers
   must be stable for the duration of the copy, and no other group mutations
   may race registration.

3. **Native LLVM source-actor bootstrap contract gap.** The compiler-facing
   `oreslang_source_actor_spawn_wire` likewise previously validated a borrowed
   contract before copying it. It now bounds, allocates and snapshots all
   contract fields *before* independently validating their grammar and
   canonical digest. Invalid declarations fail before allocating/starting an
   actor. A successfully registered actor owns its schema and shape until its
   supervised termination.

4. **API lifetime/feature ambiguity.** Header contracts now correctly explain
   owned registration, per-actor DATA slot reservations and producer fencing.
   The source-actor wire ABI is **one schema per actor**. It is not a substitute
   for compiler lowering of a multi-alternative closed pattern, and guest code
   cannot choose a new schema from untrusted bytes.

## Executed verification

- Java/GraalVM: [funded exact-source targeted Maven/JUnit
  #38175221113](https://github.com/gha-indie-worker/gha-indie-worker-docs/actions/runs/38175221113),
  **57 tests, 0 failures, 0 errors, 0 skips**, with **83 exact SHA-pinned
  Java source/test blobs**. Includes real guest-source actor tests, production
  tagged messages, a 384-alternative closed registry, forbidden schema and
  version mutations, ownership, envelope canonicalization and typed transport.
- LLVM native: [funded exact-source C
  #38175456858](https://github.com/gha-indie-worker/gha-indie-worker-docs/actions/runs/38175456858):
  source-actor wire ABI with GCC ASan+UBSan, legacy scalar actor compatibility,
  the byte-identical Java tagged enum golden, and generic actor-group
  adversarial tests under GCC sanitizers; also Clang optimized repeats.
  This evidence executes native library/test code, not compiled Oreslang
  tagged-variant guest programs.
- Production Java tagged golden file
  `contracts/v1/design/tagged_message_vectors.json` has the identical Git
  blob in both repositories. The canonical tagged transport contract also has
  byte-identical content across both repositories.

## Merge blockers and unresolved risks

1. **LLVM source frontend/codegen:** `define enum`, closed `define pattern`,
   per-case compiler-generated schema registration, source `ActorRef.send`,
   typed `receive`, exhaustive ordered `match`, and payload narrowing and
   destructuring must execute as guest programs; host C adapters are not proof.
2. **Typed futures and cleanup:** fully implemented `Future<T>`,
   nonblocking await continuations, cancellation propagation and finalizers
   must satisfy one-owner/single-delivery guarantees, with `defer/recover`
   and execution-context safety rules.
3. **Producer fencing:** generic and source native actor destructor APIs require
   external isolation of producer threads. No concurrent send may access an
   actor handle after trusted stop/destroy begins. This is not enforced by a
   ref-counted lease type in the current C ABI; lifecycle admission from guest
   code is a separate, release-critical compiler invariant.
4. **Broader validation:** full Maven project suite and GraalVM native-image
   at exact current merged heads, full LLVM CMake/CTest, cross-backend guest
   differential fixtures, stress tests under rapid stop/cancel, malformed
   message fuzzing, schema evolution/migration, and authenticated cross-process
   communication. SHA-256 shape hashes give structural compatibility, **not**
   origin authentication.
5. **No premature merge:** Java #623/#645 and LLVM #163/#180 are stacked
   drafts. Current targeted funded success must not be represented as full
   project CI, AOT success or complete two-backend parity.

The test org mirrors **exact Git blob hashes** because the source org may fail
GitHub Actions before allocating a runner (`runner_id=0`, no steps).
No private cross-org checkout token or secrets are required.


## Same-day follow-up: enforce nominal enum case identity

A typed `oreslang.enum.*` message is NOT validated merely by a legitimate
schema ID and the tuple's SHA-256 shape digest. For example, the same
`T[S,I,S]` frame remains structurally valid when the embedded enum owner
`Status` changes or the declared numeric tag changes from `2` to `3`.
Java's production TaggedMessageCodec already rejected those values, while
LLVM's initial generic variant transport checked only shape/schema.

Fixed in LLVM #180:
- `oreslang_source_actor_spawn_tagged_variants` registers a compiler-pinned
  enum owner and declared positive 32-bit numeric tag for every case.
- The receiver validates the full detached owned MessagePack snapshot then
  independently checks both owner and tag before invoking guest callbacks.
- The structural-only `spawn_variants` entrypoint rejects
  `oreslang.enum.*` registrations. Typed enum messages cannot downgrade to
  structurally checked delivery.
- Existing scalar, structural and registered one-schema host integrations
  remain compatible with their existing contracts.

Native [exact-Git-blob CI #38177885872](https://github.com/gha-indie-worker/gha-indie-worker-docs/actions/runs/38177885872)
passed GCC ASan/UBSan, the forged-owner/tag regression, the
structural-only API fail-closed test, the serial actor's eight-producer
stress test, backward compatibility, the Java wire vector, and optimized Clang.

Java #645 gained an independent byte-mutation regression proving that
`TaggedMessageCodec` rejects both wrong owner and wrong numeric tag despite
valid structural payloads. Java [exact-Git-blob CI #38177982870](https://github.com/gha-indie-worker/gha-indie-worker-docs/actions/runs/38177982870)
passed **58 targeted JUnit tests, 0 failures, 0 errors, 0 skipped**.

**Scope:** This corrects native API admission, not Oreslang source compiler
parity. The LLVM frontend must actually *generate* the pinned nominal ABI and
execute real guest-source send/receive/match programs before merging. Native
producer leases also do not make opaque pointers safe after destruction.

## 2026-10-11: compiler-produced tagged integer messages

LLVM #180 now compiles and executes an actual Oreslang guest source fixture,
`tests/fixtures/source-tagged-int-payload-dispatch.ores`, not merely a C
callback harness. The intentionally bounded source subset admits
`ref.send(Enum.Case(19))` for a literal unsigned 31-bit integer payload
and `on Enum.Case as n { count = count + n; }` for an admitted `int`
case. It emits compiler-pinned nominal contracts and Java-compatible
MessagePack v2 `T[S,I,I]` frames and binds `n` from owned, validated
frame bytes on the actor's execution lane. All variants use the SAME
mailbox and actor-wide admission budget.

The fixture has source-order-reordered `Signals.Sub`, `Signals.Ping`,
`Signals.Add` alternatives, with distinct `Add` and `Sub` tags sharing
the exact same payload shape. It sends `Add(19)`, `Sub(4)`, and
`Ping`. The native actor processes exactly three messages and ends at
`count=23`, even though `main` independently returns 43.
Unknown/wrong-domain variants, payloadless-argument mismatches,
unsupported payload shapes, missing integer values, nonliteral
expressions and invalid binders are rejected by the compiler.

**LLVM guest execution:** [funded exact-source CI
#38183268136](https://github.com/zed-pkg-test/java-app/actions/runs/38183268136)
PASS: original payloadless fixture; tagged int source compiled to LLVM IR
then Clang native executable; 20 repeated native runs; independent parser
admission/negative tests; and 10 repeated guest executions under
AddressSanitizer/UndefinedBehaviorSanitizer. Exact source blob SHA pins
precede each build.

**JVM canonical parity:** Java #645 includes the exact wire golden
vectors for `Add(19)` and `Sub(4)` produced independently by
`TaggedMessageCodec`. [Funded Java CI
#38183802496](https://github.com/zed-pkg-test/java-app/actions/runs/38183802496)
PASS: 83 independently verified exact Git blobs, 59 targeted JUnit
tests, zero failures/errors/skips.

This is a **first executable primitive payload slice**, NOT full
`define enum`/`define pattern` support. Unsupported: string and tuple
payload construction/destructuring; dynamic or signed integer source
payloads outside the admitted literal subset; typed
`self.respond`/`self.forward`/`self.done` lowering;
`Future<T>`/await/cancellation; general lifetime-safe stale native actor
handles; native 4,096-variant parity (LLVM remains capped at 64); and
full cross-backend build/native-image and long fuzz regression evidence.

Keep Java #645 and LLVM #180 draft until those merge gates pass.
