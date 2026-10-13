#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/NativeActorFuture.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static void check(int okay) { if (!okay) abort(); }
typedef struct {
    OresNativeActorFuture* ready;
    OresNativeActorFuture* done;
    atomic_int parent_busy, parent_started, release_parent, child_active;
    atomic_int child_completed, control_count, violations;
    uint64_t control_token[2];
} State;
static void nap(void) {
    struct timespec t = {0, 1000000};
    nanosleep(&t, NULL);
}
static void await_at_least(atomic_int* field, int value) {
    for (int i = 0; i < 2000; ++i) {
        if (atomic_load(field) >= value) return;
        nap();
    }
    check(0);
}
static void normal(void* raw, size_t actor, uint64_t payload) {
    State* s = (State*)raw;
    if (actor == 0) {
        if (atomic_fetch_add(&s->parent_busy, 1)) atomic_fetch_add(&s->violations, 1);
        check(payload == 101);
        atomic_store(&s->parent_started, 1);
        while (!atomic_load(&s->release_parent)) nap();
        atomic_fetch_sub(&s->parent_busy, 1);
    } else {
        check(actor == 1 && payload == 7);
        atomic_store(&s->child_active, 1);
        check(ores_native_actor_future_complete(s->ready, ORES_NATIVE_FUTURE_READY));
        atomic_store(&s->child_active, 0);
        atomic_store(&s->child_completed, 1);
    }
}
static void control(void* raw, size_t actor, uint64_t payload) {
    State* s = (State*)raw;
    if (atomic_fetch_add(&s->parent_busy, 1)) atomic_fetch_add(&s->violations, 1);
    if (actor != 0 || atomic_load(&s->child_active))
        atomic_fetch_add(&s->violations, 1);
    int index = atomic_fetch_add(&s->control_count, 1);
    if (index >= 2 || payload != s->control_token[index])
        atomic_fetch_add(&s->violations, 1);
    if (index == 0 && ores_native_actor_future_status(s->ready) != ORES_NATIVE_FUTURE_READY)
        atomic_fetch_add(&s->violations, 1);
    if (index == 1 && ores_native_actor_future_status(s->done) != ORES_NATIVE_FUTURE_FAILED)
        atomic_fetch_add(&s->violations, 1);
    atomic_fetch_sub(&s->parent_busy, 1);
}
int main(void) {
    State s = {0};
    s.control_token[0] = 10001;
    s.control_token[1] = 20002;
    OresNativeActorScheduler* scheduler =
        ores_native_actor_scheduler_open_control(2, 1, 3, normal, control, &s);
    check(scheduler != NULL);
    OresNativeActorRef parent = ores_native_actor_scheduler_spawn(scheduler, 0);
    OresNativeActorRef child = ores_native_actor_scheduler_spawn(scheduler, 1);
    check(parent.scheduler_domain && child.scheduler_domain);
    s.ready = ores_native_actor_future_open(scheduler, parent, s.control_token[0]);
    s.done = ores_native_actor_future_open(scheduler, parent, s.control_token[1]);
    check(s.ready && s.done);
    check(ores_native_actor_scheduler_send(scheduler, parent, 101));
    await_at_least(&s.parent_started, 1);
    /* Normal capacity one is fully occupied by parent user code. */
    check(!ores_native_actor_scheduler_send(scheduler, parent, 102));
    check(ores_native_actor_scheduler_send(scheduler, child, 7));
    await_at_least(&s.child_completed, 1);
    /* CONTROL slot must remain available even while normal lane is full.
     * A second future cannot claim the occupied reserved control slot. */
    check(ores_native_actor_future_status(s.ready) == ORES_NATIVE_FUTURE_READY);
    check(!ores_native_actor_future_complete(s.done, ORES_NATIVE_FUTURE_FAILED));
    check(ores_native_actor_future_status(s.done) == ORES_NATIVE_FUTURE_PENDING);
    atomic_store(&s.release_parent, 1);
    await_at_least(&s.control_count, 1);
    check(ores_native_actor_future_complete(s.done, ORES_NATIVE_FUTURE_FAILED));
    check(!ores_native_actor_future_complete(s.done, ORES_NATIVE_FUTURE_FAILED));
    check(ores_native_actor_scheduler_drain(scheduler));
    check(atomic_load(&s.control_count) == 2);
    check(atomic_load(&s.violations) == 0);
    check(ores_native_actor_scheduler_control_completed(scheduler) == 2);
    check(ores_native_actor_future_status(s.done) == ORES_NATIVE_FUTURE_FAILED);
    check(ores_native_actor_scheduler_stop(scheduler, child));
    check(ores_native_actor_scheduler_stop(scheduler, parent));
    check(ores_native_actor_scheduler_poll_done(scheduler, parent));
    check(ores_native_actor_scheduler_poll_done(scheduler, child));
    ores_native_actor_scheduler_retire(scheduler);
    check(ores_native_actor_scheduler_try_destroy(scheduler));
    ores_native_actor_future_destroy(s.ready);
    ores_native_actor_future_destroy(s.done);
    puts("NATIVE_SUPERVISOR_CONTROL_PASS reserved=1 terminal_futures=2 lane_exclusive=1");
    return 0;
}
