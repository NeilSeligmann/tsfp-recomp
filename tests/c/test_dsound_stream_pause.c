/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "test_d3d8_support.h"
#include "dsound_device.h"
#include "dsound_hle.h"
#include "dsound_stream.h"
#include "kernel_clock.h"
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */

static uint32_t device, descriptor, format;
static uint8_t irql;
static bool irql_known = true;
static bool current_irql(uint8_t *out) { *out = irql; return irql_known; }
static uint32_t create_stream(bool spatial, uint32_t output)
{
    uint16_t channels = spatial ? 1u : 2u;
    const uint32_t desc[6] = {spatial ? 16u : 0u, 3u, format, 0u, 0u, 0u};
    uint8_t bytes[20] = {0};
    const uint16_t prefix[2] = {0x69u, channels};
    const uint16_t suffix[4] = {(uint16_t)(36u * channels), 4u, 2u, 64u};
    uint32_t rate = 44100u, average = (44100u * 36u * channels) >> 6u;
    memcpy(bytes, prefix, 4u);
    memcpy(bytes + 4u, &rate, 4u);
    memcpy(bytes + 8u, &average, 4u);
    memcpy(bytes + 12u, suffix, 8u);
    CHECK(kernel_guest_write_bytes(descriptor, desc, sizeof(desc)));
    CHECK(kernel_guest_write_bytes(format, bytes, sizeof(bytes)));
    CHECK_EQ_U32(dsound_stream_create(descriptor, output), 0u);
    return load(output);
}
static void unchanged(uint32_t stream, const dsound_stream_snapshot *before)
{
    dsound_stream_snapshot after;
    CHECK(dsound_stream_get_snapshot(stream, &after));
    CHECK(memcmp(before, &after, sizeof(after)) == 0);
}
static void test_success_and_permissions(uint32_t stream)
{
    dsound_stream_snapshot before, after;
    CHECK(dsound_stream_get_snapshot(stream, &before));
    CHECK(!before.pause_seen);
    CHECK_EQ_U32(before.pause_mode, 0u);
    uint32_t header[10], parent[11];
    CHECK(kernel_guest_read_bytes(stream, header, sizeof(header)));
    CHECK(kernel_guest_read_bytes(device, parent, sizeof(parent)));
    size_t heaps = guest_mem_heap_count();
    uint64_t clock = kernel_clock_peek();
    /* The pause request is observation only, including both parent and child. */
    uint32_t child_page = stream & ~4095u, parent_page = device & ~4095u;
    CHECK(mprotect((void *)(uintptr_t)child_page, 4096u, PROT_READ) == 0);
    CHECK(mprotect((void *)(uintptr_t)parent_page, 4096u, PROT_READ) == 0);
    RUN_EXPECTING_FATAL(CHECK_EQ_U32(dsound_stream_cache_pause(stream, 1u), 0u));
    CHECK(!fatal_seen);
    if (fatal_seen) {
        CHECK(mprotect((void *)(uintptr_t)child_page, 4096u, PROT_READ | PROT_WRITE) == 0);
        CHECK(mprotect((void *)(uintptr_t)parent_page, 4096u, PROT_READ | PROT_WRITE) == 0);
        return;
    }
    CHECK(dsound_stream_get_snapshot(stream, &after));
    CHECK(after.pause_seen);
    CHECK_EQ_U32(after.pause_mode, 1u);
    before.pause_seen = true;
    before.pause_mode = 1u;
    CHECK(memcmp(&before, &after, sizeof(after)) == 0);
    CHECK_EQ_U32(dsound_stream_cache_pause(stream, 1u), 0u);
    unchanged(stream, &after);
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
static void test_scope_and_ownership(uint32_t stream, uint32_t spatial)
{
    dsound_stream_snapshot before;
    CHECK(dsound_stream_get_snapshot(stream, &before));
    const uint32_t modes[] = {0u, 2u, 3u, 4u, UINT32_MAX};
    for (unsigned i = 0u; i < sizeof(modes) / sizeof(modes[0]); i++) {
        RUN_EXPECTING_FATAL((void)dsound_stream_cache_pause(stream, modes[i]));
        CHECK(fatal_seen);
        unchanged(stream, &before);
    }
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_pause(spatial, 1u));
    CHECK(fatal_seen);
    dsound_stream_snapshot spatial_before;
    CHECK(dsound_stream_get_snapshot(spatial, &spatial_before));
    CHECK(!spatial_before.pause_seen);
    const uint32_t foreign[] = {0u, stream + 4u, 0x1000u, device + 8u};
    for (unsigned i = 0u; i < sizeof(foreign) / sizeof(foreign[0]); i++) {
        RUN_EXPECTING_FATAL((void)dsound_stream_cache_pause(foreign[i], 1u));
        CHECK(fatal_seen);
        unchanged(stream, &before);
    }
    dsound_stream_set_enabled(false);
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_pause(stream, 1u));
    CHECK(fatal_seen);
    dsound_stream_set_enabled(true);
    unchanged(stream, &before);
    irql = 2u;
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_pause(stream, 1u));
    CHECK(fatal_seen);
    irql = 0u;
    irql_known = false;
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_pause(stream, 1u));
    CHECK(fatal_seen);
    irql_known = true;
    store(0x4124A8u, 1u);
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_pause(stream, 1u));
    CHECK(fatal_seen);
    store(0x4124A8u, 0u);
    unchanged(stream, &before);
    for (unsigned word = 0u; word < 10u; word++) {
        store(stream + 4u * word, before.header[word] ^ 1u);
        RUN_EXPECTING_FATAL((void)dsound_stream_cache_pause(stream, 1u));
        CHECK(fatal_seen);
        store(stream + 4u * word, before.header[word]);
        unchanged(stream, &before);
    }
    for (unsigned word = 0u; word < 11u; word++) {
        uint32_t original = load(device + 4u * word);
        store(device + 4u * word, original ^ 1u);
        RUN_EXPECTING_FATAL((void)dsound_stream_cache_pause(stream, 1u));
        CHECK(fatal_seen);
        store(device + 4u * word, original);
        unchanged(stream, &before);
    }
    const uint32_t pages[] = {stream & ~4095u, device & ~4095u, 0x412000u};
    for (unsigned i = 0u; i < sizeof(pages) / sizeof(pages[0]); i++) {
        CHECK(mprotect((void *)(uintptr_t)pages[i], 4096u, PROT_NONE) == 0);
        RUN_EXPECTING_FATAL((void)dsound_stream_cache_pause(stream, 1u));
        CHECK(fatal_seen);
        CHECK(mprotect((void *)(uintptr_t)pages[i], 4096u, PROT_READ | PROT_WRITE) == 0);
        unchanged(stream, &before);
    }
}
static void test_frame(uint32_t stream)
{
    CHECK_EQ_U32(dsound_stream_register(), 10u);
    uint32_t args[2] = {stream, 1u};
    kernel_call_frame frame = {0};
    CHECK(kernel_frame_build(&frame, call_scratch, 0x100u, args, 2u));
    store(frame.stack_ptr, 0x29B5Au);
    dsound_stream_snapshot before;
    CHECK(dsound_stream_get_snapshot(stream, &before));
    CHECK_EQ_U32(dsound_hle_call(0x407B23u, &frame), 0u);
    unchanged(stream, &before);
    store(frame.stack_ptr, 0x29A51u);
    RUN_EXPECTING_FATAL((void)dsound_hle_call(0x407B23u, &frame));
    CHECK(fatal_seen);
    unchanged(stream, &before);
    store(frame.stack_ptr, 0x29B5Au);
    frame.stack_limit = frame.stack_ptr + 8u;
    RUN_EXPECTING_FATAL((void)dsound_hle_call(0x407B23u, &frame));
    CHECK(fatal_seen);
    unchanged(stream, &before);
    RUN_EXPECTING_FATAL((void)dsound_hle_call(0x407B23u, NULL));
    CHECK(fatal_seen);
    unchanged(stream, &before);
}
static void test_spatial_startup(uint32_t spatial)
{
    const uint32_t parameters[9] = {0u, 0u, 0xFFFFF448u, 0u, 0u, 0u, 0u, 0u, 0u};
    const uint32_t parameter_address = SCRATCH_DATA + 512u;
    map_fixed(0x4B9000u, 4096u);
    const uint32_t curve[4] = {0x3F000000u, 0x3E800000u, 0x3E000000u, 0u};
    CHECK(kernel_guest_write_bytes(0x4B914Cu, curve, sizeof(curve)));
    CHECK(kernel_guest_write_bytes(parameter_address, parameters, sizeof(parameters)));
    CHECK_EQ_U32(dsound_stream_cache_i3dl2(spatial, parameter_address, 0u), 0u);
    dsound_stream_snapshot before;
    CHECK(dsound_stream_get_snapshot(spatial, &before));
    CHECK_EQ_U32(before.cache_mask, 1u);
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_pause(spatial, 1u));
    CHECK(fatal_seen);
    unchanged(spatial, &before);
    CHECK_EQ_U32(dsound_stream_cache_min_distance(spatial, 0x3F800000u, 0u), 0u);
    CHECK(dsound_stream_get_snapshot(spatial, &before));
    CHECK_EQ_U32(before.cache_mask, 3u);
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_pause(spatial, 1u));
    CHECK(fatal_seen);
    unchanged(spatial, &before);
    CHECK_EQ_U32(dsound_stream_cache_rolloff(spatial, 0x4B914Cu, 4u, 0u), 0u);
    CHECK(dsound_stream_get_snapshot(spatial, &before));
    CHECK_EQ_U32(before.cache_mask, 7u);
    const uint32_t modes[] = {0u, 2u, 3u, 4u, UINT32_MAX};
    for (unsigned i = 0u; i < sizeof(modes) / sizeof(modes[0]); i++) {
        RUN_EXPECTING_FATAL((void)dsound_stream_cache_pause(spatial, modes[i]));
        CHECK(fatal_seen);
        unchanged(spatial, &before);
    }
    test_success_and_permissions(spatial);
    CHECK(dsound_stream_get_snapshot(spatial, &before));
    if (before.pause_seen) test_frame(spatial);
}
static void test_stale_child(uint32_t stream)
{
    dsound_stream_snapshot before;
    CHECK(dsound_stream_get_snapshot(stream, &before));
    CHECK(guest_heap_destroy(before.stream_heap));
    map_fixed(stream & ~4095u, 4096u);
    store(stream, 0xFACECAFEu);
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_pause(stream, 1u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(load(stream), 0xFACECAFEu);
    dsound_stream_snapshot sentinel, old;
    memset(&sentinel, 0xA5, sizeof(sentinel));
    old = sentinel;
    CHECK(!dsound_stream_get_snapshot(stream, &sentinel));
    CHECK(memcmp(&old, &sentinel, sizeof(old)) == 0);
    CHECK(!dsound_stream_reset_checked());
    CHECK_EQ_U32(load(stream), 0xFACECAFEu);
}
int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    dsound_hle_init();
    dsound_device_set_fatal(catching_fatal);
    dsound_stream_set_fatal(catching_fatal);
    dsound_stream_set_irql_provider(current_irql);
    dsound_stream_set_enabled(true);
    map_fixed(0x412000u, 4096u);
    map_fixed(0x4A1000u, 4096u);
    for (unsigned i = 0u; i < 15u; i++) store(0x4A1CF0u + 4u * i, 0x406879u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    CHECK_EQ_U32(dsound_device_create(0u, SCRATCH_DATA, 0u), 0u);
    device = load(SCRATCH_DATA) - 8u;
    descriptor = SCRATCH_DATA + 256u;
    format = SCRATCH_DATA + 320u;
    uint32_t stream = create_stream(false, SCRATCH_DATA + 128u);
    uint32_t spatial = create_stream(true, SCRATCH_DATA + 132u);
    test_success_and_permissions(stream);
    test_scope_and_ownership(stream, spatial);
    test_frame(stream);
    test_spatial_startup(spatial);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        failures = 0;
        test_stale_child(stream);
        fflush(stdout);
        _exit(failures ? 1 : 0);
    }
    if (child > 0) {
        int status;
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    CHECK_EQ_U32(load(device + 4u), 8u);
    CHECK(dsound_stream_reset_checked());
    CHECK_EQ_U32(load(device + 4u), 6u);
    dsound_stream_snapshot sentinel;
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_pause(stream, 1u));
    CHECK(fatal_seen);
    CHECK(!dsound_stream_get_snapshot(stream, &sentinel));
    CHECK(dsound_device_reset_checked());
    environment_end();
    printf("passive stream Pause: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
