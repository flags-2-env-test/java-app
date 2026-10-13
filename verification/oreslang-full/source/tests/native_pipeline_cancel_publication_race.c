/*
 * Deterministic cancellation-vs-DATA-publication regression.
 *
 * Link a real compiler-generated Oreslang two-actor guest with
 *   -Wl,--wrap=oreslang_source_actor_pipeline_connect_many
 * and the production SourceActorBootstrap.c under
 * ORESLANG_SOURCE_ACTOR_TESTING.  The test-only weak hook blocks trusted
 * main after it validates and copies the root's MessagePack envelope, but
 * before the actual bounded scheduler DATA enqueue. A second OS thread
 * publishes terminal cancellation while main is paused. The resumed root
 * must observe terminal status 7 and MUST NOT submit late DATA. The
 * ORESLANG_CANCEL_RACE_FORWARD_HOP build variant pauses the first actor's
 * downstream emit instead of trusted-main initial ingress.
 */
#include "oreslang/runtime/SourceActorBootstrap.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static pthread_t canceller;
static void* source;
static void* downstream;
static int thread_started;
static int publication_waiting;
static int cancellation_done;

void __real_oreslang_source_actor_pipeline_connect_many(
    void* const* stages, uint64_t count);

static void* terminal_cancellation(void* unused) {
    (void)unused;
    if (pthread_mutex_lock(&gate) != 0) abort();
    while (!publication_waiting) {
        if (pthread_cond_wait(&ready, &gate) != 0) abort();
    }
    if (pthread_mutex_unlock(&gate) != 0) abort();
    oreslang_source_actor_pipeline_cancel(source);
    if (pthread_mutex_lock(&gate) != 0) abort();
    cancellation_done = 1;
    if (pthread_cond_broadcast(&ready) != 0) abort();
    if (pthread_mutex_unlock(&gate) != 0) abort();
    return NULL;
}

void __wrap_oreslang_source_actor_pipeline_connect_many(
    void* const* stages, uint64_t count) {
    if (!stages || count != 2 || thread_started) abort();
    __real_oreslang_source_actor_pipeline_connect_many(stages, count);
    source = stages[0];
    downstream = stages[1];
    if (!source || !downstream || pthread_create(&canceller, NULL,
                                 terminal_cancellation, NULL) != 0) abort();
    thread_started = 1;
}

void oreslang_source_actor_test_before_pipeline_publish(
    void* actor, unsigned direction) {
#ifdef ORESLANG_CANCEL_RACE_FORWARD_HOP
    /* The hook receives the DESTINATION mailbox, not the emitting source. */
    const unsigned target_direction = 1;
    const void* expected_actor = downstream;
#else
    const unsigned target_direction = 0;
    const void* expected_actor = source;
#endif
    if (actor != expected_actor || direction != target_direction) return;
    if (pthread_mutex_lock(&gate) != 0) abort();
    if (publication_waiting) abort();
    publication_waiting = 1;
    if (pthread_cond_broadcast(&ready) != 0) abort();
    while (!cancellation_done) {
        if (pthread_cond_wait(&ready, &gate) != 0) abort();
    }
    if (pthread_mutex_unlock(&gate) != 0) abort();
}

__attribute__((destructor))
static void verify_and_join(void) {
    if (!thread_started) {
        fputs("SOURCE_PIPELINE_RACE_HARNESS_NOT_REACHED\n", stderr);
        abort();
    }
    if (pthread_join(canceller, NULL) != 0
            || !publication_waiting || !cancellation_done) {
        fputs("SOURCE_PIPELINE_RACE_HARNESS_FAILED\n", stderr);
        abort();
    }
    fputs("SOURCE_PIPELINE_CANCEL_PUBLICATION_RACE_PASS\n", stderr);
}
