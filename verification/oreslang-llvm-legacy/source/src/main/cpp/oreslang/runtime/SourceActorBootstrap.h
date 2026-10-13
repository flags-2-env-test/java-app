#pragma once
#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include "oreslang/runtime/MessagePackActorV2.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Restricted compiler-generated source-actor bootstrap: a pointer to the
 * generated constructor and fixed number of actor-owned i64 fields.
 * Returned void* values are NON-DEREFERENCEABLE, monotonically-issued
 * 64-bit actor ID capabilities (supported native targets have >=64-bit
 * pointers). Never cast the returned value to a SourceActor* or inspect it.
 * Every public lookup validates the live registry identity under its lock.
 * A freed actor and a later actor reusing its scheduler slot cannot share a
 * capability. The scheduler-owned actor ref separately binds domain/slot/
 * generation. Handles must not be serialized as authority.
 * Source stop is only permitted from trusted zero-argument main, not an
 * actor carrier. Later await must use scheduler-admitted continuations.
 */
typedef void (*OresSourceActorInit)(OresActorI64Heap*, uint64_t, uint64_t);
typedef int64_t (*OresSourceActorMessage)(OresActorI64Heap*, uint64_t, uint64_t, int64_t);
void* oreslang_source_actor_spawn(OresSourceActorInit initializer,
                                  OresSourceActorMessage on_message,
                                  uint64_t fields);
/* Nonblocking bounded one-way send. Overload currently fails closed with a
 * host error; a typed guest overload result still needs compiler lowering.
 */
void oreslang_source_actor_send(void* actor, int64_t payload);

/*
 * Compiler-facing MessagePack v2 source actor mode.
 *
 * Every wire DATA message uses the SAME NativeActorScheduler actor lane,
 * actor-owned i64 heap, lifecycle barriers, and CONTROL initialization as
 * ordinary scalar source actors. A compiler-trusted descriptor is stable
 * during registration; it is copied first, then independently validated.
 * Caller-owned descriptor memory is not retained.
 *
 * This entrypoint registers ONE schema per actor. General enum/pattern
 * domains with multiple variants require explicit LLVM compiler lowering
 * and per-variant dispatch and MUST NOT be represented by an unchecked
 * caller-selected schema or an extra independent scheduler.
 *
 * Wire bytes belong to the admitted callback only. Decode into actor-owned
 * storage before retaining data beyond the turn. Configuration and stop
 * require trusted single-owner lifecycle; public DATA producers may race
 * trusted stop safely: a registry lookup pins a producer lease before
 * teardown can free its actor.
 */
typedef int64_t (*OresSourceActorWireMessage)(
    OresActorI64Heap*, uint64_t domain, uint64_t epoch,
    const uint8_t* wire, size_t wire_size);
void* oreslang_source_actor_spawn_wire(
    OresSourceActorInit initializer, OresSourceActorWireMessage on_wire,
    uint64_t fields, const OresMp2Contract* compiler_owned_contract);
/* Returns 1 on admission, 0 for malformed frame, full mailbox, stopped or
 * stale/fabricated actor capability. Caller retains input-byte ownership.
 * Registered wire actors cannot use scalar source_actor_send.
 */
int oreslang_source_actor_send_wire(
    void* actor, const uint8_t* wire, size_t size);

/* Closed typed mailbox: compiler registers a fixed ordered schema table.
 * variant_index is the compiler-local ordinal (0..contract_count-1), NOT
 * an enum's declared numeric wire tag. Schema identity is verified from
 * the MessagePack envelope and the complete typed payload is validated.
 * All variants share ONE native DATA queue, capacity budget and serial
 * actor execution lane. No runtime shape inference and no second pool.
 *
 * Registration copies all contracts before validating them. The supplied
 * contracts must remain stable for the duration of the registration call.
 * Duplicate schema IDs, invalid digests, empty/oversized domains and
 * unsupported descriptors fail closed without spawning an actor.
 *
 * send_variant returns zero for unknown/forged/noncanonical envelopes or
 * queue saturation; it never dispatches by payload-shape guessing. The
 * registered ordinal is independent of the declared enum wire tag.
 *
 * Trusted stop closes admission and waits for already-entered source DATA
 * calls before releasing contracts. Public send_wire/send_variant take the
 * registry lock and acquire their producer lease before dereferencing any
 * actor; callers may retain or attempt to use stale capabilities, which
 * reject instead of touching reclaimed actor memory. This is still a
 * restricted trusted-main bootstrap, not a general guest Future<T> ABI.
 */
typedef int64_t (*OresSourceActorVariantMessage)(
    OresActorI64Heap*, uint64_t domain, uint64_t epoch,
    size_t variant_index, const uint8_t* wire, size_t wire_size);
void* oreslang_source_actor_spawn_variants(
    OresSourceActorInit initializer,
    OresSourceActorVariantMessage on_variant,
    uint64_t fields, const OresMp2Contract* contracts,
    size_t contract_count);
int oreslang_source_actor_send_variant(
    void* actor, const uint8_t* wire, size_t size);
/* Compiler-only fail-closed DATA send. Never silently drop source
 * `ref.send(Enum.Case)` when the bounded mailbox rejects a message.
 * Only trusted main may invoke this synchronous, no-retry admission form.
 * Future<T> backpressure is a separate, not-yet-lowered path. */
/* Only compiled receive arms may call this after nominal admission.
 * Fails closed if an invalid or differently shaped payload reaches an
 * int64 binder; arbitrary caller memory is not an acceptable input. */
int64_t oreslang_source_actor_tagged_i64(const uint8_t* wire, size_t size);
void oreslang_source_actor_send_variant_checked(
    void* actor, const uint8_t* wire, size_t size);
/* REQUIRED path for Java-compatible 'define enum' / 'define pattern':
 * a variant's SHA-256 commits to tuple SHAPE but not the 'Status' name
 * or numeric case tag inside its tuple. Pin these nominal identities at
 * compile time and verify BOTH before entering a guest receive callback.
 *
 * The generic spawn_variants API above is structural-only; it is NOT
 * sufficient by itself for typed nominal enum source messages.
 * All tag descriptors are copied before verification and registration.
 */
typedef struct {
    const char* enum_name;
    uint32_t declared_tag; /* 1..INT32_MAX; NOT the variant index */
} OresSourceNominalCase;
void* oreslang_source_actor_spawn_tagged_variants(
    OresSourceActorInit initializer,
    OresSourceActorVariantMessage on_variant, uint64_t fields,
    const OresMp2Contract* contracts,
    const OresSourceNominalCase* identities, size_t contract_count);

/* Compiler-only bounded linear pipeline, 2..16 distinct nominally
 * compatible live actors. The stack reference array is borrowed ONLY
 * during registration, never retained by the native runtime. All
 * directional sends require a scheduler-issued DATA ingress context.
 * Current subset permits one in-flight flow per ordered chain. */
void oreslang_source_actor_pipeline_connect_many(void* const* stages, uint64_t count);
/* Legacy C embedding ABI; guest LLVM lowering always uses connect_many. */
void oreslang_source_actor_pipeline_connect(void* source, void* transform, void* sink);
void oreslang_source_actor_pipeline_emit_i64(uint64_t direction,
                                               uint64_t variant, int64_t value);
/* Read-only, scheduler-authored DATA direction for the currently running
 * compiled variant callback: 1 upstream->downstream, 2 downstream->upstream,
 * 0 without a pipeline delivery. Never accepts an untrusted direction arg. */
uint64_t oreslang_source_actor_pipeline_ingress(void);
/* Compiler-only invalid-direction or wrong-nominal-domain delivery gate.
 * Marks terminal pipeline failure, never invokes guest handler code. */
void oreslang_source_actor_pipeline_reject_delivery(void);
/* 0 is completed success. Nonzero is a terminal pipeline failure:
 * 1 wrong ingress, 2 invalid phase/replay, 3 rejected DATA admission,
 * 4 stage omitted its required hop, 5 timeout, 6 wrong final result,
 * 7 explicit cancellation. Unlike expect(), this never aborts. */
int32_t oreslang_source_actor_pipeline_expect_status(void* source,
                                                    int64_t expected);
/* Trusted-main cancellation shares the final DATA-publication mutex. */
void oreslang_source_actor_pipeline_cancel(void* source);
/* Legacy fatal assertion ABI. Guest code uses expect_status and returns
 * a nonzero process result only AFTER stopping every pipeline actor. */
void oreslang_source_actor_pipeline_expect(void* source, int64_t expected);

void oreslang_source_actor_stop(void* actor);
/* A source-compiled continuation runs on the root ActorGroup CONTROL lane.
 * Only the outer trusted-main ABI waits to return the program's i64 result;
 * no scheduler worker blocks while the readiness Future is pending. */
typedef int64_t (*OresSourceActorContinuation)(void* actor_ref);
int64_t oreslang_source_actor_await_ready(
    void* actor, OresSourceActorContinuation continuation);
#ifdef __cplusplus
}
#endif
