/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gpu_vsh_draw (T100d): several vertices in ONE draw through the renderer's own device, with the
 * exact values captured by transform feedback and the pixels rendered. Self-contained: the two
 * vertex modules below are tiny hand-written shaders with the interface of the translated NV2A
 * vertex shaders (v1/v2 inputs, std140 c[192] at set 0 binding 0, oD0 at location 0), so this
 * runs without the gitignored corpus. The real translated programs, the generated selection
 * table and the T96 inverse are covered by tests/test_gpu_vsh_draw.py.
 *
 * Every device that exists is exercised ("hardware" and "software", the selector of T100d). A
 * device that is absent prints a stated SKIP, and when nothing at all ran the test exits 77 (a
 * ctest SKIP, not a pass).
 *
 * capture_vertex_words (src: GLSL below) and draw_vertex_words are glslang output, spirv-opt
 * --strip-debug -O, spirv-val clean:
 *
 *   #version 450                                      // capture
 *   layout(location = 1) in vec4 v1;
 *   layout(std140, set = 0, binding = 0) uniform Constants { vec4 c[192]; };
 *   layout(location = 0, xfb_buffer = 0, xfb_offset = 48, xfb_stride = 256) out vec4 oD0;
 *   out gl_PerVertex { layout(xfb_buffer = 0, xfb_offset = 0, xfb_stride = 256) vec4 gl_Position; };
 *   void main() { gl_Position = v1 * 2.0 + c[5]; oD0 = c[7] + v1.wzyx; }
 *
 *   #version 450                                      // draw
 *   layout(location = 1) in vec4 v1;
 *   layout(location = 2) in vec4 v2;
 *   layout(std140, set = 0, binding = 0) uniform Constants { vec4 c[192]; };
 *   layout(location = 0) out vec4 oD0;
 *   void main() { gl_Position = v1; oD0 = v2.zyxw + c[3]; }
 */
#include "gpu_device.h"
#include "gpu_vsh_draw.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t capture_vertex_words[] = {
    0x07230203u, 0x00010300u, 0x0008000bu, 0x00000026u, 0x00000000u, 0x00020011u,
    0x00000001u, 0x00020011u, 0x00000035u, 0x0006000bu, 0x00000001u, 0x4c534c47u,
    0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0008000fu, 0x00000000u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x0000000au,
    0x0000000eu, 0x0000001fu, 0x00030010u, 0x00000004u, 0x0000000bu, 0x00030047u,
    0x00000008u, 0x00000002u, 0x00050048u, 0x00000008u, 0x00000000u, 0x0000000bu,
    0x00000000u, 0x00050048u, 0x00000008u, 0x00000000u, 0x00000023u, 0x00000000u,
    0x00040047u, 0x0000000au, 0x00000024u, 0x00000000u, 0x00040047u, 0x0000000au,
    0x00000025u, 0x00000100u, 0x00040047u, 0x0000000eu, 0x0000001eu, 0x00000001u,
    0x00040047u, 0x00000014u, 0x00000006u, 0x00000010u, 0x00030047u, 0x00000015u,
    0x00000002u, 0x00050048u, 0x00000015u, 0x00000000u, 0x00000023u, 0x00000000u,
    0x00040047u, 0x00000017u, 0x00000021u, 0x00000000u, 0x00040047u, 0x00000017u,
    0x00000022u, 0x00000000u, 0x00040047u, 0x0000001fu, 0x0000001eu, 0x00000000u,
    0x00040047u, 0x0000001fu, 0x00000023u, 0x00000030u, 0x00040047u, 0x0000001fu,
    0x00000024u, 0x00000000u, 0x00040047u, 0x0000001fu, 0x00000025u, 0x00000100u,
    0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u,
    0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000004u,
    0x0003001eu, 0x00000008u, 0x00000007u, 0x00040020u, 0x00000009u, 0x00000003u,
    0x00000008u, 0x0004003bu, 0x00000009u, 0x0000000au, 0x00000003u, 0x00040015u,
    0x0000000bu, 0x00000020u, 0x00000001u, 0x0004002bu, 0x0000000bu, 0x0000000cu,
    0x00000000u, 0x00040020u, 0x0000000du, 0x00000001u, 0x00000007u, 0x0004003bu,
    0x0000000du, 0x0000000eu, 0x00000001u, 0x0004002bu, 0x00000006u, 0x00000010u,
    0x40000000u, 0x00040015u, 0x00000012u, 0x00000020u, 0x00000000u, 0x0004002bu,
    0x00000012u, 0x00000013u, 0x000000c0u, 0x0004001cu, 0x00000014u, 0x00000007u,
    0x00000013u, 0x0003001eu, 0x00000015u, 0x00000014u, 0x00040020u, 0x00000016u,
    0x00000002u, 0x00000015u, 0x0004003bu, 0x00000016u, 0x00000017u, 0x00000002u,
    0x0004002bu, 0x0000000bu, 0x00000018u, 0x00000005u, 0x00040020u, 0x00000019u,
    0x00000002u, 0x00000007u, 0x00040020u, 0x0000001du, 0x00000003u, 0x00000007u,
    0x0004003bu, 0x0000001du, 0x0000001fu, 0x00000003u, 0x0004002bu, 0x0000000bu,
    0x00000020u, 0x00000007u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u,
    0x00000003u, 0x000200f8u, 0x00000005u, 0x0004003du, 0x00000007u, 0x0000000fu,
    0x0000000eu, 0x0005008eu, 0x00000007u, 0x00000011u, 0x0000000fu, 0x00000010u,
    0x00060041u, 0x00000019u, 0x0000001au, 0x00000017u, 0x0000000cu, 0x00000018u,
    0x0004003du, 0x00000007u, 0x0000001bu, 0x0000001au, 0x00050081u, 0x00000007u,
    0x0000001cu, 0x00000011u, 0x0000001bu, 0x00050041u, 0x0000001du, 0x0000001eu,
    0x0000000au, 0x0000000cu, 0x0003003eu, 0x0000001eu, 0x0000001cu, 0x00060041u,
    0x00000019u, 0x00000021u, 0x00000017u, 0x0000000cu, 0x00000020u, 0x0004003du,
    0x00000007u, 0x00000022u, 0x00000021u, 0x0009004fu, 0x00000007u, 0x00000024u,
    0x0000000fu, 0x0000000fu, 0x00000003u, 0x00000002u, 0x00000001u, 0x00000000u,
    0x00050081u, 0x00000007u, 0x00000025u, 0x00000022u, 0x00000024u, 0x0003003eu,
    0x0000001fu, 0x00000025u, 0x000100fdu, 0x00010038u,
};

static const uint32_t draw_vertex_words[] = {
    0x07230203u, 0x00010300u, 0x0008000bu, 0x00000023u, 0x00000000u, 0x00020011u,
    0x00000001u, 0x0006000bu, 0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu,
    0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u, 0x0009000fu, 0x00000000u,
    0x00000004u, 0x6e69616du, 0x00000000u, 0x0000000du, 0x00000011u, 0x00000015u,
    0x00000016u, 0x00030047u, 0x0000000bu, 0x00000002u, 0x00050048u, 0x0000000bu,
    0x00000000u, 0x0000000bu, 0x00000000u, 0x00050048u, 0x0000000bu, 0x00000001u,
    0x0000000bu, 0x00000001u, 0x00050048u, 0x0000000bu, 0x00000002u, 0x0000000bu,
    0x00000003u, 0x00050048u, 0x0000000bu, 0x00000003u, 0x0000000bu, 0x00000004u,
    0x00040047u, 0x00000011u, 0x0000001eu, 0x00000001u, 0x00040047u, 0x00000015u,
    0x0000001eu, 0x00000000u, 0x00040047u, 0x00000016u, 0x0000001eu, 0x00000002u,
    0x00040047u, 0x0000001au, 0x00000006u, 0x00000010u, 0x00030047u, 0x0000001bu,
    0x00000002u, 0x00050048u, 0x0000001bu, 0x00000000u, 0x00000023u, 0x00000000u,
    0x00040047u, 0x0000001du, 0x00000021u, 0x00000000u, 0x00040047u, 0x0000001du,
    0x00000022u, 0x00000000u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u,
    0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u,
    0x00000006u, 0x00000004u, 0x00040015u, 0x00000008u, 0x00000020u, 0x00000000u,
    0x0004002bu, 0x00000008u, 0x00000009u, 0x00000001u, 0x0004001cu, 0x0000000au,
    0x00000006u, 0x00000009u, 0x0006001eu, 0x0000000bu, 0x00000007u, 0x00000006u,
    0x0000000au, 0x0000000au, 0x00040020u, 0x0000000cu, 0x00000003u, 0x0000000bu,
    0x0004003bu, 0x0000000cu, 0x0000000du, 0x00000003u, 0x00040015u, 0x0000000eu,
    0x00000020u, 0x00000001u, 0x0004002bu, 0x0000000eu, 0x0000000fu, 0x00000000u,
    0x00040020u, 0x00000010u, 0x00000001u, 0x00000007u, 0x0004003bu, 0x00000010u,
    0x00000011u, 0x00000001u, 0x00040020u, 0x00000013u, 0x00000003u, 0x00000007u,
    0x0004003bu, 0x00000013u, 0x00000015u, 0x00000003u, 0x0004003bu, 0x00000010u,
    0x00000016u, 0x00000001u, 0x0004002bu, 0x00000008u, 0x00000019u, 0x000000c0u,
    0x0004001cu, 0x0000001au, 0x00000007u, 0x00000019u, 0x0003001eu, 0x0000001bu,
    0x0000001au, 0x00040020u, 0x0000001cu, 0x00000002u, 0x0000001bu, 0x0004003bu,
    0x0000001cu, 0x0000001du, 0x00000002u, 0x0004002bu, 0x0000000eu, 0x0000001eu,
    0x00000003u, 0x00040020u, 0x0000001fu, 0x00000002u, 0x00000007u, 0x00050036u,
    0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u,
    0x0004003du, 0x00000007u, 0x00000012u, 0x00000011u, 0x00050041u, 0x00000013u,
    0x00000014u, 0x0000000du, 0x0000000fu, 0x0003003eu, 0x00000014u, 0x00000012u,
    0x0004003du, 0x00000007u, 0x00000017u, 0x00000016u, 0x0009004fu, 0x00000007u,
    0x00000018u, 0x00000017u, 0x00000017u, 0x00000002u, 0x00000001u, 0x00000000u,
    0x00000003u, 0x00060041u, 0x0000001fu, 0x00000020u, 0x0000001du, 0x0000000fu,
    0x0000001eu, 0x0004003du, 0x00000007u, 0x00000021u, 0x00000020u, 0x00050081u,
    0x00000007u, 0x00000022u, 0x00000018u, 0x00000021u, 0x0003003eu, 0x00000015u,
    0x00000022u, 0x000100fdu, 0x00010038u,
};

static int failures;
static int checks;

static void check(int condition, const char *what)
{
    checks++;
    if (!condition) {
        failures++;
        printf("  FAIL %s\n", what);
    } else {
        printf("  ok   %s\n", what);
    }
}

#define VERTICES 5u

static float attributes[VERTICES * GPU_VSH_ATTRIBUTE_FLOATS];
static float constants[GPU_VSH_CONSTANT_ROWS * 4u];
static float capture[VERTICES * GPU_VSH_CAPTURE_FLOATS];

static uint32_t float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static void test_capture_returns_every_vertex(gpu_device *device)
{
    printf("test_capture_returns_every_vertex (%s)\n", gpu_device_name(device));
    memset(attributes, 0, sizeof attributes);
    memset(constants, 0, sizeof constants);
    for (uint32_t vertex = 0u; vertex < VERTICES; vertex++) {
        float *v1 = attributes + vertex * GPU_VSH_ATTRIBUTE_FLOATS + 4u;
        for (uint32_t lane = 0u; lane < 4u; lane++) {
            v1[lane] = (float)(vertex * 10u + lane + 1u);
        }
    }
    for (uint32_t lane = 0u; lane < 4u; lane++) {
        constants[5u * 4u + lane] = 100.0f + (float)lane;
        constants[7u * 4u + lane] = 1000.0f + (float)lane;
    }
    const gpu_vsh_draw draw = {
        .words = capture_vertex_words,
        .word_count = sizeof capture_vertex_words / sizeof(uint32_t),
        .vertex_count = VERTICES,
        .attributes = attributes,
        .constants = constants,
    };
    check(gpu_vsh_capture(device, &draw, capture) == GPU_OK, "gpu_vsh_capture succeeded");
    int exact = 1;
    int distinct = 1;
    int sentinel = 1;
    for (uint32_t vertex = 0u; vertex < VERTICES; vertex++) {
        const float *out = capture + vertex * GPU_VSH_CAPTURE_FLOATS;
        const float *v1 = attributes + vertex * GPU_VSH_ATTRIBUTE_FLOATS + 4u;
        for (uint32_t lane = 0u; lane < 4u; lane++) {
            exact &= out[lane] == v1[lane] * 2.0f + (100.0f + (float)lane); /* oPos at slot 0 */
            exact &= out[12u + lane] == (1000.0f + (float)lane) + v1[3u - lane]; /* oD0 at slot 3 */
            /* slots 1 and 2 are never written by the shader: they keep the sentinel */
            sentinel &= float_bits(out[4u + lane]) == GPU_VSH_CAPTURE_SENTINEL_BITS;
            sentinel &= float_bits(out[8u + lane]) == GPU_VSH_CAPTURE_SENTINEL_BITS;
        }
        if (vertex > 0u) {
            distinct &= memcmp(out, out - GPU_VSH_CAPTURE_FLOATS, 16u) != 0;
        }
    }
    check(exact, "all 5 vertices of one draw have oPos = 2 v1 + c5 and oD0 = c7 + v1.wzyx exactly");
    check(distinct, "consecutive vertices differ (no vertex was copied or skipped)");
    check(sentinel, "uncaptured slots keep the sentinel");
}

static void test_render_draws_two_triangles_in_one_call(gpu_device *device)
{
    printf("test_render_draws_two_triangles_in_one_call (%s)\n", gpu_device_name(device));
    static const float corners[6][2] = {
        { -0.9f, -0.9f }, { -0.1f, -0.9f }, { -0.5f, -0.1f }, /* top left, red */
        { 0.1f, 0.1f }, { 0.9f, 0.1f }, { 0.5f, 0.9f },       /* bottom right, green */
    };
    float vertices[6u * GPU_VSH_ATTRIBUTE_FLOATS];
    memset(vertices, 0, sizeof vertices);
    memset(constants, 0, sizeof constants);
    for (uint32_t vertex = 0u; vertex < 6u; vertex++) {
        float *v1 = vertices + vertex * GPU_VSH_ATTRIBUTE_FLOATS + 4u;
        float *v2 = vertices + vertex * GPU_VSH_ATTRIBUTE_FLOATS + 8u;
        v1[0] = corners[vertex][0];
        v1[1] = corners[vertex][1];
        v1[2] = 0.5f;
        v1[3] = 1.0f;
        /* oD0 = v2.zyxw + c3: red is v2 = (0, 0, 1, 1), green is (0, 1, 0, 1) */
        v2[2] = vertex < 3u ? 1.0f : 0.0f;
        v2[1] = vertex < 3u ? 0.0f : 1.0f;
        v2[3] = 1.0f;
    }
    const gpu_vsh_draw draw = {
        .words = draw_vertex_words,
        .word_count = sizeof draw_vertex_words / sizeof(uint32_t),
        .vertex_count = 6u,
        .attributes = vertices,
        .constants = constants,
    };
    static const float clear[4] = { 0.2f, 0.2f, 0.2f, 1.0f };
    gpu_image image = { 0 };
    check(gpu_vsh_render(device, 64u, 64u, clear, &draw, &image) == GPU_OK, "gpu_vsh_render succeeded");
    if (!image.pixels) {
        failures++;
        return;
    }
    const uint8_t *red = image.pixels + gpu_image_offset(&image, 16u, 16u);
    const uint8_t *green = image.pixels + gpu_image_offset(&image, 48u, 48u);
    const uint8_t *background = image.pixels + gpu_image_offset(&image, 48u, 16u);
    check(red[0] == 255u && red[1] == 0u && red[2] == 0u && red[3] == 255u,
          "the first triangle is red, upper left (clip y = -1 is the top row)");
    check(green[0] == 0u && green[1] == 255u && green[2] == 0u && green[3] == 255u,
          "the second triangle of the same draw is green, lower right");
    check(background[0] == 51u && background[1] == 51u && background[2] == 51u,
          "the pixel outside both triangles is the clear colour");
    /* Between the two triangles: a triangle STRIP over the same six vertices would fill it. */
    const uint8_t *between = image.pixels + gpu_image_offset(&image, 27u, 26u);
    check(between[0] == 51u && between[1] == 51u && between[2] == 51u,
          "the gap between the triangles stays clear, so the topology is a list");
    uint32_t covered = 0u;
    for (uint32_t i = 0u; i < 64u * 64u; i++) {
        covered += image.pixels[i * 4u] != 51u || image.pixels[i * 4u + 1u] != 51u;
    }
    check(covered > 500u && covered < 1500u, "the covered area is two triangles, not a blank or a full image");
    gpu_image_free(&image);
}

static void test_arguments_are_rejected(gpu_device *device)
{
    printf("test_arguments_are_rejected (%s)\n", gpu_device_name(device));
    gpu_vsh_draw draw = {
        .words = capture_vertex_words,
        .word_count = sizeof capture_vertex_words / sizeof(uint32_t),
        .vertex_count = VERTICES,
        .attributes = attributes,
        .constants = constants,
    };
    check(gpu_vsh_capture(device, &draw, NULL) == GPU_ERR_ARGUMENT, "a NULL capture buffer is rejected");
    check(gpu_vsh_capture(NULL, &draw, capture) == GPU_ERR_ARGUMENT, "a NULL device is rejected");
    draw.vertex_count = 0u;
    check(gpu_vsh_capture(device, &draw, capture) == GPU_ERR_ARGUMENT, "zero vertices are rejected");
    draw.vertex_count = GPU_VSH_MAX_VERTICES + 1u;
    check(gpu_vsh_capture(device, &draw, capture) == GPU_ERR_ARGUMENT, "too many vertices are rejected");
    draw.vertex_count = VERTICES;
    uint32_t bad[8] = { 0x12345678u };
    draw.words = bad;
    draw.word_count = 8u;
    check(gpu_vsh_capture(device, &draw, capture) == GPU_ERR_ARGUMENT, "a module without the SPIR-V magic is rejected");
    draw.words = capture_vertex_words;
    draw.word_count = sizeof capture_vertex_words / sizeof(uint32_t);
    static const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    gpu_image image = { 0 };
    check(gpu_vsh_render(device, 0u, 64u, clear, &draw, &image) == GPU_ERR_ARGUMENT, "a zero width is rejected");
    check(image.pixels == NULL, "no pixels were allocated for a rejected render");
}

/* T259: edges of gpu_vsh_render the mutation sweep (tools/mutate/sets/gpu_vsh.py) found unpinned:
 * a non-square target, a clear colour with four different channels, the size limits, NULL blocks
 * and the fragment-stage argument checks. */
static void test_render_edges(gpu_device *device)
{
    printf("test_render_edges (%s)\n", gpu_device_name(device));
    static float flat_vertices[GPU_VSH_ATTRIBUTE_FLOATS * 3u];
    static float corner_vertices[GPU_VSH_ATTRIBUTE_FLOATS * 3u];
    memset(flat_vertices, 0, sizeof flat_vertices);
    memset(corner_vertices, 0, sizeof corner_vertices);
    memset(constants, 0, sizeof constants);
    static const float lower[3][2] = { { -0.5f, 0.2f }, { 0.5f, 0.2f }, { 0.0f, 0.9f } };
    for (uint32_t vertex = 0u; vertex < 3u; vertex++) {
        float *flat = flat_vertices + vertex * GPU_VSH_ATTRIBUTE_FLOATS;
        flat[4u + 2u] = 0.5f; /* v1 = (0, 0, 0.5, 1): three identical points, no area */
        flat[4u + 3u] = 1.0f;
        float *corner = corner_vertices + vertex * GPU_VSH_ATTRIBUTE_FLOATS;
        corner[4u + 0u] = lower[vertex][0];
        corner[4u + 1u] = lower[vertex][1];
        corner[4u + 2u] = 0.5f;
        corner[4u + 3u] = 1.0f;
        corner[8u + 1u] = 1.0f; /* v2 = (0, 1, 0, 1): oD0 = v2.zyxw is green */
        corner[8u + 3u] = 1.0f;
    }
    gpu_vsh_draw draw = {
        .words = draw_vertex_words,
        .word_count = sizeof draw_vertex_words / sizeof(uint32_t),
        .vertex_count = 3u,
        .attributes = corner_vertices,
        .constants = constants,
    };
    static const float grey[4] = { 0.2f, 0.2f, 0.2f, 1.0f };
    gpu_image image = { 0 };

    /* a 64 x 32 target: the viewport is the TARGET's size, so a triangle in the lower half of
     * clip space lands in rows 19 to 30. A square viewport would push it below the image */
    check(gpu_vsh_render(device, 64u, 32u, grey, &draw, &image) == GPU_OK, "a non-square render succeeds");
    if (image.pixels) {
        const uint8_t *inside = image.pixels + gpu_image_offset(&image, 32u, 24u);
        check(inside[0] == 0u && inside[1] == 255u && inside[2] == 0u,
              "a triangle in the lower half of a 64 x 32 target is drawn there");
        const uint8_t *above = image.pixels + gpu_image_offset(&image, 32u, 4u);
        check(above[0] == 51u && above[1] == 51u && above[2] == 51u, "and the rows above it stay clear");
    }
    gpu_image_free(&image);

    /* four different clear channels come back as four different bytes (nothing is drawn) */
    draw.attributes = flat_vertices;
    static const float colourful[4] = { 0.2f, 0.4f, 0.6f, 0.8f };
    check(gpu_vsh_render(device, 4u, 4u, colourful, &draw, &image) == GPU_OK, "a render with no coverage succeeds");
    if (image.pixels) {
        const uint8_t *pixel = image.pixels + gpu_image_offset(&image, 2u, 2u);
        check(pixel[0] == 51u && pixel[1] == 102u && pixel[2] == 153u && pixel[3] == 204u,
              "the clear colour keeps all four channels (51, 102, 153, 204)");
    }
    gpu_image_free(&image);

    /* the target size limit is 8192 in each direction, inclusive */
    check(gpu_vsh_render(device, 8193u, 1u, grey, &draw, &image) == GPU_ERR_ARGUMENT, "a width of 8193 is rejected");
    check(gpu_vsh_render(device, 1u, 8193u, grey, &draw, &image) == GPU_ERR_ARGUMENT, "a height of 8193 is rejected");
    check(gpu_vsh_render(device, 8192u, 1u, grey, &draw, &image) == GPU_OK, "a width of 8192 is accepted");
    gpu_image_free(&image);

    /* exactly GPU_VSH_MAX_VERTICES vertices are accepted */
    float *many = calloc((size_t)GPU_VSH_MAX_VERTICES * GPU_VSH_ATTRIBUTE_FLOATS, sizeof *many);
    check(many != NULL, "the maximum-size vertex block was allocated");
    if (many) {
        draw.attributes = many;
        draw.vertex_count = GPU_VSH_MAX_VERTICES;
        check(gpu_vsh_render(device, 4u, 4u, grey, &draw, &image) == GPU_OK,
              "a draw of exactly GPU_VSH_MAX_VERTICES vertices is accepted");
        gpu_image_free(&image);
        draw.vertex_count = GPU_VSH_MAX_VERTICES + 1u;
        check(gpu_vsh_render(device, 4u, 4u, grey, &draw, &image) == GPU_ERR_ARGUMENT,
              "one vertex more is rejected");
        free(many);
    }
    draw.vertex_count = 3u;

    /* a NULL attribute or constants block is rejected, never copied from */
    draw.attributes = NULL;
    check(gpu_vsh_render(device, 4u, 4u, grey, &draw, &image) == GPU_ERR_ARGUMENT, "NULL attributes are rejected");
    draw.attributes = flat_vertices;
    draw.constants = NULL;
    check(gpu_vsh_render(device, 4u, 4u, grey, &draw, &image) == GPU_ERR_ARGUMENT, "NULL constants are rejected");
    draw.constants = constants;

    /* a fragment stage must be a whole module with constants, and its textures must have an area */
    static float fragment_constants[GPU_VSH_FRAGMENT_VEC4S * 4u];
    static const uint8_t texel[4] = { 1u, 2u, 3u, 255u };
    gpu_vsh_fragment fragment = {
        .words = draw_vertex_words,
        .word_count = 4u,
        .constants = fragment_constants,
    };
    draw.fragment = &fragment;
    check(gpu_vsh_render(device, 4u, 4u, grey, &draw, &image) == GPU_ERR_ARGUMENT,
          "a fragment module shorter than a SPIR-V header is rejected");
    fragment.word_count = sizeof draw_vertex_words / sizeof(uint32_t);
    fragment.textures[0].rgba = texel;
    fragment.textures[0].width = 1u;
    fragment.textures[0].height = 0u;
    check(gpu_vsh_render(device, 4u, 4u, grey, &draw, &image) == GPU_ERR_ARGUMENT,
          "a test texture with no height is rejected");
}

static int run_device(const char *selector)
{
    gpu_device *device = NULL;
    const gpu_result created = gpu_device_create_selected(selector, &device);
    if (created != GPU_OK) {
        printf("SKIP %s: %s\n", selector, gpu_result_string(created));
        return 0;
    }
    if (!gpu_device_has_transform_feedback(device)) {
        printf("SKIP %s (%s): no VK_EXT_transform_feedback\n", selector, gpu_device_name(device));
        gpu_device_destroy(device);
        return 0;
    }
    test_capture_returns_every_vertex(device);
    test_render_draws_two_triangles_in_one_call(device);
    test_arguments_are_rejected(device);
    test_render_edges(device);
    gpu_device_destroy(device);
    return 1;
}

int main(void)
{
    if (!gpu_vulkan_available()) {
        printf("SKIP: no Vulkan loader on this machine\n");
        return 77;
    }
    gpu_device *any = NULL;
    const gpu_result probe = gpu_device_create_selected(NULL, &any);
    if (probe != GPU_OK) {
        printf("SKIP: no Vulkan device (%s)\n", gpu_result_string(probe));
        return 77;
    }
    gpu_device_destroy(any);
    gpu_device *none = NULL;
    check(gpu_device_create_selected("no-such-device-name", &none) == GPU_ERR_NO_PHYSICAL_DEVICE &&
              none == NULL,
          "a selector that matches nothing is GPU_ERR_NO_PHYSICAL_DEVICE and returns NULL");
    int ran = 0;
    ran += run_device("hardware");
    ran += run_device("software");
    printf("%d checks, %d failures, %d device(s) exercised\n", checks, failures, ran);
    if (failures) {
        return 1;
    }
    return ran > 0 ? 0 : 77;
}
