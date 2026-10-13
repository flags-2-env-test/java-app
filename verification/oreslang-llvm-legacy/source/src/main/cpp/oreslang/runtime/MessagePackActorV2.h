#ifndef ORESLANG_MESSAGEPACK_ACTOR_V2_H
#define ORESLANG_MESSAGEPACK_ACTOR_V2_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/*
 * Canonical transport is MessagePack v2, byte-identical to the Java
 * MessagePackCollectionCodec: [2, schema-id, bin8(sha256(shape)), payload].
 * DO NOT use the experimental OMSG/1 frame as the actor's default wire ABI.
 *
 * Contract data is trusted COMPILER output, stable for the duration
 * of registration. The group COPIES schema, shape and digest and owns them
 * until destruction. A hash is type compatibility, NOT authentication.
 * Shape grammar: B I F S T[...] FA[...] FL[...] A[shape] L[shape].
 */
typedef struct {
    const char* schema_id;
    const char* shape;
    const uint8_t* shape_sha256; /* exactly 32 bytes, compiler-pinned */
} OresMp2Contract;
typedef struct {
    uint8_t* bytes;
    size_t size;
} OresMp2Owned;
enum { ORES_MP2_MAX_BYTES=1048576, ORES_MP2_MAX_NODES=100000, ORES_MP2_MAX_DEPTH=64 };

/* Recompute and check schema/shape digest independently of the caller.
 * The declaration must come from trusted compiler-owned source metadata.
 */
int ores_mp2_contract_valid(const OresMp2Contract* contract);

/* Validate an already detached/owned snapshot IN PLACE. No new copy is
 * made. The caller retains ownership whether the frame is valid or not and
 * must call ores_mp2_owned_release exactly once on every exit path. Never
 * invoke on a caller-owned concurrently mutable send buffer. This is used
 * after reserve-and-snapshot by source actor variant dispatch. */
int ores_mp2_validate_owned(const OresMp2Contract* contract,
                            const OresMp2Owned* snapshot);
/* Companion to validate_owned: verify the compiler-pinned NOMINAL identity
 * stored inside Java tagged enum payload [enum_name, int_tag, data?].
 * Only call on a snapshot that already passed ores_mp2_validate_owned.
 * The schema/digest alone cannot attest a declared enum tag: two different
 * numbers are both valid shape I. This function fails closed on an incorrect
 * tuple arity, enum owner or declared positive signed-32 tag. */
int ores_mp2_tagged_identity_matches_validated(
    const OresMp2Contract* contract, const OresMp2Owned* validated,
    const char* enum_name, uint32_t declared_tag);
/* Trusted callback-side extractor for a fully validated tagged tuple
 * [enumOwner, declaredTag, int64Payload]. This DOES NOT authenticate or
 * validate the schema itself: only call after owned frame admission using
 * the compiler-pinned nominal contract. Returns zero on malformed data.
 */
int ores_mp2_extract_tagged_i64_trusted(const uint8_t* wire, size_t size,
                                        int64_t* out_payload);
int ores_mp2_validate_copy(const OresMp2Contract* contract,
                           const uint8_t* bytes, size_t size, OresMp2Owned* out);
void ores_mp2_owned_release(OresMp2Owned* value);

typedef struct OresMp2ActorGroup OresMp2ActorGroup;
typedef struct {
    uint64_t scheduler_domain;
    uint64_t actor_slot;
    uint64_t generation;
} OresMp2ActorRef;
/* Callback may read bytes only for this admitted actor turn. It must not
 * retain the pointer. The owner releases and zeroizes after it returns.
 */
typedef void (*OresMp2OnMessage)(void* context, size_t actor_slot,
                                  const uint8_t* bytes, size_t size);
OresMp2ActorGroup* ores_mp2_group_open(size_t actors, size_t queue_capacity,
                                      size_t carriers, OresMp2OnMessage on_message,
                                      void* context);
/* Configure once per slot BEFORE spawn. All contract bytes are deep-copied
 * and verified after copying; the caller retains them and may release them
 * immediately after this function returns. Construction/registration must
 * not race with spawn, send, stop or destroy of the same group. */
int ores_mp2_group_contract(OresMp2ActorGroup* group, size_t slot,
                            OresMp2Contract contract);
OresMp2ActorRef ores_mp2_group_spawn(OresMp2ActorGroup* group, size_t slot);
/* Reserves a bounded per-actor DATA slot BEFORE allocating or decoding.
 * Copies then validates the caller's frame against its pinned destination
 * schema; the caller retains input memory. Rejection releases both the
 * snapshot and reservation; acceptance transfers them to one actor lane.
 * Submitters MUST be fenced externally before group destruction. */
int ores_mp2_group_send(OresMp2ActorGroup* group, OresMp2ActorRef target,
                        const uint8_t* wire, size_t size);
/* Permanently close new DATA send admission, then wait for all producer
 * calls that entered before closure to exit their copy/validate/publish
 * section. Does not stop actors or wait for accepted callbacks. This object
 * MUST remain alive and all callers must join/fence BEFORE group_destroy;
 * no C API can safely accept an arbitrary stale pointer after free.
 * Returns 1 on completed closure, 0 for a null group.
 */
int ores_mp2_group_close_admission(OresMp2ActorGroup* group);
int ores_mp2_group_stop(OresMp2ActorGroup* group, OresMp2ActorRef target);
int ores_mp2_group_wait_done(OresMp2ActorGroup* group, OresMp2ActorRef target);
/* Only a trusted, non-carrier caller after producer fencing may destroy.
 * Calls retire + drain and joins worker carriers. */
int ores_mp2_group_destroy(OresMp2ActorGroup* group);
#ifdef __cplusplus
}
#endif
#endif
