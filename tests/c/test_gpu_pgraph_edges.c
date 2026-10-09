/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Edges of gpu_pgraph (decoder) and the device-free half of gpu_pgraph_replay that the first
 * mutation sweep over them (tools/mutate/sets/gpu_pgraph.py, gpu_pgraph_replay.py, T259) found
 * UNPINNED: each test below kills mutants that survived test_gpu_pgraph and test_gpu_pgraph_replay.
 * Every group says which. Needs no Vulkan device: replays with no draw to render never touch it.
 */
#include "gpu_pgraph.h"
#include "gpu_pgraph_replay.h"
#include "gpu_pgraph_test_support.h"
#include "gpu_vsh_draw.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(condition)                                                                      \
    do {                                                                                      \
        checks++;                                                                             \
        if (!(condition)) {                                                                   \
            failures++;                                                                       \
            printf("  FAIL line %d: %s\n", __LINE__, #condition);                             \
        }                                                                                     \
    } while (0)

static gpu_pgraph_result decode_pairs(gpu_pgraph *pgraph, const gpu_pgraph_command *pairs, size_t count)
{
    return gpu_pgraph_decode(pgraph, pairs, count);
}

/* ---- the format word and the argument check ------------------------------------------------ */

static void test_format_and_arguments(void)
{
    printf("test_format_and_arguments\n");
    /* the type field is FOUR bits: a type above 7 must survive */
    const gpu_pgraph_format high = gpu_pgraph_decode_format((64u << 8) | (3u << 4) | 0xAu);
    CHECK(high.type == 0xAu && high.size == 3u && high.stride == 64u);
    const gpu_pgraph_format top = gpu_pgraph_decode_format(0xFu);
    CHECK(top.type == 0xFu && top.size == 0u && top.stride == 0u);
    /* a NULL command array with a count is an argument error, with none it is an empty stream */
    gpu_pgraph *pgraph = gpu_pgraph_create();
    CHECK(gpu_pgraph_decode(pgraph, NULL, 3u) == GPU_PGRAPH_ERR_ARGUMENT);
    CHECK(gpu_pgraph_decode(pgraph, NULL, 0u) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_decode(NULL, NULL, 0u) == GPU_PGRAPH_ERR_ARGUMENT);
    gpu_pgraph_destroy(pgraph);
}

/* ---- reset: everything it promises to forget ---------------------------------------------- */

static void test_reset_forgets(void)
{
    printf("test_reset_forgets\n");
    gpu_pgraph *pgraph = gpu_pgraph_create();
    gpu_pgraph_set_strict(pgraph, false);
    stream_builder stream = {0};
    stream_array(&stream, 3u, 0x1234u, array_format(12u, 3u, GPU_PGRAPH_TYPE_F));
    /* a program load cursor and a constant load cursor left mid-file, an unhandled method */
    stream_pair(&stream, GPU_PGRAPH_PROGRAM_LOAD, 7u);
    stream_pair(&stream, GPU_PGRAPH_CONSTANT_LOAD, 9u);
    stream_pair(&stream, 0x4000u, 1u);
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(decode_pairs(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_count(pgraph) == 1u && gpu_pgraph_unhandled_count(pgraph) == 1u);
    /* and a refusal, so an error text exists */
    const gpu_pgraph_command lone_end[] = {{GPU_PGRAPH_BEGIN_END, 0u}};
    CHECK(decode_pairs(pgraph, lone_end, 1u) == GPU_PGRAPH_ERR_MALFORMED);
    CHECK(gpu_pgraph_error(pgraph)[0] != '\0');

    gpu_pgraph_reset(pgraph);
    CHECK(gpu_pgraph_draw_count(pgraph) == 0u);
    CHECK(gpu_pgraph_unhandled_count(pgraph) == 0u);
    CHECK(gpu_pgraph_error(pgraph)[0] == '\0');
    const gpu_pgraph_stats stats = gpu_pgraph_get_stats(pgraph);
    CHECK(stats.pairs == 0u && stats.draws == 0u);
    /* the arrays are gone: a draw after the reset records no array */
    stream_builder after = {0};
    stream_draw_arrays(&after, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    /* the load cursors are back at 0: data with no load lands in slot 0 and row 0 */
    stream_pair(&after, GPU_PGRAPH_PROGRAM_DATA, 0x11u);
    stream_pair(&after, GPU_PGRAPH_CONSTANT_DATA, float_bits_of(2.0f));
    CHECK(decode_pairs(pgraph, after.pairs, after.count) == GPU_PGRAPH_OK);
    const gpu_pgraph_draw *draw = gpu_pgraph_draw_at(pgraph, 0u);
    CHECK(draw != NULL && gpu_pgraph_draw_count(pgraph) == 1u);
    if (draw != NULL) {
        CHECK(!draw->arrays[3].address_set && !draw->arrays[3].format_set);
    }
    const gpu_pgraph_state *state = gpu_pgraph_state_now(pgraph);
    CHECK(state->program[0] == 0x11u && state->program[7u * 4u] == 0u);
    CHECK(state->constants[0] == 2.0f && state->constants[9u * 4u] == 0.0f);
    stream_free(&after);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
}

/* ---- the unhandled table's bound -------------------------------------------------------- */

static void test_unhandled_overflow(void)
{
    printf("test_unhandled_overflow\n");
    gpu_pgraph *pgraph = gpu_pgraph_create();
    stream_builder stream = {0};
    const uint32_t distinct = GPU_PGRAPH_UNHANDLED_TABLE + 10u;
    for (uint32_t i = 0u; i < distinct; i++) {
        stream_pair(&stream, 0x4000u + 4u * i, 1u);
    }
    CHECK(decode_pairs(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_unhandled_count(pgraph) == GPU_PGRAPH_UNHANDLED_TABLE);
    CHECK(gpu_pgraph_unhandled_overflow(pgraph) == 10u);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
}

/* ---- the three budgets ------------------------------------------------------------------ */

static void test_budgets(void)
{
    printf("test_budgets\n");
    /* indices: exactly GPU_PGRAPH_MAX_INDICES fit, one more is refused as FULL */
    {
        gpu_pgraph *pgraph = gpu_pgraph_create();
        stream_builder stream = {0};
        stream_pair(&stream, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_POINTS);
        for (uint32_t i = 0u; i < GPU_PGRAPH_MAX_INDICES / 256u; i++) {
            stream_pair(&stream, GPU_PGRAPH_DRAW_ARRAYS, (255u << 24) | (i * 256u));
        }
        CHECK(decode_pairs(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
        const gpu_pgraph_command one_more[] = {{GPU_PGRAPH_ARRAY_ELEMENT32, 5u}};
        CHECK(decode_pairs(pgraph, one_more, 1u) == GPU_PGRAPH_ERR_FULL);
        CHECK(strstr(gpu_pgraph_error(pgraph), "index list is full") != NULL);
        stream_free(&stream);
        gpu_pgraph_destroy(pgraph);
    }
    /* draws: GPU_PGRAPH_MAX_DRAWS with the state unchanged share one snapshot, the next is FULL */
    {
        gpu_pgraph *pgraph = gpu_pgraph_create();
        stream_builder stream = {0};
        for (uint32_t i = 0u; i < GPU_PGRAPH_MAX_DRAWS; i++) {
            stream_pair(&stream, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_POINTS);
            stream_pair(&stream, GPU_PGRAPH_ARRAY_ELEMENT32, i);
            stream_pair(&stream, GPU_PGRAPH_BEGIN_END, 0u);
        }
        CHECK(decode_pairs(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
        CHECK(gpu_pgraph_draw_count(pgraph) == GPU_PGRAPH_MAX_DRAWS);
        CHECK(gpu_pgraph_snapshot_count(pgraph) == 1u);
        stream_builder extra = {0};
        stream_pair(&extra, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_POINTS);
        stream_pair(&extra, GPU_PGRAPH_ARRAY_ELEMENT32, 1u);
        stream_pair(&extra, GPU_PGRAPH_BEGIN_END, 0u);
        CHECK(decode_pairs(pgraph, extra.pairs, extra.count) == GPU_PGRAPH_ERR_FULL);
        CHECK(strstr(gpu_pgraph_error(pgraph), "GPU_PGRAPH_MAX_DRAWS") != NULL);
        CHECK(gpu_pgraph_draw_count(pgraph) == GPU_PGRAPH_MAX_DRAWS);
        stream_free(&extra);
        stream_free(&stream);
        gpu_pgraph_destroy(pgraph);
    }
    /* snapshots: a state change before each draw makes one snapshot per draw, GPU_PGRAPH_MAX_SNAPSHOTS fit (T1268: it was 512,
     * one heavy frame of the Story level exceeded it and latched the replay off for good) and the peak is reported */
    {
        gpu_pgraph *pgraph = gpu_pgraph_create();
        stream_builder stream = {0};
        for (uint32_t i = 0u; i < GPU_PGRAPH_MAX_SNAPSHOTS; i++) {
            stream_pair(&stream, GPU_PGRAPH_EXECUTION_MODE, 2u + 4u * i);
            stream_pair(&stream, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_POINTS);
            stream_pair(&stream, GPU_PGRAPH_ARRAY_ELEMENT32, i);
            stream_pair(&stream, GPU_PGRAPH_BEGIN_END, 0u);
        }
        CHECK(decode_pairs(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
        CHECK(GPU_PGRAPH_MAX_SNAPSHOTS > 512u);
        CHECK(gpu_pgraph_snapshot_count(pgraph) == GPU_PGRAPH_MAX_SNAPSHOTS);
        CHECK(gpu_pgraph_get_stats(pgraph).snapshots_peak == GPU_PGRAPH_MAX_SNAPSHOTS);
        CHECK(gpu_pgraph_get_stats(pgraph).draws_peak == GPU_PGRAPH_MAX_SNAPSHOTS);
        CHECK(gpu_pgraph_draw_count(pgraph) == GPU_PGRAPH_MAX_SNAPSHOTS);
        stream_builder extra = {0};
        stream_pair(&extra, GPU_PGRAPH_EXECUTION_MODE, 3u);
        stream_pair(&extra, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_POINTS);
        stream_pair(&extra, GPU_PGRAPH_ARRAY_ELEMENT32, 1u);
        stream_pair(&extra, GPU_PGRAPH_BEGIN_END, 0u);
        CHECK(decode_pairs(pgraph, extra.pairs, extra.count) == GPU_PGRAPH_ERR_FULL);
        CHECK(strstr(gpu_pgraph_error(pgraph), "snapshot") != NULL);
        CHECK(gpu_pgraph_draw_count(pgraph) == GPU_PGRAPH_MAX_SNAPSHOTS);
        stream_free(&extra);
        stream_free(&stream);
        gpu_pgraph_destroy(pgraph);
    }
}

/* ---- which write starts a new snapshot ------------------------------------------------ */

/* One state write between two otherwise identical draws must split their snapshots, and the
 * second snapshot must hold the new value. test_snapshots_are_copies only changes a constant. */
static void check_split(const char *name, uint32_t method, uint32_t data, bool preload_program)
{
    gpu_pgraph *pgraph = gpu_pgraph_create();
    stream_builder stream = {0};
    if (preload_program) {
        stream_pair(&stream, GPU_PGRAPH_PROGRAM_LOAD, 0u);
        stream_pair(&stream, GPU_PGRAPH_PROGRAM_DATA, 1u);
        stream_pair(&stream, GPU_PGRAPH_PROGRAM_DATA + 4u, 2u);
        stream_pair(&stream, GPU_PGRAPH_PROGRAM_DATA + 8u, 3u);
        stream_pair(&stream, GPU_PGRAPH_PROGRAM_DATA + 12u, 4u);
        stream_pair(&stream, GPU_PGRAPH_PROGRAM_LOAD, 0u);
    }
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    stream_pair(&stream, method, data);
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    const bool decoded = decode_pairs(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK;
    CHECK(decoded);
    const gpu_pgraph_draw *first = gpu_pgraph_draw_at(pgraph, 0u);
    const gpu_pgraph_draw *second = gpu_pgraph_draw_at(pgraph, 1u);
    CHECK(first != NULL && second != NULL);
    if (decoded && first != NULL && second != NULL) {
        if (first->snapshot == second->snapshot) {
            printf("  %s: the second draw shares the first's snapshot\n", name);
        }
        CHECK(first->snapshot != second->snapshot);
        CHECK(gpu_pgraph_snapshot_count(pgraph) == 2u);
    }
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
}

static void test_each_write_splits_the_snapshot(void)
{
    printf("test_each_write_splits_the_snapshot\n");
    check_split("viewport offset", GPU_PGRAPH_VIEWPORT_OFFSET + 8u, float_bits_of(3.0f), false);
    check_split("viewport scale", GPU_PGRAPH_VIEWPORT_SCALE + 4u, float_bits_of(3.0f), false);
    check_split("program data", GPU_PGRAPH_PROGRAM_DATA, 0x77u, true);
    check_split("constant data", GPU_PGRAPH_CONSTANT_DATA + 4u, float_bits_of(3.0f), false);
    check_split("execution mode", GPU_PGRAPH_EXECUTION_MODE, 6u, false);
    check_split("program start", GPU_PGRAPH_PROGRAM_START, 3u, false);
    /* and a draw whose state did NOT change shares it (the control for all of the above) */
    gpu_pgraph *pgraph = gpu_pgraph_create();
    stream_builder stream = {0};
    stream_pair(&stream, GPU_PGRAPH_EXECUTION_MODE, 6u);
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(decode_pairs(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_snapshot_count(pgraph) == 1u && gpu_pgraph_draw_count(pgraph) == 2u);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
}

/* ---- the first draw of a stream takes a snapshot even with no state write -------------- */

static void test_first_draw_snapshots(void)
{
    printf("test_first_draw_snapshots\n");
    gpu_pgraph *pgraph = gpu_pgraph_create();
    stream_builder stream = {0};
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(decode_pairs(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_count(pgraph) == 1u && gpu_pgraph_snapshot_count(pgraph) == 1u);
    const gpu_pgraph_draw *draw = gpu_pgraph_draw_at(pgraph, 0u);
    CHECK(draw != NULL && gpu_pgraph_snapshot(pgraph, draw != NULL ? draw->snapshot : 99u) != NULL);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
}

/* ---- viewport offset w inside a bracket ------------------------------------------------ */

static void test_last_viewport_offset_register_is_vertex_state(void)
{
    printf("test_last_viewport_offset_register_is_vertex_state\n");
    gpu_pgraph *pgraph = gpu_pgraph_create();
    const gpu_pgraph_command inside[] = {{GPU_PGRAPH_BEGIN_END, 5u},
                                         {GPU_PGRAPH_VIEWPORT_OFFSET + 12u, 0u}};
    CHECK(decode_pairs(pgraph, inside, 2u) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(gpu_pgraph_error(pgraph), "inside a BEGIN_END") != NULL);
    gpu_pgraph_reset(pgraph);
    /* the first register past the run is NOT vertex state: an unhandled method, counted */
    const gpu_pgraph_command past[] = {{GPU_PGRAPH_BEGIN_END, 5u}, {GPU_PGRAPH_VIEWPORT_OFFSET + 16u, 0u}};
    CHECK(decode_pairs(pgraph, past, 2u) == GPU_PGRAPH_OK);
    gpu_pgraph_destroy(pgraph);
}

/* ---- partly rewritten slots and rows ------------------------------------------------- */

static void test_partial_rewrites(void)
{
    printf("test_partial_rewrites\n");
    gpu_pgraph *pgraph = gpu_pgraph_create();
    stream_builder stream = {0};
    stream_pair(&stream, GPU_PGRAPH_PROGRAM_LOAD, 4u);
    for (uint32_t i = 0u; i < 4u; i++) {
        stream_pair(&stream, GPU_PGRAPH_PROGRAM_DATA, 0x10u + i);
    }
    stream_pair(&stream, GPU_PGRAPH_CONSTANT_LOAD, 6u);
    for (uint32_t i = 0u; i < 4u; i++) {
        stream_pair(&stream, GPU_PGRAPH_CONSTANT_DATA, float_bits_of(1.0f + (float)i));
    }
    CHECK(decode_pairs(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_state_now(pgraph)->slot_written[4] && gpu_pgraph_state_now(pgraph)->constant_written[6]);
    /* rewrite both from the start with two dwords: neither is complete any more */
    stream_builder partial = {0};
    stream_pair(&partial, GPU_PGRAPH_PROGRAM_LOAD, 4u);
    stream_pair(&partial, GPU_PGRAPH_PROGRAM_DATA, 0x20u);
    stream_pair(&partial, GPU_PGRAPH_PROGRAM_DATA, 0x21u);
    stream_pair(&partial, GPU_PGRAPH_CONSTANT_LOAD, 6u);
    stream_pair(&partial, GPU_PGRAPH_CONSTANT_DATA, float_bits_of(9.0f));
    stream_pair(&partial, GPU_PGRAPH_CONSTANT_DATA, float_bits_of(8.0f));
    CHECK(decode_pairs(pgraph, partial.pairs, partial.count) == GPU_PGRAPH_OK);
    CHECK(!gpu_pgraph_state_now(pgraph)->slot_written[4]);
    CHECK(!gpu_pgraph_state_now(pgraph)->constant_written[6]);
    /* three of four dwords: still not complete (a row or slot is complete at the FOURTH) */
    const gpu_pgraph_command third_slot[] = {{GPU_PGRAPH_PROGRAM_DATA, 0x22u}};
    CHECK(decode_pairs(pgraph, third_slot, 1u) == GPU_PGRAPH_OK);
    CHECK(!gpu_pgraph_state_now(pgraph)->slot_written[4]);
    const gpu_pgraph_command third_row[] = {{GPU_PGRAPH_CONSTANT_DATA, float_bits_of(7.0f)}};
    CHECK(decode_pairs(pgraph, third_row, 1u) == GPU_PGRAPH_OK);
    CHECK(!gpu_pgraph_state_now(pgraph)->constant_written[6]);
    const gpu_pgraph_command fourth_slot[] = {{GPU_PGRAPH_PROGRAM_DATA, 0x23u}};
    CHECK(decode_pairs(pgraph, fourth_slot, 1u) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_state_now(pgraph)->slot_written[4]);
    const gpu_pgraph_command fourth_row[] = {{GPU_PGRAPH_CONSTANT_DATA, float_bits_of(6.0f)}};
    CHECK(decode_pairs(pgraph, fourth_row, 1u) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_state_now(pgraph)->constant_written[6]);
    stream_free(&partial);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
}

/* ---- index widths ---------------------------------------------------------------------- */

static void test_index_widths(void)
{
    printf("test_index_widths\n");
    gpu_pgraph *pgraph = gpu_pgraph_create();
    stream_builder stream = {0};
    stream_pair(&stream, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_POINTS);
    /* a DrawVertices start above 16 bits (24 bit field) */
    stream_pair(&stream, GPU_PGRAPH_DRAW_ARRAYS, (1u << 24) | 0x00ABCDEFu);
    /* a 16-bit index with its top bit set, low half then high half */
    stream_pair(&stream, GPU_PGRAPH_ARRAY_ELEMENT16, (0x8001u << 16) | 0x8000u);
    /* a 32-bit index above 16 bits */
    stream_pair(&stream, GPU_PGRAPH_ARRAY_ELEMENT32, 0x12345678u);
    stream_pair(&stream, GPU_PGRAPH_BEGIN_END, 0u);
    CHECK(decode_pairs(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    const gpu_pgraph_draw *draw = gpu_pgraph_draw_at(pgraph, 0u);
    CHECK(draw != NULL && draw->index_count == 5u);
    if (draw != NULL && draw->index_count == 5u) {
        const uint32_t *all = gpu_pgraph_indices(pgraph) + draw->first_index;
        CHECK(all[0] == 0x00ABCDEFu && all[1] == 0x00ABCDF0u);
        CHECK(all[2] == 0x8000u && all[3] == 0x8001u);
        CHECK(all[4] == 0x12345678u);
    }
    /* BEGIN_END(10) is POLYGON, the last known operation: recorded, refused later at replay */
    gpu_pgraph_reset(pgraph);
    stream_builder polygon = {0};
    stream_pair(&polygon, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_POLYGON);
    stream_pair(&polygon, GPU_PGRAPH_ARRAY_ELEMENT32, 1u);
    stream_pair(&polygon, GPU_PGRAPH_BEGIN_END, 0u);
    CHECK(decode_pairs(pgraph, polygon.pairs, polygon.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_count(pgraph) == 1u);
    CHECK(gpu_pgraph_draw_at(pgraph, 0u) != NULL &&
          gpu_pgraph_draw_at(pgraph, 0u)->primitive == GPU_PGRAPH_OP_POLYGON);
    stream_free(&polygon);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
}

/* ---- topology expansion at the exact fit ------------------------------------------------ */

static void test_expansion_exact_fit(void)
{
    printf("test_expansion_exact_fit\n");
    const uint32_t ids[8] = {10u, 11u, 12u, 13u, 14u, 15u, 16u, 17u};
    uint32_t out[32];
    /* a single quad: 4 indices, two triangles split along (0,2) */
    CHECK(gpu_pgraph_triangulate(GPU_PGRAPH_OP_QUADS, ids, 4u, out, 6u) == 6u);
    CHECK(out[0] == 10u && out[1] == 11u && out[2] == 12u);
    CHECK(out[3] == 10u && out[4] == 12u && out[5] == 13u);
    CHECK(gpu_pgraph_triangulate(GPU_PGRAPH_OP_QUADS, ids, 8u, out, 12u) == 12u);
    CHECK(out[6] == 14u && out[7] == 15u && out[8] == 16u && out[9] == 14u && out[10] == 16u &&
          out[11] == 17u);
    CHECK(gpu_pgraph_triangulate(GPU_PGRAPH_OP_QUADS, ids, 4u, out, 5u) == UINT32_MAX);
    /* the last triangle of a list, a strip and a fan, in a buffer that is exactly big enough */
    CHECK(gpu_pgraph_triangulate(GPU_PGRAPH_OP_TRIANGLES, ids, 3u, out, 3u) == 3u);
    CHECK(gpu_pgraph_triangulate(GPU_PGRAPH_OP_TRIANGLE_STRIP, ids, 3u, out, 3u) == 3u);
    CHECK(gpu_pgraph_triangulate(GPU_PGRAPH_OP_TRIANGLE_FAN, ids, 3u, out, 3u) == 3u);
    CHECK(out[0] == 10u && out[1] == 11u && out[2] == 12u);
    CHECK(gpu_pgraph_triangulate(GPU_PGRAPH_OP_TRIANGLE_STRIP, ids, 4u, out, 6u) == 6u);
    CHECK(out[3] == 12u && out[4] == 11u && out[5] == 13u); /* odd triangle: winding flipped */
    CHECK(gpu_pgraph_triangulate(GPU_PGRAPH_OP_TRIANGLE_FAN, ids, 4u, out, 6u) == 6u);
    CHECK(out[3] == 10u && out[4] == 12u && out[5] == 13u); /* pivot on the first */
    /* lines in a buffer that is exactly big enough */
    CHECK(gpu_pgraph_lineate(GPU_PGRAPH_OP_LINES, ids, 2u, out, 2u) == 2u);
    CHECK(out[0] == 10u && out[1] == 11u);
    CHECK(gpu_pgraph_lineate(GPU_PGRAPH_OP_LINES, ids, 4u, out, 4u) == 4u);
    CHECK(out[2] == 12u && out[3] == 13u);
    CHECK(gpu_pgraph_lineate(GPU_PGRAPH_OP_LINE_STRIP, ids, 3u, out, 4u) == 4u);
    CHECK(out[0] == 10u && out[1] == 11u && out[2] == 11u && out[3] == 12u);
    CHECK(gpu_pgraph_lineate(GPU_PGRAPH_OP_LINES, ids, 2u, out, 1u) == UINT32_MAX);
    /* LINE_LOOP, QUAD_STRIP and POLYGON are refused, not expanded to nothing */
    CHECK(gpu_pgraph_lineate(GPU_PGRAPH_OP_LINE_LOOP, ids, 4u, out, 32u) == UINT32_MAX);
    CHECK(gpu_pgraph_triangulate(GPU_PGRAPH_OP_QUAD_STRIP, ids, 8u, out, 32u) == UINT32_MAX);
    CHECK(gpu_pgraph_triangulate(GPU_PGRAPH_OP_POLYGON, ids, 8u, out, 32u) == UINT32_MAX);
}

/* ---- program resolution at the end of the file ------------------------------------------ */

static const char *const names_synthetic[] = {SYNTHETIC_PROGRAM_NAME};
static const struct gpu_vsh_table table_synthetic = {0u, 0u, NULL, 0u, NULL, 1u, names_synthetic};

static void test_program_at_the_last_slot(void)
{
    printf("test_program_at_the_last_slot\n");
    gpu_pgraph_backend backend = {0};
    backend.table = &table_synthetic;
    backend.allowed_inferences = GPU_PGRAPH_INFER_ALL;
    gpu_pgraph_program program;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    gpu_pgraph *pgraph = gpu_pgraph_create();
    /* one instruction with FINAL in slot 135, the last slot of the file */
    stream_builder stream = {0};
    stream_pair(&stream, GPU_PGRAPH_EXECUTION_MODE, 6u);
    stream_program(&stream, 135u, synthetic_program, 1u);
    stream_pair(&stream, GPU_PGRAPH_PROGRAM_START, 135u);
    CHECK(decode_pairs(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_resolve_program(gpu_pgraph_state_now(pgraph), &backend, &program, &report) ==
          GPU_PGRAPH_OK);
    CHECK(program.instructions == 1u && program.module == 0u && program.is_static);
    stream_free(&stream);
    /* the same instruction without FINAL in slot 135: every slot to the end of the file is written
     * and none has the bit, which is MALFORMED and names the FINAL bit */
    gpu_pgraph_reset(pgraph);
    uint32_t no_final[4];
    memcpy(no_final, synthetic_program, sizeof no_final);
    no_final[3] &= ~1u;
    stream_pair(&stream, GPU_PGRAPH_EXECUTION_MODE, 6u);
    stream_program(&stream, 135u, no_final, 1u);
    stream_pair(&stream, GPU_PGRAPH_PROGRAM_START, 135u);
    CHECK(decode_pairs(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_resolve_program(gpu_pgraph_state_now(pgraph), &backend, &program, &report) ==
          GPU_PGRAPH_ERR_MALFORMED);
    CHECK(strstr(report.error, "FINAL bit") != NULL);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
}

/* ---- vertex fetch: address overflow, no reader, the defaults inference per type ---------- */

static bool always_readable(void *context, uint32_t address, void *out, size_t bytes)
{
    (void)context;
    (void)address;
    memset(out, 0, bytes);
    return true;
}

/* All 16 slots enabled with one format, so no slot is the disabled (0, 0, 0, 1) default. */
static void enable_every_slot(stream_builder *stream, uint32_t address, uint32_t format)
{
    for (uint32_t slot = 0u; slot < GPU_PGRAPH_ATTRIBUTES; slot++) {
        stream_array(stream, slot, address, format);
    }
}

static gpu_pgraph_result assemble_one(uint32_t format, uint32_t address, const gpu_pgraph_backend *backend,
                                      gpu_pgraph_assembled *assembled, gpu_pgraph_report *report)
{
    gpu_pgraph *pgraph = gpu_pgraph_create();
    stream_builder stream = {0};
    enable_every_slot(&stream, address, format);
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(decode_pairs(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    const gpu_pgraph_result result = gpu_pgraph_assemble_draw(pgraph, 0u, backend, assembled, report);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
    return result;
}

static void test_fetch_edges(void)
{
    printf("test_fetch_edges\n");
    uint8_t memory[256] = {0};
    fake_guest guest = {0x1000u, memory, sizeof memory};
    gpu_pgraph_backend backend = {0};
    backend.read_guest = fake_guest_read;
    backend.context = &guest;
    backend.allowed_inferences = GPU_PGRAPH_INFER_ALL;
    gpu_pgraph_assembled assembled;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);

    /* an address whose vertex runs past 4 GiB is refused even when the reader would answer */
    gpu_pgraph_backend generous = backend;
    generous.read_guest = always_readable;
    CHECK(assemble_one(array_format(16u, 4u, GPU_PGRAPH_TYPE_F), 0xFFFFFFF8u, &generous, &assembled, &report) ==
          GPU_PGRAPH_ERR_MALFORMED);
    CHECK(strstr(report.error, "guest read") != NULL);
    /* the last vertex that fits is read */
    CHECK(assemble_one(array_format(0u, 4u, GPU_PGRAPH_TYPE_F), 0xFFFFFFEFu, &generous, &assembled, &report) ==
          GPU_PGRAPH_OK);
    gpu_pgraph_assembled_free(&assembled);
    /* a backend with no reader is refused, not called */
    gpu_pgraph_backend mute = backend;
    mute.read_guest = NULL;
    CHECK(assemble_one(array_format(16u, 4u, GPU_PGRAPH_TYPE_F), 0x1000u, &mute, &assembled, &report) ==
          GPU_PGRAPH_ERR_MALFORMED);
    CHECK(strstr(report.error, "guest read") != NULL);

    /* the component-defaults inference is used by a float3 and an S32K x2 and NOT by a float4 */
    const uint32_t without = GPU_PGRAPH_INFER_ALL & ~GPU_PGRAPH_INFER_COMPONENT_DEFAULTS;
    gpu_pgraph_backend strict = backend;
    strict.allowed_inferences = without;
    CHECK(assemble_one(array_format(0u, 4u, GPU_PGRAPH_TYPE_F), 0x1000u, &strict, &assembled, &report) ==
          GPU_PGRAPH_OK);
    CHECK((assembled.used_inferences & GPU_PGRAPH_INFER_COMPONENT_DEFAULTS) == 0u);
    gpu_pgraph_assembled_free(&assembled);
    CHECK(assemble_one(array_format(0u, 3u, GPU_PGRAPH_TYPE_F), 0x1000u, &strict, &assembled, &report) ==
          GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "(0, 0, 0, 1)") != NULL);
    CHECK(assemble_one(array_format(0u, 2u, GPU_PGRAPH_TYPE_S32K), 0x1000u, &strict, &assembled, &report) ==
          GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "(0, 0, 0, 1)") != NULL);
    CHECK(assemble_one(array_format(0u, 3u, GPU_PGRAPH_TYPE_F), 0x1000u, &backend, &assembled, &report) ==
          GPU_PGRAPH_OK);
    CHECK((assembled.used_inferences & GPU_PGRAPH_INFER_COMPONENT_DEFAULTS) != 0u);
    gpu_pgraph_assembled_free(&assembled);
    /* and the S32K gate itself: withheld, an S32K x2 is refused naming the unscaled reading */
    strict.allowed_inferences = GPU_PGRAPH_INFER_ALL & ~GPU_PGRAPH_INFER_S32K_UNNORMALISED;
    CHECK(assemble_one(array_format(0u, 2u, GPU_PGRAPH_TYPE_S32K), 0x1000u, &strict, &assembled, &report) ==
          GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "S32K") != NULL);
}

/* ---- the expansion buffer is big enough for a long strip -------------------------------- */

static void test_long_strip_and_vertex_bound(void)
{
    printf("test_long_strip_and_vertex_bound\n");
    uint8_t memory[64] = {0};
    fake_guest guest = {0x2000u, memory, sizeof memory};
    gpu_pgraph_backend backend = {0};
    backend.read_guest = fake_guest_read;
    backend.context = &guest;
    backend.allowed_inferences = GPU_PGRAPH_INFER_ALL;
    gpu_pgraph_assembled assembled;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);

    /* a strip of 12 indices makes 10 triangles, 30 expanded vertices (12 * 6 is the worst case) */
    gpu_pgraph *pgraph = gpu_pgraph_create();
    stream_builder stream = {0};
    stream_array(&stream, 1u, 0x2000u, array_format(0u, 3u, GPU_PGRAPH_TYPE_F));
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLE_STRIP, 0u, 12u);
    CHECK(decode_pairs(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &assembled, &report) == GPU_PGRAPH_OK);
    CHECK(assembled.vertex_count == 30u);
    gpu_pgraph_assembled_free(&assembled);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);

    /* exactly GPU_VSH_MAX_VERTICES expanded vertices are assembled, one more is refused */
    for (uint32_t extra = 0u; extra < 2u; extra++) {
        pgraph = gpu_pgraph_create();
        stream_array(&stream, 1u, 0x2000u, array_format(0u, 1u, GPU_PGRAPH_TYPE_F));
        stream_pair(&stream, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_POINTS);
        const uint32_t total = GPU_VSH_MAX_VERTICES + extra;
        for (uint32_t done = 0u; done < total; done += 256u) {
            const uint32_t chunk = total - done < 256u ? total - done : 256u;
            stream_pair(&stream, GPU_PGRAPH_DRAW_ARRAYS, ((chunk - 1u) << 24) | done);
        }
        stream_pair(&stream, GPU_PGRAPH_BEGIN_END, 0u);
        CHECK(decode_pairs(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
        const gpu_pgraph_result result = gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &assembled, &report);
        if (extra == 0u) {
            CHECK(result == GPU_PGRAPH_OK && assembled.vertex_count == GPU_VSH_MAX_VERTICES);
            gpu_pgraph_assembled_free(&assembled);
        } else {
            CHECK(result == GPU_PGRAPH_ERR_FULL);
            CHECK(strstr(report.error, "draw limit") != NULL);
        }
        stream_free(&stream);
        gpu_pgraph_destroy(pgraph);
    }
}

/* ---- the viewport offset register against a c59 the stream wrote itself ------------------ */

static void test_stream_written_offset_row_wins(void)
{
    printf("test_stream_written_offset_row_wins\n");
    uint8_t memory[16] = {0};
    fake_guest guest = {0x100u, memory, sizeof memory};
    gpu_pgraph_backend backend = {0};
    backend.read_guest = fake_guest_read;
    backend.context = &guest;
    backend.allowed_inferences = GPU_PGRAPH_INFER_ALL;
    gpu_pgraph *pgraph = gpu_pgraph_create();
    stream_builder stream = {0};
    static const float offset[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    static const float scale[4] = {5.0f, 6.0f, 7.0f, 8.0f};
    stream_viewport(&stream, offset, scale);
    static const float direct[4] = {10.0f, 20.0f, 30.0f, 40.0f};
    stream_constants(&stream, 59u, direct, 4u); /* only the OFFSET row, written by the title */
    stream_array(&stream, 1u, 0x100u, array_format(4u, 1u, GPU_PGRAPH_TYPE_F));
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(decode_pairs(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    gpu_pgraph_assembled assembled;
    gpu_pgraph_report report;
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &assembled, &report) == GPU_PGRAPH_OK);
    CHECK(assembled.constants[59u * 4u] == 10.0f && assembled.constants[59u * 4u + 3u] == 40.0f);
    CHECK(assembled.constants[58u * 4u] == 5.0f); /* the scale row comes from the register */
    gpu_pgraph_assembled_free(&assembled);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
}

/* ---- replay with nothing to render: argument limits, the clear, degenerate draws ----------- */

static void test_replay_without_a_device_draw(void)
{
    printf("test_replay_without_a_device_draw\n");
    char device_stand_in = 0; /* never dereferenced: nothing here reaches the device */
    gpu_device *device = (gpu_device *)&device_stand_in;
    gpu_pgraph_backend backend = {0};
    backend.table = &table_synthetic;
    backend.allowed_inferences = GPU_PGRAPH_INFER_ALL;
    gpu_pgraph *pgraph = gpu_pgraph_create();
    gpu_image image;
    gpu_pgraph_report report;
    const float clear[4] = {0.5f, 0.0f, 1.0f, 2.0f};

    /* the target size limit is 8192 in each direction, inclusive */
    CHECK(gpu_pgraph_replay(pgraph, device, &backend, 8193u, 1u, clear, &image, &report) ==
          GPU_PGRAPH_ERR_ARGUMENT);
    CHECK(gpu_pgraph_replay(pgraph, device, &backend, 1u, 8193u, clear, &image, &report) ==
          GPU_PGRAPH_ERR_ARGUMENT);
    CHECK(gpu_pgraph_replay(pgraph, device, &backend, 0u, 1u, clear, &image, &report) ==
          GPU_PGRAPH_ERR_ARGUMENT);
    CHECK(gpu_pgraph_replay(pgraph, device, &backend, 8192u, 1u, clear, &image, &report) == GPU_PGRAPH_OK);
    gpu_image_free(&image);

    /* the clear: 0.5 rounds to 128 (truncating would give 127), 1.0 and above clamp to 255 */
    CHECK(gpu_pgraph_replay(pgraph, device, &backend, 2u, 2u, clear, &image, &report) == GPU_PGRAPH_OK);
    CHECK(image.pixels != NULL && image.width == 2u && image.height == 2u && image.stride_bytes == 8u);
    if (image.pixels != NULL) {
        CHECK(image.pixels[0] == 128u && image.pixels[1] == 0u && image.pixels[2] == 255u &&
              image.pixels[3] == 255u);
        CHECK(image.pixels[4] == 128u && image.pixels[15] == 255u);
    }
    gpu_image_free(&image);
    const float negative[4] = {-1.0f, 0.25f, 0.0f, 1.0f};
    CHECK(gpu_pgraph_replay(pgraph, device, &backend, 1u, 1u, negative, &image, &report) == GPU_PGRAPH_OK);
    if (image.pixels != NULL) {
        CHECK(image.pixels[0] == 0u && image.pixels[1] == 64u && image.pixels[2] == 0u &&
              image.pixels[3] == 255u);
    }
    gpu_image_free(&image);

    /* a QUADS draw of three indices expands to nothing: counted as degenerate, never rendered */
    stream_builder stream = {0};
    stream_pair(&stream, GPU_PGRAPH_EXECUTION_MODE, 6u);
    stream_program(&stream, 5u, synthetic_program, 1u);
    stream_pair(&stream, GPU_PGRAPH_PROGRAM_START, 5u);
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_QUADS, 0u, 3u);
    CHECK(decode_pairs(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_replay(pgraph, device, &backend, 4u, 4u, clear, &image, &report) == GPU_PGRAPH_OK);
    CHECK(report.draws == 1u && report.degenerate == 1u && report.drawn == 0u && report.vertices == 0u);
    CHECK((report.used_inferences & GPU_PGRAPH_INFER_PROGRAM_HEADER) == 0u); /* nothing was drawn */
    gpu_image_free(&image);
    /* a second draw whose program is not in the table is refused and named by index */
    stream_free(&stream);
    static const uint32_t other_program[4] = {0x01010101u, 0x02020202u, 0x03030303u, 0x04040405u};
    stream_program(&stream, 5u, other_program, 1u);
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_QUADS, 0u, 3u);
    CHECK(decode_pairs(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_count(pgraph) == 2u);
    CHECK(gpu_pgraph_replay(pgraph, device, &backend, 4u, 4u, clear, &image, &report) ==
          GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(report.failed_draw == 1u && report.draws == 2u);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
}

int main(void)
{
    test_format_and_arguments();
    test_reset_forgets();
    test_unhandled_overflow();
    test_budgets();
    test_each_write_splits_the_snapshot();
    test_first_draw_snapshots();
    test_last_viewport_offset_register_is_vertex_state();
    test_partial_rewrites();
    test_index_widths();
    test_expansion_exact_fit();
    test_program_at_the_last_slot();
    test_fetch_edges();
    test_long_strip_and_vertex_bound();
    test_stream_written_offset_row_wins();
    test_replay_without_a_device_draw();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 && checks > 150 ? 0 : 1;
}
