/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T791 test fixtures shared by test_live_vk_pipeline.c and test_live_vk_frame.c: the synthetic one program scenes (opaque,
 * blend, depth, stencil, cull, scissor, alpha test, lines) as decoded gpu_pgraph models over a flat fake guest memory.
 * The includer defines CHECK (and WIDTH and HEIGHT are defined here) before including it.
 */
#ifndef TSFP_LIVE_VK_SCENES_H
#define TSFP_LIVE_VK_SCENES_H

#include "gpu_pgraph.h"
#include "gpu_combiner_words.h"
#include "gpu_pgraph_replay.h"
#include "gpu_pgraph_test_support.h"
#include "gpu_pgraph_vertex_words.h"

#include <stdio.h>
#include <string.h>

#define WIDTH 64u
#define HEIGHT 64u
#define MEMORY_BASE 0x00600000u
#define TRIANGLE_BYTES (3u * 28u)
#define ALL_GROUPS (GPU_PGRAPH_OUTPUT_SCISSOR | GPU_PGRAPH_OUTPUT_CULL | GPU_PGRAPH_OUTPUT_BLEND | \
                    GPU_PGRAPH_OUTPUT_ALPHA_TEST | GPU_PGRAPH_OUTPUT_DEPTH_STENCIL | GPU_PGRAPH_OUTPUT_CLEAR | GPU_PGRAPH_OUTPUT_FIXED)
#define EVERYTHING (GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_OUTPUT_ALL | GPU_PGRAPH_INFER_COMBINER_ALL)

static const char *const module_names[] = {SYNTHETIC_PROGRAM_NAME};
static const struct gpu_vsh_table table = {0u, 0u, NULL, 0u, NULL, 1u, module_names};
static uint8_t memory[0x800];

__attribute__((unused)) static bool load_module(void *context, uint32_t module, const uint32_t **words, size_t *word_count)
{
    (void)context;
    if (module != 0u) {
        return false;
    }
    *words = draw_vertex_words;
    *word_count = sizeof draw_vertex_words / sizeof(uint32_t);
    return true;
}

typedef struct {
    float position[3];
    float colour[4];
} float_vertex;

/* Triangle t: position corners, one colour. Triangles 0 and 1 overlap, 2 is the mirror winding of 1, 3 is a line pair. */
__attribute__((unused)) static void put_triangle(uint32_t index, const float corners[3][3], const float colour[4])
{
    for (uint32_t corner = 0u; corner < 3u; corner++) {
        float_vertex vertex;
        memcpy(vertex.position, corners[corner], sizeof vertex.position);
        memcpy(vertex.colour, colour, sizeof vertex.colour);
        memcpy(memory + index * TRIANGLE_BYTES + corner * sizeof vertex, &vertex, sizeof vertex);
    }
}

__attribute__((unused)) static void fill_memory(void)
{
    memset(memory, 0, sizeof memory);
    static const float a[3][3] = {{-0.8f, -0.8f, 0.2f}, {0.6f, -0.8f, 0.2f}, {-0.8f, 0.6f, 0.2f}};
    static const float b[3][3] = {{-0.5f, -0.5f, 0.8f}, {0.8f, -0.5f, 0.8f}, {0.8f, 0.8f, 0.8f}};
    static const float c[3][3] = {{-0.5f, -0.5f, 0.5f}, {0.8f, 0.8f, 0.5f}, {0.8f, -0.5f, 0.5f}}; /* b reversed */
    static const float d[3][3] = {{-0.9f, 0.0f, 0.5f}, {0.9f, 0.0f, 0.5f}, {0.0f, 0.9f, 0.5f}};
    static const float e[3][3] = {{-0.5f, -0.5f, 0.1f}, {0.8f, 0.8f, 0.1f}, {0.8f, -0.5f, 0.1f}}; /* c in front of a */
    static const float f[3][3] = {{-0.8f, -0.8f, 0.2f}, {0.6f, -0.8f, 0.2f}, {-0.8f, 0.6f, 0.2f}}; /* a, coplanar */
    static const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    static const float green_half[4] = {0.0f, 1.0f, 0.0f, 0.5f};
    static const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    static const float faint[4] = {1.0f, 1.0f, 0.0f, 0.2f};
    put_triangle(0u, a, red);
    put_triangle(1u, b, green_half);
    put_triangle(2u, c, blue);
    put_triangle(3u, d, faint);
    put_triangle(4u, e, blue);
    put_triangle(5u, f, green_half);
}

__attribute__((unused)) static void stream_setup(stream_builder *stream)
{
    static const float offset[4] = {32.0f, 32.0f, 0.0f, 0.0f};
    static const float scale[4] = {32.0f, -32.0f, 1.0f, 0.0f};
    stream_viewport(stream, offset, scale);
    stream_pair(stream, GPU_PGRAPH_EXECUTION_MODE, 6u);
    stream_program(stream, 0u, synthetic_program, 1u);
    stream_pair(stream, GPU_PGRAPH_PROGRAM_START, 0u);
    static const float c3_tint[4] = {0.0f, 0.25f, 0.0f, 0.0f}; /* oD0 = v2.zyxw + c3: a tint the constant upload must carry */
    stream_constants(stream, 3u, c3_tint, 4u);
}

__attribute__((unused)) static void stream_draw(stream_builder *stream, uint32_t triangle, uint32_t op, uint32_t count)
{
    stream_array(stream, 1u, MEMORY_BASE + triangle * TRIANGLE_BYTES, array_format(28u, 3u, GPU_PGRAPH_TYPE_F));
    stream_array(stream, 2u, MEMORY_BASE + triangle * TRIANGLE_BYTES + 12u, array_format(28u, 4u, GPU_PGRAPH_TYPE_F));
    stream_draw_arrays(stream, op, 0u, count);
}

typedef enum {
    SCENE_OPAQUE,
    SCENE_BLEND,
    SCENE_DEPTH,
    SCENE_STENCIL,
    SCENE_CULL,
    SCENE_CULL_FRONT,
    SCENE_SCISSOR,
    SCENE_ALPHA,
    SCENE_LINES,
    SCENE_BLEND_CONSTANT,
    SCENE_COLOUR_MASK,
    SCENE_OFFSET,
    SCENE_COMBINER_MULTIPLY,
    SCENE_COMBINER_FINAL,
    SCENE_COMBINER_TWOVARY,
    SCENE_CLEAR_COLOUR,
    SCENE_CLEAR_RECT,
    SCENE_CLEAR_MASK,
    SCENE_CLEAR_DEPTH,
    SCENE_CLEAR_STENCIL,
    SCENE_FLIP_Y,
    SCENE_FLIP_Y_DEPTH,
    SCENE_POLYGON_LINE,
    SCENE_POLYGON_POINT,
    SCENE_POLYGON_ZERO,
    SCENE_COMBINER_ALPHA,
    SCENE_COUNT
} scene_kind;

__attribute__((unused)) static const char *const scene_names[SCENE_COUNT] = {"opaque", "blend", "depth", "stencil", "cull", "cull-front", "scissor", "alpha", "lines",
                                                         "blend-constant", "colour-mask", "offset", "combiner-multiply", "combiner-final", "combiner-twovary",
                                                         "clear-colour", "clear-rect", "clear-mask", "clear-depth", "clear-stencil", "flip-y", "flip-y-depth",
                                                         "polygon-line", "polygon-point", "polygon-zero", "combiner-alpha"};
/* The scenes that need a depth attachment (the window kind refuses them). */
__attribute__((unused)) static bool scene_needs_depth(scene_kind kind)
{
    return kind == SCENE_DEPTH || kind == SCENE_STENCIL || kind == SCENE_OFFSET || kind == SCENE_CLEAR_DEPTH ||
           kind == SCENE_CLEAR_STENCIL || kind == SCENE_FLIP_Y_DEPTH;
}

/* The scenes whose backend reverses the rows of the finished frame (flip_y). */
__attribute__((unused)) static bool scene_flips(scene_kind kind)
{
    return kind == SCENE_FLIP_Y || kind == SCENE_FLIP_Y_DEPTH;
}

/* The scenes with a clear that writes depth or stencil: the window pass has no such attachment and refuses that part. */
__attribute__((unused)) static bool scene_clears_depth(scene_kind kind)
{
    return kind == SCENE_CLEAR_DEPTH || kind == SCENE_CLEAR_STENCIL || kind == SCENE_FLIP_Y_DEPTH;
}

/* One clear as the library's Clear writes it: the rectangle (INCLUSIVE), the depth/stencil word, the colour, then the event. */
__attribute__((unused)) static void scene_clear(stream_builder *stream, uint32_t flags, uint32_t colour, uint32_t zstencil,
                                                uint32_t x_min, uint32_t x_max, uint32_t y_min, uint32_t y_max)
{
    stream_pair(stream, 0x1D98u, x_min | (x_max << 16));
    stream_pair(stream, 0x1D9Cu, y_min | (y_max << 16));
    stream_pair(stream, 0x1D8Cu, zstencil);
    stream_pair(stream, 0x1D90u, colour);
    stream_pair(stream, GPU_PGRAPH_CLEAR_SURFACE, flags);
}

__attribute__((unused)) static bool scene_uses_combiner(scene_kind kind)
{
    return kind == SCENE_COMBINER_MULTIPLY || kind == SCENE_COMBINER_FINAL || kind == SCENE_COMBINER_TWOVARY ||
           kind == SCENE_COMBINER_ALPHA;
}

/* The combiner words of the two combiner scenes (the same entries tests/c/test_gpu_combiner.c builds its modules from). */
#define COMBINER_WORDS GPU_PGRAPH_COMBINER_WORDS
static const struct {
    uint32_t index;
    uint32_t value;
} combiner_multiply_entries[] = {{34u, 0x01020000u}, {0u, 0xD4300000u}, {45u, 0xC0u}, {26u, 0xC0u}, {53u, 0x11101u},
                                 {8u, 0x0Cu}, {9u, 0x1C80u}, {10u, 0x00FF8040u}, {18u, 0x0080C0FFu}},
  combiner_final_entries[] = {{53u, 0x11101u}, {8u, 0x0Fu}, {9u, 0x01021180u}, {43u, 0x80FF4020u}, {44u, 0x00808080u}},
  combiner_twovary_entries[] = {{34u, 0x04050000u}, {0u, 0xD4300000u}, {45u, 0xC0u}, {26u, 0xC0u}, {53u, 0x11101u}, {8u, 0x0Cu},
                                {9u, 0x1C80u}, {10u, 0x00FF8040u}};

__attribute__((unused)) static void combiner_words_of(scene_kind kind, uint32_t words[COMBINER_WORDS])
{
    memset(words, 0, COMBINER_WORDS * sizeof words[0]);
    if (kind == SCENE_COMBINER_MULTIPLY) {
        for (size_t i = 0u; i < sizeof combiner_multiply_entries / sizeof combiner_multiply_entries[0]; i++) {
            words[combiner_multiply_entries[i].index] = combiner_multiply_entries[i].value;
        }
    } else if (kind == SCENE_COMBINER_ALPHA) {
        static const uint32_t pass_entries[][2] = {{34u, 0xC4200000u}, {0u, 0xD4300000u}, {45u, 0xC0u}, {26u, 0xC0u},
                                                   {53u, 0x11101u}, {8u, 0x0Cu}, {9u, 0x1C80u}};
        for (size_t i = 0u; i < sizeof pass_entries / sizeof pass_entries[0]; i++) {
            words[pass_entries[i][0]] = pass_entries[i][1];
        }
    } else if (kind == SCENE_COMBINER_TWOVARY) {
        for (size_t i = 0u; i < sizeof combiner_twovary_entries / sizeof combiner_twovary_entries[0]; i++) {
            words[combiner_twovary_entries[i].index] = combiner_twovary_entries[i].value;
        }
    } else {
        for (size_t i = 0u; i < sizeof combiner_final_entries / sizeof combiner_final_entries[0]; i++) {
            words[combiner_final_entries[i].index] = combiner_final_entries[i].value;
        }
    }
}

static const char *const fragment_names[] = {COMBINER_NAME_PASS, COMBINER_NAME_MULTIPLY, COMBINER_NAME_FINAL,
                                             COMBINER_NAME_TWOVARY, COMBINER_NAME_PASS_ALPHA};
static const struct gpu_vsh_table fragment_table = {0u, 0u, NULL, 0u, NULL, 5u, fragment_names};

__attribute__((unused)) static bool load_fragment_module(void *context, uint32_t module, const uint32_t **words,
                                                        size_t *word_count)
{
    (void)context;
    switch (module) {
    case 0u: *words = combiner_words_pass; *word_count = sizeof combiner_words_pass / 4u; return true;
    case 1u: *words = combiner_words_multiply; *word_count = sizeof combiner_words_multiply / 4u; return true;
    case 2u: *words = combiner_words_final; *word_count = sizeof combiner_words_final / 4u; return true;
    case 3u: *words = combiner_words_twovary; *word_count = sizeof combiner_words_twovary / 4u; return true;
    case 4u: *words = combiner_words_pass_alpha; *word_count = sizeof combiner_words_pass_alpha / 4u; return true;
    default: return false;
    }
}

__attribute__((unused)) static void build_stream(stream_builder *stream, scene_kind kind)
{
    stream_setup(stream);
    switch (kind) {
    case SCENE_OPAQUE:
        stream_draw(stream, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        stream_draw(stream, 1u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        break;
    case SCENE_BLEND:
        stream_draw(stream, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        stream_pair(stream, 0x0304u, 1u);
        stream_pair(stream, 0x0348u, 0x303u);
        stream_pair(stream, 0x0344u, 0x302u);
        stream_pair(stream, 0x0350u, 0x8006u);
        stream_draw(stream, 1u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        break;
    case SCENE_DEPTH:
        stream_pair(stream, 0x030Cu, 1u);
        stream_pair(stream, 0x0354u, 0x203u);
        stream_pair(stream, 0x035Cu, 1u);
        stream_draw(stream, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        stream_draw(stream, 2u, GPU_PGRAPH_OP_TRIANGLES, 3u); /* behind 0 where they overlap */
        break;
    case SCENE_STENCIL:
        stream_pair(stream, 0x030Cu, 1u);
        stream_pair(stream, 0x0354u, 0x203u);
        stream_pair(stream, 0x035Cu, 1u);
        stream_pair(stream, 0x032Cu, 1u);
        stream_pair(stream, 0x0364u, 0x207u); /* ALWAYS */
        stream_pair(stream, 0x0368u, 7u);
        stream_pair(stream, 0x036Cu, 0xFFu);
        stream_pair(stream, 0x0360u, 0xFFu);
        stream_pair(stream, 0x0370u, 0x1E00u);
        stream_pair(stream, 0x0374u, 0x1E00u);
        stream_pair(stream, 0x0378u, 0x1E01u); /* REPLACE: write 7 where it passes */
        stream_draw(stream, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        stream_pair(stream, 0x0364u, 0x202u); /* EQUAL: only where the first draw wrote 7 */
        stream_pair(stream, 0x0378u, 0x1E00u);
        stream_draw(stream, 4u, GPU_PGRAPH_OP_TRIANGLES, 3u); /* in front of 0: shows only where the stencil is 7 */
        break;
    case SCENE_CULL:
        stream_pair(stream, 0x03A0u, 0x900u);
        stream_pair(stream, 0x0308u, 1u);
        stream_pair(stream, 0x039Cu, 0x405u);
        stream_draw(stream, 1u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        stream_draw(stream, 2u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        break;
    case SCENE_CULL_FRONT:
        stream_pair(stream, 0x03A0u, 0x900u);
        stream_pair(stream, 0x0308u, 1u);
        stream_pair(stream, 0x039Cu, 0x404u);
        stream_draw(stream, 1u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        stream_draw(stream, 2u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        break;
    case SCENE_SCISSOR:
        stream_pair(stream, 0x0200u, 8u | (40u << 16));
        stream_pair(stream, 0x0204u, 12u | (30u << 16));
        stream_draw(stream, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        stream_draw(stream, 1u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        break;
    case SCENE_ALPHA:
        stream_pair(stream, 0x0300u, 1u);
        stream_pair(stream, 0x033Cu, 0x204u); /* GREATER */
        stream_pair(stream, 0x0340u, 100u);
        stream_draw(stream, 3u, GPU_PGRAPH_OP_TRIANGLES, 3u); /* alpha 0.2: fails */
        stream_draw(stream, 1u, GPU_PGRAPH_OP_TRIANGLES, 3u); /* alpha 0.5: passes */
        break;
    case SCENE_LINES:
        stream_draw(stream, 0u, GPU_PGRAPH_OP_LINES, 2u);
        stream_draw(stream, 2u, GPU_PGRAPH_OP_LINE_STRIP, 3u);
        break;
    case SCENE_BLEND_CONSTANT:
        stream_draw(stream, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        stream_pair(stream, 0x0304u, 1u);
        stream_pair(stream, 0x0344u, 0x8001u); /* CONSTANT_COLOR */
        stream_pair(stream, 0x0348u, 0x0001u); /* ONE */
        stream_pair(stream, 0x0350u, 0x8006u);
        stream_pair(stream, 0x034Cu, 0xFF804020u);
        stream_draw(stream, 4u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        break;
    case SCENE_COLOUR_MASK:
        stream_draw(stream, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        stream_pair(stream, 0x0358u, 0x01000101u); /* no red write */
        stream_draw(stream, 1u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        break;
    case SCENE_OFFSET:
        stream_pair(stream, 0x030Cu, 1u);
        stream_pair(stream, 0x0354u, 0x203u); /* LEQUAL */
        stream_pair(stream, 0x035Cu, 1u);
        stream_draw(stream, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        stream_pair(stream, 0x0338u, 1u); /* fill offset on, triangle 5 is coplanar with triangle 0 */
        stream_pair(stream, 0x0384u, 0x00000000u);
        stream_pair(stream, 0x0388u, 0x40800000u); /* +4.0 ulps of depth: pushes the coplanar triangle behind, LEQUAL then fails */
        stream_draw(stream, 5u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        break;
    case SCENE_COMBINER_MULTIPLY: {
        uint32_t words[COMBINER_WORDS];
        combiner_words_of(kind, words);
        stream_pixel_shader(stream, words);
        stream_draw(stream, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        stream_pair(stream, 0x0A60u, 0x00808040u); /* factor0[0] changes between the draws */
        stream_draw(stream, 1u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        break;
    }
    case SCENE_COMBINER_TWOVARY:
    case SCENE_COMBINER_FINAL: {
        uint32_t words[COMBINER_WORDS];
        combiner_words_of(kind, words);
        stream_pixel_shader(stream, words);
        stream_draw(stream, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        stream_draw(stream, 1u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        break;
    }
    case SCENE_CLEAR_COLOUR:
        scene_clear(stream, 0xF0u, 0xFF3060C0u, 0u, 0u, WIDTH - 1u, 0u, HEIGHT - 1u); /* every channel, the whole target */
        stream_draw(stream, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        stream_draw(stream, 1u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        break;
    case SCENE_CLEAR_RECT:
        stream_draw(stream, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        scene_clear(stream, 0xF0u, 0xFFC08020u, 0u, 4u, 27u, 10u, 50u); /* a rectangle over the first draw, between draws */
        stream_draw(stream, 1u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        scene_clear(stream, 0xF0u, 0x80204060u, 0u, 50u, 63u, 0u, 7u); /* and one after the last draw, a corner */
        break;
    case SCENE_CLEAR_MASK:
        stream_draw(stream, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        scene_clear(stream, 0x50u, 0xFFAABBCCu, 0u, 8u, 40u, 8u, 40u); /* red and blue only: green and alpha stay */
        scene_clear(stream, 0x20u, 0x00112233u, 0u, 30u, 58u, 30u, 58u); /* green only, overlapping the first */
        stream_draw(stream, 1u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        break;
    case SCENE_CLEAR_DEPTH:
        stream_pair(stream, 0x030Cu, 1u);
        stream_pair(stream, 0x0354u, 0x203u); /* LEQUAL */
        stream_pair(stream, 0x035Cu, 1u);
        stream_pair(stream, 0x032Cu, 1u); /* stencil EQUAL 3 with KEEP: passes only while the depth clear left the stencil alone */
        stream_pair(stream, 0x0364u, 0x202u);
        stream_pair(stream, 0x0368u, 3u);
        stream_pair(stream, 0x036Cu, 0xFFu);
        stream_pair(stream, 0x0360u, 0xFFu);
        stream_pair(stream, 0x0370u, 0x1E00u);
        stream_pair(stream, 0x0374u, 0x1E00u);
        stream_pair(stream, 0x0378u, 0x1E00u);
        scene_clear(stream, 0x02u, 0u, 0x00000003u, 0u, WIDTH - 1u, 0u, HEIGHT - 1u); /* stencil 3 */
        scene_clear(stream, 0xF1u, 0xFF204060u, 0x80000007u, 0u, WIDTH - 1u, 0u, HEIGHT - 1u); /* colour and depth 0.5, 7 unflagged */
        stream_draw(stream, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u); /* z 0.2 passes */
        stream_draw(stream, 1u, GPU_PGRAPH_OP_TRIANGLES, 3u); /* z 0.8 fails against 0.5 everywhere: without the clear it shows */
        break;
    case SCENE_CLEAR_STENCIL:
        stream_pair(stream, 0x030Cu, 1u); /* depth LEQUAL: the stencil clear's zstencil word holds depth 0, which must not be applied */
        stream_pair(stream, 0x0354u, 0x203u);
        stream_pair(stream, 0x035Cu, 1u);
        stream_pair(stream, 0x032Cu, 1u);
        stream_pair(stream, 0x0364u, 0x202u); /* EQUAL */
        stream_pair(stream, 0x0368u, 7u);
        stream_pair(stream, 0x036Cu, 0xFFu);
        stream_pair(stream, 0x0360u, 0xFFu);
        stream_pair(stream, 0x0370u, 0x1E00u);
        stream_pair(stream, 0x0374u, 0x1E00u);
        stream_pair(stream, 0x0378u, 0x1E00u); /* KEEP: only the clear writes stencil */
        scene_clear(stream, 0x02u, 0u, 0x00000007u, 10u, 45u, 12u, 36u); /* stencil 7 over a rectangle */
        stream_draw(stream, 1u, GPU_PGRAPH_OP_TRIANGLES, 3u); /* shows only inside it */
        break;
    case SCENE_FLIP_Y:
        scene_clear(stream, 0xF0u, 0xFFC08020u, 0u, 4u, 27u, 6u, 30u);
        stream_pair(stream, 0x0200u, 2u | (60u << 16));
        stream_pair(stream, 0x0204u, 10u | (30u << 16)); /* a scissor that is not symmetric in y */
        stream_pair(stream, 0x03A0u, 0x900u);
        stream_pair(stream, 0x0308u, 1u);
        stream_pair(stream, 0x039Cu, 0x405u); /* cull back: one of the two mirror windings goes */
        stream_draw(stream, 1u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        stream_draw(stream, 2u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        stream_draw(stream, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        break;
    case SCENE_FLIP_Y_DEPTH:
        stream_pair(stream, 0x030Cu, 1u);
        stream_pair(stream, 0x0354u, 0x203u);
        stream_pair(stream, 0x035Cu, 1u);
        scene_clear(stream, 0xF1u, 0xFF204060u, 0x80000000u, 0u, WIDTH - 1u, 0u, 39u); /* top rows only, in the title's rows */
        stream_draw(stream, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        stream_draw(stream, 1u, GPU_PGRAPH_OP_TRIANGLES, 3u); /* hidden in the cleared rows, shown below them */
        break;
    case SCENE_POLYGON_LINE:
    case SCENE_POLYGON_POINT:
    case SCENE_POLYGON_ZERO: {
        /* T860/T885: LINE and POINT are supported; zero remains a negative unknown-value case. */
        const uint32_t mode = kind == SCENE_POLYGON_LINE ? 0x1B01u : kind == SCENE_POLYGON_POINT ? 0x1B00u : 0u;
        stream_pair(stream, 0x038Cu, mode);
        stream_pair(stream, 0x0390u, mode);
        stream_draw(stream, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        stream_draw(stream, 1u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        break;
    }
    case SCENE_COMBINER_ALPHA: {
        /* T860: the SCENE_ALPHA draws through the combiner stage, whose `_alpha` module runs the test */
        uint32_t words[COMBINER_WORDS];
        combiner_words_of(kind, words);
        stream_pixel_shader(stream, words);
        stream_pair(stream, 0x0300u, 1u);
        stream_pair(stream, 0x033Cu, 0x204u); /* GREATER */
        stream_pair(stream, 0x0340u, 100u);
        stream_draw(stream, 3u, GPU_PGRAPH_OP_TRIANGLES, 3u); /* alpha 0.2: fails */
        stream_draw(stream, 1u, GPU_PGRAPH_OP_TRIANGLES, 3u); /* alpha 0.5: passes */
        break;
    }
    default:
        break;
    }
}

typedef struct {
    gpu_pgraph *model;
    fake_guest guest;
} scene;

__attribute__((unused)) static scene make_scene(scene_kind kind)
{
    scene result;
    result.guest = (fake_guest){MEMORY_BASE, memory, sizeof memory};
    result.model = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(result.model, ALL_GROUPS | GPU_PGRAPH_OUTPUT_POLYGON_OFFSET);
    gpu_pgraph_set_combiner(result.model, scene_uses_combiner(kind));
    stream_builder stream = {0};
    build_stream(&stream, kind);
    CHECK(gpu_pgraph_decode(result.model, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    stream_free(&stream);
    return result;
}

__attribute__((unused)) static gpu_pgraph_backend make_backend_for(scene_kind kind, fake_guest *guest, uint32_t inferences)
{
    gpu_pgraph_backend backend = {0};
    backend.table = &table;
    backend.read_guest = fake_guest_read;
    backend.load_module = load_module;
    backend.context = guest;
    backend.allowed_inferences = inferences;
    backend.output_groups = ALL_GROUPS | GPU_PGRAPH_OUTPUT_POLYGON_OFFSET;
    backend.line_width = 1.0f;
    backend.flip_y = scene_flips(kind);
    backend.combiner = scene_uses_combiner(kind);
    backend.fragment_table = &fragment_table;
    backend.load_fragment_module = load_fragment_module;
    return backend;
}

__attribute__((unused)) static gpu_pgraph_backend make_backend(fake_guest *guest, uint32_t inferences)
{
    return make_backend_for(SCENE_OPAQUE, guest, inferences);
}

#endif
