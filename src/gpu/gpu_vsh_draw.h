/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_VSH_DRAW_H
#define TSFP_GPU_VSH_DRAW_H

#include "gpu_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Draws a translated NV2A vertex shader (tools/nv2a/translate.py vertex_shader_source, selected
 * by gpu_vsh_select.h) through the renderer's own device, with SEVERAL vertices in ONE draw call
 * (T100d). The module's interface is the one the translator emits:
 *
 *   input  location n    vec4 vn, n = 0..15, taken from the vertex buffer at byte offset 16 * n
 *   uniform set 0 bind 0 std140 block, 192 vec4 (the hardware constant file, D3D8 c[n] is row n + 96)
 *   output location 0..8 the varyings (oD0 is location 0), gl_Position, gl_PointSize
 *
 * The constants are shared by every vertex of the draw. The attributes are per vertex. */
#define GPU_VSH_INPUTS 16u
#define GPU_VSH_CONSTANT_ROWS 192u
/* Floats per vertex in `attributes` (16 vec4) and in a capture (the shader's xfb_stride, 256 bytes). */
#define GPU_VSH_ATTRIBUTE_FLOATS (GPU_VSH_INPUTS * 4u)
#define GPU_VSH_CAPTURE_FLOATS 64u
#define GPU_VSH_MAX_VERTICES 65536u

/* Primitive topology of gpu_vsh_render's vertices (T84b). The zero value is the triangle list, so a
 * draw that never names one is what it always was. */
#define GPU_VSH_TOPOLOGY_TRIANGLE_LIST 0u
#define GPU_VSH_TOPOLOGY_POINT_LIST 1u
#define GPU_VSH_TOPOLOGY_LINE_LIST 2u

/* An optional replacement for the fixed fragment stage (T75): a translated register-combiner module
 * (tools/nv2a_combiner/glsl.py replay_fragment_shader), entry point "main", whose interface is
 *
 *   input  location n    the vertex module's output at location n (the module must write every one
 *                        it reads, the caller checks that with gpu_spirv_interface_locations)
 *   uniform set 0 bind 1 std140 block of GPU_VSH_FRAGMENT_VEC4S vec4: factor0[8], factor1[8],
 *                        final_c0, final_c1, fog_color, alpha_ref
 *   sampler set 0 bind 2 + n  sampler2D, ONLY for a stage n whose `textures[n].rgba` is set
 *   output location 0    vec4, the colour written to the target
 *
 * set 0 binding 0 stays the vertex constants block. gpu_vsh_render only, gpu_vsh_capture ignores it.
 * A texture is RGBA8, row 0 on top, sampled NEAREST with the edge CLAMPED (a stated test contract,
 * not the title's texture state). */
#define GPU_VSH_FRAGMENT_VEC4S 20u
#define GPU_VSH_FRAGMENT_TEXTURES 4u
typedef struct {
    const uint8_t *rgba; /* NULL: no texture for the stage */
    uint32_t width;      /* 1 .. 4096 */
    uint32_t height;
    bool linear;         /* T510: bilinear magnification and minification, else nearest (the default) */
    bool repeat;         /* T735: U and V wrap (repeat), else clamp to the edge (the default) */
} gpu_vsh_texture;

typedef struct {
    const uint32_t *words;
    size_t word_count;
    const float *constants; /* GPU_VSH_FRAGMENT_VEC4S * 4 */
    gpu_vsh_texture textures[GPU_VSH_FRAGMENT_TEXTURES];
} gpu_vsh_fragment;

/* OUTPUT STATE of a draw (T267, OPT-IN). gpu_vsh_draw.output NULL, the default, is the draw as it always
 * was: the whole target, no culling, no blending, no depth. Every field is the decoded hardware state
 * already turned into plain numbers by gpu_pgraph_replay.c, which is where the NV2A encodings and what is
 * measured about them are. gpu_vsh_render only, gpu_vsh_capture ignores it.
 *
 * `scissor`: only pixels in [scissor_x, scissor_x + scissor_width) x [scissor_y, scissor_y +
 * scissor_height), in target pixels with y down from row 0, are written. The rectangle must lie inside
 * the target (GPU_ERR_ARGUMENT otherwise). A zero width or height writes nothing.
 *
 * `cull_mode` GPU_VSH_CULL_*: triangles facing that way are not drawn (points and lines are never
 * culled). A triangle is clockwise when its vertices run clockwise in the TARGET as displayed with row 0
 * at the top, which is Vulkan's own rule for a y-down framebuffer; `front_clockwise` says which winding
 * is the front face. Any other cull_mode is GPU_ERR_ARGUMENT.
 *
 * `blend`: the fragment colour is blended into the target with the factors and equation below
 * (GPU_VSH_BLEND_*, `blend_constant` is the constant colour as R, G, B, A in 0..1). Out of range factors
 * and equations are GPU_ERR_ARGUMENT. `color_write_disable` is a mask of the channels NOT written
 * (GPU_VSH_CHANNEL_*), so a zeroed struct writes every channel. `destination`, when not NULL, is the
 * width x height x 4 bytes of RGBA8 (row 0 first, stride width * 4) the target STARTS as: the draw is then
 * rendered onto those pixels instead of onto the clear colour, which is what a blend or a partial colour
 * write needs and what makes the readback the frame itself rather than a coverage image.
 *
 * `alpha_test`: a fragment whose alpha, clamped and rounded to a byte, fails the comparison with
 * `alpha_ref` (0..255) under `alpha_func` (GPU_VSH_COMPARE_*) writes nothing. Only the fixed fragment stage
 * has it: with `fragment` set it is GPU_ERR_ARGUMENT.
 *
 * `depth_test` / `stencil_test`: the draw runs against a depth and stencil buffer the CALLER holds across
 * draws, `depth` (width * height floats, the depth as Vulkan holds it, 0 near .. 1 far) and `stencil` (width *
 * height bytes). Both are read as the buffers' contents before the draw and written back after it, so a
 * sequence of draws sees one buffer. Either pointer NULL with its test on is GPU_ERR_ARGUMENT. `depth_func` and
 * `stencil_func` are GPU_VSH_COMPARE_*, `depth_write` allows the depth write (only while `depth_test`), the
 * stencil state is one-sided (front and back alike): a fragment's stencil value is compared as
 * (reference & compare_mask) func (stored & compare_mask) and updated under `stencil_write_mask` by the op of
 * the outcome (GPU_VSH_STENCIL_OP_*), where `stencil_fail_op` is a stencil test failure, `stencil_zfail_op` a
 * stencil pass and depth test failure and `stencil_zpass_op` both passing. Reference and masks are 0..255, an
 * unknown op or compare is GPU_ERR_ARGUMENT, a stencil test with a replacement fragment stage is allowed. The
 * buffers are float32 depth with an 8 bit stencil on the device (VK_FORMAT_D32_SFLOAT_S8_UINT).
 *
 * `depth_bias` (T502, polygon offset): the depth of every TRIANGLE fragment is offset by Vulkan's depth bias,
 * `depth_bias_constant` * r + `depth_bias_slope` * m (r the smallest resolvable depth difference, m the
 * triangle's depth slope, clamp 0), before the depth test and the depth write. It needs `depth_test` (a bias
 * with no depth test is GPU_ERR_ARGUMENT, nothing could observe it) and finite factors. Lines and points are
 * never biased: the pipeline carries no bias for those topologies, so the result does not depend on whether an
 * implementation biases them. */
#define GPU_VSH_STENCIL_OP_KEEP 0u
#define GPU_VSH_STENCIL_OP_ZERO 1u
#define GPU_VSH_STENCIL_OP_REPLACE 2u
#define GPU_VSH_STENCIL_OP_INCREMENT_CLAMP 3u
#define GPU_VSH_STENCIL_OP_DECREMENT_CLAMP 4u
#define GPU_VSH_STENCIL_OP_INVERT 5u
#define GPU_VSH_STENCIL_OP_INCREMENT_WRAP 6u
#define GPU_VSH_STENCIL_OP_DECREMENT_WRAP 7u
#define GPU_VSH_STENCIL_OPS 8u
#define GPU_VSH_BLEND_ZERO 0u
#define GPU_VSH_BLEND_ONE 1u
#define GPU_VSH_BLEND_SRC_COLOR 2u
#define GPU_VSH_BLEND_ONE_MINUS_SRC_COLOR 3u
#define GPU_VSH_BLEND_SRC_ALPHA 4u
#define GPU_VSH_BLEND_ONE_MINUS_SRC_ALPHA 5u
#define GPU_VSH_BLEND_DST_ALPHA 6u
#define GPU_VSH_BLEND_ONE_MINUS_DST_ALPHA 7u
#define GPU_VSH_BLEND_DST_COLOR 8u
#define GPU_VSH_BLEND_ONE_MINUS_DST_COLOR 9u
#define GPU_VSH_BLEND_SRC_ALPHA_SATURATE 10u
#define GPU_VSH_BLEND_CONSTANT_COLOR 11u
#define GPU_VSH_BLEND_ONE_MINUS_CONSTANT_COLOR 12u
#define GPU_VSH_BLEND_CONSTANT_ALPHA 13u
#define GPU_VSH_BLEND_ONE_MINUS_CONSTANT_ALPHA 14u
#define GPU_VSH_BLEND_FACTORS 15u
#define GPU_VSH_BLEND_OP_ADD 0u
#define GPU_VSH_BLEND_OP_SUBTRACT 1u
#define GPU_VSH_BLEND_OP_REVERSE_SUBTRACT 2u
#define GPU_VSH_BLEND_OP_MIN 3u
#define GPU_VSH_BLEND_OP_MAX 4u
#define GPU_VSH_BLEND_OPS 5u
#define GPU_VSH_CHANNEL_R 1u
#define GPU_VSH_CHANNEL_G 2u
#define GPU_VSH_CHANNEL_B 4u
#define GPU_VSH_CHANNEL_A 8u
#define GPU_VSH_COMPARE_NEVER 0u
#define GPU_VSH_COMPARE_LESS 1u
#define GPU_VSH_COMPARE_EQUAL 2u
#define GPU_VSH_COMPARE_LEQUAL 3u
#define GPU_VSH_COMPARE_GREATER 4u
#define GPU_VSH_COMPARE_NOTEQUAL 5u
#define GPU_VSH_COMPARE_GEQUAL 6u
#define GPU_VSH_COMPARE_ALWAYS 7u
#define GPU_VSH_CULL_NONE 0u
#define GPU_VSH_CULL_FRONT 1u
#define GPU_VSH_CULL_BACK 2u
/* T860: gpu_vsh_output.polygon_mode, the VkPolygonMode of a triangle. */
#define GPU_VSH_POLYGON_FILL 0u
#define GPU_VSH_POLYGON_LINE 1u
#define GPU_VSH_POLYGON_POINT 2u
typedef struct {
    bool scissor;
    uint32_t scissor_x;
    uint32_t scissor_y;
    uint32_t scissor_width;
    uint32_t scissor_height;
    uint32_t cull_mode;
    bool front_clockwise;
    bool blend;
    uint32_t blend_source;      /* GPU_VSH_BLEND_* */
    uint32_t blend_destination; /* GPU_VSH_BLEND_* */
    uint32_t blend_equation;    /* GPU_VSH_BLEND_OP_* */
    float blend_constant[4];
    uint32_t color_write_disable; /* GPU_VSH_CHANNEL_* of the channels not written */
    const uint8_t *destination;   /* NULL: the target starts as the clear colour */
    bool alpha_test;
    uint32_t alpha_func; /* GPU_VSH_COMPARE_* */
    uint32_t alpha_ref;  /* 0 .. 255 */
    bool depth_test;
    bool depth_write;
    uint32_t depth_func; /* GPU_VSH_COMPARE_* */
    bool stencil_test;
    uint32_t stencil_func; /* GPU_VSH_COMPARE_* */
    uint32_t stencil_ref;
    uint32_t stencil_compare_mask;
    uint32_t stencil_write_mask;
    uint32_t stencil_fail_op;  /* GPU_VSH_STENCIL_OP_* */
    uint32_t stencil_zfail_op;
    uint32_t stencil_zpass_op;
    bool depth_bias;
    float depth_bias_constant; /* NV2A bias (0x0388) as Vulkan's depthBiasConstantFactor */
    float depth_bias_slope;    /* NV2A scale factor (0x0384) as Vulkan's depthBiasSlopeFactor */
    /* T860: the front and back polygon mode words (0x038C, 0x0390) after the replay's decode, GPU_VSH_POLYGON_*. Only triangles are
     * rasterized by it (a point or line list ignores it). LINE and POINT need a device made with fillModeNonSolid
     * (gpu_device_native.fill_mode_non_solid), else the draw is GPU_ERR_ARGUMENT. 0 (the zero-initialised default) is FILL. */
    uint32_t polygon_mode;
    float *depth;       /* width * height, in and out, see above */
    uint8_t *stencil;   /* width * height, in and out */
} gpu_vsh_output;

typedef struct {
    const uint32_t *words; /* SPIR-V of the vertex module, entry point "main" */
    size_t word_count;
    uint32_t vertex_count; /* 1 .. GPU_VSH_MAX_VERTICES */
    const float *attributes; /* vertex_count * GPU_VSH_ATTRIBUTE_FLOATS */
    const float *constants; /* GPU_VSH_CONSTANT_ROWS * 4 */
    /* gpu_vsh_render only (gpu_vsh_capture ignores both). */
    uint32_t topology; /* GPU_VSH_TOPOLOGY_*, 0 = triangle list */
    /* Line width in pixels, read for the line list only and then REQUIRED to be exactly 1.0f: the
     * device is created without wideLines, so Vulkan guarantees no other width. A point's size is
     * the module's own gl_PointSize (oPts.x), and only 1.0 is defined without largePoints. */
    float line_width;
    /* gpu_vsh_render only: NULL (the default) is the fixed fragment stage vsh_draw.frag. */
    const gpu_vsh_fragment *fragment;
    /* gpu_vsh_render only: NULL (the default) is no output state, see gpu_vsh_output. */
    const gpu_vsh_output *output;
} gpu_vsh_draw;

/* Run the draw as a point list with rasterizer discard and capture every vertex's outputs with
 * VK_EXT_transform_feedback into out_capture (vertex_count * GPU_VSH_CAPTURE_FLOATS floats).
 * The MODULE must carry the xfb layout qualifiers (xfb_buffer 0, xfb_stride 256, xfb_offset
 * 16 * NV2A output address, see tools/nv2a/vertex_device.py capture_source). Floats the module
 * never writes keep the sentinel GPU_VSH_CAPTURE_SENTINEL_BITS. Returns GPU_ERR_ARGUMENT when
 * the device has no transform feedback (gpu_device_has_transform_feedback). */
#define GPU_VSH_CAPTURE_SENTINEL_BITS 0x7FC0DEADu
gpu_result gpu_vsh_capture(gpu_device *device, const gpu_vsh_draw *draw, float *out_capture);

/* Clear a width x height RGBA8 target and draw the vertices as a triangle list by default (a remainder
 * of fewer than three vertices draws nothing), as a point list or as a line list (a trailing odd
 * vertex draws nothing) when draw->topology says so, with the module and the fixed fragment stage
 * src/gpu/shaders/vsh_draw.frag, which writes oD0 (location 0) unchanged. Culling, blending and
 * depth are off. The module must write oD0. */
gpu_result gpu_vsh_render(gpu_device *device, uint32_t width, uint32_t height,
                          const float clear_rgba[4], const gpu_vsh_draw *draw,
                          gpu_image *out_image);

#endif
