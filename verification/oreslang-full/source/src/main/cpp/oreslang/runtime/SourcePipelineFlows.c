#include "oreslang/runtime/SourcePipelineFlows.h"
#include <limits.h>
#include <errno.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Intentionally independent of the legacy single-flow global phase.
 * The runtime adapter must preserve scheduler producer leases, actor turn
 * finalization, and generation-checked mailbox publication. No guest FFI. */
typedef struct {
    uint64_t id;
    uint64_t pipeline_id;
    uint64_t route_generation;
    uint64_t last_message;
    uint8_t stage_count;
    OresFlowActor stages[ORES_FLOW_MAX_STAGES];
    uint32_t phase;
    OresFlowTerminal state;
    int reserved;
    OresFlowTicket ticket;
} Flow;

struct OresFlowTable {
    /* Only indexes, active slot assignment, and global-ID allocation.
     * A slow publisher cannot hold this lock while admitting DATA. */
    pthread_mutex_t mutex;
    pthread_mutex_t flow_locks[ORES_FLOW_MAX_ACTIVE];
    Flow flows[ORES_FLOW_MAX_ACTIVE];
};
/* Never recycle source identities when tables are destroyed/recreated.
 * Every increment is protected by table.mutex for its critical-section
 * ordering, but IDs are atomic across multiple independent tables. */
static _Atomic uint64_t next_flow_id = 1;
static _Atomic uint64_t next_message_id = 1;
/* Reject ALL synchronous registry entry from any publish callback.
 * Checking only callback_table == table permits A->B->A lock inversion:
 * B's nested publisher reset the TLS sentinel while A still owned a
 * flow lock. Cross-thread callbacks must still be strictly nonblocking. */
static _Thread_local const OresFlowTable* callback_table;
static Flow* find_flow(OresFlowTable* table, uint64_t id);
static uint64_t allocate_identity(_Atomic uint64_t* counter) {
    uint64_t current = atomic_load_explicit(counter, memory_order_relaxed);
    while (current != UINT64_MAX) {
        if (atomic_compare_exchange_weak_explicit(counter, &current,
                current + 1, memory_order_relaxed, memory_order_relaxed))
            return current;
    }
    return 0;
}
static void release_flow(OresFlowTable* table, Flow* flow) {
    pthread_mutex_unlock(&table->flow_locks[flow - table->flows]);
}
/* Acquire the flow's own mutex without holding table.mutex while waiting.
 * Retire/reuse checks the exact ID again under this lock. */
static Flow* acquire_flow(OresFlowTable* table, uint64_t id) {
    pthread_mutex_lock(&table->mutex);
    Flow* flow = find_flow(table, id);
    pthread_mutex_unlock(&table->mutex);
    if (!flow) return NULL;
    pthread_mutex_lock(&table->flow_locks[flow - table->flows]);
    if (flow->id != id) {
        release_flow(table, flow);
        return NULL;
    }
    return flow;
}

static int actor_equal(OresFlowActor a, OresFlowActor b) {
    return a.actor_id == b.actor_id &&
           a.actor_generation == b.actor_generation &&
           a.scheduler_domain == b.scheduler_domain;
}
static int valid_actor(OresFlowActor actor) {
    return actor.actor_id != 0 && actor.actor_generation != 0 &&
           actor.scheduler_domain != 0;
}
static Flow* find_flow(OresFlowTable* table, uint64_t id) {
    for (size_t i = 0; i < ORES_FLOW_MAX_ACTIVE; ++i)
        if (table->flows[i].id == id) return &table->flows[i];
    return NULL;
}
static int ticket_equal(const OresFlowTicket* a, const OresFlowTicket* b) {
    return a->flow_id == b->flow_id &&
           a->correlation_id == b->correlation_id &&
           a->pipeline_id == b->pipeline_id &&
           a->route_generation == b->route_generation &&
           a->message_id == b->message_id &&
           a->parent_message_id == b->parent_message_id &&
           actor_equal(a->sender, b->sender) &&
           actor_equal(a->destination, b->destination) &&
           a->direction == b->direction && a->stage_index == b->stage_index;
}
OresFlowTable* ores_flow_table_create(void) {
    OresFlowTable* table = calloc(1, sizeof(*table));
    if (!table) return NULL;
    if (pthread_mutex_init(&table->mutex, NULL) != 0) {
        free(table);
        return NULL;
    }
    for (size_t i = 0; i < ORES_FLOW_MAX_ACTIVE; ++i) {
        if (pthread_mutex_init(&table->flow_locks[i], NULL) != 0) {
            while (i > 0) pthread_mutex_destroy(&table->flow_locks[--i]);
            pthread_mutex_destroy(&table->mutex);
            free(table);
            return NULL;
        }
    }
    return table;
}
OresFlowError ores_flow_table_try_destroy(OresFlowTable* table) {
    if (!table) return ORES_FLOW_BAD_ARGUMENT;
    if (callback_table != NULL) return ORES_FLOW_REENTRANT;
    /* A nonblocking inspection of all flow locks under the directory lock
     * makes a blocked publication observable as BUSY. The trusted scheduler
     * must still fence/join new callers before invoking this function. */
    pthread_mutex_lock(&table->mutex);
    size_t acquired = 0;
    for (; acquired < ORES_FLOW_MAX_ACTIVE; ++acquired) {
        if (pthread_mutex_trylock(&table->flow_locks[acquired]) != 0) {
            while (acquired > 0)
                pthread_mutex_unlock(&table->flow_locks[--acquired]);
            pthread_mutex_unlock(&table->mutex);
            return ORES_FLOW_BUSY;
        }
    }
    int incomplete = 0;
    for (size_t i = 0; i < ORES_FLOW_MAX_ACTIVE; ++i)
        if (table->flows[i].id != 0) incomplete = 1;
    for (size_t i = ORES_FLOW_MAX_ACTIVE; i > 0; --i)
        pthread_mutex_unlock(&table->flow_locks[i - 1]);
    pthread_mutex_unlock(&table->mutex);
    if (incomplete) return ORES_FLOW_INCOMPLETE;
    for (size_t i = 0; i < ORES_FLOW_MAX_ACTIVE; ++i)
        if (pthread_mutex_destroy(&table->flow_locks[i]) != 0) abort();
    if (pthread_mutex_destroy(&table->mutex) != 0) abort();
    free(table);
    return ORES_FLOW_OK;
}
void ores_flow_table_destroy(OresFlowTable* table) {
    if (!table) return;
    /* Fail closed rather than silently drop an admitted or reserved flow. */
    if (ores_flow_table_try_destroy(table) != ORES_FLOW_OK) abort();
}
OresFlowError ores_flow_open(OresFlowTable* table, uint64_t pipeline_id,
    uint64_t route_generation, const OresFlowActor* stages,
    uint8_t stage_count, uint64_t* flow_id) {
    if (!table || !pipeline_id || !route_generation || !stages || !flow_id ||
        stage_count < 2 || stage_count > ORES_FLOW_MAX_STAGES)
        return ORES_FLOW_BAD_ARGUMENT;
    if (callback_table != NULL) return ORES_FLOW_REENTRANT;
    for (uint8_t i = 0; i < stage_count; ++i) {
        if (!valid_actor(stages[i])) return ORES_FLOW_BAD_ARGUMENT;
        /* No cross-domain actor graph adapter is implemented for this
         * restricted linear pipeline. Fail closed until there is an
         * explicitly authorized scheduler-bridge route capability. */
        if (stages[i].scheduler_domain != stages[0].scheduler_domain)
            return ORES_FLOW_BAD_ARGUMENT;
        for (uint8_t j = 0; j < i; ++j)
            if (stages[i].scheduler_domain == stages[j].scheduler_domain &&
                stages[i].actor_id == stages[j].actor_id)
                return ORES_FLOW_BAD_ARGUMENT;
    }
    pthread_mutex_lock(&table->mutex);
    Flow* slot = NULL;
    size_t index = 0;
    for (size_t i = 0; i < ORES_FLOW_MAX_ACTIVE; ++i) {
        if (!table->flows[i].id &&
            pthread_mutex_trylock(&table->flow_locks[i]) == 0) {
            slot = &table->flows[i];
            index = i;
            break;
        }
    }
    if (!slot) {
        pthread_mutex_unlock(&table->mutex);
        return ORES_FLOW_CAPACITY;
    }
    const uint64_t identity = allocate_identity(&next_flow_id);
    if (!identity) {
        pthread_mutex_unlock(&table->flow_locks[index]);
        pthread_mutex_unlock(&table->mutex);
        return ORES_FLOW_CAPACITY;
    }
    memset(slot, 0, sizeof(*slot));
    slot->id = identity;
    slot->pipeline_id = pipeline_id;
    slot->route_generation = route_generation;
    slot->stage_count = stage_count;
    memcpy(slot->stages, stages, stage_count * sizeof(stages[0]));
    slot->state = ORES_FLOW_ACTIVE;
    *flow_id = slot->id;
    pthread_mutex_unlock(&table->flow_locks[index]);
    pthread_mutex_unlock(&table->mutex);
    return ORES_FLOW_OK;
}
OresFlowError ores_flow_reserve(OresFlowTable* table, uint64_t flow_id,
    OresFlowActor sender, OresFlowDirection direction, uint64_t causal_parent,
    OresFlowTicket* out) {
    if (!table || !flow_id || !out) return ORES_FLOW_BAD_ARGUMENT;
    if (callback_table != NULL) return ORES_FLOW_REENTRANT;
    Flow* f = acquire_flow(table, flow_id);
    OresFlowError error = ORES_FLOW_OK;
    if (!f) error = ORES_FLOW_NOT_FOUND;
    else if (f->state != ORES_FLOW_ACTIVE) error = ORES_FLOW_TERMINAL;
    else if (f->reserved) error = ORES_FLOW_ALREADY_RESERVED;
    else if (f->phase >= 2u * f->stage_count - 1u)
        error = ORES_FLOW_INCOMPLETE; /* all DATA hops already committed */
    else if (causal_parent != f->last_message)
        error = ORES_FLOW_BAD_CAUSAL_PARENT;
    else {
        uint32_t phase = f->phase;
        uint32_t n = f->stage_count;
        OresFlowActor expected_sender = {0, 0, 0};
        OresFlowDirection expected_direction;
        uint8_t destination;
        if (phase == 0) {
            expected_direction = ORES_FLOW_ROOT;
            destination = 0;
        } else if (phase < n) {
            expected_direction = ORES_FLOW_FORWARD;
            expected_sender = f->stages[phase - 1];
            destination = (uint8_t)phase;
        } else {
            expected_direction = ORES_FLOW_BACKWARD;
            expected_sender = f->stages[2u * (n - 1u) - phase + 1u];
            destination = (uint8_t)(2u * (n - 1u) - phase);
        }
        if (!actor_equal(sender, expected_sender) ||
            direction != expected_direction)
            error = ORES_FLOW_WRONG_SENDER;
        else {
            OresFlowTicket t = {0};
            t.flow_id = f->id;
            t.correlation_id = f->id;
            t.pipeline_id = f->pipeline_id;
            t.route_generation = f->route_generation;
            t.message_id = allocate_identity(&next_message_id);
            if (!t.message_id) {
                error = ORES_FLOW_CAPACITY;
            } else {
            t.parent_message_id = f->last_message;
            t.sender = sender;
            t.destination = f->stages[destination];
            t.stage_index = destination;
            t.direction = (uint8_t)direction;
            f->ticket = t;
            f->reserved = 1;
            *out = t;
            }
        }
    }
    if (f) release_flow(table, f);
    return error;
}
OresFlowError ores_flow_publish(OresFlowTable* table,
    const OresFlowTicket* ticket, OresFlowTryPublish try_publish, void* ctx) {
    if (!table || !ticket || !try_publish) return ORES_FLOW_BAD_ARGUMENT;
    if (callback_table != NULL) return ORES_FLOW_REENTRANT;
    Flow* f = acquire_flow(table, ticket->flow_id);
    OresFlowError error = ORES_FLOW_OK;
    if (!f) error = ORES_FLOW_NOT_FOUND;
    else if (f->state != ORES_FLOW_ACTIVE) error = ORES_FLOW_TERMINAL;
    else if (!f->reserved || !ticket_equal(ticket, &f->ticket))
        error = ORES_FLOW_BAD_TICKET;
    else {
        /* Linearization point: flow cancellation and nonblocking scheduler
         * publication share this flow-specific mutex. try_publish MUST NOT wait. */
        /* Copy the checked reservation: caller-provided ticket storage
         * cannot change mid-callback (nor be reused for another flow). */
        const OresFlowTicket pinned = f->ticket;
        /* While this callback runs, *no* flow table API may be entered
         * synchronously on this thread, including another table. */
        callback_table = table;
        const int admitted = try_publish(ctx, &pinned);
        callback_table = NULL;
        if (admitted == 1) {
            f->last_message = pinned.message_id;
            ++f->phase;
        } else {
            f->state = ORES_FLOW_REJECTED;
            error = ORES_FLOW_REJECTED_MAIL;
        }
        f->reserved = 0;
        memset(&f->ticket, 0, sizeof(f->ticket));
    }
    if (f) release_flow(table, f);
    return error;
}
OresFlowError ores_flow_cancel(OresFlowTable* table, uint64_t flow_id,
    OresFlowActor source) {
    if (!table || !flow_id) return ORES_FLOW_BAD_ARGUMENT;
    if (callback_table != NULL) return ORES_FLOW_REENTRANT;
    Flow* f = acquire_flow(table, flow_id);
    OresFlowError error;
    if (!f) error = ORES_FLOW_NOT_FOUND;
    else if (!actor_equal(source, f->stages[0]))
        error = ORES_FLOW_WRONG_SENDER;
    else if (f->state != ORES_FLOW_ACTIVE) error = ORES_FLOW_TERMINAL;
    else {
        /* Reserved-but-not-published DATA is invalidated, never admitted. */
        f->state = ORES_FLOW_CANCELLED;
        f->reserved = 0;
        memset(&f->ticket, 0, sizeof(f->ticket));
        error = ORES_FLOW_OK;
    }
    if (f) release_flow(table, f);
    return error;
}
OresFlowError ores_flow_finish(OresFlowTable* table, uint64_t flow_id,
    OresFlowActor source, uint64_t causal_parent) {
    if (!table || !flow_id) return ORES_FLOW_BAD_ARGUMENT;
    if (callback_table != NULL) return ORES_FLOW_REENTRANT;
    Flow* f = acquire_flow(table, flow_id);
    OresFlowError error;
    if (!f) error = ORES_FLOW_NOT_FOUND;
    else if (!actor_equal(source, f->stages[0])) error = ORES_FLOW_WRONG_SENDER;
    else if (f->state != ORES_FLOW_ACTIVE) error = ORES_FLOW_TERMINAL;
    else if (f->reserved || f->phase != 2u * f->stage_count - 1u)
        error = ORES_FLOW_INCOMPLETE;
    else if (causal_parent != f->last_message)
        error = ORES_FLOW_BAD_CAUSAL_PARENT;
    else {
        /* Caller must verify the source DATA turn/finalizer completed. */
        f->state = ORES_FLOW_COMPLETED;
        error = ORES_FLOW_OK;
    }
    if (f) release_flow(table, f);
    return error;
}
OresFlowError ores_flow_snapshot(OresFlowTable* table, uint64_t flow_id,
    OresFlowSnapshot* out) {
    if (!table || !out || !flow_id) return ORES_FLOW_BAD_ARGUMENT;
    if (callback_table != NULL) return ORES_FLOW_REENTRANT;
    Flow* f = acquire_flow(table, flow_id);
    if (!f) return ORES_FLOW_NOT_FOUND;
    out->state = f->state;
    out->last_message_id = f->last_message;
    out->correlation_id = f->id;
    out->committed_hops = f->phase;
    out->expected_hops = 2u * f->stage_count - 1u;
    out->reserved = (uint8_t)f->reserved;
    release_flow(table, f);
    return ORES_FLOW_OK;
}
OresFlowError ores_flow_retire(OresFlowTable* table, uint64_t flow_id) {
    if (!table || !flow_id) return ORES_FLOW_BAD_ARGUMENT;
    if (callback_table != NULL) return ORES_FLOW_REENTRANT;
    /* Never wait under the directory mutex for a slow in-flight callback.
     * A busy flow is retried by trusted teardown AFTER fencing producers. */
    pthread_mutex_lock(&table->mutex);
    Flow* f = find_flow(table, flow_id);
    if (!f) {
        pthread_mutex_unlock(&table->mutex);
        return ORES_FLOW_NOT_FOUND;
    }
    const size_t index = (size_t)(f - table->flows);
    if (pthread_mutex_trylock(&table->flow_locks[index]) != 0) {
        pthread_mutex_unlock(&table->mutex);
        return ORES_FLOW_BUSY;
    }
    OresFlowError error;
    if (f->id != flow_id) error = ORES_FLOW_NOT_FOUND;
    else if (f->state == ORES_FLOW_ACTIVE || f->reserved)
        error = ORES_FLOW_INCOMPLETE;
    else {
        memset(f, 0, sizeof(*f));
        error = ORES_FLOW_OK;
    }
    pthread_mutex_unlock(&table->flow_locks[index]);
    pthread_mutex_unlock(&table->mutex);
    return error;
}
