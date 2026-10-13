#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <threads.h>
#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "actor heap check failed at %d\n", __LINE__); return 1; } } while (0)

enum Misuse { WRONG_OWNER, WRONG_EPOCH, FORGED_SLOT, DROP_LOANED, WRITE_LOANED, WRITE_IMMUTABLE_FIELD, WRITE_LOANED_FIELD, GENERIC_WRITE_FIXED_FIELD, GENERIC_DROP_FIXED_FIELD, READ_GENERIC_AS_FIXED_FIELD, DOUBLE_LOAN_RELEASE, REENTER, CALL_AFTER_RETIRE };
static void invalid_call(enum Misuse which) {
    OresActorI64Heap* heap = ores_actor_i64_heap_open(29, 7, 2);
    OresActorTurn turn = ores_actor_i64_heap_enter(heap, 29, 7);
    OresActorSlot slot = ores_actor_i64_heap_new(heap, turn, 99);
    if (which == WRONG_OWNER) {
        ores_actor_i64_heap_leave(heap, turn);
        (void) ores_actor_i64_heap_enter(heap, 30, 7);
    } else if (which == WRONG_EPOCH) {
        ores_actor_i64_heap_leave(heap, turn);
        OresActorTurn current = ores_actor_i64_heap_enter(heap, 29, 7);
        (void) current;
        (void) ores_actor_i64_heap_read(heap, turn, slot);
    } else if (which == FORGED_SLOT) {
        slot.domain++;
        (void) ores_actor_i64_heap_read(heap, turn, slot);
    } else if (which == DROP_LOANED) {
        (void) ores_actor_i64_heap_borrow(heap, turn, slot);
        ores_actor_i64_heap_drop(heap, turn, slot);
    } else if (which == WRITE_LOANED) {
        (void) ores_actor_i64_heap_borrow(heap, turn, slot);
        ores_actor_i64_heap_write(heap, turn, slot, 42);
    } else if (which == WRITE_IMMUTABLE_FIELD) {
        ores_actor_i64_heap_field_init(heap, turn.domain, turn.epoch, 1, 5);
        ores_actor_i64_heap_field_write(heap, turn.domain, turn.epoch, 1, 6);
    } else if (which == WRITE_LOANED_FIELD) {
        ores_actor_i64_heap_field_init_mutable(heap, turn.domain, turn.epoch, 1, 5);
        OresActorSlot field = {turn.domain, 1, 1};
        (void) ores_actor_i64_heap_borrow(heap, turn, field);
        ores_actor_i64_heap_field_write(heap, turn.domain, turn.epoch, 1, 6);
    } else if (which == GENERIC_WRITE_FIXED_FIELD) {
        ores_actor_i64_heap_field_init_mutable(heap, turn.domain, turn.epoch, 1, 5);
        OresActorSlot field = {turn.domain, 1, 1};
        ores_actor_i64_heap_write(heap, turn, field, 6);
    } else if (which == GENERIC_DROP_FIXED_FIELD) {
        ores_actor_i64_heap_field_init_mutable(heap, turn.domain, turn.epoch, 1, 5);
        OresActorSlot field = {turn.domain, 1, 1};
        ores_actor_i64_heap_drop(heap, turn, field);
    } else if (which == READ_GENERIC_AS_FIXED_FIELD) {
        (void) ores_actor_i64_heap_field_read(heap, turn.domain, turn.epoch, 0);
    } else if (which == DOUBLE_LOAN_RELEASE) {
        OresActorLoan loan = ores_actor_i64_heap_borrow(heap, turn, slot);
        ores_actor_i64_heap_release(heap, loan);
        ores_actor_i64_heap_release(heap, loan);
    } else if (which == REENTER) {
        (void) ores_actor_i64_heap_enter(heap, 29, 7);
    } else if (which == CALL_AFTER_RETIRE) {
        ores_actor_i64_heap_retire(heap);
        (void) ores_actor_i64_heap_new(heap, turn, 6);
    }
    /* None of these calls may finish normally. */
}
#if defined(__unix__) || defined(__APPLE__)
static int rejects(enum Misuse which) {
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        struct rlimit limit = {0, 0};
        (void) setrlimit(RLIMIT_CORE, &limit);
        invalid_call(which);
        _exit(91);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return 0;
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}
#endif
typedef struct {
    OresActorI64Heap* heap;
    uint64_t expected;
    uint64_t updated;
    int failed;
} Carrier;
static int second_carrier(void* argument) {
    Carrier* carrier = (Carrier*) argument;
    OresActorTurn turn = ores_actor_i64_heap_enter(carrier->heap, 29, 7);
    OresActorSlot slot = ores_actor_i64_heap_new(carrier->heap, turn, (int64_t) carrier->expected);
    if (ores_actor_i64_heap_read(carrier->heap, turn, slot) != (int64_t) carrier->expected) carrier->failed = 1;
    ores_actor_i64_heap_write(carrier->heap, turn, slot, (int64_t) carrier->updated);
    if (ores_actor_i64_heap_read(carrier->heap, turn, slot) != (int64_t) carrier->updated) carrier->failed = 1;
    ores_actor_i64_heap_drop(carrier->heap, turn, slot);
    ores_actor_i64_heap_leave(carrier->heap, turn);
    return 0;
}

int main(void) {
    /* One actor heap survives between logical turns and native carriers. */
    OresActorI64Heap* heap = ores_actor_i64_heap_open(29, 7, 2);
    OresActorTurn first = ores_actor_i64_heap_enter(heap, 29, 7);
    OresActorSlot saved = ores_actor_i64_heap_new(heap, first, 40);
    CHECK(ores_actor_i64_heap_read(heap, first, saved) == 40);
    OresActorLoan suspended = ores_actor_i64_heap_borrow(heap, first, saved);
    ores_actor_i64_heap_leave(heap, first); /* simulates await/suspension */
    OresActorTurn next = ores_actor_i64_heap_enter(heap, 29, 7);
    CHECK(ores_actor_i64_heap_read(heap, next, saved) == 40);
    /* Suspended shared loan prevents writes until released. */
    ores_actor_i64_heap_leave(heap, next);
    /* A different native thread executes the next actor turn. */
    Carrier carrier = {heap, 21, 43, 0};
    thrd_t native;
    CHECK(thrd_create(&native, second_carrier, &carrier) == thrd_success);
    int exit_status = -1;
    CHECK(thrd_join(native, &exit_status) == thrd_success);
    CHECK(exit_status == 0 && !carrier.failed);
    OresActorTurn after = ores_actor_i64_heap_enter(heap, 29, 7);
    CHECK(ores_actor_i64_heap_read(heap, after, saved) == 40);
    ores_actor_i64_heap_leave(heap, after);
    ores_actor_i64_heap_retire(heap);
    CHECK(ores_actor_i64_heap_try_destroy(heap) == 0); /* outstanding loan */
    ores_actor_i64_heap_release(heap, suspended);
    CHECK(ores_actor_i64_heap_try_destroy(heap) == 1);
#if defined(__unix__) || defined(__APPLE__)
    CHECK(rejects(WRONG_OWNER));
    CHECK(rejects(WRONG_EPOCH));
    CHECK(rejects(FORGED_SLOT));
    CHECK(rejects(DROP_LOANED));
    CHECK(rejects(WRITE_LOANED));
    CHECK(rejects(WRITE_IMMUTABLE_FIELD));
    CHECK(rejects(WRITE_LOANED_FIELD));
    CHECK(rejects(GENERIC_WRITE_FIXED_FIELD));
    CHECK(rejects(GENERIC_DROP_FIXED_FIELD));
    CHECK(rejects(READ_GENERIC_AS_FIXED_FIELD));
    CHECK(rejects(DOUBLE_LOAN_RELEASE));
    CHECK(rejects(REENTER));
    CHECK(rejects(CALL_AFTER_RETIRE));
#endif
    return 0;
}
