#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static void require(int truth) { if (!truth) abort(); }
typedef struct { atomic_int busy, released, first_done; } State;
static void nap(void) { struct timespec ts={0,1000000}; nanosleep(&ts,NULL); }
static void actor_turn(void* context, size_t actor, uint64_t message) {
    State* state=(State*)context;
    require(message==42);
    if (actor==0) { atomic_store(&state->first_done,1); return; }
    require(actor==1);
    atomic_store(&state->busy,1);
    while (!atomic_load(&state->released)) nap();
}
int main(void) {
    State state={0};
    OresNativeActorScheduler* scheduler =
        ores_native_actor_scheduler_open(2,2,2,actor_turn,&state);
    require(scheduler!=NULL);
    OresNativeActorRef one=ores_native_actor_scheduler_spawn(scheduler,0);
    OresNativeActorRef two=ores_native_actor_scheduler_spawn(scheduler,1);
    require(one.scheduler_domain && two.scheduler_domain);
    require(ores_native_actor_scheduler_send(scheduler,one,42));
    require(ores_native_actor_scheduler_send(scheduler,two,42));
    for (int n=0;n<2000 && !atomic_load(&state.busy);n++) nap();
    require(atomic_load(&state.busy));
    require(ores_native_actor_scheduler_stop(scheduler,one));
    /* Must complete while actor two is still stuck inside user code.
     * Global drain would deadlock here. */
    require(ores_native_actor_scheduler_wait_done(scheduler,one));
    require(atomic_load(&state.first_done));
    require(!atomic_load(&state.released));
    require(!ores_native_actor_scheduler_poll_done(scheduler,two));
    require(!ores_native_actor_scheduler_wait_done(scheduler,two));
    require(ores_native_actor_scheduler_stop(scheduler,two));
    atomic_store(&state.released,1);
    require(ores_native_actor_scheduler_wait_done(scheduler,two));
    ores_native_actor_scheduler_retire(scheduler);
    require(ores_native_actor_scheduler_try_destroy(scheduler));
    puts("ACTORGROUP_PER_CHILD_WAIT_PASS first_done=1 sibling_still_running=1");
    return 0;
}
