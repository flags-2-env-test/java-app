#include "oreslang/runtime/SourceActorBootstrap.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static _Atomic int deliveries = 0;
static uint8_t canonical[512];
static size_t nbytes;
static uint8_t nibble(char c) {
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    abort();
}
static size_t from_hex(const char* hex, uint8_t* out) {
    size_t n = strlen(hex);
    assert(n % 2 == 0 && n / 2 <= 512);
    for (size_t i = 0; i < n/2; i++)
        out[i] = (uint8_t)((nibble(hex[2*i]) << 4) | nibble(hex[2*i+1]));
    return n/2;
}
static size_t owner_pos(const uint8_t* frame, size_t size) {
    static const char owner[] = "Status";
    for (size_t i = 1; i + sizeof(owner) < size; ++i)
        if (frame[i-1] == 0xa6 && memcmp(frame+i, owner, 6) == 0)
            return i;
    abort();
}
static void init(OresActorI64Heap* heap, uint64_t d, uint64_t e) {
    ores_actor_i64_heap_field_init_mutable(heap, d, e, 0, 0);
}
static int64_t receive(OresActorI64Heap* heap, uint64_t d, uint64_t e,
                       size_t variant, const uint8_t* wire, size_t size) {
    assert(variant == 0);
    assert(size == nbytes && memcmp(wire, canonical, size) == 0);
    atomic_fetch_add(&deliveries, 1);
    int64_t count = ores_actor_i64_heap_field_read(heap, d, e, 0);
    ores_actor_i64_heap_field_write(heap, d, e, 0, count+1);
    return count+1;
}
int main(void) {
    static const char hex[] =
        "9402d98f6f7265736c616e672e656e756d2e363461373862626130343533393535323739306336633537666561306630"
        "353337346338373864623836313134393130356261366631373332633837326539342e34626264346332316663303037"
        "643839636136386637663061396136313438666162313266376539313361353931636633626131383033333061353931"
        "383634c420085917a950bf1470f59d654358e5da6f1744d6eadd4c4128f334ddbb6e032ddb93a653746174757302a568"
        "656c6c6f";
    nbytes = from_hex(hex, canonical);
    const char* schema = "oreslang.enum.64a78bba04539552790c6c57fea0f05374c878db861149105ba6f1732c872e94.4bbd4c21fc007d89ca68f7f0a9a6148fab12f7e913a591cf3ba180330a591864";
    const char* shape = "T[S,I,S]";
    uint8_t digest[32];
    size_t digest_pos = 3 + 1 + strlen(schema);
    assert(canonical[digest_pos] == 0xc4 && canonical[digest_pos+1] == 32);
    memcpy(digest, canonical+digest_pos+2, 32);
    OresMp2Contract c = {schema, shape, digest};
    assert(ores_mp2_contract_valid(&c));
    assert(!oreslang_source_actor_spawn_tagged_variants(
        init, receive, 1, &c, NULL, 1));
    OresSourceNominalCase zero = {"Status", 0};
    assert(!oreslang_source_actor_spawn_tagged_variants(
        init, receive, 1, &c, &zero, 1));
    OresSourceNominalCase forged_owner = {"WrongOwner", 2};
    void* denied = oreslang_source_actor_spawn_tagged_variants(
        init, receive, 1, &c, &forged_owner, 1);
    assert(denied); /* Valid declaration: mismatch must reject at send. */
    assert(!oreslang_source_actor_send_variant(denied, canonical, nbytes));
    oreslang_source_actor_stop(denied);
    char owner_mutable[] = "Status";
    OresSourceNominalCase input_identity = {owner_mutable, 2};
    void* actor = oreslang_source_actor_spawn_tagged_variants(
        init, receive, 1, &c, &input_identity, 1);
    assert(actor);
    memset(owner_mutable, 'x', strlen(owner_mutable));
    uint8_t frame[512];
    memcpy(frame, canonical, nbytes);
    size_t owner = owner_pos(frame, nbytes);
    frame[owner] = 'T'; /* Structurally valid S; wrong nominal enum owner. */
    OresMp2Owned snapshot = {0};
    assert(ores_mp2_validate_copy(&c, frame, nbytes, &snapshot));
    assert(!ores_mp2_tagged_identity_matches_validated(
        &c, &snapshot, "Status", 2));
    ores_mp2_owned_release(&snapshot);
    assert(!oreslang_source_actor_send_variant(actor, frame, nbytes));
    memcpy(frame, canonical, nbytes);
    assert(frame[owner+6] == 2);
    frame[owner+6] = 3; /* Structurally valid I; wrong declared wire tag. */
    assert(ores_mp2_validate_copy(&c, frame, nbytes, &snapshot));
    assert(!ores_mp2_tagged_identity_matches_validated(
        &c, &snapshot, "Status", 2));
    ores_mp2_owned_release(&snapshot);
    assert(!oreslang_source_actor_send_variant(actor, frame, nbytes));
    memcpy(frame, canonical, nbytes);
    assert(oreslang_source_actor_send_variant(actor, frame, nbytes));
    memset(frame, 0, nbytes);
    oreslang_source_actor_stop(actor);
    assert(atomic_load(&deliveries) == 1);
    puts("NOMINAL_ENUM_WIRE_GUARD_PASS forged_owner=1 forged_tag=1 copy=1");
    return 0;
}
