#pragma once
#include "oreslang/runtime/NativeActorScheduler.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct OresNativeActorFuture OresNativeActorFuture;
enum {
    ORES_NATIVE_FUTURE_PENDING = 0,
    ORES_NATIVE_FUTURE_READY = 1,
    ORES_NATIVE_FUTURE_FAILED = 2,
    ORES_NATIVE_FUTURE_CANCELLED = 3
};
/* One-shot native future. Completion enqueues a CONTROL wake for the owner
 * logical actor lane; never invokes user handlers from the completer thread.
 * A full reserved CONTROL slot leaves the future pending so the producer
 * can retry or escalate, without silently losing an event.
 */
OresNativeActorFuture* ores_native_actor_future_open(
    OresNativeActorScheduler* scheduler, OresNativeActorRef owner,
    uint64_t control_token);
int ores_native_actor_future_complete(OresNativeActorFuture* future, int status);
int ores_native_actor_future_status(OresNativeActorFuture* future);
void ores_native_actor_future_destroy(OresNativeActorFuture* future);
#ifdef __cplusplus
}
#endif
