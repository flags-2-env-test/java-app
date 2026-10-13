#include "oreslang/runtime/NativeActorFuture.h"
#include <pthread.h>
#include <stdlib.h>
struct OresNativeActorFuture {
    pthread_mutex_t lock;
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef owner;
    uint64_t token;
    int state;
};
OresNativeActorFuture* ores_native_actor_future_open(
    OresNativeActorScheduler* scheduler, OresNativeActorRef owner,
    uint64_t control_token) {
    if (!scheduler || !ores_native_actor_scheduler_poll_ready(scheduler, owner))
        return NULL;
    OresNativeActorFuture* future = (OresNativeActorFuture*)calloc(1, sizeof(*future));
    if (!future) return NULL;
    if (pthread_mutex_init(&future->lock, NULL) != 0) {
        free(future); return NULL;
    }
    future->scheduler = scheduler;
    future->owner = owner;
    future->token = control_token;
    return future;
}
int ores_native_actor_future_complete(OresNativeActorFuture* f, int status) {
    if (!f || status < ORES_NATIVE_FUTURE_READY
        || status > ORES_NATIVE_FUTURE_CANCELLED) return 0;
    if (pthread_mutex_lock(&f->lock) != 0) abort();
    int success = 0;
    if (f->state == ORES_NATIVE_FUTURE_PENDING) {
        /* Publish the terminal state before exposing the CONTROL wake.
         * The callback may be admitted immediately on another worker. */
        f->state = status;
        success = ores_native_actor_scheduler_publish_control(
            f->scheduler, f->owner, f->token);
        if (!success) f->state = ORES_NATIVE_FUTURE_PENDING;
    }
    if (pthread_mutex_unlock(&f->lock) != 0) abort();
    return success;
}
int ores_native_actor_future_status(OresNativeActorFuture* f) {
    if (!f) return -1;
    if (pthread_mutex_lock(&f->lock) != 0) abort();
    int status = f->state;
    if (pthread_mutex_unlock(&f->lock) != 0) abort();
    return status;
}
void ores_native_actor_future_destroy(OresNativeActorFuture* f) {
    if (!f) return;
    /* The supervisor owns the fence: no concurrent completers or callbacks
     * may access this future once it is physically retired. */
    if (pthread_mutex_destroy(&f->lock) != 0) abort();
    free(f);
}
