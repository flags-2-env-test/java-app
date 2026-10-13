#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Native actor heap experimental ABI. Actor IDs and enter tokens MUST remain
 * private to the scheduler, not exposed as forgeable guest-language values.
 * One actor turn runs at a time. This does not require stable carrier threads
 * and it does not use a tracing garbage collector. */
typedef struct OresActorI64Heap OresActorI64Heap;
typedef struct { uint64_t domain, epoch; } OresActorTurn;
typedef struct { uint64_t domain, index, generation; } OresActorSlot;
typedef struct { uint64_t domain, nonce, index, generation; } OresActorLoan;

/* Allocated once per actor incarnation, survives any number of turns. */
OresActorI64Heap* ores_actor_i64_heap_open(uint64_t actor_id,
                                           uint64_t incarnation,
                                           uint64_t slot_capacity);
OresActorTurn ores_actor_i64_heap_enter(OresActorI64Heap* heap,
                                         uint64_t actor_id,
                                         uint64_t incarnation);
void ores_actor_i64_heap_leave(OresActorI64Heap* heap, OresActorTurn turn);
OresActorSlot ores_actor_i64_heap_new(OresActorI64Heap* heap,
                                       OresActorTurn turn, int64_t value);
int64_t ores_actor_i64_heap_read(OresActorI64Heap* heap,
                                  OresActorTurn turn, OresActorSlot slot);
/* Generic writes cannot target compiler-initialized fixed actor fields.
 * Fixed-field writes must go through the checked field_write entrypoint. */
void ores_actor_i64_heap_write(OresActorI64Heap* heap,
                                 OresActorTurn turn,
                                 OresActorSlot slot, int64_t value);
void ores_actor_i64_heap_drop(OresActorI64Heap* heap,
                                OresActorTurn turn, OresActorSlot slot);
/* Loans survive turns/await. They forbid drop and physical heap release.
 * A loan cannot itself dereference memory outside an admitted turn. */
OresActorLoan ores_actor_i64_heap_borrow(OresActorI64Heap* heap,
                                          OresActorTurn turn, OresActorSlot slot);
void ores_actor_i64_heap_release(OresActorI64Heap* heap, OresActorLoan loan);
/* Compiler-emitted, serial actor field ABI. Only admitted turns may access
 * fixed-layout owned fields. Field initialization is single-use. */
void ores_actor_i64_heap_field_init(OresActorI64Heap* heap,
                                    uint64_t domain, uint64_t epoch,
                                    uint64_t index, int64_t initial);
int64_t ores_actor_i64_heap_field_read(OresActorI64Heap* heap,
                                       uint64_t domain, uint64_t epoch,
                                       uint64_t index);
/* Only compiler-declared mutable fields are writable.
 * Outstanding loans prohibit writes, including after a suspended turn. */
void ores_actor_i64_heap_field_init_mutable(OresActorI64Heap* heap,
                                            uint64_t domain, uint64_t epoch,
                                            uint64_t index, int64_t initial);
void ores_actor_i64_heap_field_write(OresActorI64Heap* heap,
                                     uint64_t domain, uint64_t epoch,
                                     uint64_t index, int64_t value);
void ores_actor_i64_heap_retire(OresActorI64Heap* heap);
/* Returns zero while a turn or loan is outstanding; returns one and frees
 * native memory after quiescence. Never call with an invalidated heap pointer.
 * The supervisor is responsible for fencing stale wakeups/callback holders. */
int ores_actor_i64_heap_try_destroy(OresActorI64Heap* heap);
#ifdef __cplusplus
}
#endif
