# Oreslang actor message transport — canonical MessagePack v2

Status: **implemented host/runtime foundation; source compiler integration pending**.

## One authoritative format

MessagePack v2 is the only candidate default for cross-actor and
cross-supervisor mail, including SHARED actors. The earlier experimental
`OMSG/1` tagged frame (draft Java #629 / LLVM #171) is **not** release ABI:
do not lower guest `actor.send` to it. It is retained solely as an unmerged
experimental prototype until it can be removed or explicitly archived.

A message is a canonical MessagePack four-element array:

```text
[2, variant_schema_id, bin8(sha256(canonical_shape_descriptor)), payload]
```

The canonical shape grammar is `B | I | F | S | T[...] | FA[...] |
FL[...] | A[shape] | L[shape]`. Scalars are bool, signed i64,
finite float64 and strict UTF-8 string. Tuples and fixed sequences have
exact arity; arrays and lists have one element descriptor and bounded size.

A variant belongs to one closed actor message domain. Compiler-generated
IDs are unique and stable within that domain, and a contract's full
variant-specific schema identity separates alternatives with identical
payload shapes. Current host reference spelling:
`<domain>#variant=<positive-decimal-id>`. These IDs are **not**
authentication secrets: source typechecking must prove a sender is allowed
to use the receiving actor's contract and selected alternative.

No implicit shape-first arbitration is permitted. An ambiguous plain
`send(value)` must fail compile-time admission unless the source type
identifies one exact alternative or the caller selects an explicit enum
case / tagged value. The receiver must validate the exact schema, digest,
shape, version and canonical payload before `match` dispatch.

## Ownership and scheduler contract

1. The producer serializes before mailbox admission and keeps ownership
   of its original value and input memory.
2. Mailbox admission validates the receiver's registered compiler-owned
   contract and creates a **detached owned snapshot**. If the queue rejects,
   it immediately zeroizes/frees the temporary allocation. Never retain
   a sender's mutable pointer.
3. A successfully admitted frame becomes queue-owned. NativeActorScheduler
   delivers it on exactly one exclusive actor DATA turn. The adapter
   zeroizes/releases it after callback return. Callbacks cannot retain the
   borrowed frame bytes; eventual source lowering must create an actor-owned
   decoded value before user code can retain it.
4. Stop drains accepted work before finalization. CONTROL remains separate
   from bounded user DATA. Cancellation/force-retirement must additionally
   fence outstanding continuations and physical actor-owned heap loans.
5. Nothing permits external `actor.someMethod()` invocation. Guest
   `actor.send(...)` remains the sole public message API.

The new native `MessagePackActorV2` host adapter uses the existing
generation-checked `NativeActorScheduler` with its shared carrier pool.
It is NOT yet wired into `SourceActorBootstrap`'s LLVM guest `send`
lowering. The Java `MessagePackActorContract` routes multi-alternative
MessagePack v2 encoded values but is NOT yet the production
`ActorRuntime.send` default. Treat both as incremental foundations.

## Independent regression gates

- Exact six golden MessagePack v2 vectors copied from Java
  `contracts/v1/design/messagepack_collection_vectors.json` including
  string, tuple, fixed array/list and variable array/list.
- Reject invalid shape/hash/schema, noncanonical or partial values,
  version mismatch, malformed UTF-8, oversized data, and payload type
  confusion.
- Verify mutable sender bytes cannot alter an accepted mail snapshot.
- Bounded overloaded single-lane send must reject without leaking.
- Independent actors can process concurrently; each actor remains
  logically single-flight. Stop drains and releases already-accepted data.
- Execute full Java Maven/JUnit and Graal native-image, LLVM CMake/CTest,
  and exact Oreslang guest-source parity **before** declaring release ready.
- Use funded CI exact Git blobs when source-org GitHub Actions fails before
  allocating a runner. No private cross-organization checkout tokens.

## Audit hardening gates (2026-10-11)

- **Snapshot before parsing**. Never validate caller-owned bytes and then
  copy them later: a concurrent sender mutation can otherwise bypass validation.
  Java variant dispatch and native transport decode the same detached snapshot
  subsequently owned by the receiver/admitted mailbox.
- **Pinned contract storage**. Native registration copies the schema ID,
  canonical shape descriptor, and SHA-256 hash into group-owned storage before
  spawn. Mutation or lifetime expiration of the host's registration buffer must
  not change the actor's schema.
- **Independently verified digest**. Native code computes SHA-256 over
  `oreslang.actor.shape.v2/` + the canonical shape descriptor, and rejects
  any supplied hash that differs. Wire-provided digests never authorize a new
  decoder or type.
- **Matched descriptor bounds**. Both compilers' actor-mail profiles enforce
  a maximum **4096 ASCII shape-descriptor bytes**, depth **64**, and frame
  length **1 MiB**. The generic Java MessagePack collection codec can admit
  additional trusted shapes; they are not automatically portable actor mail.
- **Float64/UTF-8**. Canonical scalar float64 encodes finite IEEE-754 binary64
  and rejects NaN/Infinity, just as both decoders reject malformed UTF-8 and
  noncanonical integer and collection headers.
- **Bounded pre-admission allocations**. The native adapter reserves a
  per-recipient in-flight DATA slot (queued, active, or preparing) before
  allocating, snapshotting, hashing or decoding input. Failed validation and
  rejected enqueue return the slot; admitted DATA returns its reservation only
  after its receiver callback and owned-frame finalizer finish. Capacity
  includes active turns, not merely pending queue entries.
- **Lifecycle rule**. Contract registration is a trusted single-threaded
  construction phase, complete before concurrent actor send/spawn. Scheduler
  shutdown/final destruction additionally requires producer-fencing; source
  lowering must enforce that capability boundary rather than exposing host
  registration/destruction to guest actors.

These guarantees are targeted host/runtime components, not yet all compiled
Oreslang guest-source, Graal AOT, or arbitrary Future<T> behavior.

## Remaining compiler integration gates

- Java/Graal and LLVM must derive the same closed contract descriptor and
  stable variant IDs from source enum/pattern declarations.
- Lower source actor send/receive to registered native/Java MessagePack v2
  endpoints, with copied/actor-owned decode, full static Serializable checks,
  non-aliasing ownership, and fail-closed unsupported payload diagnostics.
- Lower full exhaustive match with narrowing, tuple destructuring,
  unreachable/overlap/duplicate checking and missing alternative rejection.
- Generalize Future<T>, await, typed replies, CONTROL continuation resumption
  and finalized/cancelled ownership semantics without occupying carriers.
- Benchmark optional rt copy for local SHARED actors only after portable
  encode/decode semantics and isolation are proven identical.
