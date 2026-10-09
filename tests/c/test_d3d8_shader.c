/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_shader.h"

int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    store(D3D8_DEVICE_POINTER_SLOT, D3D8_DEVICE_BASE);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    CHECK(d3d8_pushbuffer_create());
    store(D3D8_GLOBAL_DIRTY_MASK, 0x100u);
    store(0x003E3F10u, 0xAABBCCDDu);
    for (uint32_t row = 0u; row < 3u; row++) {
        for (uint32_t i = 0u; i < 6u; i++) {
            store(0x003E3B18u + row * 0x80u + i * 4u, row * 100u + i);
        }
    }
    d3d8_device_store32(0x784u, 0x123u);
    d3d8_device_store32(0x788u, 1u);
    const uint32_t start = d3d8_device_load32(D3D8_DEV_CURSOR);
    CHECK_EQ_U32(d3d8_set_pixel_shader(0u), start + 160u);
    CHECK_EQ_U32(d3d8_device_load32(0x784u), 0u);
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK), 0x6900u);
    CHECK_EQ_U32(load(start), 0x00400A60u);
    for (uint32_t i = 0u; i < 16u; i++) CHECK_EQ_U32(load(start + 4u + i * 4u), 0xAABBCCDDu);
    for (uint32_t row = 0u; row < 3u; row++) {
        CHECK_EQ_U32(load(start + 68u + row * 28u), 0x00181B68u + row * 0x40u);
        for (uint32_t i = 0u; i < 6u; i++) {
            CHECK_EQ_U32(load(start + 72u + row * 28u + i * 4u), row * 100u + i);
        }
    }
    CHECK_EQ_U32(load(start + 152u), 0x00041E78u);
    CHECK_EQ_U32(load(start + 156u), 0x00210000u);
    store(D3D8_GLOBAL_DIRTY_MASK, 0u);
    d3d8_device_store32(0x788u, 0u);
    (void)d3d8_set_pixel_shader_v(0u);
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK), 0x4800u);
    /* Nonzero program: every copied word differs so shifted blocks are detected. */
    for (uint32_t i = 0u; i < 60u; i++) store(SCRATCH_DATA + i * 4u, i * 13u + 7u);
    const uint32_t program_start = d3d8_device_load32(D3D8_DEV_CURSOR);
    const uint32_t program_end = d3d8_set_pixel_shader(SCRATCH_DATA);
    CHECK_EQ_U32(program_end - program_start, 84u + 252u);
    CHECK_EQ_U32(d3d8_device_load32(0x784u), D3D8_DEVICE_BASE + 0x924u);
    CHECK_EQ_U32(d3d8_device_load32(0x78Cu), (59u * 13u + 7u) & 0x100u);
    CHECK_EQ_U32(d3d8_device_load32(0x790u), 54u * 13u + 7u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE + 0x92Cu), SCRATCH_DATA);
    for (uint32_t i = 0u; i < 57u; i++) CHECK_EQ_U32(load(0x003E3CC0u + i * 4u), i * 13u + 7u);
    CHECK_EQ_U32(load(0x003E3EE0u), 54u * 13u + 7u);
    const uint32_t headers[] = {0x00200260u, 0x00800A60u, 0x000417F8u,
                                0x00081E20u, 0x00241E40u, 0x00081E74u, 0x00080288u};
    const uint32_t offsets[] = {0u, 10u, 42u, 43u, 45u, 55u, 8u};
    const uint32_t counts[] = {8u, 32u, 1u, 2u, 9u, 2u, 2u};
    uint32_t position = program_start + 84u;
    for (uint32_t block = 0u; block < 7u; block++) {
        CHECK_EQ_U32(load(position), headers[block]);
        position += 4u;
        for (uint32_t i = 0u; i < counts[block]; i++) {
            CHECK_EQ_U32(load(position), (offsets[block] + i) * 13u + 7u);
            position += 4u;
        }
    }
    CHECK_EQ_U32(position, program_end);
    /* Rebinding an existing shader skips the three programmable texture-state rows. */
    const uint32_t second = d3d8_set_pixel_shader(SCRATCH_DATA);
    CHECK_EQ_U32(second - program_end, 252u);
    d3d8_device_store32(D3D8_DEV_FLAGS, 0x10u);
    store(0x003E3CC0u, 0xDEADBEEFu);
    store(SCRATCH_DATA + 32u, 0u);
    store(SCRATCH_DATA + 36u, 0u);
    store(D3D8_GLOBAL_DIRTY_MASK, 0u);
    CHECK_EQ_U32(d3d8_set_pixel_shader(SCRATCH_DATA) - second, 240u);
    CHECK_EQ_U32(load(0x003E3CC0u), 0xDEADBEEFu);
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK), 0x6000u);
    /* Packed vertex constants: nine vectors cross the 32-dword chunk boundary. */
    store(SCRATCH_DATA, (9u << 16) | 0xFFFFu);
    for (uint32_t i = 0u; i < 36u; i++) store(SCRATCH_DATA + 4u + i * 4u, 0xAB000000u + i * 13u);
    d3d8_device_store32(D3D8_DEV_FLAGS, 0u);
    const uint32_t constants = d3d8_device_load32(D3D8_DEV_CURSOR);
    CHECK_EQ_U32(d3d8_upload_vertex_shader_constants(SCRATCH_DATA, 7u) - constants, 160u);
    CHECK_EQ_U32(load(constants), 0x00041E9Cu);
    CHECK_EQ_U32(load(constants + 4u), 7u);
    CHECK_EQ_U32(load(constants + 8u), 0x00800B00u);
    CHECK_EQ_U32(load(constants + 140u), 0x00100B00u);
    for (uint32_t i = 0u; i < 36u; i++) {
        CHECK_EQ_U32(d3d8_device_load32(0x10A8u + 7u * 16u + i * 4u), 0xAB000000u + i * 13u);
        CHECK_EQ_U32(load(constants + 12u + i * 4u + (i >= 32u ? 4u : 0u)), 0xAB000000u + i * 13u);
    }
    store(SCRATCH_DATA, 0u);
    const uint32_t zero = d3d8_device_load32(D3D8_DEV_CURSOR);
    CHECK_EQ_U32(d3d8_upload_vertex_shader_constants(SCRATCH_DATA, 7u) - zero, 12u);
    CHECK_EQ_U32(load(zero + 8u), 0x00000B00u);
    d3d8_device_store32(D3D8_DEV_LIMIT, zero);
    store(SCRATCH_DATA, 128u << 16);
    /* T1078: 128 vectors at cursor == limit need the sized reservation 0x003D6B30 (limit + 0x200 is exceeded), which the
     * port now performs (it refused by name before): the ring rolls over and the packet lands at the new cursor. */
    const uint64_t rolled = d3d8_pushbuffer_rollovers();
    const uint32_t rolled_end = d3d8_upload_vertex_shader_constants(SCRATCH_DATA, 7u);
    CHECK(d3d8_pushbuffer_rollovers() == rolled + 1u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), rolled_end);
    /* T870: the original's CreateDevice (0x003DB0B1) zeroes the 0x46 dwords at 0x003E2C68 and stores flags 0x10 at
     * 0x003E2C6C; poison the whole span and the words on both sides first. */
    for (uint32_t i = 0u; i < 0x48u + 2u; i++) store(0x003E2C64u + i * 4u, 0xA5A5A5A5u);
    d3d8_shader_create();
    CHECK_EQ_U32(load(0x003E2C64u), 0xA5A5A5A5u);
    CHECK_EQ_U32(load(0x003E2C68u), 0u);
    CHECK_EQ_U32(load(0x003E2C6Cu), 0x10u);
    for (uint32_t i = 2u; i < 0x46u; i++) CHECK_EQ_U32(load(0x003E2C68u + i * 4u), 0u);
    CHECK_EQ_U32(load(0x003E2D80u), 0xA5A5A5A5u);
    CHECK_EQ_U32(d3d8_device_load32(0x794u), 0x003E2D80u);
    CHECK_EQ_U32(d3d8_device_load32(0x798u), 2u);
    d3d8_device_store32(D3D8_DEV_LIMIT, d3d8_device_load32(D3D8_DEV_PB_END) - 0x200u);
    store(0x003E2D84u, 0x10u);
    store(0x003E2C6Cu, 0x10u);
    store(D3D8_GLOBAL_DIRTY_MASK, 0u);
    for (uint32_t i = 0u; i < 64u; i++) store(SCRATCH_DATA + i * 4u, i * 17u + 3u);
    const uint32_t vertex_start = d3d8_device_load32(D3D8_DEV_CURSOR);
    CHECK_EQ_U32(d3d8_set_vertex_shader(SCRATCH_DATA, 23u), vertex_start + 8u);
    CHECK_EQ_U32(d3d8_device_load32(0x794u), 0x003E2C68u);
    CHECK_EQ_U32(d3d8_device_load32(0x798u), 0x003E2C69u);
    CHECK_EQ_U32(d3d8_device_load32(0x79Cu), 23u);
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK), 0x70u);
    CHECK_EQ_U32(load(vertex_start), 0x00041EA0u);
    CHECK_EQ_U32(load(vertex_start + 4u), 23u);
    for (uint32_t i = 0u; i < 64u; i++) CHECK_EQ_U32(load(0x003E2C7Cu + i * 4u), i * 17u + 3u);
    (void)d3d8_set_vertex_shader(0u, 9u);
    CHECK_EQ_U32(d3d8_device_load32(0x794u), 0x003E2C68u);
    CHECK_EQ_U32(d3d8_device_load32(0x79Cu), 9u);
    store(0x003E2D84u, 0u);
    d3d8_shader_create();
    const uint32_t changed = d3d8_device_load32(D3D8_DEV_CURSOR);
    CHECK_EQ_U32(d3d8_set_vertex_shader(SCRATCH_DATA, 0u) - changed, 72u);
    CHECK_EQ_U32(load(changed), 0x00100A20u);
    CHECK_EQ_U32(load(changed + 40u), 0x00080394u);
    CHECK_EQ_U32(load(changed + 52u), 0x00081E94u);
    CHECK_EQ_U32(load(changed + 64u), 0x00041EA0u);
    /* SSE clamp and rounded float32*255 followed by nearest-even integer conversion. */
    map_fixed(0x00540000u, 0x10000u);
    for (uint32_t i = 0u; i < 16u; i++) store(0x005496E0u + i * 4u, i * 0x11111111u);
    const uint32_t definition = SCRATCH_DATA + 0x800u;
    for (uint32_t i = 0u; i < 60u; i++) store(definition + i * 4u, 0u);
    store(D3D8_DEVICE_BASE + 0x92Cu, definition);
    d3d8_device_store32(0x784u, D3D8_DEVICE_BASE + 0x924u);
    store(SCRATCH_DATA, 0x3F000000u); /* 0.5*255 =127.5, tie to128 */
    store(SCRATCH_DATA + 4u, 0x3BC0C0C1u); /* rounded product1.5, tie to2 */
    store(SCRATCH_DATA + 8u, 0x7FC00000u); /* MINPS selects1 for NaN */
    store(SCRATCH_DATA + 12u, 0xFF800000u); /* negative infinity clamps0 */
    const uint32_t pixel_start = d3d8_device_load32(D3D8_DEV_CURSOR);
    CHECK_EQ_U32(d3d8_set_pixel_shader_constants(0u, SCRATCH_DATA, 1u), 0u);
    CHECK_EQ_U32(d3d8_device_load32(0x8E4u), 0x008002FFu);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR) - pixel_start, 160u);
    for (uint32_t i = 0u; i < 16u; i++) CHECK_EQ_U32(load(0x003E3CE8u + i * 4u), 0x008002FFu);
    CHECK_EQ_U32(load(0x003E3D6Cu), 0x008002FFu);
    CHECK_EQ_U32(load(0x003E3D70u), 0x008002FFu);
    CHECK_EQ_U32(load(pixel_start + 144u), 0x000C181Cu);
    CHECK_EQ_U32(load(pixel_start + 148u), 0x3F000000u);
    CHECK_EQ_U32(load(pixel_start + 152u), 0x3BC0C0C1u);
    CHECK_EQ_U32(load(pixel_start + 156u), 0x7FC00000u);
    const uint32_t empty_start = d3d8_device_load32(D3D8_DEV_CURSOR);
    CHECK_EQ_U32(d3d8_set_pixel_shader_constants(0u, 0xBAD00000u, 0u), 0u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), empty_start);
    /* Every selected factor changes even when device shadow-copy flag0x10 is set. */
    d3d8_device_store32(D3D8_DEV_FLAGS, 0x10u);
    store(definition + 57u * 4u, 0x11111111u);
    store(SCRATCH_DATA, 0x3C20A0A1u); /* rounded product2.5, tie to2 */
    CHECK_EQ_U32(d3d8_set_pixel_shader_constants(1u, SCRATCH_DATA, 1u), 0u);
    CHECK_EQ_U32(d3d8_device_load32(0x8E8u), 0x000202FFu);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR) - empty_start, 64u);
    CHECK_EQ_U32(load(0x003E3CE8u), 0x000202FFu);
    environment_end();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
