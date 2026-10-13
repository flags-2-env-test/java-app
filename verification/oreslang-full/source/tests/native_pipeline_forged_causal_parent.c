/* Test-only source guest fault: forge a stale causal predecessor BEFORE
 * the group publication lock is taken. This is never linked in production.
 * The real compiled .ores Source actor must fail DATA admission rather than
 * accepting a valid-shaped MessagePack message with wrong ancestry.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
static atomic_int injected = 0;
void oreslang_source_actor_test_corrupt_causal_parent(
    uint64_t* active_parent, unsigned direction) {
    if (direction != 1) return;
    int expected = 0;
    if (atomic_compare_exchange_strong(&injected, &expected, 1))
        *active_parent = UINT64_MAX - UINT64_C(31);
}
__attribute__((destructor))
static void verify_fault_injected(void) {
    if (atomic_load(&injected) != 1) {
        fputs("SOURCE_CAUSAL_FAULT_NOT_EXERCISED\n", stderr);
        abort();
    }
    fputs("SOURCE_CAUSAL_FORGED_PARENT_INJECTED count=1\n", stderr);
}
