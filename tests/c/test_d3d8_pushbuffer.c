/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The D3D8 pushbuffer ring and the 0x003D6C90 primitive (src/gpu/d3d8_pushbuffer.c).
 *
 * THE EXPECTED NUMBERS come from the original's own arithmetic: the limit is the kickoff segment
 * less 0x204 bytes (0x003D63A2), a segment that would end within 0x4000 of the ring's end is
 * clamped to it when half a kickoff still fits and otherwise wraps (0x003D6A37 onwards), and the
 * wrap writes a jump word, `(base & 0xFFFFFFF) + 1`, at the old cursor (0x003D6A5F). Run under the
 * emulated oracle, the original's `CreateDevice` produced the base, end, cursor and limit fields
 * these checks derive from the sizes 0x100000 and 0x10000.
 */

#include "test_d3d8_support.h"

#define KICKOFF 0x10000u
#define RING_BYTES 0x100000u

static struct {
    unsigned calls;
    uint32_t begin;
    uint32_t end;
} consumed;

static void recording_consumer(void *context, uint32_t begin, uint32_t end)
{
    (void)context;
    consumed.calls++;
    consumed.begin = begin;
    consumed.end = end;
}

static uint32_t create_ring(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    memset(&consumed, 0, sizeof(consumed));
    d3d8_pushbuffer_set_consumer(recording_consumer, NULL);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, RING_BYTES);
    store(D3D8_GLOBAL_KICKOFF_SIZE, KICKOFF);
    CHECK(d3d8_pushbuffer_create());
    return load(0x003E3F60u + 0x24u);
}

static void test_creation_derives_the_fields(void)
{
    const uint32_t base = create_ring();
    CHECK(base != 0u);
    CHECK_EQ_U32(base & 0xFFFu, 0u);

    /* device+0x28 is the end, device+0 the cursor, device+4 the limit, device+0x2C the mode.
     * MUTATION: size and kickoff swapped, or the 0x204 slack dropped, fails one of these. */
    CHECK_EQ_U32(load(0x003E3F60u + 0x28u), base + 0x100000u);
    CHECK_EQ_U32(load(0x003E3F60u + 0x00u), base);
    CHECK_EQ_U32(load(0x003E3F60u + 0x04u), base + 0x10000u - 0x204u);
    CHECK_EQ_U32(load(0x003E3F60u + 0x2Cu), 5u);

    /* The kernel was asked for write-combined memory, protect 0x404, as the original's wrapper
     * asks (the oracle logged 0x404 for the pushbuffer). MUTATION: a plain read-write request is
     * recorded as 4. */
    const guest_region *region = guest_region_at(base);
    CHECK(region != NULL);
    if (region != NULL) {
        CHECK_EQ_U32(region->protect, 0x404u);
    }

    /* The sizes round down to a dword. */
    environment_end();
    environment_begin(KERNEL_AV_PACK_HDTV);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100003u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10003u);
    CHECK(d3d8_pushbuffer_create());
    const uint32_t odd = load(0x003E3F60u + 0x24u);
    CHECK_EQ_U32(load(0x003E3F60u + 0x28u), odd + 0x100000u);
    CHECK_EQ_U32(load(0x003E3F60u + 0x04u), odd + 0x10000u - 0x204u);
    environment_end();
}

static void test_primitive_writes_pairs_and_advances(void)
{
    const uint32_t base = create_ring();

    /* The primitive stores the header then the value and leaves the cursor eight bytes on.
     * MUTATION: a swapped pair, or a cursor advanced by 4, fails here. */
    d3d8_pushbuffer_emit_pair(0x00040308u, 0u);
    CHECK_EQ_U32(load(base + 0u), 0x00040308u);
    CHECK_EQ_U32(load(base + 4u), 0u);
    CHECK_EQ_U32(load(0x003E3F60u), base + 8u);

    d3d8_pushbuffer_emit_pair(0x0004039Cu, 0x404u);
    CHECK_EQ_U32(load(base + 8u), 0x0004039Cu);
    CHECK_EQ_U32(load(base + 12u), 0x404u);
    CHECK_EQ_U32(load(0x003E3F60u), base + 16u);
    CHECK(d3d8_pushbuffer_dwords_written() == 4u);
    CHECK(d3d8_pushbuffer_rollovers() == 0u);
    CHECK_EQ_U32(consumed.calls, 0u);
    environment_end();
}

static void test_begin_and_end(void)
{
    const uint32_t base = create_ring();
    /* begin returns the cursor without moving it, and end publishes and counts the dwords. */
    CHECK_EQ_U32(d3d8_pushbuffer_begin(), base);
    store(base, 0x1111u);
    store(base + 4u, 0x2222u);
    store(base + 8u, 0x3333u);
    d3d8_pushbuffer_end(base + 12u);
    CHECK_EQ_U32(load(0x003E3F60u), base + 12u);
    CHECK(d3d8_pushbuffer_dwords_written() == 3u);
    environment_end();
}

static void test_plain_rollover_moves_the_limit(void)
{
    const uint32_t base = create_ring();
    const uint32_t limit = base + 0x10000u - 0x204u;

    /* A cursor 4 bytes short of the limit: the next pair would reach it, so the ring rolls over
     * FIRST and writes the pair afterwards. The segment [base, cursor) goes to the consumer, the
     * new limit is cursor + kickoff - 0x204, and nothing wraps (the ring has room). MUTATION: a
     * limit of cursor + kickoff/2 or a missing consumer call fails here. */
    store(0x003E3F60u, limit - 4u);
    d3d8_pushbuffer_emit_pair(0xAAAA0001u, 0xBBBB0002u);
    CHECK(d3d8_pushbuffer_rollovers() == 1u);
    CHECK_EQ_U32(consumed.calls, 1u);
    CHECK_EQ_U32(consumed.begin, base);
    CHECK_EQ_U32(consumed.end, limit - 4u);
    CHECK_EQ_U32(load(0x003E3F60u + 0x04u), (limit - 4u) + 0x10000u - 0x204u);
    CHECK_EQ_U32(load(limit - 4u), 0xAAAA0001u);
    CHECK_EQ_U32(load(limit), 0xBBBB0002u);
    CHECK_EQ_U32(load(0x003E3F60u), limit + 4u);
    CHECK_EQ_U32(load(0x003E3F60u + 0x40u), 0u);

    /* The next segment starts where the cursor was handed off. */
    store(0x003E3F60u, load(0x003E3F60u + 4u));
    d3d8_pushbuffer_emit_pair(1u, 2u);
    CHECK_EQ_U32(consumed.calls, 2u);
    CHECK_EQ_U32(consumed.begin, limit - 4u);
    environment_end();
}

static void test_rollover_near_the_end_clamps_or_wraps(void)
{
    uint32_t base = create_ring();
    uint32_t end = base + RING_BYTES;

    /* Clamp: the candidate end (cursor + 0x10000) would come within 0x4000 of the ring's end, but
     * half a kickoff (0x8000) still fits, so the segment is cut to the ring's end and the limit is
     * the end less the slack. The cursor stays. MUTATION: wrapping here instead moves the cursor
     * to the base. */
    store(0x003E3F60u, end - 0xA000u);
    store(0x003E3F60u + 4u, end - 0xA000u);
    d3d8_pushbuffer_begin();
    CHECK(d3d8_pushbuffer_rollovers() == 1u);
    CHECK_EQ_U32(load(0x003E3F60u + 0x04u), end - 0x204u);
    CHECK_EQ_U32(load(0x003E3F60u), end - 0xA000u);
    CHECK_EQ_U32(load(0x003E3F60u + 0x40u), 0u);
    environment_end();

    /* The margin: a candidate end that is still below the ring's end but within 0x4000 of it is
     * clamped to it too. cursor + 0x10000 is end - 0x2000 here. MUTATION: comparing the candidate
     * with the end alone leaves the limit at candidate - 0x204 = end - 0x2204. */
    base = create_ring();
    end = base + RING_BYTES;
    store(0x003E3F60u, end - 0x12000u);
    store(0x003E3F60u + 4u, end - 0x12000u);
    d3d8_pushbuffer_begin();
    CHECK_EQ_U32(load(0x003E3F60u + 0x04u), end - 0x204u);
    CHECK_EQ_U32(load(0x003E3F60u), end - 0x12000u);
    environment_end();

    /* The boundary: cursor + half a kickoff lands exactly ON the ring end, which still fits (the
     * original's `jbe`), so the segment is clamped, not wrapped. MUTATION: `>=` wraps here. */
    base = create_ring();
    end = base + RING_BYTES;
    store(0x003E3F60u, end - 0x8000u);
    store(0x003E3F60u + 4u, end - 0x8000u);
    d3d8_pushbuffer_begin();
    CHECK_EQ_U32(load(0x003E3F60u + 0x04u), end - 0x204u);
    CHECK_EQ_U32(load(0x003E3F60u), end - 0x8000u);
    CHECK_EQ_U32(load(0x003E3F60u + 0x40u), 0u);
    environment_end();

    /* Wrap: half a kickoff does NOT fit. The old cursor gets the jump word, the wrap count and
     * the distance are recorded, and the cursor restarts at the base with a fresh segment. */
    base = create_ring();
    end = base + RING_BYTES;
    const uint32_t old_cursor = end - 0x2000u;
    store(0x003E3F60u, old_cursor);
    store(0x003E3F60u + 4u, old_cursor);
    const uint32_t returned = d3d8_pushbuffer_begin();
    CHECK_EQ_U32(returned, base);
    CHECK_EQ_U32(load(0x003E3F60u), base);
    CHECK_EQ_U32(load(0x003E3F60u + 0x04u), base + 0x10000u - 0x204u);
    CHECK_EQ_U32(load(0x003E3F60u + 0x40u), 1u);
    CHECK_EQ_U32(load(0x003E3F60u + 0x44u), old_cursor - base);
    /* The jump: the low 28 bits of the base, plus one. MUTATION: a missing +1 or a missing mask
     * changes the word. */
    CHECK_EQ_U32(load(old_cursor), (base & 0x0FFFFFFFu) + 1u);
    /* The consumer saw the segment up to the wrap point, and the jump word is not part of it. */
    CHECK_EQ_U32(consumed.end, old_cursor);

    /* A second wrap counts again, and records ITS distance. MUTATION: a counter stored as 1 stays
     * at 1. */
    const uint32_t second_cursor = end - 0x3000u;
    store(0x003E3F60u, second_cursor);
    store(0x003E3F60u + 4u, second_cursor);
    (void)d3d8_pushbuffer_begin();
    CHECK_EQ_U32(load(0x003E3F60u + 0x40u), 2u);
    CHECK_EQ_U32(load(0x003E3F60u + 0x44u), second_cursor - base);
    environment_end();
}

static void test_use_before_creation_is_fatal(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    /* The device region is zero: no ring exists. The original would write through a null cursor.
     * MUTATION: letting the roll-over proceed loops or writes at address 0. */
    RUN_EXPECTING_FATAL(d3d8_pushbuffer_emit_pair(1u, 2u));
    CHECK(fatal_seen);
    CHECK(fatal_address == 0x003D6B20u);
    environment_end();
}

static void test_allocation_failure_is_reported(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    /* A size the allocator cannot satisfy: the original returns E_OUTOFMEMORY and so does the
     * port, as `false`. */
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0xF0000000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, KICKOFF);
    CHECK(!d3d8_pushbuffer_create());
    CHECK_EQ_U32(load(0x003E3F60u + 0x24u), 0u);
    environment_end();
}

/* The read-only previews must predict exactly what the real roll-over does (T368), because the
 * draw cascade plans with them and then runs the real thing. */
static void test_previews_predict_the_real_rollover(void)
{
    static const struct {
        uint32_t cursor_back_from_end;
        uint32_t limit_offset; /* limit relative to the cursor */
        const char *what;
    } cases[] = {
        {0xA000u, 0u, "clamp"}, {0x12000u, 0u, "margin clamp"}, {0x2000u, 0u, "wrap"},
        {0x30000u, 0u, "plain"}, {0x30000u, 4u, "plain, cursor short of the limit"},
    };
    for (unsigned i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const uint32_t base = create_ring();
        const uint32_t end = base + RING_BYTES;
        const uint32_t cursor = end - cases[i].cursor_back_from_end;
        const uint32_t limit = cursor + cases[i].limit_offset;
        store(0x003E3F60u, cursor);
        store(0x003E3F60u + 4u, limit);
        uint32_t predicted_cursor = cursor;
        uint32_t predicted_limit = limit;
        const bool rolls = d3d8_pushbuffer_preview_begin(&predicted_cursor, &predicted_limit);
        /* Read-only: nothing moved. */
        CHECK_EQ_U32(load(0x003E3F60u), cursor);
        CHECK_EQ_U32(load(0x003E3F60u + 4u), limit);
        CHECK(d3d8_pushbuffer_rollovers() == 0u);
        CHECK(rolls == (cursor >= limit));
        const uint32_t real = d3d8_pushbuffer_begin();
        CHECK_EQ_U32(real, predicted_cursor);
        CHECK_EQ_U32(load(0x003E3F60u + 4u), predicted_limit);
        CHECK(d3d8_pushbuffer_rollovers() == (rolls ? 1u : 0u));
        environment_end();
    }
}

static void test_sized_reservation_arithmetic(void)
{
    uint32_t base = create_ring();
    /* 0x003D6B30(n): refill when cursor + n*4 >= limit + 0x200, the new sizes are the larger of
     * the kickoff (or half) and n*4 + 0x204. Kickoff 0x10000: 200 dwords refill without a raise, 0x4000 dwords do
     * not and raise both sizes to 0x10204. MUTATION: `>` for `>=` misses the boundary case. */
    store(0x003E3F60u, base + 0x1000u);
    store(0x003E3F60u + 4u, base + 0x1000u);
    uint32_t cursor = base + 0x1000u;
    uint32_t limit = base + 0x1000u;
    CHECK(d3d8_pushbuffer_preview_reserve(200u, &cursor, &limit));
    CHECK_EQ_U32(limit, base + 0x1000u + 0x10000u - 0x204u);
    cursor = base + 0x1000u;
    limit = base + 0x1000u;
    CHECK(d3d8_pushbuffer_preview_reserve(0x4000u, &cursor, &limit));
    CHECK_EQ_U32(limit, base + 0x1000u + 0x10000u);
    /* The boundary: 128 dwords is exactly 0x200 bytes, which is not below limit + 0x200. */
    cursor = base + 0x1000u;
    limit = base + 0x1000u;
    CHECK(d3d8_pushbuffer_preview_reserve(128u, &cursor, &limit));
    cursor = base + 0x1000u;
    limit = base + 0x1000u;
    CHECK(!d3d8_pushbuffer_preview_reserve(127u, &cursor, &limit));
    CHECK_EQ_U32(cursor, base + 0x1000u);
    CHECK_EQ_U32(limit, base + 0x1000u);
    CHECK_EQ_U32(d3d8_pushbuffer_reserve(127u), base + 0x1000u);
    CHECK(d3d8_pushbuffer_rollovers() == 0u);
    CHECK_EQ_U32(d3d8_pushbuffer_reserve(0x4000u), base + 0x1000u);
    CHECK(d3d8_pushbuffer_rollovers() == 1u);
    CHECK_EQ_U32(load(0x003E3F60u + 4u), base + 0x1000u + 0x10000u);
    environment_end();
    (void)base;
}

static void test_refills_the_model_cannot_do_refuse(void)
{
    uint32_t base = create_ring();
    store(0x003E3F60u, base + 0x100u);
    store(0x003E3F60u + 4u, base + 0x100u);
    /* Device flag 4 is the original's alternate put pointer: refused, by the preview too. */
    store(0x003E3F60u + 8u, load(0x003E3F60u + 8u) | 4u);
    uint32_t cursor = base + 0x100u;
    uint32_t limit = base + 0x100u;
    RUN_EXPECTING_FATAL((void)d3d8_pushbuffer_preview_begin(&cursor, &limit));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003D69E0u);
    RUN_EXPECTING_FATAL((void)d3d8_pushbuffer_begin());
    CHECK(fatal_seen);
    CHECK_EQ_U32(load(0x003E3F60u), base + 0x100u);
    CHECK(d3d8_pushbuffer_rollovers() == 0u);
    CHECK_EQ_U32(consumed.calls, 0u);
    environment_end();

    /* A segment longer than the ring would run past its end. */
    base = create_ring();
    store(0x003E3F60u, base + 0x100u);
    store(0x003E3F60u + 4u, base + 0x100u);
    cursor = base + 0x100u;
    limit = base + 0x100u;
    RUN_EXPECTING_FATAL((void)d3d8_pushbuffer_preview_reserve(0x40000u, &cursor, &limit));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003D69E0u);
    RUN_EXPECTING_FATAL((void)d3d8_pushbuffer_reserve(0x40000u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(load(0x003E3F60u), base + 0x100u);
    CHECK_EQ_U32(load(0x003E3F60u + 4u), base + 0x100u);
    environment_end();
}

static void test_simulated_sites_advance_and_refuse_unmapped_spans(void)
{
    const uint32_t base = create_ring();
    const uint32_t limit = base + 0x1000u;
    store(0x003E3F60u, limit - 8u);
    store(0x003E3F60u + 4u, limit);
    d3d8_pushbuffer_sim sim = d3d8_pushbuffer_sim_start();
    CHECK_EQ_U32(sim.cursor, limit - 8u);
    d3d8_pushbuffer_sim_site(&sim, 0x1234u, 8u); /* cursor < limit: no roll, ends at the limit */
    CHECK_EQ_U32(sim.refills, 0u);
    CHECK_EQ_U32(sim.cursor, limit);
    d3d8_pushbuffer_sim_site(&sim, 0x1234u, 20u); /* at the limit: rolls first */
    CHECK_EQ_U32(sim.refills, 1u);
    CHECK_EQ_U32(sim.bytes, 28u);
    CHECK_EQ_U32(sim.cursor, limit + 20u);
    CHECK_EQ_U32(sim.limit, limit + 0x10000u - 0x204u);
    /* Nothing real moved. */
    CHECK_EQ_U32(load(0x003E3F60u), limit - 8u);
    CHECK_EQ_U32(load(0x003E3F60u + 4u), limit);
    /* A span that leaves mapped guest memory refuses. */
    sim.cursor = base + RING_BYTES - 4u;
    sim.limit = base + RING_BYTES;
    RUN_EXPECTING_FATAL(d3d8_pushbuffer_sim_site(&sim, 0x1234u, 8u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x1234u);
    environment_end();
}


/* T487: a site written in units with a re-check between two of them (the vertex constants emitter: a head, FULL
 * chunks of 68 bytes, a tail). The numbers are the original's (0x003D5877: `cmp edi, [limit]` after a full chunk when
 * another full chunk follows, then the refill with the cursor published). No GPU module is attached here, so the
 * refill writes no fence packet (the equality with the original, fence packets included, is
 * tests/test_d3d8_vertex_constants_refill_oracle.py). */
static void test_split_sites_refill_between_two_units(void)
{
    const uint32_t base = create_ring();
    const uint32_t first = base + 0x1000u;
    d3d8_pushbuffer_split plan;
    /* limit one byte above the last boundary (8 + 2 * 68 = 144): no refill, one run of 8 + 3 * 68 + 12 bytes */
    store(0x003E3F60u, first);
    store(0x003E3F60u + 4u, first + 145u);
    d3d8_pushbuffer_sim sim = d3d8_pushbuffer_sim_start();
    d3d8_pushbuffer_sim_split_site(&sim, 0x1234u, 8u, 68u, 3u, 12u, &plan);
    CHECK(!plan.split);CHECK(!plan.entered);
    CHECK_EQ_U32(plan.begin, first);
    CHECK_EQ_U32(plan.runs, 1u);
    CHECK_EQ_U32(plan.run[0].begin, first);
    CHECK_EQ_U32(plan.run[0].bytes, 8u + 3u * 68u + 12u);
    CHECK_EQ_U32(sim.refills, 0u);
    CHECK_EQ_U32(sim.cursor, first + 224u);
    CHECK_EQ_U32(sim.bytes, 224u);
    /* the limit AT the first boundary: the refill splits the site after one unit */
    store(0x003E3F60u + 4u, first + 76u);
    sim = d3d8_pushbuffer_sim_start();
    d3d8_pushbuffer_sim_split_site(&sim, 0x1234u, 8u, 68u, 3u, 12u, &plan);
    CHECK(plan.split);CHECK(!plan.entered);
    CHECK_EQ_U32(plan.runs, 2u);
    CHECK(!plan.run[0].wraps);
    CHECK_EQ_U32(plan.run[0].begin, first);
    CHECK_EQ_U32(plan.run[0].bytes, 76u);
    CHECK_EQ_U32(plan.run[1].begin, first + 76u);
    CHECK_EQ_U32(plan.run[1].bytes, 224u - 76u);
    CHECK_EQ_U32(plan.run[0].fence_bytes, 0u);
    CHECK_EQ_U32(sim.refills, 1u);
    CHECK_EQ_U32(sim.limit, first + 76u + KICKOFF - 0x204u);
    CHECK_EQ_U32(sim.cursor, first + 224u);
    /* nothing real moved */
    CHECK_EQ_U32(load(0x003E3F60u), first);CHECK_EQ_U32(load(0x003E3F60u + 4u), first + 76u);
    CHECK_EQ_U32(d3d8_pushbuffer_rollovers(), 0u);
    /* the last unit is never followed by a check, and neither is a tail: two units and a tail, a limit passed by the
     * first unit refills once and a limit only the second unit passes does not */
    store(0x003E3F60u + 4u, first + 100u);
    sim = d3d8_pushbuffer_sim_start();
    d3d8_pushbuffer_sim_split_site(&sim, 0x1234u, 8u, 68u, 2u, 8u, &plan);
    CHECK(!plan.split);
    CHECK_EQ_U32(sim.refills, 0u);
    /* a single unit is never checked */
    store(0x003E3F60u + 4u, first + 20u);
    sim = d3d8_pushbuffer_sim_start();
    d3d8_pushbuffer_sim_split_site(&sim, 0x1234u, 8u, 68u, 1u, 0u, &plan);
    CHECK(!plan.split);
    /* the entry roll-over alone: a cursor at the limit, then the site in one run after it */
    store(0x003E3F60u, first);store(0x003E3F60u + 4u, first);
    sim = d3d8_pushbuffer_sim_start();
    d3d8_pushbuffer_sim_split_site(&sim, 0x1234u, 8u, 68u, 2u, 0u, &plan);
    CHECK(!plan.split);CHECK_EQ_U32(sim.refills, 1u);CHECK_EQ_U32(plan.begin, first);
    /* T526: a second refill in one site is planned from what the first left. An entry roll-over and a split (a small
     * kickoff), then two splits, then many. */
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x400u);
    /* the entry roll-over sets the limit 0x1FC bytes on, so the check after unit 8 (8 + 8 * 68 = 552 bytes in) is
     * the ONE split: two refills in all */
    sim = d3d8_pushbuffer_sim_start();
    d3d8_pushbuffer_sim_split_site(&sim, 0x1234u, 8u, 68u, 9u, 0u, &plan);
    CHECK(plan.entered);CHECK(plan.split);CHECK_EQ_U32(sim.refills, 2u);CHECK_EQ_U32(plan.runs, 2u);
    CHECK_EQ_U32(plan.run[0].begin, first);
    CHECK_EQ_U32(plan.run[0].bytes, 8u + 8u * 68u);
    CHECK_EQ_U32(plan.run[1].begin, first + 8u + 8u * 68u);
    CHECK_EQ_U32(plan.run[1].bytes, 68u);
    /* without the entry roll-over the same site splits once */
    store(0x003E3F60u + 4u, first + 0x1FCu);
    sim = d3d8_pushbuffer_sim_start();
    d3d8_pushbuffer_sim_split_site(&sim, 0x1234u, 8u, 68u, 9u, 0u, &plan);
    CHECK(!plan.entered);CHECK(plan.split);CHECK_EQ_U32(sim.refills, 1u);CHECK_EQ_U32(plan.run[0].bytes, 8u + 8u * 68u);
    /* 40 units against a limit 100 bytes in: the check after unit 1 (76) passes, after unit 2 (144) refills, and then
     * every 0x1FC bytes (7 units, 476 bytes, 0x204 slack: the next limit is 0x400 - 0x204 = 0x1FC on) */
    store(0x003E3F60u + 4u, first + 100u);
    sim = d3d8_pushbuffer_sim_start();
    d3d8_pushbuffer_sim_split_site(&sim, 0x1234u, 8u, 68u, 40u, 0u, &plan);
    CHECK(plan.split);CHECK(sim.refills > 1u);CHECK_EQ_U32(plan.runs, sim.refills + 1u);
    {
        uint32_t total = 0u;
        for (uint32_t run = 0u; run < plan.runs; run++) {
            total += plan.run[run].bytes;
            if (run + 1u < plan.runs) CHECK_EQ_U32(plan.run[run + 1u].begin, plan.run[run].begin + plan.run[run].bytes);
        }
        CHECK_EQ_U32(total, 8u + 40u * 68u);
        CHECK_EQ_U32(sim.cursor, plan.run[plan.runs - 1u].begin + plan.run[plan.runs - 1u].bytes);
    }
    /* more refills than the plan can hold are refused by name */
    store(0x003E3F60u + 4u, first + 8u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x300u);
    sim = d3d8_pushbuffer_sim_start();
    RUN_EXPECTING_FATAL(d3d8_pushbuffer_sim_split_site(&sim, 0x1234u, 8u, 68u, 400u, 0u, &plan));
    CHECK(fatal_seen);CHECK(strstr(fatal_text, "refilling more than") != NULL);
    CHECK_EQ_U32(fatal_address, 0x1234u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, KICKOFF);
    /* a span running off mapped memory, and a size that wraps the address space, are refused at the entry */
    store(0x003E3F60u, base + RING_BYTES - 40u);store(0x003E3F60u + 4u, base + RING_BYTES);
    sim = d3d8_pushbuffer_sim_start();
    RUN_EXPECTING_FATAL(d3d8_pushbuffer_sim_split_site(&sim, 0x1234u, 8u, 68u, 1u, 0u, &plan));
    CHECK(fatal_seen);
    /* a split that wraps the ring: the jump word's cursor is the end of the first run and the second run starts at
     * the ring's base (the cursor is within half a kickoff of the end, so the segment cannot fit) */
    environment_end();
    const uint32_t wrap_base = create_ring();
    const uint32_t wrap_cursor = wrap_base + RING_BYTES - 0x2000u;
    store(0x003E3F60u, wrap_cursor);store(0x003E3F60u + 4u, wrap_cursor + 76u);
    d3d8_pushbuffer_set_put(wrap_cursor);
    sim = d3d8_pushbuffer_sim_start();
    d3d8_pushbuffer_sim_split_site(&sim, 0x1234u, 8u, 68u, 3u, 12u, &plan);
    CHECK(plan.split);CHECK(plan.run[0].wraps);
    CHECK_EQ_U32(plan.run[0].bytes, 76u);CHECK_EQ_U32(plan.run[1].begin, wrap_base);
    CHECK_EQ_U32(sim.cursor, wrap_base + 224u - 76u);
    environment_end();
}

/* T579: the fixed vertex constants emitter 0x003D5720 has no entry preamble: it tests `cursor + bytes >= limit` and
 * refills, then tests again from the new cursor and limit. No GPU module is attached here, so the refill writes no
 * fence packet and leaves the cursor where it was (the equality with the original, fence packets included, is
 * tests/test_d3d8_four_vertex_constants_refill_oracle.py). A refill leaves a limit `kickoff - 0x204` on, so another
 * follows while that is at most `bytes` away: kickoff 0x250 does not return, 0x254 refills once. */
static void test_checked_sites_refill_until_the_packet_fits(void)
{
    const uint32_t base = create_ring();
    const uint32_t first = base + 0x1000u;
    d3d8_pushbuffer_split plan;
    /* the limit one byte above cursor + 76: no refill, one run of 76 bytes at the cursor */
    store(0x003E3F60u, first);
    store(0x003E3F60u + 4u, first + 77u);
    d3d8_pushbuffer_sim sim = d3d8_pushbuffer_sim_start();
    d3d8_pushbuffer_sim_checked_site(&sim, 0x1234u, 76u, &plan);
    CHECK(!plan.split);CHECK(!plan.entered);
    CHECK_EQ_U32(plan.begin, first);CHECK_EQ_U32(plan.runs, 1u);
    CHECK_EQ_U32(plan.run[0].begin, first);CHECK_EQ_U32(plan.run[0].bytes, 76u);
    CHECK_EQ_U32(sim.refills, 0u);CHECK_EQ_U32(sim.cursor, first + 76u);CHECK_EQ_U32(sim.bytes, 76u);
    CHECK_EQ_U32(sim.span_count, 1u);CHECK_EQ_U32(sim.span[0].begin, first);CHECK_EQ_U32(sim.span[0].bytes, 76u);
    /* the limit AT cursor + 76 refills: an empty run, then the packet after the refill */
    store(0x003E3F60u + 4u, first + 76u);
    sim = d3d8_pushbuffer_sim_start();
    d3d8_pushbuffer_sim_checked_site(&sim, 0x1234u, 76u, &plan);
    CHECK(plan.split);CHECK_EQ_U32(plan.runs, 2u);
    CHECK_EQ_U32(plan.run[0].begin, first);CHECK_EQ_U32(plan.run[0].bytes, 0u);CHECK(!plan.run[0].wraps);
    CHECK_EQ_U32(plan.run[0].fence_bytes, 0u);
    CHECK_EQ_U32(plan.run[1].begin, first);CHECK_EQ_U32(plan.run[1].bytes, 76u);
    CHECK_EQ_U32(plan.begin, first);
    CHECK_EQ_U32(sim.refills, 1u);CHECK_EQ_U32(sim.limit, first + KICKOFF - 0x204u);
    CHECK_EQ_U32(sim.cursor, first + 76u);CHECK_EQ_U32(sim.bytes, 76u);
    /* a limit at or below the cursor refills as well, and nothing real moved */
    store(0x003E3F60u + 4u, first - 4u);
    sim = d3d8_pushbuffer_sim_start();
    d3d8_pushbuffer_sim_checked_site(&sim, 0x1234u, 76u, &plan);
    CHECK_EQ_U32(sim.refills, 1u);CHECK_EQ_U32(plan.runs, 2u);
    CHECK_EQ_U32(load(0x003E3F60u), first);CHECK_EQ_U32(load(0x003E3F60u + 4u), first - 4u);
    CHECK_EQ_U32(d3d8_pushbuffer_rollovers(), 0u);
    store(0x003E3F60u + 4u, first + 76u);
    /* the new limit is only 76 on: the next refill follows at once and never ends */
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x250u);
    sim = d3d8_pushbuffer_sim_start();
    RUN_EXPECTING_FATAL(d3d8_pushbuffer_sim_checked_site(&sim, 0x1234u, 76u, &plan));
    CHECK(fatal_seen);CHECK(strstr(fatal_text, "refilling more than") != NULL);
    CHECK_EQ_U32(fatal_address, 0x1234u);
    /* one byte (four) more of kickoff and the packet fits after the first refill */
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x254u);
    sim = d3d8_pushbuffer_sim_start();
    d3d8_pushbuffer_sim_checked_site(&sim, 0x1234u, 76u, &plan);
    CHECK_EQ_U32(sim.refills, 1u);CHECK_EQ_U32(plan.runs, 2u);CHECK_EQ_U32(sim.limit, first + 80u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, KICKOFF);
    environment_end();
    /* a refill that wraps the ring: the jump word is at the old cursor, the packet at the ring's base */
    const uint32_t wrap_base = create_ring();
    const uint32_t wrap_cursor = wrap_base + RING_BYTES - 0x2000u;
    store(0x003E3F60u, wrap_cursor);store(0x003E3F60u + 4u, wrap_cursor + 76u);
    d3d8_pushbuffer_set_put(wrap_cursor);
    sim = d3d8_pushbuffer_sim_start();
    d3d8_pushbuffer_sim_checked_site(&sim, 0x1234u, 76u, &plan);
    CHECK(plan.split);CHECK_EQ_U32(plan.runs, 2u);
    CHECK(plan.run[0].wraps);CHECK_EQ_U32(plan.run[0].begin, wrap_cursor);CHECK_EQ_U32(plan.run[0].bytes, 0u);
    CHECK_EQ_U32(plan.run[1].begin, wrap_base);CHECK_EQ_U32(plan.begin, wrap_base);
    CHECK_EQ_U32(sim.span_count, 2u);
    CHECK_EQ_U32(sim.span[0].begin, wrap_cursor);CHECK_EQ_U32(sim.span[0].bytes, 4u);
    CHECK_EQ_U32(sim.span[1].begin, wrap_base);CHECK_EQ_U32(sim.span[1].bytes, 76u);
    /* a packet running off mapped memory is refused where the packet is */
    store(0x003E3F60u, wrap_base + RING_BYTES - 40u);store(0x003E3F60u + 4u, wrap_base + RING_BYTES + 0x1000u);
    sim = d3d8_pushbuffer_sim_start();
    RUN_EXPECTING_FATAL(d3d8_pushbuffer_sim_checked_site(&sim, 0x1234u, 76u, &plan));
    CHECK(fatal_seen);CHECK_EQ_U32(fatal_address, 0x1234u);
    environment_end();
}

/* The refill between two units publishes the cursor, hands the first run to the consumer, wraps when it must and
 * returns the cursor the second run starts at. */
static void test_refill_at_publishes_the_cursor_and_rolls_over(void)
{
    const uint32_t base = create_ring();
    const uint32_t cursor = base + 0x1000u;
    store(0x003E3F60u, cursor - 0x100u);
    d3d8_pushbuffer_drain();
    memset(&consumed, 0, sizeof(consumed));
    const uint32_t start = d3d8_pushbuffer_refill_at(cursor);
    CHECK_EQ_U32(start, cursor);
    CHECK_EQ_U32(load(0x003E3F60u), cursor);
    CHECK_EQ_U32(load(0x003E3F60u + 4u), cursor + KICKOFF - 0x204u);
    CHECK_EQ_U32(consumed.calls, 1u);CHECK_EQ_U32(consumed.begin, cursor - 0x100u);CHECK_EQ_U32(consumed.end, cursor);
    CHECK_EQ_U32(d3d8_pushbuffer_rollovers(), 1u);
    CHECK_EQ_U32(d3d8_pushbuffer_dwords_written(), 0x100u / 4u);
    environment_end();
}

/* T391: what the refill records and refuses beyond the arithmetic when the GPU module is attached (its
 * refill tail). Every expected number is the original's (0x003D6530, 0x003D6A62 to 0x003D6A88, 0x003D6AE0)
 * and tests/test_d3d8_refill_oracle.py measures them against the original bytes. */
#define DEV 0x003E3F60u
static unsigned tail_calls;
static uint32_t tail_limit;
static uint32_t tail_cursor;
static void counting_tail(void)
{
    tail_calls++;
    tail_limit = load(DEV + 4u);
    tail_cursor = load(DEV);
}

static uint32_t create_gpu_ring(void)
{
    const uint32_t base = create_ring();
    d3d8_pushbuffer_set_refill_tail(counting_tail);
    tail_calls = 0u;
    return base;
}

static void place_cursor(uint32_t address)
{
    store(DEV, address);
    store(DEV + 4u, address);
}

static void refill_at(uint32_t cursor, uint32_t put)
{
    place_cursor(cursor);
    d3d8_pushbuffer_set_put(put);
    (void)d3d8_pushbuffer_begin();
}

static void test_the_refill_records_get_and_refuses_what_it_cannot_model(void)
{
    uint32_t base = create_gpu_ring();
    CHECK_EQ_U32(d3d8_pushbuffer_put(), base); /* create puts the GPU at the base */
    CHECK_EQ_U32(load(DEV + 0x5Cu), 0u);

    /* GET below the cursor: the shadow is GET and the generation is the wrap count. */
    store(DEV + 0x40u, 3u);
    refill_at(base + 0x1000u, base + 0x800u);
    CHECK_EQ_U32(load(DEV + 0x5Cu), base + 0x800u);
    CHECK_EQ_U32(load(DEV + 0x60u), 3u);
    environment_end();

    /* GET ahead of the cursor and past the new segment end: the generation is one behind, no wait. */
    base = create_gpu_ring();
    store(DEV + 0x40u, 3u);
    refill_at(base + 0x1000u, base + 0x50000u);
    CHECK_EQ_U32(load(DEV + 0x5Cu), base + 0x50000u);
    CHECK_EQ_U32(load(DEV + 0x60u), 2u);
    environment_end();

    /* GET inside the area the new segment would reuse: the original waits on its fence history, which the
     * port refuses, before any write. The area is (cursor, segment end], the end itself included. */
    static const uint32_t waits[] = {0x1004u, 0x2000u, 0x11000u};
    for (unsigned i = 0u; i < sizeof(waits) / sizeof(waits[0]); i++) {
        base = create_gpu_ring();
        place_cursor(base + 0x1000u);
        d3d8_pushbuffer_set_put(base + waits[i]);
        RUN_EXPECTING_FATAL((void)d3d8_pushbuffer_begin());
        CHECK(fatal_seen);
        CHECK_EQ_U32(fatal_address, 0x003D6A88u);
        CHECK_EQ_U32(load(DEV), base + 0x1000u);
        CHECK_EQ_U32(load(DEV + 4u), base + 0x1000u);
        CHECK_EQ_U32(load(DEV + 0x5Cu), 0u);
        CHECK(d3d8_pushbuffer_rollovers() == 0u);
        environment_end();
    }
    /* One byte past the end, and at the cursor itself, are not inside it. */
    base = create_gpu_ring();
    refill_at(base + 0x1000u, base + 0x11004u);
    CHECK_EQ_U32(load(DEV + 0x5Cu), base + 0x11004u);
    environment_end();
    base = create_gpu_ring();
    refill_at(base + 0x1000u, base + 0x1000u);
    CHECK_EQ_U32(load(DEV + 0x5Cu), base + 0x1000u);
    CHECK_EQ_U32(load(DEV + 0x60u), 0u); /* GET at the cursor is not ahead of it: no lap behind */
    environment_end();
    /* GET at the very first byte of the ring is inside it, and is not in the area of a refill far above. */
    base = create_gpu_ring();
    refill_at(base + 0x20000u, base);
    CHECK_EQ_U32(load(DEV + 0x5Cu), base);
    environment_end();

    /* A wrap: GET at or below the base, or ahead of the old cursor (which the original reads as the base), is
     * the GPU still at the start of the ring, so the wait. GET beyond the new segment is not. */
    static const struct {
        uint32_t get_offset;
        bool waits;
    } wraps[] = {{0u, true}, {4u, true}, {0x1000u, true}, {0x10000u, true}, {0x10004u, false},
                 {0x20000u, false}, {0xFE000u, false}, {0xFE004u, true}, {0xFF000u, true}};
    for (unsigned i = 0u; i < sizeof(wraps) / sizeof(wraps[0]); i++) {
        base = create_gpu_ring();
        place_cursor(base + RING_BYTES - 0x2000u);
        d3d8_pushbuffer_set_put(base + wraps[i].get_offset);
        RUN_EXPECTING_FATAL((void)d3d8_pushbuffer_begin());
        CHECK(fatal_seen == wraps[i].waits);
        if (wraps[i].waits) {
            CHECK_EQ_U32(fatal_address, 0x003D6A88u);
        }
        environment_end();
    }

    /* GET outside the ring: the original reads the GPU's own registers, which nothing models. */
    base = create_gpu_ring();
    place_cursor(base + 0x1000u);
    d3d8_pushbuffer_set_put(0u);
    RUN_EXPECTING_FATAL((void)d3d8_pushbuffer_begin());
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003D6530u);
    d3d8_pushbuffer_set_put(base + RING_BYTES); /* the end is outside */
    RUN_EXPECTING_FATAL((void)d3d8_pushbuffer_begin());
    CHECK(fatal_seen);
    d3d8_pushbuffer_set_put(base - 4u);
    RUN_EXPECTING_FATAL((void)d3d8_pushbuffer_begin());
    CHECK(fatal_seen);
    CHECK_EQ_U32(load(DEV), base + 0x1000u);
    environment_end();

    /* Without the GPU module there is no DMA state: the refill is the arithmetic alone, as before T391. */
    base = create_ring();
    place_cursor(base + 0x1000u);
    d3d8_pushbuffer_set_put(0u);
    RUN_EXPECTING_FATAL((void)d3d8_pushbuffer_begin());
    CHECK(!fatal_seen);
    CHECK_EQ_U32(load(DEV + 0x5Cu), 0u);
    CHECK_EQ_U32(load(DEV + 4u), base + 0x1000u + 0x10000u - 0x204u);
    environment_end();
    /* The shadow is the GPU module's too: with a put inside the ring but no module it is still not written. */
    base = create_ring();
    place_cursor(base + 0x1000u);
    d3d8_pushbuffer_set_put(base + 0x800u);
    (void)d3d8_pushbuffer_begin();
    CHECK_EQ_U32(load(DEV + 0x5Cu), 0u);
    CHECK_EQ_U32(load(DEV + 0x60u), 0u);
    environment_end();

    /* The kick moves the put: a drain at the cursor is where the GPU then is. */
    base = create_ring();
    store(DEV, base + 0x300u);
    d3d8_pushbuffer_drain();
    CHECK_EQ_U32(d3d8_pushbuffer_put(), base + 0x300u);
    environment_end();
}

/* T486: the refill's wait. The walk of 0x003D6A8E over the second history names a fence, and the fence the
 * GPU has not passed is refused before any write. tests/test_d3d8_refill_oracle.py measures the same walk
 * against the original bytes; these checks pin the pieces in isolation. */
#define WAIT_PAGE 0x70000000u
#define WAIT_HISTORY (WAIT_PAGE + 0x100u)
static bool stub_answer;
static unsigned stub_calls;
static uint32_t stub_fence;
static uint32_t stub_counter;
static uint32_t stub_semaphore;
static bool stub_reached(uint32_t fence, uint32_t counter, uint32_t semaphore)
{
    stub_calls++;
    stub_fence = fence;
    stub_counter = counter;
    stub_semaphore = semaphore;
    return stub_answer;
}

/* The ring of the unit tests is 0x100000 bytes with a 0x10000 kickoff, so the walk looks within 0x80000 of the
 * new cursor and not within 0x8000. The cursor sits at base + 0x1000, GET inside the segment about to be
 * reused, the history holds entries (fence, position) and the semaphore says 5 is the last fence passed. */
static uint32_t create_waiting_ring(uint32_t head, uint32_t wrap_delta)
{
    const uint32_t base = create_gpu_ring();
    d3d8_pushbuffer_set_fence_reached(stub_reached);
    stub_calls = 0u;
    stub_fence = 0xFFFFFFFFu;
    map_fixed(WAIT_PAGE, 0x1000u);
    store(WAIT_PAGE, 5u);
    store(DEV + 0x30u, WAIT_PAGE);
    store(DEV + 0x34u, head);
    store(DEV + 0x38u, 7u);
    store(DEV + 0x44u, wrap_delta);
    store(DEV + 0x48u, WAIT_HISTORY);
    place_cursor(base + 0x1000u);
    d3d8_pushbuffer_set_put(base + 0x2000u);
    return base;
}

static void history_entry(unsigned index, uint32_t fence, uint32_t position)
{
    store(WAIT_HISTORY + index * 8u, fence);
    store(WAIT_HISTORY + index * 8u + 4u, position);
}

static void test_the_refill_wait_names_a_fence_and_refuses_one_not_passed(void)
{
    /* Entry 1 is the head: its fence is 5 and its position is entry 0's, 0x50000 on, inside half the ring
     * and outside half the kickoff. MUTATION: half the kickoff, or the head entry's own position, goes on to
     * entry 0's fence 7. */
    uint32_t base = create_waiting_ring(1u, 0u);
    history_entry(0u, 7u, base + 0x50000u);
    history_entry(1u, 5u, base + 0x90000u);
    stub_answer = true;
    (void)d3d8_pushbuffer_begin();
    CHECK_EQ_U32(stub_calls, 1u);
    CHECK_EQ_U32(stub_fence, 5u);
    CHECK_EQ_U32(tail_calls, 1u); /* the wait passed and the refill went on */
    environment_end();

    /* The same fence not passed: refused at 0x003D6870 before any write. */
    base = create_waiting_ring(1u, 0u);
    history_entry(0u, 7u, base + 0x50000u);
    history_entry(1u, 5u, base + 0x90000u);
    stub_answer = false;
    RUN_EXPECTING_FATAL((void)d3d8_pushbuffer_begin());
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003D6870u);
    CHECK_EQ_U32(load(DEV), base + 0x1000u);
    CHECK_EQ_U32(load(DEV + 4u), base + 0x1000u);
    CHECK_EQ_U32(load(DEV + 0x5Cu), 0u);
    CHECK_EQ_U32(tail_calls, 0u);
    CHECK(d3d8_pushbuffer_rollovers() == 0u);
    environment_end();

    /* A position below the cursor is shifted by the wrap distance: here out of the window, so the walk goes
     * on to entry 0's fence 7 (it is above the semaphore) and then, entry 7 being empty, stops there with 7.
     * A position EQUAL to the cursor is not below it. MUTATION: no shift, or a shift at equality. */
    base = create_waiting_ring(1u, 0x10000000u);
    history_entry(0u, 7u, base + 0x100u);
    history_entry(1u, 5u, base + 0x90000u);
    stub_answer = true;
    (void)d3d8_pushbuffer_begin();
    CHECK_EQ_U32(stub_fence, 7u);
    environment_end();
    base = create_waiting_ring(1u, 0x10000000u);
    history_entry(0u, 7u, base + 0x1000u);
    history_entry(1u, 5u, base + 0x90000u);
    (void)d3d8_pushbuffer_begin();
    CHECK_EQ_U32(stub_fence, 5u);
    environment_end();

    /* An entry the GPU has passed ends the walk even when its position is outside the window. */
    base = create_waiting_ring(1u, 0u);
    history_entry(0u, 3u, base + 0xF0000u);
    history_entry(1u, 5u, base + 0xF0000u);
    (void)d3d8_pushbuffer_begin();
    CHECK_EQ_U32(stub_fence, 5u);
    environment_end();
}

static void test_the_refill_tail_runs_last_and_the_previews_see_its_packet(void)
{
    uint32_t base = create_gpu_ring();
    /* The tail runs once per refill, after the limit and the cursor are final. */
    refill_at(base + 0x1000u, base + 0x1000u);
    CHECK_EQ_U32(tail_calls, 1u);
    CHECK_EQ_U32(tail_limit, base + 0x1000u + 0x10000u - 0x204u);
    CHECK_EQ_U32(tail_cursor, base + 0x1000u);
    /* No refill, no tail. */
    (void)d3d8_pushbuffer_begin();
    CHECK_EQ_U32(tail_calls, 1u);
    environment_end();

    /* The previews put the cursor where the fence packet will leave it: 0x20 bytes on, unless flag 0x800
     * skips the packet. They write nothing. */
    for (unsigned flag = 0u; flag < 2u; flag++) {
        base = create_gpu_ring();
        place_cursor(base + 0x1000u);
        store(DEV + 8u, load(DEV + 8u) | (flag != 0u ? D3D8_PUSHBUFFER_FLAG_REFILL_WITHOUT_FENCE : 0u));
        d3d8_pushbuffer_set_put(base + 0x1000u);
        uint32_t cursor = base + 0x1000u;
        uint32_t limit = base + 0x1000u;
        CHECK(d3d8_pushbuffer_preview_begin(&cursor, &limit));
        CHECK_EQ_U32(cursor, base + 0x1000u + (flag != 0u ? 0u : D3D8_PUSHBUFFER_FENCE_PACKET_BYTES));
        CHECK_EQ_U32(limit, base + 0x1000u + 0x10000u - 0x204u);
        cursor = base + 0x1000u;
        limit = base + 0x1000u;
        CHECK(d3d8_pushbuffer_preview_reserve(0x4000u, &cursor, &limit));
        CHECK_EQ_U32(cursor, base + 0x1000u + (flag != 0u ? 0u : D3D8_PUSHBUFFER_FENCE_PACKET_BYTES));
        CHECK_EQ_U32(load(DEV + 0x5Cu), 0u);
        CHECK_EQ_U32(tail_calls, 0u);
        /* The simulation puts the commands after the fence packet too, for a site and for a sized reservation, and
         * returns where the site starts. */
        d3d8_pushbuffer_sim sim = d3d8_pushbuffer_sim_start();
        const uint32_t fence_bytes = flag != 0u ? 0u : D3D8_PUSHBUFFER_FENCE_PACKET_BYTES;
        CHECK_EQ_U32(d3d8_pushbuffer_sim_site(&sim, 0x1234u, 8u), base + 0x1000u + fence_bytes);
        CHECK_EQ_U32(sim.cursor, base + 0x1000u + fence_bytes + 8u);
        sim = d3d8_pushbuffer_sim_start();
        CHECK(d3d8_pushbuffer_sim_reserve(&sim, 0x4000u));
        CHECK_EQ_U32(sim.cursor, base + 0x1000u + fence_bytes);
        CHECK_EQ_U32(sim.refills, 1u);
        CHECK(!d3d8_pushbuffer_sim_reserve(&sim, 4u)); /* far from the new limit: nothing to do */
        CHECK_EQ_U32(sim.refills, 1u);
        environment_end();
    }
}

/* T526: a plan carries what a refill leaves to the refills after it. The tail here is a stub, which the previews
 * model as the GPU module's (a fence packet of 0x20 bytes at the new cursor, a history entry, the counter plus 2, the
 * put after the packet); tests/test_d3d8_vertex_constants_refill_oracle.py measures the same arithmetic against the
 * original, over a real GPU module, for two and more refills of one packet. */
static void test_a_plan_carries_what_a_refill_leaves(void)
{
    uint32_t base = create_gpu_ring();
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x400u);
    store(DEV + 0x2Cu, 11u); /* fence counter */
    store(DEV + 0x34u, 3u);  /* history cursor */
    store(DEV + 0x38u, 7u);  /* history mask */
    store(DEV + 0x40u, 5u);  /* wrap count */
    place_cursor(base + 0x1000u);
    d3d8_pushbuffer_set_put(base + 0x900u);
    d3d8_pushbuffer_sim sim = d3d8_pushbuffer_sim_start();
    d3d8_pushbuffer_split plan;
    /* the entry roll-over, then one split: two refills, each inserting a fence */
    d3d8_pushbuffer_sim_split_site(&sim, 0x1234u, 8u, 68u, 9u, 0u, &plan);
    CHECK(plan.entered);CHECK(plan.split);CHECK_EQ_U32(sim.refills, 2u);
    CHECK_EQ_U32(plan.run[0].begin, base + 0x1000u + 0x20u);
    CHECK_EQ_U32(plan.run[0].fence_bytes, 0x20u);
    CHECK_EQ_U32(plan.run[1].begin, plan.run[0].begin + plan.run[0].bytes + 0x20u);
    CHECK_EQ_U32(sim.state.fence_counter, 15u);
    CHECK_EQ_U32(sim.state.history_cursor, 5u);
    CHECK_EQ_U32(sim.state.fences, 2u);
    CHECK_EQ_U32(sim.state.history[0].index, 4u);
    CHECK_EQ_U32(sim.state.history[0].fence, 11u);
    CHECK_EQ_U32(sim.state.history[0].position, base + 0x1000u);
    CHECK_EQ_U32(sim.state.history[1].index, 5u);
    CHECK_EQ_U32(sim.state.history[1].fence, 13u);
    CHECK_EQ_U32(sim.state.history[1].position, plan.run[0].begin + plan.run[0].bytes);
    CHECK_EQ_U32(sim.state.put, plan.run[1].begin);
    CHECK_EQ_U32(sim.state.wrap_count, 5u);
    /* nothing real moved */
    CHECK_EQ_U32(load(DEV + 0x2Cu), 11u);CHECK_EQ_U32(load(DEV + 0x34u), 3u);
    CHECK_EQ_U32(d3d8_pushbuffer_put(), base + 0x900u);CHECK_EQ_U32(tail_calls, 0u);
    environment_end();

    /* With flag 0x800 the refill inserts no fence: no packet, no entry, the counter stays, the put is the cursor. */
    base = create_gpu_ring();
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x400u);
    store(DEV + 0x2Cu, 11u);
    store(DEV + 8u, load(DEV + 8u) | D3D8_PUSHBUFFER_FLAG_REFILL_WITHOUT_FENCE);
    place_cursor(base + 0x1000u);
    sim = d3d8_pushbuffer_sim_start();
    d3d8_pushbuffer_sim_split_site(&sim, 0x1234u, 8u, 68u, 9u, 0u, &plan);
    CHECK(plan.entered);CHECK(plan.split);CHECK_EQ_U32(plan.run[0].begin, base + 0x1000u);
    CHECK_EQ_U32(plan.run[0].fence_bytes, 0u);
    CHECK_EQ_U32(sim.state.fence_counter, 11u);CHECK_EQ_U32(sim.state.fences, 0u);
    CHECK_EQ_U32(sim.state.put, plan.run[1].begin);
    environment_end();

    /* The second refill reads the GET the first one's kick left. The entry roll-over wraps (the cursor is 0x100 from
     * the ring's end), GET was in the area the SECOND refill reuses and is the first fence packet's end by then. */
    base = create_gpu_ring();
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x400u);
    d3d8_pushbuffer_set_fence_reached(stub_reached);
    stub_calls = 0u;
    stub_answer = true;
    place_cursor(base + RING_BYTES - 0x100u);
    d3d8_pushbuffer_set_put(base + 0x500u);
    sim = d3d8_pushbuffer_sim_start();
    d3d8_pushbuffer_sim_split_site(&sim, 0x1234u, 8u, 68u, 9u, 0u, &plan);
    CHECK(plan.entered);CHECK(plan.split);CHECK_EQ_U32(sim.refills, 2u);
    CHECK_EQ_U32(plan.run[0].begin, base + 0x20u);
    CHECK_EQ_U32(sim.state.wrap_count, 1u);
    CHECK_EQ_U32(sim.state.wrap_delta, RING_BYTES - 0x100u);
    /* the first fence packet is at the ring's start (where the wrap restarted), not where the refill was called */
    CHECK_EQ_U32(sim.state.fences, 2u);
    CHECK_EQ_U32(sim.state.history[0].position, base);
    CHECK_EQ_U32(stub_calls, 0u); /* a GET still at base + 0x500 would be inside the second refill's area and wait */
    environment_end();

    /* What the latest refill writes outside the commands (T525: callers keep their inputs clear of it): the jump word
     * where a wrap was called, the fence packet where the segment restarted. */
    base = create_gpu_ring();
    place_cursor(base + RING_BYTES - 0x100u);
    d3d8_pushbuffer_set_put(base + 0x50000u); /* outside the area the wrap reuses: no wait */
    sim = d3d8_pushbuffer_sim_start();
    (void)d3d8_pushbuffer_sim_site(&sim, 0x1234u, 8u);
    CHECK_EQ_U32(sim.last_refill.at, base + RING_BYTES - 0x100u);
    CHECK(sim.last_refill.wraps);
    CHECK_EQ_U32(sim.last_refill.fence_begin, base);
    CHECK_EQ_U32(sim.last_refill.fence_bytes, 0x20u);
    environment_end();
    base = create_gpu_ring();
    place_cursor(base + 0x1000u);
    d3d8_pushbuffer_set_put(base + 0x800u);
    sim = d3d8_pushbuffer_sim_start();
    (void)d3d8_pushbuffer_sim_site(&sim, 0x1234u, 8u);
    CHECK_EQ_U32(sim.last_refill.at, base + 0x1000u);
    CHECK(!sim.last_refill.wraps);
    CHECK_EQ_U32(sim.last_refill.fence_begin, base + 0x1000u);
    environment_end();

    /* A refill whose fence packet would itself refill (a segment shorter than the slack) is refused by name. */
    base = create_gpu_ring();
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x100u);
    place_cursor(base + 0x1000u);
    RUN_EXPECTING_FATAL((void)d3d8_pushbuffer_begin());
    CHECK(fatal_seen);CHECK_EQ_U32(fatal_address, 0x003D67B0u);
    CHECK_EQ_U32(load(DEV), base + 0x1000u);
    environment_end();
}

/* T526: the wait of a refill walks the history the plan's earlier refills added (their fences are not in guest memory
 * yet), asks the completion test with the counter and the semaphore they leave, and a history index that comes round
 * again holds the newest entry. Each refill but the last is forced (the simulated cursor set to its limit) and the
 * last one is made to wait by putting GET inside the area it reuses, as a plan whose GET is carried never does by
 * itself. */
static void force_refill_with_get_in_its_area(d3d8_pushbuffer_sim *sim, uint32_t get_offset)
{
    sim->limit = sim->cursor;
    sim->state.put = sim->cursor + get_offset;
}

static void test_a_refill_waits_on_the_fences_the_plan_inserted(void)
{
    uint32_t base = create_waiting_ring(0u, 0u);
    store(DEV + 0x2Cu, 20u); /* the fence counter */
    history_entry(0u, 19u, base + 0x300u);
    stub_answer = true;
    place_cursor(base + 0x1000u);
    d3d8_pushbuffer_set_put(base + 0x800u);
    d3d8_pushbuffer_sim sim = d3d8_pushbuffer_sim_start();
    (void)d3d8_pushbuffer_sim_site(&sim, 0x1234u, 8u); /* the entry roll-over: fence 20 at index 1, no wait */
    CHECK_EQ_U32(stub_calls, 0u);
    CHECK_EQ_U32(sim.state.fences, 1u);
    force_refill_with_get_in_its_area(&sim, 0x100u);
    (void)d3d8_pushbuffer_sim_site(&sim, 0x1234u, 8u); /* GET carried inside the area: waits */
    CHECK_EQ_U32(stub_calls, 1u);
    CHECK_EQ_U32(stub_fence, 20u);     /* the newest entry, which only the plan knows */
    CHECK_EQ_U32(stub_counter, 22u);   /* one fence inserted, the counter two on */
    CHECK_EQ_U32(stub_semaphore, 20u); /* and the semaphore at the fence the kick completed */
    environment_end();

    /* A history of two entries: the third refill's fence and the first one share an index, and the newest is read. */
    base = create_waiting_ring(0u, 0u);
    store(DEV + 0x38u, 1u);
    store(DEV + 0x2Cu, 20u);
    stub_answer = true;
    place_cursor(base + 0x1000u);
    d3d8_pushbuffer_set_put(base + 0x800u);
    sim = d3d8_pushbuffer_sim_start();
    (void)d3d8_pushbuffer_sim_site(&sim, 0x1234u, 8u); /* fence 20 at index 1 */
    sim.limit = sim.cursor;
    (void)d3d8_pushbuffer_sim_site(&sim, 0x1234u, 8u); /* fence 22 at index 0 */
    sim.limit = sim.cursor;
    (void)d3d8_pushbuffer_sim_site(&sim, 0x1234u, 8u); /* fence 24 at index 1 again */
    CHECK_EQ_U32(sim.state.fences, 3u);
    CHECK_EQ_U32(stub_calls, 0u);
    force_refill_with_get_in_its_area(&sim, 0x100u);
    (void)d3d8_pushbuffer_sim_site(&sim, 0x1234u, 8u);
    CHECK_EQ_U32(stub_calls, 1u);
    CHECK_EQ_U32(stub_fence, 24u);
    CHECK_EQ_U32(stub_counter, 26u);
    CHECK_EQ_U32(stub_semaphore, 24u);
    environment_end();

    /* Without the GPU module's tail a refill is the arithmetic alone: no kick, no fence, nothing carried but the
     * wrap. */
    base = create_ring();
    store(DEV + 0x2Cu, 20u);
    d3d8_pushbuffer_set_put(base + 0x800u);
    place_cursor(base + RING_BYTES - 0x100u);
    sim = d3d8_pushbuffer_sim_start();
    (void)d3d8_pushbuffer_sim_site(&sim, 0x1234u, 8u);
    CHECK_EQ_U32(sim.refills, 1u);
    CHECK_EQ_U32(sim.state.fences, 0u);
    CHECK_EQ_U32(sim.state.fence_counter, 20u);
    CHECK_EQ_U32(sim.state.put, base + 0x800u);
    CHECK(!sim.state.semaphore_known);
    CHECK_EQ_U32(sim.state.wrap_count, 1u);
    CHECK_EQ_U32(sim.cursor, base + 8u); /* no fence packet */
    environment_end();
}

/* T546: a plan lists every span it writes (the sites, the helper written under a reservation, each refill's jump word and
 * fence packet), merging the ones that touch, and carries the device flags its refills see: a refill under flag 0x800
 * inserts no fence packet and leaves flag 0x1000, which the indexed draw answers with a fence at its end. */
static void test_a_plan_records_its_spans_and_carries_its_flags(void)
{
    uint32_t base = create_gpu_ring();
    place_cursor(base + 0x1000u);
    store(DEV + 4u, base + 0x1000u + 0x8000u);
    d3d8_pushbuffer_set_put(base + 0x1000u);
    d3d8_pushbuffer_sim sim = d3d8_pushbuffer_sim_start();
    CHECK_EQ_U32(d3d8_pushbuffer_sim_site(&sim, 0x1234u, 8u), base + 0x1000u);
    CHECK_EQ_U32(d3d8_pushbuffer_sim_write(&sim, 0x1234u, 12u), base + 0x1008u);
    CHECK_EQ_U32(sim.cursor, base + 0x1014u);
    CHECK_EQ_U32(sim.bytes, 20u);
    CHECK_EQ_U32(sim.span_count, 1u);
    CHECK_EQ_U32(sim.span[0].begin, base + 0x1000u);
    CHECK_EQ_U32(sim.span[0].bytes, 20u);
    /* The next site's check refills (the limit is at the cursor): its fence packet touches the span before it and the site
     * after it, so all three are one span. */
    sim.limit = sim.cursor;
    CHECK_EQ_U32(d3d8_pushbuffer_sim_site(&sim, 0x1234u, 8u), base + 0x1014u + D3D8_PUSHBUFFER_FENCE_PACKET_BYTES);
    CHECK_EQ_U32(sim.refills, 1u);
    CHECK_EQ_U32(sim.span_count, 1u);
    CHECK_EQ_U32(sim.span[0].bytes, 20u + D3D8_PUSHBUFFER_FENCE_PACKET_BYTES + 8u);
    /* The flags are the device's until the plan is given its own. */
    CHECK_EQ_U32(d3d8_pushbuffer_sim_flags(&sim), load(DEV + 8u));
    environment_end();

    /* A refill that wraps writes its jump word at the old cursor and the fence packet at the base: separate spans. */
    base = create_gpu_ring();
    const uint32_t end = load(DEV + 0x28u);
    place_cursor(end - 0x100u);
    d3d8_pushbuffer_set_put(end - 0x100u); /* the GPU is caught up: the wrap waits for nothing */
    sim = d3d8_pushbuffer_sim_start();
    CHECK_EQ_U32(d3d8_pushbuffer_sim_site(&sim, 0x1234u, 8u), base + D3D8_PUSHBUFFER_FENCE_PACKET_BYTES);
    CHECK_EQ_U32(sim.span_count, 2u);
    CHECK_EQ_U32(sim.span[0].begin, end - 0x100u);
    CHECK_EQ_U32(sim.span[0].bytes, 4u);
    CHECK_EQ_U32(sim.span[1].begin, base);
    CHECK_EQ_U32(sim.span[1].bytes, D3D8_PUSHBUFFER_FENCE_PACKET_BYTES + 8u);
    environment_end();

    /* Under flag 0x800 the refill writes no fence packet and leaves flag 0x1000 in the plan's flags (and not in the device). */
    base = create_gpu_ring();
    place_cursor(base + 0x1000u);
    d3d8_pushbuffer_set_put(base + 0x1000u);
    sim = d3d8_pushbuffer_sim_start();
    d3d8_pushbuffer_sim_set_flags(&sim, load(DEV + 8u) | D3D8_PUSHBUFFER_FLAG_REFILL_WITHOUT_FENCE);
    CHECK_EQ_U32(d3d8_pushbuffer_sim_site(&sim, 0x1234u, 8u), base + 0x1000u);
    CHECK_EQ_U32(sim.refills, 1u);
    CHECK_EQ_U32(sim.span_count, 1u);
    CHECK_EQ_U32(sim.span[0].bytes, 8u);
    CHECK((d3d8_pushbuffer_sim_flags(&sim) & 0x1000u) != 0u);
    CHECK((d3d8_pushbuffer_sim_flags(&sim) & D3D8_PUSHBUFFER_FLAG_REFILL_WITHOUT_FENCE) != 0u);
    CHECK((load(DEV + 8u) & 0x1000u) == 0u);
    /* A plan that never refills leaves the flags as given. */
    sim = d3d8_pushbuffer_sim_start();
    store(DEV + 4u, base + 0x1000u + 0x8000u);
    sim.limit = base + 0x1000u + 0x8000u;
    d3d8_pushbuffer_sim_set_flags(&sim, 0x800u);
    (void)d3d8_pushbuffer_sim_site(&sim, 0x1234u, 8u);
    CHECK_EQ_U32(sim.refills, 0u);
    CHECK_EQ_U32(d3d8_pushbuffer_sim_flags(&sim), 0x800u);
    environment_end();
}

int main(void)
{
    test_creation_derives_the_fields();
    test_primitive_writes_pairs_and_advances();
    test_begin_and_end();
    test_plain_rollover_moves_the_limit();
    test_rollover_near_the_end_clamps_or_wraps();
    test_use_before_creation_is_fatal();
    test_allocation_failure_is_reported();
    test_previews_predict_the_real_rollover();
    test_sized_reservation_arithmetic();
    test_refills_the_model_cannot_do_refuse();
    test_simulated_sites_advance_and_refuse_unmapped_spans();
    test_split_sites_refill_between_two_units();
    test_checked_sites_refill_until_the_packet_fits();
    test_refill_at_publishes_the_cursor_and_rolls_over();
    test_the_refill_records_get_and_refuses_what_it_cannot_model();
    test_the_refill_wait_names_a_fence_and_refuses_one_not_passed();
    test_the_refill_tail_runs_last_and_the_previews_see_its_packet();
    test_a_plan_carries_what_a_refill_leaves();
    test_a_refill_waits_on_the_fences_the_plan_inserted();
    test_a_plan_records_its_spans_and_carries_its_flags();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
