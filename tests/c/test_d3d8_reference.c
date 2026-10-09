/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_reference.h"
#include "d3d8_bind.h"
#include "d3d8_cube_surface.h"
#define ROOT 0x00665504u
#define PARENT (ROOT + 0x100u)
static void begin(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    d3d8_cube_surface_reset();
    map_fixed(0x00665000u, 0x3000u);
    store_byte(0x003E1828u + 7u, 0xA1u);
    const uint32_t words[] = {0x40001u, 0x1234000u, 0u, 0x0661072Du, 0u, 0u};
    for (uint32_t i = 0u; i < 6u; i++) store(ROOT + i * 4u, words[i]);
    store(0x003E2BA4u, 0u);
}
static void end(void) { d3d8_cube_surface_reset(); environment_end(); }
static void test_pure_counts_and_recursive_order(void)
{
    begin();
    store(ROOT, 0xFFFFFFFFu);
    CHECK_EQ_U32(d3d8_reference_add_ref(ROOT), 0u);
    CHECK_EQ_U32(load(ROOT), 0u);
    CHECK_EQ_U32(d3d8_reference_release(ROOT), 0xFFFFu);
    CHECK_EQ_U32(load(ROOT), 0xFFFFFFFFu);
    store(ROOT, 0x01050000u); store(ROOT + 20u, PARENT); store(PARENT, 0x40001u);
    CHECK_EQ_U32(d3d8_reference_add_ref(ROOT), 1u);
    CHECK_EQ_U32(load(PARENT), 0x40002u);
    CHECK_EQ_U32(d3d8_reference_add_ref(ROOT), 2u);
    CHECK_EQ_U32(load(PARENT), 0x40002u);
    CHECK_EQ_U32(d3d8_reference_release(ROOT), 1u);
    store(ROOT, 0x010D0001u);
    CHECK_EQ_U32(d3d8_reference_release(ROOT), 0u);
    CHECK_EQ_U32(load(ROOT), 0x010D0000u);
    CHECK_EQ_U32(load(PARENT), 0x40001u);
    CHECK_EQ_U32(d3d8_reference_add_ref(ROOT), 1u);
    CHECK_EQ_U32(load(PARENT), 0x40002u);
    end();
}
static void test_owned_free_and_unsupported_parent_are_atomic(void)
{
    begin();
    const uint32_t child = d3d8_get_cube_map_surface2(ROOT, 0u, 0u);
    CHECK(d3d8_cube_surface_owned(child));
    CHECK(!d3d8_cube_surface_owned(child + 4u));
    CHECK_EQ_U32(d3d8_reference_release(child), 0u);
    CHECK_EQ_U32(load(ROOT), 0x40001u);
    CHECK(!d3d8_cube_surface_owned(child));
    CHECK(d3d8_cube_surface_retired(child));
    RUN_EXPECTING_FATAL((void)d3d8_reference_release(child));
    CHECK(fatal_seen);
    CHECK_EQ_U32(load(ROOT), 0x40001u);
    RUN_EXPECTING_FATAL((void)d3d8_reference_add_ref(child));
    CHECK(fatal_seen);
    const uint32_t again = d3d8_get_cube_map_surface2(ROOT, 1u, 0u);
    CHECK(d3d8_cube_surface_owned(again));
    CHECK(!d3d8_cube_surface_retired(again));
    store(again + 4u, 0x50001u);
    RUN_EXPECTING_FATAL((void)d3d8_reference_release(again + 4u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(load(again + 4u), 0x50001u);
    CHECK(d3d8_cube_surface_owned(again));
    store(ROOT, 0x40001u);
    RUN_EXPECTING_FATAL((void)d3d8_reference_release(again));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x3D4B30u);
    CHECK_EQ_U32(load(ROOT), 0x40001u);
    CHECK_EQ_U32(load(again), 0x01050001u);
    CHECK(d3d8_cube_surface_owned(again));
    store(ROOT, 0x40002u);
    store(0x3E2BA4u, ROOT);
    RUN_EXPECTING_FATAL((void)d3d8_reference_release(again));
    CHECK(fatal_seen);
    CHECK_EQ_U32(load(ROOT), 0x40002u);
    CHECK_EQ_U32(load(again), 0x01050001u);
    store(0x3E2BA4u, 0u);
    store(again, 0x81050001u);
    RUN_EXPECTING_FATAL((void)d3d8_reference_release(again));
    CHECK(fatal_seen);
    CHECK_EQ_U32(load(ROOT), 0x40002u);
    CHECK_EQ_U32(load(again), 0x81050001u);
    end();
}
static void test_owned_parent_chain(void)
{
    begin();
    uint32_t parent = d3d8_get_cube_map_surface2(ROOT, 0u, 0u);
    uint32_t child = d3d8_get_cube_map_surface2(parent, 0u, 0u);
    store(parent, 0x01050001u);
    store(parent + 20u, child);
    RUN_EXPECTING_FATAL((void)d3d8_reference_release(child));
    CHECK(fatal_seen);
    CHECK_EQ_U32(load(parent), 0x01050001u);
    CHECK_EQ_U32(load(child), 0x01050001u);
    CHECK(d3d8_cube_surface_owned(parent) && d3d8_cube_surface_owned(child));
    store(parent + 20u, ROOT);
    CHECK_EQ_U32(d3d8_reference_release(child), 0u);
    CHECK(d3d8_cube_surface_retired(parent));
    CHECK(d3d8_cube_surface_retired(child));
    CHECK_EQ_U32(load(ROOT), 0x40001u);
    end();
}
static void test_cycles_unmapped_depth_and_binding_preflight(void)
{
    begin();
    store(ROOT, 0x50000u); store(ROOT + 20u, ROOT);
    RUN_EXPECTING_FATAL((void)d3d8_reference_add_ref(ROOT));
    CHECK(fatal_seen); CHECK_EQ_U32(load(ROOT), 0x50000u);
    store(ROOT + 20u, 0xDEADBEEFu);
    RUN_EXPECTING_FATAL((void)d3d8_reference_add_ref(ROOT));
    CHECK(fatal_seen); CHECK_EQ_U32(load(ROOT), 0x50000u);
    for (uint32_t i = 0u; i < 65u; i++) {
        store(ROOT + i * 32u, 0x50000u);
        store(ROOT + i * 32u + 20u, i == 64u ? 0u : ROOT + (i + 1u) * 32u);
    }
    RUN_EXPECTING_FATAL((void)d3d8_reference_add_ref(ROOT));
    CHECK(fatal_seen);
    for (uint32_t i = 0u; i < 65u; i++) CHECK_EQ_U32(load(ROOT + i * 32u), 0x50000u);
    store(ROOT, 0xD0001u); store(ROOT + 20u, PARENT); store(PARENT, 0x80000u);
    RUN_EXPECTING_FATAL(d3d8_reference_release_binding(ROOT));
    CHECK(fatal_seen); CHECK_EQ_U32(load(PARENT), 0x80000u); CHECK_EQ_U32(load(ROOT), 0xD0001u);
    store(PARENT, 0xC0002u);
    d3d8_reference_release_binding(ROOT);
    CHECK_EQ_U32(load(PARENT), 0x40002u); CHECK_EQ_U32(load(ROOT), 0x50001u);
    end();
}
static void test_overlapping_headers_refuse_without_window_changes(void)
{
    begin();
    /* The original would update the parent first and then reload child Common.
     * Distinct unaligned words overlap; a snapshot plan must refuse them. */
    const uint32_t offsets[] = {1u, 2u, 3u, 20u, 23u};
    for (unsigned operation = 0u; operation < 3u; operation++) {
        for (unsigned i = 0u; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
            uint8_t *window = kernel_guest_at(ROOT, 64u);
            memset(window, 0xA5, 64u);
            store(ROOT, operation == 0u ? 0x50000u : 0xD0001u);
            store(ROOT + 20u, ROOT + offsets[i]);
            uint8_t before[64];
            memcpy(before, window, sizeof(before));
            if (operation == 0u) {
                RUN_EXPECTING_FATAL((void)d3d8_reference_add_ref(ROOT));
            } else if (operation == 1u) {
                RUN_EXPECTING_FATAL((void)d3d8_reference_release(ROOT));
            } else {
                RUN_EXPECTING_FATAL(d3d8_reference_release_binding(ROOT));
            }
            CHECK(fatal_seen);
            CHECK_EQ_U32(fatal_address, operation == 0u ? 0x3D4C50u :
                                       operation == 1u ? 0x3D4C90u : 0x3D4DA0u);
            CHECK(memcmp(before, window, sizeof(before)) == 0);
        }
    }
    end();
}

static void test_set_texture_destruction_refuses_before_mutation(void)
{
    begin();
    store(D3D8_DEVICE_BASE + 0xF88u, ROOT);
    store(D3D8_DEVICE_BASE + 0x2Cu, 99u);
    store(ROOT, 0xC0000u);
    store(ROOT + 8u, 123u);
    RUN_EXPECTING_FATAL(d3d8_set_texture(0u, 0u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(load(ROOT), 0xC0000u);
    CHECK_EQ_U32(load(ROOT + 8u), 123u);
    CHECK_EQ_U32(d3d8_device_load32(0xF88u), ROOT);
    end();
}

static void test_stale_heap_identity(void)
{
    begin();
    uint32_t child = d3d8_get_cube_map_surface2(ROOT, 0u, 0u);
    guest_mem_reset();
    CHECK(!d3d8_cube_surface_owned(child));
    CHECK(!d3d8_cube_surface_retired(child));
    uint32_t heap = guest_heap_create(0u, GUEST_HEAP_CHUNK_MIN, 0u);
    CHECK(heap != 0u);
    CHECK(!d3d8_cube_surface_owned(child));
    RUN_EXPECTING_FATAL((void)d3d8_reference_release(child)); CHECK(fatal_seen);
    const uint32_t foreign = guest_heap_alloc(heap, 24u);
    CHECK(foreign != 0u);
    CHECK(!d3d8_cube_surface_owned(foreign));
    map_fixed(D3D_REGION_BASE, D3D_REGION_BYTES);
    map_fixed(0x00665000u, 0x3000u);
    store(ROOT, 0x40001u); store(ROOT + 4u, 0x1234000u);
    store(ROOT + 12u, 0x0661072Du);
    store_byte(0x3E182Fu, 0xA1u);
    const uint32_t renewed = d3d8_get_cube_map_surface2(ROOT, 0u, 0u);
    CHECK(d3d8_cube_surface_owned(renewed));
    CHECK(!d3d8_cube_surface_owned(foreign));
    CHECK(!d3d8_cube_surface_free_owned(foreign));
    uint32_t size;
    CHECK(guest_heap_block_size(heap, foreign, &size));
    CHECK_EQ_U32(size, 24u);
    d3d8_cube_surface_reset();
    CHECK(guest_heap_valid(heap));
    environment_end();
}
int main(void)
{
    test_pure_counts_and_recursive_order();
    test_owned_free_and_unsupported_parent_are_atomic();
    test_owned_parent_chain();
    test_cycles_unmapped_depth_and_binding_preflight();
    test_stale_heap_identity();
    test_set_texture_destruction_refuses_before_mutation();
    test_overlapping_headers_refuse_without_window_changes();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
