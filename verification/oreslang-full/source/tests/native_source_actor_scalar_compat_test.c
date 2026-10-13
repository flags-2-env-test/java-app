#include "oreslang/runtime/SourceActorBootstrap.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

static atomic_int count;
static void scalar_init(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    ores_actor_i64_heap_field_init_mutable(heap, domain, epoch, 0, 0);
}
static int64_t scalar_message(OresActorI64Heap* heap, uint64_t domain,
                              uint64_t epoch, int64_t n) {
    assert(n >= 1 && n <= 4);
    const int64_t before = ores_actor_i64_heap_field_read(heap, domain, epoch, 0);
    ores_actor_i64_heap_field_write(heap, domain, epoch, 0, before + n);
    atomic_fetch_add(&count, 1);
    return before + n;
}
int main(void) {
    void* actor = oreslang_source_actor_spawn(scalar_init, scalar_message, 1);
    assert(actor);
    for (int64_t i = 1; i <= 4; ++i) oreslang_source_actor_send(actor, i);
    oreslang_source_actor_stop(actor);
    assert(atomic_load(&count) == 4);
    puts("SCALAR_SOURCE_ACTOR_REGRESSION_PASS turns=4");
    return 0;
}
