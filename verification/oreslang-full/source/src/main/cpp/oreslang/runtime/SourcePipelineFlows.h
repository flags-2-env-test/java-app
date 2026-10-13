#pragma once
/*
 * Experimental host-owned, correlated linear-flow admission state.
 * Not yet connected to guest SourceActorBootstrap. This does NOT enable
 * arbitrary multiple in-flight flows in compiled .ores pipelines.
 *
 * Only compiler/scheduler-trusted callers may use this interface. Actor
 * identities are liveness fences, not unforgeable FFI authorization tokens.
 * The caller must join all producer threads before destroying the table.
 */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define ORES_FLOW_MAX_STAGES 16u
#define ORES_FLOW_MAX_ACTIVE 128u

typedef struct OresFlowTable OresFlowTable;
typedef struct {
    uint64_t actor_id;         /* scheduler-issued, never recycled in domain */
    uint64_t actor_generation; /* actual destination incarnation */
    uint64_t scheduler_domain; /* owning scheduler isolation domain */
} OresFlowActor;
typedef enum {
    ORES_FLOW_ROOT = 0,
    ORES_FLOW_FORWARD = 1,
    ORES_FLOW_BACKWARD = 2
} OresFlowDirection;
typedef enum {
    ORES_FLOW_OK = 0,
    ORES_FLOW_BAD_ARGUMENT = 1,
    ORES_FLOW_CAPACITY = 2,
    ORES_FLOW_NOT_FOUND = 3,
    ORES_FLOW_TERMINAL = 4,
    ORES_FLOW_WRONG_SENDER = 5,
    ORES_FLOW_BAD_CAUSAL_PARENT = 6,
    ORES_FLOW_ALREADY_RESERVED = 7,
    ORES_FLOW_BAD_TICKET = 8,
    ORES_FLOW_REJECTED_MAIL = 9,
    ORES_FLOW_INCOMPLETE = 10,
    ORES_FLOW_BUSY = 11,       /* in-flight publisher; retirement not allowed */
    ORES_FLOW_REENTRANT = 12   /* callback attempted table API reentry */
} OresFlowError;
typedef enum {
    ORES_FLOW_ACTIVE,
    ORES_FLOW_COMPLETED,
    ORES_FLOW_CANCELLED,
    ORES_FLOW_REJECTED
} OresFlowTerminal;
typedef struct {
    uint64_t flow_id;
    uint64_t correlation_id;
    uint64_t pipeline_id;
    uint64_t route_generation;
    uint64_t message_id;
    uint64_t parent_message_id;
    OresFlowActor sender;
    OresFlowActor destination;
    uint8_t direction;
    uint8_t stage_index; /* receiving stage */
} OresFlowTicket;
typedef struct {
    OresFlowTerminal state;
    uint64_t last_message_id;
    uint64_t correlation_id;
    uint32_t committed_hops;
    uint32_t expected_hops;
    uint8_t reserved;
} OresFlowSnapshot;

/* A single try_publish must synchronously generation-check and perform
 * nonblocking bounded scheduler DATA admission. It holds ONLY the particular
 * flow's mutex: unrelated flows, opens and cancellations must still progress.
 * It MUST NOT wait, call ANY flow-table API (including another table),
 * or invoke guest code. Cross-table callback reentry is rejected with
 * ORES_FLOW_REENTRANT, not allowed to form a nested A->B->A deadlock.
 * No scheduler lock held by another flow may be acquired in reverse order.
 * The caller owns snapshot/credit cleanup on failure. Return 1 if admitted.
 * Retire of an in-use flow returns ORES_FLOW_BUSY, not a blocking wait.
 * IDs remain unique across flow table recreation within the same process. */
typedef int (*OresFlowTryPublish)(void* ctx, const OresFlowTicket* ticket);

OresFlowTable* ores_flow_table_create(void);
/* Checked terminal teardown. Caller MUST fence/join all external producers
 * before attempting this operation; this function does not manufacture a
 * shutdown barrier or wait for guest continuations. Returns BUSY if any
 * per-flow callback holds a lock, INCOMPLETE if a live/terminal slot has not
 * been explicitly retired, or REENTRANT from inside any publish callback.
 * On OK, frees the table; do not access it again. */
OresFlowError ores_flow_table_try_destroy(OresFlowTable* table);
/* Legacy fail-closed destructor: aborts if checked teardown rejects.
 * All flows MUST be terminalized and explicitly retired first. */
void ores_flow_table_destroy(OresFlowTable* table);
OresFlowError ores_flow_open(OresFlowTable* table, uint64_t pipeline_id,
    uint64_t route_generation, const OresFlowActor* stages,
    uint8_t stage_count, uint64_t* flow_id);
OresFlowError ores_flow_reserve(OresFlowTable* table, uint64_t flow_id,
    OresFlowActor sender, OresFlowDirection direction, uint64_t causal_parent,
    OresFlowTicket* out);
OresFlowError ores_flow_publish(OresFlowTable* table,
    const OresFlowTicket* ticket, OresFlowTryPublish try_publish, void* ctx);
OresFlowError ores_flow_cancel(OresFlowTable* table, uint64_t flow_id,
    OresFlowActor source);
OresFlowError ores_flow_finish(OresFlowTable* table, uint64_t flow_id,
    OresFlowActor source, uint64_t causal_parent);
OresFlowError ores_flow_snapshot(OresFlowTable* table,
    uint64_t flow_id, OresFlowSnapshot* out);
OresFlowError ores_flow_retire(OresFlowTable* table, uint64_t flow_id);
#ifdef __cplusplus
}
#endif
