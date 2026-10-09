/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A verified LockRect alias must invalidate the cache's guest-virtual read span. */
#include "test_d3d8_support.h"

#include "d3d8_gpu_pgraph.h"
#include "d3d8_lock.h"
#include "d3d8_resource.h"
#include "live_texture.h"
#include "live_texture_watch.h"

#define HEADER_A 0x00665500u
#define HEADER_B 0x00665520u
#define DATA_A 0x9125A000u
#define DATA_B 0xA025A000u
#define FORMAT_SWIZZLED 0x01110629u
#define TEXTURE_BYTES 16u

static void initialize_header(uint32_t header, uint32_t base)
{
    store(header, 0x00040001u);
    store(header + 4u, 0u);
    store(header + 8u, 0u);
    store(header + 12u, FORMAT_SWIZZLED);
    store(header + 16u, 0u);
    CHECK_EQ_U32(d3d8_register_resource(header, base), base & 0x0FFFFFFFu);
}

static void test_lock_notifies_only_the_matching_virtual_alias(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(0x00665000u, 0x1000u);
    store_byte(0x003E1828u + 6u, 0x20u);
    store(0x003E3F58u, D3D8_DEVICE_BASE);

    guest_region_request request = {.bytes = 0x4000u,
                                    .fixed_base = DATA_A,
                                    .contiguous = true,
                                    .state = MEM_COMMIT,
                                    .protect = PAGE_READWRITE};
    nt_status status = STATUS_SUCCESS;
    CHECK_EQ_U32(guest_region_alloc(&request, &status), DATA_A);
    request.fixed_base = DATA_B;
    CHECK_EQ_U32(guest_region_alloc(&request, &status), DATA_B);
    initialize_header(HEADER_A, DATA_A);
    initialize_header(HEADER_B, DATA_B);
    CHECK(load(HEADER_A + 4u) != load(HEADER_B + 4u));

    live_texture_cache cache;
    live_texture_cache_init(&cache, true);
    const live_texture_binding binding_a = {HEADER_A, FORMAT_SWIZZLED, 0u, load(HEADER_A + 4u), 0u};
    const live_texture_binding binding_b = {HEADER_B, FORMAT_SWIZZLED, 0u, load(HEADER_B + 4u), 0u};
    live_texture_result first_a, first_b, fresh_a, retained_b;
    live_texture_lookup_resolved(&cache, &binding_a, d3d8_gpu_read_virtual, NULL,
                                 d3d8_gpu_resolve_texture, NULL, &first_a);
    live_texture_lookup_resolved(&cache, &binding_b, d3d8_gpu_read_virtual, NULL,
                                 d3d8_gpu_resolve_texture, NULL, &first_b);
    CHECK(first_a.source == LIVE_TEXTURE_SOURCE_GUEST && first_a.needs_upload);
    CHECK(first_b.source == LIVE_TEXTURE_SOURCE_GUEST && first_b.needs_upload);
    live_texture_mark_uploaded(&cache, first_a.entry, first_a.generation);
    live_texture_mark_uploaded(&cache, first_b.entry, first_b.generation);

    live_texture_watch_reset();
    CHECK_EQ_U32(d3d8_texture_lock_rect(HEADER_A, 0u, SCRATCH_DATA, 0u, 0x20u), SCRATCH_DATA);
    uint32_t noted_address = 0u, noted_bytes = 0u;
    CHECK(live_texture_watch_pending(0u, &noted_address, &noted_bytes));
    CHECK(!live_texture_watch_pending(1u, &noted_address, &noted_bytes));
    CHECK_EQ_U32(noted_address, load(HEADER_A + 4u));
    CHECK_EQ_U32(noted_bytes, TEXTURE_BYTES);

    const uint8_t changed[4] = {0xD1u, 0xA2u, 0xB3u, 0xC4u};
    CHECK(kernel_guest_write_bytes(DATA_A, changed, sizeof(changed)));
    CHECK_EQ_U32(live_texture_watch_drain(&cache), 1u);
    CHECK_EQ_U32(cache.invalidations, 1u);
    live_texture_lookup_resolved(&cache, &binding_a, d3d8_gpu_read_virtual, NULL,
                                 d3d8_gpu_resolve_texture, NULL, &fresh_a);
    live_texture_lookup_resolved(&cache, &binding_b, d3d8_gpu_read_virtual, NULL,
                                 d3d8_gpu_resolve_texture, NULL, &retained_b);
    CHECK(fresh_a.needs_upload);
    CHECK_EQ_U32(fresh_a.generation, first_a.generation + 1u);
    CHECK(!retained_b.needs_upload);
    CHECK_EQ_U32(retained_b.generation, first_b.generation);

    live_texture_cache_free(&cache);
    environment_end();
}

static void test_modified_binding_refuses_before_queuing_watch(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(0x00665000u, 0x1000u);
    store_byte(0x003E1828u + 6u, 0x20u);
    store(0x003E3F58u, D3D8_DEVICE_BASE);
    guest_region_request request = {.bytes = 0x4000u,
                                    .fixed_base = DATA_A,
                                    .contiguous = true,
                                    .state = MEM_COMMIT,
                                    .protect = PAGE_READWRITE};
    nt_status status = STATUS_SUCCESS;
    CHECK_EQ_U32(guest_region_alloc(&request, &status), DATA_A);
    initialize_header(HEADER_A, DATA_A);
    store(HEADER_A, 0x01040001u); /* Immutable Common identity changed; type/ref remain valid. */
    live_texture_watch_reset();
    RUN_EXPECTING_FATAL((void)d3d8_texture_lock_rect(HEADER_A, 0u, SCRATCH_DATA, 0u, 0x20u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003D4E60u);
    uint32_t noted_address = 0u, noted_bytes = 0u;
    CHECK(!live_texture_watch_pending(0u, &noted_address, &noted_bytes));
    environment_end();
}

int main(void)
{
    test_lock_notifies_only_the_matching_virtual_alias();
    test_modified_binding_refuses_before_queuing_watch();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
