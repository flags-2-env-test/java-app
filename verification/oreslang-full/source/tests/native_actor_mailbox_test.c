#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorMailbox.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define THREADS 12
static void check(int okay) { if (!okay) abort(); }
static void nap(void) {
    struct timespec one_ms={0,1000000};
    nanosleep(&one_ms,NULL);
}
static void wait_for_pending(OresNativeActorMailbox* lane, size_t expected) {
    for (int i=0;i<2000;i++) {
        if (ores_native_actor_mailbox_pending(lane)==expected) return;
        nap();
    }
    check(0);
}
typedef struct {
    OresNativeActorMailbox* lane;
    atomic_int* active;
    atomic_int* max_active;
    atomic_int* finished;
    atomic_int* order_index;
    int* order;
    int id;
} ThreadArgs;
static void* worker(void* arg) {
    ThreadArgs* a=(ThreadArgs*)arg;
    if (!ores_native_actor_mailbox_enter(a->lane)) return NULL;
    int simultaneous=atomic_fetch_add(a->active,1)+1;
    int old=atomic_load(a->max_active);
    while (simultaneous>old
           && !atomic_compare_exchange_weak(a->max_active,&old,simultaneous)) { }
    if (a->order) {
        int index=atomic_fetch_add(a->order_index,1);
        check(index>=0 && index<THREADS);
        a->order[index]=a->id;
    }
    nap();
    atomic_fetch_sub(a->active,1);
    atomic_fetch_add(a->finished,1);
    ores_native_actor_mailbox_leave(a->lane);
    return NULL;
}
int main(void) {
    OresNativeActorMailbox* lane=ores_native_actor_mailbox_open(THREADS+1);
    check(lane != NULL);
    atomic_int active=0, max_active=0, finished=0, order_index=0;
    int order[THREADS]={0};
    ThreadArgs args[THREADS];
    pthread_t threads[THREADS];
    /* Force exact queue order while lane owner is already admitted. */
    check(ores_native_actor_mailbox_enter(lane)==1);
    for(int i=0;i<THREADS;i++) {
        args[i]=(ThreadArgs){lane,&active,&max_active,&finished,&order_index,order,i};
        check(pthread_create(&threads[i],NULL,worker,&args[i])==0);
        wait_for_pending(lane,(size_t)i+2);
    }
    check(ores_native_actor_mailbox_pending(lane)==THREADS+1);
    ores_native_actor_mailbox_leave(lane);
    for(int i=0;i<THREADS;i++) check(pthread_join(threads[i],NULL)==0);
    check(atomic_load(&finished)==THREADS);
    check(atomic_load(&max_active)==1);
    check(atomic_load(&order_index)==THREADS);
    for(int i=0;i<THREADS;i++) check(order[i]==i);
    check(ores_native_actor_mailbox_pending(lane)==0);
    check(ores_native_actor_mailbox_rejected(lane)==0);
    ores_native_actor_mailbox_retire(lane);
    check(ores_native_actor_mailbox_enter(lane)==0);
    check(ores_native_actor_mailbox_try_destroy(lane)==1);

    OresNativeActorMailbox* bounded=ores_native_actor_mailbox_open(1);
    OresNativeActorMailbox* nested=ores_native_actor_mailbox_open(1);
    check(bounded != NULL && nested != NULL);
    check(ores_native_actor_mailbox_enter(bounded)==1);
    check(ores_native_actor_mailbox_enter(bounded)==0);
    check(ores_native_actor_mailbox_enter(nested)==0); /* no reentrancy */
    ThreadArgs denial={bounded,&active,&max_active,&finished,&order_index,NULL,0};
    pthread_t denied;
    check(pthread_create(&denied,NULL,worker,&denial)==0);
    check(pthread_join(denied,NULL)==0);
    check(ores_native_actor_mailbox_rejected(bounded)==1);
    check(ores_native_actor_mailbox_pending(bounded)==1);
    ores_native_actor_mailbox_retire(bounded);
    check(ores_native_actor_mailbox_try_destroy(bounded)==0);
    ores_native_actor_mailbox_leave(bounded);
    check(ores_native_actor_mailbox_try_destroy(bounded)==1);
    ores_native_actor_mailbox_retire(nested);
    check(ores_native_actor_mailbox_try_destroy(nested)==1);

    OresNativeActorMailbox* retiring=ores_native_actor_mailbox_open(2);
    check(retiring != NULL);
    check(ores_native_actor_mailbox_enter(retiring)==1);
    ThreadArgs waiting={retiring,&active,&max_active,&finished,&order_index,NULL,0};
    pthread_t blocked;
    check(pthread_create(&blocked,NULL,worker,&waiting)==0);
    wait_for_pending(retiring,2);
    ores_native_actor_mailbox_retire(retiring);
    check(ores_native_actor_mailbox_try_destroy(retiring)==0);
    ores_native_actor_mailbox_leave(retiring);
    check(pthread_join(blocked,NULL)==0);
    check(atomic_load(&finished)==THREADS);
    check(ores_native_actor_mailbox_pending(retiring)==0);
    check(ores_native_actor_mailbox_try_destroy(retiring)==1);
    puts("NATIVE_ACTOR_FIFO_MAILBOX_PASS fifo=12 max_active=1 capacity=1 retire_wake=1");
    return 0;
}
