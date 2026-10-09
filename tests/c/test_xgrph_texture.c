/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "xgrph_hle.h"
#include "xgrph_texture.h"
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */
#define HEADER 0x00665504u
static void initialise(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(0x00402000u, 0x1000u);
    map_fixed(0x00665000u, 0x1000u);
    xgrph_hle_set_fatal(catching_fatal);
    /* Decoded metadata: swizzled 32bpp, DXT1 4bpp, linear 32bpp. */
    CHECK(kernel_guest_write_u8(0x00402D77u, 0xA1u));
    CHECK(kernel_guest_write_u8(0x00402D7Cu, 4u));
    CHECK(kernel_guest_write_u8(0x00402D82u, 0xA2u));
    for (uint32_t i = 0u; i < 8u; i++) store(HEADER + i * 4u, 0xAAAAAAAAu);
}
static void test_measured_cube_and_tail(void)
{
    initialise();
    CHECK_EQ_U32(xgrph_set_cube_texture_header(64u, 1u, 0u, 7u, 0u, HEADER, 0u, 256u), 0x18000u);
    const uint32_t expected[5] = {0x40001u, 0u, 0u, 0x0661072Du, 0u};
    for (uint32_t i = 0u; i < 5u; i++) CHECK_EQ_U32(load(HEADER + i * 4u), expected[i]);
    for (uint32_t i = 5u; i < 8u; i++) CHECK_EQ_U32(load(HEADER + i * 4u), 0xAAAAAAAAu);
    CHECK_EQ_U32(xgrph_set_cube_texture_header(64u, 1u, 0x10000u, 7u, 9u, HEADER, 0xDEADBEEFu, 1u), 0x18000u);
    CHECK_EQ_U32(load(HEADER + 4u), 0xDEADBEEFu);
    CHECK_EQ_U32(load(HEADER + 12u), 0x06610725u);
    /* BSF uses trailing zeroes: non-power-of-two edge 3 has exponent zero. */
    CHECK_EQ_U32(xgrph_set_cube_texture_header(3u, 0u, 0u, 7u, 0u, HEADER, 0u, 0u), 768u);
    CHECK_EQ_U32(load(HEADER + 12u), 0x0001072Du);
    /* For compressed formats each mip retains the minimum 4x4 block. */
    CHECK_EQ_U32(xgrph_set_cube_texture_header(4u, 0u, 0u, 12u, 0u, HEADER, 0u, 0u), 768u);
    CHECK_EQ_U32(load(HEADER + 12u), 0x02230C2Du);
    environment_end();
}
static void test_linear_and_large_level_tail(void)
{
    initialise();
    CHECK_EQ_U32(xgrph_set_cube_texture_header(64u, 0u, 0u, 18u, 0u, HEADER, 0u, 0u), 16384u);
    CHECK_EQ_U32(load(HEADER + 12u), 0x0001122Du);
    CHECK_EQ_U32(load(HEADER + 16u), 0x0303F03Fu);
    CHECK_EQ_U32(xgrph_set_cube_texture_header(0u, 0u, 0u, 18u, 0u, HEADER, 0u, 17u), 0u);
    CHECK_EQ_U32(load(HEADER + 16u), 0u);
    /* UINT32_MAX levels: constant mip tail and alignment wrap modulo 2^32. */
    CHECK_EQ_U32(xgrph_set_cube_texture_header(1u, UINT32_MAX, 0u, 7u, 0u, HEADER, 0u, 0u), 0u);
    CHECK_EQ_U32(load(HEADER + 12u), 0xFFFF072Du);
    environment_end();
}
static void test_preflight_and_dispatch_abi(void)
{
    initialise();
    RUN_EXPECTING_FATAL((void)xgrph_set_cube_texture_header(64u,1u,0u,64u,0u,HEADER,0u,0u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003E66AEu);
    CHECK_EQ_U32(load(HEADER), 0xAAAAAAAAu);
    RUN_EXPECTING_FATAL((void)xgrph_set_cube_texture_header(0u,1u,0u,7u,0u,HEADER,0u,0u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(load(HEADER), 0xAAAAAAAAu);
    store(0x00665FF0u, 0xBBBBBBBBu);
    RUN_EXPECTING_FATAL((void)xgrph_set_cube_texture_header(64u,1u,0u,7u,0u,0x00665FF0u,0u,0u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(load(0x00665FF0u), 0xBBBBBBBBu);
    const xgrph_surface_entry row = {0x003E66AEu, "cube header", 1u};
    CHECK(xgrph_hle_init(&row, 1u));
    CHECK_EQ_U32(xgrph_texture_register(), 1u);
    const uint32_t args[8] = {64u,1u,0u,7u,0u,HEADER,0x12345678u,256u};
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    CHECK(kernel_frame_build(&frame, call_scratch, 0x100u, args, 8u));
    CHECK_EQ_U32(xgrph_hle_call(0x003E66AEu, &frame), 0x18000u);
    CHECK_EQ_U32(load(HEADER + 4u), 0x12345678u);
    CHECK_EQ_U32(load(HEADER + 12u), 0x0661072Du);
    xgrph_hle_shutdown();
    environment_end();
}
static void test_2d_header_and_guard(void)
{
    initialise();
    CHECK(kernel_guest_write_u8(0x00402D76u, 0xA1u));
    CHECK_EQ_U32(xgrph_set_texture_header(4096u, 1u, 1u, 0u, 6u, 0u,
                                         HEADER, 0u, 16384u), 0x4000u);
    CHECK_EQ_U32(load(HEADER + 12u), 0x00C10629u);
    CHECK_EQ_U32(xgrph_set_texture_header(64u, 32u, 1u, 0u, 6u, 99u, HEADER, 0u, 256u), 8192u);
    const uint32_t expected[5]={0x40001u, 0u, 0u, 0x05610629u, 0u};
    for (uint32_t i = 0u; i < 5u; i++) CHECK_EQ_U32(load(HEADER+i*4u), expected[i]);
    for (uint32_t i=5u;i<8u; i++) CHECK_EQ_U32(load(HEADER+i*4u), 0xAAAAAAAAu);
    CHECK_EQ_U32(xgrph_set_texture_header(64u, 32u, 0u, 0x10000u, 18u, 0u, HEADER, 7u, 0u), 8192u);
    CHECK_EQ_U32(load(HEADER+16u), 0x0301F03Fu);
    CHECK_EQ_U32(xgrph_set_texture_header(1u, 1u, UINT32_MAX, 0u, 6u, 0u, HEADER, 0u, 0u), 0xFFFFFFFCu);
    uint32_t preserved[8];for (uint32_t i = 0u; i < 8u; i++)preserved[i]=load(HEADER+i*4u);
    RUN_EXPECTING_FATAL((void)xgrph_set_texture_header(0u, 32u, 1u, 0u, 6u, 0u, HEADER, 0u, 0u));
    CHECK(fatal_seen);CHECK_EQ_U32(fatal_address, 0x3E6684u);
    RUN_EXPECTING_FATAL((void)xgrph_set_texture_header(64u, 32u, 1u, 0u, 64u, 0u, HEADER, 0u, 0u));
    CHECK(fatal_seen);
    CHECK(mprotect((void *)(uintptr_t)0x665000u, 0x1000u, PROT_READ)==0);
    RUN_EXPECTING_FATAL((void)xgrph_set_texture_header(64u, 32u, 1u, 0u, 6u, 0u, HEADER, 0u, 0u));
    CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)xgrph_set_cube_texture_header(64u, 1u, 0u, 7u, 0u, HEADER, 0u, 256u));
    CHECK(fatal_seen); CHECK_EQ_U32(fatal_address, 0x3E66AEu);
    CHECK(mprotect((void *)(uintptr_t)0x665000u, 0x1000u, PROT_READ|PROT_WRITE)==0);
    for (uint32_t i = 0u; i < 8u; i++) CHECK_EQ_U32(load(HEADER+i*4u), preserved[i]);
    const xgrph_surface_entry row={0x3E6684u, "2D header", 1u};
    CHECK(xgrph_hle_init(&row, 1u));CHECK_EQ_U32(xgrph_texture_register(), 1u);
    const uint32_t a[9]={64u, 32u, 1u, 0u, 6u, 0u, HEADER, 0x12345678u, 256u};
    kernel_call_frame frame;memset(&frame, 0, sizeof(frame));
    CHECK(kernel_frame_build(&frame, call_scratch, 0x100u, a, 9u));
    CHECK_EQ_U32(xgrph_hle_call(0x3E6684u, &frame), 8192u);
    CHECK_EQ_U32(load(HEADER+4u), 0x12345678u);
    xgrph_hle_shutdown();environment_end();
}
int main(void)
{
    test_2d_header_and_guard();
    test_measured_cube_and_tail();
    test_linear_and_large_level_tail();
    test_preflight_and_dispatch_abi();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
