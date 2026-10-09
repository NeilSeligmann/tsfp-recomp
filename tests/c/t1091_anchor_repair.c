/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Verification-only declared snapshot boundary inputs; production is unchanged. */
#define main completion_baseline_main
#define dsound_stream_reset_checked verification_end
#include "test_dsound_completion.c"
#undef main
#undef dsound_stream_reset_checked

static unsigned snapshot_mode;
bool __real_dsound_stream_get_snapshot(uint32_t stream, dsound_stream_snapshot *out);
bool __real_dsound_buffer_get_snapshot(uint32_t buffer, dsound_buffer_snapshot *out);
bool dsound_stream_reset_checked(void);
bool __wrap_dsound_stream_get_snapshot(uint32_t stream, dsound_stream_snapshot *out)
{
    if (!__real_dsound_stream_get_snapshot(stream, out)) return false;
    if (snapshot_mode == 1u) {
        out->scope.block_align = 40u;
        out->format_sets = 0u;
    }
    return true;
}
bool __wrap_dsound_buffer_get_snapshot(uint32_t buffer, dsound_buffer_snapshot *out)
{
    if (!__real_dsound_buffer_get_snapshot(buffer, out)) return false;
    if (snapshot_mode == 2u) out->scope.flags = 0x20u;
    if (snapshot_mode == 3u) {
        out->scope.flags = 0x10u;
        out->cache_mask = 3u;
    }
    return true;
}
bool verification_end(void)
{
    /* The baseline's final policy-off check leaves it disabled. */
    dsound_completion_set_enabled(true);
    const uint32_t stream = load(SCRATCH_DATA + 0x714u);
    snapshot_mode = 1u;
    store(words_at, 0xA5A5A5A5u);
    RUN_EXPECTING_FATAL((void)dsound_completion_stream_info(stream, words_at));
    CHECK(fatal_seen);
    CHECK_EQ_U32(load(words_at), 0xA5A5A5A5u);
    snapshot_mode = 0u;
    const uint32_t buffer = load(0x5818F8u);
    for (snapshot_mode = 2u; snapshot_mode <= 3u; ++snapshot_mode) {
        RUN_EXPECTING_FATAL((void)call_frame(0x407AA4u, 0x27468u, buffer, 0u, 0u, 0u, 1u));
        CHECK(fatal_seen);
    }
    snapshot_mode = 0u;
    return dsound_stream_reset_checked();
}
int main(void) { return completion_baseline_main(); }
