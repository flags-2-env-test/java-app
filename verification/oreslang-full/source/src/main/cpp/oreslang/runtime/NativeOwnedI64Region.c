#include "oreslang/runtime/NativeOwnedI64Region.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int64_t value;
    unsigned char alive;
} OwnedI64Slot;

struct OresOwnedI64Region {
    uint64_t capacity;
    uint64_t live_count;
    OwnedI64Slot slots[];
};

static void fail_closed(void) { abort(); }

OresOwnedI64Region* oreslang_owned_i64_region_open(uint64_t slots) {
    if (slots == 0 || slots > 4096) fail_closed();
    if (slots > (SIZE_MAX - offsetof(OresOwnedI64Region, slots)) / sizeof(OwnedI64Slot))
        fail_closed();
    const size_t bytes = offsetof(OresOwnedI64Region, slots)
                       + (size_t) slots * sizeof(OwnedI64Slot);
    OresOwnedI64Region* region = (OresOwnedI64Region*) calloc(1, bytes);
    if (!region) fail_closed(); /* Never hand unchecked null to generated code. */
    region->capacity = slots;
    return region;
}

int64_t* oreslang_owned_i64_region_new(OresOwnedI64Region* region, int64_t value) {
    if (!region || region->live_count >= region->capacity) fail_closed();
    for (uint64_t i = 0; i < region->capacity; ++i) {
        OwnedI64Slot* slot = &region->slots[i];
        if (!slot->alive) {
            slot->value = value;
            slot->alive = 1;
            region->live_count++;
            return &slot->value;
        }
    }
    fail_closed();
    return NULL;
}

void oreslang_owned_i64_region_drop(OresOwnedI64Region* region, int64_t* value) {
    if (!region || !value || region->live_count == 0) fail_closed();
    /* Provenance check before dereference. Exact slot value address only. */
    const uintptr_t first = (uintptr_t) &region->slots[0].value;
    const uintptr_t addr = (uintptr_t) value;
    const size_t stride = sizeof(OwnedI64Slot);
    if (addr < first || ((addr - first) % stride) != 0) fail_closed();
    const uint64_t index = (addr - first) / stride;
    if (index >= region->capacity) fail_closed();
    OwnedI64Slot* slot = &region->slots[index];
    if (!slot->alive || &slot->value != value) fail_closed();
    slot->value = 0; /* erase guest contents before physical region release */
    slot->alive = 0;
    region->live_count--;
}

void oreslang_owned_i64_region_close(OresOwnedI64Region* region) {
    if (!region || region->live_count != 0) fail_closed();
    const size_t bytes = offsetof(OresOwnedI64Region, slots)
                       + (size_t) region->capacity * sizeof(OwnedI64Slot);
    memset(region, 0, bytes);
    free(region); /* Native allocator only; no collector/statepoints. */
}
