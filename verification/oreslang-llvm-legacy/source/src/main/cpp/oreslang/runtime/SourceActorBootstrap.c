#include "oreslang/runtime/SourceActorBootstrap.h"
#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/NativeActorFuture.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

#define SOURCE_ACTOR_MAGIC UINT64_C(0x4f52455353504157)
/* Source ref ABI is a non-dereferenceable uint64 capability encoded in void*.
 * The supported native targets must retain all 64 token bits. */
_Static_assert(sizeof(uintptr_t) >= sizeof(uint64_t),
               "source actor handles require 64-bit pointer targets");
#define SOURCE_GROUP_SLOTS 64u
#define SOURCE_GROUP_CARRIERS 4u
#define SOURCE_GROUP_MAILBOX 128u
#define SOURCE_GROUP_ROOT_SLOT (SOURCE_GROUP_SLOTS - 1u)

#define SOURCE_MAX_VARIANTS 64u
#define SOURCE_PIPELINE_MAX_STAGES 16u
enum {
    SOURCE_PIPELINE_OK = 0,
    SOURCE_PIPELINE_INVALID_ROUTE = 1,
    SOURCE_PIPELINE_BAD_PHASE = 2,
    SOURCE_PIPELINE_REJECTED_MAIL = 3,
    SOURCE_PIPELINE_MISSING_HOP = 4,
    SOURCE_PIPELINE_TIMEOUT = 5,
    SOURCE_PIPELINE_WRONG_RESULT = 6,
    SOURCE_PIPELINE_CANCELLED = 7
};
#define SOURCE_PRODUCER_CLOSED (UINT64_C(1) << 63)
#define SOURCE_PRODUCER_COUNT (SOURCE_PRODUCER_CLOSED - 1)

typedef struct {
    OresMp2Contract value;
    char* owned_schema;
    char* owned_shape;
    uint8_t sha[32];
    char* owned_enum_name;
    uint32_t declared_tag;
} SourceVariantContract;

typedef struct {
    OresMp2Owned packet;
    size_t variant_index;
    uint8_t direction;
    /* Scheduler/host-owned, never decoded from guest MessagePack data.
     * The current source lowering permits ONE flow; a future multi-flow
     * adapter must replace the global phase, not bypass it. */
    uint64_t flow_id;
    uint64_t message_id;
    uint64_t parent_message_id;
    uint64_t destination_domain;
    uint64_t destination_generation;
} SourceWireDelivery;

typedef struct SourceActor {
    uint64_t magic;
    uint64_t id;
    struct SourceActor* upstream;
    struct SourceActor* downstream;
    size_t pipeline_index; /* bound once at compiler-owned registration */
    OresActorI64Heap* heap;
    OresNativeActorRef ref;
    OresSourceActorInit constructor;
    OresSourceActorMessage on_message;
    OresSourceActorWireMessage on_wire;
    OresSourceActorVariantMessage on_variant;
    SourceVariantContract* variants;
    size_t variant_count;
    atomic_size_t wire_reserved;
    atomic_uint_fast64_t producer_gate;
    atomic_int initialized;
    atomic_uint_fast64_t processed;
    atomic_int_fast64_t last_result;
} SourceActor;

typedef struct {
    pthread_cond_t completed;
    OresNativeActorFuture* ready_future;
    OresSourceActorContinuation continuation;
    SourceActor* actor;
    void* continuation_handle;
    int done;
    int64_t result;
} ReadyAwaitFrame;

typedef struct {
    pthread_mutex_t mutex;
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef root_ref;
    ReadyAwaitFrame* pending_ready;
    pthread_cond_t pipeline_completed;
    SourceActor* pipeline_source;
    int pipeline_done;
    int64_t pipeline_result;
    atomic_int pipeline_phase; /* 0..(2*(count-1)+1) */
    atomic_int pipeline_failure;
    atomic_int pipeline_root_sent;
    uint64_t pipeline_flow_id; /* identity committed on first root DATA admit */
    uint64_t pipeline_last_message_id; /* committed causal predecessor */
    SourceActor* pipeline_stages[SOURCE_PIPELINE_MAX_STAGES];
    size_t pipeline_count;
    atomic_uint_fast64_t pipeline_forward_count;
    atomic_uint_fast64_t pipeline_backward_count;
    SourceActor* actors[SOURCE_GROUP_SLOTS];
    size_t next_slot;
    size_t live;
    size_t peak;
} SourceActorGroup;

/* Process-local trusted-main group; the compiler only supports linear refs
 * in a zero-argument top-level main. There is exactly one worker pool per
 * active group, not one pool per actor.
 */
static SourceActorGroup group = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .pipeline_completed = PTHREAD_COND_INITIALIZER
};
/* The active context is issued only while a scheduler-owned DATA callback
 * is executing. A source program cannot forge it through message bytes. */
static _Thread_local SourceActor* active_pipeline_actor;
static _Thread_local uint8_t active_pipeline_ingress;
static _Thread_local uint64_t active_pipeline_flow_id;
static _Thread_local uint64_t active_pipeline_message_id;

/* Compiler-only actor callback dispatch; the trusted route direction lives
 * in carrier-local state for exactly the duration of the admitted DATA turn.
 * No guest-supplied flags or wire properties may set this value. */
uint64_t oreslang_source_actor_pipeline_ingress(void) {
    return active_pipeline_actor ? (uint64_t)active_pipeline_ingress : 0;
}
static _Atomic uint64_t next_pipeline_flow_id = 1;
static _Atomic uint64_t next_pipeline_message_id = 1;
/* Identity allocation is monotonic for the process lifetime, including
 * stop/restart and slot reuse. Return zero on exhaustion; never wrap. */
static uint64_t pipeline_issue_id(_Atomic uint64_t* counter) {
    uint64_t current = atomic_load_explicit(counter, memory_order_relaxed);
    while (current != UINT64_MAX) {
        if (atomic_compare_exchange_weak_explicit(counter, &current,
                current + 1, memory_order_relaxed, memory_order_relaxed))
            return current;
    }
    return 0;
}
/* Optional test-only scheduler fault point. Production builds neither
 * export nor invoke a testing callback. Never call under group.mutex. */
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
__attribute__((weak))
void oreslang_source_actor_test_before_pipeline_publish(void* actor,
                                                         unsigned direction) {
    (void)actor; (void)direction;
}
/* Deterministic fault injection on the caller's scheduler-issued TLS
 * ancestry, before acquiring group.mutex. No hook executes under the
 * publication/cancellation linearization monitor. Testing-only symbol. */
__attribute__((weak))
void oreslang_source_actor_test_corrupt_causal_parent(
    uint64_t* active_parent, unsigned direction) {
    (void)active_parent; (void)direction;
}
#endif
static _Atomic uint64_t next_actor_id = 4100;
static void check_invariant(int condition, const char* predicate, int line) {
    if (!condition) {
        fprintf(stderr, "SOURCE_ACTOR_INVARIANT_FAILURE line=%d check=%s\n",
                line, predicate);
        abort();
    }
}
#define ensure(expr) check_invariant((expr), #expr, __LINE__)
static void group_lock(void) { ensure(pthread_mutex_lock(&group.mutex) == 0); }
static void group_unlock(void) { ensure(pthread_mutex_unlock(&group.mutex) == 0); }
/* Public refs are monotonically issued ID capabilities, never heap addresses.
 * Lookup runs under group.mutex before inspecting any mutable SourceActor.
 * The ID is never recycled when the scheduler closes/reopens or slots reuse.
 * The DATA producer lease is acquired before releasing the registry lock. */
static SourceActor* source_resolve_locked(void* opaque) {
    const uint64_t token = (uint64_t)(uintptr_t)opaque;
    if (!token) return NULL;
    for (size_t slot = 0; slot < group.next_slot; ++slot) {
        SourceActor* actor = group.actors[slot];
        if (actor && actor->id == token
                && actor->magic == SOURCE_ACTOR_MAGIC
                && actor->ref.actor_slot == slot
                && actor->ref.scheduler_domain != 0) return actor;
    }
    return NULL;
}

/* Actor workers may signal failure, but never synchronously tear down peers
 * or terminate the process. Only the host-main waiter performs teardown. */
static void pipeline_fail(int code) {
    if (code <= 0) return;
    /* Publication and terminal cancellation share this mutex. The winner
     * commits first; an enqueue cannot slip in after failure publication.
     * Never wait for a foreign actor here: scheduler_send is nonblocking. */
    group_lock();
    int expected = SOURCE_PIPELINE_OK;
    const int won = atomic_compare_exchange_strong_explicit(
            &group.pipeline_failure, &expected, code,
            memory_order_acq_rel, memory_order_acquire);
    if (won) {
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
        fprintf(stderr, "SOURCE_PIPELINE_FAILURE code=%d\n", code);
#endif
        ensure(pthread_cond_broadcast(&group.pipeline_completed) == 0);
    }
    group_unlock();
}

static SourceActor* lookup(size_t index) {
    ensure(index < SOURCE_GROUP_SLOTS);
    SourceActor* actor = group.actors[index];
    ensure(actor && actor->magic == SOURCE_ACTOR_MAGIC);
    return actor;
}
static void initialize(void* raw, size_t slot, uint64_t payload) {
    ensure(raw == &group);
    if (slot == SOURCE_GROUP_ROOT_SLOT) {
        /* Root continuation is admitted as its own exclusive CONTROL turn.
         * It cannot run on the child constructor's carrier inline. */
        ensure(payload == 1);
        group_lock();
        ReadyAwaitFrame* frame = group.pending_ready;
        ensure(frame && !frame->done && frame->ready_future);
        ensure(ores_native_actor_future_status(frame->ready_future)
            == ORES_NATIVE_FUTURE_READY);
        OresSourceActorContinuation continuation = frame->continuation;
        void* handle = frame->continuation_handle;
        group_unlock();
        const int64_t result = continuation(handle);
        group_lock();
        frame->result = result;
        frame->done = 1;
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
        fputs("SOURCE_AWAIT_READY_CONTINUATION_PASS root_lane=1 real_future=1\n",
            stderr);
#endif
        ensure(pthread_cond_signal(&frame->completed) == 0);
        group_unlock();
        return;
    }
    ensure(payload == 0);
    SourceActor* instance = lookup(slot);
    OresActorTurn turn = ores_actor_i64_heap_enter(instance->heap, instance->id, 1);
    instance->constructor(instance->heap, turn.domain, turn.epoch);
    ores_actor_i64_heap_leave(instance->heap, turn);
    atomic_store_explicit(&instance->initialized, 1, memory_order_release);
    group_lock();
    /* Registration may race with initialization. Both paths serialize via
     * group.mutex and settle exactly once through NativeActorFuture. */
    ReadyAwaitFrame* frame = group.pending_ready;
    if (frame && frame->actor == instance
        && ores_native_actor_future_status(frame->ready_future)
            == ORES_NATIVE_FUTURE_PENDING) {
        /* Registration can observe initialized=1 before the initializer
         * acquires group.mutex. Only the still-pending Future gets settled;
         * an already-published READY wake must never be completed twice. */
        ensure(ores_native_actor_future_complete(
            frame->ready_future, ORES_NATIVE_FUTURE_READY));
    }
    group_unlock();
}
static void deliver(void* raw, size_t slot, uint64_t payload) {
    ensure(raw == &group);
    SourceActor* instance = lookup(slot);
    ensure((instance->on_message != NULL)
        + (instance->on_wire != NULL) + (instance->on_variant != NULL) == 1);
    ensure(atomic_load_explicit(&instance->initialized, memory_order_acquire) == 1);
    OresActorTurn turn = ores_actor_i64_heap_enter(instance->heap, instance->id, 1);
    int64_t result;
    int is_pipeline_backward = 0;
    if (instance->on_wire || instance->on_variant) {
        SourceWireDelivery* accepted = (SourceWireDelivery*)(uintptr_t)payload;
        ensure(accepted && accepted->packet.bytes && accepted->packet.size);
        ensure(accepted->variant_index < instance->variant_count);
        if (instance->on_variant) {
            SourceActor* prior = active_pipeline_actor;
            uint8_t prior_ingress = active_pipeline_ingress;
            uint64_t prior_flow_id = active_pipeline_flow_id;
            uint64_t prior_message_id = active_pipeline_message_id;
            if (instance->upstream || instance->downstream) {
                group_lock();
                /* DATA was enqueued with pinned scheduler identity, but a
                 * stopped/restarted actor must not receive stale context. */
                ensure(accepted->flow_id != 0
                    && accepted->flow_id == group.pipeline_flow_id
                    && accepted->message_id != 0
                    && accepted->destination_domain == instance->ref.scheduler_domain
                    && accepted->destination_generation == instance->ref.generation
                    && (accepted->direction == 0
                        ? accepted->parent_message_id == 0
                        : accepted->parent_message_id != 0));
                group_unlock();
            }
            active_pipeline_actor = instance;
            active_pipeline_ingress = accepted->direction;
            active_pipeline_flow_id = accepted->flow_id;
            active_pipeline_message_id = accepted->message_id;
            result = instance->on_variant(instance->heap, turn.domain, turn.epoch,
                    accepted->variant_index, accepted->packet.bytes,
                    accepted->packet.size);
            active_pipeline_ingress = prior_ingress;
            active_pipeline_actor = prior;
            active_pipeline_flow_id = prior_flow_id;
            active_pipeline_message_id = prior_message_id;
        } else {
            ensure(accepted->variant_index == 0 && instance->variant_count == 1);
            result = instance->on_wire(instance->heap, turn.domain, turn.epoch,
                    accepted->packet.bytes, accepted->packet.size);
        }
        const uint8_t direction = accepted->direction;
        ores_actor_i64_heap_leave(instance->heap, turn);
        ores_mp2_owned_release(&accepted->packet);
        free(accepted);
        size_t before = atomic_fetch_sub_explicit(
            &instance->wire_reserved, 1, memory_order_acq_rel);
        ensure(before > 0);
        is_pipeline_backward = direction == 2;
        if (instance->on_variant && group.pipeline_count >= 2) {
            const size_t index = instance->pipeline_index;
            const size_t count = group.pipeline_count;
            int minimum_after_turn = -1;
            if (index == 0 && direction == 0)
                minimum_after_turn = 1;
            else if (index > 0 && index + 1 < count && direction == 1)
                minimum_after_turn = (int)(index + 1);
            else if (index + 1 == count && direction == 1)
                minimum_after_turn = (int)count;
            else if (index > 0 && direction == 2)
                minimum_after_turn = (int)(2 * (count - 1) - index + 1);
            if (minimum_after_turn >= 0
                && atomic_load_explicit(&group.pipeline_phase,
                                        memory_order_acquire) < minimum_after_turn)
                pipeline_fail(SOURCE_PIPELINE_MISSING_HOP);
            else if (minimum_after_turn < 0 && !(index == 0 && direction == 2))
                pipeline_fail(SOURCE_PIPELINE_INVALID_ROUTE);
        }
    } else {
        result = instance->on_message(instance->heap, turn.domain,
                                      turn.epoch, (int64_t)payload);
        ores_actor_i64_heap_leave(instance->heap, turn);
    }
    atomic_store_explicit(&instance->last_result, result, memory_order_relaxed);
    atomic_fetch_add_explicit(&instance->processed, 1, memory_order_release);
    /* Completion is published only AFTER the DATA finalizer, actor-state
     * release, and processed-result publication have all completed. */
    if (instance->upstream || instance->downstream || is_pipeline_backward) {
        group_lock();
        if (is_pipeline_backward && group.pipeline_source == instance) {
            ensure(!group.pipeline_done);
            int expected = (int)(2 * (group.pipeline_count - 1));
            if (!atomic_compare_exchange_strong_explicit(
                &group.pipeline_phase, &expected, expected + 1,
                memory_order_acq_rel, memory_order_acquire)) {
                int zero = SOURCE_PIPELINE_OK;
                atomic_compare_exchange_strong_explicit(
                    &group.pipeline_failure, &zero, SOURCE_PIPELINE_BAD_PHASE,
                    memory_order_acq_rel, memory_order_acquire);
            } else if (!atomic_load_explicit(&group.pipeline_failure,
                                             memory_order_acquire)) {
                group.pipeline_result = result;
                group.pipeline_done = 1;
            }
        }
        /* Backward delivery can overtake the emitting actor's finalizer on
         * another scheduler worker. Every completed pipeline DATA turn must
         * wake the join predicate, not only the root's final reply. */
        ensure(pthread_cond_broadcast(&group.pipeline_completed) == 0);
        group_unlock();
    }
}
/* Compiler contracts are stable for the duration of the construction
 * call, but not necessarily afterwards. Validate only our own immutable
 * snapshot, not borrowed fields that can change after an initial check.
 */
static size_t source_bounded_length(const char* string, size_t max_length) {
    if (!string) return max_length + 1;
    for (size_t i = 0; i <= max_length; ++i)
        if (string[i] == '\0') return i;
    return max_length + 1;
}
static void release_source_variants(SourceVariantContract* variants,
                                    size_t count) {
    if (!variants) return;
    for (size_t i = 0; i < count; ++i) {
        free(variants[i].owned_schema);
        free(variants[i].owned_shape);
        free(variants[i].owned_enum_name);
        memset(variants[i].sha, 0, sizeof(variants[i].sha));
    }
    free(variants);
}

/* Compiler-generated domains are immutable and complete. Copy-before-check
 * prevents a mutable host descriptor from being validated under one shape
 * and registered under another. Duplicate schema IDs are ambiguous and
 * always rejected rather than matched by payload or declaration order. */
static SourceVariantContract* source_copy_variants(
    const OresMp2Contract* contracts, size_t count,
    const OresSourceNominalCase* nominal) {
    if (!contracts || count == 0 || count > SOURCE_MAX_VARIANTS) return NULL;
    SourceVariantContract* owned = calloc(count, sizeof(*owned));
    if (!owned) return NULL;
    for (size_t i = 0; i < count; ++i) {
        const OresMp2Contract* src = &contracts[i];
        if (!src->schema_id || !src->shape || !src->shape_sha256) goto fail;
        size_t slen = source_bounded_length(src->schema_id, 256);
        size_t tlen = source_bounded_length(src->shape, 4096);
        if (!slen || slen > 256 || !tlen || tlen > 4096) goto fail;
        owned[i].owned_schema = malloc(slen + 1);
        owned[i].owned_shape = malloc(tlen + 1);
        if (!owned[i].owned_schema || !owned[i].owned_shape) goto fail;
        memcpy(owned[i].owned_schema, src->schema_id, slen + 1);
        memcpy(owned[i].owned_shape, src->shape, tlen + 1);
        memcpy(owned[i].sha, src->shape_sha256, sizeof(owned[i].sha));
        owned[i].value = (OresMp2Contract){
            owned[i].owned_schema, owned[i].owned_shape, owned[i].sha
        };
        if (!ores_mp2_contract_valid(&owned[i].value)) goto fail;
        /* A Java/Oreslang typed enum schema is NOMINAL. The structural
         * registration entrypoint cannot guarantee its embedded enum owner
         * and tag. Require the pinned-tag API, never silently degrade it. */
        if (!nominal && strncmp(owned[i].value.schema_id,
                               "oreslang.enum.",
                               sizeof("oreslang.enum.") - 1) == 0)
            goto fail;
        if (nominal) {
            if (!nominal[i].enum_name || !nominal[i].declared_tag
                    || nominal[i].declared_tag > INT32_MAX
                    || strncmp(owned[i].value.shape, "T[S,I", 5) != 0
                    || (owned[i].value.shape[5] != ']'
                        && owned[i].value.shape[5] != ','))
                goto fail;
            size_t nlen = source_bounded_length(nominal[i].enum_name, 256);
            if (!nlen || nlen > 256) goto fail;
            /* Source enum owners are compiler identifiers, not arbitrary
             * runtime strings chosen by serialized wire payloads. */
            for (size_t j = 0; j < nlen; ++j) {
                const unsigned char ch = (unsigned char)nominal[i].enum_name[j];
                if (!((ch >= 'A' && ch <= 'Z')
                      || (ch >= 'a' && ch <= 'z')
                      || ch == '_' || (j > 0 && ch >= '0' && ch <= '9')))
                    goto fail;
            }
            owned[i].owned_enum_name = malloc(nlen + 1);
            if (!owned[i].owned_enum_name) goto fail;
            memcpy(owned[i].owned_enum_name, nominal[i].enum_name, nlen + 1);
            owned[i].declared_tag = nominal[i].declared_tag;
        }
        for (size_t earlier = 0; earlier < i; ++earlier) {
            if (strcmp(owned[earlier].value.schema_id,
                       owned[i].value.schema_id) == 0) goto fail;
        }
    }
    return owned;
fail:
    release_source_variants(owned, count);
    return NULL;
}
/* Returns zero only for invalid trusted registration. Configuration and
 * teardown are single-owner operations; concurrent host senders must be
 * fenced before stop to avoid use-after-free of the opaque actor handle.
 */
static void* source_spawn(
    OresSourceActorInit constructor, OresSourceActorMessage on_message,
    OresSourceActorWireMessage on_wire,
    OresSourceActorVariantMessage on_variant, uint64_t fields,
    const OresMp2Contract* contracts, size_t variant_count,
    const OresSourceNominalCase* nominal) {
    ensure(constructor != NULL && fields >= 1 && fields <= 4096);
    /* Constructor-only actors are valid; they never accept DATA messages.
     * Every actor with a message handler must choose exactly one transport. */
    ensure((on_message != NULL) + (on_wire != NULL)
        + (on_variant != NULL) <= 1);
    SourceVariantContract* registered = NULL;
    if (on_wire || on_variant) {
        registered = source_copy_variants(contracts, variant_count, nominal);
        if (!registered) return NULL;
    }
    group_lock();
    if (!group.scheduler) {
        ensure(group.live == 0 && group.next_slot == 0 && group.peak == 0
            && group.pending_ready == NULL);
        group.scheduler = ores_native_actor_scheduler_open_control(
            SOURCE_GROUP_SLOTS, SOURCE_GROUP_MAILBOX, SOURCE_GROUP_CARRIERS,
            deliver, initialize, &group);
        ensure(group.scheduler != NULL);
        group.root_ref = ores_native_actor_scheduler_spawn(
            group.scheduler, SOURCE_GROUP_ROOT_SLOT);
        ensure(group.root_ref.scheduler_domain != 0);
    }
    ensure(group.next_slot < SOURCE_GROUP_ROOT_SLOT);
    const size_t slot = group.next_slot++;
    SourceActor* instance = (SourceActor*)calloc(1, sizeof(*instance));
    ensure(instance != NULL);
    instance->magic = SOURCE_ACTOR_MAGIC;
    instance->id = atomic_fetch_add_explicit(&next_actor_id, 1, memory_order_relaxed);
    ensure(instance->id != 0 && instance->id != UINT64_MAX);
    instance->constructor = constructor;
    instance->on_message = on_message;
    instance->on_wire = on_wire;
    instance->on_variant = on_variant;
    instance->variants = registered;
    instance->variant_count = registered ? variant_count : 0;
    atomic_init(&instance->wire_reserved, 0);
    atomic_init(&instance->producer_gate, 0);
    instance->heap = ores_actor_i64_heap_open(instance->id, 1, fields);
    group.actors[slot] = instance;
    instance->ref = ores_native_actor_scheduler_spawn(group.scheduler, slot);
    ensure(instance->ref.scheduler_domain && instance->ref.actor_slot == slot);
    group.live++;
    if (group.live > group.peak) group.peak = group.live;
    /* CONTROL initializes the actual compiler-emitted actor constructor
     * ahead of normal messages, without blocking the submitting carrier. */
    ensure(ores_native_actor_scheduler_publish_control(
        group.scheduler, instance->ref, 0));
    group_unlock();
    return (void*)(uintptr_t)instance->id;
}
void* oreslang_source_actor_spawn(OresSourceActorInit constructor,
                                  OresSourceActorMessage on_message,
                                  uint64_t fields) {
    return source_spawn(constructor, on_message, NULL, NULL,
                        fields, NULL, 0, NULL);
}
void* oreslang_source_actor_spawn_wire(
    OresSourceActorInit constructor, OresSourceActorWireMessage on_wire,
    uint64_t fields, const OresMp2Contract* contract) {
    if (!on_wire || !contract) return NULL;
    return source_spawn(constructor, NULL, on_wire, NULL,
                        fields, contract, 1, NULL);
}
void* oreslang_source_actor_spawn_variants(
    OresSourceActorInit constructor,
    OresSourceActorVariantMessage on_variant, uint64_t fields,
    const OresMp2Contract* contracts, size_t contract_count) {
    if (!on_variant) return NULL;
    return source_spawn(constructor, NULL, NULL, on_variant,
                        fields, contracts, contract_count, NULL);
}
void* oreslang_source_actor_spawn_tagged_variants(
    OresSourceActorInit constructor, OresSourceActorVariantMessage on_variant,
    uint64_t fields, const OresMp2Contract* contracts,
    const OresSourceNominalCase* identities, size_t contract_count) {
    if (!on_variant || !identities) return NULL;
    return source_spawn(constructor, NULL, NULL, on_variant,
                        fields, contracts, contract_count, identities);
}
int64_t oreslang_source_actor_await_ready(
    void* opaque, OresSourceActorContinuation continuation) {
    ensure(continuation != NULL);
    group_lock();
    SourceActor* actor = source_resolve_locked(opaque);
    ensure(actor && group.scheduler && !group.pending_ready);
    const size_t slot = (size_t)actor->ref.actor_slot;
    ensure(slot < group.next_slot && group.actors[slot] == actor);
    ReadyAwaitFrame frame = {0};
    ensure(pthread_cond_init(&frame.completed, NULL) == 0);
    frame.actor = actor;
    frame.continuation_handle = opaque;
    frame.continuation = continuation;
    frame.ready_future = ores_native_actor_future_open(
        group.scheduler, group.root_ref, 1);
    ensure(frame.ready_future != NULL);
    group.pending_ready = &frame;
    if (atomic_load_explicit(&actor->initialized, memory_order_acquire)) {
        ensure(ores_native_actor_future_complete(
            frame.ready_future, ORES_NATIVE_FUTURE_READY));
    }
    /* External entrypoint barrier only. This caller is never a scheduler
     * worker; pending Future waits consume no ActorGroup carrier thread. */
    while (!frame.done) {
        ensure(pthread_cond_wait(&frame.completed, &group.mutex) == 0);
    }
    ensure(group.pending_ready == &frame);
    group.pending_ready = NULL;
    const int64_t result = frame.result;
    ores_native_actor_future_destroy(frame.ready_future);
    ensure(pthread_cond_destroy(&frame.completed) == 0);
    group_unlock();
    return result;
}
void oreslang_source_actor_send(void* opaque, int64_t payload) {
    /* Scalar send is a trusted-main, nonblocking operation; lock the registry
     * through its admission to prevent stop/recycle during the submission. */
    group_lock();
    SourceActor* instance = source_resolve_locked(opaque);
    ensure(instance && instance->on_message && !instance->on_wire
        && !(atomic_load_explicit(&instance->producer_gate,
                                  memory_order_acquire) & SOURCE_PRODUCER_CLOSED));
    ensure(ores_native_actor_scheduler_send(
        group.scheduler, instance->ref, (uint64_t)payload));
    group_unlock();
}
/* Peek the *canonical* schema ONLY from a detached owned wire snapshot.
 * The selected contract then validates those exact same bytes including
 * version, SHA and recursive payload shape; never touch caller memory twice.
 */
static int source_select_variant(const SourceActor* actor,
                                 const uint8_t* wire, size_t size,
                                 size_t* selected) {
    if (!wire || size < 5 || wire[0] != 0x94 || wire[1] != 2) return 0;
    size_t pos = 3;
    size_t schema_bytes;
    uint8_t head = wire[2];
    if ((head & 0xe0) == 0xa0) {
        schema_bytes = (size_t)(head & 31);
    } else if (head == 0xd9) {
        if (pos >= size) return 0;
        schema_bytes = wire[pos++];
        if (schema_bytes <= 31) return 0;
    } else if (head == 0xda) {
        if (size - pos < 2) return 0;
        schema_bytes = ((size_t)wire[pos] << 8) | wire[pos+1];
        pos += 2;
        if (schema_bytes <= 255) return 0;
    } else return 0;
    if (schema_bytes == 0 || schema_bytes > 256
            || schema_bytes > size - pos) return 0;
    for (size_t i = 0; i < actor->variant_count; ++i) {
        const char* name = actor->variants[i].value.schema_id;
        if (strlen(name) == schema_bytes
                && memcmp(name, wire + pos, schema_bytes) == 0) {
            *selected = i;
            return 1;
        }
    }
    return 0;
}
/* Producer leases protect the actor after a locked token lookup. A public
 * call never dereferences a stale/fabricated pointer; internal scheduler-owned
 * callbacks continue to pass live SourceActor* values directly. */
static int source_enter_producer(SourceActor* actor) {
    uint_fast64_t state = atomic_load_explicit(&actor->producer_gate,
                                               memory_order_acquire);
    do {
        if ((state & SOURCE_PRODUCER_CLOSED)
                || (state & SOURCE_PRODUCER_COUNT) == SOURCE_PRODUCER_COUNT)
            return 0;
    } while (!atomic_compare_exchange_weak_explicit(
        &actor->producer_gate, &state, state + 1,
        memory_order_acq_rel, memory_order_acquire));
    return 1;
}
static void source_leave_producer(SourceActor* actor) {
    uint_fast64_t prior = atomic_fetch_sub_explicit(
        &actor->producer_gate, 1, memory_order_acq_rel);
    ensure((prior & SOURCE_PRODUCER_COUNT) != 0);
}
/* Resolve + pin is atomic with respect to stop's terminal gate. Even if
 * callers race stop, the acquired lease protects actor lifetime until exit. */
static SourceActor* source_pin_public_producer(void* opaque) {
    group_lock();
    SourceActor* actor = source_resolve_locked(opaque);
    if (actor && !source_enter_producer(actor)) actor = NULL;
    group_unlock();
    return actor;
}
static int source_send_binary_unlocked(SourceActor* actor,
                                       const uint8_t* wire, size_t size,
                                       uint8_t direction);
static int source_send_binary_leased(SourceActor* actor,
                                     const uint8_t* wire, size_t size,
                                     uint8_t direction) {
    if ((actor->upstream || actor->downstream)
            && atomic_load_explicit(&group.pipeline_failure,
                                    memory_order_acquire) != SOURCE_PIPELINE_OK)
        return 0;
    /* Do not claim the single-flow root token yet: malformed/oversized
     * envelopes, allocation failures and full mailboxes must not consume
     * it. Admission and root ownership commit share group.mutex below. */
    return source_send_binary_unlocked(actor, wire, size, direction);
}
static int source_send_binary(SourceActor* actor,
                              const uint8_t* wire, size_t size,
                              uint8_t direction) {
    /* Used only with scheduler-owned live pointers in an executing turn. */
    if (!actor || actor->magic != SOURCE_ACTOR_MAGIC
            || !source_enter_producer(actor)) return 0;
    const int result = source_send_binary_leased(actor, wire, size, direction);
    source_leave_producer(actor);
    return result;
}
static int source_send_binary_unlocked(SourceActor* actor,
                              const uint8_t* wire, size_t size,
                              uint8_t direction) {
    if (!actor || actor->magic != SOURCE_ACTOR_MAGIC
            || (!actor->on_wire && !actor->on_variant) || !wire
            || size > ORES_MP2_MAX_BYTES) return 0;
    /* Every variant shares one admission counter and one actor scheduler
     * lane. Reserve BEFORE snapshot allocation or recursive typed parsing. */
    size_t pending = atomic_load_explicit(&actor->wire_reserved,
                                          memory_order_relaxed);
    do {
        if (pending >= SOURCE_GROUP_MAILBOX) return 0;
    } while (!atomic_compare_exchange_weak_explicit(
        &actor->wire_reserved, &pending, pending + 1,
        memory_order_acq_rel, memory_order_relaxed));
    /* Snapshot once BEFORE consulting the nominal schema ID. Previously
     * selection parsed caller-owned wire bytes before the codec copied
     * them, leading to two different reads of a mutable sender buffer.
     * The decoder now consumes only this identical pinned snapshot. */
    SourceWireDelivery* accepted = calloc(1, sizeof(*accepted));
    if (!accepted) {
        atomic_fetch_sub_explicit(&actor->wire_reserved, 1,
                                  memory_order_acq_rel);
        return 0;
    }
    if (size < 40 || size > ORES_MP2_MAX_BYTES) {
        free(accepted);
        atomic_fetch_sub_explicit(&actor->wire_reserved, 1,
                                  memory_order_acq_rel);
        return 0;
    }
    accepted->packet.bytes = malloc(size);
    if (!accepted->packet.bytes) {
        free(accepted);
        atomic_fetch_sub_explicit(&actor->wire_reserved, 1,
                                  memory_order_acq_rel);
        return 0;
    }
    accepted->packet.size = size;
    memcpy(accepted->packet.bytes, wire, size);
    size_t index = 0;
    if (!source_select_variant(actor, accepted->packet.bytes, size, &index)
            || !ores_mp2_validate_owned(&actor->variants[index].value,
                                         &accepted->packet)
            || (actor->variants[index].owned_enum_name
                && !ores_mp2_tagged_identity_matches_validated(
                    &actor->variants[index].value, &accepted->packet,
                    actor->variants[index].owned_enum_name,
                    actor->variants[index].declared_tag))) {
        ores_mp2_owned_release(&accepted->packet);
        free(accepted);
        atomic_fetch_sub_explicit(&actor->wire_reserved, 1,
                                  memory_order_acq_rel);
        return 0;
    }
    accepted->variant_index = index;
    accepted->direction = direction;
    /* The terminal-status / cancellation linearization point is shared
     * with final DATA publication. Validate and allocate outside the lock,
     * but never publish after cancellation won the group mutex. */
    const int pipeline_route = actor->upstream || actor->downstream;
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    /* Read the validated, actor-owned canonical frame BEFORE enqueue.
     * The receiving worker may free the frame immediately after admission.
     * Diagnostic traces only report successfully admitted DATA below. */
    int64_t admitted_trace_value = 0;
    const int has_admitted_trace = pipeline_route
        && ores_mp2_extract_tagged_i64_trusted(
            accepted->packet.bytes, accepted->packet.size,
            &admitted_trace_value);

    /* Allows a race test to force cancellation after validating owned wire
     * bytes but BEFORE the nonblocking scheduler admission commit. */
    /* The test hook compares the PUBLIC actor capability, not a private
     * heap address. Fault injection must still work after handle fencing. */
    if (pipeline_route)
        oreslang_source_actor_test_before_pipeline_publish(
            (void*)(uintptr_t)actor->id, direction);
    if (pipeline_route && direction != 0)
        oreslang_source_actor_test_corrupt_causal_parent(
            &active_pipeline_message_id, direction);
#endif
    if (pipeline_route) group_lock();
    /* Root ownership is a single atomic publication decision. Validate the
     * live source ID and one-flow token under the SAME mutex as terminal
     * cancellation and scheduler DATA publication. Competing valid roots
     * serialize; rejected frames never burn the only token. */
    const int root_allowed = !pipeline_route || direction != 0
        || (group.pipeline_source == actor
            && atomic_load_explicit(&group.pipeline_root_sent,
                                    memory_order_acquire) == 0);
    int context_valid = 1;
    if (pipeline_route && root_allowed) {
        /* Capture immutable route context at the same group-mutex
         * linearization point as physical DATA publication. The flow ID is
         * committed only after a successful scheduler admission. */
        accepted->flow_id = direction == 0
            ? pipeline_issue_id(&next_pipeline_flow_id)
            : active_pipeline_flow_id;
        accepted->message_id = pipeline_issue_id(&next_pipeline_message_id);
        accepted->parent_message_id = direction == 0 ? 0
            : active_pipeline_message_id;
        accepted->destination_domain = actor->ref.scheduler_domain;
        accepted->destination_generation = actor->ref.generation;
        /* Every accepted hop is the one successor of the last ADMITTED
         * message, not merely a nonzero caller-supplied parent. This is the
         * restricted linear one-flow proof. An actor's physical ingress
         * context is necessary but not sufficient: a stale earlier turn
         * cannot fork the route even if it still carries a valid flow ID.
         *
         * Both predecessor validation and last-message commit happen under
         * group.mutex, the same barrier as cancellation and DATA enqueue.
         * No new worker may read this context before the commit completes. */
        const int parent_current =
            accepted->parent_message_id == group.pipeline_last_message_id;
        const int authorized_source = direction == 0
            ? active_pipeline_actor == NULL
                && group.pipeline_source == actor
                && group.pipeline_last_message_id == 0
            : active_pipeline_actor != NULL
                && active_pipeline_actor != actor
                && active_pipeline_actor->ref.scheduler_domain
                     == actor->ref.scheduler_domain
                && (direction == 1
                    ? active_pipeline_actor->downstream == actor
                    : direction == 2
                        && active_pipeline_actor->upstream == actor);
        context_valid = accepted->flow_id != 0
            && accepted->message_id != 0
            && parent_current && authorized_source
            && (direction == 0
                ? accepted->parent_message_id == 0
                : accepted->parent_message_id != 0
                    && accepted->flow_id == group.pipeline_flow_id);
    }
    const int admitted = root_allowed && context_valid
        && (!pipeline_route
            || atomic_load_explicit(&group.pipeline_failure,
                                    memory_order_acquire) == SOURCE_PIPELINE_OK)
        && ores_native_actor_scheduler_send(group.scheduler, actor->ref,
                                             (uint64_t)(uintptr_t)accepted);
    /* A consumer can run on another carrier immediately after enqueue.
     * Commit both root admission and directional counts BEFORE releasing
     * the shared publication lock, otherwise final receive can overtake. */
    if (admitted && pipeline_route) {
        if (direction == 0)
            group.pipeline_flow_id = accepted->flow_id;
        group.pipeline_last_message_id = accepted->message_id;
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
        if (has_admitted_trace) {
            fprintf(stderr,
                "SOURCE_PIPELINE_ADMIT target=%zu direction=%u variant=%zu value=%lld\n",
                actor->pipeline_index, (unsigned)direction,
                index, (long long)admitted_trace_value);
        }
        fprintf(stderr,
            "SOURCE_PIPELINE_CONTEXT_ADMIT flow=%llu message=%llu parent=%llu target=%zu direction=%u domain=%llu generation=%llu\n",
            (unsigned long long)accepted->flow_id,
            (unsigned long long)accepted->message_id,
            (unsigned long long)accepted->parent_message_id,
            actor->pipeline_index, (unsigned)direction,
            (unsigned long long)accepted->destination_domain,
            (unsigned long long)accepted->destination_generation);
#endif
        if (direction == 0)
            atomic_store_explicit(&group.pipeline_root_sent, 1,
                                  memory_order_release);
        if (direction == 1)
            atomic_fetch_add_explicit(&group.pipeline_forward_count, 1,
                                      memory_order_acq_rel);
        else if (direction == 2)
            atomic_fetch_add_explicit(&group.pipeline_backward_count, 1,
                                      memory_order_acq_rel);
    }
    if (pipeline_route) group_unlock();
    if (!admitted) {
        ores_mp2_owned_release(&accepted->packet);
        free(accepted);
        atomic_fetch_sub_explicit(&actor->wire_reserved, 1,
                                  memory_order_acq_rel);
        return 0;
    }
    return 1;
}
int oreslang_source_actor_send_wire(void* opaque,
                                   const uint8_t* wire, size_t size) {
    SourceActor* actor = source_pin_public_producer(opaque);
    if (!actor) return 0;
    const int accepted = actor->on_wire && actor->variant_count == 1
        && source_send_binary_leased(actor, wire, size, 0);
    source_leave_producer(actor);
    return accepted;
}
int oreslang_source_actor_send_variant(void* opaque,
                                      const uint8_t* wire, size_t size) {
    SourceActor* actor = source_pin_public_producer(opaque);
    if (!actor) return 0;
    const int accepted = actor->on_variant
        && source_send_binary_leased(actor, wire, size, 0);
    source_leave_producer(actor);
    return accepted;
}
int64_t oreslang_source_actor_tagged_i64(const uint8_t* wire, size_t size) {
    int64_t value = 0;
    ensure(ores_mp2_extract_tagged_i64_trusted(wire, size, &value));
    return value;
}
void oreslang_source_actor_send_variant_checked(
    void* opaque, const uint8_t* wire, size_t size) {
    // A one-way source-level send must have observable failure semantics.
    // This narrow bootstrap uses terminal host error until Future<T>
    // backpressure/cancellation has a proper compiler continuation.
    if (oreslang_source_actor_send_variant(opaque, wire, size)) return;
    group_lock();
    const int pipeline_source = group.pipeline_source
        && group.pipeline_source->id == (uint64_t)(uintptr_t)opaque;
    group_unlock();
    if (pipeline_source) {
        pipeline_fail(SOURCE_PIPELINE_REJECTED_MAIL);
        return;
    }
    ensure(0 && "source actor typed admission rejected");
}

/* Bounded three-actor pipeline bootstrap. This is a linear, single-flow
 * compiler-owned route: no graph branching, cycles, or guest-owned refs.
 * Source main configures the route before sending and joins before stop. */
void oreslang_source_actor_pipeline_connect_many(void* const* opaque,
                                                 uint64_t requested_count) {
    ensure(opaque && requested_count >= 2
        && requested_count <= SOURCE_PIPELINE_MAX_STAGES);
    const size_t count = (size_t)requested_count;
    SourceActor* stages[SOURCE_PIPELINE_MAX_STAGES] = {0};
    /* The stack array is borrowed only during registration. Resolve every
     * capability while holding the registry lock: never dereference it. */
    group_lock();
    ensure(group.scheduler && !group.pipeline_source && group.pipeline_count == 0);
    for (size_t i = 0; i < count; ++i)
        stages[i] = source_resolve_locked(opaque[i]);
    SourceActor* const first = stages[0];
    ensure(first && first->magic == SOURCE_ACTOR_MAGIC
        && first->variant_count >= 1 && first->on_variant);
    for (size_t i = 0; i < count; ++i) {
        SourceActor* actor = stages[i];
        ensure(actor && actor->magic == SOURCE_ACTOR_MAGIC && actor->on_variant
            && !actor->upstream && !actor->downstream
            && actor->variant_count == first->variant_count
            && actor->ref.actor_slot < group.next_slot
            && group.actors[actor->ref.actor_slot] == actor);
        for (size_t k = 0; k < i; ++k) ensure(stages[k] != actor);
        for (size_t v = 0; v < first->variant_count; ++v) {
            const SourceVariantContract* lhs = &first->variants[v];
            const SourceVariantContract* rhs = &actor->variants[v];
            ensure(lhs->owned_enum_name && rhs->owned_enum_name
                && lhs->declared_tag == rhs->declared_tag
                && strcmp(lhs->owned_enum_name, rhs->owned_enum_name) == 0
                && strcmp(lhs->value.schema_id, rhs->value.schema_id) == 0
                && strcmp(lhs->value.shape, rhs->value.shape) == 0
                && memcmp(lhs->sha, rhs->sha, sizeof(lhs->sha)) == 0);
        }
    }
    for (size_t i = 0; i < count; ++i) {
        stages[i]->pipeline_index = i;
        stages[i]->upstream = i > 0 ? stages[i - 1] : NULL;
        stages[i]->downstream = i + 1 < count ? stages[i + 1] : NULL;
        group.pipeline_stages[i] = stages[i];
    }
    group.pipeline_count = count;
    group.pipeline_flow_id = 0;
    group.pipeline_last_message_id = 0;
    group.pipeline_source = first;
    atomic_store_explicit(&group.pipeline_forward_count, 0, memory_order_release);
    atomic_store_explicit(&group.pipeline_backward_count, 0, memory_order_release);
    group.pipeline_done = 0;
    group.pipeline_result = 0;
    atomic_store_explicit(&group.pipeline_phase, 0, memory_order_release);
    atomic_store_explicit(&group.pipeline_failure, 0, memory_order_release);
    atomic_store_explicit(&group.pipeline_root_sent, 0, memory_order_release);
    group_unlock();
}
/* Retain old C embedding ABI; the compiler uses the general ordered route. */
void oreslang_source_actor_pipeline_connect(void* first, void* middle, void* last) {
    void* const stages[3] = {first, middle, last};
    oreslang_source_actor_pipeline_connect_many(stages, 3);
}
/* The compiler inserts this gate in the transform callback's invalid
 * ingress/direction switch arm. Do not allow wrong-domain frames to fall
 * through as a normal user-handler return and later time out ambiguously. */
void oreslang_source_actor_pipeline_reject_delivery(void) {
    SourceActor* actor = active_pipeline_actor;
    if (!actor || actor->magic != SOURCE_ACTOR_MAGIC)
        return; /* no user hook is invoked without admitted DATA authority */
    pipeline_fail(SOURCE_PIPELINE_INVALID_ROUTE);
}
static size_t pipeline_write_positive(uint8_t* dst, uint64_t n) {
    if (n <= 127) { dst[0] = (uint8_t)n; return 1; }
    unsigned width = n <= UINT8_MAX ? 1 : n <= UINT16_MAX ? 2
                    : n <= UINT32_MAX ? 4 : 8;
    dst[0] = width == 1 ? 0xcc : width == 2 ? 0xcd
           : width == 4 ? 0xce : 0xcf;
    for (unsigned i = 0; i < width; ++i)
        dst[1 + i] = (uint8_t)(n >> (8 * (width - i - 1)));
    return 1 + width;
}
static size_t pipeline_write_i64(uint8_t* dst, int64_t number) {
    if (number >= 0) return pipeline_write_positive(dst, (uint64_t)number);
    if (number >= -32) { dst[0] = (uint8_t)number; return 1; }
    unsigned width = number >= INT8_MIN ? 1 : number >= INT16_MIN ? 2
                    : number >= INT32_MIN ? 4 : 8;
    dst[0] = width == 1 ? 0xd0 : width == 2 ? 0xd1
           : width == 4 ? 0xd2 : 0xd3;
    for (unsigned i = 0; i < width; ++i)
        dst[1 + i] = (uint8_t)((uint64_t)number >> (8 * (width - i - 1)));
    return 1 + width;
}
static size_t pipeline_write_string(uint8_t* dst, const char* s) {
    size_t n = strlen(s);
    ensure(n > 0 && n <= 256);
    size_t off = 0;
    if (n <= 31) dst[off++] = (uint8_t)(0xa0 | n);
    else if (n <= 255) { dst[off++] = 0xd9; dst[off++] = (uint8_t)n; }
    else { dst[off++] = 0xda; dst[off++] = 1; dst[off++] = 0; }
    memcpy(dst + off, s, n);
    return off + n;
}
void oreslang_source_actor_pipeline_emit_i64(uint64_t direction,
                                              uint64_t variant, int64_t value) {
    SourceActor* actor = active_pipeline_actor;
    ensure(actor && actor->magic == SOURCE_ACTOR_MAGIC
        && (direction == 1 || direction == 2) && variant < actor->variant_count);
    SourceActor* target = direction == 1 ? actor->downstream : actor->upstream;
    ensure(target && target->magic == SOURCE_ACTOR_MAGIC);
    /* Only the actual scheduler-admitted ingress may authorize a directional
     * hop. The one-use phase CAS prevents duplicate, reordered or reentrant
     * sends, including when multiple actors occupy different workers. */
    const size_t index = actor->pipeline_index;
    const size_t count = group.pipeline_count;
    ensure(count >= 2 && count <= SOURCE_PIPELINE_MAX_STAGES
        && index < count && group.pipeline_stages[index] == actor);
    int expected_phase = -1;
    if (direction == 1 && index + 1 < count
        && active_pipeline_ingress == (index == 0 ? 0 : 1))
        expected_phase = (int)index;
    else if (direction == 2 && index > 0
        && active_pipeline_ingress == (index + 1 == count ? 1 : 2))
        expected_phase = (int)(2 * (count - 1) - index);
    if (expected_phase < 0) {
        pipeline_fail(SOURCE_PIPELINE_INVALID_ROUTE);
        return;
    }
    group_lock();
    if (atomic_load_explicit(&group.pipeline_failure, memory_order_acquire)) {
        group_unlock();
        return;
    }
    if (!atomic_compare_exchange_strong_explicit(
        &group.pipeline_phase, &expected_phase, expected_phase + 1,
        memory_order_acq_rel, memory_order_acquire)) {
        group_unlock();
        pipeline_fail(SOURCE_PIPELINE_BAD_PHASE);
        return;
    }
    group_unlock();
    const SourceVariantContract* contract = &actor->variants[variant];
    ensure(contract->owned_enum_name && contract->declared_tag
        && strcmp(contract->value.shape, "T[S,I,I]") == 0);
    /* Only immutable compiler-registered nominal cases may be emitted.
     * Snapshot and validate at the recipient before bounded DATA admission. */
    uint8_t wire[1024];
    size_t at = 0;
    wire[at++] = 0x94;
    wire[at++] = 0x02;
    at += pipeline_write_string(wire + at, contract->value.schema_id);
    wire[at++] = 0xc4;
    wire[at++] = 32;
    memcpy(wire + at, contract->sha, 32);
    at += 32;
    wire[at++] = 0x93;
    at += pipeline_write_string(wire + at, contract->owned_enum_name);
    at += pipeline_write_positive(wire + at, contract->declared_tag);
    at += pipeline_write_i64(wire + at, value);
    ensure(at < sizeof(wire));
    /* This first slice is single-flow and fail-closed on saturation.
     * Future<T>-based suspension/backpressure belongs to the next slice. */
    /* The target admission path commits direction counters atomically with
     * bounded scheduler enqueue, before consumers can publish completion. */
    if (!source_send_binary(target, wire, at, (uint8_t)direction))
        pipeline_fail(SOURCE_PIPELINE_REJECTED_MAIL);
}
/* Returns a recoverable status; only trusted main may wait or tear down
 * the pipeline. Carrier workers never wait for downstream DATA delivery.
 * A terminal error seals subsequent admission, but final physical actor
 * teardown is performed by main after this routine returns. */
int32_t oreslang_source_actor_pipeline_expect_status(void* source,
                                                    int64_t expected) {
    group_lock();
    SourceActor* actor = source_resolve_locked(source);
    if (!actor) {
        group_unlock();
        return SOURCE_PIPELINE_INVALID_ROUTE;
    }
    const size_t count = group.pipeline_count;
    if (group.pipeline_source != actor || count < 2
            || count > SOURCE_PIPELINE_MAX_STAGES) {
        group_unlock();
        return SOURCE_PIPELINE_INVALID_ROUTE;
    }
    struct timespec deadline;
    ensure(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 20;
    int32_t status = SOURCE_PIPELINE_OK;
    for (;;) {
        status = atomic_load_explicit(&group.pipeline_failure, memory_order_acquire);
        if (status) break;
        bool finished = group.pipeline_done;
        for (size_t index = 0; index < count; ++index) {
            SourceActor* stage = group.pipeline_stages[index];
            ensure(stage && stage->magic == SOURCE_ACTOR_MAGIC);
            const uint64_t expected_turns = index + 1 == count ? 1 : 2;
            if (atomic_load_explicit(&stage->processed, memory_order_acquire)
                != expected_turns) finished = false;
            if (atomic_load_explicit(&stage->wire_reserved, memory_order_acquire)
                != 0) finished = false;
        }
        if (finished) break;
        int rc = pthread_cond_timedwait(
            &group.pipeline_completed, &group.mutex, &deadline);
        if (rc != 0) {
            status = SOURCE_PIPELINE_TIMEOUT;
            int zero = SOURCE_PIPELINE_OK;
            atomic_compare_exchange_strong_explicit(
                &group.pipeline_failure, &zero, status,
                memory_order_acq_rel, memory_order_acquire);
            status = atomic_load_explicit(&group.pipeline_failure,
                                          memory_order_acquire);
            break;
        }
    }
    if (status == SOURCE_PIPELINE_OK) {
        if (group.pipeline_result != expected
            || atomic_load_explicit(&actor->last_result,
                                    memory_order_acquire) != expected) {
            status = SOURCE_PIPELINE_WRONG_RESULT;
        } else if (atomic_load_explicit(&group.pipeline_phase,
                                         memory_order_acquire)
                    != (int)(2 * (count - 1) + 1)
            || atomic_load_explicit(&group.pipeline_forward_count,
                                    memory_order_acquire) != count - 1
            || atomic_load_explicit(&group.pipeline_backward_count,
                                    memory_order_acquire) != count - 1) {
            status = SOURCE_PIPELINE_BAD_PHASE;
        }
        if (status != SOURCE_PIPELINE_OK) {
            int zero = SOURCE_PIPELINE_OK;
            atomic_compare_exchange_strong_explicit(
                &group.pipeline_failure, &zero, status,
                memory_order_acq_rel, memory_order_acquire);
        }
    }
    group_unlock();
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    if (status == SOURCE_PIPELINE_OK) {
        fprintf(stderr,
            "SOURCE_PIPELINE_ROUNDTRIP_PASS actors=%zu typed=1 forward=%zu backward=%zu result=%lld\n",
            count, count - 1, count - 1, (long long)expected);
    } else {
        fprintf(stderr, "SOURCE_PIPELINE_TERMINAL_STATUS code=%d\n", status);
    }
#endif
    return status;
}
void oreslang_source_actor_pipeline_cancel(void* source) {
    /* Cancellation and DATA publication use the same group mutex: neither
     * can overtake the other once admitted. Verify live token identity under
     * that barrier so an old source cannot cancel a new pipeline. */
    group_lock();
    SourceActor* actor = source_resolve_locked(source);
    if (actor && actor == group.pipeline_source) {
        int expected = SOURCE_PIPELINE_OK;
        if (atomic_compare_exchange_strong_explicit(
                &group.pipeline_failure, &expected, SOURCE_PIPELINE_CANCELLED,
                memory_order_acq_rel, memory_order_acquire)) {
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
            fprintf(stderr, "SOURCE_PIPELINE_FAILURE code=%d\n",
                    SOURCE_PIPELINE_CANCELLED);
#endif
            ensure(pthread_cond_broadcast(&group.pipeline_completed) == 0);
        }
    }
    group_unlock();
}
void oreslang_source_actor_pipeline_expect(void* source, int64_t expected) {
    ensure(oreslang_source_actor_pipeline_expect_status(source, expected)
           == SOURCE_PIPELINE_OK);
}

void oreslang_source_actor_stop(void* opaque) {
    group_lock();
    SourceActor* instance = source_resolve_locked(opaque);
    ensure(instance != NULL);
    const size_t slot = (size_t)instance->ref.actor_slot;
    ensure(group.scheduler && slot < group.next_slot && group.actors[slot] == instance);
    /* Once this bit is set no new sender lease can enter. Producers that
     * already entered must finish their validation/enqueue or reject paths
     * before actor-owned contracts can be freed. */
    uint_fast64_t old_gate = atomic_fetch_or_explicit(
        &instance->producer_gate, SOURCE_PRODUCER_CLOSED,
        memory_order_acq_rel);
    ensure((old_gate & SOURCE_PRODUCER_CLOSED) == 0);
    ensure(ores_native_actor_scheduler_stop(group.scheduler, instance->ref));
    /* Never hold the ActorGroup mutex during a blocking host join.
     * A finishing initializer itself needs that mutex to publish readiness.
     * Trusted main is the sole source stop owner in this compiler subset. */
    group_unlock();
    /* Shutdown-only barrier, not cooperative polling inside actor execution.
     * The caller is the trusted external host, never a scheduler carrier. */
    while ((atomic_load_explicit(&instance->producer_gate,
                                memory_order_acquire) & SOURCE_PRODUCER_COUNT) != 0) {
        sched_yield();
    }
    ensure(ores_native_actor_scheduler_wait_done(group.scheduler, instance->ref));
    group_lock();
    ensure(group.actors[slot] == instance);
    ensure(atomic_load_explicit(&instance->initialized, memory_order_acquire) == 1);
    ensure(ores_native_actor_scheduler_poll_done(group.scheduler, instance->ref));
    ores_actor_i64_heap_retire(instance->heap);
    ensure(ores_actor_i64_heap_try_destroy(instance->heap));
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    fprintf(stderr,
        "SOURCE_ACTOR_SPAWN_STOP_VERIFIED constructor_on_worker=1 released=1 processed=%llu last=%lld\n",
        (unsigned long long)atomic_load_explicit(&instance->processed, memory_order_acquire),
        (long long)atomic_load_explicit(&instance->last_result, memory_order_relaxed));
#endif
    group.actors[slot] = NULL;
    if (instance == group.pipeline_source) {
        ensure(group.pipeline_done ||
            atomic_load_explicit(&group.pipeline_failure, memory_order_acquire) != 0);
        group.pipeline_source = NULL;
        for (size_t i = 0; i < group.pipeline_count; ++i) {
            group.pipeline_stages[i] = NULL;
        }
        group.pipeline_count = 0;
    }
    ensure(atomic_load_explicit(&instance->wire_reserved,
                                 memory_order_acquire) == 0);
    release_source_variants(instance->variants, instance->variant_count);
    instance->magic = 0;
    free(instance);
    ensure(group.live > 0);
    group.live--;
    if (group.live == 0) {
        ensure(group.pending_ready == NULL);
        /* The root also lives on the shared pool, with an exclusive lane. */
        ensure(ores_native_actor_scheduler_stop(group.scheduler, group.root_ref));
        ensure(ores_native_actor_scheduler_wait_done(group.scheduler, group.root_ref));
        /* All children and root continuations are quiescent. */
        ores_native_actor_scheduler_retire(group.scheduler);
        ensure(ores_native_actor_scheduler_try_destroy(group.scheduler));
        group.scheduler = NULL;
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
        fprintf(stderr,
            "SOURCE_ACTOR_GROUP_VERIFIED active=0 slots=%zu peak=%zu carriers=%u\n",
            group.next_slot, group.peak, SOURCE_GROUP_CARRIERS);
#endif
        group.next_slot = 0;
        group.peak = 0;
    }
    group_unlock();
}
