/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1490: the texture stage programs 3 (cube map), 9 (DOT_ST) and 17 (DOTPRODUCT) through the live Vulkan renderer on a real (software)
 * device. Each case is a synthetic draw of one triangle covering a 64 x 64 target, a vertex program writing oT0..oT3 from affine
 * attributes (tests/fixtures/t1490_modes/make_modules.py, the live translator) and the combiner module the live host would make
 * (tools.nv2a_combiner.replay_modules, from the same combiner words), with real swizzled guest textures. The pixels are compared with a
 * CPU reference of xemu's generated GLSL (hw/xbox/nv2a/pgraph/glsl/psh.c, commit 478b4f49) computed here: bilinear sampling with
 * clamp to edge (the measured filter 0x02062000), a pixel may match the evaluation at a few sample positions a tenth of a pixel apart (the GPU's
 * fixed point filter weights and a boundary of the second lookup), and a cube lookup within half a texel of a face edge is skipped (the
 * hardware blends the neighbouring face there, which the reference does not model).
 * Usage: test_t1490_texture_modes DIR [--png OUT_DIR], DIR from make_modules.py. Skips with 77 without a device.
 */
#define VK_NO_PROTOTYPES

#include "gpu_png.h"
#include "live_texture.h"
#include "live_vk_bind.h"
#include "live_vk_draw.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;
static const char *png_directory;

#define CHECK(condition)                                                                      \
    do {                                                                                      \
        checks++;                                                                             \
        if (!(condition)) {                                                                   \
            failures++;                                                                       \
            printf("  FAIL line %d: %s\n", __LINE__, #condition);                             \
        }                                                                                     \
    } while (0)

#include "live_vk_scenes.h"

#include "live_vk_rig.h"

#define SURFACE_ALL_GROUPS (ALL_GROUPS | GPU_PGRAPH_OUTPUT_POLYGON_OFFSET | GPU_PGRAPH_OUTPUT_SURFACE)
#define TARGET_DATA 0x0200000u
#define TARGET_FORMAT 0x00011229u /* linear A8R8G8B8 */
#define TARGET_SIZE (63u | (63u << 12) | (3u << 24)) /* 64 x 64, pitch 256 */
#define TARGET_SIDE 64u
#define LINEAR_FILTER 0x02062000u /* the measured filter word of every title texture: bilinear */
#define CLAMP_BOTH 0x00000303u
#define CLAMP_PLUS_WRAP_P 0x00010303u /* the cube stage's P field (xemu reads ADDRP, a cube map ignores it) */
#define FORMAT_2D(side_exponent) (0x00010621u | ((side_exponent) << 20) | ((side_exponent) << 24)) /* swizzled A8R8G8B8, 1 level, 2D */
#define FORMAT_CUBE(side_exponent) (FORMAT_2D(side_exponent) | 4u)
#define VERTEX_BYTES 92u

/* The guest memory map (memory[] of live_vk_scenes.h is 0x800 bytes at MEMORY_BASE). */
#define OFFSET_VERTICES 0x000u
#define OFFSET_NORMALS 0x140u  /* 2 x 2 */
#define OFFSET_CONTROL 0x180u  /* 4 x 4 */
#define OFFSET_LOOKUP 0x200u   /* 4 x 4 */
#define OFFSET_CUBE 0x300u     /* six 4 x 4 faces 128 bytes apart (NV2A_CUBEMAP_FACE_ALIGNMENT) */

typedef struct {
    bool bound;
    uint32_t format, offset, address;
} stage_binding;

typedef struct {
    const char *name;
    uint32_t words[GPU_PGRAPH_COMBINER_WORDS];
    uint32_t program, mapping, other;
    stage_binding bindings[4];
    bool constant_normals;   /* the normal map is one colour (200, 190, 230): the coordinates of the lookup are then exact, see build_cases */
    double tolerance;        /* per channel, 0..1 */
    double jitter_pixels;    /* the largest sample position offset a pixel may match at */
    float base[4][4], gradient_x[4][4], gradient_y[4][4]; /* oTn = base + gradient_x * P + gradient_y * Q, P and Q in 0..1 over the target */
} mode_case;

static stage_binding current_bindings[4];
static char vertex_name[128];
static char fragment_names_storage[16][100];
static const char *fragment_name_pointers[16];
static uint32_t *fragment_modules[16];
static size_t fragment_module_words[16];
static uint32_t *vertex_module;
static size_t vertex_module_words;
static const char *vertex_name_pointer;
static struct gpu_vsh_table vertex_table;
static struct gpu_vsh_table fragment_names_table;
static size_t fragment_count;
static char fragment_case[16][32];

static bool binding_for(void *context, size_t draw, uint32_t stage, live_texture_binding *out, const char **refusal)
{
    (void)context;
    (void)draw;
    if (stage >= 4u || !current_bindings[stage].bound) {
        *refusal = "stage not bound by the case";
        return false;
    }
    *out = (live_texture_binding){.header = 0u, .format = current_bindings[stage].format, .size_word = 0u,
                                  .data = MEMORY_BASE + current_bindings[stage].offset};
    return true;
}

static bool load_vertex_module(void *context, uint32_t module, const uint32_t **words, size_t *word_count)
{
    (void)context;
    if (module != 0u) return false;
    *words = vertex_module;
    *word_count = vertex_module_words;
    return true;
}

static bool load_combiner_module(void *context, uint32_t module, const uint32_t **words, size_t *word_count)
{
    (void)context;
    if (module >= fragment_count) return false;
    *words = fragment_modules[module];
    *word_count = fragment_module_words[module];
    return true;
}

static uint32_t *read_words(const char *directory, const char *stem, size_t *count)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.spv", directory, stem);
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    fseek(file, 0, SEEK_END);
    const long bytes = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint32_t *words = bytes > 0 && bytes % 4 == 0 ? malloc((size_t)bytes) : NULL;
    if (words != NULL && fread(words, 1u, (size_t)bytes, file) != (size_t)bytes) {
        free(words);
        words = NULL;
    }
    fclose(file);
    *count = words != NULL ? (size_t)bytes / 4u : 0u;
    return words;
}

static bool read_manifest(const char *directory)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/manifest.txt", directory);
    FILE *file = fopen(path, "r");
    if (file == NULL) return false;
    char label[64], name[128];
    while (fscanf(file, "%63s %127s", label, name) == 2) {
        if (strcmp(label, "vertex") == 0) {
            snprintf(vertex_name, sizeof vertex_name, "%s", name);
            vertex_module = read_words(directory, "vertex", &vertex_module_words);
        } else if (fragment_count < 16u) {
            snprintf(fragment_case[fragment_count], sizeof fragment_case[0], "%s", label);
            snprintf(fragment_names_storage[fragment_count], sizeof fragment_names_storage[0], "%s", name);
            fragment_name_pointers[fragment_count] = fragment_names_storage[fragment_count];
            fragment_modules[fragment_count] = read_words(directory, name, &fragment_module_words[fragment_count]);
            if (fragment_modules[fragment_count] == NULL) return false;
            fragment_count++;
        }
    }
    fclose(file);
    vertex_name_pointer = vertex_name;
    vertex_table = (struct gpu_vsh_table){0u, 0u, NULL, 0u, NULL, 1u, &vertex_name_pointer};
    fragment_names_table = (struct gpu_vsh_table){0u, 0u, NULL, 0u, NULL, (uint32_t)fragment_count, fragment_name_pointers};
    return vertex_module != NULL && fragment_count != 0u;
}

/* --- the CPU reference: xemu psh.c ----------------------------------------------------------------------------------------- */

typedef struct {
    double r, g, b;
} colour3;

static double sign1(double c) { double x = c * 255.0; return (x - 128.0) / 127.0; }
static double sign2(double c)
{
    double x = c * 255.0;
    return x >= 128.0 ? (x - 255.5) / 127.5 : (x + 0.5) / 127.5;
}
static double sign3(double c)
{
    double x = c * 255.0;
    return x >= 128.0 ? (x - 256.0) / 127.0 : x / 127.0;
}

/* dotmap_zero_to_one, dotmap_minus1_to_1_d3d, dotmap_minus1_to_1_gl, dotmap_minus1_to_1 */
static colour3 dot_map(uint32_t mapper, colour3 c)
{
    switch (mapper) {
    case 1u: return (colour3){sign1(c.r), sign1(c.g), sign1(c.b)};
    case 2u: return (colour3){sign2(c.r), sign2(c.g), sign2(c.b)};
    case 3u: return (colour3){sign3(c.r), sign3(c.g), sign3(c.b)};
    default: return c;
    }
}

static colour3 texel_at(uint32_t offset, uint32_t side, int x, int y)
{
    x = x < 0 ? 0 : x >= (int)side ? (int)side - 1 : x;
    y = y < 0 ? 0 : y >= (int)side ? (int)side - 1 : y;
    const uint8_t *texel = memory + offset + (size_t)live_texture_swizzle_offset((uint32_t)x, (uint32_t)y, side, side) * 4u;
    return (colour3){texel[2] / 255.0, texel[1] / 255.0, texel[0] / 255.0};
}

/* bilinear sample of a swizzled A8R8G8B8 2D texture at `offset`, side x side, normalised coordinate, clamp to edge (the sampler of xemu's
 * GL_LINEAR with GL_CLAMP_TO_EDGE) */
static colour3 sample_2d(uint32_t offset, uint32_t side, double u, double v)
{
    const double x = u * side - 0.5, y = v * side - 0.5;
    const int x0 = (int)floor(x), y0 = (int)floor(y);
    const double fx = x - x0, fy = y - y0;
    const colour3 a = texel_at(offset, side, x0, y0), b = texel_at(offset, side, x0 + 1, y0);
    const colour3 c = texel_at(offset, side, x0, y0 + 1), d = texel_at(offset, side, x0 + 1, y0 + 1);
    return (colour3){(a.r * (1 - fx) + b.r * fx) * (1 - fy) + (c.r * (1 - fx) + d.r * fx) * fy,
                     (a.g * (1 - fx) + b.g * fx) * (1 - fy) + (c.g * (1 - fx) + d.g * fx) * fy,
                     (a.b * (1 - fx) + b.b * fx) * (1 - fy) + (c.b * (1 - fx) + d.b * fx) * fy};
}

#define CUBE_SIDE 4u

/* GL / Vulkan cube map face selection (the spec table), faces in the order +X -X +Y -Y +Z -Z (xemu gl/texture.c upload order), 4 x 4 faces
 * 128 bytes apart. False for a lookup within half a texel of a face edge. */
static bool sample_cube(double x, double y, double z, colour3 *out)
{
    const double ax = fabs(x), ay = fabs(y), az = fabs(z);
    uint32_t face;
    double sc, tc, ma;
    if (ax >= ay && ax >= az) {
        face = x >= 0.0 ? 0u : 1u;
        sc = x >= 0.0 ? -z : z;
        tc = -y;
        ma = ax;
    } else if (ay >= az) {
        face = y >= 0.0 ? 2u : 3u;
        sc = x;
        tc = y >= 0.0 ? z : -z;
        ma = ay;
    } else {
        face = z >= 0.0 ? 4u : 5u;
        sc = z >= 0.0 ? x : -x;
        tc = -y;
        ma = az;
    }
    const double s = (sc / ma + 1.0) / 2.0, t = (tc / ma + 1.0) / 2.0;
    const double edge = 0.5 / CUBE_SIDE;
    if (s < edge || s > 1.0 - edge || t < edge || t > 1.0 - edge) return false;
    *out = sample_2d(OFFSET_CUBE + face * 128u, CUBE_SIDE, s, t);
    return true;
}

static void coordinate(const mode_case *c, uint32_t stage, double p, double q, double out[4])
{
    for (uint32_t i = 0u; i < 4u; i++) {
        out[i] = (double)c->base[stage][i] + (double)c->gradient_x[stage][i] * p + (double)c->gradient_y[stage][i] * q;
    }
}

/* The final colour of one pixel for case `name`; (p, q) is the sample position in 0..1 over the target. */
static bool reference_colour(const mode_case *c, const char *name, double p, double q, colour3 *out)
{
    double t[4][4];
    for (uint32_t stage = 0u; stage < 4u; stage++) coordinate(c, stage, p, q, t[stage]);
    if (strncmp(name, "control2d", 9) == 0) {
        *out = sample_2d(OFFSET_CONTROL, 4u, t[0][0] / t[0][3], t[0][1] / t[0][3]); /* textureProj of pT0.xyw */
        return true;
    }
    if (strncmp(name, "cube", 4) == 0) {
        return sample_cube(t[1][0], t[1][1], t[1][2], out); /* texture(texSamp1, pT1.xyz) */
    }
    /* texm3x2: stage 1 (or 0) is the normal map, stage 2 PS_TEXTUREMODES_DOTPRODUCT, stage 3 PS_TEXTUREMODES_DOT_ST */
    const uint32_t source = c->other == 0u ? 0u : 1u;
    const colour3 normal_texel = sample_2d(c->bindings[source].offset, 2u, t[source][0] / t[source][3], t[source][1] / t[source][3]);
    const colour3 n2 = dot_map((c->mapping >> 4) & 0xFu, normal_texel); /* ps->dot_map[2] */
    const colour3 n3 = dot_map((c->mapping >> 8) & 0xFu, normal_texel); /* ps->dot_map[3] */
    const double dot2 = t[2][0] * n2.r + t[2][1] * n2.g + t[2][2] * n2.b;
    const double dot3 = t[3][0] * n3.r + t[3][1] * n3.g + t[3][2] * n3.b;
    *out = sample_2d(c->bindings[3].offset, 4u, dot2, dot3);
    return true;
}

static bool near_colour(const uint8_t *bgra, colour3 expected, double tolerance)
{
    const double got[3] = {bgra[2] / 255.0, bgra[1] / 255.0, bgra[0] / 255.0};
    return fabs(got[0] - expected.r) <= tolerance && fabs(got[1] - expected.g) <= tolerance && fabs(got[2] - expected.b) <= tolerance;
}

/* --- the scene ------------------------------------------------------------------------------------------------------------- */

static const mode_case *find_case(const mode_case *cases, size_t count, const char *name)
{
    for (size_t i = 0u; i < count; i++) {
        if (strcmp(cases[i].name, name) == 0) return &cases[i];
    }
    return NULL;
}

static void put_texel(uint32_t offset, uint32_t side, uint32_t x, uint32_t y, uint8_t red, uint8_t green, uint8_t blue)
{
    uint8_t *texel = memory + offset + (size_t)live_texture_swizzle_offset(x, y, side, side) * 4u;
    texel[0] = blue;
    texel[1] = green;
    texel[2] = red;
    texel[3] = 255u;
}

static void fill_textures(void)
{
    memset(memory, 0, sizeof memory);
    /* distinct texels everywhere, so a mixed up face, orientation or Morton order shows */
    for (uint32_t y = 0u; y < 4u; y++) {
        for (uint32_t x = 0u; x < 4u; x++) {
            put_texel(OFFSET_CONTROL, 4u, x, y, (uint8_t)(30 + 55 * x), (uint8_t)(20 + 60 * y), (uint8_t)(200 - 25 * (x + 4 * y)));
            /* high contrast, so a small error in a dot product moves the colour a lot */
            put_texel(OFFSET_LOOKUP, 4u, x, y, (uint8_t)(x * 73 + y * 151 + 17), (uint8_t)(x * 199 + y * 61 + 90), (uint8_t)(x * 37 + y * 113 + 200));
        }
    }
    static const uint8_t normals[4][3] = {{200u, 128u, 128u}, {60u, 190u, 150u}, {128u, 50u, 220u}, {230u, 230u, 100u}};
    for (uint32_t y = 0u; y < 2u; y++) {
        for (uint32_t x = 0u; x < 2u; x++) {
            put_texel(OFFSET_NORMALS, 2u, x, y, normals[x + 2u * y][0], normals[x + 2u * y][1], normals[x + 2u * y][2]);
        }
    }
    for (uint32_t face = 0u; face < 6u; face++) {
        for (uint32_t y = 0u; y < CUBE_SIDE; y++) {
            for (uint32_t x = 0u; x < CUBE_SIDE; x++) {
                put_texel(OFFSET_CUBE + face * 128u, CUBE_SIDE, x, y, (uint8_t)(35 * face + 20 + 18 * x), (uint8_t)(200 - 25 * face + 12 * y),
                          (uint8_t)(15 + 14 * x + 40 * y));
            }
        }
    }
}

static void write_vertices(const mode_case *c)
{
    static const float corner[3][2] = {{-8.0f, -8.0f}, {136.0f, -8.0f}, {-8.0f, 136.0f}};
    for (uint32_t vertex = 0u; vertex < 3u; vertex++) {
        float data[VERTEX_BYTES / 4u];
        memset(data, 0, sizeof data);
        data[0] = corner[vertex][0];
        data[1] = corner[vertex][1];
        data[2] = 0.5f;
        data[3] = data[4] = data[5] = data[6] = 1.0f; /* v2: oD0 alpha 1 */
        const double p = (corner[vertex][0]) / 64.0, q = (corner[vertex][1]) / 64.0;
        for (uint32_t stage = 0u; stage < 4u; stage++) {
            double value[4];
            coordinate(c, stage, p, q, value);
            for (uint32_t i = 0u; i < 4u; i++) data[7u + stage * 4u + i] = (float)value[i];
        }
        memcpy(memory + OFFSET_VERTICES + vertex * VERTEX_BYTES, data, sizeof data);
    }
}

static gpu_pgraph *case_model(const mode_case *c)
{
    gpu_pgraph *model = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(model, SURFACE_ALL_GROUPS | GPU_PGRAPH_OUTPUT_TEXTURE);
    gpu_pgraph_set_combiner(model, true);
    stream_builder stream = {0};
    static const float offset[4] = {0.0f, 0.0f, 0.0f, 0.0f}, scale[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    stream_viewport(&stream, offset, scale);
    stream_pair(&stream, 0x0208u, 0x128u);
    stream_pair(&stream, 0x020Cu, 256u);
    stream_pair(&stream, 0x0210u, TARGET_DATA);
    stream_pair(&stream, GPU_PGRAPH_EXECUTION_MODE, 6u);
    /* the program of make_modules.py vertex_program: MOV oPos,v1; MOV oD0,v2; MOV oT0..oT3,v9..v12 */
    uint32_t rows[6u * 4u];
    for (uint32_t row = 0u; row < 6u; row++) {
        const uint32_t source = row == 0u ? 1u : row == 1u ? 2u : 9u + (row - 2u);
        const uint32_t address = row == 0u ? 0u : row == 1u ? 3u : 9u + (row - 2u);
        rows[row * 4u] = 0u;
        rows[row * 4u + 1u] = 0x00200000u | (source << 9) | 0x1Bu;
        rows[row * 4u + 2u] = 0x0836186Cu;
        rows[row * 4u + 3u] = 0xF800u | (address << 3) | (row == 5u ? 1u : 0u);
    }
    stream_program(&stream, 0u, rows, 6u);
    stream_pair(&stream, GPU_PGRAPH_PROGRAM_START, 0u);
    stream_pixel_shader(&stream, c->words);
    stream_pair(&stream, 0x1E70u, c->program);
    stream_pair(&stream, 0x1E74u, c->mapping);
    stream_pair(&stream, 0x1E78u, c->other);
    for (uint32_t stage = 0u; stage < 4u; stage++) {
        if (c->bindings[stage].bound) {
            stream_pair(&stream, 0x1B08u + 0x40u * stage, c->bindings[stage].address);
            stream_pair(&stream, 0x1B14u + 0x40u * stage, LINEAR_FILTER);
        }
    }
    stream_array(&stream, 1u, MEMORY_BASE + OFFSET_VERTICES, array_format(VERTEX_BYTES, 3u, GPU_PGRAPH_TYPE_F));
    stream_array(&stream, 2u, MEMORY_BASE + OFFSET_VERTICES + 12u, array_format(VERTEX_BYTES, 4u, GPU_PGRAPH_TYPE_F));
    for (uint32_t stage = 0u; stage < 4u; stage++) {
        stream_array(&stream, 9u + stage, MEMORY_BASE + OFFSET_VERTICES + 28u + 16u * stage, array_format(VERTEX_BYTES, 4u, GPU_PGRAPH_TYPE_F));
    }
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(gpu_pgraph_decode(model, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    stream_free(&stream);
    return model;
}

typedef struct {
    rig *r;
    gpu_device *device;
    live_texture_cache cache;
    live_vk_target_set *targets;
    live_vk_bind *bind;
    fake_guest guest;
} harness;

/* One frame of one draw. Returns drawn (1) or 0 with the refusal text. */
static bool draw_case(harness *h, const gpu_pgraph *model, bool modes_enabled, char *refusal, size_t refusal_bytes)
{
    gpu_pgraph_backend backend = {0};
    backend.table = &vertex_table;
    backend.read_guest = fake_guest_read;
    backend.load_module = load_vertex_module;
    backend.context = &h->guest;
    backend.allowed_inferences = EVERYTHING;
    backend.output_groups = SURFACE_ALL_GROUPS | GPU_PGRAPH_OUTPUT_TEXTURE;
    backend.line_width = 1.0f;
    backend.combiner = true;
    backend.fragment_table = &fragment_names_table;
    backend.load_fragment_module = load_combiner_module;
    backend.live_raster_modules = true;
    backend.live_texture_modes = modes_enabled;
    live_vk_bind_backend(h->bind, &backend);
    char error[256] = "";
    live_vk_renderer *renderer = live_vk_renderer_create(&h->r->device, h->r->colour_pass, &backend, error, sizeof error);
    CHECK(renderer != NULL);
    if (renderer == NULL) return false;
    bool drawn = false;
    live_vk_draw *draw = live_vk_draw_create(&h->r->device, renderer, h->targets, error, sizeof error);
    CHECK(draw != NULL);
    if (draw != NULL) {
        const live_vk_target_hook target_hook = live_vk_draw_hook(draw);
        live_vk_renderer_set_target_hook(renderer, &target_hook);
        const live_vk_texture_hook texture_hook = live_vk_bind_hook(h->bind);
        live_vk_renderer_set_texture_hook(renderer, &texture_hook);
        live_vk_renderer_begin_frame(renderer);
        live_vk_draw_begin_frame(draw);
        live_vk_bind_begin_frame(h->bind);
        refusal[0] = '\0';
        drawn = live_vk_renderer_draw(renderer, model, 0u, VK_NULL_HANDLE, 0u, 0u, refusal, refusal_bytes);
        CHECK(live_vk_draw_flush(draw));
        live_vk_bind_end_frame(h->bind);
        live_vk_draw_destroy(draw);
    }
    live_vk_renderer_destroy(renderer);
    return drawn;
}

static void clear_target(harness *h)
{
    static uint8_t zero[256u * 64u];
    CHECK(live_vk_target_upload(h->targets, TARGET_DATA, zero, sizeof zero) == LIVE_VK_OK);
}

static void write_png(const char *name, const uint8_t *bgra)
{
    if (png_directory == NULL) return;
    uint8_t rgba[TARGET_SIDE * TARGET_SIDE * 4u];
    for (size_t pixel = 0u; pixel < TARGET_SIDE * TARGET_SIDE; pixel++) {
        rgba[pixel * 4u] = bgra[pixel * 4u + 2u];
        rgba[pixel * 4u + 1u] = bgra[pixel * 4u + 1u];
        rgba[pixel * 4u + 2u] = bgra[pixel * 4u];
        rgba[pixel * 4u + 3u] = 255u;
    }
    char path[512];
    snprintf(path, sizeof path, "%s/%s.png", png_directory, name);
    (void)gpu_png_write_rgba(path, rgba, TARGET_SIDE, TARGET_SIDE, TARGET_SIDE * 4u);
}

static size_t distinct_colours(const uint8_t *bgra)
{
    uint32_t seen[4096];
    size_t count = 0u;
    for (size_t pixel = 0u; pixel < TARGET_SIDE * TARGET_SIDE; pixel++) {
        const uint32_t key = (uint32_t)bgra[pixel * 4u] | ((uint32_t)bgra[pixel * 4u + 1u] << 8) | ((uint32_t)bgra[pixel * 4u + 2u] << 16);
        bool known = false;
        for (size_t i = 0u; i < count; i++) known = known || seen[i] == key;
        if (!known && count < 4096u) seen[count++] = key;
    }
    return count;
}

/* Compare the readback with the reference over every pixel: a pixel passes when it equals the reference at the centre or at any of the
 * four jittered sample positions (a texel boundary may fall either way). Returns the mismatching pixel count. */
static size_t compare(const mode_case *c, const char *name, const uint8_t *bgra, size_t *compared)
{
    size_t bad = 0u;
    *compared = 0u;
    /* pixels: the GPU's filter weights are fixed point, so a lookup fed by a discontinuity (the sign2 and sign3 maps jump by 1 at a texel byte of 128)
     * moves its jump by a fraction of a pixel */
    static const double jitter[9][2] = {{0, 0}, {0.5, 0}, {-0.5, 0}, {0, 0.5}, {0, -0.5}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}}; /* times jitter_pixels */
    for (uint32_t y = 0u; y < TARGET_SIDE; y++) {
        for (uint32_t x = 0u; x < TARGET_SIDE; x++) {
            const uint8_t *pixel = bgra + ((size_t)y * TARGET_SIDE + x) * 4u;
            colour3 centre;
            if (!reference_colour(c, name, (x + 0.5) / 64.0, (y + 0.5) / 64.0, &centre)) continue; /* a cube seam */
            (*compared)++;
            bool match = false;
            for (uint32_t j = 0u; j < 9u && !match; j++) {
                colour3 expected;
                if (reference_colour(c, name, (x + 0.5 + jitter[j][0] * c->jitter_pixels) / 64.0, (y + 0.5 + jitter[j][1] * c->jitter_pixels) / 64.0, &expected)) {
                    match = near_colour(pixel, expected, c->tolerance);
                }
            }
            if (!match) printf("  mismatch at (%u,%u) got %u,%u,%u expected %.1f,%.1f,%.1f\n", x, y, pixel[2], pixel[1], pixel[0], centre.r * 255, centre.g * 255, centre.b * 255);
            bad += !match;
        }
    }
    return bad;
}

static void fill_case(mode_case *c, const char *name, uint32_t stage_read, uint32_t program, uint32_t mapping, uint32_t other)
{
    memset(c, 0, sizeof *c);
    c->name = name;
    c->words[0] = 0xD4300000u;
    c->words[45] = 0xC0u;
    c->words[26] = 0xC0u;
    c->words[53] = 0x11101u;
    c->words[8] = 0x0Cu;
    c->words[9] = 0x1C80u;
    c->words[34] = ((8u + stage_read) << 24) | 0x00200000u;
    c->words[54] = program;
    c->words[55] = mapping;
    c->words[56] = other;
    c->program = program;
    c->mapping = mapping;
    c->other = other;
    /* the GPU's bilinear weights are fixed point, a normal map blended per pixel and then used as a coordinate of a high contrast texture moves a few
     * levels, so the varying cases use 8 levels, the exact cases below 2.5 */
    c->tolerance = 8.0 / 255.0;
    c->jitter_pixels = 0.3;
}

static void set_gradient(mode_case *c, uint32_t stage, const float base[4], const float gx[4], const float gy[4])
{
    memcpy(c->base[stage], base, sizeof c->base[stage]);
    memcpy(c->gradient_x[stage], gx, sizeof c->gradient_x[stage]);
    memcpy(c->gradient_y[stage], gy, sizeof c->gradient_y[stage]);
}

static void build_cases(mode_case *cases, size_t *count)
{
    size_t n = 0u;
    /* control: stage 0 mode 1, the supported path, untouched by T1490 */
    fill_case(&cases[n], "control2d", 0u, 1u, 0u, 0u);
    cases[n].bindings[0] = (stage_binding){true, FORMAT_2D(2u), OFFSET_CONTROL, CLAMP_BOTH};
    set_gradient(&cases[n], 0u, (float[4]){0.0f, 0.0f, 0.0f, 1.0f}, (float[4]){1.0f, 0.0f, 0.0f, 0.0f}, (float[4]){0.0f, 1.0f, 0.0f, 0.0f});
    n++;
    /* the cube map in stage 1: a direction that sweeps +X -X +Y -Y and +Z (z = 0.3) or -Z (z = -0.3) */
    for (uint32_t sign = 0u; sign < 2u; sign++) {
        fill_case(&cases[n], sign == 0u ? "cube" : "cube_back", 1u, 3u << 5, 0u, 0u);
        cases[n].bindings[1] = (stage_binding){true, FORMAT_CUBE(2u), OFFSET_CUBE, CLAMP_PLUS_WRAP_P};
        set_gradient(&cases[n], 1u, (float[4]){-1.6f, -1.6f, sign == 0u ? 0.3f : -0.3f, 1.0f}, (float[4]){3.2f, 0.0f, 0.0f, 0.0f},
                     (float[4]){0.0f, 3.2f, 0.0f, 0.0f});
        n++;
    }
    /* the title's texm3x2 pair: program 0x4C421 = (1, 1, 17, 9), the normal map in stage 1, the lookup in stage 3 */
    static const struct {
        const char *name;
        uint32_t program, mapping, other;
    } dots[] = {{"dot_pair", 0x4C421u, 0x110u, 0x00110000u}, {"dot_zero_to_one", 0x4C421u, 0x000u, 0x00110000u},
                {"dot_sign2", 0x4C421u, 0x220u, 0x00110000u}, {"dot_sign3", 0x4C421u, 0x330u, 0x00110000u},
                {"dot_from_stage0", 0x4C401u, 0x110u, 0x00000000u}};
    for (size_t i = 0u; i < sizeof dots / sizeof dots[0]; i++) {
        fill_case(&cases[n], dots[i].name, 3u, dots[i].program, dots[i].mapping, dots[i].other);
        /* T1491: the title leaves other textures bound to stages that sample none (a 3D one here, no decoder): no lookup, no refusal counted */
        cases[n].bindings[2] = (stage_binding){true, 0x00011831u, OFFSET_CUBE, CLAMP_BOTH};
        const uint32_t source = dots[i].other == 0u ? 0u : 1u;
        cases[n].bindings[source] = (stage_binding){true, FORMAT_2D(1u), OFFSET_NORMALS, CLAMP_BOTH};
        cases[n].bindings[3] = (stage_binding){true, FORMAT_2D(2u), OFFSET_LOOKUP, CLAMP_BOTH};
        set_gradient(&cases[n], source, (float[4]){0.0f, 0.0f, 0.0f, 1.0f}, (float[4]){1.0f, 0.0f, 0.0f, 0.0f}, (float[4]){0.0f, 1.0f, 0.0f, 0.0f});
        set_gradient(&cases[n], 2u, (float[4]){0.9f, 0.1f, 0.4f, 1.0f}, (float[4]){0.0f, 0.5f, 0.0f, 0.0f}, (float[4]){-0.3f, 0.0f, 0.4f, 0.0f});
        set_gradient(&cases[n], 3u, (float[4]){0.2f, 0.9f, 0.3f, 1.0f}, (float[4]){0.5f, 0.0f, -0.2f, 0.0f}, (float[4]){0.0f, -0.4f, 0.3f, 0.0f});
        n++;
    }
    /* exact coordinates: a constant normal map (200, 190, 230) so n is the mapped colour everywhere, oT2 = (P / n2.r, 0, 0) and oT3 = (0, Q / n3.g, 0)
     * make the lookup coordinate (dot2, dot3) exactly (P, Q). A wrong mapping (127 against 128, 255.0 against 255.5 in sign2) then shifts the whole
     * picture by a fraction of a texel of a high contrast texture, which the tolerance of the varying cases would hide. */
    static const struct {
        const char *name;
        uint32_t mapping;
    } exact[] = {{"exact_sign1", 0x110u}, {"exact_zero_to_one", 0x000u}, {"exact_sign2", 0x220u}, {"exact_sign3", 0x330u}};
    for (size_t i = 0u; i < sizeof exact / sizeof exact[0]; i++) {
        fill_case(&cases[n], exact[i].name, 3u, 0x4C421u, exact[i].mapping, 0x00110000u);
        cases[n].constant_normals = true;
        cases[n].tolerance = 2.5 / 255.0;
        cases[n].jitter_pixels = 0.1;
        cases[n].bindings[1] = (stage_binding){true, FORMAT_2D(1u), OFFSET_NORMALS, CLAMP_BOTH};
        cases[n].bindings[3] = (stage_binding){true, FORMAT_2D(2u), OFFSET_LOOKUP, CLAMP_BOTH};
        const colour3 normal = {200.0 / 255.0, 190.0 / 255.0, 230.0 / 255.0};
        const colour3 n2 = dot_map((exact[i].mapping >> 4) & 0xFu, normal), n3 = dot_map((exact[i].mapping >> 8) & 0xFu, normal);
        set_gradient(&cases[n], 1u, (float[4]){0.0f, 0.0f, 0.0f, 1.0f}, (float[4]){0.0f, 0.0f, 0.0f, 0.0f}, (float[4]){0.0f, 0.0f, 0.0f, 0.0f});
        set_gradient(&cases[n], 2u, (float[4]){0.0f, 0.0f, 0.0f, 1.0f}, (float[4]){(float)(1.0 / n2.r), 0.0f, 0.0f, 0.0f}, (float[4]){0.0f, 0.0f, 0.0f, 0.0f});
        set_gradient(&cases[n], 3u, (float[4]){0.0f, 0.0f, 0.0f, 1.0f}, (float[4]){0.0f, 0.0f, 0.0f, 0.0f}, (float[4]){0.0f, (float)(1.0 / n3.g), 0.0f, 0.0f});
        n++;
    }
    *count = n;
}

static void run_cases(harness *h)
{
    mode_case cases[24];
    size_t count = 0u;
    build_cases(cases, &count);
    uint8_t first_dot_image[256u * 64u];
    bool have_first_dot = false;
    for (size_t index = 0u; index < count; index++) {
        mode_case *c = &cases[index];
        printf("case %s\n", c->name);
        memcpy(current_bindings, c->bindings, sizeof current_bindings);
        fill_textures();
        (void)live_texture_note_write(&h->cache, MEMORY_BASE, sizeof memory); /* this harness has no resolver, the cache learns of guest writes by notes */
        if (c->constant_normals) {
            for (uint32_t y = 0u; y < 2u; y++) for (uint32_t x = 0u; x < 2u; x++) put_texel(OFFSET_NORMALS, 2u, x, y, 200u, 190u, 230u);
            (void)live_texture_note_write(&h->cache, MEMORY_BASE, sizeof memory);
        }
        write_vertices(c);
        gpu_pgraph *model = case_model(c);
        /* the module of the case: the manifest name for the base case name (cube_back shares the cube module) */
        char refusal[LIVE_PIPELINE_REASON_BYTES];
        /* modes off: the 2D control still draws, every other case is refused by name (INFERRED, opt-in) */
        clear_target(h);
        const bool drawn_off = draw_case(h, model, false, refusal, sizeof refusal);
        if (strcmp(c->name, "control2d") == 0) {
            CHECK(drawn_off);
        } else {
            CHECK(!drawn_off);
            CHECK(strstr(refusal, "INFERRED") != NULL && strstr(refusal, "not allowed") != NULL);
        }
        clear_target(h);
        const uint64_t refusals_before = h->cache.refusals;
        const bool drawn = draw_case(h, model, true, refusal, sizeof refusal);
        CHECK(h->cache.refusals == refusals_before); /* no texture lookup of any stage was refused (T1491: stages 0 and 17 are not looked up) */
        if (!drawn) printf("  refused: %s\n", refusal);
        CHECK(drawn);
        uint8_t readback[256u * 64u];
        CHECK(live_vk_target_readback(h->targets, TARGET_DATA, readback, sizeof readback) == LIVE_VK_OK);
        size_t compared = 0u;
        const size_t bad = compare(c, c->name, readback, &compared);
        const size_t colours = distinct_colours(readback);
        printf("  compared %zu of 4096 pixels, mismatching %zu, distinct colours %zu\n", compared, bad, colours);
        CHECK(compared >= 2800u);
        CHECK(bad == 0u);
        CHECK(colours >= 16u); /* not a vacuous match: the picture varies */
        write_png(c->name, readback);
        if (strcmp(c->name, "dot_pair") == 0) {
            memcpy(first_dot_image, readback, sizeof first_dot_image);
            have_first_dot = true;
        } else if (strncmp(c->name, "dot_", 4) == 0 && have_first_dot && strcmp(c->name, "dot_from_stage0") != 0) {
            /* a different input mapping must give a different picture (the mapper is not ignored) */
            size_t different = 0u;
            for (size_t i = 0u; i < sizeof readback; i += 4u) different += memcmp(readback + i, first_dot_image + i, 3u) != 0;
            printf("  differs from dot_pair in %zu pixels\n", different);
            CHECK(different > 100u);
        }
        gpu_pgraph_destroy(model);
    }
}

/* Refusals by name: the stage program says cube and the bound texture is 2D (and the reverse). */
static void run_mismatches(harness *h)
{
    mode_case cases[24];
    size_t count = 0u;
    build_cases(cases, &count);
    char refusal[LIVE_PIPELINE_REASON_BYTES];
    mode_case *cube = (mode_case *)find_case(cases, count, "cube");
    CHECK(cube != NULL);
    if (cube == NULL) return;
    printf("case cube program with a 2D texture bound\n");
    cube->bindings[1].format = FORMAT_2D(2u);
    cube->bindings[1].address = CLAMP_BOTH; /* a 2D texture takes no P field */
    memcpy(current_bindings, cube->bindings, sizeof current_bindings);
    fill_textures();
    (void)live_texture_note_write(&h->cache, MEMORY_BASE, sizeof memory);
    write_vertices(cube);
    gpu_pgraph *model = case_model(cube);
    clear_target(h);
    CHECK(!draw_case(h, model, true, refusal, sizeof refusal));
    printf("  refusal: %s\n", refusal);
    CHECK(strstr(refusal, "needs a cube map but the bound texture is 2D") != NULL);
    gpu_pgraph_destroy(model);
    printf("case 2D program with a cube bound\n");
    mode_case *control = (mode_case *)find_case(cases, count, "control2d");
    control->bindings[0].format = FORMAT_CUBE(2u);
    memcpy(current_bindings, control->bindings, sizeof current_bindings);
    write_vertices(control);
    model = case_model(control);
    clear_target(h);
    CHECK(!draw_case(h, model, true, refusal, sizeof refusal));
    CHECK(strstr(refusal, "needs a 2D texture but the bound texture is a cube map") != NULL);
    gpu_pgraph_destroy(model);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: %s DIR [--png OUT_DIR]\n", argv[0]);
        return 2;
    }
    for (int index = 2; index + 1 < argc; index++) {
        if (strcmp(argv[index], "--png") == 0) png_directory = argv[index + 1];
    }
    if (!read_manifest(argv[1])) {
        printf("FAIL: no manifest or module in %s\n", argv[1]);
        return 2;
    }
    if (!gpu_vulkan_available()) {
        printf("SKIP: no Vulkan loader\n");
        return 77;
    }
    gpu_device *device = NULL;
    if (gpu_device_create_selected("software", &device) != GPU_OK) {
        printf("SKIP: no software Vulkan device\n");
        return 77;
    }
    rig r;
    CHECK(rig_init(&r, device));
    harness h = {&r, device, {0}, NULL, NULL, {MEMORY_BASE, memory, sizeof memory}};
    live_texture_cache_init(&h.cache, true);
    live_vk_target_device description = {0};
    description.device = r.native.device;
    description.queue = r.native.queue;
    description.queue_family = r.native.queue_family;
    description.command_pool = r.native.command_pool;
    description.memory = r.native.memory_properties;
    description.get_device_proc_addr = r.native.get_device_proc_addr;
    h.targets = live_vk_target_create(&description, 0u, &h.cache);
    CHECK(h.targets != NULL);
    gpu_window_native native = {0};
    native.instance = r.native.instance;
    native.physical_device = r.native.physical_device;
    native.device = r.native.device;
    native.queue = r.native.queue;
    native.queue_family = r.native.queue_family;
    native.command_pool = r.native.command_pool;
    native.get_device_proc_addr = r.native.get_device_proc_addr;
    native.memory_properties = r.native.memory_properties;
    const char *texture_error = NULL;
    live_vk_texture_set *texture_set = live_vk_texture_create(&native, r.native.get_instance_proc_addr, &texture_error);
    CHECK(texture_set != NULL);
    if (h.targets == NULL || texture_set == NULL) return 1;
    CHECK(live_vk_target_register(h.targets, TARGET_DATA, TARGET_FORMAT, TARGET_SIZE, NULL) == LIVE_VK_OK);
    const live_vk_bind_source source = {binding_for, NULL, fake_guest_read, &h.guest};
    h.bind = live_vk_bind_create(&h.cache, texture_set, &source);
    CHECK(h.bind != NULL);
    live_vk_bind_use_targets(h.bind, h.targets);
    run_cases(&h);
    run_mismatches(&h);
    live_vk_bind_destroy(h.bind);
    live_vk_texture_destroy(texture_set);
    live_vk_target_destroy(h.targets);
    live_texture_cache_free(&h.cache);
    r.fn.vkDestroyRenderPass(r.native.device, r.colour_pass, NULL);
    gpu_device_destroy(device);
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
