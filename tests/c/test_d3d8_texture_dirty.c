/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_texture_dirty.h"

static unsigned hand_offs;
static uint32_t last_hand_off_end;
static void note_hand_off(void *context, uint32_t begin, uint32_t end)
{
    (void)context;(void)begin;
    hand_offs++;last_hand_off_end=end;
}
static void initialise(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    CHECK(d3d8_pushbuffer_create());
    store(D3D8_GLOBAL_DIRTY_MASK, 0xFF7F7Fu);
    for (uint32_t stage = 0; stage < 4u; stage++) {
        const uint32_t shadow = 0x3E3AC4u + stage * 0x80u;
        store(shadow + 8u, 2u);
        store(shadow + 12u, 2u);
        store(shadow + 16u, 2u);
    }
    store(D3D8_DEVICE_BASE + 0x794u, 0x3E2C68u);
}
static void test_actual_texture_packets(void)
{
    initialise();
    store(0x3E3AC0u, 3u);
    store(0x3E3AC4u, 3u);
    store(D3D8_DEVICE_BASE + 0xF88u, 0x3E5984u);
    const uint32_t start = d3d8_device_load32(D3D8_DEV_CURSOR);
    d3d8_texture_stages_preflight(15u);
    CHECK_EQ_U32(d3d8_texture_packet_bytes(15u), 80u);
    CHECK_EQ_U32(d3d8_texture_packet_bytes(0x40000000u), 0u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), start);
    CHECK_EQ_U32(d3d8_emit_texture_stages(15u), 0u);
    for (uint32_t stage = 0; stage < 4u; stage++) {
        const uint32_t cursor = start + stage * 20u;
        CHECK_EQ_U32(load(cursor), 0x81B08u + stage * 0x40u);
        CHECK_EQ_U32(load(cursor + 4u), stage == 0u ? 0x303u : 0u);
        CHECK_EQ_U32(load(cursor + 8u), stage == 0u ? 0x4003FFC0u : 0x3FFC0u);
        CHECK_EQ_U32(load(cursor + 12u), 0x41B14u + stage * 0x40u);
        CHECK_EQ_U32(load(cursor + 16u), 0x2062000u);
        CHECK_EQ_U32(d3d8_device_load32(0x774u + stage * 4u), 0x4003FFC0u);
    }
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), start + 80u);
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK), 0xFF7F7Fu);
    CHECK_EQ_U32(d3d8_emit_texture_stages(0x40000000u), 0u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), start + 80u);
    environment_end();
}
static void test_texture_refusals_are_atomic(void)
{
    initialise();
    const uint32_t start = d3d8_device_load32(D3D8_DEV_CURSOR);
    store(start, 0xDEADBEEFu);
    store(D3D8_DEVICE_BASE + 0x774u, 0x12345678u);
    /* Last stage is invalid: none of the preceding stages may mutate. */
    store(0x3E3AC4u + 3u * 0x80u + 16u, 3u);
    RUN_EXPECTING_FATAL((void)d3d8_emit_texture_stages(15u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003DDCB0u);
    CHECK_EQ_U32(load(start), 0xDEADBEEFu);
    CHECK_EQ_U32(d3d8_device_load32(0x774u), 0x12345678u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), start);
    store(0x3E3AC4u + 3u * 0x80u + 16u, 2u);
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK), 0xFF7F7Fu);
    /* Planning never depends on the cursor: a limit one packet away does not refuse it. */
    store(D3D8_DEVICE_BASE + D3D8_DEV_LIMIT, start + 20u);
    d3d8_texture_stages_preflight(3u);
    CHECK_EQ_U32(load(start), 0xDEADBEEFu);
    CHECK_EQ_U32(d3d8_device_load32(0x774u), 0x12345678u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), start);
    environment_end();
}
static void test_each_stage_has_its_own_reservation(void)
{
    initialise();
    const uint32_t start = d3d8_device_load32(D3D8_DEV_CURSOR);
    /* The limit is exactly one stage packet on. Stage 0 is written (its preamble sees cursor <
     * limit) and leaves the cursor AT the limit, so stage 1's own preamble rolls the ring over.
     * MUTATION: one reservation for all four stages never rolls, and the limit stays at start + 20. */
    store(D3D8_DEVICE_BASE + D3D8_DEV_LIMIT, start + 20u);
    const uint64_t rolls = d3d8_pushbuffer_rollovers();
    CHECK_EQ_U32(d3d8_emit_texture_stages(3u), 0u);
    CHECK(d3d8_pushbuffer_rollovers() == rolls + 1u);
    CHECK_EQ_U32(load(start), 0x81B08u);
    CHECK_EQ_U32(load(start + 20u), 0x81B48u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), start + 40u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_LIMIT), start + 20u + 0x10000u - 0x204u);
    /* Planning agrees: the simulated writer takes exactly one roll-over at the same place. */
    store(D3D8_DEVICE_BASE + D3D8_DEV_CURSOR, start);
    store(D3D8_DEVICE_BASE + D3D8_DEV_LIMIT, start + 20u);
    d3d8_pushbuffer_sim sim = d3d8_pushbuffer_sim_start();
    d3d8_plan_texture_stages(3u, &sim);
    CHECK_EQ_U32(sim.refills, 1u);
    CHECK_EQ_U32(sim.bytes, 40u);
    CHECK_EQ_U32(sim.cursor, start + 40u);
    CHECK_EQ_U32(sim.limit, start + 20u + 0x10000u - 0x204u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), start);
    environment_end();
}
static void test_disabled_fog_and_refusals(void)
{
    initialise();
    store(D3D8_DEVICE_BASE + 0x784u, 0x3E4884u);
    store(D3D8_DEVICE_BASE + 0x788u, 0x1C80u);
    uint32_t start = d3d8_device_load32(D3D8_DEV_CURSOR);
    d3d8_fog_preflight();
    CHECK_EQ_U32(d3d8_fog_packet_bytes(), 8u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), start);
    CHECK_EQ_U32(d3d8_emit_disabled_fog(), start + 8u);
    CHECK_EQ_U32(load(start), 0x402A4u);
    CHECK_EQ_U32(load(start + 4u), 0u);
    store(D3D8_DEVICE_BASE + 0x788u, 0u);
    store(0x3E3E5Cu, 1u);
    CHECK_EQ_U32(d3d8_fog_packet_bytes(), 20u);
    start += 8u;
    CHECK_EQ_U32(d3d8_emit_disabled_fog(), start + 20u);
    CHECK_EQ_U32(load(start + 8u), 0x80288u);
    CHECK_EQ_U32(load(start + 12u), 14u);
    CHECK_EQ_U32(load(start + 16u), 0x1C80u);
    start += 20u;
    store(start, 0xDEADBEEFu);
    store(0x3E3E30u, 1u);
    CHECK_EQ_U32(d3d8_fog_packet_bytes(), 48u);
    CHECK_EQ_U32(d3d8_emit_fog(), start + 48u);
    CHECK_EQ_U32(load(start), 0x802A0u);
    CHECK_EQ_U32(load(start + 4u), 0u);
    CHECK_EQ_U32(load(start + 24u), 0x3F800000u);
    CHECK_EQ_U32(load(start + 40u), 0x130E0300u);
    start += 48u;
    store(start, 0xDEADBEEFu);
    store(0x3E3E30u, 0u);
    store(0x3E2C6Cu, 2u);
    RUN_EXPECTING_FATAL(d3d8_fog_preflight());
    CHECK(fatal_seen);
    CHECK_EQ_U32(load(start), 0xDEADBEEFu);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), start);
    store(0x3E2C6Cu, 0u);
    /* The fog packet has one preamble (0x003DE01A). At the limit the ring rolls over first. */
    store(D3D8_DEVICE_BASE + D3D8_DEV_LIMIT, start);
    store(0x3E3E30u, 0u);
    const uint64_t rolls = d3d8_pushbuffer_rollovers();
    CHECK_EQ_U32(d3d8_emit_disabled_fog(), start + 20u);
    CHECK(d3d8_pushbuffer_rollovers() == rolls + 1u);
    CHECK_EQ_U32(load(start), 0x402A4u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_LIMIT), start + 0x10000u - 0x204u);
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK), 0xFF7F7Fu);
    environment_end();
}
static void test_enabled_fog_linear_and_preflight(void)
{
    initialise();
    store(0x475C78u, 0x3F800000u);
    store(0x3E2958u, 0x46000000u);
    store(0x3E3E30u, 1u); store(0x3E3E34u, 3u);
    store(0x3E3E38u, 0x3F800000u); store(0x3E3E3Cu, 0x40000000u);
    store(D3D8_DEVICE_BASE+0x784u, 1u);store(D3D8_DEVICE_BASE+0x788u, 1u);
    uint32_t start = d3d8_device_load32(D3D8_DEV_CURSOR);
    CHECK_EQ_U32(d3d8_fog_packet_bytes(),36u);
    CHECK_EQ_U32(d3d8_emit_fog(),start+36u);
    CHECK_EQ_U32(load(start+24u),0x40400000u);
    CHECK_EQ_U32(load(start+28u),0xBF800000u);
    store(0x3E3E3Cu,0x3F800000u);start+=36u;
    CHECK_EQ_U32(d3d8_emit_fog(),start+36u);
    CHECK_EQ_U32(load(start+24u),0x46000400u);
    CHECK_EQ_U32(load(start+28u),0xC6000000u);
    start+=36u;store(start,0xDEADBEEFu);
    const uint32_t limit=load(D3D8_DEVICE_BASE+D3D8_DEV_LIMIT);
    /* The original checks cursor < limit once, before the packet, and writes the whole 36 bytes
     * into the slack past the limit. MUTATION: refusing when cursor + bytes exceeds the limit. */
    store(D3D8_DEVICE_BASE+D3D8_DEV_LIMIT,start+35u);
    const uint64_t rolls=d3d8_pushbuffer_rollovers();
    CHECK_EQ_U32(d3d8_emit_fog(),start+36u);
    CHECK(d3d8_pushbuffer_rollovers()==rolls);
    CHECK_EQ_U32(load(start+24u),0x46000400u);
    store(D3D8_DEVICE_BASE+D3D8_DEV_CURSOR,start);
    store(start,0xDEADBEEFu);
    store(D3D8_DEVICE_BASE+D3D8_DEV_LIMIT,limit);
    store(0x3E3E34u,1u); /* Unmapped density scale constant must refuse before writes. */
    RUN_EXPECTING_FATAL((void)d3d8_emit_fog());
    CHECK(fatal_seen);CHECK_EQ_U32(load(start),0xDEADBEEFu);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR),start);
    store(0x3E3E34u,3u);
    store(0x3E2C6Cu,2u);
    RUN_EXPECTING_FATAL((void)d3d8_emit_fog());
    CHECK(fatal_seen);CHECK_EQ_U32(load(start),0xDEADBEEFu);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR),start);
    store(0x3E2C6Cu,0u);
    store(D3D8_DEVICE_BASE+D3D8_DEV_CURSOR,0x70000000u);
    store(D3D8_DEVICE_BASE+D3D8_DEV_LIMIT,0x70001000u);
    RUN_EXPECTING_FATAL((void)d3d8_emit_fog());
    CHECK(fatal_seen);CHECK_EQ_U32(load(start),0xDEADBEEFu);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR),0x70000000u);
    store(D3D8_DEVICE_BASE+D3D8_DEV_CURSOR,start);
    store(D3D8_DEVICE_BASE+D3D8_DEV_LIMIT,limit);
    store(D3D8_DEVICE_BASE+0x794u,UINT32_MAX);
    RUN_EXPECTING_FATAL((void)d3d8_emit_fog());
    CHECK(fatal_seen);CHECK_EQ_U32(load(start),0xDEADBEEFu);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR),start);
    environment_end();
}
static void test_fog_program_packets_and_atomic_refusals(void)
{
    initialise();
    store(0x3E3F58u,D3D8_DEVICE_BASE);store(0x3E2C6Cu,2u);
    store(0x475C78u,0x3F800000u);store(0x475CACu,0u);store(0x475CD4u,0x3F000000u);
    store(0x3E3EFCu,2u);store(0x3E3F20u,1u);
    store(D3D8_DEVICE_BASE+0x944u,0x3F800000u);
    store(D3D8_DEVICE_BASE+0x948u,0x40000000u);
    store(D3D8_DEVICE_BASE+0xEF8u,0x3F800000u);
    store(D3D8_DEVICE_BASE+0xEFCu,0x40000000u);
    for(unsigned branch=0u;branch<3u;branch++) {
        store(0x3E3E34u,branch==0u?0u:1u);
        store(D3D8_DEVICE_BASE+8u,branch==2u?0x8002u:0u);
        const uint32_t size=branch==2u?236u:252u;
        const uint32_t start=d3d8_device_load32(D3D8_DEV_CURSOR);
        CHECK_EQ_U32(d3d8_emit_fog_vertex_program(),start+size);
        CHECK_EQ_U32(load(start),0x41EA4u);
        CHECK_EQ_U32(load(start+24u),0x40000000u);
        CHECK_EQ_U32(load(start+28u),branch==2u?0x3F000000u:0x3F800000u);
        CHECK_EQ_U32(load(start+32u),branch==2u?0x3FC00000u:0x40000000u);
        CHECK_EQ_U32(load(start+52u),0x800B00u);
    }
    const uint32_t start=d3d8_device_load32(D3D8_DEV_CURSOR);
    const uint32_t limit=d3d8_device_load32(D3D8_DEV_LIMIT);
    store(start,0xCAFEBABEu);
    store(0x3E1908u,0xABC12345u);
    for(unsigned trial=0u;trial<5u;trial++) {
        if(trial==1u)continue; /* T546: a reservation rollover is no longer a refusal, see below */
        if(trial==0u)store(0x3E3F58u,0u);
        if(trial==2u){store(0x3E3F58u,D3D8_DEVICE_BASE);store(D3D8_DEVICE_BASE+D3D8_DEV_LIMIT,UINT32_MAX);}
        if(trial==3u){store(D3D8_DEVICE_BASE+D3D8_DEV_LIMIT,limit);store(D3D8_DEVICE_BASE+D3D8_DEV_CURSOR,0x3E1908u);}
        if(trial==4u){store(D3D8_DEVICE_BASE+D3D8_DEV_CURSOR,0x70000000u);store(D3D8_DEVICE_BASE+D3D8_DEV_LIMIT,0x70001000u);}
        RUN_EXPECTING_FATAL((void)d3d8_emit_fog_vertex_program());
        CHECK(fatal_seen);CHECK_EQ_U32(load(start),0xCAFEBABEu);
        CHECK_EQ_U32(load(0x3E1908u),0xABC12345u);
        CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR),trial==4u?0x70000000u:trial==3u?0x3E1908u:start);
    }
    store(0x3E3F58u,D3D8_DEVICE_BASE);
    /* T546: the helper's sized reservation 0x003D6B30(count + 30) refills once the cursor is 216 bytes or more past the limit
     * (cursor + 296 >= limit + 0x200), and the helper writes after it. Without a GPU module the refill is the arithmetic. */
    store(D3D8_DEVICE_BASE+D3D8_DEV_CURSOR,start);
    store(D3D8_DEVICE_BASE+D3D8_DEV_LIMIT,start-216u);
    uint64_t rolls=d3d8_pushbuffer_rollovers();
    CHECK_EQ_U32(d3d8_emit_fog_vertex_program(),start+236u);
    CHECK(d3d8_pushbuffer_rollovers()==rolls+1u);
    CHECK_EQ_U32(load(start),0x41EA4u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_LIMIT),start+0x10000u-0x204u);
    /* One byte inside: no refill. */
    store(D3D8_DEVICE_BASE+D3D8_DEV_CURSOR,start);store(start,0xCAFEBABEu);
    store(D3D8_DEVICE_BASE+D3D8_DEV_LIMIT,start-215u);
    rolls=d3d8_pushbuffer_rollovers();
    CHECK_EQ_U32(d3d8_emit_fog_vertex_program(),start+236u);
    CHECK(d3d8_pushbuffer_rollovers()==rolls);
    /* The fog packet has its own preamble after the helper: a limit AT the end of the helper refills between the two, the
     * helper is handed over first and the fog packet starts the new segment. */
    store(D3D8_DEVICE_BASE+D3D8_DEV_CURSOR,start);store(start,0xCAFEBABEu);
    store(D3D8_DEVICE_BASE+D3D8_DEV_LIMIT,start+236u);
    d3d8_pushbuffer_set_consumer(note_hand_off,NULL);hand_offs=0u;
    rolls=d3d8_pushbuffer_rollovers();
    const uint32_t both=d3d8_fog_packet_bytes();
    CHECK(both>236u);
    CHECK_EQ_U32(d3d8_emit_fog(),start+both);
    CHECK(d3d8_pushbuffer_rollovers()==rolls+1u);
    CHECK_EQ_U32(hand_offs,1u);CHECK_EQ_U32(last_hand_off_end,start+236u);
    CHECK_EQ_U32(load(start),0x41EA4u);
    CHECK_EQ_U32(load(start+236u),0x402A4u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_LIMIT),start+236u+0x10000u-0x204u);
    /* A limit one byte past the end of the helper: no refill, one run. */
    store(D3D8_DEVICE_BASE+D3D8_DEV_CURSOR,start);store(start,0xCAFEBABEu);
    store(D3D8_DEVICE_BASE+D3D8_DEV_LIMIT,start+237u);
    rolls=d3d8_pushbuffer_rollovers();
    CHECK_EQ_U32(d3d8_emit_fog(),start+both);
    CHECK(d3d8_pushbuffer_rollovers()==rolls);
    /* The same refill planned over a shared simulation: the fog site is where the second run starts. */
    store(D3D8_DEVICE_BASE+D3D8_DEV_CURSOR,start);store(D3D8_DEVICE_BASE+D3D8_DEV_LIMIT,start+236u);
    d3d8_pushbuffer_sim sim=d3d8_pushbuffer_sim_start();
    d3d8_plan_fog(&sim);
    CHECK_EQ_U32(sim.refills,1u);CHECK_EQ_U32(sim.bytes,both);CHECK_EQ_U32(sim.cursor,start+both);
    d3d8_pushbuffer_set_consumer(NULL,NULL);
    store(D3D8_DEVICE_BASE+D3D8_DEV_CURSOR,start);
    store(start,0xCAFEBABEu);
    store(D3D8_DEVICE_BASE+D3D8_DEV_LIMIT,limit);
    store(0x3E2C6Cu,0u);
    CHECK_EQ_U32(d3d8_emit_fog_vertex_program(),0x3E2C68u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR),start);
    store(0x3E2C6Cu,2u);
    CHECK(guest_region_free(RDATA_REGION_BASE));
    RUN_EXPECTING_FATAL((void)d3d8_emit_fog_vertex_program());
    CHECK(fatal_seen);CHECK_EQ_U32(load(start),0xCAFEBABEu);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR),start);
    environment_end();
}
int main(void)
{
    test_actual_texture_packets();
    test_texture_refusals_are_atomic();
    test_each_stage_has_its_own_reservation();
    test_disabled_fog_and_refusals();
    test_enabled_fog_linear_and_preflight();
    test_fog_program_packets_and_atomic_refusals();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
