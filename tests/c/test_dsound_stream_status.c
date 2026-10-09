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
    CHECK(before.volume_seen);
    uint32_t header[10], parent[11];
    CHECK(kernel_guest_read_bytes(stream, header, sizeof(header)));
    CHECK(kernel_guest_read_bytes(device, parent, sizeof(parent)));
    size_t heaps = guest_mem_heap_count();
    uint64_t clock = kernel_clock_peek();
    /* Volume records a host request only, including read-only parent and child. */
    uint32_t child_page = stream & ~4095u, parent_page = device & ~4095u;
    CHECK(mprotect((void *)(uintptr_t)child_page, 4096u, PROT_READ) == 0);
    CHECK(mprotect((void *)(uintptr_t)parent_page, 4096u, PROT_READ) == 0);
    CHECK_EQ_U32(dsound_stream_get_startup_status(stream, SCRATCH_DATA+1024u), 0u);
    CHECK(dsound_stream_get_snapshot(stream, &after));
    CHECK(after.volume_seen);
    CHECK_EQ_U32(load(SCRATCH_DATA+1024u),1u);
    CHECK(memcmp(&before, &after, sizeof(after)) == 0);
    CHECK_EQ_U32(dsound_stream_get_startup_status(stream, SCRATCH_DATA+1024u), 0u);
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
    RUN_EXPECTING_FATAL((void)dsound_stream_get_startup_status(spatial,SCRATCH_DATA+1024u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(dsound_stream_cache_volume(spatial,-10000),0u);
    dsound_stream_snapshot stereo_ready;
    CHECK(dsound_stream_get_snapshot(spatial,&stereo_ready));
    CHECK_EQ_U32(dsound_stream_get_startup_status(spatial,SCRATCH_DATA+1024u),0u);
    CHECK_EQ_U32(dsound_stream_get_startup_status(spatial,SCRATCH_DATA+1024u),0u);
    CHECK_EQ_U32(load(SCRATCH_DATA+1024u),1u);
    unchanged(spatial,&stereo_ready);
    CHECK_EQ_U32(dsound_stream_cache_pause(spatial,1u),0u);
    RUN_EXPECTING_FATAL((void)dsound_stream_get_startup_status(spatial, SCRATCH_DATA+1024u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(dsound_stream_cache_flush_ex(spatial,0u,0u,1u),0u);
    RUN_EXPECTING_FATAL((void)dsound_stream_get_startup_status(spatial,SCRATCH_DATA+1024u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(dsound_stream_cache_discontinuity(spatial),0u);
    /* T605: the whole start (Discontinuity recorded) is the status of a started stream, answered 1 like the fresh one. */
    dsound_stream_snapshot spatial_before;
    CHECK(dsound_stream_get_snapshot(spatial, &spatial_before));
    CHECK(spatial_before.volume_seen);
    store(SCRATCH_DATA+1024u,0xA5A5A5A5u);
    CHECK_EQ_U32(dsound_stream_get_startup_status(spatial,SCRATCH_DATA+1024u),0u);
    CHECK_EQ_U32(load(SCRATCH_DATA+1024u),1u);
    unchanged(spatial,&spatial_before);
    const uint32_t foreign[] = {0u, stream + 4u, 0x1000u, device + 8u};
    for (unsigned i = 0u; i < sizeof(foreign) / sizeof(foreign[0]); i++) {
        RUN_EXPECTING_FATAL((void)dsound_stream_get_startup_status(foreign[i], SCRATCH_DATA+1024u));
        CHECK(fatal_seen);
        unchanged(stream, &before);
    }
    dsound_stream_set_enabled(false);
    RUN_EXPECTING_FATAL((void)dsound_stream_get_startup_status(stream, SCRATCH_DATA+1024u));
    CHECK(fatal_seen);
    dsound_stream_set_enabled(true);
    unchanged(stream, &before);
    irql = 2u;
    RUN_EXPECTING_FATAL((void)dsound_stream_get_startup_status(stream, SCRATCH_DATA+1024u));
    CHECK(fatal_seen);
    irql = 0u;
    irql_known = false;
    RUN_EXPECTING_FATAL((void)dsound_stream_get_startup_status(stream, SCRATCH_DATA+1024u));
    CHECK(fatal_seen);
    irql_known = true;
    store(0x4124A8u, 1u);
    RUN_EXPECTING_FATAL((void)dsound_stream_get_startup_status(stream, SCRATCH_DATA+1024u));
    CHECK(fatal_seen);
    store(0x4124A8u, 0u);
    unchanged(stream, &before);
    for (unsigned word = 0u; word < 10u; word++) {
        store(stream + 4u * word, before.header[word] ^ 1u);
        RUN_EXPECTING_FATAL((void)dsound_stream_get_startup_status(stream, SCRATCH_DATA+1024u));
        CHECK(fatal_seen);
        store(stream + 4u * word, before.header[word]);
        unchanged(stream, &before);
    }
    for (unsigned word = 0u; word < 11u; word++) {
        uint32_t original = load(device + 4u * word);
        store(device + 4u * word, original ^ 1u);
        RUN_EXPECTING_FATAL((void)dsound_stream_get_startup_status(stream, SCRATCH_DATA+1024u));
        CHECK(fatal_seen);
        store(device + 4u * word, original);
        unchanged(stream, &before);
    }
    const uint32_t pages[] = {stream & ~4095u, device & ~4095u, 0x412000u};
    for (unsigned i = 0u; i < sizeof(pages) / sizeof(pages[0]); i++) {
        CHECK(mprotect((void *)(uintptr_t)pages[i], 4096u, PROT_NONE) == 0);
        RUN_EXPECTING_FATAL((void)dsound_stream_get_startup_status(stream, SCRATCH_DATA+1024u));
        CHECK(fatal_seen);
        CHECK(mprotect((void *)(uintptr_t)pages[i], 4096u, PROT_READ | PROT_WRITE) == 0);
        unchanged(stream, &before);
    }
}
static void test_outputs(uint32_t stream,uint32_t other)
{
    dsound_stream_snapshot before;
    CHECK(dsound_stream_get_snapshot(stream,&before));
    dsound_stream_snapshot other_before;
    CHECK(dsound_stream_get_snapshot(other,&other_before));
    CHECK_EQ_U32(dsound_stream_get_startup_status(stream,other_before.scope.format_address+12u),0u);
    CHECK_EQ_U32(load(other_before.scope.format_address+12u),1u);
    unchanged(stream,&before);unchanged(other,&other_before);
    uint32_t output=SCRATCH_DATA+1024u;
    uint32_t aliases[]={0u,stream,device,0x412B30u,0x4124A8u,0x4A1CF0u,0x4A1D00u,
      0x4B914Cu,before.publication_address,0xFFFFFFFEu};
    for(unsigned i=0u;i<sizeof(aliases)/sizeof(aliases[0]);i++) {
        RUN_EXPECTING_FATAL((void)dsound_stream_get_startup_status(stream,aliases[i]));
        CHECK(fatal_seen);unchanged(stream,&before);
    }
    uint32_t reused[]={before.scope.descriptor_address,before.scope.format_address,
        before.i3dl2_address};
    for(unsigned i=0u;i<sizeof(reused)/sizeof(reused[0]);i++) {
        CHECK_EQ_U32(dsound_stream_get_startup_status(stream,reused[i]),0u);
        CHECK_EQ_U32(load(reused[i]),1u);unchanged(stream,&before);
    }
    store(output,0xA5A5A5A5u);store(output+4u,0x12345678u);
    CHECK_EQ_U32(dsound_stream_get_startup_status(stream,output),0u);
    CHECK_EQ_U32(load(output),1u);CHECK_EQ_U32(load(output+4u),0x12345678u);
    uint32_t page=output&~4095u;
    CHECK(mprotect((void *)(uintptr_t)page,4096u,PROT_READ)==0);
    RUN_EXPECTING_FATAL((void)dsound_stream_get_startup_status(stream,output));
    CHECK(fatal_seen);CHECK_EQ_U32(load(output),1u);
    CHECK(mprotect((void *)(uintptr_t)page,4096u,PROT_NONE)==0);
    RUN_EXPECTING_FATAL((void)dsound_stream_get_startup_status(stream,output));
    CHECK(fatal_seen);
    CHECK(mprotect((void *)(uintptr_t)page,4096u,PROT_READ|PROT_WRITE)==0);
    uint32_t cross=0x36000000u;map_fixed(cross,8192u);store(cross+4094u,0xABCDEF01u);
    CHECK(mprotect((void *)(uintptr_t)(cross+4096u),4096u,PROT_READ)==0);
    RUN_EXPECTING_FATAL((void)dsound_stream_get_startup_status(stream,cross+4094u));
    CHECK(fatal_seen);CHECK_EQ_U32(load(cross+4094u),0xABCDEF01u);
    CHECK(mprotect((void *)(uintptr_t)(cross+4096u),4096u,PROT_READ|PROT_WRITE)==0);
    unchanged(stream,&before);
}
static void test_stale_child(uint32_t stream)
{
    dsound_stream_snapshot before;
    CHECK(dsound_stream_get_snapshot(stream, &before));
    CHECK(guest_heap_destroy(before.stream_heap));
    map_fixed(stream & ~4095u, 4096u);
    store(stream, 0xFACECAFEu);
    RUN_EXPECTING_FATAL((void)dsound_stream_get_startup_status(stream, SCRATCH_DATA+1024u));
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
    uint32_t stream = create_stream(true, SCRATCH_DATA + 128u);
    uint32_t spatial = create_stream(false, SCRATCH_DATA + 132u);
    dsound_stream_snapshot unready;
    CHECK(dsound_stream_get_snapshot(stream, &unready));
    RUN_EXPECTING_FATAL((void)dsound_stream_get_startup_status(stream, SCRATCH_DATA+1024u));
    CHECK(fatal_seen);
    unchanged(stream, &unready);
    uint32_t params[9] = {0u,0u,0xFFFFF448u,0u,0u,0u,0u,0u,0u};
    CHECK(kernel_guest_write_bytes(SCRATCH_DATA + 512u, params, sizeof(params)));
    CHECK_EQ_U32(dsound_stream_cache_i3dl2(stream,SCRATCH_DATA+512u,0u),0u);
    RUN_EXPECTING_FATAL((void)dsound_stream_get_startup_status(stream, SCRATCH_DATA+1024u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(dsound_stream_cache_min_distance(stream,0x3F800000u,0u),0u);
    RUN_EXPECTING_FATAL((void)dsound_stream_get_startup_status(stream, SCRATCH_DATA+1024u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(dsound_stream_cache_rolloff(stream,0x4B914Cu,4u,0u),0u);
    RUN_EXPECTING_FATAL((void)dsound_stream_get_startup_status(stream,SCRATCH_DATA+1024u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(dsound_stream_cache_volume(stream,-10000),0u);
    test_success_and_permissions(stream);
    test_scope_and_ownership(stream, spatial);
    test_outputs(stream,spatial);
    CHECK_EQ_U32(dsound_stream_register(), 10u);

    CHECK(dsound_hle_entry(0x40733Bu) == NULL);
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
    RUN_EXPECTING_FATAL((void)dsound_stream_get_startup_status(stream, SCRATCH_DATA+1024u));
    CHECK(fatal_seen);
    CHECK(!dsound_stream_get_snapshot(stream, &sentinel));
    CHECK(dsound_device_reset_checked());
    environment_end();
    printf("passive spatial startup status: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
