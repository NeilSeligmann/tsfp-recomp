/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T267: the output state the REAL ported emitters write, over a real pushbuffer ring, kicked, recorded by
 * the instantaneous GPU and decoded by gpu_pgraph.c with the group on. This is what ties the decoder's
 * output-state methods and layouts to what the library's code writes rather than to the decoder's own idea
 * of them (test_d3d8_gpu_pgraph.c does the same for the vertex stage).
 *
 * SCISSOR: d3d8_set_scissors (SetScissors 0x003D4470, itself checked against the original under
 * emulation by tests/test_d3d8_scissor_oracle.py) writes nine dwords for one inclusive rectangle.
 *
 * THE TITLE'S OWN STATE BLOCK: its 19 immediate records (index and literal NV2A-encoded value, read from the
 * retail image and pinned by tests/test_gpu_state_census.py) pushed through the generated render-state table
 * (d3d8_render_state.c, the port of the dispatch the title's inlined SetRenderState runs), whose emit callback
 * is what writes the pair through 0x003D6C90. The decoder must handle exactly the groups it has and leave the
 * rest counted by number.
 *
 * CLEAR: the library's Clear (0x003D5EB0) writes the 0x1D98 rectangle run and the 0x1D8C run. Its port did write
 * nothing, which this file pinned, until T440 made d3d8_clear emit them. The producer is now checked, and its stream
 * decoded with the group on, in tests/c/test_d3d8_clear_decode.c.
 *
 * CULL has a measured emitter (the library's helpers 0x003D7060 and 0x003D70D0, read from the image's
 * bytes) and NO PRODUCER: their ports write no pair, which this file pins, so that the day a port starts
 * to emit them this test fails and the cull group's evidence (docs/tasks.md T267) is revisited.
 * Device free. Synthetic guest memory, not original-XBE equivalence evidence.
 */
#include "test_d3d8_support.h"

#include "d3d8_frame.h"
#include "d3d8_gpu.h"
#include "d3d8_gpu_pgraph.h"
#include "d3d8_scissor.h"
#include "d3d8_render_state.h"
#include "d3d8_state.h"
#include "gpu_pgraph.h"
#include "gpu_pgraph_replay.h"

#define ARENA 0x00D00000u
#define RECT (ARENA + 0x0000u)

typedef struct {
    gpu_pgraph_command pairs[32];
    size_t count;
} emitted;

static void collect(void *context, uint32_t header, uint32_t value)
{
    emitted *out = context;
    out->pairs[out->count].method = header & 0x1FFCu;
    out->pairs[out->count].data = value;
    out->count++;
}

static void record_scissor(uint32_t left, uint32_t top, uint32_t right, uint32_t bottom,
                           uint32_t surface_width, uint32_t surface_height)
{
    store(D3D8_DEVICE_BASE + 0x954u, surface_width);
    store(D3D8_DEVICE_BASE + 0x958u, surface_height);
    store(D3D8_DEVICE_BASE + 0x95Cu, 0x3F800000u); /* scale 1.0 */
    store(D3D8_DEVICE_BASE + 0x960u, 0x3F800000u);
    store(0x475CD4u, 0x3F000000u); /* the bias the original adds, 0.5, truncated away for an integer */
    store(RECT + 0u, left);
    store(RECT + 4u, top);
    store(RECT + 8u, right);
    store(RECT + 12u, bottom);
    CHECK_EQ_U32(d3d8_set_scissors(1u, 0u, RECT), 16u);
    d3d8_gpu_kick();
}

int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(ARENA, 0x10000u);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    store(0x003E3F58u, D3D8_DEVICE_BASE);
    CHECK(d3d8_gpu_create());
    CHECK(d3d8_pushbuffer_create());
    d3d8_device_store32(D3D8_DEV_FLAGS, 0x4203u);

    record_scissor(10u, 20u, 110u, 80u, 640u, 480u);
    CHECK_EQ_U32(d3d8_gpu_stream_count(), 5u);
    CHECK_EQ_U32(d3d8_gpu_get_stats().malformed_dwords, 0u);
    /* the five pairs, as the consumer recorded them: the count-2 header 0x80200 became two methods */
    static const uint32_t expected_methods[5] = {0x0200u, 0x0204u, 0x02B4u, 0x02C0u, 0x02E0u};
    static const uint32_t expected_data[5] = {10u | (100u << 16), 20u | (60u << 16), 0u,
                                              640u << 16, 480u << 16};
    for (uint32_t i = 0u; i < 5u; i++) {
        CHECK_EQ_U32(d3d8_gpu_stream_at(i).method, expected_methods[i]);
        CHECK_EQ_U32(d3d8_gpu_stream_at(i).data, expected_data[i]);
    }

    /* group OFF (the default): all five are unhandled, strict refuses, nothing else changed */
    gpu_pgraph *off = gpu_pgraph_create();
    size_t next = 0u;
    CHECK(d3d8_gpu_decode_recording(off, &next) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_unhandled_count(off) == 5u);
    gpu_pgraph_destroy(off);
    gpu_pgraph *strict = gpu_pgraph_create();
    gpu_pgraph_set_strict(strict, true);
    next = 0u;
    CHECK(d3d8_gpu_decode_recording(strict, &next) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(next == 0u);
    gpu_pgraph_destroy(strict);

    /* group ON, strict: every method the emitter wrote is one the model handles */
    gpu_pgraph *model = gpu_pgraph_create();
    gpu_pgraph_set_strict(model, true);
    gpu_pgraph_set_output_groups(model, GPU_PGRAPH_OUTPUT_SCISSOR);
    next = 0u;
    CHECK(d3d8_gpu_decode_recording(model, &next) == GPU_PGRAPH_OK);
    CHECK(next == 5u && gpu_pgraph_unhandled_count(model) == 0u);
    CHECK(gpu_pgraph_get_stats(model).pairs_handled == 5u);
    const gpu_pgraph_state *state = gpu_pgraph_state_now(model);
    for (uint32_t i = 0u; i < 5u; i++) {
        CHECK(state->output_written[i]);
        CHECK_EQ_U32(state->output[i], expected_data[i]);
    }

    /* and the replay's resolution of it for the 640x480 target is the rectangle that was asked for */
    gpu_pgraph_backend backend = {0};
    backend.output_groups = GPU_PGRAPH_OUTPUT_SCISSOR;
    backend.allowed_inferences = GPU_PGRAPH_INFER_OUTPUT_SCISSOR;
    gpu_pgraph_output output;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    CHECK(gpu_pgraph_resolve_output(state, &backend, 640u, 480u, &output, &report) == GPU_PGRAPH_OK);
    CHECK(output.active && output.output.scissor);
    CHECK_EQ_U32(output.output.scissor_x, 10u);
    CHECK_EQ_U32(output.output.scissor_y, 20u);
    CHECK_EQ_U32(output.output.scissor_width, 100u);
    CHECK_EQ_U32(output.output.scissor_height, 60u);
    CHECK_EQ_U32(output.used_inferences, GPU_PGRAPH_INFER_OUTPUT_SCISSOR);
    gpu_pgraph_destroy(model);

    /* the whole surface asked for is the whole surface */
    record_scissor(0u, 0u, 640u, 480u, 640u, 480u);
    model = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(model, GPU_PGRAPH_OUTPUT_SCISSOR);
    next = 5u;
    CHECK(d3d8_gpu_decode_recording(model, &next) == GPU_PGRAPH_OK);
    CHECK(next == 10u);
    backend.allowed_inferences = 0u;
    CHECK(gpu_pgraph_resolve_output(gpu_pgraph_state_now(model), &backend, 640u, 480u, &output, &report) ==
          GPU_PGRAPH_OK);
    CHECK(!output.active && output.used_inferences == 0u);
    gpu_pgraph_destroy(model);

    /* the title's own immediate render states, through the real table, into the decoder */
    static const struct {
        int32_t state;
        uint32_t value;
    } title_block[] = {
        {0x3B, 1u},      {0x3A, 0x204u},  {0x3D, 1u},      {0x3C, 0u},      {0x4A, 0x8006u},
        {0x43, 0x01010101u}, {0x3F, 0u},  {0x4E, 0u},      {0x4D, 0u},      {0x51, 0u},
        {0x4F, 0u},      {0x3E, 0x302u},  {0x46, 0x207u},  {0x44, 0x1E00u}, {0x45, 0x1E00u},
        {0x47, 0u},      {0x39, 0x203u},  {0x40, 1u},      {0x4C, 3u},
    };
    d3d8_render_state table_state;
    emitted title = {.count = 0u};
    d3d8_render_state_init(&table_state, collect, &title);
    for (size_t i = 0u; i < sizeof title_block / sizeof title_block[0]; i++) {
        CHECK(d3d8_set_render_state_inlined(&table_state, title_block[i].state, title_block[i].value) ==
              D3D8_RS_OK_IMMEDIATE);
    }
    CHECK(title.count == 19u);
    /* off: all 19 unhandled, strict refuses at the first (0x0304, BLEND_ENABLE) */
    gpu_pgraph *none = gpu_pgraph_create();
    CHECK(gpu_pgraph_decode(none, title.pairs, title.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_unhandled_count(none) == 19u);
    gpu_pgraph_destroy(none);
    gpu_pgraph *strict_none = gpu_pgraph_create();
    gpu_pgraph_set_strict(strict_none, true);
    CHECK(gpu_pgraph_decode(strict_none, title.pairs, title.count) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(gpu_pgraph_error(strict_none), "pair 0 method 0x0304") != NULL);
    gpu_pgraph_destroy(strict_none);
    /* blend and alpha test on: their 8 words are handled, the other 11 stay unhandled and strict still
     * refuses the first of them (0x0388, POLYGON_OFFSET_BIAS, the 8th pair in the title's own order) */
    gpu_pgraph *strict_block = gpu_pgraph_create();
    gpu_pgraph_set_strict(strict_block, true);
    gpu_pgraph_set_output_groups(strict_block, GPU_PGRAPH_OUTPUT_BLEND | GPU_PGRAPH_OUTPUT_ALPHA_TEST);
    CHECK(gpu_pgraph_decode(strict_block, title.pairs, title.count) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(gpu_pgraph_error(strict_block), "pair 7 method 0x0388") != NULL);
    gpu_pgraph_destroy(strict_block);
    gpu_pgraph *block = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(block, GPU_PGRAPH_OUTPUT_BLEND | GPU_PGRAPH_OUTPUT_ALPHA_TEST);
    CHECK(gpu_pgraph_decode(block, title.pairs, title.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_get_stats(block).pairs_handled == 8u);
    CHECK(gpu_pgraph_unhandled_count(block) == 11u);
    const gpu_pgraph_state *title_state = gpu_pgraph_state_now(block);
    CHECK(title_state->output[GPU_PGRAPH_OUT_BLEND_ENABLE] == 1u);
    CHECK(title_state->output[GPU_PGRAPH_OUT_BLEND_SFACTOR] == 0x302u);
    CHECK(title_state->output[GPU_PGRAPH_OUT_BLEND_DFACTOR] == 0u);
    CHECK(title_state->output[GPU_PGRAPH_OUT_BLEND_EQUATION] == 0x8006u);
    CHECK(title_state->output[GPU_PGRAPH_OUT_COLOR_MASK] == 0x01010101u);
    CHECK(title_state->output[GPU_PGRAPH_OUT_ALPHA_TEST_ENABLE] == 0u);
    CHECK(title_state->output[GPU_PGRAPH_OUT_ALPHA_FUNC] == 0x204u);
    CHECK(title_state->output[GPU_PGRAPH_OUT_ALPHA_REF] == 1u);
    CHECK(!title_state->output_written[GPU_PGRAPH_OUT_BLEND_COLOR]);
    /* and the resolution of the title's own block: blending on with SRC_ALPHA over ZERO, no alpha test,
     * every colour channel written */
    gpu_pgraph_backend title_backend = {0};
    title_backend.output_groups = GPU_PGRAPH_OUTPUT_BLEND | GPU_PGRAPH_OUTPUT_ALPHA_TEST;
    title_backend.allowed_inferences = GPU_PGRAPH_INFER_OUTPUT_ALL;
    gpu_pgraph_output title_output;
    gpu_pgraph_report title_report;
    memset(&title_report, 0, sizeof title_report);
    CHECK(gpu_pgraph_resolve_output(title_state, &title_backend, 640u, 480u, &title_output, &title_report) ==
          GPU_PGRAPH_OK);
    CHECK(title_output.active && title_output.output.blend && title_output.needs_destination);
    CHECK_EQ_U32(title_output.output.blend_source, GPU_VSH_BLEND_SRC_ALPHA);
    CHECK_EQ_U32(title_output.output.blend_destination, GPU_VSH_BLEND_ZERO);
    CHECK_EQ_U32(title_output.output.blend_equation, GPU_VSH_BLEND_OP_ADD);
    CHECK(!title_output.output.alpha_test && title_output.output.color_write_disable == 0u);
    CHECK_EQ_U32(title_output.used_inferences, GPU_PGRAPH_INFER_OUTPUT_BLEND_MODEL);
    gpu_pgraph_destroy(block);
    /* every group on (T502): the title's whole 19 record block decodes in strict mode with nothing unhandled. Depth
     * and stencil take 6 words, polygon offset 4 (0x0388, 0x0384, 0x0338, 0x0330) and 0x09F8 is the one pair of the
     * IGNORED group, decoded and counted as skipped on purpose */
    gpu_pgraph *everything = gpu_pgraph_create();
    gpu_pgraph_set_strict(everything, true);
    gpu_pgraph_set_output_groups(everything, GPU_PGRAPH_OUTPUT_ALL_MEASURED);
    CHECK(gpu_pgraph_decode(everything, title.pairs, title.count) == GPU_PGRAPH_OK);
    CHECK(title.count == 19u);
    CHECK(gpu_pgraph_get_stats(everything).pairs_handled == 19u);
    CHECK(gpu_pgraph_unhandled_count(everything) == 0u);
    CHECK(gpu_pgraph_get_stats(everything).pairs_ignored == 1u);
    {
        const gpu_pgraph_state *offsets = gpu_pgraph_state_now(everything);
        CHECK(offsets->output_written[GPU_PGRAPH_OUT_POLY_OFFSET_BIAS] && offsets->output_written[GPU_PGRAPH_OUT_POLY_OFFSET_SCALE]);
        CHECK(offsets->output_written[GPU_PGRAPH_OUT_POLY_OFFSET_FILL] && offsets->output_written[GPU_PGRAPH_OUT_POLY_OFFSET_POINT]);
        CHECK(!offsets->output_written[GPU_PGRAPH_OUT_POLY_OFFSET_LINE]); /* the ZBIAS handler's pairs are elided */
        CHECK_EQ_U32(offsets->output[GPU_PGRAPH_OUT_POLY_OFFSET_BIAS] | offsets->output[GPU_PGRAPH_OUT_POLY_OFFSET_SCALE] |
                         offsets->output[GPU_PGRAPH_OUT_POLY_OFFSET_FILL] | offsets->output[GPU_PGRAPH_OUT_POLY_OFFSET_POINT], 0u);
        CHECK_EQ_U32(offsets->output[GPU_PGRAPH_OUT_SPECULAR_PARAMS], 3u);
        CHECK(!offsets->output_written[GPU_PGRAPH_OUT_DITHER_ENABLE]);
    }
    const gpu_pgraph_state *all_state = gpu_pgraph_state_now(everything);
    CHECK_EQ_U32(all_state->output[GPU_PGRAPH_OUT_DEPTH_FUNC], 0x203u);
    CHECK_EQ_U32(all_state->output[GPU_PGRAPH_OUT_DEPTH_MASK], 1u);
    CHECK_EQ_U32(all_state->output[GPU_PGRAPH_OUT_STENCIL_FUNC], 0x207u);
    CHECK_EQ_U32(all_state->output[GPU_PGRAPH_OUT_STENCIL_REF], 0u);
    CHECK_EQ_U32(all_state->output[GPU_PGRAPH_OUT_STENCIL_OP_ZFAIL], 0x1E00u);
    CHECK_EQ_U32(all_state->output[GPU_PGRAPH_OUT_STENCIL_OP_ZPASS], 0x1E00u);
    CHECK(!all_state->output_written[GPU_PGRAPH_OUT_DEPTH_ENABLE] &&
          !all_state->output_written[GPU_PGRAPH_OUT_STENCIL_ENABLE]);
    /* resolved: the title's block depth tests (LEQUAL, writing) once the enable is assumed, and its stencil words
     * change nothing */
    title_backend.output_groups = GPU_PGRAPH_OUTPUT_ALL_MEASURED;
    CHECK(gpu_pgraph_resolve_output(all_state, &title_backend, 640u, 480u, &title_output, &title_report) ==
          GPU_PGRAPH_OK);
    CHECK(title_output.output.depth_test && title_output.output.depth_write && title_output.needs_depth);
    CHECK_EQ_U32(title_output.output.depth_func, GPU_VSH_COMPARE_LEQUAL);
    CHECK(!title_output.output.stencil_test && !title_output.output.alpha_test && !title_output.output.scissor);
    CHECK_EQ_U32(title_output.output.cull_mode, GPU_VSH_CULL_NONE);
    CHECK_EQ_U32(title_output.used_inferences,
                 GPU_PGRAPH_INFER_OUTPUT_BLEND_MODEL | GPU_PGRAPH_INFER_OUTPUT_DEPTH_MODEL |
                     GPU_PGRAPH_INFER_OUTPUT_DEPTH_STENCIL_ENABLE);
    /* without the enable inference the depth state of the real block cannot be applied: refused, naming it */
    title_backend.allowed_inferences = GPU_PGRAPH_INFER_OUTPUT_BLEND_MODEL | GPU_PGRAPH_INFER_OUTPUT_DEPTH_MODEL;
    CHECK(gpu_pgraph_resolve_output(all_state, &title_backend, 640u, 480u, &title_output, &title_report) ==
          GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(title_report.error, "0x030C") != NULL);
    gpu_pgraph_destroy(everything);

    /* the cull and front face helper ports (states 0x93 and the front face one) record NOTHING */
    const size_t before = d3d8_gpu_stream_count();
    const uint32_t cursor_before = d3d8_device_load32(D3D8_DEV_CURSOR);
    d3d8_state_set_93(0x901u);
    d3d8_state_set_93(0u);
    d3d8_gpu_kick();
    CHECK(d3d8_gpu_stream_count() == before);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), cursor_before);

    environment_end();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0 || checks < 40;
}
