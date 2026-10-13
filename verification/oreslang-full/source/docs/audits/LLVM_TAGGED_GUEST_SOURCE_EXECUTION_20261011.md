# LLVM source-compiled nominal tagged actor execution — 2026-10-11

**Status:** FIRST EXECUTABLE guest-language integration **GREEN on funded SHA-pinned
CI**. Deliberately limited to closed, payloadless enum cases and actor-owned
scalar state updates. Remain DRAFT; not full Java/LLVM tagged message parity.

## What is implemented

The LLVM frontend now admits one narrowly checked source-language feature:

- `define enum Signals as Ping = 1; Pong = 255; Stop = 65536 end`.
- `define pattern WorkerMessages as Signals.Pong; Signals.Ping; Signals.Stop end`.
- `define actor Worker<messages=WorkerMessages>` with a single
  `receive(Message m)` and exhaustive `do match m over { on Signals.X {...} }`.
- Arms may perform source-typed updates to declared mutable actor-owned i64
  fields, with no unverified receiver access and no accidental implicit reply.
- Trusted zero-argument `main` spawns a typed actor, calls
  `actor.send(Signals.X)` and stops that actor. Every send checks admission
  instead of silently dropping an overloaded message.

LLVM codegen now generates a compiler-owned **ordered nominal contract table**
and a single per-actor variant callback. The native runtime uses the existing
native actor scheduler, one actor DATA lane, one bounded DATA capacity and
its existing start/stop generation fence. Dispatch switches on the
compiler-local ordinal, not the declared numeric wire tag.

## Canonical Java / LLVM transport identity

Emit the existing Java `TaggedMessageCodec` scheme; DO NOT invent an
independent transport:

- Domain fingerprint:
  `SHA256("oreslang.tagged.domain.v1/" + pattern_name + "/" + concat(sorted(length + ":" + qualified_case + ";")))`
- Per-case schema:
  `oreslang.enum.<domain_hex>.<SHA256(qualified_name + "#" + tag)_hex>`
- Payloadless case shape: `T[S,I]`, shape SHA-256 over
  `"oreslang.actor.shape.v2/T[S,I]"`
- Encoded frame: canonical MessagePack
  `[2, schema_id, bin8(shape_sha256), [enum_owner, declared_tag]]`.
  Cases with tags 1, 255 and 65536 exercise fixint, uint8 and uint32.
- Source pattern declaration order affects local dispatch **ordinal** but
  does not affect the sorted Java domain fingerprint.
- The existing native tagged spawn registration pins schema identity, owner,
  tag and digest; incoming DATA is decoded from a single owned snapshot.
  No runtime payload-shape guessing or secondary executor.

Independent expected domain hash for the source fixture's
`WorkerMessages` contract:
`e5a943e4263cf4ff7a59b28b63ffbac5a6a3279952ec16ca95f26193831c33bf`.
The compiled IR is tested against this known Java-derived hash.

The new compiler code supports LLVM 18's opaque pointer API via
`llvm::PointerType::get(context, 0)`, replacing the removed
`llvm::Type::getInt8PtrTy`.

## Executable source fixture

`tests/fixtures/source-tagged-payloadless-dispatch.ores` sends three
distinct messages with differing source-ordered match arms. Each arm updates
the actor's `count` by a distinct value (2, 4, 8). The verified runtime
ends with **processed=3 last=14**, demonstrating real typed dispatch, not
just no-op transport or permissive compiler admission.

## Verification evidence

- [Funded guest-source compiler, native execution, and ASan/UBSan
  #38180609339](https://github.com/gha-indie-worker/gha-indie-worker-docs/actions/runs/38180609339):
  SHA-pinned source compiler and guest fixture; compile actual C++ LLVM
  frontend/codegen with LLVM 18, verify IR, link against source actor
  native runtime, execute expected result 43 and actor diagnostic
  `processed=3 last=14`, repeat under AddressSanitizer/UndefinedBehaviorSanitizer.
- [Native source actor and MessagePack conformance
  #38180609585](https://github.com/gha-indie-worker/gha-indie-worker-docs/actions/runs/38180609585):
  exact Git blob checks, native source actor variant lifecycle,
  Java tagged golden vectors, scalar compatibility, bounded admission,
  sanitizer and optimized Clang regressions.
- [Nominal parser/match admission
  #38180609364](https://github.com/gha-indie-worker/gha-indie-worker-docs/actions/runs/38180609364):
  exact-source repeated GCC sanitizer and Clang parser tests,
  closed domain selection, binder/exhaustiveness checks, reject
  out-of-pattern sends, reject unsupported payload-bearing actors.
- Added native and ASan fixture entries to `CMakeLists.txt`. Full
  source-repository CMake/CTest has **NOT** been run on this updated head.

## Fail-closed scope and release blockers

This does **not** claim general tagged actor support:

1. Payload-bearing enum cases, strings, tuples, arrays and lists do NOT
   execute on LLVM guest source. Their actor compilation fails with
   `LLVM tagged source subset requires payloadless cases`, even though
   native MessagePack tests already support typed values.
2. `self.respond`, `self.forward`, `self.send`, `self.done`, typed
   `Future<T>`, asynchronous actor effects and mailbox backpressure
   continuations remain unlowered. Receive's scalar return in this
   fixture is an internal handler result for testing actor state;
   it is NOT an implicit actor response.
3. Synchronous checked send terminates the trusted host on admission
   failure rather than silently dropping a message; typed overload
   propagation and scheduling backpressure remain separate work.
4. C source-actor handles are still externally fenced opaque pointers;
   producer-close counters protect already-entered sends but **do not**
   make a freed pointer safe for a newly starting producer. Guest
   actor-ref generation/lifetime and cancellation require stronger ABI.
5. The new fixture passes focused CI, not full LLVM CMake/CTest,
   Maven/GraalVM native-image, differential whole-language suites,
   hostile actor-handle fuzzing or merge-admission verification.

Keep the source code draft and merge only after full-stack verification.
