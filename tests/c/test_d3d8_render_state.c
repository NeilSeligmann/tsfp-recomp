/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The render-state dispatch and the table it walks.
 *
 * DELIBERATELY FREE OF LIFTED CODE, THE XBE AND ANY DISC. Every number asserted here was
 * measured once by `tools/d3dscan/rstable.py` from a user-supplied image and then written
 * down; nothing in this file reads a binary, so a fresh clone with no `generated/` and no
 * disc runs the whole suite. That is the point: the table is the deliverable, and a suite
 * that needed the image to check it would only ever be run by whoever already had one.
 *
 * WHAT IT IS ACTUALLY DEFENDING AGAINST. `docs/d3d8-usage.md` §2.1 records an NV2A method
 * table that was wrong in 46 of 102 checkable entries, off by ONE SLOT, so every name was a
 * real method name on the wrong number and nothing about it looked wrong. A suite that
 * spot-checks a few rows does not catch that -- a slot shift moves ALL of them and any
 * individual row still looks like a plausible method. So the structural checks here matter
 * more than the spot checks:
 *
 *   - `test_the_array_geometry_pins_the_slot_alignment` re-derives the table's +4 run
 *     decomposition inside the test, the way `tests/c/test_gpu_png.c` re-implements CRC-32
 *     rather than calling the implementation twice, and asserts the run lengths. A shift of
 *     one slot changes them.
 *   - `test_every_immediate_method_is_a_well_formed_pgraph_method` rejects a method that is
 *     not 4-aligned or is outside pgraph's window, which is what a byte-swapped number
 *     becomes.
 *   - `test_the_dirty_masks_have_the_measured_histogram` checks the multiset of masks, not
 *     one mask, so moving a bit from one state to another is visible even though the two
 *     states individually still hold plausible values.
 *
 * NON-EMPTY BEFORE EQUAL. Several assertions compare counted sets. Each one asserts the
 * count is non-zero first, because two empty sets are equal and an empty-set bug reads as a
 * pass.
 */

#include "d3d8_render_state.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        checks++;                                                                          \
        if (!(cond)) {                                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                         \
            failures++;                                                                    \
        }                                                                                  \
    } while (0)

#define CHECK_EQ(actual, expected)                                                         \
    do {                                                                                   \
        checks++;                                                                          \
        unsigned long long a_ = (unsigned long long)(actual);                               \
        unsigned long long e_ = (unsigned long long)(expected);                             \
        if (a_ != e_) {                                                                     \
            printf("FAIL %s:%d  %s: got %llu (0x%llX), want %llu (0x%llX)\n", __FILE__,     \
                   __LINE__, #actual, a_, a_, e_, e_);                                      \
            failures++;                                                                    \
        }                                                                                  \
    } while (0)

/* ------------------------------------------------------------------ an emit sink */

#define SINK_MAX 64

typedef struct {
    uint32_t headers[SINK_MAX];
    uint32_t values[SINK_MAX];
    size_t count;
} sink;

static void sink_emit(void *context, uint32_t header, uint32_t value)
{
    sink *s = (sink *)context;
    if (s->count < SINK_MAX) {
        s->headers[s->count] = header;
        s->values[s->count] = value;
    }
    s->count++;
}

/* ------------------------------------------------------------------ the bounds */

static void test_the_three_class_bounds_are_the_measured_ones(void)
{
    /* MEASURED twice from the image, from two separately compiled copies of the dispatch:
     * the game's inlined one and D3D8's own out-of-line one. Both carry `cmp index, 0x5C`
     * and `cmp index, 0x88`. The third bound is not a compare at all -- neither copy has
     * one -- it is where the handler pointer table stops holding function entry points. */
    CHECK_EQ(d3d8_rs_immediate_bound, 0x5Cu);
    CHECK_EQ(d3d8_rs_deferred_bound, 0x88u);
    CHECK_EQ(d3d8_rs_handler_bound, 0xA6u);
    CHECK_EQ(D3D8_RS_TABLE_ROWS, d3d8_rs_handler_bound);

    /* 0x5C is 92 decimal, which is also the number of header entries. That the two agree is
     * the whole reason the immediate class can be driven by a table, so pin it. */
    CHECK_EQ(d3d8_rs_immediate_bound, 92u);
    CHECK_EQ(d3d8_rs_deferred_bound - d3d8_rs_immediate_bound, 44u);
    CHECK_EQ(d3d8_rs_handler_bound - d3d8_rs_deferred_bound, 30u);
}

static void test_classify_switches_class_exactly_at_each_bound(void)
{
    /* THE OFF-BY-ONE EDGE. Each bound is checked from both sides, because an off-by-one in
     * a `<` turns one of these and not the other. */
    CHECK_EQ(d3d8_rs_classify(0), D3D8_RS_OK_IMMEDIATE);
    CHECK_EQ(d3d8_rs_classify(0x5B), D3D8_RS_OK_IMMEDIATE);
    CHECK_EQ(d3d8_rs_classify(0x5C), D3D8_RS_OK_DEFERRED);
    CHECK_EQ(d3d8_rs_classify(0x87), D3D8_RS_OK_DEFERRED);
    CHECK_EQ(d3d8_rs_classify(0x88), D3D8_RS_UNIMPLEMENTED_HANDLER);
    CHECK_EQ(d3d8_rs_classify(0xA5), D3D8_RS_UNIMPLEMENTED_HANDLER);
    CHECK_EQ(d3d8_rs_classify(0xA6), D3D8_RS_OUT_OF_RANGE);
    CHECK_EQ(d3d8_rs_classify(0x7FFFFFFF), D3D8_RS_OUT_OF_RANGE);

    /* The guest's compare is SIGNED, so a negative state reaches the immediate class and
     * indexes the header table backwards. Reported separately rather than merged into
     * OUT_OF_RANGE so the divergence from the original is visible in the API. */
    CHECK_EQ(d3d8_rs_classify(-1), D3D8_RS_NEGATIVE);
    CHECK_EQ(d3d8_rs_classify(-0x5C), D3D8_RS_NEGATIVE);
    CHECK(d3d8_rs_row(-1) == NULL);
    CHECK(d3d8_rs_row(0xA6) == NULL);
    CHECK(d3d8_rs_row(0) != NULL);
    CHECK(d3d8_rs_row(0xA5) != NULL);
}

/* ------------------------------------------------------------------ the table's shape */

static void test_every_row_class_matches_its_index_and_every_index_is_reachable(void)
{
    /* BOTH DIRECTIONS. Checking only "each row's kind matches its index" passes on a table
     * where a whole class is missing; checking only the per-class counts passes on a table
     * where two rows swapped classes. So both, and the counts are asserted non-zero before
     * being compared. */
    size_t immediate = 0;
    size_t deferred = 0;
    size_t handler = 0;
    for (uint32_t index = 0; index < D3D8_RS_TABLE_ROWS; index++) {
        const d3d8_render_state_row *row = &d3d8_render_state_rows[index];
        uint8_t want;
        if (index < d3d8_rs_immediate_bound) {
            want = D3D8_RS_IMMEDIATE;
            immediate++;
        } else if (index < d3d8_rs_deferred_bound) {
            want = D3D8_RS_DEFERRED;
            deferred++;
        } else {
            want = D3D8_RS_HANDLER;
            handler++;
        }
        CHECK_EQ(row->kind, want);

        /* A row only carries the field its class uses. The other must be zero, or a
         * regeneration that put a mask on an immediate row would go unnoticed. */
        if (want == D3D8_RS_IMMEDIATE) {
            CHECK_EQ(row->dirty_bit, 0u);
            CHECK(row->method != 0);
        } else if (want == D3D8_RS_DEFERRED) {
            CHECK_EQ(row->method, 0u);
        } else {
            CHECK_EQ(row->method, 0u);
            CHECK_EQ(row->dirty_bit, 0u);
        }
    }
    CHECK(immediate > 0 && deferred > 0 && handler > 0);
    CHECK_EQ(immediate, 92u);
    CHECK_EQ(deferred, 44u);
    CHECK_EQ(handler, 30u);
}

static void test_every_immediate_method_is_a_well_formed_pgraph_method(void)
{
    /* A method is a dword offset into pgraph's 0x2000-wide register window. A byte-swapped
     * or otherwise corrupted number fails one of these: 0x0260 byte-swapped is 0x6002,
     * which is both outside the window and not 4-aligned. */
    size_t seen = 0;
    for (uint32_t index = 0; index < d3d8_rs_immediate_bound; index++) {
        uint16_t method = d3d8_render_state_rows[index].method;
        CHECK((method & 0x3u) == 0u);
        CHECK(method >= 0x0100u);
        CHECK(method <= 0x1FFCu);
        seen++;
    }
    CHECK(seen > 0);
    CHECK_EQ(seen, 92u);
}

static void test_the_array_geometry_pins_the_slot_alignment(void)
{
    /* RE-DERIVED HERE, not read from the implementation. Decompose the immediate methods
     * into maximal runs that step by exactly +4, which is what an auto-incrementing
     * hardware array looks like, and assert the lengths.
     *
     * This is the check a spot test cannot make. Shifting the table by ONE SLOT -- the exact
     * defect that got 46 of 102 entries wrong elsewhere in this project -- leaves every
     * individual method plausible but moves every run boundary. */
    uint32_t lengths[D3D8_RS_TABLE_ROWS];
    size_t runs = 0;
    size_t total = 0;
    uint32_t length = 1;
    for (uint32_t index = 1; index <= d3d8_rs_immediate_bound; index++) {
        int continues = index < d3d8_rs_immediate_bound &&
                        d3d8_render_state_rows[index].method ==
                            d3d8_render_state_rows[index - 1].method + 4u;
        if (continues) {
            length++;
            continue;
        }
        lengths[runs++] = length;
        total += length;
        length = 1;
    }
    CHECK(runs > 0);
    CHECK_EQ(total, 92u);
    CHECK_EQ(runs, 35u);

    /* The first six runs, which is where the combiner arrays live and where a slot shift is
     * most visible: eight alpha input words, two specular-fog words, then thirty-two
     * contiguous dwords spanning four more eight-wide combiner arrays. */
    CHECK_EQ(lengths[0], 8u);
    CHECK_EQ(lengths[1], 2u);
    CHECK_EQ(lengths[2], 32u);
    CHECK_EQ(lengths[3], 1u);
    CHECK_EQ(lengths[4], 2u);
    CHECK_EQ(lengths[5], 9u);

    /* The tail: the last eight immediate states all point at the SAME method, so each is its
     * own run of one. A shift would merge or split those. */
    for (size_t index = runs - 8; index < runs; index++) {
        CHECK_EQ(lengths[index], 1u);
    }
}

static void test_the_spot_checked_rows_are_the_measured_methods(void)
{
    /* Chosen at the run boundaries, where an off-by-one slot changes the value rather than
     * merely relabelling it. */
    CHECK_EQ(d3d8_render_state_rows[0x00].method, 0x0260u);
    CHECK_EQ(d3d8_render_state_rows[0x07].method, 0x027Cu);
    CHECK_EQ(d3d8_render_state_rows[0x08].method, 0x0288u); /* the +12 step */
    CHECK_EQ(d3d8_render_state_rows[0x0A].method, 0x0A60u);
    CHECK_EQ(d3d8_render_state_rows[0x29].method, 0x0ADCu);
    CHECK_EQ(d3d8_render_state_rows[0x2A].method, 0x17F8u);
    CHECK_EQ(d3d8_render_state_rows[0x35].method, 0x1E60u);
    CHECK_EQ(d3d8_render_state_rows[0x37].method, 0x1E74u);
    CHECK_EQ(d3d8_render_state_rows[0x39].method, 0x0354u);
    CHECK_EQ(d3d8_render_state_rows[0x53].method, 0x147Cu);
    CHECK_EQ(d3d8_render_state_rows[0x5B].method, 0x1D90u);

    /* 84 distinct methods over 92 rows, because the trailing nine rows share one method.
     * Counted here rather than stated, so the duplication cannot drift. */
    size_t distinct = 0;
    for (uint32_t index = 0; index < d3d8_rs_immediate_bound; index++) {
        int earlier = 0;
        for (uint32_t other = 0; other < index; other++) {
            if (d3d8_render_state_rows[other].method == d3d8_render_state_rows[index].method) {
                earlier = 1;
                break;
            }
        }
        if (!earlier) {
            distinct++;
        }
    }
    CHECK(distinct > 0);
    CHECK_EQ(distinct, 84u);
}

static void test_the_dirty_masks_have_the_measured_histogram(void)
{
    /* A MULTISET, not a per-index check. Moving one bit from state A to state B leaves both
     * individually plausible and changes this. */
    static const struct {
        uint16_t mask;
        uint32_t count;
    } want[] = {
        { 0x0000, 12 }, { 0x000F, 4 }, { 0x0100, 7 }, { 0x0900, 1 },
        { 0x1000, 12 }, { 0x1200, 1 }, { 0x2000, 6 }, { 0x3000, 1 },
    };
    size_t total = 0;
    for (size_t slot = 0; slot < sizeof(want) / sizeof(want[0]); slot++) {
        uint32_t seen = 0;
        for (uint32_t index = d3d8_rs_immediate_bound; index < d3d8_rs_deferred_bound;
             index++) {
            if (d3d8_render_state_rows[index].dirty_bit == want[slot].mask) {
                seen++;
            }
        }
        CHECK_EQ(seen, want[slot].count);
        total += seen;
    }
    CHECK(total > 0);
    CHECK_EQ(total, 44u); /* the histogram accounts for every deferred row */

    /* The twelve zero masks are MEASURED, not missing: states 0x7C..0x87 store the shadow
     * and mark nothing dirty. Asserting they are contiguous and where they are stops a
     * regeneration that merely lost some masks from passing the histogram above. */
    for (uint32_t index = 0x7C; index < 0x88; index++) {
        CHECK_EQ(d3d8_render_state_rows[index].dirty_bit, 0u);
    }
    CHECK(d3d8_render_state_rows[0x7B].dirty_bit != 0u);
    CHECK_EQ(d3d8_render_state_rows[0x5C].dirty_bit, 0x2000u);
}

static void test_name_provenance_is_carried_in_the_table(void)
{
    /* Every immediate row is named; the two that rest on a SINGLE reference are marked as
     * such rather than quietly included, and this asserts WHICH two, by index, because a
     * count alone would survive the marking moving to a different row. */
    size_t named = 0;
    size_t single = 0;
    size_t disputed = 0;
    for (uint32_t index = 0; index < d3d8_rs_immediate_bound; index++) {
        const d3d8_render_state_row *row = &d3d8_render_state_rows[index];
        CHECK(row->base_name != NULL);
        CHECK(row->name_sources >= 1);
        named++;
        if (row->name_sources == 1) {
            single++;
        }
        if (row->name_disputed) {
            disputed++;
        }
    }
    CHECK(named > 0);
    CHECK_EQ(named, 92u);
    CHECK_EQ(single, 2u);
    CHECK_EQ(d3d8_render_state_rows[0x4C].name_sources, 1u);
    CHECK_EQ(d3d8_render_state_rows[0x53].name_sources, 1u);
    CHECK_EQ(disputed, 1u);
    CHECK_EQ(d3d8_render_state_rows[0x42].name_disputed, 1u);

    /* A name reached N dwords past its base is weaker evidence than one read exactly, and
     * the reach is bounded at 7 because the widest array here is eight dwords. 0x4C is the
     * one row that is BOTH single-source and reached, which is exactly why it is marked. */
    for (uint32_t index = 0; index < d3d8_rs_immediate_bound; index++) {
        CHECK(d3d8_render_state_rows[index].name_element <= 7u);
    }
    CHECK_EQ(d3d8_render_state_rows[0x4C].name_element, 6u);
    CHECK_EQ(d3d8_render_state_rows[0x00].name_element, 0u);
    CHECK_EQ(d3d8_render_state_rows[0x03].name_element, 3u);

    /* Deferred and handler rows name nothing, because there is no method to name. */
    for (uint32_t index = d3d8_rs_immediate_bound; index < D3D8_RS_TABLE_ROWS; index++) {
        CHECK(d3d8_render_state_rows[index].base_name == NULL);
        CHECK_EQ(d3d8_render_state_rows[index].name_sources, 0u);
    }
}

/* ------------------------------------------------------------------ the header encoding */

static void test_the_header_encoding_reproduces_the_measured_dwords(void)
{
    /* The table holds bare methods; these are the raw dwords actually present in the image
     * at those slots, so the macro is checked against measurement rather than against
     * itself. */
    CHECK_EQ(D3D8_NV2A_HEADER(d3d8_render_state_rows[0x00].method), 0x00040260u);
    CHECK_EQ(D3D8_NV2A_HEADER(d3d8_render_state_rows[0x53].method), 0x0004147Cu);
    CHECK_EQ(D3D8_NV2A_HEADER(d3d8_render_state_rows[0x5B].method), 0x00041D90u);

    /* And the fields are where the encoding says: one parameter, subchannel 0,
     * auto-incrementing, reserved bits clear. */
    for (uint32_t index = 0; index < d3d8_rs_immediate_bound; index++) {
        uint32_t header = D3D8_NV2A_HEADER(d3d8_render_state_rows[index].method);
        CHECK_EQ((header >> 18) & 0x7FFu, 1u);          /* count */
        CHECK_EQ((header >> 13) & 0x7u, 0u);            /* subchannel */
        CHECK_EQ((header >> 16) & 0x3u, 0u);            /* reserved */
        CHECK_EQ(header & (1u << 30), 0u);              /* auto-incrementing */
        CHECK_EQ(header & 0x1FFFu, d3d8_render_state_rows[index].method);
    }
}

/* ------------------------------------------------------------------ the dispatch */

static void test_an_immediate_state_emits_once_and_marks_nothing_dirty(void)
{
    sink out;
    d3d8_render_state rs;
    memset(&out, 0, sizeof(out));
    d3d8_render_state_init(&rs, sink_emit, &out);

    CHECK_EQ(d3d8_set_render_state(&rs, 0x08, 0xDEADBEEFu), D3D8_RS_OK_IMMEDIATE);
    CHECK_EQ(out.count, 1u);
    CHECK_EQ(out.headers[0], 0x00040288u);
    CHECK_EQ(out.values[0], 0xDEADBEEFu);
    CHECK_EQ(rs.dirty, 0u);
    CHECK_EQ(rs.shadow[0x08], 0xDEADBEEFu);
    CHECK_EQ(rs.rejected, 0u);
}

static void test_a_deferred_state_ors_its_bit_and_emits_nothing(void)
{
    sink out;
    d3d8_render_state rs;
    memset(&out, 0, sizeof(out));
    d3d8_render_state_init(&rs, sink_emit, &out);

    CHECK_EQ(d3d8_set_render_state(&rs, 0x5C, 0x11u), D3D8_RS_OK_DEFERRED);
    CHECK_EQ(out.count, 0u);
    CHECK_EQ(rs.dirty, 0x2000u);
    CHECK_EQ(rs.shadow[0x5C], 0x11u);

    /* Dirty bits accumulate, and a state whose mask is zero must leave the word alone
     * without being mistaken for a failure. */
    CHECK_EQ(d3d8_set_render_state(&rs, 0x6A, 0x22u), D3D8_RS_OK_DEFERRED);
    CHECK(rs.dirty != 0x2000u);
    uint32_t after_two = rs.dirty;
    CHECK_EQ(d3d8_set_render_state(&rs, 0x7C, 0x33u), D3D8_RS_OK_DEFERRED);
    CHECK_EQ(rs.dirty, after_two);
    CHECK_EQ(rs.shadow[0x7C], 0x33u);
    CHECK_EQ(out.count, 0u);
}

static void test_the_inlined_copy_diverges_from_the_library_on_the_shadow(void)
{
    /* MEASURED divergence between the two compiled copies: D3D8's out-of-line
     * SetRenderState stores the shadow on the immediate path, the copy MSVC inlined into the
     * game's code does not. Collapsing them would make a later read of the shadow return
     * the right value where the original returned a stale one. */
    sink out;
    d3d8_render_state rs;
    memset(&out, 0, sizeof(out));
    d3d8_render_state_init(&rs, sink_emit, &out);

    CHECK_EQ(d3d8_set_render_state_inlined(&rs, 0x08, 0xA5A5A5A5u), D3D8_RS_OK_IMMEDIATE);
    CHECK_EQ(out.count, 1u);
    CHECK_EQ(out.headers[0], 0x00040288u);
    CHECK_EQ(rs.shadow[0x08], 0u); /* NOT stored -- this is the divergence */

    /* On the deferred path the two agree: both store the shadow. */
    CHECK_EQ(d3d8_set_render_state_inlined(&rs, 0x5C, 0x77u), D3D8_RS_OK_DEFERRED);
    CHECK_EQ(rs.shadow[0x5C], 0x77u);
}

static void test_a_handler_state_is_reported_and_never_silently_dropped(void)
{
    sink out;
    d3d8_render_state rs;
    memset(&out, 0, sizeof(out));
    d3d8_render_state_init(&rs, sink_emit, &out);

    CHECK_EQ(d3d8_set_render_state(&rs, 0x88, 1u), D3D8_RS_UNIMPLEMENTED_HANDLER);
    CHECK_EQ(d3d8_set_render_state(&rs, 0xA5, 2u), D3D8_RS_UNIMPLEMENTED_HANDLER);
    CHECK_EQ(rs.unimplemented_handlers, 2u);
    CHECK_EQ(out.count, 0u);
    CHECK_EQ(rs.dirty, 0u);
    CHECK_EQ(rs.rejected, 0u);
}

static void test_an_out_of_range_state_is_rejected_without_touching_anything(void)
{
    sink out;
    d3d8_render_state rs;
    memset(&out, 0, sizeof(out));
    d3d8_render_state_init(&rs, sink_emit, &out);

    CHECK_EQ(d3d8_set_render_state(&rs, 0xA6, 1u), D3D8_RS_OUT_OF_RANGE);
    CHECK_EQ(d3d8_set_render_state(&rs, -1, 1u), D3D8_RS_NEGATIVE);
    CHECK_EQ(rs.rejected, 2u);
    CHECK_EQ(out.count, 0u);
    CHECK_EQ(rs.dirty, 0u);
    for (uint32_t index = 0; index < D3D8_RS_TABLE_ROWS; index++) {
        CHECK_EQ(rs.shadow[index], 0u);
    }
}

static void test_a_state_block_is_walked_at_its_own_stride(void)
{
    /* The game drives this from a table of records in its own data, and the two MEASURED
     * call sites use different record sizes over the same dispatch, so the stride is a
     * parameter rather than a constant. */
    sink out;
    d3d8_render_state rs;
    memset(&out, 0, sizeof(out));
    d3d8_render_state_init(&rs, sink_emit, &out);

    static const uint32_t packed[] = { 0x08, 0x1111u, 0x09, 0x2222u, 0x5C, 0x3333u };
    CHECK_EQ(d3d8_apply_render_state_block(&rs, packed, 3, 2), 0u);
    CHECK_EQ(out.count, 2u);
    CHECK_EQ(out.headers[0], 0x00040288u);
    CHECK_EQ(out.headers[1], 0x0004028Cu);
    CHECK_EQ(rs.dirty, 0x2000u);

    /* A wider record with two ignored dwords per entry, and one out-of-range state so the
     * rejection count is exercised rather than assumed to be zero. */
    memset(&out, 0, sizeof(out));
    d3d8_render_state_init(&rs, sink_emit, &out);
    static const uint32_t wide[] = {
        0x08, 0x4444u, 0xFFFFu, 0xFFFFu,
        0xFFFF, 0x5555u, 0xFFFFu, 0xFFFFu,
    };
    CHECK_EQ(d3d8_apply_render_state_block(&rs, wide, 2, 4), 1u);
    CHECK_EQ(out.count, 1u);
    CHECK_EQ(out.values[0], 0x4444u);

    /* A stride that cannot hold a (state, value) pair rejects the whole block rather than
     * reading past each record. */
    CHECK_EQ(d3d8_apply_render_state_block(&rs, packed, 3, 1), 3u);
    CHECK_EQ(d3d8_apply_render_state_block(&rs, NULL, 3, 2), 3u);
}

int main(void)
{
    printf("d3d8 render-state table and dispatch tests\n");

    test_the_three_class_bounds_are_the_measured_ones();
    test_classify_switches_class_exactly_at_each_bound();
    test_every_row_class_matches_its_index_and_every_index_is_reachable();
    test_every_immediate_method_is_a_well_formed_pgraph_method();
    test_the_array_geometry_pins_the_slot_alignment();
    test_the_spot_checked_rows_are_the_measured_methods();
    test_the_dirty_masks_have_the_measured_histogram();
    test_name_provenance_is_carried_in_the_table();
    test_the_header_encoding_reproduces_the_measured_dwords();
    test_an_immediate_state_emits_once_and_marks_nothing_dirty();
    test_a_deferred_state_ors_its_bit_and_emits_nothing();
    test_the_inlined_copy_diverges_from_the_library_on_the_shadow();
    test_a_handler_state_is_reported_and_never_silently_dropped();
    test_an_out_of_range_state_is_rejected_without_touching_anything();
    test_a_state_block_is_walked_at_its_own_stride();

    printf("%s: %d checks, %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", checks,
           failures);
    return failures == 0 ? 0 : 1;
}
