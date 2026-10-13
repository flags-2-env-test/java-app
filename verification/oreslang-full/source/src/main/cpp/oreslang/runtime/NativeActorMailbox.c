#include "oreslang/runtime/NativeActorMailbox.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>

struct OresNativeActorMailbox {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    size_t capacity;
    size_t pending; /* queued callers plus the admitted turn */
    uint64_t next_ticket, served;
    uint64_t rejected;
    int active;
    int retired;
};
static _Thread_local OresNativeActorMailbox* current_lane;

OresNativeActorMailbox* ores_native_actor_mailbox_open(size_t capacity) {
    if (capacity < 1 || capacity > 65536) return NULL;
    OresNativeActorMailbox* m = (OresNativeActorMailbox*)calloc(1, sizeof(*m));
    if (!m) return NULL;
    if (pthread_mutex_init(&m->lock, NULL) != 0) { free(m); return NULL; }
    if (pthread_cond_init(&m->changed, NULL) != 0) {
        pthread_mutex_destroy(&m->lock); free(m); return NULL;
    }
    m->capacity = capacity;
    return m;
}
int ores_native_actor_mailbox_enter(OresNativeActorMailbox* m) {
    if (!m || current_lane != NULL) return 0;
    if (pthread_mutex_lock(&m->lock) != 0) abort();
    if (m->retired || m->pending >= m->capacity || m->next_ticket == UINT64_MAX) {
        m->rejected++;
        pthread_mutex_unlock(&m->lock);
        return 0;
    }
    const uint64_t ticket = m->next_ticket++;
    m->pending++;
    while (!m->retired && (m->active || ticket != m->served))
        if (pthread_cond_wait(&m->changed, &m->lock) != 0) abort();
    if (m->retired) {
        m->pending--;
        pthread_cond_broadcast(&m->changed);
        pthread_mutex_unlock(&m->lock);
        return 0;
    }
    if (m->active || m->served != ticket) abort();
    m->active = 1;
    current_lane = m;
    pthread_mutex_unlock(&m->lock);
    return 1;
}
void ores_native_actor_mailbox_leave(OresNativeActorMailbox* m) {
    if (!m || current_lane != m) abort();
    if (pthread_mutex_lock(&m->lock) != 0) abort();
    if (!m->active || m->pending == 0 || m->served == UINT64_MAX) abort();
    m->active = 0;
    m->pending--;
    m->served++;
    current_lane = NULL;
    pthread_cond_broadcast(&m->changed);
    pthread_mutex_unlock(&m->lock);
}
void ores_native_actor_mailbox_retire(OresNativeActorMailbox* m) {
    if (!m) abort();
    if (pthread_mutex_lock(&m->lock) != 0) abort();
    m->retired = 1;
    pthread_cond_broadcast(&m->changed);
    pthread_mutex_unlock(&m->lock);
}
int ores_native_actor_mailbox_try_destroy(OresNativeActorMailbox* m) {
    if (!m) return 0;
    if (pthread_mutex_lock(&m->lock) != 0) abort();
    const int ready = m->retired && m->pending == 0 && !m->active;
    pthread_mutex_unlock(&m->lock);
    if (!ready) return 0;
    /* Caller is responsible for quiescence, including completed enter() calls. */
    if (pthread_cond_destroy(&m->changed) != 0 || pthread_mutex_destroy(&m->lock) != 0) abort();
    free(m);
    return 1;
}
uint64_t ores_native_actor_mailbox_rejected(OresNativeActorMailbox* m) {
    if (!m) abort();
    if (pthread_mutex_lock(&m->lock) != 0) abort();
    uint64_t count = m->rejected;
    pthread_mutex_unlock(&m->lock);
    return count;
}

size_t ores_native_actor_mailbox_pending(OresNativeActorMailbox* m) {
    if (!m) abort();
    if (pthread_mutex_lock(&m->lock) != 0) abort();
    const size_t pending = m->pending;
    pthread_mutex_unlock(&m->lock);
    return pending;
}
