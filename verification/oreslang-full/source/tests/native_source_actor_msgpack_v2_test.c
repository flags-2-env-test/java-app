#include "oreslang/runtime/SourceActorBootstrap.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Atomic int occupied = 0;
static _Atomic int messages = 0;
static _Atomic int initialized = 0;
static unsigned char canonical[256];
static size_t canonical_size;

static void constructor(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    ores_actor_i64_heap_field_init_mutable(heap, domain, epoch, 0, 0);
    atomic_store(&initialized, 1);
}
static int64_t on_wire(OresActorI64Heap* heap, uint64_t domain,
                       uint64_t epoch, const uint8_t* bytes, size_t size) {
    assert(atomic_exchange(&occupied, 1) == 0);
    assert(atomic_load(&initialized));
    assert(size == canonical_size && memcmp(bytes, canonical, size) == 0);
    int64_t count = ores_actor_i64_heap_field_read(heap, domain, epoch, 0);
    ores_actor_i64_heap_field_write(heap, domain, epoch, 0, count + 1);
    atomic_fetch_add(&messages, 1);
    assert(atomic_exchange(&occupied, 0) == 1);
    return count + 1;
}
static uint8_t nibble(char ch) {
    if (ch >= '0' && ch <= '9') return (uint8_t)(ch - '0');
    if (ch >= 'a' && ch <= 'f') return (uint8_t)(ch - 'a' + 10);
    abort();
}
static size_t unpack(const char* hex, uint8_t* out) {
    const size_t n = strlen(hex);
    assert(!(n % 2) && n / 2 <= 256);
    for (size_t i = 0; i < n / 2; ++i)
        out[i] = (uint8_t)(nibble(hex[i*2]) << 4 | nibble(hex[i*2+1]));
    return n / 2;
}
int main(void) {
    static const char hex[] =
        "9402ae64656d6f2e537472696e672e7631c420"
        "21c2efdbff08dd4e1320d8abe1ba0cc6aea8e4cf104b984be7b7079968fad3cda3666f6f";
    canonical_size = unpack(hex, canonical);
    char mutable_schema[] = "demo.String.v1";
    char mutable_shape[] = "S";
    uint8_t mutable_hash[32];
    memcpy(mutable_hash, canonical + 2 + 1 + strlen(mutable_schema) + 2, 32);
    OresMp2Contract source = {mutable_schema, mutable_shape, mutable_hash};
    assert(ores_mp2_contract_valid(&source));
    OresMp2Contract bad = source;
    bad.shape = "I";
    assert(!oreslang_source_actor_spawn_wire(constructor, on_wire, 1, &bad));

    void* actor = oreslang_source_actor_spawn_wire(constructor, on_wire, 1, &source);
    assert(actor);
    /* Future user registrations cannot mutate the live compiled contract. */
    memset(mutable_schema, 'x', strlen(mutable_schema));
    mutable_shape[0] = 'I';
    memset(mutable_hash, 0, sizeof(mutable_hash));
    uint8_t submitted[256];
    memcpy(submitted, canonical, canonical_size);
    submitted[1] = 1;
    assert(!oreslang_source_actor_send_wire(actor, submitted, canonical_size));
    memcpy(submitted, canonical, canonical_size);
    submitted[3 + strlen("demo.String.v1") + 2] ^= 1;
    assert(oreslang_source_actor_send_wire(actor, canonical, canonical_size));
    /* A forged SHA-256 descriptor must fail, even when payload is valid. */
    assert(oreslang_source_actor_send_wire(actor, submitted, canonical_size) == 0);
    memcpy(submitted, canonical, canonical_size);
    assert(oreslang_source_actor_send_wire(actor, submitted, canonical_size));
    memset(submitted, 0, canonical_size);
    oreslang_source_actor_stop(actor);
    assert(atomic_load(&messages) == 2);
    assert(atomic_load(&initialized) == 1);
    assert(atomic_load(&occupied) == 0);
    puts("SOURCE_ACTOR_MESSAGEPACK_V2_PASS compiler_abi=1 same_lane=1 copied_contract=1");
    return 0;
}
