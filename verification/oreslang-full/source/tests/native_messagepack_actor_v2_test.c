#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/MessagePackActorV2.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *schema, *shape, *hex; } Golden;
static const Golden golden[] = {
    {"demo.TupleBoolString.v1", "T[B,S]", "9402b764656d6f2e5475706c65426f6f6c537472696e672e7631c420d254b4ecd3d7964454dcdf86fd10ede585a17b3af27982a289819afdc2379ab292c3a3666f6f"},
    {"demo.FixedArrayInts.v1", "FA[I,I,I]", "9402b664656d6f2e46697865644172726179496e74732e7631c4206a46f6c65c619cc1c5354f7bd3b35ec0fcfbc9866c0760a15b860cfef79773b993010203"},
    {"demo.FixedListNested.v1", "FL[L[S],B]", "9402b764656d6f2e46697865644c6973744e65737465642e7631c4202007a0279d88d3eca7d21cf32b37327b996593c1353963dd78555f56f1ba6ddc9292a161a162c2"},
    {"demo.ArrayStrings.v1", "A[S]", "9402b464656d6f2e4172726179537472696e67732e7631c420139722a10b9ef4d4af0cea1bfbc50ff92a0459aabf837f63a6fec052fe5dc5c992a161a162"},
    {"demo.ListStrings.v1", "L[S]", "9402b364656d6f2e4c697374537472696e67732e7631c4204efbea59591d5c31d9f13ee93ef5c1d954e653144d380bb2d064ea4fa6d56b8c92a161a162"},
    {"demo.String.v1", "S", "9402ae64656d6f2e537472696e672e7631c42021c2efdbff08dd4e1320d8abe1ba0cc6aea8e4cf104b984be7b7079968fad3cda3666f6f"},
    {"demo.Float64.v1", "F", "9402af64656d6f2e466c6f617436342e7631c42085e27ecf506d7dfa7e3db8c1449d9f3cdf7639765689c429f693b6cbf5d68f91cb3ff8000000000000"}
};
static uint8_t read_hex_digit(char c) {
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    abort();
}
static size_t decode_hex(const char* s, uint8_t* into, size_t cap) {
    size_t n = strlen(s);
    assert(n % 2 == 0 && n / 2 <= cap);
    for (size_t i = 0; i < n / 2; ++i)
        into[i] = (uint8_t)((read_hex_digit(s[i * 2]) << 4) | read_hex_digit(s[i * 2 + 1]));
    return n / 2;
}
static OresMp2Contract contract(size_t index, const uint8_t* bytes, size_t n) {
    /* The golden manifest's schema ID is followed immediately by c4 20
     * <sha256(shape)>; the SHA is pinned by compiler-generated declaration.
     */
    size_t schema = strlen(golden[index].schema);
    size_t at = 2 + 1 + schema;
    assert(n >= at + 34 && bytes[0] == 0x94 && bytes[1] == 2
        && bytes[2] == (uint8_t)(0xa0 | schema));
    assert(bytes[at] == 0xc4 && bytes[at + 1] == 32);
    return (OresMp2Contract){golden[index].schema, golden[index].shape, bytes + at + 2};
}
typedef struct {
    atomic_int entered[2], finished[2], release;
    atomic_int active[2], max_active[2];
    uint8_t expected[2][1024];
    size_t expected_size[2];
} State;
static void on_message(void* raw, size_t slot, const uint8_t* bytes, size_t n) {
    State* state = raw;
    assert(slot < 2);
    int active = atomic_fetch_add(&state->active[slot], 1) + 1;
    int old = atomic_load(&state->max_active[slot]);
    while (active > old && !atomic_compare_exchange_weak(&state->max_active[slot], &old, active)) {}
    atomic_fetch_add(&state->entered[slot], 1);
    if (slot == 0) while (!atomic_load(&state->release)) sched_yield();
    assert(n == state->expected_size[slot]);
    assert(memcmp(bytes, state->expected[slot], n) == 0);
    atomic_fetch_sub(&state->active[slot], 1);
    atomic_fetch_add(&state->finished[slot], 1);
}
static void wait_atomic(atomic_int* counter, int expected) {
    for (int tries = 0; tries < 1000000; ++tries) {
        if (atomic_load(counter) >= expected) return;
        sched_yield();
    }
    abort();
}
typedef struct {
    OresMp2ActorGroup* group;
    OresMp2ActorRef actor;
    const uint8_t* wire;
    size_t size;
    atomic_int* rejected;
} FloodArgs;
static void* saturated_sender(void* raw) {
    FloodArgs* args = raw;
    if (ores_mp2_group_send(args->group, args->actor, args->wire, args->size))
        abort(); /* capacity=1, slot's original turn is still active */
    atomic_fetch_add(args->rejected, 1);
    return NULL;
}
int main(void) {
    uint8_t wire[7][1024]; size_t sizes[7];
    OresMp2Contract schemas[7];
    for (size_t i = 0; i < 7; ++i) {
        sizes[i] = decode_hex(golden[i].hex, wire[i], sizeof(wire[i]));
        schemas[i] = contract(i, wire[i], sizes[i]);
        OresMp2Owned owned = {0};
        assert(ores_mp2_validate_copy(&schemas[i], wire[i], sizes[i], &owned));
        assert(owned.size == sizes[i] && memcmp(owned.bytes, wire[i], sizes[i]) == 0);
        assert(!ores_mp2_validate_copy(&schemas[i], wire[i], sizes[i], &owned));
        assert(ores_mp2_validate_owned(&schemas[i], &owned));
        wire[i][sizes[i] - 1] ^= 0xff; /* detach from mutable sender bytes */
        assert(memcmp(owned.bytes, wire[i], sizes[i]) != 0);
        /* Decoder can ONLY examine the owned snapshot, never a later
         * mutation of the caller's buffer. */
        assert(ores_mp2_validate_owned(&schemas[i], &owned));
        OresMp2Owned deliberately_invalid = {0};
        deliberately_invalid.bytes = malloc(owned.size);
        assert(deliberately_invalid.bytes);
        deliberately_invalid.size = owned.size;
        memcpy(deliberately_invalid.bytes, owned.bytes, owned.size);
        deliberately_invalid.bytes[1] = 1; /* unsupported protocol version */
        assert(!ores_mp2_validate_owned(&schemas[i], &deliberately_invalid));
        ores_mp2_owned_release(&deliberately_invalid);
        wire[i][sizes[i] - 1] ^= 0xff;
        ores_mp2_owned_release(&owned);
        ores_mp2_owned_release(&owned);
        assert(!owned.bytes && !owned.size);
        assert(!ores_mp2_validate_copy(&schemas[i], wire[i], sizes[i] - 1, &owned));
        OresMp2Contract mismatch = schemas[i];
        mismatch.shape = "I";
        if (strcmp(golden[i].shape, "I") != 0)
            assert(!ores_mp2_validate_copy(&mismatch, wire[i], sizes[i], &owned));
        uint8_t corrupted[1024];
        memcpy(corrupted, wire[i], sizes[i]);
        corrupted[1] = 1; /* v1 is not accepted under v2 */
        assert(!ores_mp2_validate_copy(&schemas[i], corrupted, sizes[i], &owned));
        memcpy(corrupted, wire[i], sizes[i]);
        corrupted[3 + strlen(golden[i].schema) + 2] ^= 1; /* forged shape hash */
        assert(!ores_mp2_validate_copy(&schemas[i], corrupted, sizes[i], &owned));
        size_t digest_at = 3 + strlen(golden[i].schema) + 2;
        assert(digest_at + 32 <= sizes[i]);
        for (size_t byte = 0; byte < 32; ++byte) {
            memcpy(corrupted, wire[i], sizes[i]);
            corrupted[digest_at + byte] ^= 1;
            assert(!ores_mp2_validate_copy(&schemas[i], corrupted, sizes[i], &owned));
        }
        memcpy(corrupted, wire[i], sizes[i]);
        corrupted[sizes[i]] = 0; /* trailing value */
        assert(!ores_mp2_validate_copy(&schemas[i], corrupted, sizes[i] + 1, &owned));
        assert(!ores_mp2_validate_copy(&schemas[(i + 1) % 7], wire[i], sizes[i], &owned));
    }
    /* Strict UTF-8 rejects overlong two-byte sequences at the receiver. */
    OresMp2Owned invalid_text = {0};
    uint8_t malformed_utf8[1024];
    memcpy(malformed_utf8, wire[5], sizes[5]);
    malformed_utf8[sizes[5] - 3] = 0xc0;
    malformed_utf8[sizes[5] - 2] = 0xaf;
    assert(!ores_mp2_validate_copy(&schemas[5], malformed_utf8,
                                    sizes[5], &invalid_text));
    uint8_t* enormous = calloc((size_t)ORES_MP2_MAX_BYTES + 1, 1);
    assert(enormous);
    assert(!ores_mp2_validate_copy(&schemas[5], enormous,
                                    (size_t)ORES_MP2_MAX_BYTES + 1, &invalid_text));
    free(enormous);
    assert(!invalid_text.bytes && invalid_text.size == 0);

    /* A non-finite IEEE-754 binary64 must be rejected even with an
     * otherwise valid schema ID and independently verified shape hash.
     * The final eight bytes of the finite 1.5 golden contain the double.
     */
    OresMp2Owned invalid_float = {0};
    uint8_t nonfinite[1024];
    memcpy(nonfinite, wire[6], sizes[6]);
    nonfinite[sizes[6] - 8] = 0x7f;
    nonfinite[sizes[6] - 7] = 0xf8;
    for (size_t i = sizes[6] - 6; i < sizes[6]; ++i) nonfinite[i] = 0;
    assert(!ores_mp2_validate_copy(&schemas[6], nonfinite, sizes[6], &invalid_float));
    assert(!invalid_float.bytes && !invalid_float.size);
    /* Noncanonical positive one as uint8 is forbidden; Java rejects it. */
    uint8_t inflated_integer[1024];
    memcpy(inflated_integer, wire[1], sizes[1] - 3);
    inflated_integer[sizes[1]-3] = 0xcc;
    inflated_integer[sizes[1]-2] = 1; /* nonminimal uint8(1) */
    inflated_integer[sizes[1]-1] = 2;
    inflated_integer[sizes[1]] = 3;
    assert(!ores_mp2_validate_copy(&schemas[1], inflated_integer, sizes[1] + 1, &invalid_float));
    State state = {0};
    state.expected_size[0] = sizes[0]; state.expected_size[1] = sizes[5];
    memcpy(state.expected[0], wire[0], sizes[0]);
    memcpy(state.expected[1], wire[5], sizes[5]);
    OresMp2ActorGroup* group = ores_mp2_group_open(2, 1, 2, on_message, &state);
    assert(group);
    /* Hash must be derived from the pinned shape, not blindly trusted
     * just because a host pointer or MessagePack frame supplies 32 bytes.
     */
    OresMp2Contract forged_shape = schemas[0];
    forged_shape.shape = "I";
    assert(!ores_mp2_group_contract(group, 0, forged_shape));
    uint8_t wrong_hash[32];
    memcpy(wrong_hash, schemas[0].shape_sha256, 32);
    wrong_hash[0] ^= 0x80;
    OresMp2Contract forged_hash = schemas[0];
    forged_hash.shape_sha256 = wrong_hash;
    assert(!ores_mp2_group_contract(group, 0, forged_hash));
    /* Contract registration deep-copies mutable caller buffers. */
    char mutable_schema[256];
    char mutable_shape[128];
    uint8_t mutable_hash[32];
    strcpy(mutable_schema, schemas[0].schema_id);
    strcpy(mutable_shape, schemas[0].shape);
    memcpy(mutable_hash, schemas[0].shape_sha256, 32);
    OresMp2Contract borrowed = {mutable_schema, mutable_shape, mutable_hash};
    assert(ores_mp2_group_contract(group, 0, borrowed));
    memset(mutable_schema, 'x', strlen(mutable_schema));
    memset(mutable_shape, 'x', strlen(mutable_shape));
    memset(mutable_hash, 0, 32);
    assert(!ores_mp2_group_contract(group, 0, schemas[0]));
    assert(ores_mp2_group_contract(group, 1, schemas[5]));
    OresMp2ActorRef a = ores_mp2_group_spawn(group, 0);
    OresMp2ActorRef b = ores_mp2_group_spawn(group, 1);
    assert(a.generation && b.generation);
    assert(!ores_mp2_group_contract(group, 0, schemas[0]));
    assert(!ores_mp2_group_spawn(group, 0).generation);
    assert(!ores_mp2_group_send(group, a, wire[5], sizes[5]));
    assert(!ores_mp2_group_send(group, b, wire[0], sizes[0]));
    assert(ores_mp2_group_send(group, a, wire[0], sizes[0]));
    wait_atomic(&state.entered[0], 1);
    /* Capacity counts an actively executing turn: cannot acquire a second
     * buffer/admission in the same lane. Independent actor may still run. */
    assert(!ores_mp2_group_send(group, a, wire[0], sizes[0]));
    /* Concurrent producers cannot create unbounded pre-admission snapshots
     * while one accepted DATA callback occupies the only capacity slot.
     */
    enum { SENDERS = 12 };
    pthread_t flood_threads[SENDERS];
    atomic_int flood_rejected = 0;
    FloodArgs flood = {group, a, wire[0], sizes[0], &flood_rejected};
    for (size_t i = 0; i < SENDERS; ++i)
        assert(pthread_create(&flood_threads[i], NULL, saturated_sender, &flood) == 0);
    for (size_t i = 0; i < SENDERS; ++i)
        assert(pthread_join(flood_threads[i], NULL) == 0);
    assert(atomic_load(&flood_rejected) == SENDERS);
    OresMp2ActorRef stale = b;
    stale.generation++;
    assert(!ores_mp2_group_send(group, stale, wire[5], sizes[5]));
    OresMp2ActorRef foreign = b;
    foreign.scheduler_domain++;
    assert(!ores_mp2_group_send(group, foreign, wire[5], sizes[5]));
    /* Failed stale/foreign sends must release their reservation, allowing
     * the valid generation-fenced destination to accept the same payload.
     */
    assert(ores_mp2_group_send(group, b, wire[5], sizes[5]));
    wait_atomic(&state.finished[1], 1);
    wire[0][sizes[0] - 1] ^= 0xff; /* mutate sender after acceptance */
    atomic_store(&state.release, 1);
    wait_atomic(&state.finished[0], 1);
    wire[0][sizes[0] - 1] ^= 0xff; /* restore canonical sender input */
    /* The completed DATA turn must release its admission quota. Retry
     * only while the carrier is finishing the post-callback cleanup.
     */
    int second_admitted = 0;
    for (int attempt = 0; attempt < 1000000; ++attempt) {
        if (ores_mp2_group_send(group, a, wire[0], sizes[0])) {
            second_admitted = 1;
            break;
        }
        sched_yield();
    }
    assert(second_admitted);
    wait_atomic(&state.finished[0], 2);
    /* A terminal admission gate rejects fresh producer work without freeing
     * the group or disturbing completed messages and stop/control handling. */
    assert(ores_mp2_group_close_admission(group));
    assert(ores_mp2_group_close_admission(group)); /* idempotent */
    assert(!ores_mp2_group_send(group, a, wire[0], sizes[0]));
    assert(!ores_mp2_group_send(group, b, wire[5], sizes[5]));
    atomic_store(&flood_rejected, 0);
    for (size_t i = 0; i < SENDERS; ++i)
        assert(pthread_create(&flood_threads[i], NULL, saturated_sender, &flood) == 0);
    for (size_t i = 0; i < SENDERS; ++i)
        assert(pthread_join(flood_threads[i], NULL) == 0);
    assert(atomic_load(&flood_rejected) == SENDERS);
    assert(ores_mp2_group_stop(group, a));
    assert(ores_mp2_group_stop(group, b));
    assert(ores_mp2_group_wait_done(group, a));
    assert(ores_mp2_group_wait_done(group, b));
    assert(!ores_mp2_group_send(group, a, wire[0], sizes[0]));
    assert(ores_mp2_group_destroy(group));
    assert(atomic_load(&state.finished[0]) == 2);
    assert(atomic_load(&state.finished[1]) == 1);
    assert(atomic_load(&state.max_active[0]) == 1);
    assert(atomic_load(&state.max_active[1]) == 1);
    puts("MESSAGEPACK_V2_NATIVE_PASS vectors=7 owned=1 scheduler=1 overload=1 independent=1 close_admission=1");
    return 0;
}
