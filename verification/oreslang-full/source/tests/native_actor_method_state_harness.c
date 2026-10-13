#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include <stdint.h>
#include <stdio.h>

// These generated ABI entrypoints are supervisor-only. Guest Oreslang code
// must not be allowed to fabricate heap pointers or scheduler turn tokens.
extern void oreslang_actor_Counter_init(OresActorI64Heap*, uint64_t, uint64_t);
extern int64_t oreslang_actor_Counter_current(OresActorI64Heap*, uint64_t, uint64_t);
extern int64_t oreslang_actor_Counter_increment(OresActorI64Heap*, uint64_t, uint64_t, int64_t);
extern int64_t oreslang_actor_Counter_reset(OresActorI64Heap*, uint64_t, uint64_t, int64_t);

#define VERIFY(condition) do { if (!(condition)) { \
    fprintf(stderr, "actor-state native assertion failed at %d\n", __LINE__); \
    return 2; \
} } while (0)

int main(void) {
    OresActorI64Heap* heap = ores_actor_i64_heap_open(17, 5, 1);
    OresActorTurn first = ores_actor_i64_heap_enter(heap, 17, 5);
    oreslang_actor_Counter_init(heap, first.domain, first.epoch);
    VERIFY(oreslang_actor_Counter_current(heap, first.domain, first.epoch) == 40);
    VERIFY(oreslang_actor_Counter_increment(heap, first.domain, first.epoch, 2) == 42);
    VERIFY(oreslang_actor_Counter_current(heap, first.domain, first.epoch) == 42);
    ores_actor_i64_heap_leave(heap, first);
    // New scheduler turn, same owned heap and values.
    OresActorTurn second = ores_actor_i64_heap_enter(heap, 17, 5);
    VERIFY(second.epoch != first.epoch);
    VERIFY(oreslang_actor_Counter_current(heap, second.domain, second.epoch) == 42);
    VERIFY(oreslang_actor_Counter_reset(heap, second.domain, second.epoch, 9) == 9);
    VERIFY(oreslang_actor_Counter_current(heap, second.domain, second.epoch) == 9);
    ores_actor_i64_heap_leave(heap, second);
    ores_actor_i64_heap_retire(heap);
    VERIFY(ores_actor_i64_heap_try_destroy(heap) == 1);
    puts("ACTOR_METHOD_STATE_NATIVE_PASS");
    return 43;
}
