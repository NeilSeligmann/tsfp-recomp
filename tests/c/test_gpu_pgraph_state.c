/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T267: the OUTPUT STATE groups of the pushbuffer replay (scissor, then the groups added in later
 * commits), opt-in, decoded by gpu_pgraph.c and applied by gpu_pgraph_replay.c through gpu_vsh_draw.
 *
 * Three layers, so a failure says which one broke:
 *   1. DECODE, no device: with a group off its methods are unhandled exactly as before (counted by
 *      number, strict refuses), with it on they land in gpu_pgraph_state.output, snapshots hold them,
 *      a write inside a BEGIN_END bracket is refused.
 *   2. RESOLVE, no device (gpu_pgraph_resolve_output): the numbers the replay will hand to Vulkan, and
 *      every refusal, each one asserted by its message so a refusal for the wrong reason cannot pass.
 *   3. PIXELS, on every Vulkan device that exists ("hardware" and "software"): a stated SKIP per absent
 *      device, exit 77 (a ctest SKIP) when nothing ran. Every pixel is compared, not a sample.
 *
 * The module and the scene are test_gpu_pgraph_replay.c's: a hand-written vertex shader (v1 -> gl_Position,
 * oD0 = v2.zyxw + c[3]) found by the SHA-256 name of a one-instruction program.
 */
#include "gpu_device.h"
#include "gpu_pgraph.h"
#include "gpu_pgraph_replay.h"
#include "gpu_pgraph_test_support.h"
#include "gpu_pgraph_vertex_words.h"
#include "gpu_vsh_draw.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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

#define WIDTH 64u
#define HEIGHT 64u
#define MEMORY_BASE 0x00600000u

typedef struct {
    float position[3];
    float v2[4];
} float_vertex; /* 28 bytes */

static const char *const module_names[] = {SYNTHETIC_PROGRAM_NAME};
static const struct gpu_vsh_table table = {0u, 0u, NULL, 0u, NULL, 1u, module_names};

static bool load_module(void *context, uint32_t module, const uint32_t **words, size_t *word_count)
{
    (void)context;
    if (module != 0u) {
        return false;
    }
    *words = draw_vertex_words;
    *word_count = sizeof draw_vertex_words / sizeof(uint32_t);
    return true;
}

static uint8_t memory[0x400];

/* Vertices 0..2: a large triangle (upper left half of the target), vertices 3..5: a second one (lower
 * right half), vertices 6..8: the second one again in the other order, vertices 9..11 and 12..14: the first
 * again at depth 0.25 and at depth 0.75 (0..2 are at 0.5). v2 = (0, 0, 1, 1) so oD0 = v2.zyxw =
 * (1, 0, 0, 1), red. Seen on the target with row 0 at the top, triangles 0..2 and 3..5 run CLOCKWISE (left to
 * right along the top, down, back) and 6..8 run COUNTER-CLOCKWISE. */
static void fill_memory(void)
{
    memset(memory, 0, sizeof memory);
    static const float corners[15][3] = {
        {-0.9f, -0.9f, 0.5f}, {0.9f, -0.9f, 0.5f}, {-0.9f, 0.9f, 0.5f}, /* upper left half */
        {0.9f, -0.8f, 0.5f},  {0.9f, 0.9f, 0.5f},  {-0.8f, 0.9f, 0.5f}, /* lower right half */
        {-0.8f, 0.9f, 0.5f},  {0.9f, 0.9f, 0.5f},  {0.9f, -0.8f, 0.5f}, /* the same, counter-clockwise */
        {-0.9f, -0.9f, 0.25f}, {0.9f, -0.9f, 0.25f}, {-0.9f, 0.9f, 0.25f}, /* upper left, near */
        {-0.9f, -0.9f, 0.75f}, {0.9f, -0.9f, 0.75f}, {-0.9f, 0.9f, 0.75f}, /* upper left, far */
    };
    for (uint32_t i = 0u; i < 15u; i++) {
        const float_vertex vertex = {{corners[i][0], corners[i][1], corners[i][2]}, {0.0f, 0.0f, 1.0f, 1.0f}};
        memcpy(memory + i * sizeof vertex, &vertex, sizeof vertex);
    }
    /* T502: vertices 15 and 16, a line inside the upper left half at depth 0.5, 17 and 18 the same line at 0.25,
     * 19..21 a SLOPED triangle (the upper left half again, depth 0.25 at the left edge to 0.75 at the right). */
    static const float extra[7][3] = {
        {-0.5f, -0.5f, 0.5f},  {-0.2f, -0.5f, 0.5f},  {-0.5f, -0.5f, 0.25f}, {-0.2f, -0.5f, 0.25f},
        {-0.9f, -0.9f, 0.25f}, {0.9f, -0.9f, 0.75f},  {-0.9f, 0.9f, 0.25f},
    };
    for (uint32_t i = 0u; i < 7u; i++) {
        const float_vertex vertex = {{extra[i][0], extra[i][1], extra[i][2]}, {0.0f, 0.0f, 1.0f, 1.0f}};
        memcpy(memory + (15u + i) * sizeof vertex, &vertex, sizeof vertex);
    }
}

static void stream_setup(stream_builder *stream)
{
    static const float offset[4] = {32.0f, 32.0f, 0.0f, 0.0f};
    static const float scale[4] = {32.0f, -32.0f, 1.0f, 0.0f};
    stream_viewport(stream, offset, scale);
    stream_pair(stream, GPU_PGRAPH_EXECUTION_MODE, 6u);
    stream_program(stream, 0u, synthetic_program, 1u);
    stream_pair(stream, GPU_PGRAPH_PROGRAM_START, 0u);
    static const float c3_zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    stream_constants(stream, 3u, c3_zero, 4u);
    stream_array(stream, 1u, MEMORY_BASE, array_format(28u, 3u, GPU_PGRAPH_TYPE_F));
    stream_array(stream, 2u, MEMORY_BASE + 12u, array_format(28u, 4u, GPU_PGRAPH_TYPE_F));
}

/* The packet d3d8_scissor.c writes for one inclusive rectangle (9 dwords: 0x80200, x | w << 16,
 * y | h << 16, 0x402B4, 0, 0x402C0, fields[0] << 16, 0x402E0, fields[1] << 16), as method/data pairs the
 * way the consumer records it. */
static void stream_scissor(stream_builder *stream, uint32_t x, uint32_t y, uint32_t width,
                           uint32_t height, uint32_t window_width, uint32_t window_height)
{
    stream_pair(stream, 0x0200u, x | (width << 16));
    stream_pair(stream, 0x0204u, y | (height << 16));
    stream_pair(stream, 0x02B4u, 0u);
    stream_pair(stream, 0x02C0u, window_width << 16);
    stream_pair(stream, 0x02E0u, window_height << 16);
}

/* The cull helpers' arithmetic, read from the library's bytes: 0x003D7060 writes 0x0308 = (mode != 0) and,
 * for a non-zero mode, 0x039C = 0x404 + (mode != the stored front face); 0x003D70D0 writes 0x03A0 = its
 * argument (and then runs the cull helper again with the stored mode). */
static void stream_front_face(stream_builder *stream, uint32_t front)
{
    stream_pair(stream, 0x03A0u, front);
}

static void stream_cull_mode(stream_builder *stream, uint32_t mode, uint32_t stored_front)
{
    stream_pair(stream, 0x0308u, mode != 0u ? 1u : 0u);
    if (mode != 0u) {
        stream_pair(stream, 0x039Cu, 0x404u + (mode != stored_front ? 1u : 0u));
    }
}

/* The pairs of the title's own state table walk (the inlined SetRenderState through 0x003D6C90, one pair per
 * state), in the order the methods are written there. */
static void stream_blend(stream_builder *stream, uint32_t enable, uint32_t source, uint32_t destination,
                         uint32_t equation)
{
    stream_pair(stream, 0x0304u, enable);
    stream_pair(stream, 0x0348u, destination);
    stream_pair(stream, 0x0344u, source);
    stream_pair(stream, 0x0350u, equation);
}

static void stream_alpha_test(stream_builder *stream, uint32_t enable, uint32_t function, uint32_t reference)
{
    stream_pair(stream, 0x033Cu, function);
    stream_pair(stream, 0x0340u, reference);
    stream_pair(stream, 0x0300u, enable);
}

static void stream_depth_state(stream_builder *stream, uint32_t enable, uint32_t function, uint32_t mask)
{
    stream_pair(stream, 0x030Cu, enable);
    stream_pair(stream, 0x0354u, function);
    stream_pair(stream, 0x035Cu, mask);
}

static void stream_stencil_state(stream_builder *stream, uint32_t enable, uint32_t function, uint32_t reference,
                                 uint32_t function_mask, uint32_t write_mask, uint32_t on_fail, uint32_t on_zfail,
                                 uint32_t on_zpass)
{
    stream_pair(stream, 0x032Cu, enable);
    stream_pair(stream, 0x0364u, function);
    stream_pair(stream, 0x0368u, reference);
    stream_pair(stream, 0x036Cu, function_mask);
    stream_pair(stream, 0x0360u, write_mask);
    stream_pair(stream, 0x0370u, on_fail);
    stream_pair(stream, 0x0374u, on_zfail);
    stream_pair(stream, 0x0378u, on_zpass);
}

/* One clear as the library's Clear writes it (0x003D61C4): the count-2 rectangle run at 0x1D98, then the count-3 run
 * at 0x1D8C that ends with CLEAR_SURFACE. The rectangle is INCLUSIVE. */
static void stream_clear(stream_builder *stream, uint32_t flags, uint32_t colour, uint32_t zstencil,
                         uint32_t x_min, uint32_t x_max, uint32_t y_min, uint32_t y_max)
{
    stream_pair(stream, 0x1D98u, x_min | (x_max << 16));
    stream_pair(stream, 0x1D9Cu, y_min | (y_max << 16));
    stream_pair(stream, 0x1D8Cu, zstencil);
    stream_pair(stream, 0x1D90u, colour);
    stream_pair(stream, GPU_PGRAPH_CLEAR_SURFACE, flags);
}

static void stream_colour_constant(stream_builder *stream, float r, float g, float b, float a)
{
    const float c3[4] = {r, g, b, a};
    stream_constants(stream, 3u, c3, 4u);
}

static gpu_pgraph_backend make_backend(fake_guest *guest, uint32_t groups, uint32_t inferences)
{
    gpu_pgraph_backend backend = {0};
    backend.table = &table;
    backend.read_guest = fake_guest_read;
    backend.load_module = load_module;
    backend.context = guest;
    backend.allowed_inferences = GPU_PGRAPH_INFER_ALL | inferences;
    backend.output_groups = groups;
    return backend;
}

static bool pixel_is(const gpu_image *image, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b,
                     uint8_t a)
{
    const uint8_t *p = image->pixels + gpu_image_offset(image, x, y);
    return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}

static bool same_pixel(const gpu_image *a, const gpu_image *b, uint32_t x, uint32_t y)
{
    return memcmp(a->pixels + gpu_image_offset(a, x, y), b->pixels + gpu_image_offset(b, x, y), 4u) == 0;
}

static bool identical(const gpu_image *a, const gpu_image *b)
{
    return a->width == b->width && a->height == b->height &&
           memcmp(a->pixels, b->pixels, (size_t)a->stride_bytes * a->height) == 0;
}

static uint32_t count_red(const gpu_image *image)
{
    uint32_t count = 0u;
    for (uint32_t y = 0u; y < image->height; y++) {
        for (uint32_t x = 0u; x < image->width; x++) {
            count += pixel_is(image, x, y, 255u, 0u, 0u, 255u);
        }
    }
    return count;
}

/* what the plain scene depends on: the program header, the viewport registers feeding c58/c59 and the
 * (0, 0, 0, 1) component default of its 3 float positions. No D3DCOLOR, point, line or S32K. */
#define PLAIN_INFERENCES                                                                      \
    (GPU_PGRAPH_INFER_PROGRAM_HEADER | GPU_PGRAPH_INFER_VIEWPORT_CONSTANTS |                    \
     GPU_PGRAPH_INFER_COMPONENT_DEFAULTS)

static const float clear_grey[4] = {0.2f, 0.2f, 0.2f, 1.0f}; /* 51, 51, 51, 255 */

/* --- 1. decode ---------------------------------------------------------------------------- */

static void test_decode(void)
{
    printf("test_decode\n");
    /* the table: every scissor word has its method and group, nothing else is one */
    uint32_t group = 0u;
    CHECK(gpu_pgraph_output_index(0x0200u, &group) == GPU_PGRAPH_OUT_SURFACE_CLIP_HORIZONTAL);
    CHECK(group == GPU_PGRAPH_OUTPUT_SCISSOR);
    CHECK(gpu_pgraph_output_index(0x0204u, NULL) == GPU_PGRAPH_OUT_SURFACE_CLIP_VERTICAL);
    CHECK(gpu_pgraph_output_index(0x02B4u, NULL) == GPU_PGRAPH_OUT_WINDOW_CLIP_TYPE);
    CHECK(gpu_pgraph_output_index(0x02C0u, NULL) == GPU_PGRAPH_OUT_WINDOW_CLIP_HORIZONTAL);
    CHECK(gpu_pgraph_output_index(0x02E0u, NULL) == GPU_PGRAPH_OUT_WINDOW_CLIP_VERTICAL);
    CHECK(gpu_pgraph_output_index(0x02C4u, NULL) == -1); /* window clip entry 1: no emitter writes it */
    CHECK(gpu_pgraph_output_index(0x02E4u, NULL) == -1);
    CHECK(gpu_pgraph_output_index(0x0208u, NULL) == GPU_PGRAPH_OUT_SURFACE_FORMAT); /* T462: the SURFACE group decodes it now */
    CHECK(gpu_pgraph_output_group(GPU_PGRAPH_OUT_COUNT) == 0u);
    CHECK(GPU_PGRAPH_OUT_COUNT >= 5u);

    stream_builder stream = {0};
    stream_scissor(&stream, 8u, 16u, 30u, 20u, 640u, 480u);
    CHECK(stream.count == 5u);

    /* OFF (the default): every scissor method is unhandled, counted by number, strict refuses the first */
    gpu_pgraph *off = gpu_pgraph_create();
    CHECK(gpu_pgraph_decode(off, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_unhandled_count(off) == 5u);
    uint32_t method = 0u;
    uint64_t pairs = 0u;
    gpu_pgraph_unhandled_at(off, 0u, &method, &pairs);
    CHECK(method == 0x0200u && pairs == 1u);
    CHECK(!gpu_pgraph_state_now(off)->output_written[GPU_PGRAPH_OUT_SURFACE_CLIP_HORIZONTAL]);
    gpu_pgraph_destroy(off);
    gpu_pgraph *strict = gpu_pgraph_create();
    gpu_pgraph_set_strict(strict, true);
    CHECK(gpu_pgraph_decode(strict, stream.pairs, stream.count) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(gpu_pgraph_error(strict), "pair 0 method 0x0200") != NULL);
    gpu_pgraph_destroy(strict);

    /* ON: handled, strict accepts, the shadow holds the raw dwords */
    gpu_pgraph *on = gpu_pgraph_create();
    gpu_pgraph_set_strict(on, true);
    gpu_pgraph_set_output_groups(on, GPU_PGRAPH_OUTPUT_SCISSOR);
    CHECK(gpu_pgraph_decode(on, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_unhandled_count(on) == 0u);
    CHECK(gpu_pgraph_get_stats(on).pairs_handled == 5u);
    const gpu_pgraph_state *state = gpu_pgraph_state_now(on);
    CHECK(state->output_groups == GPU_PGRAPH_OUTPUT_SCISSOR);
    CHECK(state->output_written[GPU_PGRAPH_OUT_SURFACE_CLIP_HORIZONTAL] &&
          state->output[GPU_PGRAPH_OUT_SURFACE_CLIP_HORIZONTAL] == (8u | (30u << 16)));
    CHECK(state->output_written[GPU_PGRAPH_OUT_SURFACE_CLIP_VERTICAL] &&
          state->output[GPU_PGRAPH_OUT_SURFACE_CLIP_VERTICAL] == (16u | (20u << 16)));
    CHECK(state->output_written[GPU_PGRAPH_OUT_WINDOW_CLIP_TYPE] &&
          state->output[GPU_PGRAPH_OUT_WINDOW_CLIP_TYPE] == 0u);
    CHECK(state->output[GPU_PGRAPH_OUT_WINDOW_CLIP_HORIZONTAL] == (640u << 16));
    CHECK(state->output[GPU_PGRAPH_OUT_WINDOW_CLIP_VERTICAL] == (480u << 16));
    /* an unmeasured neighbour is still refused when the group is on */
    const gpu_pgraph_command entry_one = {0x02C4u, 0u};
    CHECK(gpu_pgraph_decode(on, &entry_one, 1u) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(gpu_pgraph_error(on), "0x02C4") != NULL);
    /* a reset forgets the values and KEEPS the opt-in, like strictness */
    gpu_pgraph_reset(on);
    CHECK(gpu_pgraph_state_now(on)->output_groups == GPU_PGRAPH_OUTPUT_SCISSOR);
    CHECK(!gpu_pgraph_state_now(on)->output_written[GPU_PGRAPH_OUT_SURFACE_CLIP_HORIZONTAL]);
    /* a bit that is not a group is dropped, not remembered */
    gpu_pgraph_set_output_groups(on, 0x8000u);
    CHECK(gpu_pgraph_state_now(on)->output_groups == 0u);
    gpu_pgraph_destroy(on);

    /* state inside a bracket is refused, and the same word outside one is not */
    gpu_pgraph *bracket = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(bracket, GPU_PGRAPH_OUTPUT_SCISSOR);
    const gpu_pgraph_command open_bracket = {GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_TRIANGLES};
    const gpu_pgraph_command inside = {0x0200u, 0x00100000u};
    CHECK(gpu_pgraph_decode(bracket, &open_bracket, 1u) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_decode(bracket, &inside, 1u) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(gpu_pgraph_error(bracket), "inside a BEGIN_END bracket") != NULL);
    gpu_pgraph_destroy(bracket);

    /* snapshots: two draws under one scissor share a snapshot, a changed rectangle makes a new one, and
     * each draw's snapshot holds the rectangle that was current at ITS end */
    stream_builder frame = {0};
    stream_setup(&frame);
    stream_scissor(&frame, 4u, 4u, 20u, 20u, 64u, 64u);
    stream_draw_arrays(&frame, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    stream_draw_arrays(&frame, GPU_PGRAPH_OP_TRIANGLES, 3u, 3u);
    stream_scissor(&frame, 30u, 30u, 10u, 10u, 64u, 64u);
    stream_draw_arrays(&frame, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    gpu_pgraph *model = gpu_pgraph_create();
    gpu_pgraph_set_strict(model, true);
    gpu_pgraph_set_output_groups(model, GPU_PGRAPH_OUTPUT_SCISSOR);
    CHECK(gpu_pgraph_decode(model, frame.pairs, frame.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_count(model) == 3u);
    CHECK(gpu_pgraph_snapshot_count(model) == 2u);
    CHECK(gpu_pgraph_draw_at(model, 0u)->snapshot == gpu_pgraph_draw_at(model, 1u)->snapshot);
    CHECK(gpu_pgraph_draw_at(model, 2u)->snapshot != gpu_pgraph_draw_at(model, 1u)->snapshot);
    const gpu_pgraph_state *first = gpu_pgraph_snapshot(model, gpu_pgraph_draw_at(model, 0u)->snapshot);
    const gpu_pgraph_state *last = gpu_pgraph_snapshot(model, gpu_pgraph_draw_at(model, 2u)->snapshot);
    CHECK(first->output[GPU_PGRAPH_OUT_SURFACE_CLIP_HORIZONTAL] == (4u | (20u << 16)));
    CHECK(last->output[GPU_PGRAPH_OUT_SURFACE_CLIP_HORIZONTAL] == (30u | (10u << 16)));
    gpu_pgraph_destroy(model);
    stream_free(&frame);
    stream_free(&stream);

    /* CULL: its own table rows and group, independent of the scissor's */
    CHECK(gpu_pgraph_output_index(0x0308u, &group) == GPU_PGRAPH_OUT_CULL_ENABLE &&
          group == GPU_PGRAPH_OUTPUT_CULL);
    CHECK(gpu_pgraph_output_index(0x039Cu, &group) == GPU_PGRAPH_OUT_CULL_FACE &&
          group == GPU_PGRAPH_OUTPUT_CULL);
    CHECK(gpu_pgraph_output_index(0x03A0u, &group) == GPU_PGRAPH_OUT_FRONT_FACE &&
          group == GPU_PGRAPH_OUTPUT_CULL);
    CHECK(gpu_pgraph_output_index(0x03A4u, NULL) == -1); /* the third helper's word is not cull */
    CHECK(gpu_pgraph_output_index(0x0394u, NULL) == GPU_PGRAPH_OUT_CLIP_MIN && gpu_pgraph_output_index(0x0398u, NULL) == GPU_PGRAPH_OUT_CLIP_MAX); /* T462: SURFACE group, not cull */
    stream_builder cull = {0};
    stream_front_face(&cull, 0x900u);
    stream_cull_mode(&cull, 0x901u, 0x900u);
    CHECK(cull.count == 3u);
    gpu_pgraph *cull_off = gpu_pgraph_create();
    CHECK(gpu_pgraph_decode(cull_off, cull.pairs, cull.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_unhandled_count(cull_off) == 3u);
    gpu_pgraph_destroy(cull_off);
    gpu_pgraph *cull_on = gpu_pgraph_create();
    gpu_pgraph_set_strict(cull_on, true);
    gpu_pgraph_set_output_groups(cull_on, GPU_PGRAPH_OUTPUT_CULL);
    CHECK(gpu_pgraph_decode(cull_on, cull.pairs, cull.count) == GPU_PGRAPH_OK);
    const gpu_pgraph_state *cull_state = gpu_pgraph_state_now(cull_on);
    CHECK(cull_state->output_groups == GPU_PGRAPH_OUTPUT_CULL);
    CHECK(cull_state->output_written[GPU_PGRAPH_OUT_FRONT_FACE] &&
          cull_state->output[GPU_PGRAPH_OUT_FRONT_FACE] == 0x900u);
    CHECK(cull_state->output_written[GPU_PGRAPH_OUT_CULL_ENABLE] &&
          cull_state->output[GPU_PGRAPH_OUT_CULL_ENABLE] == 1u);
    CHECK(cull_state->output_written[GPU_PGRAPH_OUT_CULL_FACE] &&
          cull_state->output[GPU_PGRAPH_OUT_CULL_FACE] == 0x405u); /* 0x901 != 0x900: BACK */
    CHECK(!cull_state->output_written[GPU_PGRAPH_OUT_SURFACE_CLIP_HORIZONTAL]);
    /* the scissor's methods stay unhandled with only the cull group on, strict refuses them */
    const gpu_pgraph_command scissor_word = {0x0200u, 0x00100000u};
    CHECK(gpu_pgraph_decode(cull_on, &scissor_word, 1u) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(gpu_pgraph_error(cull_on), "0x0200") != NULL);
    /* both groups together */
    gpu_pgraph_set_output_groups(cull_on, GPU_PGRAPH_OUTPUT_ALL_MEASURED);
    CHECK(gpu_pgraph_decode(cull_on, &scissor_word, 1u) == GPU_PGRAPH_OK);
    gpu_pgraph_destroy(cull_on);
    stream_free(&cull);

    /* BLEND and ALPHA_TEST: the methods of the title's own state table, each group its own opt-in */
    static const struct {
        uint32_t method;
        int word;
        uint32_t group;
    } rows[] = {
        {0x0304u, GPU_PGRAPH_OUT_BLEND_ENABLE, GPU_PGRAPH_OUTPUT_BLEND},
        {0x0344u, GPU_PGRAPH_OUT_BLEND_SFACTOR, GPU_PGRAPH_OUTPUT_BLEND},
        {0x0348u, GPU_PGRAPH_OUT_BLEND_DFACTOR, GPU_PGRAPH_OUTPUT_BLEND},
        {0x034Cu, GPU_PGRAPH_OUT_BLEND_COLOR, GPU_PGRAPH_OUTPUT_BLEND},
        {0x0350u, GPU_PGRAPH_OUT_BLEND_EQUATION, GPU_PGRAPH_OUTPUT_BLEND},
        {0x0358u, GPU_PGRAPH_OUT_COLOR_MASK, GPU_PGRAPH_OUTPUT_BLEND},
        {0x0300u, GPU_PGRAPH_OUT_ALPHA_TEST_ENABLE, GPU_PGRAPH_OUTPUT_ALPHA_TEST},
        {0x033Cu, GPU_PGRAPH_OUT_ALPHA_FUNC, GPU_PGRAPH_OUTPUT_ALPHA_TEST},
        {0x0340u, GPU_PGRAPH_OUT_ALPHA_REF, GPU_PGRAPH_OUTPUT_ALPHA_TEST},
        {0x030Cu, GPU_PGRAPH_OUT_DEPTH_ENABLE, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL},
        {0x0354u, GPU_PGRAPH_OUT_DEPTH_FUNC, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL},
        {0x035Cu, GPU_PGRAPH_OUT_DEPTH_MASK, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL},
        {0x032Cu, GPU_PGRAPH_OUT_STENCIL_ENABLE, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL},
        {0x0360u, GPU_PGRAPH_OUT_STENCIL_MASK, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL},
        {0x0364u, GPU_PGRAPH_OUT_STENCIL_FUNC, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL},
        {0x0368u, GPU_PGRAPH_OUT_STENCIL_REF, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL},
        {0x036Cu, GPU_PGRAPH_OUT_STENCIL_FUNC_MASK, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL},
        {0x0370u, GPU_PGRAPH_OUT_STENCIL_OP_FAIL, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL},
        {0x0374u, GPU_PGRAPH_OUT_STENCIL_OP_ZFAIL, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL},
        {0x0378u, GPU_PGRAPH_OUT_STENCIL_OP_ZPASS, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL},
    };
    for (size_t i = 0u; i < sizeof rows / sizeof rows[0]; i++) {
        CHECK(gpu_pgraph_output_index(rows[i].method, &group) == rows[i].word && group == rows[i].group);
        gpu_pgraph *one = gpu_pgraph_create();
        gpu_pgraph_set_strict(one, true);
        const gpu_pgraph_command command = {rows[i].method, 0x1234u};
        CHECK(gpu_pgraph_decode(one, &command, 1u) == GPU_PGRAPH_ERR_UNMEASURED); /* off: refused */
        gpu_pgraph_set_output_groups(one, rows[i].group);
        CHECK(gpu_pgraph_decode(one, &command, 1u) == GPU_PGRAPH_OK); /* its own group: handled */
        CHECK(gpu_pgraph_state_now(one)->output_written[rows[i].word]);
        CHECK(gpu_pgraph_state_now(one)->output[rows[i].word] == 0x1234u);
        gpu_pgraph_set_output_groups(one, GPU_PGRAPH_OUTPUT_ALL_MEASURED & ~rows[i].group);
        gpu_pgraph_reset(one);
        CHECK(gpu_pgraph_decode(one, &command, 1u) == GPU_PGRAPH_ERR_UNMEASURED); /* another group: refused */
        gpu_pgraph_destroy(one);
    }
    CHECK(gpu_pgraph_output_index(0x0394u, NULL) == GPU_PGRAPH_OUT_CLIP_MIN && gpu_pgraph_output_index(0x0398u, NULL) == GPU_PGRAPH_OUT_CLIP_MAX); /* T462 */
    CHECK(gpu_pgraph_output_index(0x1D78u, NULL) == -1); /* z clamp control: not decoded (T462 decodes the clip range, 0x1D7C and 0x1D84 beside it) */
    CHECK(gpu_pgraph_output_index(0x09F4u, NULL) == -1 && gpu_pgraph_output_index(0x09FCu, NULL) == -1);
    CHECK(gpu_pgraph_output_index(0x038Cu, NULL) == GPU_PGRAPH_OUT_FRONT_POLYGON_MODE && gpu_pgraph_output_index(0x0390u, NULL) == GPU_PGRAPH_OUT_BACK_POLYGON_MODE); /* T462: FIXED group */
    /* T502: polygon offset and the IGNORED words, each in its own group */
    static const struct {
        uint32_t method;
        int word;
        uint32_t group;
    } t479_rows[] = {
        {0x0330u, GPU_PGRAPH_OUT_POLY_OFFSET_POINT, GPU_PGRAPH_OUTPUT_POLYGON_OFFSET},
        {0x0334u, GPU_PGRAPH_OUT_POLY_OFFSET_LINE, GPU_PGRAPH_OUTPUT_POLYGON_OFFSET},
        {0x0338u, GPU_PGRAPH_OUT_POLY_OFFSET_FILL, GPU_PGRAPH_OUTPUT_POLYGON_OFFSET},
        {0x0384u, GPU_PGRAPH_OUT_POLY_OFFSET_SCALE, GPU_PGRAPH_OUTPUT_POLYGON_OFFSET},
        {0x0388u, GPU_PGRAPH_OUT_POLY_OFFSET_BIAS, GPU_PGRAPH_OUTPUT_POLYGON_OFFSET},
        {0x0310u, GPU_PGRAPH_OUT_DITHER_ENABLE, GPU_PGRAPH_OUTPUT_IGNORED},
        {0x09F8u, GPU_PGRAPH_OUT_SPECULAR_PARAMS, GPU_PGRAPH_OUTPUT_IGNORED},
    };
    CHECK(sizeof t479_rows / sizeof t479_rows[0] == 7u);
    for (size_t i = 0u; i < sizeof t479_rows / sizeof t479_rows[0]; i++) {
        CHECK(gpu_pgraph_output_index(t479_rows[i].method, &group) == t479_rows[i].word &&
              group == t479_rows[i].group);
        gpu_pgraph *one = gpu_pgraph_create();
        gpu_pgraph_set_strict(one, true);
        const gpu_pgraph_command command = {t479_rows[i].method, 0x3F800000u};
        CHECK(gpu_pgraph_decode(one, &command, 1u) == GPU_PGRAPH_ERR_UNMEASURED); /* off: refused, by number */
        gpu_pgraph_reset(one);
        gpu_pgraph_set_output_groups(one, t479_rows[i].group);
        CHECK(gpu_pgraph_decode(one, &command, 1u) == GPU_PGRAPH_OK);
        CHECK(gpu_pgraph_state_now(one)->output_written[t479_rows[i].word]);
        CHECK(gpu_pgraph_state_now(one)->output[t479_rows[i].word] == 0x3F800000u);
        const bool ignored = t479_rows[i].group == GPU_PGRAPH_OUTPUT_IGNORED;
        CHECK(gpu_pgraph_get_stats(one).pairs_ignored == (ignored ? 1u : 0u)); /* counted, never silent */
        gpu_pgraph_set_output_groups(one, GPU_PGRAPH_OUTPUT_ALL_MEASURED & ~t479_rows[i].group);
        gpu_pgraph_reset(one);
        CHECK(gpu_pgraph_decode(one, &command, 1u) == GPU_PGRAPH_ERR_UNMEASURED); /* another group: refused */
        gpu_pgraph_destroy(one);
    }
    /* the whole of the title's polygon offset and specular records, in the order its state table writes them,
     * and the ZBIAS handler's five (0x0384, 0x0388, then the three enables), decode with both groups on */
    stream_builder zbias = {0};
    stream_pair(&zbias, 0x0388u, 0u);
    stream_pair(&zbias, 0x0384u, 0u);
    stream_pair(&zbias, 0x0338u, 0u);
    stream_pair(&zbias, 0x0330u, 0u);
    stream_pair(&zbias, 0x09F8u, 3u);
    stream_pair(&zbias, 0x0384u, 0x80000000u); /* ZBIAS 0: -(float)0 * 0.25 is -0.0 */
    stream_pair(&zbias, 0x0388u, 0x80000000u);
    stream_pair(&zbias, 0x0330u, 0u);
    stream_pair(&zbias, 0x0334u, 0u);
    stream_pair(&zbias, 0x0338u, 0u);
    gpu_pgraph *zbias_model = gpu_pgraph_create();
    gpu_pgraph_set_strict(zbias_model, true);
    gpu_pgraph_set_output_groups(zbias_model, GPU_PGRAPH_OUTPUT_POLYGON_OFFSET | GPU_PGRAPH_OUTPUT_IGNORED);
    CHECK(zbias.count == 10u);
    CHECK(gpu_pgraph_decode(zbias_model, zbias.pairs, zbias.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_get_stats(zbias_model).pairs_handled == 10u && gpu_pgraph_unhandled_count(zbias_model) == 0u);
    CHECK(gpu_pgraph_get_stats(zbias_model).pairs_ignored == 1u);
    CHECK(gpu_pgraph_state_now(zbias_model)->output[GPU_PGRAPH_OUT_SPECULAR_PARAMS] == 3u);
    CHECK(gpu_pgraph_state_now(zbias_model)->output[GPU_PGRAPH_OUT_POLY_OFFSET_SCALE] == 0x80000000u);
    gpu_pgraph_destroy(zbias_model);
    stream_free(&zbias);
    /* snapshots (T502): a polygon offset write between two draws splits their state, an IGNORED word does not, since no
     * draw reads it, and both are still counted in the right place */
    for (int ignored = 0; ignored < 2; ignored++) {
        stream_builder between = {0};
        stream_setup(&between);
        stream_draw_arrays(&between, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
        stream_pair(&between, ignored == 1 ? 0x09F8u : 0x0338u, 1u);
        stream_draw_arrays(&between, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
        gpu_pgraph *split = gpu_pgraph_create();
        gpu_pgraph_set_strict(split, true);
        gpu_pgraph_set_output_groups(split, GPU_PGRAPH_OUTPUT_POLYGON_OFFSET | GPU_PGRAPH_OUTPUT_IGNORED);
        CHECK(gpu_pgraph_decode(split, between.pairs, between.count) == GPU_PGRAPH_OK);
        CHECK(gpu_pgraph_draw_count(split) == 2u);
        CHECK(gpu_pgraph_snapshot_count(split) == (ignored == 1 ? 1u : 2u));
        gpu_pgraph_destroy(split);
        stream_free(&between);
    }
    /* the title's own block decodes whole, written in its own order, with every group on */
    stream_builder title = {0};
    stream_blend(&title, 1u, 0x302u, 0u, 0x8006u);
    stream_alpha_test(&title, 0u, 0x204u, 1u);
    stream_pair(&title, 0x0358u, 0x01010101u);
    gpu_pgraph *block = gpu_pgraph_create();
    gpu_pgraph_set_strict(block, true);
    gpu_pgraph_set_output_groups(block, GPU_PGRAPH_OUTPUT_BLEND | GPU_PGRAPH_OUTPUT_ALPHA_TEST);
    CHECK(gpu_pgraph_decode(block, title.pairs, title.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_get_stats(block).pairs_handled == 8u && gpu_pgraph_unhandled_count(block) == 0u);
    gpu_pgraph_destroy(block);
    stream_free(&title);

    /* CLEAR: four shadow words and one event */
    static const struct {
        uint32_t method;
        int word;
    } clear_rows[] = {
        {0x1D8Cu, GPU_PGRAPH_OUT_CLEAR_ZSTENCIL},
        {0x1D90u, GPU_PGRAPH_OUT_CLEAR_COLOR},
        {0x1D98u, GPU_PGRAPH_OUT_CLEAR_RECT_HORIZONTAL},
        {0x1D9Cu, GPU_PGRAPH_OUT_CLEAR_RECT_VERTICAL},
    };
    for (size_t i = 0u; i < sizeof clear_rows / sizeof clear_rows[0]; i++) {
        CHECK(gpu_pgraph_output_index(clear_rows[i].method, &group) == clear_rows[i].word &&
              group == GPU_PGRAPH_OUTPUT_CLEAR);
    }
    CHECK(gpu_pgraph_output_index(GPU_PGRAPH_CLEAR_SURFACE, NULL) == -1); /* the event is not a shadow word */
    CHECK(gpu_pgraph_output_index(0x1DA0u, NULL) == -1 && gpu_pgraph_output_index(0x1D78u, NULL) == -1);
    stream_builder one_clear = {0};
    stream_clear(&one_clear, 0xF1u, 0x80FF4020u, 0x12345678u, 3u, 40u, 5u, 50u);
    CHECK(one_clear.count == 5u);
    gpu_pgraph *clear_off = gpu_pgraph_create();
    gpu_pgraph_set_strict(clear_off, true);
    CHECK(gpu_pgraph_decode(clear_off, one_clear.pairs, one_clear.count) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(gpu_pgraph_error(clear_off), "pair 0 method 0x1D98") != NULL);
    gpu_pgraph_set_strict(clear_off, false);
    gpu_pgraph_reset(clear_off);
    CHECK(gpu_pgraph_decode(clear_off, one_clear.pairs, one_clear.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_unhandled_count(clear_off) == 5u && gpu_pgraph_clear_count(clear_off) == 0u);
    gpu_pgraph_destroy(clear_off);
    gpu_pgraph *clear_on = gpu_pgraph_create();
    gpu_pgraph_set_strict(clear_on, true);
    gpu_pgraph_set_output_groups(clear_on, GPU_PGRAPH_OUTPUT_CLEAR);
    CHECK(gpu_pgraph_decode(clear_on, one_clear.pairs, one_clear.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_unhandled_count(clear_on) == 0u && gpu_pgraph_get_stats(clear_on).pairs_handled == 5u);
    CHECK(gpu_pgraph_clear_count(clear_on) == 1u && gpu_pgraph_clear_at(clear_on, 1u) == NULL);
    const gpu_pgraph_clear *recorded = gpu_pgraph_clear_at(clear_on, 0u);
    CHECK(recorded != NULL && recorded->before_draw == 0u && recorded->command == 4u);
    CHECK(recorded->flags == 0xF1u && recorded->color == 0x80FF4020u && recorded->zstencil == 0x12345678u);
    CHECK(recorded->rect_horizontal == (3u | (40u << 16)) && recorded->rect_vertical == (5u | (50u << 16)));
    CHECK(recorded->zstencil_written && recorded->color_written && recorded->rect_horizontal_written &&
          recorded->rect_vertical_written);
    CHECK(gpu_pgraph_clear_at(NULL, 0u) == NULL && gpu_pgraph_clear_count(NULL) == 0u);
    /* a clear with only some words written records which */
    gpu_pgraph_reset(clear_on);
    const gpu_pgraph_command partial_clear[] = {{0x1D90u, 0xFFu}, {GPU_PGRAPH_CLEAR_SURFACE, 0x10u}};
    CHECK(gpu_pgraph_decode(clear_on, partial_clear, 2u) == GPU_PGRAPH_OK);
    recorded = gpu_pgraph_clear_at(clear_on, 0u);
    CHECK(recorded != NULL && recorded->color_written && !recorded->zstencil_written &&
          !recorded->rect_horizontal_written && !recorded->rect_vertical_written);
    gpu_pgraph_reset(clear_on);
    const gpu_pgraph_command value_only[] = {{0x1D8Cu, 0x11u}, {GPU_PGRAPH_CLEAR_SURFACE, 0x01u}};
    CHECK(gpu_pgraph_decode(clear_on, value_only, 2u) == GPU_PGRAPH_OK);
    recorded = gpu_pgraph_clear_at(clear_on, 0u);
    CHECK(recorded != NULL && recorded->zstencil_written && !recorded->color_written && recorded->zstencil == 0x11u);
    gpu_pgraph_reset(clear_on);
    const gpu_pgraph_command rect_only[] = {{0x1D98u, 0x00050001u}, {GPU_PGRAPH_CLEAR_SURFACE, 0x10u}};
    CHECK(gpu_pgraph_decode(clear_on, rect_only, 2u) == GPU_PGRAPH_OK);
    recorded = gpu_pgraph_clear_at(clear_on, 0u);
    CHECK(recorded != NULL && recorded->rect_horizontal_written && !recorded->rect_vertical_written &&
          !recorded->color_written && recorded->rect_horizontal == 0x00050001u);
    /* the event is refused inside a bracket and does not nest in it */
    gpu_pgraph_reset(clear_on);
    CHECK(gpu_pgraph_decode(clear_on, &open_bracket, 1u) == GPU_PGRAPH_OK);
    const gpu_pgraph_command in_bracket_clear = {GPU_PGRAPH_CLEAR_SURFACE, 0xF0u};
    CHECK(gpu_pgraph_decode(clear_on, &in_bracket_clear, 1u) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(gpu_pgraph_error(clear_on), "inside a BEGIN_END bracket") != NULL);
    gpu_pgraph_destroy(clear_on);
    stream_free(&one_clear);

    /* a clear is placed in the draw order: it knows how many draws came before it, begin_frame forgets it, a reset
     * forgets it too, and more than GPU_PGRAPH_MAX_CLEARS of them are refused */
    stream_builder ordered = {0};
    stream_setup(&ordered);
    stream_clear(&ordered, 0x10u, 0xFF0000FFu, 0u, 0u, 63u, 0u, 63u);
    stream_draw_arrays(&ordered, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    stream_draw_arrays(&ordered, GPU_PGRAPH_OP_TRIANGLES, 3u, 3u);
    stream_clear(&ordered, 0x20u, 0xFF00FF00u, 0u, 1u, 2u, 3u, 4u);
    stream_clear(&ordered, 0x40u, 0xFFFF0000u, 0u, 5u, 6u, 7u, 8u);
    stream_draw_arrays(&ordered, GPU_PGRAPH_OP_TRIANGLES, 6u, 3u);
    stream_clear(&ordered, 0x80u, 0x00000000u, 0u, 9u, 10u, 11u, 12u);
    gpu_pgraph *clear_model = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(clear_model, GPU_PGRAPH_OUTPUT_CLEAR);
    CHECK(gpu_pgraph_decode(clear_model, ordered.pairs, ordered.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_count(clear_model) == 3u && gpu_pgraph_clear_count(clear_model) == 4u);
    static const uint32_t before[4] = {0u, 2u, 2u, 3u};
    static const uint32_t flags_seen[4] = {0x10u, 0x20u, 0x40u, 0x80u};
    for (size_t i = 0u; i < 4u; i++) {
        const gpu_pgraph_clear *entry = gpu_pgraph_clear_at(clear_model, i);
        CHECK(entry != NULL && entry->before_draw == before[i] && entry->flags == flags_seen[i]);
        CHECK(entry != NULL && i > 0u ? entry->command > gpu_pgraph_clear_at(clear_model, i - 1u)->command : true);
    }
    CHECK(gpu_pgraph_begin_frame(clear_model) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_clear_count(clear_model) == 0u && gpu_pgraph_draw_count(clear_model) == 0u);
    CHECK(gpu_pgraph_decode(clear_model, ordered.pairs, ordered.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_clear_count(clear_model) == 4u);
    gpu_pgraph_reset(clear_model);
    CHECK(gpu_pgraph_clear_count(clear_model) == 0u);
    stream_free(&ordered);
    /* T848: a clear carries the surface words as they stood at the CLEAR_SURFACE (own target, no draw needed) */
    stream_builder surfaces = {0};
    stream_clear(&surfaces, 0x10u, 0xFF000000u, 0u, 1u, 2u, 3u, 4u);
    stream_pair(&surfaces, 0x0208u, 0x128u);
    stream_clear(&surfaces, 0x10u, 0xFF000000u, 0u, 1u, 2u, 3u, 4u); /* only the format written */
    stream_pair(&surfaces, 0x020Cu, 0x00A00A00u);
    stream_clear(&surfaces, 0x10u, 0xFF000000u, 0u, 1u, 2u, 3u, 4u); /* format and pitch */
    stream_pair(&surfaces, 0x0210u, 0x00374000u);
    stream_clear(&surfaces, 0x20u, 0xFF00FF00u, 0u, 1u, 2u, 3u, 4u);
    stream_pair(&surfaces, 0x020Cu, 64u);
    stream_pair(&surfaces, 0x0210u, 0x00500000u);
    stream_clear(&surfaces, 0x40u, 0xFFFF0000u, 0u, 1u, 2u, 3u, 4u);
    gpu_pgraph *surface_model = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(surface_model, GPU_PGRAPH_OUTPUT_CLEAR | GPU_PGRAPH_OUTPUT_SURFACE);
    CHECK(gpu_pgraph_decode(surface_model, surfaces.pairs, surfaces.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_count(surface_model) == 0u && gpu_pgraph_clear_count(surface_model) == 5u);
    /* negative controls: nothing, only the format, format and pitch written before the clear: not a named surface */
    for (size_t i = 0u; i < 3u; i++) {
        const gpu_pgraph_clear *partial = gpu_pgraph_clear_at(surface_model, i);
        CHECK(partial != NULL && !partial->surface_written);
    }
    const gpu_pgraph_clear *first_surface = gpu_pgraph_clear_at(surface_model, 3u);
    const gpu_pgraph_clear *second_surface = gpu_pgraph_clear_at(surface_model, 4u);
    CHECK(first_surface != NULL && first_surface->surface_written && first_surface->surface_format == 0x128u &&
          first_surface->surface_pitch == 0x00A00A00u && first_surface->surface_color_offset == 0x00374000u);
    CHECK(second_surface != NULL && second_surface->surface_written && second_surface->surface_format == 0x128u &&
          second_surface->surface_pitch == 64u && second_surface->surface_color_offset == 0x00500000u);
    gpu_pgraph_destroy(surface_model);
    stream_free(&surfaces);
    stream_builder many = {0};
    for (uint32_t i = 0u; i <= GPU_PGRAPH_MAX_CLEARS; i++) {
        stream_pair(&many, GPU_PGRAPH_CLEAR_SURFACE, 0x10u);
    }
    CHECK(gpu_pgraph_decode(clear_model, many.pairs, GPU_PGRAPH_MAX_CLEARS) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_clear_count(clear_model) == GPU_PGRAPH_MAX_CLEARS);
    CHECK(gpu_pgraph_decode(clear_model, many.pairs + GPU_PGRAPH_MAX_CLEARS, 1u) == GPU_PGRAPH_ERR_FULL);
    CHECK(strstr(gpu_pgraph_error(clear_model), "the clear list is full") != NULL);
    stream_free(&many);
    gpu_pgraph_destroy(clear_model);
}

/* --- 2. resolve --------------------------------------------------------------------------- */

static gpu_pgraph_state decoded_state(const stream_builder *stream, uint32_t groups)
{
    gpu_pgraph *model = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(model, groups);
    CHECK(gpu_pgraph_decode(model, stream->pairs, stream->count) == GPU_PGRAPH_OK);
    const gpu_pgraph_state state = *gpu_pgraph_state_now(model);
    gpu_pgraph_destroy(model);
    return state;
}

static void expect_refused(const gpu_pgraph_state *state, const gpu_pgraph_backend *backend,
                           const char *needle)
{
    gpu_pgraph_output output;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    const gpu_pgraph_result result =
        gpu_pgraph_resolve_output(state, backend, WIDTH, HEIGHT, &output, &report);
    CHECK(result == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, needle) != NULL);
    if (strstr(report.error, needle) == NULL) {
        printf("    refusal was: %s\n", report.error);
    }
    CHECK(!output.active);
}

static void test_resolve(void)
{
    printf("test_resolve\n");
    fake_guest guest = {MEMORY_BASE, memory, sizeof memory};
    gpu_pgraph_output output;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);

    /* nothing written: inactive, no inference, with the group on or off */
    gpu_pgraph_state empty;
    memset(&empty, 0, sizeof empty);
    gpu_pgraph_backend on = make_backend(&guest, GPU_PGRAPH_OUTPUT_SCISSOR, GPU_PGRAPH_INFER_OUTPUT_ALL);
    CHECK(gpu_pgraph_resolve_output(&empty, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(!output.active && output.used_inferences == 0u);

    /* a smaller rectangle: active, the numbers, the one inference */
    stream_builder stream = {0};
    stream_scissor(&stream, 8u, 16u, 30u, 20u, WIDTH, HEIGHT);
    gpu_pgraph_state state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_SCISSOR);
    CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(output.active && output.output.scissor);
    CHECK(output.output.scissor_x == 8u && output.output.scissor_y == 16u);
    CHECK(output.output.scissor_width == 30u && output.output.scissor_height == 20u);
    CHECK(output.used_inferences == GPU_PGRAPH_INFER_OUTPUT_SCISSOR);
    /* flip_y: the rows of the finished image, 64 - (16 + 20) = 28 */
    gpu_pgraph_backend flipped = on;
    flipped.flip_y = true;
    CHECK(gpu_pgraph_resolve_output(&state, &flipped, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(output.output.scissor_y == 28u && output.output.scissor_x == 8u);
    /* the inference is a gate: without its bit the draw is refused, naming it */
    gpu_pgraph_backend strict_inference = make_backend(&guest, GPU_PGRAPH_OUTPUT_SCISSOR, 0u);
    strict_inference.allowed_inferences = GPU_PGRAPH_INFER_ALL;
    expect_refused(&state, &strict_inference, "INFERRED and not allowed");
    /* the group not enabled in the backend while the stream wrote it: refused, never ignored */
    gpu_pgraph_backend not_enabled = make_backend(&guest, 0u, GPU_PGRAPH_INFER_OUTPUT_ALL);
    expect_refused(&state, &not_enabled, "the stream set scissor state");

    /* a rectangle that is the whole target depends on nothing: inactive, no inference, even when the
     * inference is not allowed */
    stream_free(&stream);
    stream_scissor(&stream, 0u, 0u, WIDTH, HEIGHT, WIDTH, HEIGHT);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_SCISSOR);
    CHECK(gpu_pgraph_resolve_output(&state, &strict_inference, WIDTH, HEIGHT, &output, &report) ==
          GPU_PGRAPH_OK);
    CHECK(!output.active && output.used_inferences == 0u);
    /* the same words on a bigger target: the window clip (max 64) no longer covers it */
    CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH * 2u, HEIGHT, &output, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    stream_free(&stream);

    /* a rectangle past the target */
    stream_scissor(&stream, 40u, 0u, 30u, 10u, WIDTH, HEIGHT);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_SCISSOR);
    expect_refused(&state, &on, "lies outside the 64x64 target");
    stream_free(&stream);
    stream_scissor(&stream, 0u, 60u, 10u, 5u, WIDTH, HEIGHT);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_SCISSOR);
    expect_refused(&state, &on, "lies outside the 64x64 target");
    stream_free(&stream);

    /* origins and sizes past 8 bits, on a target big enough to hold them: all 16 bits of each field count */
    stream_scissor(&stream, 300u, 260u, 340u, 220u, 640u, 480u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_SCISSOR);
    CHECK(gpu_pgraph_resolve_output(&state, &on, 640u, 480u, &output, &report) == GPU_PGRAPH_OK);
    CHECK(output.active && output.output.scissor_x == 300u && output.output.scissor_y == 260u);
    CHECK(output.output.scissor_width == 340u && output.output.scissor_height == 220u);
    stream_free(&stream);

    /* a rectangle that touches the far edges exactly is fine */
    stream_scissor(&stream, 34u, 44u, 30u, 20u, WIDTH, HEIGHT);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_SCISSOR);
    CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(output.active && output.output.scissor_x == 34u && output.output.scissor_y == 44u);
    stream_free(&stream);

    /* only one of the pair */
    stream_pair(&stream, 0x0200u, 10u | (10u << 16));
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_SCISSOR);
    expect_refused(&state, &on, "only one of the surface clip words");
    stream_free(&stream);
    stream_pair(&stream, 0x0204u, 10u | (10u << 16));
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_SCISSOR);
    expect_refused(&state, &on, "only one of the surface clip words");
    stream_free(&stream);

    /* window clip: a type other than 0, a window that starts late or ends early, each refused */
    stream_scissor(&stream, 8u, 16u, 30u, 20u, WIDTH, HEIGHT);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_SCISSOR);
    gpu_pgraph_state bad = state;
    bad.output[GPU_PGRAPH_OUT_WINDOW_CLIP_TYPE] = 1u;
    expect_refused(&bad, &on, "window clip type 0x1 is not measured");
    bad = state;
    bad.output[GPU_PGRAPH_OUT_WINDOW_CLIP_HORIZONTAL] = 4u | (WIDTH << 16);
    expect_refused(&bad, &on, "window clip horizontal");
    bad = state;
    bad.output[GPU_PGRAPH_OUT_WINDOW_CLIP_HORIZONTAL] = (WIDTH - 1u) << 16;
    expect_refused(&bad, &on, "window clip horizontal");
    bad = state;
    bad.output[GPU_PGRAPH_OUT_WINDOW_CLIP_VERTICAL] = 2u | (HEIGHT << 16);
    expect_refused(&bad, &on, "window clip vertical");
    bad = state;
    bad.output[GPU_PGRAPH_OUT_WINDOW_CLIP_VERTICAL] = (HEIGHT - 1u) << 16;
    expect_refused(&bad, &on, "window clip vertical");
    /* a window exactly the target (max == size) is accepted */
    bad = state;
    bad.output[GPU_PGRAPH_OUT_WINDOW_CLIP_HORIZONTAL] = WIDTH << 16;
    bad.output[GPU_PGRAPH_OUT_WINDOW_CLIP_VERTICAL] = HEIGHT << 16;
    CHECK(gpu_pgraph_resolve_output(&bad, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(output.active);
    /* window clip words alone (no surface clip) are checked too, and a covering one changes nothing */
    stream_free(&stream);
    stream_pair(&stream, 0x02C0u, 8u | (WIDTH << 16));
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_SCISSOR);
    expect_refused(&state, &on, "window clip horizontal");
    stream_free(&stream);
    stream_pair(&stream, 0x02C0u, WIDTH << 16);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_SCISSOR);
    CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(!output.active);
    stream_free(&stream);
}

static void test_resolve_cull(void)
{
    printf("test_resolve_cull\n");
    fake_guest guest = {MEMORY_BASE, memory, sizeof memory};
    gpu_pgraph_output output;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    gpu_pgraph_backend on = make_backend(&guest, GPU_PGRAPH_OUTPUT_CULL, GPU_PGRAPH_INFER_OUTPUT_ALL);
    gpu_pgraph_backend no_inference = make_backend(&guest, GPU_PGRAPH_OUTPUT_CULL, 0u);
    no_inference.allowed_inferences = GPU_PGRAPH_INFER_ALL;
    gpu_pgraph_backend flipped = on;
    flipped.flip_y = true;
    gpu_pgraph_backend not_enabled = make_backend(&guest, 0u, GPU_PGRAPH_INFER_OUTPUT_ALL);
    gpu_pgraph_state state;

    /* nothing written: inactive, no inference */
    memset(&state, 0, sizeof state);
    CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(!output.active && output.used_inferences == 0u);

    /* culling off needs nothing else and no inference, whatever the other words are */
    stream_builder stream = {0};
    stream_cull_mode(&stream, 0u, 0x900u);
    CHECK(stream.count == 1u && stream.pairs[0].method == 0x0308u && stream.pairs[0].data == 0u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_CULL);
    CHECK(gpu_pgraph_resolve_output(&state, &no_inference, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(!output.active && output.used_inferences == 0u);
    state.output[GPU_PGRAPH_OUT_CULL_FACE] = 0x123u; /* not written, so never looked at */
    CHECK(gpu_pgraph_resolve_output(&state, &no_inference, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(!output.active);
    stream_free(&stream);

    /* the four live combinations: face from the emitter's own arithmetic, winding from the front face */
    static const struct {
        uint32_t mode, front, face_word, cull_mode;
        bool clockwise, clockwise_flipped;
    } live[] = {
        /* T1227: NV2A CW (0x900) is Vulkan COUNTER_CLOCKWISE on the unmirrored finished image (xemu-level, HQ49), CW under flip_y */
        {0x900u, 0x900u, 0x404u, GPU_VSH_CULL_FRONT, false, true},  /* cull the CW front faces */
        {0x901u, 0x900u, 0x405u, GPU_VSH_CULL_BACK, false, true},   /* cull the CCW back faces */
        {0x901u, 0x901u, 0x404u, GPU_VSH_CULL_FRONT, true, false},
        {0x900u, 0x901u, 0x405u, GPU_VSH_CULL_BACK, true, false},
    };
    for (size_t i = 0u; i < sizeof live / sizeof live[0]; i++) {
        stream_front_face(&stream, live[i].front);
        stream_cull_mode(&stream, live[i].mode, live[i].front);
        CHECK(stream.count == 3u && stream.pairs[2].method == 0x039Cu &&
              stream.pairs[2].data == live[i].face_word);
        state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_CULL);
        CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
        CHECK(output.active && output.output.cull_mode == live[i].cull_mode);
        CHECK(output.output.front_clockwise == live[i].clockwise);
        CHECK(output.used_inferences == GPU_PGRAPH_INFER_OUTPUT_CULL_WINDING);
        CHECK(!output.output.scissor);
        CHECK(gpu_pgraph_resolve_output(&state, &flipped, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
        CHECK(output.active && output.output.cull_mode == live[i].cull_mode);
        CHECK(output.output.front_clockwise == live[i].clockwise_flipped);
        expect_refused(&state, &no_inference, "INFERRED and not allowed");
        expect_refused(&state, &not_enabled, "the stream set cull state");
        stream_free(&stream);
    }

    /* refusals, each by its message */
    stream_pair(&stream, 0x039Cu, 0x404u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_CULL);
    expect_refused(&state, &on, "never the enable");
    stream_free(&stream);
    stream_pair(&stream, 0x0308u, 2u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_CULL);
    expect_refused(&state, &on, "neither 0 nor 1");
    stream_free(&stream);
    stream_pair(&stream, 0x0308u, 1u);
    stream_pair(&stream, 0x03A0u, 0x900u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_CULL);
    expect_refused(&state, &on, "the cull face (0x039C) was never written");
    stream_free(&stream);
    stream_pair(&stream, 0x0308u, 1u);
    stream_pair(&stream, 0x039Cu, 0x404u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_CULL);
    expect_refused(&state, &on, "the front face (0x03A0) was never written");
    stream_free(&stream);
    static const uint32_t bad_faces[] = {0x408u, 0x403u, 0x406u, 0u, 0x900u};
    for (size_t i = 0u; i < sizeof bad_faces / sizeof bad_faces[0]; i++) {
        stream_pair(&stream, 0x03A0u, 0x900u);
        stream_pair(&stream, 0x0308u, 1u);
        stream_pair(&stream, 0x039Cu, bad_faces[i]);
        state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_CULL);
        expect_refused(&state, &on, "is not 0x404 (front) or 0x405 (back)");
        stream_free(&stream);
    }
    static const uint32_t bad_fronts[] = {0x902u, 0x8FFu, 0u, 1u, 0x404u};
    for (size_t i = 0u; i < sizeof bad_fronts / sizeof bad_fronts[0]; i++) {
        stream_pair(&stream, 0x03A0u, bad_fronts[i]);
        stream_pair(&stream, 0x0308u, 1u);
        stream_pair(&stream, 0x039Cu, 0x405u);
        state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_CULL);
        expect_refused(&state, &on, "is not 0x900 (CW) or 0x901 (CCW)");
        stream_free(&stream);
    }

    /* the groups are independent: a scissor in the same state does not need the cull group and does not
     * disturb it */
    stream_scissor(&stream, 8u, 16u, 30u, 20u, WIDTH, HEIGHT);
    stream_front_face(&stream, 0x900u);
    stream_cull_mode(&stream, 0x901u, 0x900u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_ALL_MEASURED);
    gpu_pgraph_backend both = make_backend(&guest, GPU_PGRAPH_OUTPUT_ALL_MEASURED, GPU_PGRAPH_INFER_OUTPUT_ALL);
    CHECK(gpu_pgraph_resolve_output(&state, &both, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(output.active && output.output.scissor && output.output.scissor_width == 30u);
    CHECK(output.output.cull_mode == GPU_VSH_CULL_BACK && !output.output.front_clockwise);
    CHECK(output.used_inferences == (GPU_PGRAPH_INFER_OUTPUT_SCISSOR | GPU_PGRAPH_INFER_OUTPUT_CULL_WINDING));
    gpu_pgraph_backend scissor_only = make_backend(&guest, GPU_PGRAPH_OUTPUT_SCISSOR, GPU_PGRAPH_INFER_OUTPUT_ALL);
    expect_refused(&state, &scissor_only, "the stream set cull state");
    gpu_pgraph_backend cull_only = make_backend(&guest, GPU_PGRAPH_OUTPUT_CULL, GPU_PGRAPH_INFER_OUTPUT_ALL);
    expect_refused(&state, &cull_only, "the stream set scissor state");
    stream_free(&stream);
}

static void test_resolve_blend(void)
{
    printf("test_resolve_blend\n");
    fake_guest guest = {MEMORY_BASE, memory, sizeof memory};
    gpu_pgraph_output output;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    gpu_pgraph_backend on = make_backend(&guest, GPU_PGRAPH_OUTPUT_BLEND, GPU_PGRAPH_INFER_OUTPUT_ALL);
    gpu_pgraph_backend no_inference = make_backend(&guest, GPU_PGRAPH_OUTPUT_BLEND, 0u);
    no_inference.allowed_inferences = GPU_PGRAPH_INFER_ALL;
    gpu_pgraph_backend not_enabled = make_backend(&guest, 0u, GPU_PGRAPH_INFER_OUTPUT_ALL);
    gpu_pgraph_state state;
    stream_builder stream = {0};

    /* nothing written, or blending off and the mask at its full value: inactive, no inference */
    memset(&state, 0, sizeof state);
    CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(!output.active && !output.needs_destination && output.used_inferences == 0u);
    stream_blend(&stream, 0u, 0x302u, 0x303u, 0x8006u);
    stream_pair(&stream, 0x0358u, 0x01010101u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_BLEND);
    CHECK(gpu_pgraph_resolve_output(&state, &no_inference, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(!output.active && !output.needs_destination && output.used_inferences == 0u);
    stream_free(&stream);

    /* every factor of the headers maps to its own enum, and the other values are refused */
    static const struct {
        uint32_t nv, ours;
        bool constant;
    } factors[] = {
        {0x0000u, GPU_VSH_BLEND_ZERO, false},
        {0x0001u, GPU_VSH_BLEND_ONE, false},
        {0x0300u, GPU_VSH_BLEND_SRC_COLOR, false},
        {0x0301u, GPU_VSH_BLEND_ONE_MINUS_SRC_COLOR, false},
        {0x0302u, GPU_VSH_BLEND_SRC_ALPHA, false},
        {0x0303u, GPU_VSH_BLEND_ONE_MINUS_SRC_ALPHA, false},
        {0x0304u, GPU_VSH_BLEND_DST_ALPHA, false},
        {0x0305u, GPU_VSH_BLEND_ONE_MINUS_DST_ALPHA, false},
        {0x0306u, GPU_VSH_BLEND_DST_COLOR, false},
        {0x0307u, GPU_VSH_BLEND_ONE_MINUS_DST_COLOR, false},
        {0x0308u, GPU_VSH_BLEND_SRC_ALPHA_SATURATE, false},
        {0x8001u, GPU_VSH_BLEND_CONSTANT_COLOR, true},
        {0x8002u, GPU_VSH_BLEND_ONE_MINUS_CONSTANT_COLOR, true},
        {0x8003u, GPU_VSH_BLEND_CONSTANT_ALPHA, true},
        {0x8004u, GPU_VSH_BLEND_ONE_MINUS_CONSTANT_ALPHA, true},
    };
    CHECK(sizeof factors / sizeof factors[0] == GPU_VSH_BLEND_FACTORS);
    for (size_t i = 0u; i < sizeof factors / sizeof factors[0]; i++) {
        CHECK(factors[i].ours == i); /* the table is in enum order, so no enum value is missed */
        for (int side = 0; side < 2; side++) {
            stream_blend(&stream, 1u, side == 0 ? factors[i].nv : 0x0001u, side == 1 ? factors[i].nv : 0x0000u,
                         0x8006u);
            stream_pair(&stream, 0x034Cu, 0x80FF8000u);
            state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_BLEND);
            CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
            CHECK(output.active && output.needs_destination && output.output.blend);
            CHECK(output.output.blend_source == (side == 0 ? factors[i].ours : GPU_VSH_BLEND_ONE));
            CHECK(output.output.blend_destination == (side == 1 ? factors[i].ours : GPU_VSH_BLEND_ZERO));
            CHECK(output.output.blend_equation == GPU_VSH_BLEND_OP_ADD);
            CHECK(output.used_inferences == GPU_PGRAPH_INFER_OUTPUT_BLEND_MODEL);
            stream_free(&stream);
        }
    }
    static const uint32_t bad_factors[] = {0x0002u, 0x02FFu, 0x0309u, 0x8000u, 0x8005u, 0x0401u, 0xFFFFu};
    for (size_t i = 0u; i < sizeof bad_factors / sizeof bad_factors[0]; i++) {
        stream_blend(&stream, 1u, bad_factors[i], 0u, 0x8006u);
        state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_BLEND);
        expect_refused(&state, &on, "blend source factor");
        stream_free(&stream);
        stream_blend(&stream, 1u, 0u, bad_factors[i], 0x8006u);
        state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_BLEND);
        expect_refused(&state, &on, "blend destination factor");
        stream_free(&stream);
    }

    /* the equations, and the ones that are refused */
    static const struct {
        uint32_t nv, ours;
    } equations[] = {
        {0x8006u, GPU_VSH_BLEND_OP_ADD},
        {0x800Au, GPU_VSH_BLEND_OP_SUBTRACT},
        {0x800Bu, GPU_VSH_BLEND_OP_REVERSE_SUBTRACT},
        {0x8007u, GPU_VSH_BLEND_OP_MIN},
        {0x8008u, GPU_VSH_BLEND_OP_MAX},
    };
    CHECK(sizeof equations / sizeof equations[0] == GPU_VSH_BLEND_OPS);
    for (size_t i = 0u; i < sizeof equations / sizeof equations[0]; i++) {
        stream_blend(&stream, 1u, 1u, 1u, equations[i].nv);
        state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_BLEND);
        CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
        CHECK(output.output.blend_equation == equations[i].ours);
        stream_free(&stream);
    }
    static const uint32_t bad_equations[] = {0xF005u, 0xF006u, 0x8009u, 0x800Cu, 0u, 1u};
    for (size_t i = 0u; i < sizeof bad_equations / sizeof bad_equations[0]; i++) {
        stream_blend(&stream, 1u, 1u, 1u, bad_equations[i]);
        state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_BLEND);
        expect_refused(&state, &on, "blend equation");
        stream_free(&stream);
    }

    /* the blend colour is ARGB: A in the top byte */
    stream_blend(&stream, 1u, 0x8001u, 0u, 0x8006u);
    stream_pair(&stream, 0x034Cu, 0x80FF4000u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_BLEND);
    CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(output.output.blend_constant[0] == 255.0f / 255.0f);
    CHECK(output.output.blend_constant[1] == 64.0f / 255.0f);
    CHECK(output.output.blend_constant[2] == 0.0f);
    CHECK(output.output.blend_constant[3] == 128.0f / 255.0f);
    stream_free(&stream);

    /* refusals: the enable, what an enabled blend needs, the colour a constant factor needs */
    static const uint32_t unenabled_words[] = {0x0344u, 0x0348u, 0x0350u, 0x034Cu};
    for (size_t i = 0u; i < sizeof unenabled_words / sizeof unenabled_words[0]; i++) {
        stream_pair(&stream, unenabled_words[i], 1u); /* each word alone, with no enable */
        state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_BLEND);
        expect_refused(&state, &on, "never the blend enable");
        stream_free(&stream);
    }
    stream_pair(&stream, 0x0304u, 2u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_BLEND);
    expect_refused(&state, &on, "neither 0 nor 1");
    stream_free(&stream);
    stream_pair(&stream, 0x0304u, 1u);
    stream_pair(&stream, 0x0348u, 0u);
    stream_pair(&stream, 0x0350u, 0x8006u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_BLEND);
    expect_refused(&state, &on, "source factor (0x0344) was never written");
    stream_free(&stream);
    stream_pair(&stream, 0x0304u, 1u);
    stream_pair(&stream, 0x0344u, 1u);
    stream_pair(&stream, 0x0350u, 0x8006u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_BLEND);
    expect_refused(&state, &on, "destination factor (0x0348) was never written");
    stream_free(&stream);
    stream_pair(&stream, 0x0304u, 1u);
    stream_pair(&stream, 0x0344u, 1u);
    stream_pair(&stream, 0x0348u, 0u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_BLEND);
    expect_refused(&state, &on, "equation (0x0350) was never written");
    stream_free(&stream);
    for (size_t i = 0u; i < sizeof factors / sizeof factors[0]; i++) {
        for (int side = 0; side < 2; side++) {
            stream_blend(&stream, 1u, side == 0 ? factors[i].nv : 0u, side == 1 ? factors[i].nv : 0u, 0x8006u);
            state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_BLEND);
            if (factors[i].constant) {
                expect_refused(&state, &on, "blend colour (0x034C) was never written");
            } else {
                CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
            }
            stream_free(&stream);
        }
    }

    /* the gates: the inference bit and the group */
    stream_blend(&stream, 1u, 0x302u, 0x303u, 0x8006u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_BLEND);
    expect_refused(&state, &no_inference, "INFERRED and not allowed");
    expect_refused(&state, &not_enabled, "the stream set blend state");
    stream_free(&stream);

    /* the colour mask: the four channels, the bits that are not channels, and the full mask needs nothing */
    static const struct {
        uint32_t mask, disabled;
    } masks[] = {
        {0x00000000u, GPU_VSH_CHANNEL_R | GPU_VSH_CHANNEL_G | GPU_VSH_CHANNEL_B | GPU_VSH_CHANNEL_A},
        {0x01010100u, GPU_VSH_CHANNEL_B},
        {0x01010001u, GPU_VSH_CHANNEL_G},
        {0x01000101u, GPU_VSH_CHANNEL_R},
        {0x00010101u, GPU_VSH_CHANNEL_A},
        {0x01010000u, GPU_VSH_CHANNEL_G | GPU_VSH_CHANNEL_B},
        {0x00000001u, GPU_VSH_CHANNEL_R | GPU_VSH_CHANNEL_G | GPU_VSH_CHANNEL_A},
    };
    for (size_t i = 0u; i < sizeof masks / sizeof masks[0]; i++) {
        stream_pair(&stream, 0x0358u, masks[i].mask);
        state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_BLEND);
        CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
        CHECK(output.active && output.needs_destination && !output.output.blend);
        CHECK(output.output.color_write_disable == masks[i].disabled);
        CHECK(output.used_inferences == GPU_PGRAPH_INFER_OUTPUT_BLEND_MODEL);
        expect_refused(&state, &no_inference, "INFERRED and not allowed");
        stream_free(&stream);
    }
    static const uint32_t bad_masks[] = {0x01010102u, 0x02000000u, 0x00000010u, 0xFFFFFFFFu, 0x00F00000u};
    for (size_t i = 0u; i < sizeof bad_masks / sizeof bad_masks[0]; i++) {
        stream_pair(&stream, 0x0358u, bad_masks[i]);
        state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_BLEND);
        expect_refused(&state, &on, "beyond the four channel enables");
        stream_free(&stream);
    }
    /* mask and blend together carry both */
    stream_blend(&stream, 1u, 1u, 1u, 0x8006u);
    stream_pair(&stream, 0x0358u, 0x01010100u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_BLEND);
    CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(output.output.blend && output.output.color_write_disable == GPU_VSH_CHANNEL_B);
    stream_free(&stream);
}

/* T860/T885: the front and back polygon mode words; CreateDevice seeds both shadows to FILL. */
static void test_resolve_polygon_mode(void)
{
    printf("test_resolve_polygon_mode\n");
    fake_guest guest = {MEMORY_BASE, memory, sizeof memory};
    gpu_pgraph_output output;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    const uint32_t groups = GPU_PGRAPH_OUTPUT_FIXED | GPU_PGRAPH_OUTPUT_POLYGON_OFFSET;
    gpu_pgraph_backend on = make_backend(&guest, groups, GPU_PGRAPH_INFER_OUTPUT_ALL);
    static const struct {
        uint32_t value, mode;
    } accepted[] = {{0x1B02u, GPU_VSH_POLYGON_FILL}, {0x1B01u, GPU_VSH_POLYGON_LINE}, {0x1B00u, GPU_VSH_POLYGON_POINT}};
    for (size_t i = 0u; i < sizeof accepted / sizeof accepted[0]; i++) {
        stream_builder stream = {0};
        stream_pair(&stream, 0x038Cu, accepted[i].value);
        stream_pair(&stream, 0x0390u, accepted[i].value);
        gpu_pgraph_state state = decoded_state(&stream, groups);
        CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
        CHECK(output.output.polygon_mode == accepted[i].mode);
        CHECK(output.active == (accepted[i].mode != GPU_VSH_POLYGON_FILL));
        stream_free(&stream);
    }
    /* A written zero is not a valid NV097 polygon-mode encoding and stays refused even when
     * every declared output inference is enabled. */
    stream_builder zero = {0};
    stream_pair(&zero, 0x038Cu, 0u);
    stream_pair(&zero, 0x0390u, 0u);
    gpu_pgraph_state zero_state = decoded_state(&zero, groups);
    expect_refused(&zero_state, &on, "none of POINT 0x1B00");
    stream_free(&zero);
    /* a value that is none of the four, and a differing pair (xemu asserts on both) */
    stream_builder odd = {0};
    stream_pair(&odd, 0x038Cu, 0x1B03u);
    gpu_pgraph_state odd_state = decoded_state(&odd, groups);
    expect_refused(&odd_state, &on, "none of POINT 0x1B00");
    stream_free(&odd);
    stream_builder differ = {0};
    stream_pair(&differ, 0x038Cu, 0x1B01u);
    stream_pair(&differ, 0x0390u, 0x1B02u);
    gpu_pgraph_state differ_state = decoded_state(&differ, groups);
    expect_refused(&differ_state, &on, "differ");
    stream_free(&differ);
    /* the polygon offset enable of the mode decides: line enable on, fill enable off, scale and bias set */
    stream_builder offset = {0};
    stream_pair(&offset, 0x038Cu, 0x1B01u);
    stream_pair(&offset, 0x0390u, 0x1B01u);
    stream_pair(&offset, 0x0330u, 0u);
    stream_pair(&offset, 0x0334u, 1u);
    stream_pair(&offset, 0x0338u, 0u);
    stream_pair(&offset, 0x0384u, 0x3F800000u);
    stream_pair(&offset, 0x0388u, 0x40000000u);
    gpu_pgraph_state offset_state = decoded_state(&offset, groups | GPU_PGRAPH_OUTPUT_DEPTH_STENCIL);
    gpu_pgraph_backend with_depth = make_backend(&guest, groups | GPU_PGRAPH_OUTPUT_DEPTH_STENCIL, GPU_PGRAPH_INFER_OUTPUT_ALL);
    CHECK(gpu_pgraph_resolve_output(&offset_state, &with_depth, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(output.output.polygon_mode == GPU_VSH_POLYGON_LINE);
    CHECK((output.used_inferences & GPU_PGRAPH_INFER_OUTPUT_POLYGON_OFFSET_MODEL) != 0u);
    stream_free(&offset);
}

static void test_resolve_alpha_test(void)
{
    printf("test_resolve_alpha_test\n");
    fake_guest guest = {MEMORY_BASE, memory, sizeof memory};
    gpu_pgraph_output output;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    gpu_pgraph_backend on = make_backend(&guest, GPU_PGRAPH_OUTPUT_ALPHA_TEST, GPU_PGRAPH_INFER_OUTPUT_ALL);
    gpu_pgraph_backend no_inference = make_backend(&guest, GPU_PGRAPH_OUTPUT_ALPHA_TEST, 0u);
    no_inference.allowed_inferences = GPU_PGRAPH_INFER_ALL;
    gpu_pgraph_backend not_enabled = make_backend(&guest, 0u, GPU_PGRAPH_INFER_OUTPUT_ALL);
    gpu_pgraph_backend with_combiner = on;
    with_combiner.combiner = true;
    gpu_pgraph_state state;
    stream_builder stream = {0};

    memset(&state, 0, sizeof state);
    CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(!output.active && output.used_inferences == 0u);
    /* the title's own record: disabled, GREATER, 1. Nothing depends on it */
    stream_alpha_test(&stream, 0u, 0x204u, 1u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_ALPHA_TEST);
    CHECK(gpu_pgraph_resolve_output(&state, &no_inference, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(!output.active && output.used_inferences == 0u);
    stream_free(&stream);

    /* each function maps to 0x200 + n, the reference passes through, and the gate names its bit */
    for (uint32_t function = 0x0200u; function <= 0x0206u; function++) {
        stream_alpha_test(&stream, 1u, function, 200u);
        state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_ALPHA_TEST);
        CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
        CHECK(output.active && output.output.alpha_test && !output.needs_destination);
        CHECK(output.output.alpha_func == function - 0x0200u && output.output.alpha_ref == 200u);
        CHECK(output.used_inferences == GPU_PGRAPH_INFER_OUTPUT_ALPHA_TEST_MODEL);
        expect_refused(&state, &no_inference, "INFERRED and not allowed");
        expect_refused(&state, &not_enabled, "the stream set alpha test state");
        stream_free(&stream);
    }
    /* ALWAYS passes every fragment and depends on nothing, whatever the reference */
    stream_alpha_test(&stream, 1u, 0x0207u, 77u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_ALPHA_TEST);
    CHECK(gpu_pgraph_resolve_output(&state, &no_inference, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(!output.active && output.used_inferences == 0u);
    stream_free(&stream);
    /* the byte limits of the reference */
    stream_alpha_test(&stream, 1u, 0x0204u, 255u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_ALPHA_TEST);
    CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(output.output.alpha_ref == 255u);
    stream_free(&stream);
    stream_alpha_test(&stream, 1u, 0x0204u, 0u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_ALPHA_TEST);
    CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(output.active && output.output.alpha_ref == 0u);
    stream_free(&stream);
    stream_alpha_test(&stream, 1u, 0x0204u, 256u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_ALPHA_TEST);
    expect_refused(&state, &on, "above 255");
    stream_free(&stream);

    static const uint32_t bad_functions[] = {0x01FFu, 0x0208u, 0u, 0x0204u | 0x10000u};
    for (size_t i = 0u; i < sizeof bad_functions / sizeof bad_functions[0]; i++) {
        stream_alpha_test(&stream, 1u, bad_functions[i], 1u);
        state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_ALPHA_TEST);
        expect_refused(&state, &on, "is not one of 0x200 (NEVER) to 0x207 (ALWAYS)");
        stream_free(&stream);
    }
    stream_pair(&stream, 0x033Cu, 0x204u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_ALPHA_TEST);
    expect_refused(&state, &on, "never the alpha test enable");
    stream_free(&stream);
    stream_pair(&stream, 0x0340u, 1u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_ALPHA_TEST);
    expect_refused(&state, &on, "never the alpha test enable");
    stream_free(&stream);
    stream_pair(&stream, 0x0300u, 2u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_ALPHA_TEST);
    expect_refused(&state, &on, "neither 0 nor 1");
    stream_free(&stream);
    stream_pair(&stream, 0x0300u, 1u);
    stream_pair(&stream, 0x0340u, 1u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_ALPHA_TEST);
    expect_refused(&state, &on, "function (0x033C) was never written");
    stream_free(&stream);
    stream_pair(&stream, 0x0300u, 1u);
    stream_pair(&stream, 0x033Cu, 0x204u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_ALPHA_TEST);
    expect_refused(&state, &on, "reference (0x0340) was never written");
    stream_free(&stream);
    /* T860: with the combiner stage the module carries the test (resolve_fragment), so the output carries none of it, but the
     * INFERRED test model is still named, and the same invalid words are still refused */
    stream_alpha_test(&stream, 1u, 0x0204u, 1u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_ALPHA_TEST);
    CHECK(gpu_pgraph_resolve_output(&state, &with_combiner, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(!output.output.alpha_test && output.used_inferences == GPU_PGRAPH_INFER_OUTPUT_ALPHA_TEST_MODEL);
    gpu_pgraph_backend combiner_no_inference = no_inference;
    combiner_no_inference.combiner = true;
    expect_refused(&state, &combiner_no_inference, "INFERRED and not allowed");
    stream_free(&stream);
    stream_alpha_test(&stream, 1u, 0x0208u, 1u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_ALPHA_TEST);
    expect_refused(&state, &with_combiner, "alpha function 0x208");
    stream_free(&stream);
    /* the three groups resolve together */
    stream_blend(&stream, 1u, 0x302u, 0x303u, 0x8006u);
    stream_alpha_test(&stream, 1u, 0x0204u, 10u);
    stream_cull_mode(&stream, 0u, 0x900u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_ALL_MEASURED);
    gpu_pgraph_backend all = make_backend(&guest, GPU_PGRAPH_OUTPUT_ALL_MEASURED, GPU_PGRAPH_INFER_OUTPUT_ALL);
    CHECK(gpu_pgraph_resolve_output(&state, &all, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(output.output.blend && output.output.alpha_test && output.needs_destination);
    CHECK(output.used_inferences ==
          (GPU_PGRAPH_INFER_OUTPUT_BLEND_MODEL | GPU_PGRAPH_INFER_OUTPUT_ALPHA_TEST_MODEL));
    stream_free(&stream);
}

static void test_resolve_depth_stencil(void)
{
    printf("test_resolve_depth_stencil\n");
    fake_guest guest = {MEMORY_BASE, memory, sizeof memory};
    gpu_pgraph_output output;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    gpu_pgraph_backend on = make_backend(&guest, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL, GPU_PGRAPH_INFER_OUTPUT_ALL);
    gpu_pgraph_backend no_inference = make_backend(&guest, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL, 0u);
    no_inference.allowed_inferences = GPU_PGRAPH_INFER_ALL;
    gpu_pgraph_backend not_enabled = make_backend(&guest, 0u, GPU_PGRAPH_INFER_OUTPUT_ALL);
    gpu_pgraph_state state;
    stream_builder stream = {0};

    memset(&state, 0, sizeof state);
    CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(!output.active && !output.needs_depth && output.used_inferences == 0u);

    /* DEPTH: every function, with the enable written, resolves to its own compare and the mask to the write */
    for (uint32_t function = 0x0200u; function <= 0x0207u; function++) {
        for (uint32_t mask = 0u; mask < 2u; mask++) {
            stream_depth_state(&stream, 1u, function, mask);
            state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL);
            CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
            if (function == 0x0207u && mask == 0u) { /* no effect: ALWAYS and nothing written */
                CHECK(!output.active && !output.needs_depth && output.used_inferences == 0u);
                CHECK(gpu_pgraph_resolve_output(&state, &no_inference, WIDTH, HEIGHT, &output, &report) ==
                      GPU_PGRAPH_OK);
            } else {
                CHECK(output.active && output.needs_depth && output.needs_destination);
                CHECK(output.output.depth_test && !output.output.stencil_test);
                CHECK(output.output.depth_func == function - 0x0200u);
                CHECK(output.output.depth_write == (mask == 1u));
                CHECK(output.used_inferences == GPU_PGRAPH_INFER_OUTPUT_DEPTH_MODEL);
                expect_refused(&state, &no_inference, "INFERRED and not allowed");
                expect_refused(&state, &not_enabled, "the stream set depth and stencil state");
            }
            stream_free(&stream);
        }
    }
    /* the title's own record: LEQUAL and mask 1 with the enable never written needs the enable inference too */
    stream_pair(&stream, 0x0354u, 0x0203u);
    stream_pair(&stream, 0x035Cu, 1u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL);
    CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(output.output.depth_test && output.output.depth_func == GPU_VSH_COMPARE_LEQUAL && output.output.depth_write);
    CHECK(output.used_inferences ==
          (GPU_PGRAPH_INFER_OUTPUT_DEPTH_MODEL | GPU_PGRAPH_INFER_OUTPUT_DEPTH_STENCIL_ENABLE));
    gpu_pgraph_backend without_enable = on;
    without_enable.allowed_inferences = GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_OUTPUT_DEPTH_MODEL;
    expect_refused(&state, &without_enable, "taking the depth test as enabled");
    stream_free(&stream);
    /* an enable of 0 turns it all off, whatever the other words say */
    stream_depth_state(&stream, 0u, 0x0201u, 1u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL);
    CHECK(gpu_pgraph_resolve_output(&state, &no_inference, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(!output.active && output.used_inferences == 0u);
    stream_free(&stream);
    /* refusals */
    stream_pair(&stream, 0x030Cu, 2u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL);
    expect_refused(&state, &on, "depth test enable 0x2 is neither 0 nor 1");
    stream_free(&stream);
    stream_pair(&stream, 0x030Cu, 1u);
    stream_pair(&stream, 0x035Cu, 1u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL);
    expect_refused(&state, &on, "function (0x0354) was never written");
    stream_free(&stream);
    stream_pair(&stream, 0x030Cu, 1u);
    stream_pair(&stream, 0x0354u, 0x0201u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL);
    expect_refused(&state, &on, "mask (0x035C) was never written");
    stream_free(&stream);
    static const uint32_t bad_depth_functions[] = {0x01FFu, 0x0208u, 0u, 1u};
    for (size_t i = 0u; i < sizeof bad_depth_functions / sizeof bad_depth_functions[0]; i++) {
        stream_depth_state(&stream, 1u, bad_depth_functions[i], 1u);
        state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL);
        expect_refused(&state, &on, "depth function");
        stream_free(&stream);
    }
    stream_depth_state(&stream, 1u, 0x0201u, 2u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL);
    expect_refused(&state, &on, "depth mask 0x2 is neither 0 nor 1");
    stream_free(&stream);

    /* STENCIL: all words written with the enable, resolved field by field */
    stream_stencil_state(&stream, 1u, 0x0202u, 0x12u, 0x34u, 0x56u, 0x1E01u, 0x1E02u, 0x1E03u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL);
    CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(output.active && output.needs_depth && output.needs_destination && output.output.stencil_test);
    CHECK(!output.output.depth_test);
    CHECK(output.output.stencil_func == GPU_VSH_COMPARE_EQUAL && output.output.stencil_ref == 0x12u);
    CHECK(output.output.stencil_compare_mask == 0x34u && output.output.stencil_write_mask == 0x56u);
    CHECK(output.output.stencil_fail_op == GPU_VSH_STENCIL_OP_REPLACE);
    CHECK(output.output.stencil_zfail_op == GPU_VSH_STENCIL_OP_INCREMENT_CLAMP);
    CHECK(output.output.stencil_zpass_op == GPU_VSH_STENCIL_OP_DECREMENT_CLAMP);
    CHECK(output.used_inferences == GPU_PGRAPH_INFER_OUTPUT_STENCIL_MODEL);
    expect_refused(&state, &no_inference, "INFERRED and not allowed");
    stream_free(&stream);
    /* the eight operations map one to one, the others are refused */
    static const struct {
        uint32_t nv, ours;
    } operations[] = {
        {0x1E00u, GPU_VSH_STENCIL_OP_KEEP},
        {0x0000u, GPU_VSH_STENCIL_OP_ZERO},
        {0x1E01u, GPU_VSH_STENCIL_OP_REPLACE},
        {0x1E02u, GPU_VSH_STENCIL_OP_INCREMENT_CLAMP},
        {0x1E03u, GPU_VSH_STENCIL_OP_DECREMENT_CLAMP},
        {0x150Au, GPU_VSH_STENCIL_OP_INVERT},
        {0x8507u, GPU_VSH_STENCIL_OP_INCREMENT_WRAP},
        {0x8508u, GPU_VSH_STENCIL_OP_DECREMENT_WRAP},
    };
    CHECK(sizeof operations / sizeof operations[0] == GPU_VSH_STENCIL_OPS);
    for (size_t i = 0u; i < sizeof operations / sizeof operations[0]; i++) {
        CHECK(operations[i].ours == i);
        for (int which = 0; which < 3; which++) {
            stream_stencil_state(&stream, 1u, 0x0207u, 0u, 0xFFu, 0xFFu, which == 0 ? operations[i].nv : 0x1E00u,
                                 which == 1 ? operations[i].nv : 0x1E00u, which == 2 ? operations[i].nv : 0x1E00u);
            state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL);
            CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
            if (i == 0u) { /* ALWAYS with every op KEEP: no effect */
                CHECK(!output.active && !output.needs_depth);
            } else {
                CHECK(output.active && output.output.stencil_test);
                CHECK(output.output.stencil_fail_op == (which == 0 ? operations[i].ours : GPU_VSH_STENCIL_OP_KEEP));
                CHECK(output.output.stencil_zfail_op == (which == 1 ? operations[i].ours : GPU_VSH_STENCIL_OP_KEEP));
                CHECK(output.output.stencil_zpass_op == (which == 2 ? operations[i].ours : GPU_VSH_STENCIL_OP_KEEP));
            }
            stream_free(&stream);
        }
    }
    static const uint32_t bad_operations[] = {0x1E04u, 0x1E05u, 0x150Bu, 0x8506u, 0x8509u, 1u, 0x1DFFu};
    for (size_t i = 0u; i < sizeof bad_operations / sizeof bad_operations[0]; i++) {
        for (int which = 0; which < 3; which++) {
            stream_stencil_state(&stream, 1u, 0x0207u, 0u, 0xFFu, 0xFFu, which == 0 ? bad_operations[i] : 0x1E00u,
                                 which == 1 ? bad_operations[i] : 0x1E00u, which == 2 ? bad_operations[i] : 0x1E00u);
            state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL);
            expect_refused(&state, &on, which == 0 ? "stencil op on fail (0x0370)"
                                       : which == 1 ? "stencil op on depth fail (0x0374)"
                                                    : "stencil op on pass (0x0378)");
            stream_free(&stream);
        }
    }
    /* byte limits: a reference, a function mask and a write mask above 255 are refused, 255 is fine */
    for (int which = 0; which < 3; which++) {
        for (uint32_t value = 255u; value <= 256u; value++) {
            stream_stencil_state(&stream, 1u, 0x0201u, which == 0 ? value : 1u, which == 1 ? value : 0xFFu,
                                 which == 2 ? value : 0xFFu, 0x1E00u, 0x1E00u, 0x1E00u);
            state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL);
            if (value == 255u) {
                CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
                CHECK(output.active);
            } else {
                expect_refused(&state, &on, which == 0 ? "stencil reference 0x100 is above 255"
                                            : which == 1 ? "stencil function mask 0x100 is above 255"
                                                         : "stencil write mask 0x100 is above 255");
            }
            stream_free(&stream);
        }
    }
    static const uint32_t bad_stencil_functions[] = {0x01FFu, 0x0208u, 0u};
    for (size_t i = 0u; i < sizeof bad_stencil_functions / sizeof bad_stencil_functions[0]; i++) {
        stream_stencil_state(&stream, 1u, bad_stencil_functions[i], 0u, 0xFFu, 0xFFu, 0x1E00u, 0x1E00u, 0x1E01u);
        state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL);
        expect_refused(&state, &on, "stencil function");
        stream_free(&stream);
    }
    stream_pair(&stream, 0x032Cu, 2u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL);
    expect_refused(&state, &on, "stencil test enable 0x2 is neither 0 nor 1");
    stream_free(&stream);
    /* an enable of 0 turns it off */
    stream_stencil_state(&stream, 0u, 0x0202u, 1u, 0xFFu, 0xFFu, 0x1E01u, 0x1E01u, 0x1E01u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL);
    CHECK(gpu_pgraph_resolve_output(&state, &no_inference, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(!output.active && output.used_inferences == 0u);
    stream_free(&stream);

    /* the title's own stencil words (function ALWAYS, reference 0, ops KEEP, enable and the rest never written)
     * change nothing, so they need no inference at all */
    stream_pair(&stream, 0x0364u, 0x0207u);
    stream_pair(&stream, 0x0368u, 0u);
    stream_pair(&stream, 0x0374u, 0x1E00u);
    stream_pair(&stream, 0x0378u, 0x1E00u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL);
    CHECK(gpu_pgraph_resolve_output(&state, &no_inference, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(!output.active && !output.needs_depth && output.used_inferences == 0u);
    stream_free(&stream);
    /* ONE word written, the op on pass: every other word, and the function and reference above all, is its default */
    stream_pair(&stream, 0x0378u, 0x1E01u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL);
    CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(output.active && output.output.stencil_test);
    CHECK(output.output.stencil_func == GPU_VSH_COMPARE_ALWAYS && output.output.stencil_ref == 0u);
    CHECK(output.output.stencil_compare_mask == 0xFFu && output.output.stencil_write_mask == 0xFFu);
    CHECK(output.output.stencil_fail_op == GPU_VSH_STENCIL_OP_KEEP && output.output.stencil_zfail_op == GPU_VSH_STENCIL_OP_KEEP);
    CHECK(output.output.stencil_zpass_op == GPU_VSH_STENCIL_OP_REPLACE);
    CHECK(output.used_inferences == (GPU_PGRAPH_INFER_OUTPUT_STENCIL_MODEL |
                                     GPU_PGRAPH_INFER_OUTPUT_DEPTH_STENCIL_ENABLE |
                                     GPU_PGRAPH_INFER_OUTPUT_STENCIL_DEFAULTS));
    stream_free(&stream);
    /* each of the other six words alone: the function (not ALWAYS) and the ops that are not KEEP make a state that
     * can change something, the reference and the two masks alone, against ALWAYS and KEEP, change nothing. The op on
     * fail is "an effect" although ALWAYS never runs it: the resolution does not look through the function. */
    static const struct {
        uint32_t method, value;
        bool active;
    } alone[] = {
        {0x0364u, 0x0202u, true}, {0x0368u, 7u, false}, {0x036Cu, 0x0Fu, false},
        {0x0360u, 0x0Fu, false},  {0x0370u, 0x1E01u, true}, {0x0374u, 0x1E01u, true},
    };
    for (size_t i = 0u; i < sizeof alone / sizeof alone[0]; i++) {
        stream_pair(&stream, alone[i].method, alone[i].value);
        state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL);
        CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
        CHECK(output.active == alone[i].active && output.needs_depth == alone[i].active);
        stream_free(&stream);
    }
    /* an effect with words never written: the enable and the defaults are two further inferences, each its own gate */
    stream_pair(&stream, 0x0364u, 0x0202u);
    stream_pair(&stream, 0x0368u, 3u);
    stream_pair(&stream, 0x0378u, 0x1E01u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL);
    CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(output.output.stencil_compare_mask == 0xFFu && output.output.stencil_write_mask == 0xFFu);
    CHECK(output.output.stencil_fail_op == GPU_VSH_STENCIL_OP_KEEP &&
          output.output.stencil_zfail_op == GPU_VSH_STENCIL_OP_KEEP);
    CHECK(output.used_inferences == (GPU_PGRAPH_INFER_OUTPUT_STENCIL_MODEL |
                                     GPU_PGRAPH_INFER_OUTPUT_DEPTH_STENCIL_ENABLE |
                                     GPU_PGRAPH_INFER_OUTPUT_STENCIL_DEFAULTS));
    gpu_pgraph_backend partial = on;
    partial.allowed_inferences = GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_OUTPUT_STENCIL_MODEL |
                                 GPU_PGRAPH_INFER_OUTPUT_STENCIL_DEFAULTS;
    expect_refused(&state, &partial, "taking the stencil test as enabled");
    partial.allowed_inferences = GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_OUTPUT_STENCIL_MODEL |
                                 GPU_PGRAPH_INFER_OUTPUT_DEPTH_STENCIL_ENABLE;
    expect_refused(&state, &partial, "taking the stencil words never written");
    stream_free(&stream);

    /* only the op on fail never written, every other word given: that one word is still a default, and says so */
    stream_pair(&stream, 0x032Cu, 1u);
    stream_pair(&stream, 0x0364u, 0x0202u);
    stream_pair(&stream, 0x0368u, 5u);
    stream_pair(&stream, 0x036Cu, 0xFFu);
    stream_pair(&stream, 0x0360u, 0xFFu);
    stream_pair(&stream, 0x0374u, 0x1E00u);
    stream_pair(&stream, 0x0378u, 0x1E01u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL);
    CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(output.used_inferences ==
          (GPU_PGRAPH_INFER_OUTPUT_STENCIL_MODEL | GPU_PGRAPH_INFER_OUTPUT_STENCIL_DEFAULTS));
    partial.allowed_inferences = GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_OUTPUT_STENCIL_MODEL;
    expect_refused(&state, &partial, "taking the stencil words never written");
    stream_free(&stream);

    /* depth and stencil resolve together, and with the title's whole block and every group */
    stream_depth_state(&stream, 1u, 0x0203u, 1u);
    stream_stencil_state(&stream, 1u, 0x0202u, 5u, 0xFFu, 0xFFu, 0x1E00u, 0x1E00u, 0x1E01u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL);
    CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(output.output.depth_test && output.output.stencil_test && output.output.depth_write);
    CHECK(output.used_inferences ==
          (GPU_PGRAPH_INFER_OUTPUT_DEPTH_MODEL | GPU_PGRAPH_INFER_OUTPUT_STENCIL_MODEL));
    stream_free(&stream);
}

/* --- polygon offset (T502) ------------------------------------------------------------------- */

static uint32_t float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

/* The words in the order the title's state table and its ZBIAS handler write them: bias, scale, fill, point, line. */
static void stream_polygon_offset(stream_builder *stream, float scale, float bias, uint32_t fill, uint32_t point,
                                  uint32_t line)
{
    stream_pair(stream, 0x0388u, float_bits(bias));
    stream_pair(stream, 0x0384u, float_bits(scale));
    stream_pair(stream, 0x0338u, fill);
    stream_pair(stream, 0x0330u, point);
    stream_pair(stream, 0x0334u, line);
}

static void test_resolve_polygon_offset(void)
{
    printf("test_resolve_polygon_offset\n");
    fake_guest guest = {MEMORY_BASE, memory, sizeof memory};
    const uint32_t groups = GPU_PGRAPH_OUTPUT_POLYGON_OFFSET | GPU_PGRAPH_OUTPUT_DEPTH_STENCIL;
    gpu_pgraph_output output;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    gpu_pgraph_backend on = make_backend(&guest, groups, GPU_PGRAPH_INFER_OUTPUT_ALL);
    gpu_pgraph_backend no_inference = make_backend(&guest, groups, 0u);
    no_inference.allowed_inferences = GPU_PGRAPH_INFER_ALL;
    gpu_pgraph_backend not_enabled = make_backend(&guest, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL, GPU_PGRAPH_INFER_OUTPUT_ALL);
    gpu_pgraph_state state;
    stream_builder stream = {0};

    /* the ALL mask names both new inferences and the group is part of the measured set, none of it in INFER_ALL */
    CHECK((GPU_PGRAPH_INFER_OUTPUT_ALL & GPU_PGRAPH_INFER_OUTPUT_POLYGON_OFFSET_MODEL) != 0u);
    CHECK((GPU_PGRAPH_INFER_OUTPUT_ALL & GPU_PGRAPH_INFER_OUTPUT_POLYGON_OFFSET_FILL_ONLY) != 0u);
    CHECK((GPU_PGRAPH_INFER_ALL & GPU_PGRAPH_INFER_OUTPUT_ALL) == 0u);
    CHECK((GPU_PGRAPH_OUTPUT_ALL_MEASURED & GPU_PGRAPH_OUTPUT_POLYGON_OFFSET) != 0u);
    CHECK((GPU_PGRAPH_OUTPUT_ALL_MEASURED & GPU_PGRAPH_OUTPUT_IGNORED) != 0u);

    /* the title's startup state (all zero) and the ZBIAS handler's -0.0 are nothing: no output, no inference */
    stream_polygon_offset(&stream, 0.0f, 0.0f, 0u, 0u, 0u);
    state = decoded_state(&stream, groups);
    CHECK(state.output_written[GPU_PGRAPH_OUT_POLY_OFFSET_BIAS]);
    CHECK(gpu_pgraph_resolve_output(&state, &no_inference, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(!output.active && output.used_inferences == 0u && !output.offset_unobserved);
    stream_free(&stream);
    stream_polygon_offset(&stream, -0.0f, -0.0f, 1u, 0u, 0u);
    state = decoded_state(&stream, groups);
    CHECK(gpu_pgraph_resolve_output(&state, &no_inference, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(!output.active && output.used_inferences == 0u && !output.output.depth_bias);
    stream_free(&stream);

    /* enabled and non-zero, a depth test present: Vulkan bias, constant = bias (0x0388), slope = scale (0x0384) */
    stream_depth_state(&stream, 1u, 0x0201u, 1u);
    stream_polygon_offset(&stream, -0.25f, -3.0f, 1u, 0u, 0u);
    state = decoded_state(&stream, groups);
    CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(output.active && output.output.depth_test && output.output.depth_bias && !output.offset_unobserved);
    CHECK(output.output.depth_bias_constant == -3.0f && output.output.depth_bias_slope == -0.25f);
    CHECK(output.used_inferences ==
          (GPU_PGRAPH_INFER_OUTPUT_DEPTH_MODEL | GPU_PGRAPH_INFER_OUTPUT_POLYGON_OFFSET_MODEL));
    expect_refused(&state, &no_inference, "INFERRED and not allowed");
    expect_refused(&state, &not_enabled, "the stream set polygon offset state");
    gpu_pgraph_backend without_model = on;
    without_model.allowed_inferences = GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_OUTPUT_DEPTH_MODEL;
    expect_refused(&state, &without_model, "offsetting the depth of filled triangles");
    stream_free(&stream);

    /* disabled fill enable with values set: nothing, whatever the values (the title's startup is this shape) */
    stream_depth_state(&stream, 1u, 0x0201u, 1u);
    stream_polygon_offset(&stream, -0.25f, -3.0f, 0u, 0u, 0u);
    state = decoded_state(&stream, groups);
    CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(!output.output.depth_bias && output.used_inferences == GPU_PGRAPH_INFER_OUTPUT_DEPTH_MODEL);
    stream_free(&stream);

    /* a bias only or a scale only is non-zero too */
    for (int which = 0; which < 2; which++) {
        stream_depth_state(&stream, 1u, 0x0201u, 1u);
        stream_polygon_offset(&stream, which == 0 ? 2.0f : 0.0f, which == 1 ? -5.0f : 0.0f, 1u, 0u, 0u);
        state = decoded_state(&stream, groups);
        CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
        CHECK(output.output.depth_bias);
        CHECK(output.output.depth_bias_slope == (which == 0 ? 2.0f : 0.0f));
        CHECK(output.output.depth_bias_constant == (which == 1 ? -5.0f : 0.0f));
        stream_free(&stream);
    }

    /* no depth test (the stream wrote none, depth off): counted as unobserved, not applied, still INFERRED */
    stream_polygon_offset(&stream, -0.25f, -3.0f, 1u, 0u, 0u);
    state = decoded_state(&stream, groups);
    CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(!output.output.depth_bias && output.offset_unobserved && !output.active);
    CHECK(output.used_inferences == GPU_PGRAPH_INFER_OUTPUT_POLYGON_OFFSET_MODEL);
    expect_refused(&state, &no_inference, "INFERRED and not allowed");
    stream_free(&stream);
    stream_depth_state(&stream, 0u, 0x0201u, 1u);
    stream_polygon_offset(&stream, -0.25f, -3.0f, 1u, 0u, 0u);
    state = decoded_state(&stream, groups);
    CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(!output.output.depth_bias && output.offset_unobserved);
    stream_free(&stream);

    /* point or line enable: ignored on purpose, named by their own inference, and only when set to 1 */
    for (uint32_t which = 0u; which < 2u; which++) {
        stream_polygon_offset(&stream, 0.0f, 0.0f, 0u, which == 0u ? 1u : 0u, which == 1u ? 1u : 0u);
        state = decoded_state(&stream, groups);
        CHECK(gpu_pgraph_resolve_output(&state, &on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
        CHECK(!output.active && output.used_inferences == GPU_PGRAPH_INFER_OUTPUT_POLYGON_OFFSET_FILL_ONLY);
        expect_refused(&state, &no_inference, "ignoring the point and line polygon offset enables");
        stream_free(&stream);
    }

    /* refusals */
    stream_pair(&stream, 0x0338u, 2u);
    state = decoded_state(&stream, groups);
    expect_refused(&state, &on, "polygon offset fill (0x0338) enable 0x2 is neither 0 nor 1");
    stream_free(&stream);
    stream_pair(&stream, 0x0330u, 0x100u);
    state = decoded_state(&stream, groups);
    expect_refused(&state, &on, "polygon offset point (0x0330) enable 0x100");
    stream_free(&stream);
    stream_pair(&stream, 0x0334u, 3u);
    state = decoded_state(&stream, groups);
    expect_refused(&state, &on, "polygon offset line (0x0334) enable 0x3");
    stream_free(&stream);
    static const uint32_t not_finite[] = {0x7F800000u, 0xFF800000u, 0x7FC00000u};
    for (size_t i = 0u; i < sizeof not_finite / sizeof not_finite[0]; i++) {
        stream_pair(&stream, 0x0384u, not_finite[i]);
        state = decoded_state(&stream, groups);
        expect_refused(&state, &on, "polygon offset scale factor");
        stream_free(&stream);
        stream_pair(&stream, 0x0388u, not_finite[i]);
        state = decoded_state(&stream, groups);
        expect_refused(&state, &on, "polygon offset bias");
        stream_free(&stream);
    }
    stream_pair(&stream, 0x0338u, 1u);
    stream_pair(&stream, 0x0388u, float_bits(-1.0f));
    state = decoded_state(&stream, groups);
    expect_refused(&state, &on, "scale factor (0x0384) was never written");
    stream_free(&stream);
    stream_pair(&stream, 0x0338u, 1u);
    stream_pair(&stream, 0x0384u, float_bits(-1.0f));
    state = decoded_state(&stream, groups);
    expect_refused(&state, &on, "bias (0x0388) was never written");
    stream_free(&stream);

    /* the IGNORED group: written with the group off in the backend is refused (never silently dropped), on is a
     * no-op that needs no inference and resolves to nothing */
    stream_pair(&stream, 0x0310u, 1u);
    stream_pair(&stream, 0x09F8u, 3u);
    state = decoded_state(&stream, GPU_PGRAPH_OUTPUT_IGNORED);
    gpu_pgraph_backend ignored_on = make_backend(&guest, GPU_PGRAPH_OUTPUT_IGNORED, 0u);
    ignored_on.allowed_inferences = GPU_PGRAPH_INFER_ALL;
    CHECK(gpu_pgraph_resolve_output(&state, &ignored_on, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(!output.active && output.used_inferences == 0u);
    expect_refused(&state, &not_enabled, "the stream set ignored (dither, specular parameters) state");
    stream_free(&stream);
}

static gpu_pgraph_clear clear_event(uint32_t flags, uint32_t colour, uint32_t zstencil, uint32_t x_min,
                                    uint32_t x_max, uint32_t y_min, uint32_t y_max)
{
    gpu_pgraph_clear clear;
    memset(&clear, 0, sizeof clear);
    clear.flags = flags;
    clear.color = colour;
    clear.zstencil = zstencil;
    clear.rect_horizontal = x_min | (x_max << 16);
    clear.rect_vertical = y_min | (y_max << 16);
    clear.color_written = clear.zstencil_written = true;
    clear.rect_horizontal_written = clear.rect_vertical_written = true;
    return clear;
}

static void expect_clear_refused(const gpu_pgraph_clear *clear, const gpu_pgraph_backend *backend,
                                 const char *needle)
{
    gpu_pgraph_clear_resolved resolved;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    CHECK(gpu_pgraph_resolve_clear(clear, backend, WIDTH, HEIGHT, &resolved, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, needle) != NULL);
    if (strstr(report.error, needle) == NULL) {
        printf("    refusal was: %s\n", report.error);
    }
}

static void test_resolve_clear(void)
{
    printf("test_resolve_clear\n");
    fake_guest guest = {MEMORY_BASE, memory, sizeof memory};
    gpu_pgraph_backend on = make_backend(&guest, GPU_PGRAPH_OUTPUT_CLEAR | GPU_PGRAPH_OUTPUT_DEPTH_STENCIL,
                                         GPU_PGRAPH_INFER_OUTPUT_ALL);
    gpu_pgraph_backend colour_only = make_backend(&guest, GPU_PGRAPH_OUTPUT_CLEAR, GPU_PGRAPH_INFER_OUTPUT_ALL);
    gpu_pgraph_backend no_inference = make_backend(&guest, GPU_PGRAPH_OUTPUT_CLEAR | GPU_PGRAPH_OUTPUT_DEPTH_STENCIL, 0u);
    no_inference.allowed_inferences = GPU_PGRAPH_INFER_ALL;
    gpu_pgraph_backend flipped = on;
    flipped.flip_y = true;
    gpu_pgraph_clear_resolved resolved;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);

    /* colour: ARGB to R, G, B, A, the channels from the flag bits, the rectangle as given (inclusive) */
    gpu_pgraph_clear clear = clear_event(0xF0u, 0x80FF4020u, 0u, 3u, 40u, 5u, 50u);
    CHECK(gpu_pgraph_resolve_clear(&clear, &colour_only, WIDTH, HEIGHT, &resolved, &report) == GPU_PGRAPH_OK);
    CHECK(resolved.colour && !resolved.depth && !resolved.stencil);
    CHECK(resolved.rgba[0] == 0xFFu && resolved.rgba[1] == 0x40u && resolved.rgba[2] == 0x20u && resolved.rgba[3] == 0x80u);
    CHECK(resolved.channels == (GPU_VSH_CHANNEL_R | GPU_VSH_CHANNEL_G | GPU_VSH_CHANNEL_B | GPU_VSH_CHANNEL_A));
    CHECK(resolved.x_min == 3u && resolved.x_max == 40u && resolved.y_min == 5u && resolved.y_max == 50u);
    CHECK(resolved.used_inferences == GPU_PGRAPH_INFER_OUTPUT_CLEAR_MODEL);
    expect_clear_refused(&clear, &no_inference, "INFERRED and not allowed");
    CHECK(gpu_pgraph_resolve_clear(&clear, &flipped, WIDTH, HEIGHT, &resolved, &report) == GPU_PGRAPH_OK);
    CHECK(resolved.y_min == 63u - 50u && resolved.y_max == 63u - 5u && resolved.x_min == 3u && resolved.x_max == 40u);
    /* every colour flag bit is its own channel */
    static const struct {
        uint32_t flag, channel;
    } channels[] = {{0x10u, GPU_VSH_CHANNEL_R}, {0x20u, GPU_VSH_CHANNEL_G}, {0x40u, GPU_VSH_CHANNEL_B}, {0x80u, GPU_VSH_CHANNEL_A}};
    for (size_t i = 0u; i < 4u; i++) {
        clear = clear_event(channels[i].flag, 0xFFFFFFFFu, 0u, 0u, 63u, 0u, 63u);
        CHECK(gpu_pgraph_resolve_clear(&clear, &colour_only, WIDTH, HEIGHT, &resolved, &report) == GPU_PGRAPH_OK);
        CHECK(resolved.colour && resolved.channels == channels[i].channel);
    }
    /* depth and stencil: D24S8, depth = value >> 8 over 16777215, stencil the low byte */
    clear = clear_event(0x03u, 0u, 0xFFFFFF7Fu, 0u, 63u, 0u, 63u);
    CHECK(gpu_pgraph_resolve_clear(&clear, &on, WIDTH, HEIGHT, &resolved, &report) == GPU_PGRAPH_OK);
    CHECK(!resolved.colour && resolved.depth && resolved.stencil && resolved.z == 1.0f && resolved.stencil_value == 0x7Fu);
    clear = clear_event(0x01u, 0u, 0x00000000u, 0u, 63u, 0u, 63u);
    CHECK(gpu_pgraph_resolve_clear(&clear, &on, WIDTH, HEIGHT, &resolved, &report) == GPU_PGRAPH_OK);
    CHECK(resolved.depth && !resolved.stencil && resolved.z == 0.0f);
    clear = clear_event(0x01u, 0u, 0x80000000u, 0u, 63u, 0u, 63u);
    CHECK(gpu_pgraph_resolve_clear(&clear, &on, WIDTH, HEIGHT, &resolved, &report) == GPU_PGRAPH_OK);
    CHECK(resolved.z > 0.49999f && resolved.z < 0.50001f);
    clear = clear_event(0x02u, 0u, 0x000000FFu, 0u, 63u, 0u, 63u);
    CHECK(gpu_pgraph_resolve_clear(&clear, &on, WIDTH, HEIGHT, &resolved, &report) == GPU_PGRAPH_OK);
    CHECK(!resolved.depth && resolved.stencil && resolved.stencil_value == 0xFFu);
    /* a colour clear needs no depth group, a depth clear without it is refused */
    clear = clear_event(0x01u, 0u, 0u, 0u, 63u, 0u, 63u);
    expect_clear_refused(&clear, &colour_only, "needs the depth and stencil group");
    clear = clear_event(0x02u, 0u, 0u, 0u, 63u, 0u, 63u);
    expect_clear_refused(&clear, &colour_only, "needs the depth and stencil group");
    /* nothing to clear: no flags, no rectangle needed, no inference */
    clear = clear_event(0u, 0u, 0u, 0u, 0u, 0u, 0u);
    clear.rect_horizontal_written = clear.rect_vertical_written = false;
    CHECK(gpu_pgraph_resolve_clear(&clear, &no_inference, WIDTH, HEIGHT, &resolved, &report) == GPU_PGRAPH_OK);
    CHECK(!resolved.colour && !resolved.depth && !resolved.stencil && resolved.used_inferences == 0u);
    /* refusals */
    static const uint32_t bad_flags[] = {0x04u, 0x08u, 0x100u, 0x0Cu, 0xFFFFFFFFu, 0x10000u};
    for (size_t i = 0u; i < sizeof bad_flags / sizeof bad_flags[0]; i++) {
        clear = clear_event(bad_flags[i], 0u, 0u, 0u, 63u, 0u, 63u);
        expect_clear_refused(&clear, &on, "have bits beyond depth (1), stencil (2) and the four colour channels");
    }
    clear = clear_event(0x10u, 0u, 0u, 0u, 63u, 0u, 63u);
    clear.rect_horizontal_written = false;
    expect_clear_refused(&clear, &on, "no horizontal rectangle (0x1D98) written");
    clear = clear_event(0x10u, 0u, 0u, 0u, 63u, 0u, 63u);
    clear.rect_vertical_written = false;
    expect_clear_refused(&clear, &on, "no vertical rectangle (0x1D9C) written");
    clear = clear_event(0x10u, 0u, 0u, 0u, 63u, 0u, 63u);
    clear.color_written = false;
    expect_clear_refused(&clear, &on, "no clear colour (0x1D90) written");
    clear = clear_event(0x01u, 0u, 0u, 0u, 63u, 0u, 63u);
    clear.zstencil_written = false;
    expect_clear_refused(&clear, &on, "no clear value (0x1D8C) written");
    clear = clear_event(0x02u, 0u, 0u, 0u, 63u, 0u, 63u);
    clear.zstencil_written = false;
    expect_clear_refused(&clear, &on, "no clear value (0x1D8C) written");
    /* a colour clear never asks for the depth value, nor a depth clear for the colour */
    clear = clear_event(0x10u, 0xFFu, 0u, 0u, 63u, 0u, 63u);
    clear.zstencil_written = false;
    CHECK(gpu_pgraph_resolve_clear(&clear, &on, WIDTH, HEIGHT, &resolved, &report) == GPU_PGRAPH_OK);
    clear = clear_event(0x01u, 0u, 0u, 0u, 63u, 0u, 63u);
    clear.color_written = false;
    CHECK(gpu_pgraph_resolve_clear(&clear, &on, WIDTH, HEIGHT, &resolved, &report) == GPU_PGRAPH_OK);
    /* rectangles: the last pixel is fine, one past it, an inverted one and a 16 bit overflow are not */
    clear = clear_event(0x10u, 0u, 0u, 63u, 63u, 63u, 63u);
    CHECK(gpu_pgraph_resolve_clear(&clear, &on, WIDTH, HEIGHT, &resolved, &report) == GPU_PGRAPH_OK);
    CHECK(resolved.x_min == 63u && resolved.x_max == 63u && resolved.y_min == 63u && resolved.y_max == 63u);
    static const uint32_t bad_rects[][4] = {{0u, 64u, 0u, 63u}, {0u, 63u, 0u, 64u}, {10u, 9u, 0u, 63u},
                                            {0u, 63u, 10u, 9u}, {64u, 64u, 0u, 63u}, {0u, 0xFFFFu, 0u, 63u}};
    for (size_t i = 0u; i < sizeof bad_rects / sizeof bad_rects[0]; i++) {
        clear = clear_event(0x10u, 0u, 0u, bad_rects[i][0], bad_rects[i][1], bad_rects[i][2], bad_rects[i][3]);
        expect_clear_refused(&clear, &on, "is empty or reaches past the 64x64 target");
    }
    /* a one pixel rectangle at the origin: x_min == x_max is a pixel, not empty */
    clear = clear_event(0x10u, 0u, 0u, 0u, 0u, 0u, 0u);
    CHECK(gpu_pgraph_resolve_clear(&clear, &on, WIDTH, HEIGHT, &resolved, &report) == GPU_PGRAPH_OK);
    /* a flipped one-pixel rectangle on the last row lands on the first row */
    clear = clear_event(0x10u, 0u, 0u, 7u, 7u, 63u, 63u);
    CHECK(gpu_pgraph_resolve_clear(&clear, &flipped, WIDTH, HEIGHT, &resolved, &report) == GPU_PGRAPH_OK);
    CHECK(resolved.y_min == 0u && resolved.y_max == 0u);
}

/* --- 3. pixels ---------------------------------------------------------------------------- */

static gpu_pgraph_result replay_frame(gpu_device *device, const stream_builder *stream, uint32_t groups,
                                      bool flip_y, gpu_image *frame, gpu_pgraph_report *report)
{
    fill_memory();
    fake_guest guest = {MEMORY_BASE, memory, sizeof memory};
    gpu_pgraph_backend backend = make_backend(&guest, groups, GPU_PGRAPH_INFER_OUTPUT_ALL);
    backend.flip_y = flip_y;
    gpu_pgraph *model = gpu_pgraph_create();
    gpu_pgraph_set_strict(model, true);
    gpu_pgraph_set_output_groups(model, groups);
    gpu_pgraph_result result = gpu_pgraph_decode(model, stream->pairs, stream->count);
    CHECK(result == GPU_PGRAPH_OK);
    memset(frame, 0, sizeof *frame);
    result = gpu_pgraph_replay(model, device, &backend, WIDTH, HEIGHT, clear_grey, frame, report);
    gpu_pgraph_destroy(model);
    return result;
}

static void test_scissor_pixels(gpu_device *device)
{
    printf("test_scissor_pixels (%s)\n", gpu_device_name(device));
    gpu_pgraph_report report;

    /* the unscissored reference, and the default path (no group, no state words) must be the SAME BYTES: turning EVERY
     * group on does nothing to a stream that never set any state, not a pixel and not an inference */
    stream_builder plain = {0};
    stream_setup(&plain);
    stream_draw_arrays(&plain, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    gpu_image reference = {0};
    gpu_image enabled = {0};
    CHECK(replay_frame(device, &plain, 0u, false, &reference, &report) == GPU_PGRAPH_OK);
    CHECK(report.used_inferences == PLAIN_INFERENCES);
    CHECK(replay_frame(device, &plain, GPU_PGRAPH_OUTPUT_ALL_MEASURED, false, &enabled, &report) == GPU_PGRAPH_OK);
    CHECK(identical(&reference, &enabled));
    CHECK(report.used_inferences == PLAIN_INFERENCES);
    gpu_image_free(&enabled);
    const uint32_t reference_red = count_red(&reference);
    CHECK(reference_red > 1500u); /* the upper left half of a 64 x 64 target, less its margin: non-empty */
    CHECK(pixel_is(&reference, 63u, 63u, 51u, 51u, 51u, 255u)); /* and the clear is elsewhere */

    /* a scissored draw: inside the rectangle the pixel is the unscissored one, outside it is the clear */
    const uint32_t rx = 12u, ry = 20u, rw = 30u, rh = 18u;
    stream_builder scissored = {0};
    stream_setup(&scissored);
    stream_scissor(&scissored, rx, ry, rw, rh, WIDTH, HEIGHT);
    stream_draw_arrays(&scissored, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    gpu_image frame = {0};
    CHECK(replay_frame(device, &scissored, GPU_PGRAPH_OUTPUT_SCISSOR, false, &frame, &report) == GPU_PGRAPH_OK);
    CHECK(report.drawn == 1u &&
          report.used_inferences == (PLAIN_INFERENCES | GPU_PGRAPH_INFER_OUTPUT_SCISSOR));
    uint32_t inside_red = 0u;
    uint32_t expected_inside = 0u;
    bool exact = true;
    for (uint32_t y = 0u; y < HEIGHT; y++) {
        for (uint32_t x = 0u; x < WIDTH; x++) {
            const bool inside = x >= rx && x < rx + rw && y >= ry && y < ry + rh;
            if (inside) {
                exact = exact && same_pixel(&frame, &reference, x, y);
                inside_red += pixel_is(&frame, x, y, 255u, 0u, 0u, 255u);
                expected_inside += pixel_is(&reference, x, y, 255u, 0u, 0u, 255u);
            } else {
                exact = exact && pixel_is(&frame, x, y, 51u, 51u, 51u, 255u);
            }
        }
    }
    CHECK(exact);
    CHECK(expected_inside > 100u && inside_red == expected_inside);
    CHECK(count_red(&frame) == expected_inside);
    gpu_image_free(&frame);

    /* flip_y: the rectangle is the rows of the FINISHED image. The reference is the flipped plain frame. */
    gpu_image reference_flipped = {0};
    CHECK(replay_frame(device, &plain, 0u, true, &reference_flipped, &report) == GPU_PGRAPH_OK);
    CHECK(replay_frame(device, &scissored, GPU_PGRAPH_OUTPUT_SCISSOR, true, &frame, &report) == GPU_PGRAPH_OK);
    exact = true;
    uint32_t flipped_inside = 0u;
    for (uint32_t y = 0u; y < HEIGHT; y++) {
        for (uint32_t x = 0u; x < WIDTH; x++) {
            const bool inside = x >= rx && x < rx + rw && y >= ry && y < ry + rh;
            exact = exact && (inside ? same_pixel(&frame, &reference_flipped, x, y)
                                     : pixel_is(&frame, x, y, 51u, 51u, 51u, 255u));
            flipped_inside += inside && pixel_is(&reference_flipped, x, y, 255u, 0u, 0u, 255u);
        }
    }
    CHECK(exact);
    CHECK(flipped_inside > 100u && flipped_inside != expected_inside);
    CHECK(count_red(&frame) == flipped_inside);
    gpu_image_free(&frame);
    gpu_image_free(&reference_flipped);

    /* a zero width or zero height rectangle writes nothing, a one pixel one writes one pixel's worth */
    stream_builder empty_rectangle = {0};
    stream_setup(&empty_rectangle);
    stream_scissor(&empty_rectangle, 10u, 10u, 0u, 20u, WIDTH, HEIGHT);
    stream_draw_arrays(&empty_rectangle, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(replay_frame(device, &empty_rectangle, GPU_PGRAPH_OUTPUT_SCISSOR, false, &frame, &report) ==
          GPU_PGRAPH_OK);
    CHECK(count_red(&frame) == 0u && report.drawn == 1u);
    gpu_image_free(&frame);
    stream_free(&empty_rectangle);
    stream_builder one_pixel = {0};
    stream_setup(&one_pixel);
    stream_scissor(&one_pixel, 20u, 20u, 1u, 1u, WIDTH, HEIGHT);
    stream_draw_arrays(&one_pixel, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(replay_frame(device, &one_pixel, GPU_PGRAPH_OUTPUT_SCISSOR, false, &frame, &report) == GPU_PGRAPH_OK);
    CHECK(count_red(&frame) == 1u && pixel_is(&frame, 20u, 20u, 255u, 0u, 0u, 255u));
    gpu_image_free(&frame);
    stream_free(&one_pixel);

    /* two draws, the scissor changing between them: each draw is clipped by the rectangle current at its
     * own end, and the second draw does not erase the first */
    stream_builder two = {0};
    stream_setup(&two);
    stream_scissor(&two, 0u, 0u, 20u, 64u, WIDTH, HEIGHT);
    stream_draw_arrays(&two, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    stream_scissor(&two, 44u, 0u, 20u, 64u, WIDTH, HEIGHT);
    stream_draw_arrays(&two, GPU_PGRAPH_OP_TRIANGLES, 3u, 3u);
    CHECK(replay_frame(device, &two, GPU_PGRAPH_OUTPUT_SCISSOR, false, &frame, &report) == GPU_PGRAPH_OK);
    CHECK(report.drawn == 2u);
    stream_builder second_only = {0};
    stream_setup(&second_only);
    stream_draw_arrays(&second_only, GPU_PGRAPH_OP_TRIANGLES, 3u, 3u);
    gpu_image second = {0};
    CHECK(replay_frame(device, &second_only, 0u, false, &second, &report) == GPU_PGRAPH_OK);
    exact = true;
    uint32_t left_red = 0u;
    uint32_t right_red = 0u;
    for (uint32_t y = 0u; y < HEIGHT; y++) {
        for (uint32_t x = 0u; x < WIDTH; x++) {
            if (x < 20u) {
                exact = exact && same_pixel(&frame, &reference, x, y);
                left_red += pixel_is(&frame, x, y, 255u, 0u, 0u, 255u);
            } else if (x >= 44u) {
                exact = exact && same_pixel(&frame, &second, x, y);
                right_red += pixel_is(&frame, x, y, 255u, 0u, 0u, 255u);
            } else {
                exact = exact && pixel_is(&frame, x, y, 51u, 51u, 51u, 255u);
            }
        }
    }
    CHECK(exact);
    CHECK(left_red > 100u && right_red > 100u);
    gpu_image_free(&second);
    gpu_image_free(&frame);
    stream_free(&second_only);
    stream_free(&two);

    /* a refused scissor refuses the draw through the whole replay, with the refusal's own message */
    stream_builder outside = {0};
    stream_setup(&outside);
    stream_scissor(&outside, 60u, 0u, 20u, 20u, WIDTH, HEIGHT);
    stream_draw_arrays(&outside, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(replay_frame(device, &outside, GPU_PGRAPH_OUTPUT_SCISSOR, false, &frame, &report) ==
          GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "lies outside the 64x64 target") != NULL && report.failed_draw == 0u);
    CHECK(frame.pixels == NULL);
    stream_free(&outside);

    stream_free(&scissored);
    stream_free(&plain);
    gpu_image_free(&reference);
}

/* gpu_vsh_render's own bounds check of the rectangle, called directly: the replay resolves the rectangle
 * first, so only a direct call reaches the device wrapper with one the resolution would have refused. */
static void test_device_bounds(gpu_device *device)
{
    printf("test_device_bounds (%s)\n", gpu_device_name(device));
    float attributes[3 * GPU_VSH_ATTRIBUTE_FLOATS];
    memset(attributes, 0, sizeof attributes);
    static const float corners[3][2] = {{-0.9f, -0.9f}, {0.9f, -0.9f}, {-0.9f, 0.9f}};
    for (uint32_t i = 0u; i < 3u; i++) {
        float *vertex = attributes + i * GPU_VSH_ATTRIBUTE_FLOATS;
        vertex[4] = corners[i][0]; /* v1 = position */
        vertex[5] = corners[i][1];
        vertex[6] = 0.5f;
        vertex[7] = 1.0f;
        vertex[8 + 2] = 1.0f; /* v2 = (0, 0, 1, 1) */
        vertex[8 + 3] = 1.0f;
    }
    float constants[GPU_VSH_CONSTANT_ROWS * 4u];
    memset(constants, 0, sizeof constants);
    gpu_vsh_output output = {0};
    output.scissor = true;
    const gpu_vsh_draw draw = {
        .words = draw_vertex_words,
        .word_count = sizeof draw_vertex_words / sizeof(uint32_t),
        .vertex_count = 3u,
        .attributes = attributes,
        .constants = constants,
        .output = &output,
    };
    static const struct {
        uint32_t x, y, width, height;
        bool accepted;
        const char *what;
    } cases[] = {
        {0u, 0u, 64u, 64u, true, "the whole target"},
        {63u, 63u, 1u, 1u, true, "the last pixel"},
        {64u, 10u, 0u, 5u, true, "an empty rectangle on the right edge"},
        {10u, 64u, 5u, 0u, true, "an empty rectangle on the bottom edge"},
        {40u, 0u, 25u, 10u, false, "ends one pixel past the right edge"},
        {0u, 40u, 10u, 25u, false, "ends one pixel past the bottom edge"},
        {60u, 0u, 70u, 10u, false, "starts inside and is wider than the target"},
        {0u, 60u, 10u, 70u, false, "starts inside and is taller than the target"},
        {70u, 0u, 0u, 10u, false, "an origin past the right edge, empty"},
        {0u, 70u, 10u, 0u, false, "an origin past the bottom edge, empty"},
        {65u, 0u, 1u, 1u, false, "a pixel past the right edge"},
    };
    static const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i++) {
        output.scissor_x = cases[i].x;
        output.scissor_y = cases[i].y;
        output.scissor_width = cases[i].width;
        output.scissor_height = cases[i].height;
        gpu_image image = {0};
        const gpu_result result = gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &image);
        if ((result == GPU_OK) != cases[i].accepted) {
            printf("    case '%s': got %s\n", cases[i].what, gpu_result_string(result));
        }
        CHECK((result == GPU_OK) == cases[i].accepted);
        CHECK(cases[i].accepted ? image.pixels != NULL : result == GPU_ERR_ARGUMENT && image.pixels == NULL);
        gpu_image_free(&image);
    }
    /* no output at all, and an output with the scissor off, are the unscissored draw */
    gpu_image plain = {0};
    gpu_image off = {0};
    gpu_vsh_draw without = draw;
    without.output = NULL;
    CHECK(gpu_vsh_render(device, WIDTH, HEIGHT, clear, &without, &plain) == GPU_OK);
    output.scissor = false;
    output.scissor_width = 3u;
    CHECK(gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &off) == GPU_OK);
    CHECK(identical(&plain, &off));
    gpu_image_free(&plain);
    gpu_image_free(&off);
}

/* --- cull pixels -------------------------------------------------------------------------- */

static gpu_pgraph_result replay_cull(gpu_device *device, const stream_builder *stream, bool flip_y,
                                     gpu_image *frame, gpu_pgraph_report *report)
{
    return replay_frame(device, stream, GPU_PGRAPH_OUTPUT_CULL, flip_y, frame, report);
}

static bool all_clear(const gpu_image *image)
{
    for (uint32_t y = 0u; y < image->height; y++) {
        for (uint32_t x = 0u; x < image->width; x++) {
            if (!pixel_is(image, x, y, 51u, 51u, 51u, 255u)) {
                return false;
            }
        }
    }
    return true;
}

static void stream_one_triangle(stream_builder *stream, uint32_t first_vertex)
{
    stream_draw_arrays(stream, GPU_PGRAPH_OP_TRIANGLES, first_vertex, 3u);
}

static void test_cull_pixels(gpu_device *device)
{
    printf("test_cull_pixels (%s)\n", gpu_device_name(device));
    gpu_pgraph_report report;
    gpu_image clockwise = {0};
    gpu_image counter = {0};
    gpu_image clockwise_flipped = {0};
    gpu_image counter_flipped = {0};
    for (int flipped = 0; flipped < 2; flipped++) {
        stream_builder plain = {0};
        stream_setup(&plain);
        stream_one_triangle(&plain, 0u);
        CHECK(replay_frame(device, &plain, 0u, flipped != 0, flipped ? &clockwise_flipped : &clockwise, &report) ==
              GPU_PGRAPH_OK);
        stream_free(&plain);
        stream_setup(&plain);
        stream_one_triangle(&plain, 6u);
        CHECK(replay_frame(device, &plain, 0u, flipped != 0, flipped ? &counter_flipped : &counter, &report) ==
              GPU_PGRAPH_OK);
        stream_free(&plain);
    }
    CHECK(count_red(&clockwise) > 1500u && count_red(&counter) > 1000u);
    CHECK(!identical(&clockwise, &counter) && !identical(&clockwise, &clockwise_flipped));

    /* every (front face, cull mode) pair, with and without flip_y, for a clockwise and a counter-clockwise
     * triangle. A triangle is drawn iff (it is a front face) == (the cull face is BACK). */
    static const uint32_t fronts[2] = {0x900u, 0x901u};
    static const uint32_t modes[2] = {0x900u, 0x901u};
    uint32_t drawn_total = 0u;
    uint32_t culled_total = 0u;
    for (int flipped = 0; flipped < 2; flipped++) {
        for (size_t f = 0u; f < 2u; f++) {
            for (size_t m = 0u; m < 2u; m++) {
                for (int triangle = 0; triangle < 2; triangle++) { /* 0 clockwise, 1 counter-clockwise */
                    stream_builder stream = {0};
                    stream_setup(&stream);
                    stream_front_face(&stream, fronts[f]);
                    stream_cull_mode(&stream, modes[m], fronts[f]);
                    stream_one_triangle(&stream, triangle == 0 ? 0u : 6u);
                    gpu_image frame = {0};
                    CHECK(replay_cull(device, &stream, flipped != 0, &frame, &report) == GPU_PGRAPH_OK);
                    const bool clockwise_on_image = (triangle == 0) != (flipped != 0);
                    /* T1227: NV2A CW (0x900) is the COUNTER-clockwise of the unmirrored finished image (xemu-level, HQ49) */
                    const bool front_face = clockwise_on_image != (fronts[f] == 0x900u);
                    const bool cull_back = modes[m] != fronts[f];
                    const bool drawn = front_face == cull_back;
                    const gpu_image *expected = triangle == 0 ? (flipped ? &clockwise_flipped : &clockwise)
                                                              : (flipped ? &counter_flipped : &counter);
                    if (drawn) {
                        CHECK(identical(&frame, expected));
                        drawn_total++;
                    } else {
                        CHECK(all_clear(&frame));
                        culled_total++;
                    }
                    CHECK(report.drawn == 1u);
                    CHECK(report.used_inferences == (PLAIN_INFERENCES | GPU_PGRAPH_INFER_OUTPUT_CULL_WINDING));
                    gpu_image_free(&frame);
                    stream_free(&stream);
                }
            }
        }
    }
    CHECK(drawn_total == 8u && culled_total == 8u); /* both outcomes happened, half each */

    /* culling off draws both and uses no inference of its own */
    for (int triangle = 0; triangle < 2; triangle++) {
        stream_builder stream = {0};
        stream_setup(&stream);
        stream_cull_mode(&stream, 0u, 0x900u);
        stream_one_triangle(&stream, triangle == 0 ? 0u : 6u);
        gpu_image frame = {0};
        CHECK(replay_cull(device, &stream, false, &frame, &report) == GPU_PGRAPH_OK);
        CHECK(identical(&frame, triangle == 0 ? &clockwise : &counter));
        CHECK(report.used_inferences == PLAIN_INFERENCES);
        gpu_image_free(&frame);
        stream_free(&stream);
    }

    /* a changing cull state inside one frame: each draw is culled by the state current at its own end */
    stream_builder sequence = {0};
    stream_setup(&sequence);
    stream_front_face(&sequence, 0x900u);
    stream_cull_mode(&sequence, 0x901u, 0x900u);  /* cull BACK: the image's counter-clockwise triangle (NV2A CW front) drawn */
    stream_one_triangle(&sequence, 6u);
    stream_cull_mode(&sequence, 0x900u, 0x900u);  /* cull FRONT: the image's clockwise triangle drawn */
    stream_one_triangle(&sequence, 0u);
    stream_one_triangle(&sequence, 6u);           /* counter-clockwise again: now culled, adds nothing */
    gpu_image frame = {0};
    CHECK(replay_cull(device, &sequence, false, &frame, &report) == GPU_PGRAPH_OK);
    CHECK(report.drawn == 3u);
    bool union_exact = true;
    for (uint32_t y = 0u; y < HEIGHT; y++) {
        for (uint32_t x = 0u; x < WIDTH; x++) {
            const bool a = pixel_is(&clockwise, x, y, 255u, 0u, 0u, 255u);
            const bool b = pixel_is(&counter, x, y, 255u, 0u, 0u, 255u);
            union_exact = union_exact && (a || b ? pixel_is(&frame, x, y, 255u, 0u, 0u, 255u)
                                                  : pixel_is(&frame, x, y, 51u, 51u, 51u, 255u));
        }
    }
    CHECK(union_exact);
    gpu_image_free(&frame);
    stream_free(&sequence);

    /* a cull stream with the group off in the backend is refused through the whole replay */
    stream_builder refused = {0};
    stream_setup(&refused);
    stream_front_face(&refused, 0x900u);
    stream_cull_mode(&refused, 0x901u, 0x900u);
    stream_one_triangle(&refused, 0u);
    CHECK(replay_frame(device, &refused, GPU_PGRAPH_OUTPUT_CULL, false, &frame, &report) == GPU_PGRAPH_OK);
    gpu_image_free(&frame);
    fill_memory();
    fake_guest guest = {MEMORY_BASE, memory, sizeof memory};
    gpu_pgraph_backend backend = make_backend(&guest, GPU_PGRAPH_OUTPUT_SCISSOR, GPU_PGRAPH_INFER_OUTPUT_ALL);
    gpu_pgraph *model = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(model, GPU_PGRAPH_OUTPUT_CULL);
    CHECK(gpu_pgraph_decode(model, refused.pairs, refused.count) == GPU_PGRAPH_OK);
    memset(&frame, 0, sizeof frame);
    CHECK(gpu_pgraph_replay(model, device, &backend, WIDTH, HEIGHT, clear_grey, &frame, &report) ==
          GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "the stream set cull state") != NULL && frame.pixels == NULL);
    gpu_pgraph_destroy(model);
    stream_free(&refused);

    gpu_image_free(&clockwise);
    gpu_image_free(&counter);
    gpu_image_free(&clockwise_flipped);
    gpu_image_free(&counter_flipped);
}

/* gpu_vsh_render takes only the cull modes it names. */
static void test_cull_bounds(gpu_device *device)
{
    printf("test_cull_bounds (%s)\n", gpu_device_name(device));
    float attributes[3 * GPU_VSH_ATTRIBUTE_FLOATS];
    memset(attributes, 0, sizeof attributes);
    static const float corners[3][2] = {{-0.9f, -0.9f}, {0.9f, -0.9f}, {-0.9f, 0.9f}};
    for (uint32_t i = 0u; i < 3u; i++) {
        float *vertex = attributes + i * GPU_VSH_ATTRIBUTE_FLOATS;
        vertex[4] = corners[i][0];
        vertex[5] = corners[i][1];
        vertex[6] = 0.5f;
        vertex[7] = 1.0f;
        vertex[10] = 1.0f;
        vertex[11] = 1.0f;
    }
    float constants[GPU_VSH_CONSTANT_ROWS * 4u];
    memset(constants, 0, sizeof constants);
    gpu_vsh_output output = {0};
    const gpu_vsh_draw draw = {
        .words = draw_vertex_words,
        .word_count = sizeof draw_vertex_words / sizeof(uint32_t),
        .vertex_count = 3u,
        .attributes = attributes,
        .constants = constants,
        .output = &output,
    };
    static const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (uint32_t mode = 0u; mode < 5u; mode++) {
        output.cull_mode = mode;
        gpu_image image = {0};
        const gpu_result result = gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &image);
        CHECK(mode <= GPU_VSH_CULL_BACK ? result == GPU_OK : result == GPU_ERR_ARGUMENT);
        CHECK((mode <= GPU_VSH_CULL_BACK) == (image.pixels != NULL));
        gpu_image_free(&image);
    }
    output.cull_mode = GPU_VSH_CULL_NONE;
    output.front_clockwise = true; /* the winding alone culls nothing */
    gpu_image image = {0};
    CHECK(gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &image) == GPU_OK);
    uint32_t covered = 0u;
    for (uint32_t i = 0u; i < WIDTH * HEIGHT; i++) {
        covered += image.pixels[i * 4u] == 255u;
    }
    CHECK(covered > 1500u);
    gpu_image_free(&image);
}

/* --- blend, colour mask and alpha test pixels ---------------------------------------------- */

static float clamp01(float value)
{
    return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

/* The value of a blend factor for one channel (0..2 colour, 3 alpha), by the definition of the factor. */
static float factor_value(uint32_t factor, const float s[4], const float d[4], const float c[4], int channel)
{
    switch (factor) {
    case GPU_VSH_BLEND_ZERO: return 0.0f;
    case GPU_VSH_BLEND_ONE: return 1.0f;
    case GPU_VSH_BLEND_SRC_COLOR: return s[channel];
    case GPU_VSH_BLEND_ONE_MINUS_SRC_COLOR: return 1.0f - s[channel];
    case GPU_VSH_BLEND_SRC_ALPHA: return s[3];
    case GPU_VSH_BLEND_ONE_MINUS_SRC_ALPHA: return 1.0f - s[3];
    case GPU_VSH_BLEND_DST_ALPHA: return d[3];
    case GPU_VSH_BLEND_ONE_MINUS_DST_ALPHA: return 1.0f - d[3];
    case GPU_VSH_BLEND_DST_COLOR: return d[channel];
    case GPU_VSH_BLEND_ONE_MINUS_DST_COLOR: return 1.0f - d[channel];
    case GPU_VSH_BLEND_SRC_ALPHA_SATURATE: {
        const float f = s[3] < 1.0f - d[3] ? s[3] : 1.0f - d[3];
        return channel < 3 ? f : 1.0f;
    }
    case GPU_VSH_BLEND_CONSTANT_COLOR: return c[channel];
    case GPU_VSH_BLEND_ONE_MINUS_CONSTANT_COLOR: return 1.0f - c[channel];
    case GPU_VSH_BLEND_CONSTANT_ALPHA: return c[3];
    default: return 1.0f - c[3];
    }
}

static const uint32_t nv_factor[GPU_VSH_BLEND_FACTORS] = {
    0x0000u, 0x0001u, 0x0300u, 0x0301u, 0x0302u, 0x0303u, 0x0304u, 0x0305u,
    0x0306u, 0x0307u, 0x0308u, 0x8001u, 0x8002u, 0x8003u, 0x8004u,
};
static const uint32_t nv_equation[GPU_VSH_BLEND_OPS] = {0x8006u, 0x800Au, 0x800Bu, 0x8007u, 0x8008u};

static bool near_byte(const uint8_t *pixel, const float expected[4], int tolerance)
{
    for (int channel = 0; channel < 4; channel++) {
        const int want = (int)(clamp01(expected[channel]) * 255.0f + 0.5f);
        const int got = pixel[channel];
        if (got < want - tolerance || got > want + tolerance) {
            return false;
        }
    }
    return true;
}

/* Every pixel of the frame is either the expected blend result (within `tolerance`, over the covered
 * triangle) or exactly the clear colour (outside it). Returns the number of covered pixels, 0 when a pixel
 * is wrong. `covered` is a plain render of the same triangle, whose non-clear pixels are the coverage. */
static uint32_t check_blended(const gpu_image *frame, const gpu_image *covered, const float expected[4],
                              int tolerance)
{
    uint32_t count = 0u;
    for (uint32_t y = 0u; y < HEIGHT; y++) {
        for (uint32_t x = 0u; x < WIDTH; x++) {
            const uint8_t *pixel = frame->pixels + gpu_image_offset(frame, x, y);
            if (pixel_is(covered, x, y, 51u, 51u, 51u, 255u)) {
                if (!pixel_is(frame, x, y, 51u, 51u, 51u, 255u)) {
                    return 0u;
                }
            } else {
                if (!near_byte(pixel, expected, tolerance)) {
                    return 0u;
                }
                count++;
            }
        }
    }
    return count;
}

static void stream_blend_scene(stream_builder *stream, const float destination[4], const float source[4])
{
    stream_setup(stream);
    /* oD0 = v2.zyxw + c3 = (1, 0, 0, 1) + c3: pick c3 so oD0 is the colour asked for */
    stream_colour_constant(stream, destination[0] - 1.0f, destination[1], destination[2], destination[3] - 1.0f);
    stream_one_triangle(stream, 0u);
    (void)source;
}

static void test_blend_pixels(gpu_device *device)
{
    printf("test_blend_pixels (%s)\n", gpu_device_name(device));
    gpu_pgraph_report report;
    static const float destination[4] = {0.2f, 0.6f, 0.9f, 0.7f};
    static const float source[4] = {0.8f, 0.4f, 0.2f, 0.6f};
    static const float constant[4] = {1.0f, 128.0f / 255.0f, 0.0f, 128.0f / 255.0f}; /* 0x80FF8000, ARGB */

    /* the destination alone, to read the bytes the blend sees, and the coverage of the triangle */
    stream_builder first = {0};
    stream_blend_scene(&first, destination, source);
    gpu_image under = {0};
    CHECK(replay_frame(device, &first, 0u, false, &under, &report) == GPU_PGRAPH_OK);
    float seen[4];
    const uint8_t *middle = under.pixels + gpu_image_offset(&under, 16u, 16u);
    for (int channel = 0; channel < 4; channel++) {
        seen[channel] = (float)middle[channel] / 255.0f;
    }
    CHECK(middle[0] == 51u && middle[1] == 153u && middle[3] >= 178u && middle[3] <= 179u); /* the colour asked for */
    stream_free(&first);

    /* every source factor and every destination factor against the definition of the factor */
    uint32_t covered_total = 0u;
    for (uint32_t factor = 0u; factor < GPU_VSH_BLEND_FACTORS; factor++) {
        for (int side = 0; side < 2; side++) {
            stream_builder stream = {0};
            stream_blend_scene(&stream, destination, source);
            stream_blend(&stream, 1u, side == 0 ? nv_factor[factor] : 0x0000u,
                         side == 1 ? nv_factor[factor] : 0x0000u, 0x8006u);
            stream_pair(&stream, 0x034Cu, 0x80FF8000u);
            stream_colour_constant(&stream, source[0] - 1.0f, source[1], source[2], source[3] - 1.0f);
            stream_one_triangle(&stream, 0u);
            gpu_image frame = {0};
            CHECK(replay_frame(device, &stream, GPU_PGRAPH_OUTPUT_BLEND, false, &frame, &report) ==
                  GPU_PGRAPH_OK);
            float expected[4];
            for (int channel = 0; channel < 4; channel++) {
                expected[channel] = side == 0 ? source[channel] * factor_value(factor, source, seen, constant, channel)
                                              : seen[channel] * factor_value(factor, source, seen, constant, channel);
            }
            const uint32_t covered = check_blended(&frame, &under, expected, 2);
            if (covered == 0u) {
                printf("    factor %u side %d: pixel (16, 16) = %u %u %u %u, wanted %.0f %.0f %.0f %.0f\n",
                       (unsigned)factor, side, middle == NULL ? 0u : frame.pixels[gpu_image_offset(&frame, 16u, 16u)],
                       frame.pixels[gpu_image_offset(&frame, 16u, 16u) + 1u],
                       frame.pixels[gpu_image_offset(&frame, 16u, 16u) + 2u],
                       frame.pixels[gpu_image_offset(&frame, 16u, 16u) + 3u], expected[0] * 255.0f,
                       expected[1] * 255.0f, expected[2] * 255.0f, expected[3] * 255.0f);
            }
            CHECK(covered > 1500u);
            covered_total += covered;
            CHECK(report.used_inferences == (PLAIN_INFERENCES | GPU_PGRAPH_INFER_OUTPUT_BLEND_MODEL));
            gpu_image_free(&frame);
            stream_free(&stream);
        }
    }
    CHECK(covered_total > 30u * 1500u);

    /* every equation with (ONE, ONE): the definition of the equation, per channel, alpha included */
    for (uint32_t equation = 0u; equation < GPU_VSH_BLEND_OPS; equation++) {
        stream_builder stream = {0};
        stream_blend_scene(&stream, destination, source);
        stream_blend(&stream, 1u, 0x0001u, 0x0001u, nv_equation[equation]);
        stream_colour_constant(&stream, source[0] - 1.0f, source[1], source[2], source[3] - 1.0f);
        stream_one_triangle(&stream, 0u);
        gpu_image frame = {0};
        CHECK(replay_frame(device, &stream, GPU_PGRAPH_OUTPUT_BLEND, false, &frame, &report) == GPU_PGRAPH_OK);
        float expected[4];
        for (int channel = 0; channel < 4; channel++) {
            const float s = source[channel];
            const float d = seen[channel];
            expected[channel] = equation == GPU_VSH_BLEND_OP_ADD               ? s + d
                                : equation == GPU_VSH_BLEND_OP_SUBTRACT         ? s - d
                                : equation == GPU_VSH_BLEND_OP_REVERSE_SUBTRACT ? d - s
                                : equation == GPU_VSH_BLEND_OP_MIN              ? (s < d ? s : d)
                                                                                : (s > d ? s : d);
        }
        CHECK(check_blended(&frame, &under, expected, 2) > 1500u);
        gpu_image_free(&frame);
        stream_free(&stream);
    }

    /* the usual one, SRC_ALPHA over ONE_MINUS_SRC_ALPHA, with alpha 0 (the draw vanishes) and 1 (it replaces) */
    for (int alpha = 0; alpha < 2; alpha++) {
        stream_builder stream = {0};
        stream_blend_scene(&stream, destination, source);
        stream_blend(&stream, 1u, 0x0302u, 0x0303u, 0x8006u);
        stream_colour_constant(&stream, -0.5f, 0.5f, 0.5f, alpha == 0 ? -1.0f : 0.0f); /* oD0 = (0.5, 0.5, 0.5, a) */
        stream_one_triangle(&stream, 0u);
        gpu_image frame = {0};
        CHECK(replay_frame(device, &stream, GPU_PGRAPH_OUTPUT_BLEND, false, &frame, &report) == GPU_PGRAPH_OK);
        const float expected_replace[4] = {0.5f, 0.5f, 0.5f, 1.0f};
        CHECK(check_blended(&frame, &under, alpha == 0 ? seen : expected_replace, 1) > 1500u);
        gpu_image_free(&frame);
        stream_free(&stream);
    }

    /* blending is stateful across draws and a later draw without it is the plain path again: a blended
     * additive draw, then blending OFF and a plain draw of the other triangle on top */
    stream_builder mixed = {0};
    stream_blend_scene(&mixed, destination, source);
    stream_blend(&mixed, 1u, 0x0001u, 0x0001u, 0x8006u);
    stream_colour_constant(&mixed, source[0] - 1.0f, source[1], source[2], source[3] - 1.0f);
    stream_one_triangle(&mixed, 0u);
    stream_blend(&mixed, 0u, 0x0001u, 0x0001u, 0x8006u);
    stream_colour_constant(&mixed, 0.0f, 0.0f, 0.0f, 0.0f); /* oD0 = (1, 0, 0, 1) */
    stream_one_triangle(&mixed, 6u);
    gpu_image frame = {0};
    CHECK(replay_frame(device, &mixed, GPU_PGRAPH_OUTPUT_BLEND, false, &frame, &report) == GPU_PGRAPH_OK);
    CHECK(report.drawn == 3u);
    stream_builder lower = {0};
    stream_setup(&lower);
    stream_one_triangle(&lower, 6u);
    gpu_image lower_frame = {0};
    CHECK(replay_frame(device, &lower, 0u, false, &lower_frame, &report) == GPU_PGRAPH_OK);
    bool exact = true;
    uint32_t red_pixels = 0u;
    uint32_t blended_pixels = 0u;
    for (uint32_t y = 0u; y < HEIGHT; y++) {
        for (uint32_t x = 0u; x < WIDTH; x++) {
            if (!pixel_is(&lower_frame, x, y, 51u, 51u, 51u, 255u)) {
                exact = exact && pixel_is(&frame, x, y, 255u, 0u, 0u, 255u);
                red_pixels++;
            } else if (!pixel_is(&under, x, y, 51u, 51u, 51u, 255u)) {
                float expected[4];
                for (int channel = 0; channel < 4; channel++) {
                    expected[channel] = source[channel] + seen[channel];
                }
                exact = exact && near_byte(frame.pixels + gpu_image_offset(&frame, x, y), expected, 2);
                blended_pixels++;
            } else {
                exact = exact && pixel_is(&frame, x, y, 51u, 51u, 51u, 255u);
            }
        }
    }
    CHECK(exact && red_pixels > 1000u && blended_pixels > 1500u);
    gpu_image_free(&frame);
    gpu_image_free(&lower_frame);
    stream_free(&lower);
    stream_free(&mixed);

    /* the blend colour feeds a constant factor: (CONSTANT_COLOR, ZERO) of a plain red source */
    stream_builder tinted = {0};
    stream_setup(&tinted);
    stream_blend(&tinted, 1u, 0x8001u, 0x0000u, 0x8006u);
    stream_pair(&tinted, 0x034Cu, 0x80FF8000u);
    stream_one_triangle(&tinted, 0u);
    CHECK(replay_frame(device, &tinted, GPU_PGRAPH_OUTPUT_BLEND, false, &frame, &report) == GPU_PGRAPH_OK);
    const float tinted_expected[4] = {1.0f * constant[0], 0.0f, 0.0f, 1.0f * constant[3]};
    CHECK(check_blended(&frame, &under, tinted_expected, 1) > 1500u);
    gpu_image_free(&frame);
    stream_free(&tinted);

    /* colour mask: each channel kept from the destination when masked off, written when on, all 16 patterns */
    for (uint32_t pattern = 0u; pattern < 16u; pattern++) {
        const uint32_t mask = ((pattern & 1u) != 0u ? 0x00010000u : 0u) | ((pattern & 2u) != 0u ? 0x00000100u : 0u) |
                              ((pattern & 4u) != 0u ? 0x00000001u : 0u) | ((pattern & 8u) != 0u ? 0x01000000u : 0u);
        stream_builder stream = {0};
        stream_blend_scene(&stream, destination, source);
        stream_pair(&stream, 0x0358u, mask);
        stream_colour_constant(&stream, source[0] - 1.0f, source[1], source[2], source[3] - 1.0f);
        stream_one_triangle(&stream, 0u);
        CHECK(replay_frame(device, &stream, GPU_PGRAPH_OUTPUT_BLEND, false, &frame, &report) == GPU_PGRAPH_OK);
        float expected[4];
        for (int channel = 0; channel < 4; channel++) {
            const bool written = (pattern & (1u << (channel == 3 ? 3 : channel))) != 0u;
            expected[channel] = written ? source[channel] : seen[channel];
        }
        const uint32_t covered = check_blended(&frame, &under, expected, 1);
        CHECK(covered > 1500u);
        CHECK(report.used_inferences == (PLAIN_INFERENCES | (pattern == 15u ? 0u : GPU_PGRAPH_INFER_OUTPUT_BLEND_MODEL)));
        gpu_image_free(&frame);
        stream_free(&stream);
    }
    gpu_image_free(&under);
}

static void test_alpha_test_pixels(gpu_device *device)
{
    printf("test_alpha_test_pixels (%s)\n", gpu_device_name(device));
    gpu_pgraph_report report;
    /* the fragment alpha is 0.5, which is the byte 128 */
    stream_builder plain = {0};
    stream_setup(&plain);
    stream_colour_constant(&plain, 0.0f, 0.0f, 0.0f, -0.5f);
    stream_one_triangle(&plain, 0u);
    gpu_image reference = {0};
    CHECK(replay_frame(device, &plain, 0u, false, &reference, &report) == GPU_PGRAPH_OK);
    const uint8_t *centre = reference.pixels + gpu_image_offset(&reference, 16u, 16u);
    CHECK(centre[0] == 255u && centre[1] == 0u && centre[2] == 0u && centre[3] == 128u);
    stream_free(&plain);

    uint32_t passed_total = 0u;
    uint32_t failed_total = 0u;
    static const uint32_t references[] = {0u, 127u, 128u, 129u, 255u};
    for (uint32_t function = 0u; function < 8u; function++) {
        for (size_t r = 0u; r < sizeof references / sizeof references[0]; r++) {
            const uint32_t ref = references[r];
            const uint32_t alpha = 128u;
            const bool pass = function == 0u ? false : function == 1u ? alpha < ref : function == 2u ? alpha == ref
                              : function == 3u ? alpha <= ref : function == 4u ? alpha > ref
                              : function == 5u ? alpha != ref : function == 6u ? alpha >= ref : true;
            stream_builder stream = {0};
            stream_setup(&stream);
            stream_alpha_test(&stream, 1u, 0x0200u + function, ref);
            stream_colour_constant(&stream, 0.0f, 0.0f, 0.0f, -0.5f);
            stream_one_triangle(&stream, 0u);
            gpu_image frame = {0};
            CHECK(replay_frame(device, &stream, GPU_PGRAPH_OUTPUT_ALPHA_TEST, false, &frame, &report) ==
                  GPU_PGRAPH_OK);
            if (pass) {
                CHECK(identical(&frame, &reference));
                passed_total++;
            } else {
                CHECK(all_clear(&frame));
                failed_total++;
            }
            CHECK(report.drawn == 1u);
            CHECK(report.used_inferences ==
                  (PLAIN_INFERENCES | (function == 7u ? 0u : GPU_PGRAPH_INFER_OUTPUT_ALPHA_TEST_MODEL)));
            gpu_image_free(&frame);
            stream_free(&stream);
        }
    }
    CHECK(passed_total > 10u && failed_total > 10u); /* both outcomes, many times */

    /* the test is disabled by its enable alone, whatever the function and reference say */
    stream_builder disabled = {0};
    stream_setup(&disabled);
    stream_alpha_test(&disabled, 0u, 0x0200u, 255u); /* NEVER, but off */
    stream_colour_constant(&disabled, 0.0f, 0.0f, 0.0f, -0.5f);
    stream_one_triangle(&disabled, 0u);
    gpu_image frame = {0};
    CHECK(replay_frame(device, &disabled, GPU_PGRAPH_OUTPUT_ALPHA_TEST, false, &frame, &report) == GPU_PGRAPH_OK);
    CHECK(identical(&frame, &reference));
    gpu_image_free(&frame);
    stream_free(&disabled);

    /* alpha near the boundaries of the byte: 0.0 (byte 0) and 1.0 (byte 255), and a negative alpha that
     * clamps to 0 */
    static const struct {
        float c3_alpha;
        uint32_t byte;
    } alphas[] = {{-1.0f, 0u}, {0.0f, 255u}, {-2.0f, 0u}, {-0.9f, 26u}};
    for (size_t i = 0u; i < sizeof alphas / sizeof alphas[0]; i++) {
        for (int less = 0; less < 2; less++) {
            /* GEQUAL the byte itself passes, GREATER than it fails */
            stream_builder stream = {0};
            stream_setup(&stream);
            stream_alpha_test(&stream, 1u, less == 0 ? 0x0206u : 0x0204u, alphas[i].byte);
            stream_colour_constant(&stream, 0.0f, 0.0f, 0.0f, alphas[i].c3_alpha);
            stream_one_triangle(&stream, 0u);
            CHECK(replay_frame(device, &stream, GPU_PGRAPH_OUTPUT_ALPHA_TEST, false, &frame, &report) ==
                  GPU_PGRAPH_OK);
            if (less == 0) {
                CHECK(!all_clear(&frame)); /* GEQUAL the byte itself passes */
            } else {
                CHECK(all_clear(&frame)); /* GREATER than the byte itself fails */
            }
            gpu_image_free(&frame);
            stream_free(&stream);
        }
    }

    /* a failed alpha test discards before the blend: the destination is untouched, and a passed one blends */
    for (int pass = 0; pass < 2; pass++) {
        stream_builder stream = {0};
        stream_setup(&stream);
        stream_colour_constant(&stream, -0.5f, 0.0f, 0.0f, 0.0f); /* oD0 = (0.5, 0, 0, 1) */
        stream_one_triangle(&stream, 0u);
        stream_blend(&stream, 1u, 0x0001u, 0x0001u, 0x8006u);
        stream_alpha_test(&stream, 1u, pass == 0 ? 0x0204u : 0x0201u, 200u); /* alpha byte 128: GREATER 200 fails */
        stream_colour_constant(&stream, 0.0f, 0.0f, 0.0f, -0.5f);            /* oD0 = (1, 0, 0, 0.5) */
        stream_one_triangle(&stream, 0u);
        CHECK(replay_frame(device, &stream, GPU_PGRAPH_OUTPUT_BLEND | GPU_PGRAPH_OUTPUT_ALPHA_TEST, false,
                           &frame, &report) == GPU_PGRAPH_OK);
        const uint8_t *pixel = frame.pixels + gpu_image_offset(&frame, 16u, 16u);
        if (pass == 0) {
            CHECK(pixel[0] == 128u && pixel[1] == 0u && pixel[3] == 255u); /* untouched: the first draw */
        } else {
            CHECK(pixel[0] == 255u && pixel[3] == 255u); /* 0.5 + 1.0 clamps, alpha 1 + 0.5 clamps */
        }
        gpu_image_free(&frame);
        stream_free(&stream);
    }
    gpu_image_free(&reference);
}

/* gpu_vsh_render's own checks of the new fields, and the destination upload, called directly. */
static void test_state_bounds(gpu_device *device)
{
    printf("test_state_bounds (%s)\n", gpu_device_name(device));
    float attributes[3 * GPU_VSH_ATTRIBUTE_FLOATS];
    memset(attributes, 0, sizeof attributes);
    static const float corners[3][2] = {{-0.9f, -0.9f}, {0.9f, -0.9f}, {-0.9f, 0.9f}};
    for (uint32_t i = 0u; i < 3u; i++) {
        float *vertex = attributes + i * GPU_VSH_ATTRIBUTE_FLOATS;
        vertex[4] = corners[i][0];
        vertex[5] = corners[i][1];
        vertex[6] = 0.5f;
        vertex[7] = 1.0f;
        vertex[10] = 1.0f;
        vertex[11] = 1.0f;
    }
    float constants[GPU_VSH_CONSTANT_ROWS * 4u];
    memset(constants, 0, sizeof constants);
    gpu_vsh_output output = {0};
    gpu_vsh_draw draw = {
        .words = draw_vertex_words,
        .word_count = sizeof draw_vertex_words / sizeof(uint32_t),
        .vertex_count = 3u,
        .attributes = attributes,
        .constants = constants,
        .output = &output,
    };
    static const float clear[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    gpu_image image = {0};

    /* out of range factors, equations, masks, alpha functions and references are refused, in range ones are not */
    output.blend = true;
    output.blend_source = GPU_VSH_BLEND_FACTORS;
    CHECK(gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &image) == GPU_ERR_ARGUMENT && image.pixels == NULL);
    output.blend_source = GPU_VSH_BLEND_FACTORS - 1u;
    output.blend_destination = GPU_VSH_BLEND_FACTORS;
    CHECK(gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &image) == GPU_ERR_ARGUMENT);
    output.blend_destination = GPU_VSH_BLEND_FACTORS - 1u;
    output.blend_equation = GPU_VSH_BLEND_OPS;
    CHECK(gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &image) == GPU_ERR_ARGUMENT);
    output.blend_equation = GPU_VSH_BLEND_OPS - 1u;
    CHECK(gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &image) == GPU_OK);
    gpu_image_free(&image);
    output.blend = false; /* factors are not looked at without the blend */
    output.blend_source = 99u;
    CHECK(gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &image) == GPU_OK);
    gpu_image_free(&image);
    output.blend_source = 0u;
    output.color_write_disable = 16u;
    CHECK(gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &image) == GPU_ERR_ARGUMENT);
    output.color_write_disable = 15u;
    CHECK(gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &image) == GPU_OK);
    gpu_image_free(&image);
    output.color_write_disable = 0u;
    output.alpha_test = true;
    output.alpha_func = GPU_VSH_COMPARE_ALWAYS + 1u;
    CHECK(gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &image) == GPU_ERR_ARGUMENT);
    output.alpha_func = GPU_VSH_COMPARE_ALWAYS;
    output.alpha_ref = 256u;
    CHECK(gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &image) == GPU_ERR_ARGUMENT);
    output.alpha_ref = 255u;
    CHECK(gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &image) == GPU_OK);
    gpu_image_free(&image);
    /* an alpha test needs the fixed fragment stage */
    static const float fragment_constants[GPU_VSH_FRAGMENT_VEC4S * 4u] = {0};
    const gpu_vsh_fragment fragment = {
        .words = draw_vertex_words,
        .word_count = sizeof draw_vertex_words / sizeof(uint32_t),
        .constants = fragment_constants,
    };
    draw.fragment = &fragment;
    CHECK(gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &image) == GPU_ERR_ARGUMENT);
    draw.fragment = NULL;
    output.alpha_test = false;

    /* the destination: the target starts as those pixels and the clear colour is not used */
    uint8_t *destination = malloc((size_t)WIDTH * HEIGHT * 4u);
    CHECK(destination != NULL);
    for (uint32_t i = 0u; i < WIDTH * HEIGHT; i++) {
        destination[i * 4u + 0u] = (uint8_t)(i & 0xFFu);
        destination[i * 4u + 1u] = (uint8_t)(i >> 4);
        destination[i * 4u + 2u] = 30u;
        destination[i * 4u + 3u] = 40u;
    }
    output.destination = destination;
    CHECK(gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &image) == GPU_OK);
    gpu_image without = {0};
    output.destination = NULL;
    CHECK(gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &without) == GPU_OK);
    bool exact = image.pixels != NULL && without.pixels != NULL;
    uint32_t drawn = 0u;
    for (uint32_t i = 0u; exact && i < WIDTH * HEIGHT; i++) {
        const bool covered = without.pixels[i * 4u + 0u] == 255u && without.pixels[i * 4u + 1u] == 0u;
        if (covered) {
            drawn++;
            exact = exact && image.pixels[i * 4u + 0u] == 255u && image.pixels[i * 4u + 1u] == 0u &&
                    image.pixels[i * 4u + 2u] == 0u && image.pixels[i * 4u + 3u] == 255u;
        } else {
            exact = exact && memcmp(image.pixels + i * 4u, destination + i * 4u, 4u) == 0;
        }
    }
    CHECK(exact && drawn > 1500u);
    gpu_image_free(&image);
    gpu_image_free(&without);
    free(destination);
}

/* --- depth and stencil pixels ---------------------------------------------------------------- */

#define COLOUR_RED 0.0f, 0.0f, 0.0f, 0.0f        /* c3 for oD0 = (1, 0, 0, 1) */
#define COLOUR_GREEN -1.0f, 1.0f, 0.0f, 0.0f     /* oD0 = (0, 1, 0, 1) */
#define COLOUR_BLUE -1.0f, 0.0f, 1.0f, 0.0f      /* oD0 = (0, 0, 1, 1) */
#define NEAR_TRIANGLE 9u
#define FAR_TRIANGLE 12u
#define MID_TRIANGLE 0u

/* A frame holding only the upper left triangle (at any depth) in the colour of `c3`, as a plain render. */
static void render_plain(gpu_device *device, float r, float g, float b, float a, bool flip_y, gpu_image *image)
{
    gpu_pgraph_report report;
    stream_builder stream = {0};
    stream_setup(&stream);
    stream_colour_constant(&stream, r, g, b, a);
    stream_one_triangle(&stream, MID_TRIANGLE);
    CHECK(replay_frame(device, &stream, 0u, flip_y, image, &report) == GPU_PGRAPH_OK);
    stream_free(&stream);
}

static void test_depth_pixels(gpu_device *device)
{
    printf("test_depth_pixels (%s)\n", gpu_device_name(device));
    gpu_pgraph_report report;
    gpu_image red = {0};
    gpu_image green = {0};
    gpu_image blue = {0};
    render_plain(device, COLOUR_RED, false, &red);
    render_plain(device, COLOUR_GREEN, false, &green);
    render_plain(device, COLOUR_BLUE, false, &blue);
    CHECK(count_red(&red) > 1500u && !identical(&red, &green) && !identical(&green, &blue));

    /* near over far in either order, with LESS: the near one shows. Depth test off: the last one shows */
    for (int near_first = 0; near_first < 2; near_first++) {
        for (int tested = 0; tested < 2; tested++) {
            stream_builder stream = {0};
            stream_setup(&stream);
            stream_depth_state(&stream, (uint32_t)tested, 0x0201u, 1u);
            stream_colour_constant(&stream, COLOUR_GREEN); /* near is green, far is blue */
            if (near_first == 1) {
                stream_one_triangle(&stream, NEAR_TRIANGLE);
                stream_colour_constant(&stream, COLOUR_BLUE);
                stream_one_triangle(&stream, FAR_TRIANGLE);
            } else {
                stream_colour_constant(&stream, COLOUR_BLUE);
                stream_one_triangle(&stream, FAR_TRIANGLE);
                stream_colour_constant(&stream, COLOUR_GREEN);
                stream_one_triangle(&stream, NEAR_TRIANGLE);
            }
            gpu_image frame = {0};
            CHECK(replay_frame(device, &stream, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL, false, &frame, &report) ==
                  GPU_PGRAPH_OK);
            /* tested: green always. untested: whichever was drawn last (near_first: blue) */
            const gpu_image *expected = tested == 1 ? &green : (near_first == 1 ? &blue : &green);
            CHECK(identical(&frame, expected));
            CHECK(report.drawn == 2u);
            CHECK(report.used_inferences ==
                  (PLAIN_INFERENCES | (tested == 1 ? GPU_PGRAPH_INFER_OUTPUT_DEPTH_MODEL : 0u)));
            gpu_image_free(&frame);
            stream_free(&stream);
        }
    }

    /* every function: a first triangle at 0.5 writes its depth (colour red), a second at 0.25, 0.5 or 0.75
     * is drawn green if its depth passes the function against 0.5, and writes no depth */
    static const uint32_t second[3] = {NEAR_TRIANGLE, MID_TRIANGLE, FAR_TRIANGLE};
    static const int order[3] = {-1, 0, 1}; /* the second's depth against 0.5 */
    uint32_t passed = 0u;
    uint32_t failed = 0u;
    for (uint32_t function = 0u; function < 8u; function++) {
        for (int k = 0; k < 3; k++) {
            const int c = order[k];
            const bool pass = function == 0u ? false : function == 1u ? c < 0 : function == 2u ? c == 0
                              : function == 3u ? c <= 0 : function == 4u ? c > 0 : function == 5u ? c != 0
                              : function == 6u ? c >= 0 : true;
            stream_builder stream = {0};
            stream_setup(&stream);
            stream_depth_state(&stream, 1u, 0x0207u, 1u); /* ALWAYS, writing */
            stream_one_triangle(&stream, MID_TRIANGLE);
            stream_depth_state(&stream, 1u, 0x0200u + function, 0u); /* the function under test, no write */
            stream_colour_constant(&stream, COLOUR_GREEN);
            stream_one_triangle(&stream, second[k]);
            gpu_image frame = {0};
            CHECK(replay_frame(device, &stream, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL, false, &frame, &report) ==
                  GPU_PGRAPH_OK);
            if (!identical(&frame, pass ? &green : &red)) {
                printf("    function %u second depth %d: expected %s\n", (unsigned)function, c, pass ? "green" : "red");
            }
            CHECK(identical(&frame, pass ? &green : &red));
            passed += pass ? 1u : 0u;
            failed += pass ? 0u : 1u;
            gpu_image_free(&frame);
            stream_free(&stream);
        }
    }
    CHECK(passed == 12u && failed == 12u); /* the truth table has half of each */

    /* the depth mask: a near draw that writes no depth leaves the far one's depth, so a middle draw passes under
     * LESS; the same draw with the write on hides the middle one */
    for (uint32_t near_mask = 0u; near_mask < 2u; near_mask++) {
        stream_builder stream = {0};
        stream_setup(&stream);
        stream_depth_state(&stream, 1u, 0x0201u, 1u);
        stream_colour_constant(&stream, COLOUR_RED);
        stream_one_triangle(&stream, FAR_TRIANGLE);
        stream_pair(&stream, 0x035Cu, near_mask);
        stream_colour_constant(&stream, COLOUR_GREEN);
        stream_one_triangle(&stream, NEAR_TRIANGLE);
        stream_pair(&stream, 0x035Cu, 1u);
        stream_colour_constant(&stream, COLOUR_BLUE);
        stream_one_triangle(&stream, MID_TRIANGLE);
        gpu_image frame = {0};
        CHECK(replay_frame(device, &stream, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL, false, &frame, &report) == GPU_PGRAPH_OK);
        /* near wrote depth: mid (0.5) fails against 0.25, the frame is green. Near did not: mid passes against 0.75 */
        CHECK(identical(&frame, near_mask == 1u ? &green : &blue));
        gpu_image_free(&frame);
        stream_free(&stream);
    }

    /* depth is per pass: replaying the same decoded draws twice gives the same frame, never a depth carried over */
    stream_builder lone = {0};
    stream_setup(&lone);
    stream_depth_state(&lone, 1u, 0x0201u, 1u);
    stream_colour_constant(&lone, COLOUR_GREEN);
    stream_one_triangle(&lone, FAR_TRIANGLE);
    fill_memory();
    fake_guest guest = {MEMORY_BASE, memory, sizeof memory};
    gpu_pgraph_backend backend = make_backend(&guest, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL, GPU_PGRAPH_INFER_OUTPUT_ALL);
    gpu_pgraph *model = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(model, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL);
    CHECK(gpu_pgraph_decode(model, lone.pairs, lone.count) == GPU_PGRAPH_OK);
    gpu_image first_pass = {0};
    gpu_image second_pass = {0};
    CHECK(gpu_pgraph_replay(model, device, &backend, WIDTH, HEIGHT, clear_grey, &first_pass, &report) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_replay(model, device, &backend, WIDTH, HEIGHT, clear_grey, &second_pass, &report) == GPU_PGRAPH_OK);
    CHECK(identical(&first_pass, &green) && identical(&second_pass, &green));
    gpu_image_free(&first_pass);
    gpu_image_free(&second_pass);
    gpu_pgraph_destroy(model);
    stream_free(&lone);

    /* depth with a scissor: the fragments outside the rectangle write no depth, so a far draw later shows there */
    stream_builder both = {0};
    stream_setup(&both);
    stream_depth_state(&both, 1u, 0x0201u, 1u);
    stream_scissor(&both, 0u, 0u, 32u, 64u, WIDTH, HEIGHT);
    stream_colour_constant(&both, COLOUR_GREEN);
    stream_one_triangle(&both, NEAR_TRIANGLE);
    stream_scissor(&both, 0u, 0u, WIDTH, HEIGHT, WIDTH, HEIGHT);
    stream_colour_constant(&both, COLOUR_BLUE);
    stream_one_triangle(&both, FAR_TRIANGLE);
    gpu_image frame = {0};
    CHECK(replay_frame(device, &both, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL | GPU_PGRAPH_OUTPUT_SCISSOR, false, &frame,
                       &report) == GPU_PGRAPH_OK);
    bool exact = true;
    for (uint32_t y = 0u; y < HEIGHT; y++) {
        for (uint32_t x = 0u; x < WIDTH; x++) {
            exact = exact && same_pixel(&frame, x < 32u ? &green : &blue, x, y);
        }
    }
    CHECK(exact);
    gpu_image_free(&frame);
    stream_free(&both);
    gpu_image_free(&red);
    gpu_image_free(&green);
    gpu_image_free(&blue);
}

static void test_stencil_pixels(gpu_device *device)
{
    printf("test_stencil_pixels (%s)\n", gpu_device_name(device));
    gpu_pgraph_report report;
    gpu_image red = {0};
    gpu_image blue = {0};
    render_plain(device, COLOUR_RED, false, &red);
    render_plain(device, COLOUR_BLUE, false, &blue);
    gpu_image frame = {0};

    /* write a stencil value, then draw only where it is equal, on the other triangle (stencil 0 there) and on
     * the same one */
    stream_builder first = {0};
    stream_setup(&first);
    stream_stencil_state(&first, 1u, 0x0207u, 1u, 0xFFu, 0xFFu, 0x1E00u, 0x1E00u, 0x1E01u); /* ALWAYS, REPLACE 1 */
    stream_one_triangle(&first, MID_TRIANGLE);
    stream_stencil_state(&first, 1u, 0x0202u, 1u, 0xFFu, 0xFFu, 0x1E00u, 0x1E00u, 0x1E00u); /* EQUAL 1, KEEP */
    stream_colour_constant(&first, COLOUR_GREEN);
    stream_one_triangle(&first, 6u); /* lower right: stencil 0 there, so nothing */
    stream_colour_constant(&first, COLOUR_BLUE);
    stream_one_triangle(&first, MID_TRIANGLE); /* the upper left one: stencil 1, drawn blue */
    CHECK(replay_frame(device, &first, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL, false, &frame, &report) == GPU_PGRAPH_OK);
    CHECK(identical(&frame, &blue));
    CHECK(report.used_inferences == (PLAIN_INFERENCES | GPU_PGRAPH_INFER_OUTPUT_STENCIL_MODEL));
    gpu_image_free(&frame);
    stream_free(&first);

    /* every operation, from a known stencil value, probed by an EQUAL draw: the probe shows (blue) exactly when the
     * reference equals what the operation should have left */
    static const struct {
        uint32_t nv;
        uint32_t initial, reference, expected;
        const char *what;
    } cases[] = {
        {0x1E00u, 5u, 9u, 5u, "KEEP"},
        {0x0000u, 5u, 9u, 0u, "ZERO"},
        {0x1E01u, 5u, 9u, 9u, "REPLACE"},
        {0x1E02u, 5u, 9u, 6u, "INCR clamp"},
        {0x1E02u, 255u, 9u, 255u, "INCR clamp at 255"},
        {0x1E03u, 5u, 9u, 4u, "DECR clamp"},
        {0x1E03u, 0u, 9u, 0u, "DECR clamp at 0"},
        {0x150Au, 5u, 9u, 250u, "INVERT"},
        {0x8507u, 5u, 9u, 6u, "INCR wrap"},
        {0x8507u, 255u, 9u, 0u, "INCR wrap at 255"},
        {0x8508u, 5u, 9u, 4u, "DECR wrap"},
        {0x8508u, 0u, 9u, 255u, "DECR wrap at 0"},
    };
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i++) {
        for (int probe_right = 0; probe_right < 2; probe_right++) {
            stream_builder stream = {0};
            stream_setup(&stream);
            /* the starting value: ALWAYS, REPLACE with the initial as the reference */
            stream_stencil_state(&stream, 1u, 0x0207u, cases[i].initial, 0xFFu, 0xFFu, 0x1E00u, 0x1E00u, 0x1E01u);
            stream_one_triangle(&stream, MID_TRIANGLE);
            /* the operation under test on a pass, reference 9 */
            stream_stencil_state(&stream, 1u, 0x0207u, cases[i].reference, 0xFFu, 0xFFu, 0x1E00u, 0x1E00u, cases[i].nv);
            stream_one_triangle(&stream, MID_TRIANGLE);
            /* the probe: EQUAL to the value it should be, or to one more (0 if that wraps) */
            const uint32_t probe = probe_right == 0 ? cases[i].expected : ((cases[i].expected + 1u) & 0xFFu);
            stream_stencil_state(&stream, 1u, 0x0202u, probe, 0xFFu, 0xFFu, 0x1E00u, 0x1E00u, 0x1E00u);
            stream_colour_constant(&stream, COLOUR_BLUE);
            stream_one_triangle(&stream, MID_TRIANGLE);
            CHECK(replay_frame(device, &stream, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL, false, &frame, &report) ==
                  GPU_PGRAPH_OK);
            const bool matched = identical(&frame, probe_right == 0 ? &blue : &red);
            if (!matched) {
                printf("    %s from %u: probe %u (%s) did not %s\n", cases[i].what, (unsigned)cases[i].initial,
                       (unsigned)probe, probe_right == 0 ? "the expected value" : "one more",
                       probe_right == 0 ? "show" : "stay hidden");
            }
            CHECK(matched);
            gpu_image_free(&frame);
            stream_free(&stream);
        }
    }

    /* the write mask and the function mask, four probes of one stencil value (0xFA: 0xF0 written whole, then 0x0A under a
     * write mask of 0x0F). The probes tell the two masks apart: with the full function mask only 0xFA matches, with 0x0F
     * 0x0A and 0x1A do too, and a write mask that were the compare mask would have left 0x0A */
    static const struct {
        uint32_t reference, function_mask;
        bool shown;
    } probes[] = {
        {0xFAu, 0xFFu, true},
        {0x0Au, 0xFFu, false},
        {0x0Au, 0x0Fu, true},
        {0x0Bu, 0x0Fu, false},
        {0x1Au, 0x0Fu, true},
        {0xF0u, 0xF0u, true},
    };
    for (size_t p = 0u; p < sizeof probes / sizeof probes[0]; p++) {
        stream_builder stream = {0};
        stream_setup(&stream);
        stream_stencil_state(&stream, 1u, 0x0207u, 0xF0u, 0xFFu, 0xFFu, 0x1E00u, 0x1E00u, 0x1E01u); /* stencil 0xF0 */
        stream_one_triangle(&stream, MID_TRIANGLE);
        stream_stencil_state(&stream, 1u, 0x0207u, 0x0Au, 0xFFu, 0x0Fu, 0x1E00u, 0x1E00u, 0x1E01u); /* write mask 0x0F */
        stream_one_triangle(&stream, MID_TRIANGLE); /* stencil = 0xF0 & ~0x0F | 0x0A & 0x0F = 0xFA */
        stream_colour_constant(&stream, COLOUR_BLUE);
        stream_stencil_state(&stream, 1u, 0x0202u, probes[p].reference, probes[p].function_mask, 0xFFu, 0x1E00u,
                             0x1E00u, 0x1E00u);
        stream_one_triangle(&stream, MID_TRIANGLE);
        CHECK(replay_frame(device, &stream, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL, false, &frame, &report) == GPU_PGRAPH_OK);
        if (!identical(&frame, probes[p].shown ? &blue : &red)) {
            printf("    probe reference 0x%02X mask 0x%02X: expected %s\n", (unsigned)probes[p].reference,
                   (unsigned)probes[p].function_mask, probes[p].shown ? "shown" : "hidden");
        }
        CHECK(identical(&frame, probes[p].shown ? &blue : &red));
        gpu_image_free(&frame);
        stream_free(&stream);
    }

    /* the stencil state of FRONT faces and of back faces is the same state: the first scenario again on the
     * counter-clockwise triangle, which Vulkan's default winding calls the front */
    stream_builder front = {0};
    stream_setup(&front);
    stream_stencil_state(&front, 1u, 0x0207u, 3u, 0xFFu, 0xFFu, 0x1E00u, 0x1E00u, 0x1E01u);
    stream_one_triangle(&front, 6u);
    stream_stencil_state(&front, 1u, 0x0202u, 3u, 0xFFu, 0xFFu, 0x1E00u, 0x1E00u, 0x1E00u);
    stream_colour_constant(&front, COLOUR_BLUE);
    stream_one_triangle(&front, 6u);
    stream_stencil_state(&front, 1u, 0x0202u, 4u, 0xFFu, 0xFFu, 0x1E00u, 0x1E00u, 0x1E00u);
    stream_colour_constant(&front, COLOUR_GREEN);
    stream_one_triangle(&front, 6u); /* stencil is 3, not 4: hidden */
    gpu_image blue_front = {0};
    stream_builder plain_front = {0};
    stream_setup(&plain_front);
    stream_colour_constant(&plain_front, COLOUR_BLUE);
    stream_one_triangle(&plain_front, 6u);
    CHECK(replay_frame(device, &plain_front, 0u, false, &blue_front, &report) == GPU_PGRAPH_OK);
    CHECK(replay_frame(device, &front, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL, false, &frame, &report) == GPU_PGRAPH_OK);
    CHECK(identical(&frame, &blue_front));
    gpu_image_free(&frame);
    gpu_image_free(&blue_front);
    stream_free(&front);
    stream_free(&plain_front);

    /* the op on a stencil fail, and the op on a depth fail: a NEVER function sends every fragment to the fail
     * op, a failing depth test with a passing stencil to the depth fail op */
    for (int which = 0; which < 2; which++) {
        for (int probe_right = 0; probe_right < 2; probe_right++) {
            stream_builder stream = {0};
            stream_setup(&stream);
            stream_depth_state(&stream, 1u, 0x0201u, 1u); /* LESS, writing */
            stream_stencil_state(&stream, 1u, 0x0207u, 7u, 0xFFu, 0xFFu, 0x1E00u, 0x1E00u, 0x1E01u); /* stencil 7 */
            stream_one_triangle(&stream, NEAR_TRIANGLE);
            if (which == 0) { /* NEVER: the stencil fails, op on fail INCR */
                stream_stencil_state(&stream, 1u, 0x0200u, 0u, 0xFFu, 0xFFu, 0x8507u, 0x1E00u, 0x1E00u);
                stream_one_triangle(&stream, NEAR_TRIANGLE);
            } else { /* ALWAYS, but the far triangle fails depth against the near one: op on depth fail INCR */
                stream_stencil_state(&stream, 1u, 0x0207u, 0u, 0xFFu, 0xFFu, 0x1E00u, 0x8507u, 0x1E00u);
                stream_one_triangle(&stream, FAR_TRIANGLE);
            }
            /* the probe: the depth test is ALWAYS (the near triangle already holds 0.25), so only the stencil decides */
            stream_depth_state(&stream, 1u, 0x0207u, 0u);
            stream_stencil_state(&stream, 1u, 0x0202u, probe_right == 0 ? 8u : 7u, 0xFFu, 0xFFu, 0x1E00u, 0x1E00u, 0x1E00u);
            stream_colour_constant(&stream, COLOUR_BLUE);
            stream_one_triangle(&stream, MID_TRIANGLE);
            CHECK(replay_frame(device, &stream, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL, false, &frame, &report) ==
                  GPU_PGRAPH_OK);
            CHECK(identical(&frame, probe_right == 0 ? &blue : &red));
            gpu_image_free(&frame);
            stream_free(&stream);
        }
    }
    gpu_image_free(&red);
    gpu_image_free(&blue);
}

/* gpu_vsh_render's own checks of the depth and stencil fields, called directly. */
static void test_depth_bounds(gpu_device *device)
{
    printf("test_depth_bounds (%s)\n", gpu_device_name(device));
    float attributes[3 * GPU_VSH_ATTRIBUTE_FLOATS];
    memset(attributes, 0, sizeof attributes);
    static const float corners[3][2] = {{-0.9f, -0.9f}, {0.9f, -0.9f}, {-0.9f, 0.9f}};
    for (uint32_t i = 0u; i < 3u; i++) {
        float *vertex = attributes + i * GPU_VSH_ATTRIBUTE_FLOATS;
        vertex[4] = corners[i][0];
        vertex[5] = corners[i][1];
        vertex[6] = 0.5f;
        vertex[7] = 1.0f;
        vertex[10] = 1.0f;
        vertex[11] = 1.0f;
    }
    float constants[GPU_VSH_CONSTANT_ROWS * 4u];
    memset(constants, 0, sizeof constants);
    float *depth = malloc((size_t)WIDTH * HEIGHT * sizeof *depth);
    uint8_t *stencil = calloc((size_t)WIDTH * HEIGHT, 1u);
    CHECK(depth != NULL && stencil != NULL);
    for (uint32_t i = 0u; i < WIDTH * HEIGHT; i++) {
        depth[i] = 1.0f;
    }
    gpu_vsh_output output = {0};
    const gpu_vsh_draw draw = {
        .words = draw_vertex_words,
        .word_count = sizeof draw_vertex_words / sizeof(uint32_t),
        .vertex_count = 3u,
        .attributes = attributes,
        .constants = constants,
        .output = &output,
    };
    static const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    gpu_image image = {0};

    /* a test with no buffer is refused, whichever test it is, and with the buffers it runs */
    output.depth_test = true;
    output.depth_func = GPU_VSH_COMPARE_LESS;
    output.depth_write = true;
    CHECK(gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &image) == GPU_ERR_ARGUMENT);
    output.depth = depth;
    CHECK(gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &image) == GPU_ERR_ARGUMENT); /* no stencil buffer */
    output.stencil = stencil;
    CHECK(gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &image) == GPU_OK);
    gpu_image_free(&image);
    /* the depth written back: 0.5 inside the triangle, 1.0 outside, and the stencil untouched */
    uint32_t written = 0u;
    uint32_t untouched = 0u;
    for (uint32_t i = 0u; i < WIDTH * HEIGHT; i++) {
        written += depth[i] == 0.5f;
        untouched += depth[i] == 1.0f && stencil[i] == 0u;
    }
    CHECK(written > 1500u && written + untouched == WIDTH * HEIGHT);
    /* a second render of the same triangle under LESS passes nowhere (0.5 is not less than 0.5) */
    memset(&image, 0, sizeof image);
    CHECK(gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &image) == GPU_OK);
    uint32_t covered = 0u;
    for (uint32_t i = 0u; i < WIDTH * HEIGHT; i++) {
        covered += image.pixels[i * 4u] == 255u;
    }
    CHECK(covered == 0u);
    gpu_image_free(&image);
    output.depth_func = GPU_VSH_COMPARE_ALWAYS + 1u;
    CHECK(gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &image) == GPU_ERR_ARGUMENT);
    output.depth_func = GPU_VSH_COMPARE_ALWAYS;
    output.depth_test = false;
    output.stencil_test = true;
    for (uint32_t i = 0u; i < WIDTH * HEIGHT; i++) {
        depth[i] = 1.0f; /* the stencil-only draws below must leave this alone, depth_write being on or not */
    }
    static const struct {
        uint32_t function, reference, compare_mask, write_mask, fail, zfail, zpass;
        gpu_result expected;
    } stencils[] = {
        {GPU_VSH_COMPARE_ALWAYS, 255u, 255u, 255u, GPU_VSH_STENCIL_OP_KEEP, GPU_VSH_STENCIL_OP_KEEP,
         GPU_VSH_STENCIL_OP_DECREMENT_WRAP, GPU_OK},
        {GPU_VSH_COMPARE_ALWAYS + 1u, 0u, 0u, 0u, 0u, 0u, 0u, GPU_ERR_ARGUMENT},
        {GPU_VSH_COMPARE_ALWAYS, 256u, 0u, 0u, 0u, 0u, 0u, GPU_ERR_ARGUMENT},
        {GPU_VSH_COMPARE_ALWAYS, 0u, 256u, 0u, 0u, 0u, 0u, GPU_ERR_ARGUMENT},
        {GPU_VSH_COMPARE_ALWAYS, 0u, 0u, 256u, 0u, 0u, 0u, GPU_ERR_ARGUMENT},
        {GPU_VSH_COMPARE_ALWAYS, 0u, 0u, 0u, GPU_VSH_STENCIL_OPS, 0u, 0u, GPU_ERR_ARGUMENT},
        {GPU_VSH_COMPARE_ALWAYS, 0u, 0u, 0u, 0u, GPU_VSH_STENCIL_OPS, 0u, GPU_ERR_ARGUMENT},
        {GPU_VSH_COMPARE_ALWAYS, 0u, 0u, 0u, 0u, 0u, GPU_VSH_STENCIL_OPS, GPU_ERR_ARGUMENT},
        {GPU_VSH_COMPARE_ALWAYS, 0u, 0u, 0u, 0u, 0u, GPU_VSH_STENCIL_OPS - 1u, GPU_OK},
    };
    for (size_t i = 0u; i < sizeof stencils / sizeof stencils[0]; i++) {
        output.stencil_func = stencils[i].function;
        output.stencil_ref = stencils[i].reference;
        output.stencil_compare_mask = stencils[i].compare_mask;
        output.stencil_write_mask = stencils[i].write_mask;
        output.stencil_fail_op = stencils[i].fail;
        output.stencil_zfail_op = stencils[i].zfail;
        output.stencil_zpass_op = stencils[i].zpass;
        memset(&image, 0, sizeof image);
        const gpu_result result = gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &image);
        if (result != stencils[i].expected) {
            printf("    stencil case %zu: got %s\n", i, gpu_result_string(result));
        }
        CHECK(result == stencils[i].expected);
        gpu_image_free(&image);
    }
    bool depth_untouched = true;
    for (uint32_t i = 0u; i < WIDTH * HEIGHT; i++) {
        depth_untouched = depth_untouched && depth[i] == 1.0f;
    }
    CHECK(depth_untouched); /* no depth test, no depth write, whatever depth_write says */
    output.stencil = NULL;
    CHECK(gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &image) == GPU_ERR_ARGUMENT);
    free(depth);
    free(stencil);
}

/* --- clear pixels ---------------------------------------------------------------------------- */

#define CLEAR_ALL 0xF0u

static bool in_rect(uint32_t x, uint32_t y, uint32_t x_min, uint32_t x_max, uint32_t y_min, uint32_t y_max)
{
    return x >= x_min && x <= x_max && y >= y_min && y <= y_max;
}

static void test_clear_pixels(gpu_device *device)
{
    printf("test_clear_pixels (%s)\n", gpu_device_name(device));
    gpu_pgraph_report report;
    gpu_image red = {0};
    render_plain(device, COLOUR_RED, false, &red);
    const uint32_t groups = GPU_PGRAPH_OUTPUT_CLEAR | GPU_PGRAPH_OUTPUT_DEPTH_STENCIL;
    gpu_image frame = {0};

    /* a clear before, between and after the draws, over a rectangle: the rectangle holds the clear colour except
     * where a LATER draw covered it */
    for (int when = 0; when < 3; when++) {
        stream_builder stream = {0};
        stream_setup(&stream);
        if (when == 0) {
            stream_clear(&stream, CLEAR_ALL, 0xFF0000FFu, 0u, 10u, 29u, 20u, 39u);
        }
        stream_one_triangle(&stream, MID_TRIANGLE);
        if (when == 1) { /* between this draw and the next, which is the same triangle again in green */
            stream_clear(&stream, CLEAR_ALL, 0xFF00FF00u, 0u, 10u, 29u, 20u, 39u);
            stream_colour_constant(&stream, COLOUR_GREEN);
            stream_one_triangle(&stream, 6u); /* the lower right one, away from the rectangle's triangle */
        }
        if (when == 2) {
            stream_clear(&stream, CLEAR_ALL, 0xFF00FF00u, 0u, 10u, 29u, 20u, 39u);
        }
        CHECK(replay_frame(device, &stream, GPU_PGRAPH_OUTPUT_CLEAR, false, &frame, &report) == GPU_PGRAPH_OK);
        bool exact = true;
        uint32_t rect_pixels = 0u;
        for (uint32_t y = 0u; y < HEIGHT; y++) {
            for (uint32_t x = 0u; x < WIDTH; x++) {
                const bool covered = !pixel_is(&red, x, y, 51u, 51u, 51u, 255u);
                if (in_rect(x, y, 10u, 29u, 20u, 39u)) {
                    rect_pixels++;
                    if (when == 0) { /* cleared first, the triangle drawn over it */
                        exact = exact && (covered ? pixel_is(&frame, x, y, 255u, 0u, 0u, 255u)
                                                  : pixel_is(&frame, x, y, 0u, 0u, 255u, 255u));
                    } else { /* cleared after the red triangle: green wherever the rectangle is */
                        exact = exact && pixel_is(&frame, x, y, 0u, 255u, 0u, 255u);
                    }
                }
            }
        }
        /* outside the rectangle the red triangle is exactly the plain one, the second draw aside */
        for (uint32_t y = 0u; y < HEIGHT && when != 1; y++) {
            for (uint32_t x = 0u; x < WIDTH; x++) {
                if (!in_rect(x, y, 10u, 29u, 20u, 39u)) {
                    exact = exact && same_pixel(&frame, &red, x, y);
                }
            }
        }
        CHECK(exact && rect_pixels == 20u * 20u);
        CHECK(report.drawn == (when == 1 ? 2u : 1u));
        CHECK(report.used_inferences == (PLAIN_INFERENCES | GPU_PGRAPH_INFER_OUTPUT_CLEAR_MODEL));
        gpu_image_free(&frame);
        stream_free(&stream);
    }

    /* flagged channels only: R and B of 0x80FF40C0 over a red triangle on grey (two channels that are not mirror
     * images of each other in the lane order, so a reversed mapping would show) */
    stream_builder channels = {0};
    stream_setup(&channels);
    stream_one_triangle(&channels, MID_TRIANGLE);
    stream_clear(&channels, 0x10u | 0x40u, 0x80FF40C0u, 0u, 0u, 63u, 0u, 63u);
    CHECK(replay_frame(device, &channels, GPU_PGRAPH_OUTPUT_CLEAR, false, &frame, &report) == GPU_PGRAPH_OK);
    bool exact = true;
    for (uint32_t y = 0u; y < HEIGHT; y++) {
        for (uint32_t x = 0u; x < WIDTH; x++) {
            const bool covered = !pixel_is(&red, x, y, 51u, 51u, 51u, 255u);
            exact = exact && (covered ? pixel_is(&frame, x, y, 255u, 0u, 0xC0u, 255u)
                                      : pixel_is(&frame, x, y, 255u, 51u, 0xC0u, 255u));
        }
    }
    CHECK(exact);
    gpu_image_free(&frame);
    stream_free(&channels);

    /* a depth clear: near (0.25, green) written, depth cleared to 1.0 over the left half, far (0.75, blue) drawn:
     * blue on the left (it passes the cleared depth), the near green stays on the right */
    stream_builder depth = {0};
    stream_setup(&depth);
    stream_depth_state(&depth, 1u, 0x0201u, 1u);
    stream_colour_constant(&depth, COLOUR_GREEN);
    stream_one_triangle(&depth, NEAR_TRIANGLE);
    stream_clear(&depth, 0x01u, 0u, 0xFFFFFF00u, 0u, 31u, 0u, 63u);
    stream_colour_constant(&depth, COLOUR_BLUE);
    stream_one_triangle(&depth, FAR_TRIANGLE);
    CHECK(replay_frame(device, &depth, groups, false, &frame, &report) == GPU_PGRAPH_OK);
    exact = true;
    uint32_t left = 0u;
    uint32_t right = 0u;
    for (uint32_t y = 0u; y < HEIGHT; y++) {
        for (uint32_t x = 0u; x < WIDTH; x++) {
            if (pixel_is(&red, x, y, 51u, 51u, 51u, 255u)) {
                exact = exact && pixel_is(&frame, x, y, 51u, 51u, 51u, 255u);
            } else if (x < 32u) {
                exact = exact && pixel_is(&frame, x, y, 0u, 0u, 255u, 255u);
                left++;
            } else {
                exact = exact && pixel_is(&frame, x, y, 0u, 255u, 0u, 255u);
                right++;
            }
        }
    }
    CHECK(exact && left > 300u && right > 300u);
    gpu_image_free(&frame);
    stream_free(&depth);

    /* the depth value of the clear: cleared to 0.0 over everything, a LESS draw then passes nowhere */
    stream_builder zero = {0};
    stream_setup(&zero);
    stream_depth_state(&zero, 1u, 0x0201u, 1u);
    stream_clear(&zero, 0x01u, 0u, 0x00000000u, 0u, 63u, 0u, 63u);
    stream_colour_constant(&zero, COLOUR_GREEN);
    stream_one_triangle(&zero, NEAR_TRIANGLE);
    CHECK(replay_frame(device, &zero, groups, false, &frame, &report) == GPU_PGRAPH_OK);
    CHECK(all_clear(&frame));
    gpu_image_free(&frame);
    stream_free(&zero);

    /* a stencil clear: stencil 5 written under the triangle, cleared to 0 over the left half, then drawn only where
     * the stencil equals 5: the right half (blue) and not the left (red from the first draw) */
    stream_builder stencil = {0};
    stream_setup(&stencil);
    stream_stencil_state(&stencil, 1u, 0x0207u, 5u, 0xFFu, 0xFFu, 0x1E00u, 0x1E00u, 0x1E01u);
    stream_one_triangle(&stencil, MID_TRIANGLE);
    stream_clear(&stencil, 0x02u, 0u, 0x00000000u, 0u, 31u, 0u, 63u);
    stream_stencil_state(&stencil, 1u, 0x0202u, 5u, 0xFFu, 0xFFu, 0x1E00u, 0x1E00u, 0x1E00u);
    stream_colour_constant(&stencil, COLOUR_BLUE);
    stream_one_triangle(&stencil, MID_TRIANGLE);
    CHECK(replay_frame(device, &stencil, groups, false, &frame, &report) == GPU_PGRAPH_OK);
    exact = true;
    for (uint32_t y = 0u; y < HEIGHT; y++) {
        for (uint32_t x = 0u; x < WIDTH; x++) {
            if (pixel_is(&red, x, y, 51u, 51u, 51u, 255u)) {
                exact = exact && pixel_is(&frame, x, y, 51u, 51u, 51u, 255u);
            } else {
                exact = exact && (x < 32u ? pixel_is(&frame, x, y, 255u, 0u, 0u, 255u)
                                          : pixel_is(&frame, x, y, 0u, 0u, 255u, 255u));
            }
        }
    }
    CHECK(exact);
    gpu_image_free(&frame);
    stream_free(&stencil);

    /* flip_y: the rectangle is rows of the FINISHED image. Cleared after the draw, rows 10..19 of it are all the clear
     * colour and no other row has any */
    stream_builder flip = {0};
    stream_setup(&flip);
    stream_one_triangle(&flip, 6u);
    stream_clear(&flip, CLEAR_ALL, 0xFF0000FFu, 0u, 0u, 63u, 10u, 19u);
    for (int flipped = 0; flipped < 2; flipped++) {
        CHECK(replay_frame(device, &flip, GPU_PGRAPH_OUTPUT_CLEAR, flipped != 0, &frame, &report) == GPU_PGRAPH_OK);
        uint32_t blue_in = 0u;
        uint32_t blue_out = 0u;
        for (uint32_t y = 0u; y < HEIGHT; y++) {
            for (uint32_t x = 0u; x < WIDTH; x++) {
                const bool blue = pixel_is(&frame, x, y, 0u, 0u, 255u, 255u);
                blue_in += blue && y >= 10u && y <= 19u;
                blue_out += blue && !(y >= 10u && y <= 19u);
            }
        }
        CHECK(blue_in == 10u * 64u && blue_out == 0u);
        gpu_image_free(&frame);
    }
    stream_free(&flip);

    gpu_image_free(&red);
}

/* A clear that sits between two passes belongs to the pass that follows, and the clears after the last draw of the
 * frame to the last pass; a clear is never applied twice and the model keeps it for another replay. */
static void test_clear_passes(gpu_device *device)
{
    printf("test_clear_passes (%s)\n", gpu_device_name(device));
    stream_builder stream = {0};
    stream_setup(&stream);
    stream_clear(&stream, CLEAR_ALL, 0xFFFF0000u, 0u, 0u, 63u, 0u, 63u);      /* before draw 0: red */
    stream_colour_constant(&stream, COLOUR_GREEN);
    stream_one_triangle(&stream, MID_TRIANGLE);                              /* draw 0 */
    stream_clear(&stream, CLEAR_ALL, 0xFF0000FFu, 0u, 0u, 63u, 0u, 31u);      /* before draw 1: blue, the top half */
    stream_one_triangle(&stream, 6u);                                        /* draw 1 */
    stream_clear(&stream, CLEAR_ALL, 0xFFFFFFFFu, 0u, 0u, 3u, 0u, 3u);        /* after the last draw: white corner */
    fill_memory();
    fake_guest guest = {MEMORY_BASE, memory, sizeof memory};
    gpu_pgraph_backend backend = make_backend(&guest, GPU_PGRAPH_OUTPUT_CLEAR, GPU_PGRAPH_INFER_OUTPUT_ALL);
    gpu_pgraph *model = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(model, GPU_PGRAPH_OUTPUT_CLEAR);
    CHECK(gpu_pgraph_decode(model, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_count(model) == 2u && gpu_pgraph_clear_count(model) == 3u);
    gpu_pgraph_report report;

    /* the whole frame: red cleared, the green triangle, blue cleared over all of it, the second triangle (green too: c3
     * persists), then the white corner */
    gpu_image whole = {0};
    CHECK(gpu_pgraph_replay(model, device, &backend, WIDTH, HEIGHT, clear_grey, &whole, &report) == GPU_PGRAPH_OK);
    CHECK(report.clears_applied == 3u);
    CHECK(pixel_is(&whole, 40u, 8u, 0u, 0u, 255u, 255u));  /* blue: the later clear covered draw 0's triangle */
    CHECK(pixel_is(&whole, 8u, 50u, 0u, 255u, 0u, 255u));  /* draw 0's triangle, below the blue half */
    CHECK(pixel_is(&whole, 56u, 56u, 0u, 255u, 0u, 255u)); /* draw 1's triangle, green */
    CHECK(pixel_is(&whole, 1u, 1u, 255u, 255u, 255u, 255u)); /* the corner cleared after the last draw */
    CHECK(pixel_is(&whole, 4u, 4u, 0u, 0u, 255u, 255u));
    /* pass 1 is draw 0 only: the clear before it (red), its triangle (green), and not the blue one that precedes draw 1 */
    gpu_image first = {0};
    CHECK(gpu_pgraph_replay_range(model, device, &backend, WIDTH, HEIGHT, clear_grey, 0u, 1u, &first, &report) ==
          GPU_PGRAPH_OK);
    CHECK(pixel_is(&first, 40u, 8u, 0u, 255u, 0u, 255u));   /* the triangle */
    CHECK(pixel_is(&first, 56u, 56u, 255u, 0u, 0u, 255u));  /* red clear: no later clear in this pass */
    CHECK(pixel_is(&first, 1u, 1u, 255u, 0u, 0u, 255u));    /* no white corner: it is after the LAST draw */
    CHECK(pixel_is(&first, 8u, 50u, 0u, 255u, 0u, 255u));   /* the triangle, and no blue half in this pass */
    /* pass 2 is draw 1 only: the blue clear before it, its triangle, and the white corner after it */
    gpu_image second = {0};
    CHECK(gpu_pgraph_replay_range(model, device, &backend, WIDTH, HEIGHT, clear_grey, 1u, 1u, &second, &report) ==
          GPU_PGRAPH_OK);
    CHECK(pixel_is(&second, 40u, 8u, 0u, 0u, 255u, 255u));  /* blue, never red */
    CHECK(pixel_is(&second, 56u, 56u, 0u, 255u, 0u, 255u)); /* draw 1's triangle */
    CHECK(pixel_is(&second, 1u, 1u, 255u, 255u, 255u, 255u)); /* the clear after the last draw */
    CHECK(pixel_is(&second, 8u, 50u, 51u, 51u, 51u, 255u));   /* the frame's own colour: draw 0 and the red clear are not here */
    CHECK(report.clears_applied == 2u); /* the blue clear and the white corner */
    /* the clears of a pass named by the caller (gpu_pgraph_replay_pass): exactly [first_clear, first_clear + count) */
    gpu_image explicit_pass = {0};
    CHECK(gpu_pgraph_replay_pass(model, device, &backend, WIDTH, HEIGHT, clear_grey, 0u, 2u, 1u, 1u, &explicit_pass,
                                 &report) == GPU_PGRAPH_OK);
    CHECK(report.clears_applied == 1u);
    CHECK(pixel_is(&explicit_pass, 40u, 8u, 0u, 0u, 255u, 255u)); /* blue: clear 1 sits before draw 1 and covers draw 0 */
    CHECK(pixel_is(&explicit_pass, 1u, 1u, 0u, 0u, 255u, 255u));  /* clear 0 (red) and clear 2 (white) are NOT this pass's */
    CHECK(pixel_is(&explicit_pass, 56u, 56u, 0u, 255u, 0u, 255u));
    gpu_image_free(&explicit_pass);
    CHECK(gpu_pgraph_replay_pass(model, device, &backend, WIDTH, HEIGHT, clear_grey, 0u, 2u, 0u, 3u, &explicit_pass,
                                 &report) == GPU_PGRAPH_OK && report.clears_applied == 3u);
    CHECK(identical(&explicit_pass, &whole));
    gpu_image_free(&explicit_pass);
    CHECK(gpu_pgraph_replay_pass(model, device, &backend, WIDTH, HEIGHT, clear_grey, 0u, 2u, 0u, 0u, &explicit_pass,
                                 &report) == GPU_PGRAPH_OK && report.clears_applied == 0u);
    CHECK(pixel_is(&explicit_pass, 1u, 1u, 51u, 51u, 51u, 255u)); /* no clear at all: the frame's own colour */
    gpu_image_free(&explicit_pass);
    CHECK(gpu_pgraph_replay_pass(model, device, &backend, WIDTH, HEIGHT, clear_grey, 0u, 2u, 0u, 4u, &explicit_pass,
                                 &report) == GPU_PGRAPH_ERR_ARGUMENT);
    CHECK(gpu_pgraph_replay_pass(model, device, &backend, WIDTH, HEIGHT, clear_grey, 0u, 2u, 4u, 0u, &explicit_pass,
                                 &report) == GPU_PGRAPH_ERR_ARGUMENT);
    CHECK(explicit_pass.pixels == NULL);
    /* the same replay again: nothing was consumed */
    gpu_image again = {0};
    CHECK(gpu_pgraph_replay(model, device, &backend, WIDTH, HEIGHT, clear_grey, &again, &report) == GPU_PGRAPH_OK);
    CHECK(identical(&whole, &again));
    gpu_image_free(&whole);
    gpu_image_free(&first);
    gpu_image_free(&second);
    gpu_image_free(&again);

    /* the group not enabled in the backend: refused with the message, at the draw it precedes */
    gpu_pgraph_backend none = make_backend(&guest, 0u, GPU_PGRAPH_INFER_OUTPUT_ALL);
    gpu_image refused = {0};
    CHECK(gpu_pgraph_replay(model, device, &none, WIDTH, HEIGHT, clear_grey, &refused, &report) ==
          GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "does not enable the clear group") != NULL && report.failed_draw == 0u);
    CHECK(refused.pixels == NULL);
    /* without the inference */
    gpu_pgraph_backend strict_inference = make_backend(&guest, GPU_PGRAPH_OUTPUT_CLEAR, 0u);
    strict_inference.allowed_inferences = GPU_PGRAPH_INFER_ALL;
    CHECK(gpu_pgraph_replay(model, device, &strict_inference, WIDTH, HEIGHT, clear_grey, &refused, &report) ==
          GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "INFERRED and not allowed") != NULL);
    gpu_pgraph_destroy(model);
    stream_free(&stream);

    /* a refused clear after the last draw is reported at the index one past it */
    stream_builder late = {0};
    stream_setup(&late);
    stream_one_triangle(&late, MID_TRIANGLE);
    stream_pair(&late, GPU_PGRAPH_CLEAR_SURFACE, 0x10u); /* no rectangle, no colour written */
    gpu_pgraph *late_model = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(late_model, GPU_PGRAPH_OUTPUT_CLEAR);
    CHECK(gpu_pgraph_decode(late_model, late.pairs, late.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_replay(late_model, device, &backend, WIDTH, HEIGHT, clear_grey, &refused, &report) ==
          GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "no horizontal rectangle") != NULL && report.failed_draw == 1u);
    gpu_pgraph_destroy(late_model);
    stream_free(&late);

    /* a depth clear with the depth group missing from the backend is refused at replay too */
    stream_builder zclear = {0};
    stream_setup(&zclear);
    stream_clear(&zclear, 0x01u, 0u, 0xFFFFFF00u, 0u, 63u, 0u, 63u);
    stream_one_triangle(&zclear, MID_TRIANGLE);
    gpu_pgraph *z_model = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(z_model, GPU_PGRAPH_OUTPUT_CLEAR);
    CHECK(gpu_pgraph_decode(z_model, zclear.pairs, zclear.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_replay(z_model, device, &backend, WIDTH, HEIGHT, clear_grey, &refused, &report) ==
          GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "needs the depth and stencil group") != NULL);
    gpu_pgraph_destroy(z_model);
    stream_free(&zclear);
}

/* The largest x of a pixel of the frame that is exactly green, or -1. */
static int rightmost_green(const gpu_image *image)
{
    int found = -1;
    for (uint32_t y = 0u; y < image->height; y++) {
        for (uint32_t x = 0u; x < image->width; x++) {
            if (pixel_is(image, x, y, 0u, 255u, 0u, 255u)) {
                found = found > (int)x ? found : (int)x;
            }
        }
    }
    return found;
}

static uint32_t count_green(const gpu_image *image)
{
    uint32_t count = 0u;
    for (uint32_t y = 0u; y < image->height; y++) {
        for (uint32_t x = 0u; x < image->width; x++) {
            count += pixel_is(image, x, y, 0u, 255u, 0u, 255u);
        }
    }
    return count;
}

#define OFFSET_GROUPS (GPU_PGRAPH_OUTPUT_DEPTH_STENCIL | GPU_PGRAPH_OUTPUT_POLYGON_OFFSET)
#define BIG_UNITS 1000000.0f /* 1e6 smallest depth steps: about 0.06 of depth near 0.5, far above any rounding */

/* A MID triangle writes depth (ALWAYS, red), then a second `second` triangle in green runs under `function` with no
 * depth write and the offset `scale`, `bias`, enables `fill`, `point`, `line`. */
static gpu_pgraph_result replay_offset_pair(gpu_device *device, uint32_t first_triangle, uint32_t second_triangle,
                                            uint32_t function, float scale, float bias, uint32_t fill,
                                            uint32_t point, uint32_t line, bool offset_first, gpu_image *frame,
                                            gpu_pgraph_report *report)
{
    stream_builder stream = {0};
    stream_setup(&stream);
    stream_depth_state(&stream, 1u, 0x0207u, 1u);
    if (offset_first) {
        stream_polygon_offset(&stream, scale, bias, fill, point, line);
    }
    stream_one_triangle(&stream, first_triangle);
    stream_depth_state(&stream, 1u, 0x0200u + function, 0u);
    if (!offset_first) {
        stream_polygon_offset(&stream, scale, bias, fill, point, line);
    } else {
        stream_polygon_offset(&stream, 0.0f, 0.0f, 0u, 0u, 0u);
    }
    stream_colour_constant(&stream, COLOUR_GREEN);
    stream_one_triangle(&stream, second_triangle);
    const gpu_pgraph_result result = replay_frame(device, &stream, OFFSET_GROUPS, false, frame, report);
    stream_free(&stream);
    return result;
}

static void test_polygon_offset_pixels(gpu_device *device)
{
    printf("test_polygon_offset_pixels (%s)\n", gpu_device_name(device));
    gpu_pgraph_report report;
    gpu_image red = {0};
    gpu_image green = {0};
    render_plain(device, COLOUR_RED, false, &red);
    render_plain(device, COLOUR_GREEN, false, &green);
    CHECK(count_red(&red) > 1500u && count_green(&green) > 1500u && !identical(&red, &green));

    /* constant bias on a constant-depth triangle at an EQUAL depth under LESS: no offset fails (red), a bias towards
     * the viewer passes (green), away from it fails, an enable of 0 and point or line enables alone do nothing */
    static const struct {
        float bias;
        uint32_t fill, point, line;
        bool green;
        uint32_t inference;
    } rows[] = {
        {0.0f, 0u, 0u, 0u, false, 0u},
        {-BIG_UNITS, 1u, 0u, 0u, true, GPU_PGRAPH_INFER_OUTPUT_POLYGON_OFFSET_MODEL},
        {BIG_UNITS, 1u, 0u, 0u, false, GPU_PGRAPH_INFER_OUTPUT_POLYGON_OFFSET_MODEL},
        {-BIG_UNITS, 0u, 0u, 0u, false, 0u},
        {-BIG_UNITS, 0u, 1u, 1u, false, GPU_PGRAPH_INFER_OUTPUT_POLYGON_OFFSET_FILL_ONLY},
        {-BIG_UNITS, 1u, 1u, 1u, true,
         GPU_PGRAPH_INFER_OUTPUT_POLYGON_OFFSET_MODEL | GPU_PGRAPH_INFER_OUTPUT_POLYGON_OFFSET_FILL_ONLY},
    };
    for (size_t i = 0u; i < sizeof rows / sizeof rows[0]; i++) {
        gpu_image frame = {0};
        CHECK(replay_offset_pair(device, MID_TRIANGLE, MID_TRIANGLE, 1u, 0.0f, rows[i].bias, rows[i].fill,
                                 rows[i].point, rows[i].line, false, &frame, &report) == GPU_PGRAPH_OK);
        if (!identical(&frame, rows[i].green ? &green : &red)) {
            printf("    row %zu: expected %s\n", i, rows[i].green ? "green" : "red");
        }
        CHECK(identical(&frame, rows[i].green ? &green : &red));
        CHECK(report.used_inferences ==
              (PLAIN_INFERENCES | GPU_PGRAPH_INFER_OUTPUT_DEPTH_MODEL | rows[i].inference));
        CHECK(report.drawn == 2u);
        CHECK(report.offset_applied == (rows[i].fill == 1u && rows[i].bias != 0.0f ? 1u : 0u));
        CHECK(report.offset_unobserved == 0u);
        gpu_image_free(&frame);
    }

    /* the stored depth is the biased one: the first triangle drawn with a bias towards the viewer stores about 0.44, so
     * a GREATER test of the same triangle later passes (0.5 > 0.44), and with no bias it fails (0.5 > 0.5) */
    for (int biased = 0; biased < 2; biased++) {
        gpu_image frame = {0};
        CHECK(replay_offset_pair(device, MID_TRIANGLE, MID_TRIANGLE, 4u, 0.0f, biased == 1 ? -BIG_UNITS : 0.0f,
                                 biased == 1 ? 1u : 0u, 0u, 0u, true, &frame, &report) == GPU_PGRAPH_OK);
        CHECK(identical(&frame, biased == 1 ? &green : &red));
        gpu_image_free(&frame);
    }

    /* the slope factor: a triangle whose depth rises 0.5 across 57.6 pixels (0.00868 per pixel) passes LESS against the
     * stored 0.5 where it is nearer, up to about pixel 32. A slope factor s moves that edge by about s pixels */
    int edge[3] = {0, 0, 0};
    static const float slopes[3] = {0.0f, -10.0f, 10.0f};
    for (int i = 0; i < 3; i++) {
        gpu_image frame = {0};
        CHECK(replay_offset_pair(device, MID_TRIANGLE, 19u, 1u, slopes[i], 0.0f, slopes[i] != 0.0f ? 1u : 0u, 0u, 0u,
                                 false, &frame, &report) == GPU_PGRAPH_OK);
        edge[i] = rightmost_green(&frame);
        CHECK(count_green(&frame) > 100u);
        printf("    slope %+.1f: rightmost green column %d\n", (double)slopes[i], edge[i]);
        gpu_image_free(&frame);
    }
    CHECK(edge[0] >= 30 && edge[0] <= 34);
    CHECK(edge[1] - edge[0] >= 8 && edge[1] - edge[0] <= 12);
    CHECK(edge[0] - edge[2] >= 8 && edge[0] - edge[2] <= 12);

    /* nothing tests depth: the offset is counted as unobserved (both draws), the frame is the unoffset one */
    {
        stream_builder with = {0};
        stream_builder without = {0};
        gpu_image frame_with = {0};
        gpu_image frame_without = {0};
        stream_setup(&with);
        stream_polygon_offset(&with, -0.25f, -BIG_UNITS, 1u, 0u, 0u);
        stream_one_triangle(&with, MID_TRIANGLE);
        stream_colour_constant(&with, COLOUR_GREEN);
        stream_one_triangle(&with, NEAR_TRIANGLE);
        stream_setup(&without);
        stream_one_triangle(&without, MID_TRIANGLE);
        stream_colour_constant(&without, COLOUR_GREEN);
        stream_one_triangle(&without, NEAR_TRIANGLE);
        CHECK(replay_frame(device, &with, OFFSET_GROUPS, false, &frame_with, &report) == GPU_PGRAPH_OK);
        CHECK(report.offset_unobserved == 2u && report.offset_applied == 0u && report.drawn == 2u);
        CHECK(report.used_inferences == (PLAIN_INFERENCES | GPU_PGRAPH_INFER_OUTPUT_POLYGON_OFFSET_MODEL));
        CHECK(replay_frame(device, &without, OFFSET_GROUPS, false, &frame_without, &report) == GPU_PGRAPH_OK);
        CHECK(report.offset_unobserved == 0u && report.used_inferences == PLAIN_INFERENCES);
        CHECK(count_green(&frame_with) > 1500u && identical(&frame_with, &frame_without));
        gpu_image_free(&frame_with);
        gpu_image_free(&frame_without);
        stream_free(&with);
        stream_free(&without);
    }

    /* lines and points are never biased: a primitive at an EQUAL depth fails LESS with a bias that lifts a triangle
     * (nothing green), the same primitive at 0.25 passes (the draw works at all, so the empty result is the rule and
     * not a failed draw). RADV biases a line or point list the pipeline carries a bias for and llvmpipe does not
     * (T552 probe, HQ55), so the unbiased pipeline is what makes this equal on both. T552: such a draw is not
     * "applied", it is counted unobserved: the triangle drew before the offset words, so the one primitive draw is
     * the only one the offset reaches. With the offset on a TRIANGLE after it the same frame counts both ways */
    for (int primitive = 0; primitive < 2; primitive++) {
        for (int near_primitive = 0; near_primitive < 2; near_primitive++) {
            stream_builder stream = {0};
            stream_setup(&stream);
            stream_depth_state(&stream, 1u, 0x0207u, 1u);
            stream_one_triangle(&stream, MID_TRIANGLE);
            stream_depth_state(&stream, 1u, 0x0201u, 0u);
            stream_polygon_offset(&stream, 0.0f, -BIG_UNITS, 1u, 0u, 0u);
            stream_colour_constant(&stream, COLOUR_GREEN);
            stream_draw_arrays(&stream, primitive == 0 ? GPU_PGRAPH_OP_LINES : GPU_PGRAPH_OP_POINTS,
                               near_primitive == 1 ? 17u : 15u, 2u);
            gpu_image frame = {0};
            CHECK(replay_frame(device, &stream, OFFSET_GROUPS, false, &frame, &report) == GPU_PGRAPH_OK);
            CHECK(report.drawn == 2u && report.offset_applied == 0u && report.offset_unobserved == 1u);
            if (near_primitive == 1) {
                CHECK(count_green(&frame) > 0u && count_green(&frame) < 40u);
            } else {
                CHECK(count_green(&frame) == 0u);
                CHECK(identical(&frame, &red));
            }
            gpu_image_free(&frame);
            stream_free(&stream);
        }
    }
    {
        stream_builder stream = {0};
        stream_setup(&stream);
        stream_depth_state(&stream, 1u, 0x0207u, 1u);
        stream_polygon_offset(&stream, 0.0f, -BIG_UNITS, 1u, 0u, 0u);
        stream_one_triangle(&stream, MID_TRIANGLE);
        stream_colour_constant(&stream, COLOUR_GREEN);
        stream_draw_arrays(&stream, GPU_PGRAPH_OP_LINES, 17u, 2u);
        stream_draw_arrays(&stream, GPU_PGRAPH_OP_POINTS, 17u, 2u);
        gpu_image frame = {0};
        CHECK(replay_frame(device, &stream, OFFSET_GROUPS, false, &frame, &report) == GPU_PGRAPH_OK);
        CHECK(report.drawn == 3u && report.offset_applied == 1u && report.offset_unobserved == 2u);
        gpu_image_free(&frame);
        stream_free(&stream);
    }
    /* a line with the offset on and nothing testing depth is unobserved once, not twice */
    {
        stream_builder stream = {0};
        stream_setup(&stream);
        stream_polygon_offset(&stream, 0.0f, -BIG_UNITS, 1u, 0u, 0u);
        stream_colour_constant(&stream, COLOUR_GREEN);
        stream_draw_arrays(&stream, GPU_PGRAPH_OP_LINES, 17u, 2u);
        gpu_image frame = {0};
        CHECK(replay_frame(device, &stream, OFFSET_GROUPS, false, &frame, &report) == GPU_PGRAPH_OK);
        CHECK(report.drawn == 1u && report.offset_applied == 0u && report.offset_unobserved == 1u);
        gpu_image_free(&frame);
        stream_free(&stream);
    }

    /* the group OFF is the default replay, byte for byte: a stream that carries the title's five polygon offset
     * words (a big offset included) decodes leniently and replays to the frame the same stream without them gives,
     * and strict mode refuses the first of them by number */
    for (int with_words = 0; with_words < 2; with_words++) {
        stream_builder stream = {0};
        stream_setup(&stream);
        stream_depth_state(&stream, 1u, 0x0207u, 1u);
        stream_one_triangle(&stream, MID_TRIANGLE);
        stream_depth_state(&stream, 1u, 0x0201u, 0u);
        if (with_words == 1) {
            stream_polygon_offset(&stream, -4.0f, -BIG_UNITS, 1u, 1u, 1u);
        }
        stream_colour_constant(&stream, COLOUR_GREEN);
        stream_one_triangle(&stream, MID_TRIANGLE);
        gpu_image frame = {0};
        fill_memory();
        fake_guest guest = {MEMORY_BASE, memory, sizeof memory};
        gpu_pgraph_backend backend = make_backend(&guest, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL, GPU_PGRAPH_INFER_OUTPUT_ALL);
        gpu_pgraph *model = gpu_pgraph_create();
        gpu_pgraph_set_output_groups(model, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL); /* the new groups off */
        CHECK(gpu_pgraph_decode(model, stream.pairs, stream.count) == GPU_PGRAPH_OK);
        CHECK(gpu_pgraph_unhandled_count(model) == (with_words == 1 ? 5u : 0u));
        CHECK(gpu_pgraph_replay(model, device, &backend, WIDTH, HEIGHT, clear_grey, &frame, &report) == GPU_PGRAPH_OK);
        CHECK(report.offset_applied == 0u && report.offset_unobserved == 0u);
        CHECK(identical(&frame, &red)); /* equal depth fails LESS: the offset words had no effect */
        gpu_pgraph_destroy(model);
        gpu_pgraph *strict = gpu_pgraph_create();
        gpu_pgraph_set_strict(strict, true);
        gpu_pgraph_set_output_groups(strict, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL);
        CHECK((gpu_pgraph_decode(strict, stream.pairs, stream.count) == GPU_PGRAPH_ERR_UNMEASURED) == (with_words == 1));
        gpu_pgraph_destroy(strict);
        gpu_image_free(&frame);
        stream_free(&stream);
    }
    gpu_image_free(&red);
    gpu_image_free(&green);
}

static void test_polygon_offset_bounds(gpu_device *device)
{
    printf("test_polygon_offset_bounds (%s)\n", gpu_device_name(device));
    float attributes[3 * GPU_VSH_ATTRIBUTE_FLOATS];
    memset(attributes, 0, sizeof attributes);
    static const float corners[3][2] = {{-0.9f, -0.9f}, {0.9f, -0.9f}, {-0.9f, 0.9f}};
    for (uint32_t i = 0u; i < 3u; i++) {
        float *vertex = attributes + i * GPU_VSH_ATTRIBUTE_FLOATS;
        vertex[4] = corners[i][0];
        vertex[5] = corners[i][1];
        vertex[6] = 0.5f;
        vertex[7] = 1.0f;
        vertex[10] = 1.0f;
        vertex[11] = 1.0f;
    }
    float constants[GPU_VSH_CONSTANT_ROWS * 4u];
    memset(constants, 0, sizeof constants);
    float *depth = malloc((size_t)WIDTH * HEIGHT * sizeof *depth);
    uint8_t *stencil = calloc((size_t)WIDTH * HEIGHT, 1u);
    CHECK(depth != NULL && stencil != NULL);
    for (uint32_t i = 0u; i < WIDTH * HEIGHT; i++) {
        depth[i] = 1.0f;
    }
    gpu_vsh_output output = {0};
    output.depth_test = true;
    output.depth_write = true;
    output.depth_func = GPU_VSH_COMPARE_LESS;
    output.depth = depth;
    output.stencil = stencil;
    const gpu_vsh_draw draw = {
        .words = draw_vertex_words,
        .word_count = sizeof draw_vertex_words / sizeof(uint32_t),
        .vertex_count = 3u,
        .attributes = attributes,
        .constants = constants,
        .output = &output,
    };
    static const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    gpu_image image = {0};

    /* a bias needs the depth test, and finite factors: anything else is refused before the device runs */
    output.depth_bias = true;
    output.depth_bias_constant = -4.0f;
    output.depth_bias_slope = 1.0f;
    CHECK(gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &image) == GPU_OK);
    gpu_image_free(&image);
    static const float not_finite[3] = {INFINITY, -INFINITY, NAN};
    for (int which = 0; which < 6; which++) {
        output.depth_bias_constant = which < 3 ? not_finite[which] : -4.0f;
        output.depth_bias_slope = which < 3 ? 1.0f : not_finite[which - 3];
        memset(&image, 0, sizeof image);
        CHECK(gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &image) == GPU_ERR_ARGUMENT && image.pixels == NULL);
    }
    output.depth_bias_constant = -4.0f;
    output.depth_bias_slope = 1.0f;
    output.depth_test = false;
    memset(&image, 0, sizeof image);
    CHECK(gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &image) == GPU_ERR_ARGUMENT && image.pixels == NULL);
    output.depth_bias = false; /* the same draw with no bias and no tests is fine */
    CHECK(gpu_vsh_render(device, WIDTH, HEIGHT, clear, &draw, &image) == GPU_OK);
    gpu_image_free(&image);
    free(depth);
    free(stencil);
}

static int run_device(const char *selector)
{
    gpu_device *device = NULL;
    const gpu_result created = gpu_device_create_selected(selector, &device);
    if (created != GPU_OK) {
        printf("SKIP %s: %s\n", selector, gpu_result_string(created));
        return 0;
    }
    test_scissor_pixels(device);
    test_device_bounds(device);
    test_cull_pixels(device);
    test_cull_bounds(device);
    test_blend_pixels(device);
    test_alpha_test_pixels(device);
    test_state_bounds(device);
    test_depth_pixels(device);
    test_stencil_pixels(device);
    test_depth_bounds(device);
    test_polygon_offset_pixels(device);
    test_polygon_offset_bounds(device);
    test_clear_pixels(device);
    test_clear_passes(device);
    gpu_device_destroy(device);
    return 1;
}

int main(void)
{
    fill_memory();
    test_decode();
    test_resolve();
    test_resolve_cull();
    test_resolve_blend();
    test_resolve_polygon_mode();
    test_resolve_alpha_test();
    test_resolve_depth_stencil();
    test_resolve_polygon_offset();
    test_resolve_clear();
    if (failures != 0) {
        printf("%d checks, %d failures\n", checks, failures);
        return 1;
    }
    if (!gpu_vulkan_available()) {
        printf("SKIP: no Vulkan loader on this machine (the device-free checks above passed)\n");
        printf("%d checks, %d failures\n", checks, failures);
        return 77;
    }
    int ran = 0;
    ran += run_device("hardware");
    ran += run_device("software");
    printf("%d checks, %d failures, %d device(s) exercised\n", checks, failures, ran);
    if (failures != 0) {
        return 1;
    }
    return ran > 0 ? 0 : 77;
}
