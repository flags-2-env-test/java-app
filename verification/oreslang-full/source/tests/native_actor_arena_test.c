#include "oreslang/runtime/ActorArena.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <threads.h>
#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "native actor arena failed:%d\n", __LINE__); return 1; } } while (0)
enum Misuse { STALE_TURN, FOREIGN_OWNER, OVERLAP, SCRATCH_AFTER_LEAVE, WRITE_LOANED, DROP_LOANED, DOUBLE_RELEASE, OUT_OF_BOUNDS, SCRATCH_OVERFLOW, BAD_ALIGNMENT, OVERSIZED_ACTOR_BUDGET, FOREIGN_CARRIER_TURN, NESTED_ACTOR_TURN };
typedef struct { OresActorArena* arena; OresArenaTurn turn; OresArenaHandle handle; } WrongCarrier;
static int different_carrier_with_stolen_turn(void* opaque) {
    WrongCarrier* x = (WrongCarrier*) opaque;
    int value = 0;
    ores_actor_arena_read(x->arena, x->turn, x->handle, 0, &value, sizeof(value));
    return 0;
}
static void invalid(enum Misuse m) {
    OresActorArena* a = ores_actor_arena_open(23, 2, 2, 64, 128);
    OresArenaTurn t = ores_actor_arena_enter(a, 23, 2);
    OresArenaHandle h = ores_actor_arena_alloc(a, t, 16);
    OresArenaScratch s = ores_actor_arena_scratch_alloc(a, t, 16, 8);
    if (m == STALE_TURN) {
        ores_actor_arena_leave(a, t);
        (void) ores_actor_arena_enter(a, 23, 2);
        int v; ores_actor_arena_read(a, t, h, 0, &v, sizeof(v));
    } else if (m == FOREIGN_OWNER) {
        ores_actor_arena_leave(a, t);
        (void) ores_actor_arena_enter(a, 24, 2);
    } else if (m == OVERLAP) {
        (void) ores_actor_arena_enter(a, 23, 2);
    } else if (m == SCRATCH_AFTER_LEAVE) {
        ores_actor_arena_leave(a, t);
        OresArenaTurn next = ores_actor_arena_enter(a, 23, 2);
        int v; ores_actor_arena_scratch_read(a, next, s, 0, &v, sizeof(v));
    } else if (m == WRITE_LOANED) {
        (void) ores_actor_arena_borrow(a, t, h);
        int v = 1; ores_actor_arena_write(a, t, h, 0, &v, sizeof(v));
    } else if (m == DROP_LOANED) {
        (void) ores_actor_arena_borrow(a, t, h);
        ores_actor_arena_drop(a, t, h);
    } else if (m == DOUBLE_RELEASE) {
        OresArenaLoan l = ores_actor_arena_borrow(a, t, h);
        ores_actor_arena_release(a, l);
        ores_actor_arena_release(a, l);
    } else if (m == OUT_OF_BOUNDS) {
        int v; ores_actor_arena_read(a, t, h, 15, &v, sizeof(v));
    } else if (m == SCRATCH_OVERFLOW) {
        (void) ores_actor_arena_scratch_alloc(a, t, 200, 8);
    } else if (m == BAD_ALIGNMENT) {
        (void) ores_actor_arena_scratch_alloc(a, t, 8, 3);
    } else if (m == OVERSIZED_ACTOR_BUDGET) {
        (void) ores_actor_arena_open(24, 4, 4096, 1024 * 1024, 128);
    } else if (m == FOREIGN_CARRIER_TURN) {
        WrongCarrier wrong = {a, t, h};
        thrd_t other;
        if (thrd_create(&other, different_carrier_with_stolen_turn, &wrong) != thrd_success)
            abort();
        (void) thrd_join(other, NULL);
    } else if (m == NESTED_ACTOR_TURN) {
        OresActorArena* other = ores_actor_arena_open(33, 2, 1, 16, 16);
        (void) ores_actor_arena_enter(other, 33, 2);
    }
}
#if defined(__unix__) || defined(__APPLE__)
static int rejects(enum Misuse m) {
    pid_t p = fork();
    if (p < 0) return 0;
    if (p == 0) {
        struct rlimit lim = {0,0};
        (void) setrlimit(RLIMIT_CORE, &lim);
        invalid(m); _exit(91);
    }
    int st = 0;
    return waitpid(p, &st, 0) == p && WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT;
}
#endif
typedef struct {OresActorArena* a; OresArenaHandle h; int failed;} Carrier;
static int other_carrier(void* p) {
    Carrier* v = (Carrier*) p;
    OresArenaTurn t = ores_actor_arena_enter(v->a, 23, 2);
    int value = 0;
    ores_actor_arena_read(v->a, t, v->h, 0, &value, sizeof(value));
    if (value != 42) v->failed = 1;
    OresArenaScratch s = ores_actor_arena_scratch_alloc(v->a, t, sizeof(value), 4);
    ores_actor_arena_scratch_read(v->a, t, s, 0, &value, sizeof(value));
    if (value != 0) v->failed = 1;
    ores_actor_arena_leave(v->a, t);
    return 0;
}
int main(void) {
    OresActorArena* a = ores_actor_arena_open(23, 2, 2, 64, 128);
    OresArenaTurn first = ores_actor_arena_enter(a, 23, 2);
    OresArenaHandle h = ores_actor_arena_alloc(a, first, 16);
    /* Repeated allocations reuse a free slot without malloc or scan, and
     * monotonic generations reject stale handles after reuse. */
    uint64_t last_generation = 0;
    for (int i = 0; i < 8192; ++i) {
        OresArenaHandle temp = ores_actor_arena_alloc(a, first, 8);
        CHECK(temp.index == 1 && temp.generation > last_generation);
        last_generation = temp.generation;
        ores_actor_arena_drop(a, first, temp);
    }
    int value = 42;
    ores_actor_arena_write(a, first, h, 0, &value, sizeof(value));
    OresArenaScratch s = ores_actor_arena_scratch_alloc(a, first, 8, 8);
    ores_actor_arena_scratch_write(a, first, s, 0, &value, sizeof(value));
    value = 0; ores_actor_arena_scratch_read(a, first, s, 0, &value, sizeof(value));
    CHECK(value == 42);
    OresArenaLoan hold = ores_actor_arena_borrow(a, first, h);
    ores_actor_arena_leave(a, first);
    /* Durable loans survive turns; scratch is zeroized and recycled. */
    Carrier car = {a, h, 0};
    thrd_t carrier;
    CHECK(thrd_create(&carrier, other_carrier, &car) == thrd_success);
    int outcome = -1;
    CHECK(thrd_join(carrier, &outcome) == thrd_success);
    CHECK(outcome == 0 && !car.failed);
    OresArenaTurn last = ores_actor_arena_enter(a, 23, 2);
    value = 0; ores_actor_arena_read(a, last, h, 0, &value, sizeof(value));
    CHECK(value == 42);
    ores_actor_arena_leave(a, last);
    ores_actor_arena_retire(a);
    CHECK(ores_actor_arena_try_destroy(a) == 0);
    ores_actor_arena_release(a, hold);
    CHECK(ores_actor_arena_try_destroy(a) == 1);
    /* Supervisor cancellation may retire a heap during an active turn. */
    a = ores_actor_arena_open(23, 3, 1, 32, 64);
    OresArenaTurn unwinding = ores_actor_arena_enter(a, 23, 3);
    ores_actor_arena_retire(a);
    CHECK(ores_actor_arena_try_destroy(a) == 0);
    ores_actor_arena_leave(a, unwinding);
    CHECK(ores_actor_arena_try_destroy(a) == 1);
#if defined(__unix__) || defined(__APPLE__)
    for (int n = STALE_TURN; n <= NESTED_ACTOR_TURN; ++n) CHECK(rejects((enum Misuse) n));
#endif
    return 0;
}
