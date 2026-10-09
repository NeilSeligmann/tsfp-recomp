/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T759: a measured stereo Process packet reaches the opt-in PCM mixer. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <string.h>
#include "test_d3d8_support.h"
#include "dsound_audio_runtime.h"
#include "dsound_completion.h"
#include "dsound_device.h"
#include "dsound_hle.h"
#include "dsound_stream.h"

#define DATA_BASE 0x23000000u

int recomp_has_stop_boundary(uint32_t address)
{
    return address == 0x408040u || address == 0x406FA9u || address == 0x406FF0u ||
           address == 0x406879u;
}

static uint64_t fake_now;
static uint64_t fake_clock(void) { return fake_now; }
static bool current_irql(uint8_t *out) { *out = 0u; return true; }
static void stop_fatal(uint32_t address, const char *reason)
{
    (void)address;
    (void)reason;
}

int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    dsound_hle_init();
    dsound_device_set_fatal(stop_fatal);
    dsound_stream_set_fatal(stop_fatal);
    dsound_completion_set_fatal(stop_fatal);
    dsound_stream_set_irql_provider(current_irql);
    map_fixed(0x412000u, 4096u);
    map_fixed(0x4A1000u, 4096u);
    map_fixed(0x4B9000u, 4096u);
    map_fixed(0x581000u, 4096u);
    map_fixed(0x583000u, 4096u);
    map_fixed(DATA_BASE, 4096u);
    for (unsigned i = 0u; i < 15u; i++) store(0x4A1CF0u + 4u * i, 0x406879u);
    for (unsigned i = 0u; i < 4u; i++) store(0x4A1CE0u + 4u * i, 0x406879u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    CHECK_EQ_U32(dsound_device_create(0u, SCRATCH_DATA, 0u), 0u);
    dsound_stream_set_enabled(true);
    CHECK(dsound_stream_register() != 0u);
    dsound_completion_set_clock(fake_clock, 32000u);
    dsound_completion_set_enabled(true);
    dsound_stream_set_completion(true, dsound_completion_note_pause);
    CHECK(dsound_audio_runtime_start(32000u, 32000u));

    const uint32_t descriptor = SCRATCH_DATA + 0x100u;
    const uint32_t format_address = SCRATCH_DATA + 0x140u;
    const uint32_t output = SCRATCH_DATA + 0x700u;
    const uint32_t packet_address = SCRATCH_DATA + 0x300u;
    const uint32_t descriptor_words[6] = {0u, 3u, format_address, 0u, 0u, 0u};
    const uint8_t startup_format[20] = {
        0x69, 0, 2, 0, 0x44, 0xAC, 0, 0, 0xCC, 0xC1, 0, 0, 72, 0, 4, 0, 2, 0, 64, 0};
    const uint8_t measured_format[20] = {
        0x69, 0, 2, 0, 0x00, 0x7D, 0, 0, 0xA0, 0x8C, 0, 0, 72, 0, 4, 0, 2, 0, 64, 0};
    CHECK(kernel_guest_write_bytes(descriptor, descriptor_words, sizeof(descriptor_words)));
    CHECK(kernel_guest_write_bytes(format_address, startup_format, sizeof(startup_format)));
    CHECK_EQ_U32(dsound_stream_create(descriptor, output), 0u);
    const uint32_t stream = load(output);
    CHECK_EQ_U32(dsound_stream_cache_volume(stream, -10000), 0u);
    CHECK_EQ_U32(dsound_stream_cache_pause(stream, 1u), 0u);
    CHECK_EQ_U32(dsound_stream_cache_flush_ex(stream, 0u, 0u, 1u), 0u);
    CHECK_EQ_U32(dsound_stream_cache_discontinuity(stream), 0u);
    CHECK(kernel_guest_write_bytes(format_address, measured_format, sizeof(measured_format)));
    CHECK_EQ_U32(dsound_stream_cache_set_format(stream, format_address), 0u);
    CHECK_EQ_U32(dsound_stream_cache_pause(stream, 0u), 0u);

    uint8_t block[72] = {0};
    block[0] = 0x20u;
    block[1] = 0x03u;
    block[4] = 0xE0u;
    block[5] = 0xFCu;
    CHECK(kernel_guest_write_bytes(DATA_BASE, block, sizeof(block)));
    const uint32_t packet[6] = {DATA_BASE, sizeof(block), 0u, 0u, 0u, 0u};
    CHECK(kernel_guest_write_bytes(packet_address, packet, sizeof(packet)));
    CHECK_EQ_U32(dsound_completion_stream_process(stream, packet_address, 0u), 0u);

    fake_now = 64u;
    dsound_completion_work();
    int16_t pcm[64u * 2u];
    size_t frames = 0u;
    CHECK(dsound_audio_runtime_render(fake_now, pcm, 64u, &frames));
    CHECK(frames == 64u);
    CHECK(pcm[0] == 800 && pcm[1] == -800);
    CHECK(pcm[126] == 800 && pcm[127] == -800);
    dsound_audio_runtime_stop();
    CHECK(dsound_stream_reset_checked());
    CHECK(dsound_device_reset_checked());
    environment_end();
    return failures == 0 ? 0 : 1;
}
