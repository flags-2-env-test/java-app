#include "oreslang/runtime/MessagePackActorV2.h"
#include "oreslang/runtime/NativeActorScheduler.h"
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sched.h>

/* Fail closed: any malformed byte or unsupported shape rejects before a
 * queue slot is reserved. No allocation based on untrusted encoded lengths. */
typedef struct {
    const uint8_t* bytes;
    size_t len, at, nodes;
} Reader;
static int take(Reader* r, size_t n, const uint8_t** out) {
    if (n > r->len - r->at) return 0;
    if (out) *out = r->bytes + r->at;
    r->at += n;
    return 1;
}
static int octet(Reader* r, uint8_t* out) {
    const uint8_t* p;
    if (!take(r, 1, &p)) return 0;
    *out = *p;
    return 1;
}
static int uint_be(Reader* r, unsigned n, uint64_t* out) {
    const uint8_t* p;
    if (n > 8 || !take(r, n, &p)) return 0;
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) v = (v << 8) | p[i];
    *out = v;
    return 1;
}
static int utf8(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n;) {
        uint8_t a = p[i++];
        if (a < 128) continue;
        unsigned extra = 0;
        uint8_t lo = 0x80, hi = 0xbf;
        if (a >= 0xc2 && a <= 0xdf) extra = 1;
        else if (a == 0xe0) { extra = 2; lo = 0xa0; }
        else if (a >= 0xe1 && a <= 0xec) extra = 2;
        else if (a == 0xed) { extra = 2; hi = 0x9f; }
        else if (a >= 0xee && a <= 0xef) extra = 2;
        else if (a == 0xf0) { extra = 3; lo = 0x90; }
        else if (a >= 0xf1 && a <= 0xf3) extra = 3;
        else if (a == 0xf4) { extra = 3; hi = 0x8f; }
        else return 0;
        if (n - i < extra || p[i] < lo || p[i] > hi) return 0;
        ++i;
        for (unsigned j = 1; j < extra; ++j, ++i)
            if (p[i] < 0x80 || p[i] > 0xbf) return 0;
    }
    return 1;
}
static int string(Reader* r, const uint8_t** data, size_t* len) {
    uint8_t t;
    uint64_t n = 0;
    if (!octet(r, &t)) return 0;
    if ((t & 0xe0) == 0xa0) n = t & 31;
    else if (t == 0xd9) {
        if (!uint_be(r, 1, &n) || n <= 31) return 0;
    } else if (t == 0xda) {
        if (!uint_be(r, 2, &n) || n <= 255) return 0;
    } else if (t == 0xdb) {
        if (!uint_be(r, 4, &n) || n <= 65535) return 0;
    } else return 0;
    if (n > ORES_MP2_MAX_BYTES || !take(r, (size_t)n, data)
            || !utf8(*data, (size_t)n)) return 0;
    *len = (size_t)n;
    return 1;
}
static int array_header(Reader* r, size_t* out) {
    uint8_t t;
    uint64_t n = 0;
    if (!octet(r, &t)) return 0;
    if ((t & 0xf0) == 0x90) n = t & 15;
    else if (t == 0xdc) {
        if (!uint_be(r, 2, &n) || n <= 15) return 0;
    } else if (t == 0xdd) {
        if (!uint_be(r, 4, &n) || n <= 65535) return 0;
    } else return 0;
    if (n > ORES_MP2_MAX_NODES) return 0;
    *out = (size_t)n;
    return 1;
}
static int signed_integer(Reader* r) {
    uint8_t t;
    uint64_t bits = 0;
    if (!octet(r, &t)) return 0;
    if (t <= 0x7f || t >= 0xe0) return 1;
    unsigned width;
    switch (t) {
        case 0xcc: case 0xd0: width = 1; break;
        case 0xcd: case 0xd1: width = 2; break;
        case 0xce: case 0xd2: width = 4; break;
        case 0xcf: case 0xd3: width = 8; break;
        default: return 0;
    }
    if (!uint_be(r, width, &bits)) return 0;
    if (t == 0xcc) return bits > 127;
    if (t == 0xcd) return bits > 255;
    if (t == 0xce) return bits > 65535;
    if (t == 0xcf) return bits > UINT64_C(0xffffffff) && bits <= INT64_MAX;
    /* Signed negative classes; positive values must use unsigned forms. */
    int64_t number;
    if (width == 1) number = (int8_t)bits;
    else if (width == 2) number = (int16_t)bits;
    else if (width == 4) number = (int32_t)bits;
    else number = (int64_t)bits;
    if (t == 0xd0) return number < -32;
    if (t == 0xd1) return number < INT8_MIN;
    if (t == 0xd2) return number < INT16_MIN;
    return number < INT32_MIN;
}
/* Only compiler-generated canonical descriptors are admissible. Parser is
 * bounded separately from the MessagePack value to avoid recursion attacks. */
static int valid_shape(const char** s, unsigned depth) {
    if (depth > ORES_MP2_MAX_DEPTH) return 0;
    char t = **s;
    if (t == 'B' || t == 'I' || t == 'S' ||
        (t == 'F' && !(!strncmp(*s, "FA[", 3) || !strncmp(*s, "FL[", 3)))) {
        ++*s; return 1;
    }
    if (!strncmp(*s, "FA[", 3) || !strncmp(*s, "FL[", 3))
        *s += 3;
    else if (t == 'T' && (*s)[1] == '[') *s += 2;
    else if ((t == 'A' || t == 'L') && (*s)[1] == '[') {
        *s += 2;
        if (!valid_shape(s, depth + 1) || **s != ']') return 0;
        ++*s; return 1;
    } else return 0;
    if (**s == ']') { ++*s; return 1; }
    for (;;) {
        if (!valid_shape(s, depth + 1)) return 0;
        if (**s == ']') { ++*s; return 1; }
        if (**s != ',') return 0;
        ++*s;
    }
}
static int typed_value(Reader* r, const char** shape, unsigned depth) {
    if (depth > ORES_MP2_MAX_DEPTH || ++r->nodes > ORES_MP2_MAX_NODES)
        return 0;
    char t = **shape;
    if (t == 'B') {
        uint8_t b; ++*shape;
        return octet(r, &b) && (b == 0xc2 || b == 0xc3);
    }
    if (t == 'I') { ++*shape; return signed_integer(r); }
    if (t == 'F' && strncmp(*shape, "FA[", 3) && strncmp(*shape, "FL[", 3)) {
        uint8_t b; uint64_t bits;
        ++*shape;
        if (!octet(r, &b) || b != 0xcb || !uint_be(r, 8, &bits)) return 0;
        double number;
        memcpy(&number, &bits, 8);
        return isfinite(number);
    }
    if (t == 'S') {
        const uint8_t* data; size_t n;
        ++*shape; return string(r, &data, &n);
    }
    int fixed = 0;
    if (!strncmp(*shape, "FA[", 3) || !strncmp(*shape, "FL[", 3)) {
        *shape += 3; fixed = 1;
    } else if (t == 'T' && (*shape)[1] == '[') {
        *shape += 2; fixed = 1;
    } else if ((t == 'A' || t == 'L') && (*shape)[1] == '[') {
        *shape += 2;
    } else return 0;
    size_t count;
    if (!array_header(r, &count)
            || count > r->len - r->at
            || count > ORES_MP2_MAX_NODES - r->nodes) return 0;
    if (fixed) {
        for (size_t i = 0; i < count; ++i) {
            if (i > 0 && *(*shape)++ != ',') return 0;
            if (!typed_value(r, shape, depth + 1)) return 0;
        }
        if (**shape != ']') return 0;
        ++*shape;
        return 1;
    }
    const char* one_shape = *shape;
    const char* end_shape = one_shape;
    if (!valid_shape(&end_shape, depth + 1) || *end_shape != ']') return 0;
    for (size_t i = 0; i < count; ++i) {
        const char* s = one_shape;
        if (!typed_value(r, &s, depth + 1) || s != end_shape) return 0;
    }
    *shape = end_shape + 1;
    return 1;
}
/*
 * Independent SHA-256 over the same canonical shape descriptor that Java's
 * MessagePackCollectionCodec.shapeDigest hashes. Neither a wire-supplied hash
 * nor a host-supplied digest can redefine the shape of a contract.
 * Input is bounded to 4096 ASCII grammar bytes, and all arithmetic is
 * bounded before forming the padded SHA-256 message.
 */
static uint32_t rotr32(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}
static const uint32_t SHA256_K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,
    0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,
    0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,
    0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,
    0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,
    0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};
static int canonical_shape_sha256(const char* shape, size_t n,
                                  uint8_t digest[32]) {
    static const char domain[] = "oreslang.actor.shape.v2/";
    enum { SHA_BUFFER_CAP = 4608 };
    if (!shape || n > 4096) return 0;
    size_t len = sizeof(domain) - 1 + n;
    size_t padded = (len + 9 + 63) & ~(size_t)63;
    if (padded > SHA_BUFFER_CAP) return 0;
    uint8_t buf[SHA_BUFFER_CAP] = {0};
    memcpy(buf, domain, sizeof(domain) - 1);
    memcpy(buf + sizeof(domain) - 1, shape, n);
    buf[len] = 0x80;
    uint64_t bit_length = (uint64_t)len * 8;
    for (int i = 0; i < 8; ++i)
        buf[padded - 1 - i] = (uint8_t)(bit_length >> (8 * i));
    uint32_t state[8] = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
    };
    for (size_t offset = 0; offset < padded; offset += 64) {
        uint32_t w[64];
        for (unsigned i = 0; i < 16; ++i) {
            const uint8_t* p = buf + offset + 4 * i;
            w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
                 | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
        }
        for (unsigned i = 16; i < 64; ++i) {
            uint32_t x = w[i - 15], y = w[i - 2];
            uint32_t s0 = rotr32(x,7) ^ rotr32(x,18) ^ (x >> 3);
            uint32_t s1 = rotr32(y,17) ^ rotr32(y,19) ^ (y >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=state[0],b=state[1],c=state[2],d=state[3];
        uint32_t e=state[4],f=state[5],g=state[6],h=state[7];
        for (unsigned i = 0; i < 64; ++i) {
            uint32_t S1=rotr32(e,6)^rotr32(e,11)^rotr32(e,25);
            uint32_t ch=(e&f)^(~e&g);
            uint32_t t1=h+S1+ch+SHA256_K[i]+w[i];
            uint32_t S0=rotr32(a,2)^rotr32(a,13)^rotr32(a,22);
            uint32_t maj=(a&b)^(a&c)^(b&c);
            uint32_t t2=S0+maj;
            h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;
        state[4]+=e;state[5]+=f;state[6]+=g;state[7]+=h;
    }
    for (unsigned i = 0; i < 8; ++i) {
        digest[4*i]=(uint8_t)(state[i]>>24);
        digest[4*i+1]=(uint8_t)(state[i]>>16);
        digest[4*i+2]=(uint8_t)(state[i]>>8);
        digest[4*i+3]=(uint8_t)(state[i]);
    }
    return 1;
}
int ores_mp2_contract_valid(const OresMp2Contract* c) {
    if (!c || !c->schema_id || !c->shape || !c->shape_sha256) return 0;
    size_t schema_len = 0;
    while (schema_len <= 256 && c->schema_id[schema_len]) ++schema_len;
    if (!schema_len || schema_len > 256) return 0;
    for (size_t i = 0; i < schema_len; ++i) {
        unsigned char ch = (unsigned char)c->schema_id[i];
        if (ch < 33 || ch > 126) return 0;
    }
    size_t shape_len = 0;
    while (shape_len <= 4096 && c->shape[shape_len]) ++shape_len;
    if (!shape_len || shape_len > 4096) return 0;
    const char* parsed = c->shape;
    if (!valid_shape(&parsed, 0) || *parsed != '\0') return 0;
    uint8_t digest[32];
    return canonical_shape_sha256(c->shape, shape_len, digest)
           && memcmp(digest, c->shape_sha256, 32) == 0;
}
/* The type-checking parser operates ONLY on an already-owned immutable
 * snapshot. Source actor routing uses it after copying incoming wire bytes
 * and before touching any guest callback or actor-owned storage. */
int ores_mp2_validate_owned(const OresMp2Contract* c,
                            const OresMp2Owned* owned) {
    if (!owned || !owned->bytes || owned->size < 40
            || owned->size > ORES_MP2_MAX_BYTES
            || !ores_mp2_contract_valid(c)) return 0;
    const size_t schema_len = strlen(c->schema_id);
    Reader r = {owned->bytes, owned->size, 0, 0};
    size_t n;
    uint8_t version;
    const uint8_t *schema, *digest;
    size_t actual_schema_len;
    int valid = array_header(&r, &n) && n == 4
            && octet(&r, &version) && version == 2
            && string(&r, &schema, &actual_schema_len)
            && actual_schema_len == schema_len
            && memcmp(schema, c->schema_id, schema_len) == 0
            && octet(&r, &version) && version == 0xc4
            && octet(&r, &version) && version == 32
            && take(&r, 32, &digest)
            && memcmp(digest, c->shape_sha256, 32) == 0;
    const char* shape = c->shape;
    if (valid) valid = typed_value(&r, &shape, 0)
                         && *shape == 0 && r.at == owned->size;
    return valid;
}
/* A nominal Java enum case carries its *owner* and numeric tag inside
 * the typed tuple. The SHA-256 tuple descriptor only verifies 'S,I,...';
 * it cannot distinguish [Status,2,...] from [Status,3,...].
 * Perform an independent shallow match AFTER the full, owned, typed decode.
 * No caller-owned pointer, heap allocation or recursive re-decode is used. */
int ores_mp2_tagged_identity_matches_validated(
    const OresMp2Contract* c, const OresMp2Owned* frame,
    const char* owner, uint32_t declared_tag) {
    if (!c || !frame || !frame->bytes || !c->shape || !owner
            || declared_tag == 0 || declared_tag > INT32_MAX
            || frame->size > ORES_MP2_MAX_BYTES || frame->size < 40)
        return 0;
    if (strncmp(c->shape, "T[S,I", 5) != 0
            || (c->shape[5] != ']' && c->shape[5] != ','))
        return 0;
    const size_t arity = c->shape[5] == ']' ? 2 : 3;
    size_t owner_len = 0;
    while (owner_len <= 256 && owner[owner_len]) ++owner_len;
    if (!owner_len || owner_len > 256) return 0;
    Reader r = {frame->bytes, frame->size, 0, 0};
    size_t n, schema_len, actual_owner_len;
    uint8_t version, kind;
    const uint8_t* schema;
    const uint8_t* digest;
    const uint8_t* actual_owner;
    if (!array_header(&r, &n) || n != 4
            || !octet(&r, &version) || version != 2
            || !string(&r, &schema, &schema_len)
            || !octet(&r, &kind) || kind != 0xc4
            || !octet(&r, &kind) || kind != 32
            || !take(&r, 32, &digest)
            || !array_header(&r, &n) || n != arity
            || !string(&r, &actual_owner, &actual_owner_len)
            || actual_owner_len != owner_len
            || memcmp(actual_owner, owner, owner_len) != 0)
        return 0;
    /* Canonical MessagePack integer classes supported by Java's positive
     * signed-32 enum tag encoder. Wider, negative, or non-minimal values are
     * never valid declared enum tags even though the scalar I shape can
     * represent them. */
    uint8_t tag_class;
    uint64_t number;
    if (!octet(&r, &tag_class)) return 0;
    if (tag_class >= 1 && tag_class <= 127) number = tag_class;
    else if (tag_class == 0xcc) {
        if (!uint_be(&r, 1, &number) || number <= 127) return 0;
    } else if (tag_class == 0xcd) {
        if (!uint_be(&r, 2, &number) || number <= 255) return 0;
    } else if (tag_class == 0xce) {
        if (!uint_be(&r, 4, &number)
                || number <= 65535 || number > INT32_MAX) return 0;
    } else return 0;
    return number == declared_tag;
}
/* This helper may only consume wire bytes from a completed, pinned
 * nominal contract validation. It never accepts an arbitrary schema in
 * place of the registration gate. Re-check canonical scalar encodings and
 * the complete outer layout to avoid making a guest binder from unchecked
 * offsets. No heap allocation, recursion or retained pointer. */
static int read_trusted_i64(Reader* r, int64_t* result) {
    const size_t previous = r->at;
    if (!signed_integer(r)) return 0;
    Reader value = *r;
    value.at = previous;
    uint8_t t;
    uint64_t bits = 0;
    if (!octet(&value, &t)) return 0;
    if (t <= 0x7f) *result = (int64_t)t;
    else if (t >= 0xe0) *result = (int64_t)(int8_t)t;
    else {
        unsigned width = 0;
        switch (t) {
            case 0xcc: case 0xd0: width = 1; break;
            case 0xcd: case 0xd1: width = 2; break;
            case 0xce: case 0xd2: width = 4; break;
            case 0xcf: case 0xd3: width = 8; break;
            default: return 0;
        }
        if (!uint_be(&value, width, &bits)) return 0;
        if (t == 0xd0) *result = (int8_t)bits;
        else if (t == 0xd1) *result = (int16_t)bits;
        else if (t == 0xd2) *result = (int32_t)bits;
        else if (t == 0xd3) memcpy(result, &bits, sizeof(bits));
        else *result = (int64_t)bits;
    }
    return value.at == r->at;
}
int ores_mp2_extract_tagged_i64_trusted(const uint8_t* wire, size_t size,
                                        int64_t* out_payload) {
    if (!wire || !out_payload || size < 40 || size > ORES_MP2_MAX_BYTES)
        return 0;
    Reader r = {wire, size, 0, 0};
    size_t n, schema_len, owner_len;
    uint8_t version, marker;
    const uint8_t *schema, *owner, *digest;
    int64_t tag, payload;
    if (!array_header(&r, &n) || n != 4
            || !octet(&r, &version) || version != 2
            || !string(&r, &schema, &schema_len)
            || !octet(&r, &marker) || marker != 0xc4
            || !octet(&r, &marker) || marker != 32
            || !take(&r, 32, &digest)
            || !array_header(&r, &n) || n != 3
            || !string(&r, &owner, &owner_len)
            || !read_trusted_i64(&r, &tag)
            || tag < 1 || tag > INT32_MAX
            || !read_trusted_i64(&r, &payload)
            || r.at != size)
        return 0;
    *out_payload = payload;
    return 1;
}
int ores_mp2_validate_copy(const OresMp2Contract* c,
                           const uint8_t* bytes, size_t size,
                           OresMp2Owned* out) {
    if (!bytes || !out || out->bytes || out->size
            || size > ORES_MP2_MAX_BYTES || size < 40) return 0;
    /* Never validate caller memory then enqueue a later independent read:
     * copied bytes are the sole source of routing and type information. */
    OresMp2Owned snapshot = {malloc(size), size};
    if (!snapshot.bytes) return 0;
    memcpy(snapshot.bytes, bytes, size);
    if (!ores_mp2_validate_owned(c, &snapshot)) {
        ores_mp2_owned_release(&snapshot);
        return 0;
    }
    *out = snapshot;
    return 1;
}
void ores_mp2_owned_release(OresMp2Owned* v) {
    if (!v) return;
    if (v->bytes) {
        volatile uint8_t* p = (volatile uint8_t*)v->bytes;
        for (size_t i = 0; i < v->size; ++i) p[i] = 0;
        free(v->bytes);
    }
    v->bytes = NULL; v->size = 0;
}

/*
 * An actor contract cannot borrow an arbitrary host buffer whose bytes may
 * change or be freed after registration. Copy schema, shape and digest once.
 * These slots are configured before actor spawn and become read-only.
 */
typedef struct {
    OresMp2Contract value;
    char* owned_schema;
    char* owned_shape;
    uint8_t owned_sha[32];
} OwnedContract;
struct OresMp2ActorGroup {
    OresNativeActorScheduler* scheduler;
    OwnedContract* contracts;
    unsigned char* spawned;
    atomic_size_t* reserved; /* active + queued + preparing per actor */
    /* One atomic word makes admission-close and producer-entry indivisible:
     * high bit = terminal close, low bits = API send calls still preparing.
     * This fences admitted in-flight callers; external caller-thread joining
     * is STILL required before freeing the opaque group pointer. */
    atomic_uint_fast64_t producer_gate;
    size_t actors;
    size_t capacity;
    OresMp2OnMessage callback;
    void* callback_context;
};
/* Scheduler's DATA callback holds the only reference to the admitted owned
 * packet. After user callback returns, its memory is zeroized and released.
 * CONTROL continues to have an independent reserved slot in the scheduler. */
static void deliver(void* raw, size_t slot, uint64_t token) {
    OresMp2ActorGroup* group = raw;
    OresMp2Owned* owned = (OresMp2Owned*)(uintptr_t)token;
    if (!owned || !owned->bytes || slot >= group->actors) abort();
    group->callback(group->callback_context, slot, owned->bytes, owned->size);
    ores_mp2_owned_release(owned);
    free(owned);
    /* Admission reservations cover the entire accepted DATA turn and are
     * released only after actor user code can no longer access this frame. */
    size_t prior = atomic_fetch_sub_explicit(&group->reserved[slot], 1,
                                             memory_order_acq_rel);
    if (prior == 0) abort();
}
static OresMp2ActorRef wrap(OresNativeActorRef r) {
    OresMp2ActorRef result = {r.scheduler_domain, r.actor_slot, r.generation};
    return result;
}
static OresNativeActorRef unwrap(OresMp2ActorRef r) {
    OresNativeActorRef result = {r.scheduler_domain, r.actor_slot, r.generation};
    return result;
}
OresMp2ActorGroup* ores_mp2_group_open(size_t actors, size_t capacity,
                                      size_t carriers, OresMp2OnMessage callback,
                                      void* context) {
    if (!callback || sizeof(uintptr_t) != sizeof(uint64_t) || !actors) return NULL;
    OresMp2ActorGroup* g = calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->contracts = calloc(actors, sizeof(*g->contracts));
    g->spawned = calloc(actors, 1);
    g->reserved = calloc(actors, sizeof(*g->reserved));
    if (!g->contracts || !g->spawned || !g->reserved) {
        free(g->reserved); free(g->spawned); free(g->contracts); free(g);
        return NULL;
    }
    for (size_t i = 0; i < actors; ++i) atomic_init(&g->reserved[i], 0);
    atomic_init(&g->producer_gate, 0);
    g->actors = actors; g->capacity = capacity;
    g->callback = callback; g->callback_context = context;
    g->scheduler = ores_native_actor_scheduler_open(actors, capacity, carriers,
                                                    deliver, g);
    if (!g->scheduler) {
        free(g->reserved); free(g->spawned); free(g->contracts); free(g);
        return NULL;
    }
    return g;
}
static size_t bounded_len(const char* text, size_t limit) {
    if (!text) return limit + 1;
    for (size_t i = 0; i <= limit; ++i)
        if (text[i] == 0) return i;
    return limit + 1;
}
int ores_mp2_group_contract(OresMp2ActorGroup* g, size_t slot,
                            OresMp2Contract contract) {
    if (!g || slot >= g->actors || g->contracts[slot].value.schema_id
            || !contract.schema_id || !contract.shape || !contract.shape_sha256
            || g->spawned[slot]) return 0;
    /* Registration is a trusted, single-threaded construction operation:
     * callers MUST NOT spawn or submit concurrently with registration.
     */
    size_t schema_len = bounded_len(contract.schema_id, 256);
    size_t shape_len = bounded_len(contract.shape, 4096);
    if (!schema_len || schema_len > 256 || !shape_len || shape_len > 4096)
        return 0;
    char* schema = malloc(schema_len + 1);
    char* shape = malloc(shape_len + 1);
    if (!schema || !shape) {
        free(schema); free(shape);
        return 0;
    }
    memcpy(schema, contract.schema_id, schema_len + 1);
    memcpy(shape, contract.shape, shape_len + 1);
    uint8_t pinned_sha[32];
    memcpy(pinned_sha, contract.shape_sha256, sizeof(pinned_sha));
    OresMp2Contract copied = {schema, shape, pinned_sha};
    if (!ores_mp2_contract_valid(&copied)) {
        free(schema);
        free(shape);
        memset(pinned_sha, 0, sizeof(pinned_sha));
        return 0;
    }
    OwnedContract* owner = &g->contracts[slot];
    memcpy(owner->owned_sha, pinned_sha, sizeof(pinned_sha));
    memset(pinned_sha, 0, sizeof(pinned_sha));
    owner->owned_schema = schema;
    owner->owned_shape = shape;
    owner->value = (OresMp2Contract){schema, shape, owner->owned_sha};
    return 1;
}
OresMp2ActorRef ores_mp2_group_spawn(OresMp2ActorGroup* g, size_t slot) {
    OresMp2ActorRef empty = {0, 0, 0};
    if (!g || slot >= g->actors || !g->contracts[slot].value.schema_id || g->spawned[slot]) return empty;
    OresMp2ActorRef ref = wrap(ores_native_actor_scheduler_spawn(g->scheduler, slot));
    if (ref.generation) g->spawned[slot] = 1;
    return ref;
}
#define MP2_GATE_CLOSED (UINT64_C(1) << 63)
#define MP2_GATE_COUNT (MP2_GATE_CLOSED - 1)
static int enter_producer(OresMp2ActorGroup* g) {
    uint_fast64_t state = atomic_load_explicit(
        &g->producer_gate, memory_order_acquire);
    for (;;) {
        if ((state & MP2_GATE_CLOSED) || (state & MP2_GATE_COUNT) == MP2_GATE_COUNT)
            return 0;
        if (atomic_compare_exchange_weak_explicit(
                &g->producer_gate, &state, state + 1,
                memory_order_acq_rel, memory_order_acquire)) return 1;
    }
}
static void leave_producer(OresMp2ActorGroup* g) {
    uint_fast64_t prior = atomic_fetch_sub_explicit(
        &g->producer_gate, 1, memory_order_acq_rel);
    if ((prior & MP2_GATE_COUNT) == 0) abort();
}
int ores_mp2_group_close_admission(OresMp2ActorGroup* g) {
    if (!g) return 0;
    atomic_fetch_or_explicit(&g->producer_gate, MP2_GATE_CLOSED,
                             memory_order_acq_rel);
    while ((atomic_load_explicit(&g->producer_gate, memory_order_acquire)
            & MP2_GATE_COUNT) != 0) {
        /* Only a terminal shutdown barrier; no per-message polling. */
        sched_yield();
    }
    return 1;
}
static int send_under_producer_lease(OresMp2ActorGroup* g,
                                     OresMp2ActorRef target,
                                     const uint8_t* wire, size_t size) {
    if (!g || target.actor_slot >= g->actors
            || !g->contracts[target.actor_slot].value.schema_id) return 0;
    /* Reserve a bounded DATA slot BEFORE any allocation/hash/recursive
     * decode. This prevents a flood of concurrent rejected 1 MiB sends from
     * creating unbounded in-flight snapshots while the mailbox is saturated.
     */
    atomic_size_t* admission = &g->reserved[target.actor_slot];
    size_t current = atomic_load_explicit(admission, memory_order_relaxed);
    do {
        if (current >= g->capacity) return 0;
    } while (!atomic_compare_exchange_weak_explicit(
        admission, &current, current + 1, memory_order_acq_rel,
        memory_order_relaxed));
    OresMp2Owned* snapshot = calloc(1, sizeof(*snapshot));
    if (!snapshot) {
        atomic_fetch_sub_explicit(admission, 1, memory_order_acq_rel);
        return 0;
    }
    if (!ores_mp2_validate_copy(&g->contracts[target.actor_slot].value,
                                wire, size, snapshot)) {
        free(snapshot);
        atomic_fetch_sub_explicit(admission, 1, memory_order_acq_rel);
        return 0;
    }
    if (!ores_native_actor_scheduler_send(g->scheduler, unwrap(target),
                                          (uint64_t)(uintptr_t)snapshot)) {
        ores_mp2_owned_release(snapshot);
        free(snapshot);
        atomic_fetch_sub_explicit(admission, 1, memory_order_acq_rel);
        return 0;
    }
    /* A successful send transfers its reservation and frame to DATA deliver. */
    return 1;
}
int ores_mp2_group_send(OresMp2ActorGroup* g, OresMp2ActorRef target,
                        const uint8_t* wire, size_t size) {
    if (!g || !enter_producer(g)) return 0;
    const int accepted = send_under_producer_lease(g, target, wire, size);
    leave_producer(g);
    return accepted;
}
int ores_mp2_group_stop(OresMp2ActorGroup* g, OresMp2ActorRef target) {
    return g && ores_native_actor_scheduler_stop(g->scheduler, unwrap(target));
}
int ores_mp2_group_wait_done(OresMp2ActorGroup* g, OresMp2ActorRef target) {
    return g && ores_native_actor_scheduler_wait_done(g->scheduler, unwrap(target));
}
int ores_mp2_group_destroy(OresMp2ActorGroup* g) {
    if (!g) return 0;
    /* After this closure no new producer can hold an admission lease, and
     * all send calls admitted before closure have returned their leases.
     * Joining threads that might call the group later remains a host duty. */
    if (!ores_mp2_group_close_admission(g)) return 0;
    ores_native_actor_scheduler_retire(g->scheduler);
    if (!ores_native_actor_scheduler_drain(g->scheduler)
            || !ores_native_actor_scheduler_try_destroy(g->scheduler)) return 0;
    for (size_t i = 0; i < g->actors; ++i) {
        free(g->contracts[i].owned_schema);
        free(g->contracts[i].owned_shape);
    }
    free(g->reserved);
    free(g->spawned);
    free(g->contracts);
    free(g);
    return 1;
}
