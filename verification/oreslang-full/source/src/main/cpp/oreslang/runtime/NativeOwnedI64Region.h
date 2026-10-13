#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* No tracing GC, no reference counting, and no cross-actor ownership.
 * This is a native lexical ownership domain: region lifetime is one
 * compiled function activation until actor-lifetime integration lands. */
typedef struct OresOwnedI64Region OresOwnedI64Region;

OresOwnedI64Region* oreslang_owned_i64_region_open(uint64_t slots);
int64_t* oreslang_owned_i64_region_new(OresOwnedI64Region* region, int64_t value);
void oreslang_owned_i64_region_drop(OresOwnedI64Region* region, int64_t* slot);
void oreslang_owned_i64_region_close(OresOwnedI64Region* region);

#ifdef __cplusplus
}
#endif
