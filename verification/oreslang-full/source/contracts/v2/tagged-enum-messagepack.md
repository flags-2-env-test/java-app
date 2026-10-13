# Canonical tagged enum actor messages — MessagePack v2

Status: **Java/GraalVM executable; LLVM native decoder byte parity implemented;
LLVM guest tagged-enum lowering pending**.

## The only production tagged-message shape

The underlying actor wire remains the canonical MessagePack v2 four-element
array:

```text
[2, schema_id, bin8(SHA256(canonical_shape_descriptor)), typed_payload]
```

For an enum variant `Enum.Case` with positive declared tag `n` and
serializable payload of type `T`, the **production Java**
`TaggedMessageCodec` uses:

```text
schema_id = "oreslang.enum." + SHA256_hex(domainIdentity)
                            + "." + SHA256_hex("Enum.Case#n")
canonical_shape_descriptor = "T[S,I,<shape(T)>]"
typed_payload = ["Enum", n, payload]
```

For a valueless enum case the shape is `T[S,I]` and the payload is
`["Enum", n]`. The source-level distinction between two alternatives with
identical string, tuple or list payloads is **nominal**: name and exact tag,
not a speculative shape-first match.

The domain identity for `define pattern Messages ... end` is the SHA-256
of the UTF-8 text `oreslang.tagged.domain.v1/Messages/` followed by the
lexically sorted, fully qualified alternative names, each encoded as
`<name_length>:<name>;`. Reordering alternatives does not change the
wire identity; the source matcher must still respect source arm order.
Adding/removing an alternative or changing a tag breaks compatibility.

The Java `TaggedActorMessageBinding` is a compiler-facing adapter that
**delegates** actual encoding and decoding to `TaggedMessageCodec`.
Earlier experimental Java `MessagePackActorContract` per-ordinal
`domain#variant=N` schema IDs are NOT source actor tagged enum
transport. Do not emit those IDs for `define enum` or `define pattern`,
and do not reintroduce experimental `OMSG/1`.

## Portable vector

`contracts/v1/design/tagged_message_vectors.json` must be the same Git
blob in Java and LLVM (initial audited blob
`c379d148baa35e80a4c8448e201f7ce3fe6629da`). The vector uses
`Status.Message2 = 2` carrying `"hello"`, with shape descriptor
`T[S,I,S]` and SHA-256-backed schema identity. Native strict decoder
must validate exact Java bytes, copied ownership, digest, version, type
and rejection of truncation/forgery. The schema digest is NOT a signature
or authentication token.

## Required compiler integration

1. Resolve enum/pattern domain and a selected explicit `Enum.Case`
   **statically**. Reject unlisted variants, untyped payloads, mutable
   alias escape, unsupported codecs and unknown source shapes.
2. Serialize before bounded DATA admission, retaining sender ownership
   and transferring exactly one detached validated snapshot to the
   recipient's actor lane. CONTROL, ready/done and finalizer events
   retain their distinct supervised scheduling semantics.
3. Decode only against compiler-registered alternatives and dispatch
   exhaustive `match` or procedural `do match` without implicit
   variant inference. Reject duplicates, missing variants and
   unreachable patterns at compile time.
4. Verify Java/Graal and LLVM guest-source behavior, not only host C/JVM
   codecs. Full Maven, Graal native image, LLVM CMake/CTest, sanitizers,
   cancellation, lifecycle and load checks are required before merging.

Java source guest tagged send and the native C MessagePack decoder exist.
LLVM `SourceActorBootstrap` now also has a compiler-facing single-schema
MessagePack DATA ABI on the same logical scheduler lane, but the LLVM
frontend still needs to emit variant-specific schema bindings and
generalized tagged receive lowering. Keep both backends' PRs draft until
this is tested end-to-end.
