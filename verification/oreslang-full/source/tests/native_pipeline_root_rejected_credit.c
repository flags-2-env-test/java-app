/* Regression: a malformed root attempt must not reserve a single-flow
 * pipeline credit. Build with an actual compiler-generated .ores guest and
 * GNU ld --wrap at the trusted-main pipeline registration boundary.
 */
#include "oreslang/runtime/SourceActorBootstrap.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static int tested;
void __real_oreslang_source_actor_pipeline_connect_many(
    void* const* stages, uint64_t count);
void __wrap_oreslang_source_actor_pipeline_connect_many(
    void* const* stages, uint64_t count) {
    assert(stages && count == 2 && !tested);
    __real_oreslang_source_actor_pipeline_connect_many(stages, count);
    /* Previously source_send_binary_leased() claimed pipeline_root_sent
     * before a frame could pass the minimum-length check; this malformed
     * candidate then poisoned every subsequent valid guest root send. */
    const uint8_t malformed[] = {0x94, 0x02, 0xa0};
    assert(!oreslang_source_actor_send_variant(
        stages[0], malformed, sizeof(malformed)));
    assert(!oreslang_source_actor_send_variant(stages[0], NULL, 0));
    tested = 1;
}
__attribute__((destructor))
static void verify_admission_regression(void) {
    if (!tested) {
        fputs("SOURCE_ROOT_REJECT_CREDIT_NOT_EXERCISED\n", stderr);
        abort();
    }
    fputs("SOURCE_ROOT_REJECT_CREDIT_PASS malformed=2 valid_roundtrip=1\n", stderr);
}
