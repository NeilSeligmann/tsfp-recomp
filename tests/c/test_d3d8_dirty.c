/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_combiner.h"
#include "d3d8_dirty.h"
#include "d3d8_set_state.h"
static void test_point_emission_and_refill(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    CHECK(d3d8_pushbuffer_create());
    store(D3D8_GLOBAL_DIRTY_MASK, 0xFF7F7Fu);
    uint32_t start = d3d8_device_load32(D3D8_DEV_CURSOR);
    CHECK_EQ_U32(d3d8_emit_point_state(), 0u);
    CHECK_EQ_U32(load(start), 0x80318u);
    CHECK_EQ_U32(load(start + 4u), 0u);
    CHECK_EQ_U32(load(start + 8u), 0u);
    CHECK_EQ_U32(load(start + 12u), 0x4043Cu);
    CHECK_EQ_U32(load(start + 16u), 0u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), start + 20u);
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK), 0xFF7F7Fu);
    store(0x3E3E90u, 0x40000000u); /* size 2 */
    store(0x3E3E94u, 0x3F800000u); /* minimum 1 */
    store(0x3E3EACu, 0x40400000u); /* maximum 3 */
    store(0x3E2954u, 0x42800000u);
    store(D3D8_DEVICE_BASE + 0x964u, 0x3F800000u);
    CHECK_EQ_U32(d3d8_emit_point_state(), 16u);
    store(0x3E3E9Cu, 1u);
    store(0x3E3EA0u, 0x40800000u); /* coefficient 4 */
    store(0x3E3EA4u, 0x41000000u); /* coefficient 8 */
    store(0x3E3EA8u, 0x41400000u); /* coefficient 12 */
    store(D3D8_DEVICE_BASE + 0xEECu, 2u);
    start = d3d8_device_load32(D3D8_DEV_CURSOR);
    CHECK_EQ_U32(d3d8_emit_point_state(), 16u);
    const uint32_t packet[] = {0x200A30u, 0x3F800000u, 0x40000000u, 0x40400000u,
        0x40000000u, 0x40000000u, 0x40000000u, 0xBF000000u, 0x3F800000u,
        0x80318u, 1u, 0u, 0x4043Cu, 16u};
    for (uint32_t i = 0; i < sizeof(packet) / sizeof(packet[0]); i++)
        CHECK_EQ_U32(load(start + i * 4u), packet[i]);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), start + sizeof(packet));
    store(0x3E3E90u, 0x7FC00001u);
    store(0x3E3E9Cu, 0u);
    CHECK_EQ_U32(d3d8_emit_point_state(), 511u);
    /* 0x003DD7CB: the emitter opens with its reservation preamble. A cursor at the limit rolls the
     * ring over BEFORE the packet is formed, so the packet lands at the unchanged cursor and the
     * limit moves a kickoff on. MUTATION: dropping the preamble leaves the limit where it was. */
    start = d3d8_device_load32(D3D8_DEV_CURSOR);
    store(D3D8_DEVICE_BASE + D3D8_DEV_LIMIT, start);
    const uint64_t rolls = d3d8_pushbuffer_rollovers();
    CHECK_EQ_U32(d3d8_emit_point_state(), 511u);
    CHECK(d3d8_pushbuffer_rollovers() == rolls + 1u);
    CHECK_EQ_U32(load(start), 0x80318u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), start + 20u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_LIMIT), start + 0x10000u - 0x204u);
    /* A cursor 4 bytes short of the limit does NOT roll: the packet is written past the limit into
     * the slack, as the original does (it checks once, before the packet). */
    start = d3d8_device_load32(D3D8_DEV_CURSOR);
    store(D3D8_DEVICE_BASE + D3D8_DEV_LIMIT, start + 4u);
    CHECK_EQ_U32(d3d8_emit_point_state(), 511u);
    CHECK(d3d8_pushbuffer_rollovers() == rolls + 1u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), start + 20u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_LIMIT), start + 4u);
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK), 0xFF7F7Fu);
    /* Device flag 4 selects the original's alternate put pointer, which nothing models: the
     * roll-over refuses, and so the emitter writes nothing. */
    start = d3d8_device_load32(D3D8_DEV_CURSOR);
    store(start, 0xDEADBEEFu);
    store(D3D8_DEVICE_BASE + D3D8_DEV_LIMIT, start);
    store(D3D8_DEVICE_BASE + D3D8_DEV_FLAGS, d3d8_device_load32(D3D8_DEV_FLAGS) | 4u);
    RUN_EXPECTING_FATAL((void)d3d8_emit_point_state());
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003D69E0u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), start);
    CHECK_EQ_U32(load(start), 0xDEADBEEFu);
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK), 0xFF7F7Fu);
    environment_end();
}
static void test_shader_stage_cache_and_refusals(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    CHECK(d3d8_pushbuffer_create());
    store(D3D8_GLOBAL_DIRTY_MASK, 0xFF7F7Fu);
    store(D3D8_DEVICE_BASE + 0x784u, 0xDEADBEEFu); /* Checked, never dereferenced. */
    store(D3D8_DEVICE_BASE + 0x78Cu, 0x100u);
    store(D3D8_DEVICE_BASE + 0x790u, 1u);
    store(D3D8_DEVICE_BASE + 0xCu, 0x20u);
    for (uint32_t offset = 0x10u; offset <= 0x18u; offset += 4u)
        store(D3D8_DEVICE_BASE + offset, 0x80000000u);
    uint32_t start = d3d8_device_load32(D3D8_DEV_CURSOR);
    CHECK_EQ_U32(d3d8_emit_shader_stage_program(), start + 8u);
    CHECK_EQ_U32(load(start), 0x41E70u);
    CHECK_EQ_U32(load(start + 4u), 1u);
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK), 0xFF7F7Fu);
    /* Four different adaptations in one program: 3,2,14,protected17. */
    store(D3D8_DEVICE_BASE + 0x790u, 0xFFF00000u | 1u | (3u << 5u) | (13u << 10u) | (17u << 15u));
    store(D3D8_DEVICE_BASE + 0xCu, 4u);
    store(D3D8_DEVICE_BASE + 0x10u, 0x30u);
    store(D3D8_DEVICE_BASE + 0x14u, 4u);
    start = d3d8_device_load32(D3D8_DEV_CURSOR);
    CHECK_EQ_U32(d3d8_emit_shader_stage_program(), start + 8u);
    CHECK_EQ_U32(load(start + 4u), 3u | (2u << 5u) | (14u << 10u) | (17u << 15u));
    CHECK_EQ_U32(d3d8_device_load32(0x790u), 0xFFF00000u | 1u | (3u << 5u) | (13u << 10u) | (17u << 15u));
    store(D3D8_DEVICE_BASE + 0x78Cu, 0u);
    start = d3d8_device_load32(D3D8_DEV_CURSOR);
    CHECK_EQ_U32(d3d8_emit_shader_stage_program(), start + 8u);
    CHECK_EQ_U32(load(start + 4u), d3d8_device_load32(0x790u));
    start = d3d8_device_load32(D3D8_DEV_CURSOR);
    store(start, 0xDEADBEEFu);
    /* T443: no pixel shader bound is the fixed-function program, one 5 bit type per stage from the
     * texture word: stage 0 holds 4 (type 3), stage 1 0x30 (type 2), stage 2 4 (type 3) and stage 3
     * is unbound (type 0), so 3 | 2 << 5 | 3 << 10 | 0 << 15. */
    store(D3D8_DEVICE_BASE + 0x784u, 0u);
    CHECK_EQ_U32(d3d8_emit_shader_stage_program(), start + 8u);
    CHECK_EQ_U32(load(start), 0x41E70u);
    CHECK_EQ_U32(load(start + 4u), 3u | (2u << 5u) | (3u << 10u));
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), start + 8u);
    store(D3D8_DEVICE_BASE + 0x784u, 1u);
    start = d3d8_device_load32(D3D8_DEV_CURSOR);
    /* At the limit the program is formed and the packet lands after a roll-over (0x003DDC8F). */
    store(D3D8_DEVICE_BASE + D3D8_DEV_LIMIT, start);
    const uint64_t rolls = d3d8_pushbuffer_rollovers();
    CHECK_EQ_U32(d3d8_emit_shader_stage_program(), start + 8u);
    CHECK(d3d8_pushbuffer_rollovers() == rolls + 1u);
    CHECK_EQ_U32(load(start), 0x41E70u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_LIMIT), start + 0x10000u - 0x204u);
    /* The same refill under device flag 4 refuses before the packet. */
    start = d3d8_device_load32(D3D8_DEV_CURSOR);
    store(start, 0xDEADBEEFu);
    store(D3D8_DEVICE_BASE + D3D8_DEV_LIMIT, start);
    store(D3D8_DEVICE_BASE + D3D8_DEV_FLAGS, d3d8_device_load32(D3D8_DEV_FLAGS) | 4u);
    RUN_EXPECTING_FATAL((void)d3d8_emit_shader_stage_program());
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003D69E0u);
    CHECK_EQ_U32(load(start), 0xDEADBEEFu);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), start);
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK), 0xFF7F7Fu);
    environment_end();
}
static void test_default_lighting_and_preflight_refusals(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    CHECK(d3d8_pushbuffer_create());
    store(D3D8_GLOBAL_DIRTY_MASK, 0xFF7F7Fu);
    store(D3D8_DEVICE_BASE + 0x794u, D3D8_DEVICE_BASE + 0x924u);
    store(D3D8_DEVICE_BASE + 0x928u, 0u);
    store(D3D8_DEVICE_BASE + 0x1928u, 1u);
    store(D3D8_DEVICE_BASE + 8u, 0x4203u);
    for (uint32_t off = 0x7ACu; off <= 0x7C8u; off += 4u)
        store(D3D8_DEVICE_BASE + off, 0u);
    store(D3D8_DEVICE_BASE + 0x7CCu, 0u);
    uint32_t start = d3d8_device_load32(D3D8_DEV_CURSOR);
    CHECK_EQ_U32(d3d8_emit_default_lighting_state(0xFF7F7Fu), D3D8_DEVICE_BASE + 0x7C8u);
    const uint32_t packet[] = {0x40314u, 0u, 0x403B8u, 0u, 0x40294u, 0x20001u, 0x403BCu, 0u};
    for (uint32_t i = 0u; i < 8u; i++) CHECK_EQ_U32(load(start + i * 4u), packet[i]);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), start + 32u);
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK), 0xFF7F7Fu);
    start = d3d8_device_load32(D3D8_DEV_CURSOR);
    store(start, 0xDEADBEEFu);
    store(D3D8_DEVICE_BASE + 0x7C8u, D3D8_DEVICE_BASE);
    RUN_EXPECTING_FATAL((void)d3d8_emit_default_lighting_state(0xFF1000u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x3DE830u);
    CHECK_EQ_U32(load(start), 0xDEADBEEFu);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), start);
    store(D3D8_DEVICE_BASE + 0x7C8u, 0u);
    store(0x3E3E58u, 1u);
    store(D3D8_DEVICE_BASE + 0x1928u, 0u);
    RUN_EXPECTING_FATAL((void)d3d8_emit_default_lighting_state(0xFF1000u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(load(start), 0xDEADBEEFu);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), start);
    /* The reservation preamble opens the emitter, so a cursor at the limit rolls over first and
     * the packets land at the unchanged cursor (0x003DE83C). */
    store(D3D8_DEVICE_BASE + 0x1928u, 1u);
    store(0x3E3E58u, 0u);
    store(D3D8_DEVICE_BASE + D3D8_DEV_LIMIT, start);
    const uint64_t rolls = d3d8_pushbuffer_rollovers();
    CHECK_EQ_U32(d3d8_emit_default_lighting_state(0xFF1000u), D3D8_DEVICE_BASE + 0x7C8u);
    CHECK(d3d8_pushbuffer_rollovers() == rolls + 1u);
    CHECK_EQ_U32(load(start), 0x40314u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), start + 32u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_LIMIT), start + 0x10000u - 0x204u);
    /* Flag 4 makes the roll-over refuse, and the refusal precedes every write. */
    start = d3d8_device_load32(D3D8_DEV_CURSOR);
    store(start, 0xDEADBEEFu);
    store(D3D8_DEVICE_BASE + D3D8_DEV_LIMIT, start);
    store(D3D8_DEVICE_BASE + D3D8_DEV_FLAGS, d3d8_device_load32(D3D8_DEV_FLAGS) | 4u);
    RUN_EXPECTING_FATAL((void)d3d8_emit_default_lighting_state(0xFF1000u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003D69E0u);
    CHECK_EQ_U32(load(start), 0xDEADBEEFu);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), start);
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK), 0xFF7F7Fu);
    environment_end();
}

/* The planning twin reserves exactly the bytes the emitter writes: the simulated cursor decides where a
 * later site rolls over and whether its span is still mapped. Stage 0 SELECTARG1 for colour and alpha,
 * stage 1 DISABLE, so the walk ends after one stage and the packet is the stage count plus four packets. */
static void test_plan_twins_reserve_what_the_emitters_write(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    CHECK(d3d8_pushbuffer_create());
    store(D3D8_DEVICE_BASE + 0x784u, 0u);
    store(D3D8_TS_SHADOW + 0x30u, 2u);
    store(D3D8_TS_SHADOW + 0x40u, 2u);
    store(D3D8_TS_SHADOW + D3D8_TS_STAGE_DWORDS * 4u + 0x30u, 1u);
    d3d8_pushbuffer_sim sim = d3d8_pushbuffer_sim_start();
    const uint32_t start = d3d8_device_load32(D3D8_DEV_CURSOR);
    const uint32_t planned = d3d8_plan_fixed_function_combiner(0x800u, &sim);
    const uint32_t emitted = d3d8_emit_fixed_function_combiner(0x800u);
    CHECK_EQ_U32(planned, emitted);
    CHECK_EQ_U32(sim.bytes, (2u + 4u * 9u) * 4u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR) - start, sim.bytes);
    CHECK_EQ_U32(sim.cursor, d3d8_device_load32(D3D8_DEV_CURSOR));
    /* The texture transforms: four stages, each with its own two dword reservation. The vertex
     * declaration pointer names zeroed device bytes, so the fixed-function mode applies. */
    store(D3D8_DEVICE_BASE + 0x794u, D3D8_DEVICE_BASE + 0x800u);
    sim = d3d8_pushbuffer_sim_start();
    const uint32_t transforms_start = d3d8_device_load32(D3D8_DEV_CURSOR);
    d3d8_plan_texture_transforms(&sim);
    d3d8_emit_texture_transforms();
    CHECK_EQ_U32(sim.bytes, 4u * 8u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR) - transforms_start, sim.bytes);
    environment_end();
}

int main(void)
{
    test_point_emission_and_refill();
    test_shader_stage_cache_and_refusals();
    test_default_lighting_and_preflight_refusals();
    test_plan_twins_reserve_what_the_emitters_write();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
