/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T602: the title's second stream scope (sub_000299C0 re-formats a started stereo startup stream).
 * Admission, named refusals and the production caller guards of SetFormat 0x408C2D. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "test_d3d8_support.h"
#include "dsound_device.h"
#include "dsound_hle.h"
#include "dsound_stream.h"
#include "kernel_clock.h"
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */

static uint32_t device, descriptor, format, input;
static uint8_t irql;
static bool irql_known = true;
static bool current_irql(uint8_t *out) { *out = irql; return irql_known; }
static const uint8_t startup[20] = {0x69, 0, 2, 0, 0x44, 0xAC, 0, 0, 0xCC, 0xC1, 0, 0, 72, 0, 4, 0, 2, 0, 64, 0};
static const uint8_t measured[20] = {0x69, 0, 2, 0, 0x00, 0x7D, 0, 0, 0xA0, 0x8C, 0, 0, 72, 0, 4, 0, 2, 0, 64, 0};
static uint32_t create_stream(bool spatial, uint32_t output)
{
    uint8_t bytes[20];
    memcpy(bytes, startup, sizeof(bytes));
    if (spatial) {
        bytes[2] = 1u;
        bytes[8] = 0xE6u;
        bytes[9] = 0x60u;
        bytes[12] = 36u;
    }
    const uint32_t desc[6] = {spatial ? 16u : 0u, 3u, format, 0u, 0u, 0u};
    CHECK(kernel_guest_write_bytes(descriptor, desc, sizeof(desc)));
    CHECK(kernel_guest_write_bytes(format, bytes, sizeof(bytes)));
    CHECK_EQ_U32(dsound_stream_create(descriptor, output), 0u);
    return load(output);
}
static void start(uint32_t stream)
{
    CHECK_EQ_U32(dsound_stream_cache_volume(stream, -10000), 0u);
    CHECK_EQ_U32(dsound_stream_cache_pause(stream, 1u), 0u);
    CHECK_EQ_U32(dsound_stream_cache_flush_ex(stream, 0u, 0u, 1u), 0u);
    CHECK_EQ_U32(dsound_stream_cache_discontinuity(stream), 0u);
}
static uint32_t create_started(void)
{
    static unsigned count;
    uint32_t stream = create_stream(false, SCRATCH_DATA + 192u + 4u * count++);
    start(stream);
    return stream;
}
static void unchanged(uint32_t stream, const dsound_stream_snapshot *before)
{
    dsound_stream_snapshot after;
    CHECK(dsound_stream_get_snapshot(stream, &after));
    CHECK(memcmp(before, &after, sizeof(after)) == 0);
}
static void refused(uint32_t stream, uint32_t address)
{
    dsound_stream_snapshot before;
    CHECK(dsound_stream_get_snapshot(stream, &before));
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_set_format(stream, address));
    CHECK(fatal_seen);
    unchanged(stream, &before);
}
/* The snapshot itself needs a valid owned object, so tampered and foreign cases only check the refusal. */
static void refused_raw(uint32_t stream, uint32_t address)
{
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_set_format(stream, address));
    CHECK(fatal_seen);
}
static void test_success_and_permissions(uint32_t stream)
{
    dsound_stream_snapshot before, after;
    CHECK(dsound_stream_get_snapshot(stream, &before));
    CHECK_EQ_U32(before.format_sets, 0u);
    uint32_t header[10], parent[11];
    CHECK(kernel_guest_read_bytes(stream, header, sizeof(header)));
    CHECK(kernel_guest_read_bytes(device, parent, sizeof(parent)));
    size_t heaps = guest_mem_heap_count();
    uint64_t clock = kernel_clock_peek();
    CHECK(kernel_guest_write_bytes(input, measured, sizeof(measured)));
    /* SetFormat records a host request only, including read-only parent and child. */
    uint32_t child_page = stream & ~4095u, parent_page = device & ~4095u;
    CHECK(mprotect((void *)(uintptr_t)child_page, 4096u, PROT_READ) == 0);
    CHECK(mprotect((void *)(uintptr_t)parent_page, 4096u, PROT_READ) == 0);
    CHECK_EQ_U32(dsound_stream_cache_set_format(stream, input), 0u);
    CHECK(dsound_stream_get_snapshot(stream, &after));
    CHECK_EQ_U32(after.format_sets, 1u);
    CHECK(memcmp(after.format, measured, sizeof(measured)) == 0);
    /* The creation scope stays the history of the stream. */
    CHECK_EQ_U32(after.scope.sample_rate, 44100u);
    CHECK_EQ_U32(after.source_rate_hz,32000u);
    before.source_rate_hz=32000u;
    before.format_sets = 1u;
    memcpy(before.format, measured, sizeof(measured));
    CHECK(memcmp(&before, &after, sizeof(after)) == 0);
    CHECK_EQ_U32(dsound_stream_cache_set_format(stream, input), 0u);
    CHECK(dsound_stream_get_snapshot(stream, &after));
    CHECK_EQ_U32(after.format_sets, 2u);
    CHECK(mprotect((void *)(uintptr_t)child_page, 4096u, PROT_READ | PROT_WRITE) == 0);
    CHECK(mprotect((void *)(uintptr_t)parent_page, 4096u, PROT_READ | PROT_WRITE) == 0);
    uint32_t actual[11];
    CHECK(kernel_guest_read_bytes(stream, actual, sizeof(header)));
    CHECK(memcmp(actual, header, sizeof(header)) == 0);
    CHECK(kernel_guest_read_bytes(device, actual, sizeof(parent)));
    CHECK(memcmp(actual, parent, sizeof(parent)) == 0);
    CHECK_EQ_U32(guest_mem_heap_count(), heaps);
    CHECK(kernel_clock_peek() == clock);
}
/* T1159: every stereo XADPCM rate in 8000 to 48000 Hz with average rate * 72 / 64 is admitted (the original validates nothing). */
static void test_adpcm_rate_family(uint32_t stream)
{
    static const uint32_t rates[] = {8000u, 11025u, 22042u, 32000u, 44100u, 48000u};
    for (unsigned i = 0u; i < sizeof(rates) / sizeof(rates[0]); i++) {
        const uint32_t average = (uint32_t)((uint64_t)rates[i] * 72u / 64u);
        uint8_t bytes[20] = {0x69, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 72, 0, 4, 0, 2, 0, 64, 0};
        memcpy(bytes + 4, &rates[i], 4);
        memcpy(bytes + 8, &average, 4);
        CHECK(kernel_guest_write_bytes(input, bytes, sizeof(bytes)));
        dsound_stream_snapshot after;
        CHECK_EQ_U32(dsound_stream_cache_set_format(stream, input), 0u);
        CHECK(dsound_stream_get_snapshot(stream, &after));
        CHECK(memcmp(after.format, bytes, sizeof(bytes)) == 0);
        /* the same rate with a wrong average is refused */
        const uint32_t wrong = average + 1u;
        memcpy(bytes + 8, &wrong, 4);
        CHECK(kernel_guest_write_bytes(input, bytes, sizeof(bytes)));
        refused(stream, input);
    }
}
static void test_format_refusals(uint32_t stream)
{
    /* every byte of the format matters: the original validates nothing, the policy admits one */
    for (unsigned byte = 0u; byte < sizeof(measured); byte++) {
        uint8_t bytes[20];
        memcpy(bytes, measured, sizeof(bytes));
        bytes[byte] ^= 1u;
        CHECK(kernel_guest_write_bytes(input, bytes, sizeof(bytes)));
        refused(stream, input);
    }
    static const uint8_t other[][20] = {
        {0x69, 0, 2, 0, 0x3F, 0x1F, 0, 0, 0x26, 0x23, 0, 0, 72, 0, 4, 0, 2, 0, 64, 0}, /* 7999 Hz, below range */
        {0x69, 0, 2, 0, 0x81, 0xBB, 0, 0, 0xF1, 0xD2, 0, 0, 72, 0, 4, 0, 2, 0, 64, 0}, /* 48001 Hz, above range */
        {0x69, 0, 1, 0, 0x00, 0x7D, 0, 0, 0x50, 0x46, 0, 0, 36, 0, 4, 0, 2, 0, 64, 0},
        {0x01, 0, 2, 0, 0x00, 0x7D, 0, 0, 0x00, 0xF4, 1, 0, 4, 0, 16, 0, 0, 0, 0, 0},
        {0},
    };
    for (unsigned i = 0u; i < sizeof(other) / sizeof(other[0]); i++) {
        CHECK(kernel_guest_write_bytes(input, other[i], 20u));
        refused(stream, input);
    }
    CHECK(kernel_guest_write_bytes(input, measured, sizeof(measured)));
    /* The measured bytes themselves placed on protected state are still refused: the original SECONDARY
     * table and the rolloff curve (the stream header and the device are checked by the ownership tests). */
    const uint32_t protected_at[] = {0x4A1CF0u, 0x4A1D1Cu, 0x4B914Cu, 0x4B913Cu, 0x4B915Bu};
    for (unsigned i = 0u; i < sizeof(protected_at) / sizeof(protected_at[0]); i++) {
        uint8_t saved[20];
        CHECK(kernel_guest_read_bytes(protected_at[i], saved, sizeof(saved)));
        CHECK(kernel_guest_write_bytes(protected_at[i], measured, sizeof(measured)));
        refused(stream, protected_at[i]);
        CHECK(kernel_guest_write_bytes(protected_at[i], saved, sizeof(saved)));
    }
    /* input addresses: null, wrapping, unreadable, aliases of every protected object */
    const uint32_t inputs[] = {0u, 0xFFFFFFF0u, 0xFFFFFFFFu, 0x7F000000u, stream, stream + 36u,
                               device, device + 40u, 0x4A1CF0u, 0x4A1D2Cu, 0x4B914Cu, 0x4124A8u, 0x412B30u};
    for (unsigned i = 0u; i < sizeof(inputs) / sizeof(inputs[0]); i++) refused(stream, inputs[i]);
}
static void test_order_and_scope(uint32_t spatial)
{
    CHECK(kernel_guest_write_bytes(input, measured, sizeof(measured)));
    /* A fresh stereo stream and every partial startup are refused, each step by name. */
    uint32_t parts = SCRATCH_DATA + 160u;
    for (unsigned stage = 0u; stage < 5u; stage++) {
        uint32_t stream = create_stream(false, parts + 4u * stage);
        if (stage > 0u) CHECK_EQ_U32(dsound_stream_cache_volume(stream, -10000), 0u);
        if (stage > 1u) CHECK_EQ_U32(dsound_stream_cache_pause(stream, 1u), 0u);
        if (stage > 2u) CHECK_EQ_U32(dsound_stream_cache_flush_ex(stream, 0u, 0u, 1u), 0u);
        if (stage > 3u) CHECK_EQ_U32(dsound_stream_cache_discontinuity(stream), 0u);
        if (stage < 4u) refused(stream, input);
        else {
            CHECK_EQ_U32(dsound_stream_cache_set_format(stream, input), 0u);
        }
    }
    /* Discontinuity before volume is the other admitted startup order, still refused for a half start. */
    uint32_t reordered = create_stream(false, parts + 24u);
    CHECK_EQ_U32(dsound_stream_cache_pause(reordered, 1u), 0u);
    CHECK_EQ_U32(dsound_stream_cache_flush_ex(reordered, 0u, 0u, 1u), 0u);
    CHECK_EQ_U32(dsound_stream_cache_discontinuity(reordered), 0u);
    refused(reordered, input);
    CHECK_EQ_U32(dsound_stream_cache_volume(reordered, -10000), 0u);
    CHECK_EQ_U32(dsound_stream_cache_set_format(reordered, input), 0u);
    /* T1181: a stereo format on the mono spatial stream stays refused, its own mono family is admitted (test_spatial_family). */
    refused(spatial, input);
}
/* T1181: the started spatial (flags 0x10, mono) stream takes the mono XADPCM family, block 36, average rate * 36 / 64. */
static void test_spatial_family(uint32_t slot)
{
    const uint32_t params[9] = {0u, 0u, 0xFFFFF448u, 0u, 0u, 0u, 0u, 0u, 0u};
    CHECK(kernel_guest_write_bytes(SCRATCH_DATA + 512u, params, sizeof(params)));
    uint32_t fresh = create_stream(true, slot);
    CHECK_EQ_U32(dsound_stream_cache_i3dl2(fresh, SCRATCH_DATA + 512u, 0u), 0u);
    CHECK_EQ_U32(dsound_stream_cache_min_distance(fresh, 0x3F800000u, 0u), 0u);
    CHECK_EQ_U32(dsound_stream_cache_rolloff(fresh, 0x4B914Cu, 4u, 0u), 0u);
    static const uint8_t title[20] = {0x69, 0, 1, 0, 0x1A, 0x56, 0, 0, 0x6E, 0x30, 0, 0, 36, 0, 4, 0, 2, 0, 64, 0};
    CHECK(kernel_guest_write_bytes(input, title, sizeof(title)));
    refused(fresh, input); /* configured but not started */
    start(fresh);
    dsound_stream_snapshot after;
    CHECK_EQ_U32(dsound_stream_cache_set_format(fresh, input), 0u);
    CHECK(dsound_stream_get_snapshot(fresh, &after));
    CHECK_EQ_U32(after.format_sets, 1u);
    CHECK(memcmp(after.format, title, sizeof(title)) == 0);
    CHECK_EQ_U32(after.scope.flags, 0x10u);
    CHECK_EQ_U32(after.scope.sample_rate, 44100u); /* the creation scope stays the history of the stream */
    const uint32_t rates[] = {8000u, 11025u, 22042u, 44100u, 48000u};
    for (unsigned i = 0u; i < sizeof(rates) / sizeof(rates[0]); i++) {
        const uint32_t average = (uint32_t)((uint64_t)rates[i] * 36u / 64u);
        uint8_t bytes[20] = {0x69, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 36, 0, 4, 0, 2, 0, 64, 0};
        memcpy(bytes + 4, &rates[i], 4);
        memcpy(bytes + 8, &average, 4);
        CHECK(kernel_guest_write_bytes(input, bytes, sizeof(bytes)));
        CHECK_EQ_U32(dsound_stream_cache_set_format(fresh, input), 0u);
        CHECK(dsound_stream_get_snapshot(fresh, &after));
        CHECK(memcmp(after.format, bytes, sizeof(bytes)) == 0);
        const uint32_t wrong = average + 1u;
        memcpy(bytes + 8, &wrong, 4);
        CHECK(kernel_guest_write_bytes(input, bytes, sizeof(bytes)));
        refused(fresh, input);
    }
    /* every byte of the title format matters, and the stereo family and block 72 are refused on the mono slot */
    for (unsigned byte = 0u; byte < sizeof(title); byte++) {
        uint8_t bytes[20];
        memcpy(bytes, title, sizeof(bytes));
        bytes[byte] ^= 1u;
        CHECK(kernel_guest_write_bytes(input, bytes, sizeof(bytes)));
        refused(fresh, input);
    }
    CHECK(kernel_guest_write_bytes(input, measured, sizeof(measured)));
    refused(fresh, input);
}
static void test_ownership_and_policy(uint32_t stream)
{
    CHECK(kernel_guest_write_bytes(input, measured, sizeof(measured)));
    dsound_stream_snapshot before;
    CHECK(dsound_stream_get_snapshot(stream, &before));
    const uint32_t foreign[] = {0u, stream + 4u, 0x1000u, device + 8u};
    for (unsigned i = 0u; i < sizeof(foreign) / sizeof(foreign[0]); i++) refused_raw(foreign[i], input);
    dsound_stream_set_enabled(false);
    refused(stream, input);
    dsound_stream_set_enabled(true);
    irql = 2u;
    refused(stream, input);
    irql = 0u;
    irql_known = false;
    refused(stream, input);
    irql_known = true;
    store(0x4124A8u, 1u);
    refused(stream, input);
    store(0x4124A8u, 0u);
    for (unsigned word = 0u; word < 10u; word++) {
        store(stream + 4u * word, before.header[word] ^ 1u);
        refused_raw(stream, input);
        store(stream + 4u * word, before.header[word]);
    }
    for (unsigned word = 0u; word < 11u; word++) {
        uint32_t original = load(device + 4u * word);
        store(device + 4u * word, original ^ 1u);
        refused_raw(stream, input);
        store(device + 4u * word, original);
    }
    unchanged(stream, &before);
}
static void test_frame(uint32_t stream)
{
    uint32_t args[2] = {stream, input};
    kernel_call_frame frame = {0};
    CHECK(kernel_frame_build(&frame, call_scratch, 0x100u, args, 2u));
    store(frame.stack_ptr, 0x299FAu);
    dsound_stream_snapshot before;
    CHECK(dsound_stream_get_snapshot(stream, &before));
    CHECK_EQ_U32(dsound_hle_call(0x408C2Du, &frame), 0u);
    dsound_stream_snapshot after;
    CHECK(dsound_stream_get_snapshot(stream, &after));
    CHECK_EQ_U32(after.format_sets, before.format_sets + 1u);
    after.format_sets = before.format_sets;
    CHECK(memcmp(&before, &after, sizeof(after)) == 0);
    CHECK(dsound_stream_get_snapshot(stream, &before));
    /* Any other return address is the unmeasured caller, wrong frames refuse too. */
    const uint32_t callers[] = {0x299F9u, 0x299FBu, 0x29B5Au, 0x29F68u, 0u};
    for (unsigned i = 0u; i < sizeof(callers) / sizeof(callers[0]); i++) {
        store(frame.stack_ptr, callers[i]);
        RUN_EXPECTING_FATAL((void)dsound_hle_call(0x408C2Du, &frame));
        CHECK(fatal_seen);
        unchanged(stream, &before);
    }
    /* Exactly the return address and the two stdcall arguments are readable: admitted. A third read would not be. */
    store(frame.stack_ptr, 0x299FAu);
    frame.stack_limit = frame.stack_ptr + 12u;
    CHECK_EQ_U32(dsound_hle_call(0x408C2Du, &frame), 0u);
    CHECK(dsound_stream_get_snapshot(stream, &before));
    frame.stack_limit = frame.stack_ptr + 8u;
    RUN_EXPECTING_FATAL((void)dsound_hle_call(0x408C2Du, &frame));
    CHECK(fatal_seen);
    unchanged(stream, &before);
    RUN_EXPECTING_FATAL((void)dsound_hle_call(0x408C2Du, NULL));
    CHECK(fatal_seen);
    unchanged(stream, &before);
}
static uint32_t call_frame(uint32_t entry, uint32_t caller, uint32_t first, uint32_t second, unsigned count)
{
    uint32_t args[4] = {first, second, 0u, 0u};
    kernel_call_frame frame = {0};
    CHECK(kernel_frame_build(&frame, call_scratch, 0x100u, args, count));
    store(frame.stack_ptr, caller);
    return dsound_hle_call(entry, &frame);
}
/* Pause(stream, 1) from the title's two callers: the startup group 0x29B5A and the re-format 0x29A51. */
static void test_pause_callers(uint32_t fresh)
{
    dsound_stream_snapshot before, after;
    CHECK(dsound_stream_get_snapshot(fresh, &before));
    CHECK_EQ_U32(before.format_sets, 0u);
    /* Without a recorded SetFormat the re-format caller is refused, the startup caller is not. */
    RUN_EXPECTING_FATAL((void)call_frame(0x407B23u, 0x29A51u, fresh, 1u, 2u));
    CHECK(fatal_seen);
    unchanged(fresh, &before);
    CHECK_EQ_U32(call_frame(0x407B23u, 0x29B5Au, fresh, 1u, 2u), 0u);
    unchanged(fresh, &before);
    CHECK(kernel_guest_write_bytes(input, measured, sizeof(measured)));
    CHECK_EQ_U32(call_frame(0x408C2Du, 0x299FAu, fresh, input, 2u), 0u);
    CHECK(dsound_stream_get_snapshot(fresh, &before));
    for (unsigned repeat = 0u; repeat < 2u; repeat++) {
        CHECK_EQ_U32(call_frame(0x407B23u, 0x29A51u, fresh, 1u, 2u), 0u);
        unchanged(fresh, &before);
        CHECK_EQ_U32(call_frame(0x407B23u, 0x29B5Au, fresh, 1u, 2u), 0u);
        unchanged(fresh, &before);
    }
    CHECK(before.pause_seen && before.pause_mode == 1u && before.format_sets == 1u);
    const uint32_t callers[] = {0x29A4Cu, 0x29A50u, 0x29A52u, 0x299FAu, 0x29B5Bu, 0x29F68u, 0u};
    for (unsigned i = 0u; i < sizeof(callers) / sizeof(callers[0]); i++) {
        RUN_EXPECTING_FATAL((void)call_frame(0x407B23u, callers[i], fresh, 1u, 2u));
        CHECK(fatal_seen);
        unchanged(fresh, &before);
    }
    /* Only mode 1: the original also answers modes 0 and 2 (it changes voice flags), the policy does not. */
    const uint32_t modes[] = {0u, 2u, 3u, 0x80000001u};
    for (unsigned i = 0u; i < sizeof(modes) / sizeof(modes[0]); i++) {
        RUN_EXPECTING_FATAL((void)call_frame(0x407B23u, 0x29A51u, fresh, modes[i], 2u));
        CHECK(fatal_seen);
        unchanged(fresh, &before);
    }
    /* Exactly the return address and two arguments are readable: admitted. One argument is not. */
    kernel_call_frame frame = {0};
    uint32_t args[2] = {fresh, 1u};
    CHECK(kernel_frame_build(&frame, call_scratch, 0x100u, args, 2u));
    store(frame.stack_ptr, 0x29A51u);
    frame.stack_limit = frame.stack_ptr + 12u;
    CHECK_EQ_U32(dsound_hle_call(0x407B23u, &frame), 0u);
    frame.stack_limit = frame.stack_ptr + 8u;
    RUN_EXPECTING_FATAL((void)dsound_hle_call(0x407B23u, &frame));
    CHECK(fatal_seen);
    CHECK(dsound_stream_get_snapshot(fresh, &after));
    CHECK(memcmp(&before, &after, sizeof(after)) == 0);
}
/* SetVolume(stream, -10000) from the title's two callers: the startup 0x29F68 and the re-format 0x29A67. */
static void test_volume_callers(uint32_t fresh)
{
    dsound_stream_snapshot before, after;
    const uint32_t startup_volume = 0xFFFFD8F0u;
    CHECK(dsound_stream_get_snapshot(fresh, &before));
    CHECK_EQ_U32(before.format_sets, 0u);
    /* Without a recorded SetFormat the re-format caller is refused, the startup caller is not. */
    RUN_EXPECTING_FATAL((void)call_frame(0x407B14u, 0x29A67u, fresh, startup_volume, 2u));
    CHECK(fatal_seen);
    unchanged(fresh, &before);
    CHECK_EQ_U32(call_frame(0x407B14u, 0x29F68u, fresh, startup_volume, 2u), 0u);
    unchanged(fresh, &before);
    CHECK(kernel_guest_write_bytes(input, measured, sizeof(measured)));
    CHECK_EQ_U32(call_frame(0x408C2Du, 0x299FAu, fresh, input, 2u), 0u);
    CHECK(dsound_stream_get_snapshot(fresh, &before));
    for (unsigned repeat = 0u; repeat < 2u; repeat++) {
        CHECK_EQ_U32(call_frame(0x407B14u, 0x29A67u, fresh, startup_volume, 2u), 0u);
        unchanged(fresh, &before);
        CHECK_EQ_U32(call_frame(0x407B14u, 0x29F68u, fresh, startup_volume, 2u), 0u);
        unchanged(fresh, &before);
    }
    CHECK(before.volume_seen && before.volume == -10000 && before.format_sets == 1u);
    const uint32_t callers[] = {0x29A62u, 0x29A66u, 0x29A68u, 0x29A51u, 0x29A5Au, 0x29F69u, 0u};
    for (unsigned i = 0u; i < sizeof(callers) / sizeof(callers[0]); i++) {
        RUN_EXPECTING_FATAL((void)call_frame(0x407B14u, callers[i], fresh, startup_volume, 2u));
        CHECK(fatal_seen);
        unchanged(fresh, &before);
    }
    /* Only -10000, from either caller: the original answers every volume. */
    const uint32_t volumes[] = {0u, 1u, 0xFFFFD8EFu, 0xFFFFD8F1u, 0x80000000u};
    for (unsigned i = 0u; i < sizeof(volumes) / sizeof(volumes[0]); i++) {
        RUN_EXPECTING_FATAL((void)call_frame(0x407B14u, 0x29A67u, fresh, volumes[i], 2u));
        CHECK(fatal_seen);
        RUN_EXPECTING_FATAL((void)call_frame(0x407B14u, 0x29F68u, fresh, volumes[i], 2u));
        CHECK(fatal_seen);
        unchanged(fresh, &before);
    }
    kernel_call_frame frame = {0};
    uint32_t args[2] = {fresh, startup_volume};
    CHECK(kernel_frame_build(&frame, call_scratch, 0x100u, args, 2u));
    store(frame.stack_ptr, 0x29A67u);
    frame.stack_limit = frame.stack_ptr + 12u;
    CHECK_EQ_U32(dsound_hle_call(0x407B14u, &frame), 0u);
    frame.stack_limit = frame.stack_ptr + 8u;
    RUN_EXPECTING_FATAL((void)dsound_hle_call(0x407B14u, &frame));
    CHECK(fatal_seen);
    CHECK(dsound_stream_get_snapshot(fresh, &after));
    CHECK(memcmp(&before, &after, sizeof(after)) == 0);
}
/* T605: GetStatus (0x4073D3, caller 0x29D28 in sub_00029D10) on the re-formatted stream. The original answers 1 (bit 0:
 * the free packet list is non-empty), the passive record has no packet so the answer is that word and nothing else. */
static uint32_t status_at;
static void status_refused(uint32_t stream, uint32_t output)
{
    dsound_stream_snapshot before;
    uint32_t word = 0xA5A5A5A5u;
    const bool readable = output != 0u && kernel_guest_read_u32(output, &word);
    CHECK(dsound_stream_get_snapshot(stream, &before));
    RUN_EXPECTING_FATAL((void)dsound_stream_get_startup_status(stream, output));
    CHECK(fatal_seen);
    unchanged(stream, &before);
    uint32_t after = 0u;
    if (readable) {
        CHECK(kernel_guest_read_u32(output, &after));
        CHECK_EQ_U32(after, word);
    }
}
static void test_status_after_reformat(uint32_t fresh, uint32_t started, uint32_t spatial)
{
    status_at = SCRATCH_DATA + 1536u;
    dsound_stream_snapshot before, after;
    uint32_t header[10], parent[11], word;
    /* Fresh startup (no Pause yet): the T570 startup status, unchanged. */
    store(status_at, 0xA5A5A5A5u);
    store(status_at + 4u, 0x11223344u);
    CHECK_EQ_U32(dsound_stream_get_startup_status(fresh, status_at), 0u);
    CHECK_EQ_U32(load(status_at), 1u);
    CHECK_EQ_U32(load(status_at + 4u), 0x11223344u);
    /* The started stereo stream before any SetFormat and the started spatial stream (the stream update's other slots) are
     * answered the same way, and only the whole start counts: a Pause or FlushEx alone is refused by name. */
    const uint32_t params_at = SCRATCH_DATA + 1700u;
    const uint32_t zero_params[9] = {0u, 0u, 0xFFFFF448u, 0u, 0u, 0u, 0u, 0u, 0u};
    CHECK(kernel_guest_write_bytes(params_at, zero_params, sizeof(zero_params)));
    uint32_t extra = create_started();
    for (unsigned pass = 0u; pass < 2u; pass++) {
        const uint32_t subjects[2] = {extra, spatial};
        dsound_stream_snapshot subject_before;
        CHECK(dsound_stream_get_snapshot(subjects[pass], &subject_before));
        for (unsigned repeat = 0u; repeat < 2u; repeat++) {
            store(status_at, 0xA5A5A5A5u);
            store(status_at + 4u, 0x11223344u);
            CHECK_EQ_U32(dsound_stream_get_startup_status(subjects[pass], status_at), 0u);
            CHECK_EQ_U32(load(status_at), 1u);
            CHECK_EQ_U32(load(status_at + 4u), 0x11223344u);
            unchanged(subjects[pass], &subject_before);
        }
    }
    for (unsigned kind = 0u; kind < 2u; kind++) {
        for (unsigned stage = 1u; stage < 5u; stage++) {
            uint32_t partial = create_stream(kind != 0u, SCRATCH_DATA + 212u + 4u * (stage + 4u * kind));
            if (kind != 0u) {
                CHECK_EQ_U32(dsound_stream_cache_i3dl2(partial, params_at, 0u), 0u);
                CHECK_EQ_U32(dsound_stream_cache_min_distance(partial, 0x3F800000u, 0u), 0u);
                CHECK_EQ_U32(dsound_stream_cache_rolloff(partial, 0x4B914Cu, 4u, 0u), 0u);
            }
            CHECK_EQ_U32(dsound_stream_cache_volume(partial, -10000), 0u);
            if (stage > 1u) CHECK_EQ_U32(dsound_stream_cache_pause(partial, 1u), 0u);
            if (stage > 2u) CHECK_EQ_U32(dsound_stream_cache_flush_ex(partial, 0u, 0u, 1u), 0u);
            if (stage > 3u) CHECK_EQ_U32(dsound_stream_cache_discontinuity(partial), 0u);
            /* volume only is the T570 startup scope, Pause and FlushEx are in between, Discontinuity completes the start */
            if (stage == 2u || stage == 3u) status_refused(partial, status_at);
            else CHECK_EQ_U32(dsound_stream_get_startup_status(partial, status_at), 0u);
        }
    }
    store(status_at, 0xA5A5A5A5u);
    CHECK(kernel_guest_write_bytes(input, measured, sizeof(measured)));
    CHECK_EQ_U32(dsound_stream_cache_set_format(started, input), 0u);
    CHECK(dsound_stream_get_snapshot(started, &before));
    CHECK(kernel_guest_read_bytes(started, header, sizeof(header)));
    CHECK(kernel_guest_read_bytes(device, parent, sizeof(parent)));
    uint64_t clock = kernel_clock_peek();
    size_t heaps = guest_mem_heap_count();
    for (unsigned repeat = 0u; repeat < 3u; repeat++) {
        store(status_at, 0xA5A5A5A5u);
        store(status_at + 4u, 0x11223344u);
        store(status_at - 4u, 0x55667788u);
        CHECK_EQ_U32(dsound_stream_get_startup_status(started, status_at), 0u);
        /* bit 0 only, and the two neighbouring words are not touched */
        CHECK_EQ_U32(load(status_at), 1u);
        CHECK_EQ_U32(load(status_at + 4u), 0x11223344u);
        CHECK_EQ_U32(load(status_at - 4u), 0x55667788u);
        unchanged(started, &before);
    }
    CHECK(dsound_stream_get_snapshot(started, &after));
    CHECK_EQ_U32(after.format_sets, 1u);
    uint32_t actual[11];
    CHECK(kernel_guest_read_bytes(started, actual, sizeof(header)));
    CHECK(memcmp(actual, header, sizeof(header)) == 0);
    CHECK(kernel_guest_read_bytes(device, actual, sizeof(parent)));
    CHECK(memcmp(actual, parent, sizeof(parent)) == 0);
    CHECK_EQ_U32(guest_mem_heap_count(), heaps);
    CHECK(kernel_clock_peek() == clock);
    /* The output may not alias protected state, wrap, be null or be unreadable. */
    const uint32_t outputs[] = {0u, 0xFFFFFFFDu, 0xFFFFFFFFu, 0x7F000000u, started, started + 36u, device,
                                device + 40u, 0x4A1CF0u, 0x4A1D28u, 0x4B914Cu, 0x4B9158u, 0x4B915Bu, 0x4124A8u};
    for (unsigned i = 0u; i < sizeof(outputs) / sizeof(outputs[0]); i++) status_refused(started, outputs[i]);
    /* Every guest word of the owned header and of the device is part of the ownership check. */
    for (unsigned index = 0u; index < 10u; index++) {
        store(started + 4u * index, before.header[index] ^ 1u);
        RUN_EXPECTING_FATAL((void)dsound_stream_get_startup_status(started, status_at));
        CHECK(fatal_seen);
        store(started + 4u * index, before.header[index]);
    }
    /* Policy: disabled, IRQL above 0 or unknown, a nonzero global audio state. */
    dsound_stream_set_enabled(false);
    status_refused(started, status_at);
    dsound_stream_set_enabled(true);
    irql = 2u;
    status_refused(started, status_at);
    irql = 0u;
    irql_known = false;
    status_refused(started, status_at);
    irql_known = true;
    store(0x4124A8u, 1u);
    status_refused(started, status_at);
    store(0x4124A8u, 0u);
    /* A foreign object is not owned. */
    RUN_EXPECTING_FATAL((void)dsound_stream_get_startup_status(started + 4u, status_at));
    CHECK(fatal_seen);
    /* The answer is read back, so the same call still works after the refusals. */
    CHECK_EQ_U32(dsound_stream_get_startup_status(started, status_at), 0u);
    CHECK(kernel_guest_read_u32(status_at, &word) && word == 1u);
    unchanged(started, &before);
}
int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    dsound_hle_init();
    dsound_device_set_fatal(catching_fatal);
    dsound_stream_set_fatal(catching_fatal);
    dsound_stream_set_irql_provider(current_irql);
    map_fixed(0x412000u, 4096u);
    map_fixed(0x4A1000u, 4096u);
    map_fixed(0x4B9000u, 4096u);
    for (unsigned i = 0u; i < 15u; i++) store(0x4A1CF0u + 4u * i, 0x406879u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    CHECK_EQ_U32(dsound_device_create(0u, SCRATCH_DATA, 0u), 0u);
    device = load(SCRATCH_DATA) - 8u;
    descriptor = SCRATCH_DATA + 256u;
    format = SCRATCH_DATA + 320u;
    input = SCRATCH_DATA + 384u;
    /* Policy off: the SetFormat route is not registered (a flags-off boot keeps its registry). */
    dsound_stream_set_enabled(false);
    CHECK_EQ_U32(dsound_stream_register(), 7u);
    CHECK(dsound_hle_entry(0x408C2Du)->state != DSOUND_ENTRY_IMPLEMENTED);
    dsound_stream_set_enabled(true);
    CHECK_EQ_U32(dsound_stream_register(), 10u);
    CHECK(dsound_hle_entry(0x408C2Du)->state == DSOUND_ENTRY_IMPLEMENTED);
    uint32_t stream = create_stream(false, SCRATCH_DATA + 128u);
    uint32_t spatial = create_stream(true, SCRATCH_DATA + 132u);
    const uint32_t params[9] = {0u, 0u, 0xFFFFF448u, 0u, 0u, 0u, 0u, 0u, 0u};
    CHECK(kernel_guest_write_bytes(SCRATCH_DATA + 512u, params, sizeof(params)));
    CHECK_EQ_U32(dsound_stream_cache_i3dl2(spatial, SCRATCH_DATA + 512u, 0u), 0u);
    CHECK_EQ_U32(dsound_stream_cache_min_distance(spatial, 0x3F800000u, 0u), 0u);
    CHECK_EQ_U32(dsound_stream_cache_rolloff(spatial, 0x4B914Cu, 4u, 0u), 0u);
    start(spatial);
    start(stream);
    test_order_and_scope(spatial);
    test_format_refusals(stream);
    test_ownership_and_policy(stream);
    test_success_and_permissions(stream);
    test_frame(stream);
    test_adpcm_rate_family(stream);
    test_spatial_family(SCRATCH_DATA + 600u);
    test_pause_callers(create_started());
    test_volume_callers(create_started());
    {
        uint32_t early = create_stream(false, SCRATCH_DATA + 208u);
        CHECK_EQ_U32(dsound_stream_cache_volume(early, -10000), 0u);
        test_status_after_reformat(early, create_started(), spatial);
    }
    CHECK(dsound_stream_reset_checked());
    CHECK_EQ_U32(load(device + 4u), 6u);
    dsound_stream_snapshot sentinel;
    refused_raw(stream, input);
    CHECK(!dsound_stream_get_snapshot(stream, &sentinel));
    CHECK(dsound_device_reset_checked());
    environment_end();
    printf("passive second stream scope: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
