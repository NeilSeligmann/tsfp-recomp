/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Independent registered-Data address, bound-span and cache-lifetime controls. */

#include "test_d3d8_support.h"

#include "d3d8_gpu_pgraph.h"
#include "d3d8_resource.h"
#include "live_texture.h"
#include "live_texture_watch.h"

#define REVIEW_HEADER 0x00665500u
#define LOW_READABLE 0x0125A000u
#define ALLOCATION 0x9125A000u
#define TEXTURE_FORMAT 0x00011229u /* measured linear A8R8G8B8 */
#define TEXTURE_SIZE 0x00003003u   /* 4x4 texels, 64-byte pitch */
#define ALLOCATION_OFFSET 16u
#define SOURCE_BYTES 256u

static void test_registered_data_is_offset_into_live_allocation(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(0x00665000u, 0x1000u);
    map_fixed(LOW_READABLE, 0x1000u); /* readable decoy at the registered low-28-bit Data */

    guest_region_request request = {.bytes = ALLOCATION_OFFSET + SOURCE_BYTES,
                                    .fixed_base = ALLOCATION,
                                    .contiguous = true,
                                    .state = MEM_COMMIT,
                                    .protect = PAGE_READWRITE};
    nt_status status = STATUS_SUCCESS;
    CHECK_EQ_U32(guest_region_alloc(&request, &status), ALLOCATION);
    const uint32_t data_offset = ALLOCATION_OFFSET;
    store(REVIEW_HEADER, 0x00040001u);
    store(REVIEW_HEADER + 4u, data_offset);
    store(REVIEW_HEADER + 8u, 0u);
    store(REVIEW_HEADER + 12u, TEXTURE_FORMAT);
    store(REVIEW_HEADER + 16u, TEXTURE_SIZE);
    const uint32_t data = d3d8_register_resource(REVIEW_HEADER, ALLOCATION);
    CHECK_EQ_U32(data, 0x0125A010u); /* original Register's (base + Data) & 0x0fffffff */

    const uint8_t marker[4] = {0x19u, 0x2Au, 0x3Bu, 0x4Cu};
    CHECK(kernel_guest_write_bytes(ALLOCATION + ALLOCATION_OFFSET, marker, sizeof(marker)));
    uint8_t raw[4] = {0};
    CHECK(d3d8_gpu_read_guest(NULL, data, raw, sizeof(raw)));
    CHECK_EQ_U32(raw[0], 0u); /* ordinary Data-as-VA lands on the readable decoy */

    live_texture_binding binding = {REVIEW_HEADER, TEXTURE_FORMAT, TEXTURE_SIZE, data, 0u};
    uint32_t resolved = 0u;
    uint64_t identity = 0u;
    const char *refusal = NULL;
    CHECK(d3d8_gpu_resolve_texture(NULL, &binding, SOURCE_BYTES, &resolved, &identity, &refusal));
    CHECK_EQ_U32(resolved, ALLOCATION + ALLOCATION_OFFSET);
    CHECK(identity != 0u);
    CHECK(d3d8_gpu_read_virtual(NULL, resolved, raw, sizeof(raw)));
    CHECK(memcmp(raw, marker, sizeof(marker)) == 0);

    uint32_t final_byte = 0u;
    uint64_t final_identity = 0u;
    CHECK(d3d8_resource_try_alias(REVIEW_HEADER, data + SOURCE_BYTES - 1u, 1u,
                                 &final_byte, &final_identity, &refusal) == D3D8_RESOURCE_ALIAS_RESOLVED);
    CHECK_EQ_U32(final_byte, ALLOCATION + ALLOCATION_OFFSET + SOURCE_BYTES - 1u);
    CHECK(d3d8_resource_try_alias(REVIEW_HEADER, data + SOURCE_BYTES - 1u, 2u,
                                  &final_byte, &final_identity, &refusal) == D3D8_RESOURCE_ALIAS_REFUSED);
    CHECK(d3d8_resource_try_alias(REVIEW_HEADER, data - 1u, 1u,
                                  &final_byte, &final_identity, &refusal) == D3D8_RESOURCE_ALIAS_REFUSED);
    CHECK(d3d8_resource_try_alias(REVIEW_HEADER, data, 0u,
                                  &final_byte, &final_identity, &refusal) == D3D8_RESOURCE_ALIAS_REFUSED);

    store(REVIEW_HEADER + 12u, TEXTURE_FORMAT ^ 0x100u);
    CHECK(!d3d8_gpu_resolve_texture(NULL, &binding, SOURCE_BYTES, &resolved, &identity, &refusal));
    CHECK(refusal != NULL && strstr(refusal, "header") != NULL);
    store(REVIEW_HEADER + 12u, TEXTURE_FORMAT);
    CHECK(guest_region_free(ALLOCATION));
    CHECK(!d3d8_gpu_resolve_texture(NULL, &binding, SOURCE_BYTES, &resolved, &identity, &refusal));
    CHECK(refusal != NULL && strstr(refusal, "stale") != NULL);

    environment_end();
}

static void test_direct_write_and_reregister_do_not_reuse_stale_texture(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(0x00665000u, 0x1000u);
    guest_region_request request = {.bytes = ALLOCATION_OFFSET + SOURCE_BYTES,
                                    .fixed_base = ALLOCATION,
                                    .contiguous = true,
                                    .state = MEM_COMMIT,
                                    .protect = PAGE_READWRITE};
    nt_status status = STATUS_SUCCESS;
    CHECK_EQ_U32(guest_region_alloc(&request, &status), ALLOCATION);
    store(REVIEW_HEADER, 0x00040001u);
    store(REVIEW_HEADER + 4u, ALLOCATION_OFFSET);
    store(REVIEW_HEADER + 12u, TEXTURE_FORMAT);
    store(REVIEW_HEADER + 16u, TEXTURE_SIZE);
    const uint32_t data = d3d8_register_resource(REVIEW_HEADER, ALLOCATION);
    live_texture_binding binding = {REVIEW_HEADER, TEXTURE_FORMAT, TEXTURE_SIZE, data, 0u};
    live_texture_cache cache;
    live_texture_cache_init(&cache, false);
    live_texture_result first, rewritten, reregistered;
    live_texture_lookup_resolved(&cache, &binding, d3d8_gpu_read_virtual, NULL,
                                 d3d8_gpu_resolve_texture, NULL, &first);
    CHECK(first.source == LIVE_TEXTURE_SOURCE_GUEST && first.needs_upload);
    live_texture_mark_uploaded(&cache, first.entry, first.generation);

    /* T937 taught the resolved cache to match a verified binding's masked Data interval
     * as well as its host guest-virtual read address. This note invalidates this alias;
     * the following backing-byte change still exercises snapshot-based refresh. */
    live_texture_watch_reset();
    live_texture_watch_note(data, SOURCE_BYTES);
    CHECK(live_texture_watch_drain(&cache) == 1u);
    CHECK(cache.invalidations == 1u);
    const uint8_t fresh_pixel[4] = {0xA1u, 0xB2u, 0xC3u, 0xD4u};
    CHECK(kernel_guest_write_bytes(ALLOCATION + ALLOCATION_OFFSET, fresh_pixel, sizeof(fresh_pixel)));
    live_texture_lookup_resolved(&cache, &binding, d3d8_gpu_read_virtual, NULL,
                                 d3d8_gpu_resolve_texture, NULL, &rewritten);
    CHECK(rewritten.source == LIVE_TEXTURE_SOURCE_GUEST && rewritten.needs_upload);
    CHECK(rewritten.generation == first.generation + 1u);
    CHECK(cache.invalidations == 1u);

    /* Reset Data to the pre-Register offset; Register writes the same Data back but creates
     * a fresh binding identity, so the cache must not reuse the previous entry. */
    store(REVIEW_HEADER + 4u, ALLOCATION_OFFSET);
    CHECK_EQ_U32(d3d8_register_resource(REVIEW_HEADER, ALLOCATION), data);
    live_texture_lookup_resolved(&cache, &binding, d3d8_gpu_read_virtual, NULL,
                                 d3d8_gpu_resolve_texture, NULL, &reregistered);
    CHECK(reregistered.source == LIVE_TEXTURE_SOURCE_GUEST && reregistered.needs_upload);
    CHECK(reregistered.entry != rewritten.entry);

    live_texture_cache_free(&cache);
    environment_end();
}

static void test_rejected_registered_alias_does_not_fall_back_to_readable_low_ram(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(0x00665000u, 0x1000u);
    map_fixed(LOW_READABLE, 0x1000u);
    const uint32_t header = REVIEW_HEADER + 32u;
    store(header, 0x00040001u);
    store(header + 4u, 0u);
    store(header + 12u, TEXTURE_FORMAT);
    store(header + 16u, TEXTURE_SIZE);

    /* A fixed mapped region is readable but is not a verified contiguous allocation.
     * Register records that rejection; resolving must preserve it instead of reading it. */
    const uint32_t data = d3d8_register_resource(header, LOW_READABLE);
    live_texture_binding binding = {header, TEXTURE_FORMAT, TEXTURE_SIZE, data, 0u};
    uint32_t address = 0u;
    uint64_t identity = 0u;
    const char *refusal = NULL;
    CHECK(!d3d8_gpu_resolve_texture(NULL, &binding, SOURCE_BYTES, &address, &identity, &refusal));
    CHECK(refusal != NULL && strstr(refusal, "contiguous") != NULL);

    environment_end();
}

int main(void)
{
    test_registered_data_is_offset_into_live_allocation();
    test_direct_write_and_reregister_do_not_reuse_stale_texture();
    test_rejected_registered_alias_does_not_fall_back_to_readable_low_ram();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
