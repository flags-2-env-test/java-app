#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct OresNativeActorMailbox OresNativeActorMailbox;
/* Host/scheduler-owned bounded FIFO for an actor's single logical lane.
 * Not a thread mutex; callers receive one admitted turn at a time.
 * A waiting carrier is not the actor's owner until enter returns OK.
 * Returned 0 means overload/retirement and MUST NOT execute guest code.
 */
OresNativeActorMailbox* ores_native_actor_mailbox_open(size_t capacity);
int ores_native_actor_mailbox_enter(OresNativeActorMailbox* mailbox);
void ores_native_actor_mailbox_leave(OresNativeActorMailbox* mailbox);
void ores_native_actor_mailbox_retire(OresNativeActorMailbox* mailbox);
/* Scheduler must join/fence every caller before trying physical destroy. */
int ores_native_actor_mailbox_try_destroy(OresNativeActorMailbox* mailbox);
uint64_t ores_native_actor_mailbox_rejected(OresNativeActorMailbox* mailbox);
/* Diagnostic count includes the admitted turn and FIFO waiters. */
size_t ores_native_actor_mailbox_pending(OresNativeActorMailbox* mailbox);
#ifdef __cplusplus
}
#endif
