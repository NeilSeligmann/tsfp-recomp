/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The draw dirty-state cascade 0x003DED80 and DrawVertices around a pushbuffer limit (T368).
 *
 * Every emitter in the cascade opens with its own `if (cursor >= limit) call 0x003D6B20`, texture
 * stages once per stage, and the draw packet is reserved with the sized 0x003D6B30. The expected
 * numbers below are hand-derived from those sites, not read back from the code: with the live
 * mask 0xFF5821 (the boot's 0xFF5871 after the controller clears 0x50) the cascade writes the
 * shader-stage program (8 bytes) at offset 0, texture stage 0 (20 bytes) at 8 and the default
 * lighting packets (24 + 8 bytes) at 28, so a roll-over falls at the first of those offsets that
 * is at or past the limit. tests/test_d3d8_cascade_refill_oracle.py replays the same states
 * through the original code and compares byte for byte.
 */

#include "test_d3d8_support.h"

#define KICKOFF 0x10000u
#define RING_BYTES 0x100000u
#define LIVE_MASK 0xFF5821u
#define PROGRAM_BYTES 8u
#define STAGE_BYTES 20u
#define LIGHT_BYTES 32u
#define CASCADE_BYTES (PROGRAM_BYTES + STAGE_BYTES + LIGHT_BYTES)


static struct {
    unsigned calls;
    uint32_t cursors[8];
} rolls;

static void record_roll(void *context, uint32_t begin, uint32_t end)
{
    (void)context;
    (void)begin;
    if (rolls.calls < 8u) rolls.cursors[rolls.calls] = end;
    rolls.calls++;
}

static uint32_t seed(uint32_t kickoff)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(0x00540000u, 0x10000u); /* the attribute-to-slot map the stream work reads */
    memset(&rolls, 0, sizeof(rolls));
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, RING_BYTES);
    store(D3D8_GLOBAL_KICKOFF_SIZE, kickoff);
    CHECK(d3d8_pushbuffer_create());
    d3d8_pushbuffer_set_consumer(record_roll, NULL);
    for (uint32_t stage = 0u; stage < 4u; stage++) {
        const uint32_t shadow = 0x3E3AC4u + stage * 0x80u;
        store(shadow + 8u, 2u);
        store(shadow + 12u, 2u);
        store(shadow + 16u, 2u);
    }
    store(D3D8_DEVICE_BASE + 0x794u, 0x3E2C68u);
    store(0x3E2C6Cu, 0u);
    store(D3D8_DEVICE_BASE + D3D8_DEV_FLAGS, 0x4003u);
    store(D3D8_DEVICE_BASE + 0x784u, 0x3E4884u);
    store(D3D8_DEVICE_BASE + 0x788u, 0x1C80u);
    store(D3D8_DEVICE_BASE + 0x78Cu, 0x100u);
    store(D3D8_DEVICE_BASE + 0x790u, 1u);
    store(D3D8_DEVICE_BASE + 0x1928u, 1u);
    store(D3D8_DEVICE_BASE + 0xCu, 0x20u);
    for (uint32_t offset = 0x10u; offset <= 0x18u; offset += 4u)
        store(D3D8_DEVICE_BASE + offset, 0x80000000u);
    for (uint32_t offset = 0x7ACu; offset <= 0x7D0u; offset += 4u)
        store(D3D8_DEVICE_BASE + offset, 0u);
    store(D3D8_DEVICE_BASE + 0x20u, 0u);
    for (uint32_t address = 0x3E3E30u; address <= 0x3E3E5Cu; address += 4u)
        store(address, 0u);
    return load(D3D8_DEVICE_BASE + 0x24u);
}

static void place(uint32_t cursor, uint32_t limit)
{
    store(D3D8_DEVICE_BASE + D3D8_DEV_CURSOR, cursor);
    store(D3D8_DEVICE_BASE + D3D8_DEV_LIMIT, limit);
}

static uint32_t device_limit(void) { return d3d8_device_load32(D3D8_DEV_LIMIT); }

/* Byte-for-byte snapshot of the D3D region and a window of the ring. */
typedef struct {
    uint8_t region[D3D_REGION_BYTES];
    uint8_t window[0x400];
} snapshot;

static void take(snapshot *shot, uint32_t window_base)
{
    memcpy(shot->region, kernel_guest_at(D3D_REGION_BASE, D3D_REGION_BYTES), D3D_REGION_BYTES);
    memcpy(shot->window, kernel_guest_at(window_base, sizeof(shot->window)), sizeof(shot->window));
}

static bool unchanged(const snapshot *shot, uint32_t window_base)
{
    return memcmp(shot->region, kernel_guest_at(D3D_REGION_BASE, D3D_REGION_BYTES),
                  D3D_REGION_BYTES) == 0 &&
           memcmp(shot->window, kernel_guest_at(window_base, sizeof(shot->window)),
                  sizeof(shot->window)) == 0;
}

/* One cascade configuration: the emitter sites in original order with the offset each starts at
 * (the bytes of the sites before it), its packet's first word, and the cascade's total bytes. A
 * site with no bytes (the stream work with nothing bound) still has its reservation preamble. */
typedef struct {
    const char *name;
    uint32_t mask;
    unsigned sites;
    uint32_t offset[10];
    uint32_t first_word[10];
    uint32_t total;
    bool fog_fallback;
    bool attenuated;
} scenario;

static const scenario SCENARIOS[] = {
    /* The boot's live mask after the controller clears 0x50: program, stage 0, default lighting. */
    {"live", LIVE_MASK, 3u, {0u, 8u, 28u}, {0x41E70u, 0x81B08u, 0x40314u}, 60u, false, false},
    /* The same plus the stream work (bit 0x40) with nothing bound: a site of zero bytes. */
    {"live+stream", LIVE_MASK | 0x40u, 4u, {0u, 8u, 28u, 60u}, {0x41E70u, 0x81B08u, 0x40314u, 0u},
     60u, false, false},
    /* The first draw's mask: point, program, four stages, disabled fog, lighting, and the stream
     * work (68 bytes: the 0x1760 header and sixteen strides, nothing bound to write). */
    {"first-draw", 0xFF7F7Fu, 9u, {0u, 20u, 28u, 48u, 68u, 88u, 108u, 116u, 148u},
     {0x80318u, 0x41E70u, 0x81B08u, 0x81B48u, 0x81B88u, 0x81BC8u, 0x402A4u, 0x40314u, 0x401760u}, 216u,
     false, false},
    /* Fog without a bound pixel-shader length writes the 20-byte fallback packet. */
    {"first-draw+fog-fallback", 0xFF7F7Fu, 9u, {0u, 20u, 28u, 48u, 68u, 88u, 108u, 128u, 160u},
     {0x80318u, 0x41E70u, 0x81B08u, 0x81B48u, 0x81B88u, 0x81BC8u, 0x402A4u, 0x40314u, 0x401760u}, 228u,
     true, false},
    /* Attenuated point sprites write 56 bytes. */
    {"first-draw+attenuated-point", 0xFF7F7Fu, 9u,
     {0u, 56u, 64u, 84u, 104u, 124u, 144u, 152u, 184u},
     {0x200A30u, 0x41E70u, 0x81B08u, 0x81B48u, 0x81B88u, 0x81BC8u, 0x402A4u, 0x40314u, 0x401760u}, 252u,
     false, true},
};
#define SCENARIO_COUNT (sizeof(SCENARIOS) / sizeof(SCENARIOS[0]))

static uint32_t seed_scenario(const scenario *sc, uint32_t flags_or)
{
    const uint32_t base = seed(KICKOFF);
    store(0x3E3E90u, 0x3F800000u); /* point size 1 */
    store(0x3E3E94u, 0x3F800000u);
    store(0x3E3EACu, 0x40400000u);
    store(0x3E2954u, 0x42800000u);
    store(D3D8_DEVICE_BASE + 0x964u, 0x3F800000u);
    store(D3D8_DEVICE_BASE + 0xEECu, 2u);
    store(0x3E3E9Cu, sc->attenuated ? 1u : 0u);
    store(0x3E3EA0u, 0x40800000u);
    store(0x3E3EA4u, 0x41000000u);
    store(0x3E3EA8u, 0x41400000u);
    if (sc->fog_fallback) store(D3D8_DEVICE_BASE + 0x788u, 0u);
    store(D3D8_DEVICE_BASE + D3D8_DEV_FLAGS, 0x4003u | flags_or);
    store(D3D8_GLOBAL_DIRTY_MASK, sc->mask);
    return base;
}

/* The first site starting at or past the limit, or UINT32_MAX when none does. */
static uint32_t first_roll_offset(const scenario *sc, uint32_t back)
{
    for (unsigned i = 0u; i < sc->sites; i++) {
        if (sc->offset[i] >= back) return sc->offset[i];
    }
    return UINT32_MAX;
}

static void test_cascade_rolls_at_the_site_the_original_does(void)
{
    unsigned rolled = 0u;
    for (unsigned which = 0u; which < SCENARIO_COUNT; which++) {
        const scenario *sc = &SCENARIOS[which];
        for (uint32_t back = 0u; back <= sc->total + 8u; back += 4u) {
            const uint32_t base = seed_scenario(sc, 0u);
            const uint32_t limit = base + 0x4000u;
            const uint32_t cursor = limit - back;
            place(cursor, limit);
            uint32_t result = 0u;
            RUN_EXPECTING_FATAL(result = d3d8_draw_vertices(8u, 17u, 3u));
            if (fatal_seen) {
                printf("  FAIL %s back %u: unexpected fatal at %#x: %s\n", sc->name, (unsigned)back,
                       (unsigned)fatal_address, fatal_text);
                failures++;
                environment_end();
                continue;
            }
            const uint32_t offset = first_roll_offset(sc, back);
            if (offset == UINT32_MAX) {
                CHECK_EQ_U32(rolls.calls, 0u);
                CHECK_EQ_U32(device_limit(), limit);
            } else {
                /* One roll, at the cursor of the first site that reached the limit, and the limit
                 * moves a kickoff on from THERE. MUTATION: a roll at any other site, or a single
                 * reservation for a whole emitter, leaves a different cursor or limit. */
                CHECK_EQ_U32(rolls.calls, 1u);
                CHECK_EQ_U32(rolls.cursors[0], cursor + offset);
                CHECK_EQ_U32(device_limit(), cursor + offset + KICKOFF - 0x204u);
                rolled++;
            }
            /* No wrap is involved, so the cascade and the draw packet are contiguous. */
            for (unsigned i = 0u; i < sc->sites; i++) {
                if (sc->first_word[i] != 0u)
                    CHECK_EQ_U32(load(cursor + sc->offset[i]), sc->first_word[i]);
            }
            CHECK_EQ_U32(load(cursor + sc->total), 0x417FCu);
            CHECK_EQ_U32(load(cursor + sc->total + 4u), 8u);
            CHECK_EQ_U32(result, cursor + sc->total + 24u);
            CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), result);
            CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK), (sc->mask & 0xFFFFFFAFu) & 0xC0000070u);
            environment_end();
        }
    }
    CHECK(rolled > 30u);
}

static void test_refusal_inside_the_cascade_precedes_every_write(void)
{
    /* Device flag 4 makes any roll-over refuse. The draw must refuse BEFORE its first write when
     * a roll-over would fall anywhere in the cascade or at its stream work, even at the last
     * site, and must complete when none does. MUTATION: dropping a site from the plan, or
     * mis-sizing one, lets the refusal arrive after earlier packets are written. */
    unsigned refused = 0u;
    for (unsigned which = 0u; which < SCENARIO_COUNT; which++) {
        const scenario *sc = &SCENARIOS[which];
        for (uint32_t back = 0u; back <= sc->total + 8u; back += 4u) {
            const uint32_t base = seed_scenario(sc, 4u);
            const uint32_t limit = base + 0x4000u;
            const uint32_t cursor = limit - back;
            place(cursor, limit);
            snapshot shot;
            take(&shot, cursor);
            RUN_EXPECTING_FATAL((void)d3d8_draw_vertices(8u, 17u, 3u));
            if (first_roll_offset(sc, back) != UINT32_MAX) {
                CHECK(fatal_seen);
                CHECK_EQ_U32(fatal_address, 0x003D69E0u);
                CHECK(unchanged(&shot, cursor));
                CHECK_EQ_U32(rolls.calls, 0u);
                refused++;
            } else {
                CHECK(!fatal_seen);
                CHECK_EQ_U32(load(cursor + sc->total), 0x417FCu);
            }
            environment_end();
        }
    }
    CHECK(refused > 30u);
}

static void test_wrap_between_two_emitters(void)
{
    const uint32_t base = seed(KICKOFF);
    const uint32_t end = base + RING_BYTES;
    const uint32_t cursor = end - 0x2000u;
    /* Limit 12 bytes on: the program (offset 0) and stage 0 (offset 8) see cursor < limit, and the
     * lighting packets (offset 28) see it passed, so the ring wraps there: half a kickoff does not
     * fit below the end. The jump word goes at the old cursor and the lighting packets at the
     * base. */
    place(cursor, cursor + 12u);
    store(D3D8_GLOBAL_DIRTY_MASK, LIVE_MASK);
    const uint32_t result = d3d8_draw_vertices(8u, 17u, 3u);
    const uint32_t wrap_at = cursor + PROGRAM_BYTES + STAGE_BYTES;
    CHECK_EQ_U32(rolls.calls, 1u);
    CHECK_EQ_U32(rolls.cursors[0], wrap_at);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE + 0x40u), 1u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE + 0x44u), wrap_at - base);
    CHECK_EQ_U32(load(wrap_at), (base & 0x0FFFFFFFu) + 1u);
    CHECK_EQ_U32(load(cursor), 0x41E70u);
    CHECK_EQ_U32(load(cursor + 8u), 0x81B08u);
    CHECK_EQ_U32(load(base), 0x40314u);
    CHECK_EQ_U32(load(base + LIGHT_BYTES), 0x417FCu);
    CHECK_EQ_U32(result, base + LIGHT_BYTES + 24u);
    CHECK_EQ_U32(device_limit(), base + KICKOFF - 0x204u);
    environment_end();
}

static void test_indexed_route_plans_the_cascade_over_a_shared_simulation(void)
{
    /* T546: the indexed draw lays its packet out from where the simulation says the cascade ends, refills included. The
     * cascade (program 8, stage 20 and lighting 32 bytes) is planned over the caller's simulation: no roll-over leaves
     * the cursor at the entry cursor plus the bytes, and one in a site moves what follows it. */
    const uint32_t base = seed(KICKOFF);
    const uint32_t limit = base + 0x4000u;
    place(limit - 0x100u, limit);
    store(D3D8_GLOBAL_DIRTY_MASK, LIVE_MASK);
    d3d8_pushbuffer_sim sim = d3d8_pushbuffer_sim_start();
    d3d8_draw_plan_deferred(&sim, 0u);
    CHECK_EQ_U32(sim.cursor, limit - 0x100u + CASCADE_BYTES);
    CHECK_EQ_U32(sim.bytes, CASCADE_BYTES);
    CHECK_EQ_U32(sim.refills, 0u);
    CHECK_EQ_U32(sim.span_count, 1u);
    CHECK_EQ_U32(sim.span[0].begin, limit - 0x100u);
    CHECK_EQ_U32(sim.span[0].bytes, CASCADE_BYTES);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), limit - 0x100u);
    /* The lighting packets (offset 28) are the last site: a limit that reaches them plans a roll-over there, and the
     * cursor after the cascade is the new segment's, not the entry cursor plus the bytes. */
    const uint32_t entry = limit - (PROGRAM_BYTES + STAGE_BYTES);
    place(entry, limit);
    sim = d3d8_pushbuffer_sim_start();
    d3d8_draw_plan_deferred(&sim, 0u);
    CHECK_EQ_U32(sim.refills, 1u);
    CHECK_EQ_U32(sim.bytes, CASCADE_BYTES);
    CHECK_EQ_U32(sim.cursor, entry + PROGRAM_BYTES + STAGE_BYTES + LIGHT_BYTES);
    CHECK_EQ_U32(sim.limit, entry + PROGRAM_BYTES + STAGE_BYTES + KICKOFF - 0x204u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), entry);
    environment_end();
}

static void test_sized_reservation_of_the_draw(void)
{
    /* 0x003D6B30(chunks + 5): below limit + 0x200 nothing happens. A tiny kickoff of 0x800 makes the
     * raise visible: 773 dwords need 3092 + 0x204 = 0x0E18 bytes, more than the kickoff, so BOTH
     * sizes become 0x0E18 and the new limit is cursor + 0x0E18 - 0x204. */
    const uint32_t base = seed(0x800u);
    place(base + 0x100u, base + 0x100u);
    store(D3D8_GLOBAL_DIRTY_MASK, 0u);
    const uint32_t count = 0x30000u;
    const uint32_t chunks = 0x300u;
    const uint32_t result = d3d8_draw_vertices(8u, 0u, count);
    CHECK_EQ_U32(rolls.calls, 1u);
    CHECK_EQ_U32(rolls.cursors[0], base + 0x100u);
    CHECK_EQ_U32(device_limit(), base + 0x100u + (chunks + 5u) * 4u);
    CHECK_EQ_U32(load(base + 0x100u), 0x417FCu);
    CHECK_EQ_U32(result, base + 0x100u + (chunks + 5u) * 4u);
    environment_end();

    /* A request that still fits below limit + 0x200 does not roll even past the limit itself. */
    const uint32_t second = seed(KICKOFF);
    place(second + 0x4000u, second + 0x4000u - 0x100u);
    store(D3D8_GLOBAL_DIRTY_MASK, 0u);
    (void)d3d8_draw_vertices(8u, 0u, 3u);
    CHECK_EQ_U32(rolls.calls, 0u);
    CHECK_EQ_U32(device_limit(), second + 0x4000u - 0x100u);
    environment_end();

    /* A request that would run past the ring's end refuses before anything is written. */
    const uint32_t third = seed(KICKOFF);
    place(third + 0x4000u, third + 0x4000u);
    store(D3D8_GLOBAL_DIRTY_MASK, 0u);
    snapshot shot;
    take(&shot, third + 0x4000u);
    RUN_EXPECTING_FATAL((void)d3d8_draw_vertices(8u, 0u, 0x10000000u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003D69E0u);
    CHECK(unchanged(&shot, third + 0x4000u));
    environment_end();
}

static void test_oversized_draw_refuses_before_the_cascade_writes(void)
{
    /* The draw's own reservation is part of the whole-draw plan: a packet that cannot fit the ring
     * must refuse before the cascade writes anything. MUTATION: planning only the cascade lets the
     * refusal arrive after its packets. */
    const uint32_t base = seed(KICKOFF);
    place(base + 0x100u, base + 0x4000u);
    store(D3D8_GLOBAL_DIRTY_MASK, LIVE_MASK);
    snapshot shot;
    take(&shot, base + 0x100u);
    RUN_EXPECTING_FATAL((void)d3d8_draw_vertices(8u, 0u, 0x10000000u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003D69E0u);
    CHECK(unchanged(&shot, base + 0x100u));
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK), LIVE_MASK);
    environment_end();
}

static void test_zero_count_refuses_before_the_cascade(void)
{
    const uint32_t base = seed(KICKOFF);
    place(base + 0x100u, base + 0x4000u);
    store(D3D8_GLOBAL_DIRTY_MASK, LIVE_MASK);
    snapshot shot;
    take(&shot, base + 0x100u);
    RUN_EXPECTING_FATAL((void)d3d8_draw_vertices(8u, 0u, 0u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003D4FB0u);
    CHECK(unchanged(&shot, base + 0x100u));
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK), LIVE_MASK);
    environment_end();
}

int main(void)
{
    test_cascade_rolls_at_the_site_the_original_does();
    test_refusal_inside_the_cascade_precedes_every_write();
    test_wrap_between_two_emitters();
    test_indexed_route_plans_the_cascade_over_a_shared_simulation();
    test_sized_reservation_of_the_draw();
    test_oversized_draw_refuses_before_the_cascade_writes();
    test_zero_count_refuses_before_the_cascade();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
