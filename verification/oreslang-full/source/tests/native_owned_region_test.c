#include "oreslang/runtime/NativeOwnedI64Region.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "owned region invariant failed at line %d\n", __LINE__); \
        return 1; \
    } \
} while (0)

enum Failure {
    DOUBLE_DROP = 1,
    FOREIGN_REGION_DROP = 2,
    CLOSE_WITH_LIVE_OWNER = 3,
    OUT_OF_CAPACITY = 4
};

static void misuse(enum Failure kind) {
    OresOwnedI64Region* a = oreslang_owned_i64_region_open(1);
    int64_t* item = oreslang_owned_i64_region_new(a, 12);
    if (kind == DOUBLE_DROP) {
        oreslang_owned_i64_region_drop(a, item);
        oreslang_owned_i64_region_drop(a, item);
    }
    if (kind == FOREIGN_REGION_DROP) {
        OresOwnedI64Region* b = oreslang_owned_i64_region_open(1);
        int64_t* other = oreslang_owned_i64_region_new(b, 11);
        oreslang_owned_i64_region_drop(a, other);
    }
    if (kind == CLOSE_WITH_LIVE_OWNER) {
        oreslang_owned_i64_region_close(a);
    }
    if (kind == OUT_OF_CAPACITY) {
        (void) oreslang_owned_i64_region_new(a, 9);
    }
    /* An erroneous native call must never return normally. */
}

#if defined(__unix__) || defined(__APPLE__)
static int rejects_misuse(enum Failure kind) {
    const pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        struct rlimit no_core = {0, 0};
        setrlimit(RLIMIT_CORE, &no_core);
        misuse(kind);
        _exit(78);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return 0;
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}
#endif

int main(void) {
    OresOwnedI64Region* region = oreslang_owned_i64_region_open(2);
    int64_t* a = oreslang_owned_i64_region_new(region, 6);
    int64_t* b = oreslang_owned_i64_region_new(region, *a * 7);
    CHECK(*a == 6);
    CHECK(*b == 42);
    oreslang_owned_i64_region_drop(region, b);
    oreslang_owned_i64_region_drop(region, a);
    oreslang_owned_i64_region_close(region);

#if defined(__unix__) || defined(__APPLE__)
    CHECK(rejects_misuse(DOUBLE_DROP));
    CHECK(rejects_misuse(FOREIGN_REGION_DROP));
    CHECK(rejects_misuse(CLOSE_WITH_LIVE_OWNER));
    CHECK(rejects_misuse(OUT_OF_CAPACITY));
#endif
    return 0;
}
