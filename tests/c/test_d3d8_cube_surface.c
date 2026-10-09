/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_cube_surface.h"
#define TEXTURE 0x00665504u
static void initialise(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    d3d8_cube_surface_reset();
    map_fixed(0x00665000u, 0x1000u);
    store_byte(0x003E1828u + 7u, 0xA1u);
    const uint32_t words[8] = {0x40001u,0x01234000u,0u,0x0661072Du,0u,0u,0xAAAAAAAAu,0xBBBBBBBBu};
    for (uint32_t i = 0u; i < 8u; i++) store(TEXTURE + i * 4u, words[i]);
}
static void test_real_allocation_face_level_and_teardown(void)
{
    initialise();
    const d3d8_surface_entry row = {0x003D4E90u, NULL, 1u};
    CHECK(d3d8_hle_init(&row, 1u));
    CHECK_EQ_U32(d3d8_cube_surface_register(), 1u);
    const uint32_t arguments[3] = {TEXTURE, 5u, 0u};
    const uint32_t header = call_stdcall(0x003D4E90u, arguments, 3u);
    CHECK(header != 0u);
    CHECK(kernel_guest_at(header, 24u) != NULL);
    const uint32_t expected[6] = {0x01050001u,0x01248000u,0u,0x0661072Du,0u,TEXTURE};
    for (uint32_t i = 0u; i < 6u; i++) CHECK_EQ_U32(load(header + i * 4u), expected[i]);
    CHECK_EQ_U32(load(TEXTURE), 0x40002u);
    CHECK_EQ_U32(load(TEXTURE + 24u), 0xAAAAAAAAu);
    CHECK_EQ_U32(load(TEXTURE + 28u), 0xBBBBBBBBu);
    const uint32_t second = d3d8_get_cube_map_surface2(TEXTURE, 0u, 1u);
    CHECK(second != 0u && second != header);
    CHECK_EQ_U32(load(second + 4u), 0x01238000u);
    CHECK_EQ_U32(load(second + 12u), 0x0551072Du);
    CHECK_EQ_U32(load(TEXTURE), 0x40003u);
    d3d8_cube_surface_reset();
    CHECK(kernel_guest_at(header, 24u) == NULL);
    CHECK(kernel_guest_at(second, 24u) == NULL);
    environment_end();
}
static void test_compression_linear_and_uint32_tail(void)
{
    initialise();
    store(TEXTURE + 12u, 0x02230C2Du);
    uint32_t header = d3d8_get_cube_map_surface2(TEXTURE, 1u, 3u);
    CHECK_EQ_U32(load(header + 4u), 0x01234098u); /* aligned face128 + three8byte blocks */
    CHECK_EQ_U32(load(header + 12u), 0x00030C2Du);
    store(TEXTURE + 12u, 0x0001072Du);
    header = d3d8_get_cube_map_surface2(TEXTURE, 0u, UINT32_MAX);
    CHECK_EQ_U32(load(header + 4u), 0x01233FFCu);
    store(TEXTURE + 12u, 0x0001122Du);
    store(TEXTURE + 16u, 0x0303F03Fu);
    header = d3d8_get_cube_map_surface2(TEXTURE, UINT32_MAX, UINT32_MAX);
    CHECK_EQ_U32(load(header + 4u), 0x01234000u);
    CHECK_EQ_U32(load(header + 12u), 0x0001122Du);
    CHECK_EQ_U32(load(header + 16u), 0x0303F03Fu);
    d3d8_cube_surface_reset();
    environment_end();
}
static void test_refusals_and_real_allocation_failure(void)
{
    initialise();
    store(TEXTURE, 0x50000u);
    store(TEXTURE + 20u, 0xDEADBEEFu);
    RUN_EXPECTING_FATAL((void)d3d8_get_cube_map_surface2(TEXTURE,0u,0u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003D4E90u);
    CHECK_EQ_U32(load(TEXTURE), 0x50000u);
    store(TEXTURE, 0x40001u);
    store(TEXTURE + 12u, 0x0661402Du);
    RUN_EXPECTING_FATAL((void)d3d8_get_cube_map_surface2(TEXTURE,0u,0u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(load(TEXTURE), 0x40001u);
    store(TEXTURE + 12u, 0x0661072Du);
    RUN_EXPECTING_FATAL((void)d3d8_get_cube_map_surface2(0x00665FF0u,0u,0u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(load(TEXTURE), 0x40001u);
    /* Exhaust REAL heap slots, rather than injecting an allocator return value. */
    uint32_t heaps[256];
    uint32_t count = 0u;
    while (count < 256u) {
        const uint32_t heap = guest_heap_create(0u,0u,0u);
        if (heap == 0u) break;
        heaps[count++] = heap;
    }
    CHECK(count > 0u);
    CHECK_EQ_U32(d3d8_get_cube_map_surface2(TEXTURE,0u,0u), 0u);
    CHECK_EQ_U32(load(TEXTURE), 0x40001u);
    bool all_destroyed = true;
    for (uint32_t i = 0u; i < count; i++) all_destroyed = guest_heap_destroy(heaps[i]) && all_destroyed;
    CHECK(all_destroyed);
    const uint32_t recovered = d3d8_get_cube_map_surface2(TEXTURE,0u,0u);
    CHECK(recovered != 0u);
    CHECK_EQ_U32(load(TEXTURE), 0x40002u);
    d3d8_cube_surface_reset();
    environment_end();
}
static void test_stale_heap_reset_preserves_foreign_owner(void)
{
    initialise();
    CHECK(d3d8_get_cube_map_surface2(TEXTURE,0u,0u) != 0u);
    guest_mem_reset(); /* Deliberately leave the module's cached handle stale. */
    const uint32_t foreign = guest_heap_create(0u,0u,0u);
    CHECK(foreign != 0u);
    d3d8_cube_surface_reset();
    CHECK(guest_heap_valid(foreign));
    CHECK(guest_heap_destroy(foreign));
    environment_end();
}
int main(void)
{
    test_real_allocation_face_level_and_teardown();
    test_compression_linear_and_uint32_tail();
    test_refusals_and_real_allocation_failure();
    test_stale_heap_reset_preserves_foreign_owner();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
