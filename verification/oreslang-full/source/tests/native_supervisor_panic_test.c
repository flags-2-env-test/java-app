#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static void check(int ok) { if (!ok) abort(); }
typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef parent, child;
    atomic_int held, release_parent, finalized, published, delivered, violations;
} State;
static void nap(void) { struct timespec t={0,1000000}; nanosleep(&t,NULL); }
static void wait_for(atomic_int* flag) {
    for(int i=0;i<2000;i++) { if(atomic_load(flag)) return; nap(); }
    check(0);
}
static void normal(void* ctx, size_t actor, uint64_t payload) {
    State* s=(State*)ctx;
    if(actor==0) {
        check(payload==41);
        atomic_store(&s->held,1);
        while(!atomic_load(&s->release_parent)) nap();
    } else {
        check(actor==1 && payload==99);
        /* Simulate completed resource finalizers before notifying parent.
         * Compiler-generated defer/finally remains future work. */
        atomic_store(&s->finalized,1);
        check(ores_native_actor_scheduler_stop(s->scheduler,s->child));
        OresNativeActorTerminalFailure failure={
            ORES_NATIVE_CONTROL_PANIC,s->child,19,702
        };
        check(ores_native_actor_scheduler_publish_failure(s->scheduler,s->parent,failure));
        atomic_store(&s->published,1);
    }
}
static void panic_handler(void* ctx, size_t parent, OresNativeActorTerminalFailure e) {
    State* s=(State*)ctx;
    if(parent!=0 || !atomic_load(&s->finalized) || e.kind!=ORES_NATIVE_CONTROL_PANIC
        || e.child.scheduler_domain!=s->child.scheduler_domain
        || e.child.actor_slot!=s->child.actor_slot || e.child.generation!=s->child.generation
        || e.failure_code!=19 || e.correlation!=702)
        atomic_fetch_add(&s->violations,1);
    atomic_fetch_add(&s->delivered,1);
}
int main(void) {
    State s={0};
    s.scheduler=ores_native_actor_scheduler_open_typed_control(
        2,1,3,normal,NULL,panic_handler,&s);
    check(s.scheduler!=NULL);
    s.parent=ores_native_actor_scheduler_spawn(s.scheduler,0);
    s.child=ores_native_actor_scheduler_spawn(s.scheduler,1);
    check(s.parent.scheduler_domain && s.child.scheduler_domain);
    check(ores_native_actor_scheduler_send(s.scheduler,s.parent,41));
    wait_for(&s.held);
    check(!ores_native_actor_scheduler_send(s.scheduler,s.parent,42));
    check(ores_native_actor_scheduler_send(s.scheduler,s.child,99));
    wait_for(&s.published);
    OresNativeActorTerminalFailure failure={ORES_NATIVE_CONTROL_PANIC,s.child,21,703};
    check(!ores_native_actor_scheduler_publish_failure(s.scheduler,s.parent,failure));
    check(!ores_native_actor_scheduler_send(s.scheduler,s.child,100));
    atomic_store(&s.release_parent,1);
    check(ores_native_actor_scheduler_drain(s.scheduler));
    check(atomic_load(&s.delivered)==1 && atomic_load(&s.violations)==0);
    check(ores_native_actor_scheduler_control_completed(s.scheduler)==1);
    check(ores_native_actor_scheduler_poll_done(s.scheduler,s.child));
    check(ores_native_actor_scheduler_stop(s.scheduler,s.parent));
    ores_native_actor_scheduler_retire(s.scheduler);
    check(ores_native_actor_scheduler_try_destroy(s.scheduler));
    puts("NATIVE_SUPERVISOR_PANIC_PASS typed=1 finalized=1 exclusive_parent=1");
    return 0;
}
