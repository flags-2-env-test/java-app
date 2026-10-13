#include "oreslang/runtime/NativeActorScheduler.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#include <stdatomic.h>

typedef struct {
    uint64_t* messages; /* fixed ring, assigned at scheduler startup */
    size_t head, tail, queued;
    unsigned active;
    unsigned has_control;
    uint64_t control_payload;
    OresNativeActorTerminalFailure failure;
    unsigned control_is_failure;
    unsigned spawned, retired;
    uint64_t generation;
} ActorLane;
struct OresNativeActorScheduler {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    pthread_cond_t drained;
    ActorLane* actors;
    uint64_t* slots;
    pthread_t* threads;
    size_t actor_count, per_actor_capacity, worker_count;
    size_t next_actor, outstanding;
    uint64_t domain;
    uint64_t accepted, rejected, completed, control_completed;
    int retired;
    OresNativeActorTaskFn callback;
    OresNativeActorTaskFn control_callback;
    OresNativeActorPanicFn panic_callback;
    void* context;
};
static _Thread_local OresNativeActorScheduler* executing;
static _Atomic(uint64_t) next_scheduler_domain = 1;

static void lock(OresNativeActorScheduler* s) {
    if (pthread_mutex_lock(&s->lock) != 0) abort();
}
static void unlock(OresNativeActorScheduler* s) {
    if (pthread_mutex_unlock(&s->lock) != 0) abort();
}
static int find_ready(OresNativeActorScheduler* s, size_t* actor, int* control) {
    /* CONTROL must be admitted ahead of normal mail, across all actors.
     * The parent still owns its exclusive logical lane. */
    for (int pass = 1; pass >= 0; --pass) {
        for (size_t offset = 0; offset < s->actor_count; ++offset) {
            size_t i = (s->next_actor + offset) % s->actor_count;
            if (!s->actors[i].active
                && (pass ? s->actors[i].has_control : s->actors[i].queued != 0)) {
                *actor = i;
                *control = pass;
                s->next_actor = (i + 1) % s->actor_count;
                return 1;
            }
        }
    }
    return 0;
}
static void* carrier(void* raw) {
    OresNativeActorScheduler* s = (OresNativeActorScheduler*)raw;
    for (;;) {
        lock(s);
        size_t i = 0;
        int control = 0;
        while (!find_ready(s, &i, &control)) {
            if (s->retired && !s->outstanding) {
                unlock(s);
                return NULL;
            }
            if (pthread_cond_wait(&s->ready, &s->lock) != 0) abort();
        }
        ActorLane* lane = &s->actors[i];
        uint64_t payload;
        OresNativeActorTerminalFailure failure = {0};
        int typed_failure = control && lane->control_is_failure;
        if (control) {
            payload = lane->control_payload;
            if (typed_failure) failure = lane->failure;
            lane->control_is_failure = 0;
            lane->has_control = 0;
        } else {
            payload = lane->messages[lane->head];
            lane->head = (lane->head + 1) % s->per_actor_capacity;
            lane->queued--;
        }
        lane->active = 1;
        unlock(s);

        executing = s;
        if (typed_failure) s->panic_callback(s->context, i, failure);
        else if (control) s->control_callback(s->context, i, payload);
        else s->callback(s->context, i, payload);
        executing = NULL;

        lock(s);
        if (!lane->active || !s->outstanding || s->completed == UINT64_MAX) abort();
        lane->active = 0;
        s->outstanding--;
        s->completed++;
        if (control) ++s->control_completed;
        if (lane->queued || lane->has_control) pthread_cond_signal(&s->ready);
        if (lane->retired && !lane->active && !lane->queued && !lane->has_control)
            pthread_cond_broadcast(&s->drained);
        if (!s->outstanding) {
            pthread_cond_broadcast(&s->drained);
            if (s->retired) pthread_cond_broadcast(&s->ready);
        }
        unlock(s);
    }
}
OresNativeActorScheduler* ores_native_actor_scheduler_open_typed_control(
    size_t actors, size_t capacity, size_t workers,
    OresNativeActorTaskFn callback, OresNativeActorTaskFn control_callback,
    OresNativeActorPanicFn panic_callback, void* ctx) {
    if (!callback || !actors || actors > 1024 || !capacity || capacity > 65536
        || !workers || workers > 128 || actors > SIZE_MAX / capacity)
        return NULL;
    OresNativeActorScheduler* s = (OresNativeActorScheduler*)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->actors = (ActorLane*)calloc(actors, sizeof(ActorLane));
    s->slots = (uint64_t*)calloc(actors * capacity, sizeof(uint64_t));
    s->threads = (pthread_t*)calloc(workers, sizeof(pthread_t));
    if (!s->actors || !s->slots || !s->threads) goto allocation_failed;
    s->domain = atomic_fetch_add_explicit(&next_scheduler_domain, 1, memory_order_relaxed);
    if (!s->domain || s->domain == UINT64_MAX) goto allocation_failed;
    s->actor_count = actors;
    s->per_actor_capacity = capacity;
    s->worker_count = workers;
    s->callback = callback;
    s->control_callback = control_callback;
    s->panic_callback = panic_callback;
    s->context = ctx;
    for (size_t i = 0; i < actors; ++i)
        s->actors[i].messages = s->slots + i * capacity;
    if (pthread_mutex_init(&s->lock, NULL) != 0) goto allocation_failed;
    if (pthread_cond_init(&s->ready, NULL) != 0) {
        pthread_mutex_destroy(&s->lock);
        goto allocation_failed;
    }
    if (pthread_cond_init(&s->drained, NULL) != 0) {
        pthread_cond_destroy(&s->ready);
        pthread_mutex_destroy(&s->lock);
        goto allocation_failed;
    }
    size_t started = 0;
    while (started < workers && pthread_create(&s->threads[started], NULL, carrier, s) == 0)
        ++started;
    if (started != workers) {
        ores_native_actor_scheduler_retire(s);
        for (size_t i = 0; i < started; ++i) pthread_join(s->threads[i], NULL);
        pthread_cond_destroy(&s->drained);
        pthread_cond_destroy(&s->ready);
        pthread_mutex_destroy(&s->lock);
        goto allocation_failed;
    }
    return s;
allocation_failed:
    free(s->threads);
    free(s->slots);
    free(s->actors);
    free(s);
    return NULL;
}
OresNativeActorScheduler* ores_native_actor_scheduler_open_control(
    size_t actors, size_t capacity, size_t workers,
    OresNativeActorTaskFn callback, OresNativeActorTaskFn control_callback,
    void* ctx) {
    return ores_native_actor_scheduler_open_typed_control(
        actors, capacity, workers, callback, control_callback, NULL, ctx);
}
OresNativeActorScheduler* ores_native_actor_scheduler_open(
    size_t actors, size_t capacity, size_t workers,
    OresNativeActorTaskFn callback, void* ctx) {
    return ores_native_actor_scheduler_open_control(
        actors, capacity, workers, callback, NULL, ctx);
}
static int matching(OresNativeActorScheduler* s, OresNativeActorRef ref) {
    return s && ref.scheduler_domain == s->domain
        && ref.actor_slot < s->actor_count
        && ref.generation != 0
        && s->actors[ref.actor_slot].spawned
        && s->actors[ref.actor_slot].generation == ref.generation;
}
static int enqueue_locked(OresNativeActorScheduler* s, size_t i, uint64_t payload) {
    if (s->retired || i >= s->actor_count
        || !s->actors[i].spawned || s->actors[i].retired
        || s->actors[i].queued + s->actors[i].active >= s->per_actor_capacity
        || s->accepted == UINT64_MAX) {
        if (s->rejected != UINT64_MAX) s->rejected++;
        return 0;
    }
    ActorLane* a = &s->actors[i];
    a->messages[a->tail] = payload;
    a->tail = (a->tail + 1) % s->per_actor_capacity;
    a->queued++;
    s->accepted++;
    s->outstanding++;
    pthread_cond_signal(&s->ready);
    return 1;
}
OresNativeActorRef ores_native_actor_scheduler_spawn(
    OresNativeActorScheduler* s, size_t i) {
    OresNativeActorRef missing = {0, 0, 0};
    if (!s) return missing;
    lock(s);
    if (s->retired || i >= s->actor_count || s->actors[i].spawned) {
        unlock(s);
        return missing;
    }
    ActorLane* a = &s->actors[i];
    a->spawned = 1;
    a->generation = 1;
    OresNativeActorRef ref = {s->domain, (uint64_t)i, a->generation};
    unlock(s);
    return ref;
}
int ores_native_actor_scheduler_send(OresNativeActorScheduler* s,
                                     OresNativeActorRef actor, uint64_t payload) {
    if (!s) return 0;
    lock(s);
    int ok = matching(s, actor)
        ? enqueue_locked(s, (size_t)actor.actor_slot, payload) : 0;
    if (!matching(s, actor) && s->rejected != UINT64_MAX) s->rejected++;
    unlock(s);
    return ok;
}
int ores_native_actor_scheduler_submit(OresNativeActorScheduler* s,
                                       size_t i, uint64_t message) {
    if (!s) return 0;
    lock(s);
    int ok = enqueue_locked(s, i, message);
    unlock(s);
    return ok;
}
int ores_native_actor_scheduler_publish_control(
    OresNativeActorScheduler* s, OresNativeActorRef parent, uint64_t payload) {
    if (!s) return 0;
    lock(s);
    int ok = !s->retired && s->control_callback && matching(s, parent)
        && !s->actors[parent.actor_slot].retired
        && !s->actors[parent.actor_slot].has_control
        && s->accepted != UINT64_MAX;
    if (ok) {
        ActorLane* lane = &s->actors[parent.actor_slot];
        lane->control_payload = payload;
        lane->control_is_failure = 0;
        lane->has_control = 1;
        ++s->accepted;
        ++s->outstanding;
        pthread_cond_signal(&s->ready);
    } else if (s->rejected != UINT64_MAX) ++s->rejected;
    unlock(s);
    return ok;
}
int ores_native_actor_scheduler_publish_failure(
    OresNativeActorScheduler* s, OresNativeActorRef parent,
    OresNativeActorTerminalFailure failure) {
    if (!s) return 0;
    lock(s);
    int ok = s->panic_callback != NULL && !s->retired
        && matching(s, parent) && matching(s, failure.child)
        && parent.actor_slot != failure.child.actor_slot
        && failure.kind >= ORES_NATIVE_CONTROL_PANIC
        && failure.kind <= ORES_NATIVE_CONTROL_STOPPED
        && !s->actors[parent.actor_slot].retired
        && !s->actors[parent.actor_slot].has_control
        && s->accepted != UINT64_MAX;
    if (ok) {
        ActorLane* a = &s->actors[parent.actor_slot];
        a->failure = failure;
        a->control_is_failure = 1;
        a->has_control = 1;
        s->accepted++;
        s->outstanding++;
        pthread_cond_signal(&s->ready);
    } else if (s->rejected != UINT64_MAX) s->rejected++;
    unlock(s);
    return ok;
}
uint64_t ores_native_actor_scheduler_control_completed(OresNativeActorScheduler* s) {
    if (!s) return 0;
    lock(s);
    uint64_t n = s->control_completed;
    unlock(s);
    return n;
}
int ores_native_actor_scheduler_stop(OresNativeActorScheduler* s,
                                     OresNativeActorRef actor) {
    if (!s) return 0;
    lock(s);
    if (!matching(s, actor) || s->actors[actor.actor_slot].retired) {
        unlock(s); return 0;
    }
    s->actors[actor.actor_slot].retired = 1;
    unlock(s);
    return 1;
}
int ores_native_actor_scheduler_poll_ready(OresNativeActorScheduler* s,
                                           OresNativeActorRef actor) {
    if (!s) return 0;
    lock(s);
    int ready = matching(s, actor) && !s->retired && !s->actors[actor.actor_slot].retired;
    unlock(s);
    return ready;
}
int ores_native_actor_scheduler_poll_done(OresNativeActorScheduler* s,
                                          OresNativeActorRef actor) {
    if (!s) return 0;
    lock(s);
    int done = matching(s, actor) && s->actors[actor.actor_slot].retired
        && !s->actors[actor.actor_slot].queued && !s->actors[actor.actor_slot].has_control
        && !s->actors[actor.actor_slot].active;
    unlock(s);
    return done;
}
int ores_native_actor_scheduler_wait_done(OresNativeActorScheduler* s,
                                          OresNativeActorRef actor) {
    if (!s || executing == s) return 0;
    lock(s);
    if (!matching(s, actor) || !s->actors[actor.actor_slot].retired) {
        unlock(s);
        return 0;
    }
    ActorLane* lane = &s->actors[actor.actor_slot];
    while (lane->queued || lane->has_control || lane->active) {
        if (pthread_cond_wait(&s->drained, &s->lock) != 0) abort();
    }
    unlock(s);
    return 1;
}
void ores_native_actor_scheduler_retire(OresNativeActorScheduler* s) {
    if (!s) abort();
    lock(s);
    s->retired = 1;
    for (size_t i = 0; i < s->actor_count; ++i) s->actors[i].retired = 1;
    pthread_cond_broadcast(&s->ready);
    unlock(s);
}
int ores_native_actor_scheduler_drain(OresNativeActorScheduler* s) {
    if (!s || executing == s) return 0;
    lock(s);
    while (s->outstanding)
        if (pthread_cond_wait(&s->drained, &s->lock) != 0) abort();
    unlock(s);
    return 1;
}
int ores_native_actor_scheduler_try_destroy(OresNativeActorScheduler* s) {
    if (!s || executing == s) return 0;
    lock(s);
    int ready = s->retired && !s->outstanding;
    if (ready) pthread_cond_broadcast(&s->ready);
    unlock(s);
    if (!ready) return 0;
    /* Caller must externally fence producers; no concurrent API calls now. */
    for (size_t i = 0; i < s->worker_count; ++i)
        if (pthread_join(s->threads[i], NULL) != 0) abort();
    if (pthread_cond_destroy(&s->drained) != 0
        || pthread_cond_destroy(&s->ready) != 0
        || pthread_mutex_destroy(&s->lock) != 0) abort();
    free(s->threads);
    free(s->slots);
    free(s->actors);
    free(s);
    return 1;
}
static uint64_t counter(OresNativeActorScheduler* s, int which) {
    if (!s) abort();
    lock(s);
    uint64_t n = which == 0 ? s->accepted : which == 1 ? s->rejected : s->completed;
    unlock(s);
    return n;
}
uint64_t ores_native_actor_scheduler_accepted(OresNativeActorScheduler* s) {
    return counter(s, 0);
}
uint64_t ores_native_actor_scheduler_rejected(OresNativeActorScheduler* s) {
    return counter(s, 1);
}
uint64_t ores_native_actor_scheduler_completed(OresNativeActorScheduler* s) {
    return counter(s, 2);
}
