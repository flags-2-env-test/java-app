#include "oreslang/runtime/MessagePackActorV2.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char schema[] = "oreslang.enum.64a78bba04539552790c6c57fea0f05374c878db861149105ba6f1732c872e94.4bbd4c21fc007d89ca68f7f0a9a6148fab12f7e913a591cf3ba180330a591864";
static const char shape[] = "T[S,I,S]";
static const char wire_hex[] = "9402d98f6f7265736c616e672e656e756d2e363461373862626130343533393535323739306336633537666561306630353337346338373864623836313134393130356261366631373332633837326539342e34626264346332316663303037643839636136386637663061396136313438666162313266376539313361353931636633626131383033333061353931383634c420085917a950bf1470f59d654358e5da6f1744d6eadd4c4128f334ddbb6e032ddb93a653746174757302a568656c6c6f";
static unsigned digit(char n) {
    if (n >= '0' && n <= '9') return (unsigned)(n - '0');
    if (n >= 'a' && n <= 'f') return (unsigned)(10 + n - 'a');
    abort();
}
int main(void) {
    size_t n = strlen(wire_hex) / 2;
    assert(strlen(wire_hex) % 2 == 0 && n < 2048);
    uint8_t bytes[2048] = {0};
    for (size_t i = 0; i < n; ++i)
        bytes[i] = (uint8_t)(digit(wire_hex[2*i]) * 16 + digit(wire_hex[2*i+1]));
    /* MessagePack [2, schema_id, digest, payload]. The 143-byte schema
     * uses canonical str8 encoding. SHA is independently recomputed below.
     */
    const size_t digest_at = 4 + strlen(schema);
    assert(bytes[0] == 0x94 && bytes[1] == 2 &&
           bytes[2] == 0xd9 && bytes[3] == (uint8_t)strlen(schema));
    assert(bytes[digest_at] == 0xc4 && bytes[digest_at+1] == 32);
    uint8_t expected_digest[32];
    memcpy(expected_digest, bytes + digest_at + 2, 32);
    OresMp2Contract contract = {schema, shape, expected_digest};
    assert(ores_mp2_contract_valid(&contract));
    OresMp2Owned owned = {0};
    assert(ores_mp2_validate_copy(&contract, bytes, n, &owned));
    assert(owned.size == n && memcmp(owned.bytes, bytes, n) == 0);
    bytes[n-1] ^= 1;
    assert(memcmp(owned.bytes, bytes, n) != 0);
    bytes[n-1] ^= 1;
    ores_mp2_owned_release(&owned);
    bytes[digest_at + 2] ^= 1;
    assert(!ores_mp2_validate_copy(&contract, bytes, n, &owned));
    bytes[digest_at + 2] ^= 1;
    uint8_t forged[32];
    memcpy(forged, expected_digest, 32);
    forged[31] ^= 1;
    OresMp2Contract false_contract = {schema, shape, forged};
    assert(!ores_mp2_contract_valid(&false_contract));
    assert(!ores_mp2_validate_copy(&false_contract, bytes, n, &owned));
    assert(!ores_mp2_validate_copy(&contract, bytes, n-1, &owned));
    assert(!owned.bytes && !owned.size);
    puts("NATIVE_TAGGED_MESSAGEPACK_V2_PASS java_wire_exact=1 shape=tuple_string_int_string");
    return 0;
}
