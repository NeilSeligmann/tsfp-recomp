/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gpu_pgraph_replay (T84): a recorded stream, written the way the measured emitters write it, is
 * decoded and replayed through gpu_vsh_draw into an RGBA frame on every Vulkan device that exists
 * ("hardware" and "software"). A device that is absent prints a stated SKIP, and when nothing at
 * all ran the test exits 77 (a ctest SKIP, not a pass).
 *
 * The module is the hand-written vertex shader of test_gpu_vsh_draw.c (v1 -> gl_Position, oD0 =
 * v2.zyxw + c[3], std140 c[192] at set 0 binding 0), found through a one-entry selector table
 * under the SHA-256 name of a synthetic one-instruction program, so the program-digest route, the
 * constants route, the attribute route and the compositing are all on the path. The REAL
 * translated programs through the generated table are tests/test_gpu_pgraph.py.
 *
 * The stream (64 x 64, clear 0.2 grey, the order matters):
 *   draw 1  upper left triangle, v2 = (0, 0, 1, 1)                -> red
 *   draw 2  after c[3] = (0, 1, 0, 0), lower right triangle       -> yellow
 *   draw 3  small triangle inside draw 1, D3DCOLOR 0x00000000     -> (0, 0, 0, 0) over the red
 *   draw 4  upper right triangle, D3DCOLOR 0xFF0000FF (blue)      -> red, because oD0 = v2.zyxw
 *           and the replay reads a D3DCOLOR as (R, G, B, A): a BGRA reading would give blue.
 *
 * T84b adds POINTS, LINES and LINE_STRIP: test_expansion (no device) pins the index expansion and the
 * inference refusals of the assembly, test_points_lines draws them with gpu_pgraph_vertex_words.h's
 * sibling gpu_pgraph_point_words.h (the same module plus gl_PointSize = c[4].x) and checks the
 * pixels, and test_refusals keeps LINE_LOOP, QUAD_STRIP and POLYGON refused.
 */
#include "gpu_device.h"
#include "gpu_pgraph.h"
#include "gpu_pgraph_replay.h"
#include "gpu_pgraph_test_support.h"
#include "gpu_png.h"
#include "gpu_pgraph_point_words.h"
#include "gpu_pgraph_vertex_words.h"

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

typedef struct {
    float position[3];
    float v2[4];
} float_vertex; /* 28 bytes */

typedef struct {
    float position[3];
    uint8_t colour[4];
} colour_vertex; /* 16 bytes, colour is B, G, R, A in memory */

static const char *const module_names[] = {SYNTHETIC_PROGRAM_NAME};
static const struct gpu_vsh_table table = {0u, 0u, NULL, 0u, NULL, 1u, module_names};
static const char *const other_names[] = {"static_00"};
static const struct gpu_vsh_table other_table = {0u, 0u, NULL, 0u, NULL, 1u, other_names};

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

static bool load_point_module(void *context, uint32_t module, const uint32_t **words,
                              size_t *word_count)
{
    (void)context;
    if (module != 0u) {
        return false;
    }
    *words = point_vertex_words;
    *word_count = sizeof point_vertex_words / sizeof(uint32_t);
    return true;
}

#define MEMORY_BASE 0x00600000u
#define REGION_B 0x400u

static uint8_t memory[0x800];

static void fill_memory(void)
{
    memset(memory, 0, sizeof memory);
    static const float corners_a[6][2] = {
        {-0.9f, -0.9f}, {-0.1f, -0.9f}, {-0.5f, -0.1f}, /* draw 1 */
        {0.1f, 0.1f},   {0.9f, 0.1f},   {0.5f, 0.9f},   /* draw 2 */
    };
    for (uint32_t i = 0u; i < 6u; i++) {
        float_vertex vertex = {{corners_a[i][0], corners_a[i][1], 0.5f}, {0.0f, 0.0f, 1.0f, 1.0f}};
        memcpy(memory + i * sizeof vertex, &vertex, sizeof vertex);
    }
    static const float corners_b[6][2] = {
        {-0.6f, -0.8f}, {-0.4f, -0.8f}, {-0.5f, -0.4f}, /* draw 3 */
        {0.1f, -0.9f},  {0.9f, -0.9f},  {0.5f, -0.1f},  /* draw 4 */
    };
    for (uint32_t i = 0u; i < 6u; i++) {
        colour_vertex vertex = {{corners_b[i][0], corners_b[i][1], 0.5f}, {0u, 0u, 0u, 0u}};
        if (i >= 3u) {
            vertex.colour[0] = 0xFFu; /* B */
            vertex.colour[3] = 0xFFu; /* A */
        }
        memcpy(memory + REGION_B + i * sizeof vertex, &vertex, sizeof vertex);
    }
}

static void build_stream(stream_builder *stream)
{
    static const float offset[4] = {32.0f, 32.0f, 0.0f, 0.0f};
    static const float scale[4] = {32.0f, -32.0f, 1.0f, 0.0f};
    stream_viewport(stream, offset, scale);
    stream_pair(stream, GPU_PGRAPH_EXECUTION_MODE, 6u);
    stream_program(stream, 0u, synthetic_program, 1u);
    stream_pair(stream, GPU_PGRAPH_PROGRAM_START, 0u);
    static const float c3_zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    static const float c3_green[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    stream_constants(stream, 3u, c3_zero, 4u);
    stream_array(stream, 1u, MEMORY_BASE, array_format(28u, 3u, GPU_PGRAPH_TYPE_F));
    stream_array(stream, 2u, MEMORY_BASE + 12u, array_format(28u, 4u, GPU_PGRAPH_TYPE_F));
    stream_draw_arrays(stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    stream_constants(stream, 3u, c3_green, 4u);
    stream_draw_arrays(stream, GPU_PGRAPH_OP_TRIANGLES, 3u, 3u);
    stream_constants(stream, 3u, c3_zero, 4u);
    stream_array(stream, 1u, MEMORY_BASE + REGION_B, array_format(16u, 3u, GPU_PGRAPH_TYPE_F));
    stream_array(stream, 2u, MEMORY_BASE + REGION_B + 12u, array_format(16u, 4u, GPU_PGRAPH_TYPE_UB_D3D));
    stream_draw_arrays(stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    stream_draw_arrays(stream, GPU_PGRAPH_OP_TRIANGLES, 3u, 3u);
}

static bool pixel_is(const gpu_image *image, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b,
                     uint8_t a)
{
    const uint8_t *p = image->pixels + gpu_image_offset(image, x, y);
    return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}

static uint32_t count_not(const gpu_image *image, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    uint32_t count = 0u;
    for (uint32_t y = 0u; y < image->height; y++) {
        for (uint32_t x = 0u; x < image->width; x++) {
            count += !pixel_is(image, x, y, r, g, b, a);
        }
    }
    return count;
}

static gpu_pgraph_backend make_backend(fake_guest *guest)
{
    gpu_pgraph_backend backend = {0};
    backend.table = &table;
    backend.read_guest = fake_guest_read;
    backend.load_module = load_module;
    backend.context = guest;
    backend.allowed_inferences = GPU_PGRAPH_INFER_ALL;
    return backend;
}

static void test_frame(gpu_device *device)
{
    printf("test_frame (%s)\n", gpu_device_name(device));
    fill_memory();
    fake_guest guest = {MEMORY_BASE, memory, sizeof memory};
    gpu_pgraph_backend backend = make_backend(&guest);
    gpu_pgraph *pgraph = gpu_pgraph_create();
    stream_builder stream = {0};
    build_stream(&stream);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_count(pgraph) == 4u);
    CHECK(gpu_pgraph_snapshot_count(pgraph) == 3u);

    static const float clear[4] = {0.2f, 0.2f, 0.2f, 1.0f};
    gpu_image frame = {0};
    gpu_pgraph_report report;
    CHECK(gpu_pgraph_replay(pgraph, device, &backend, WIDTH, HEIGHT, clear, &frame, &report) ==
          GPU_PGRAPH_OK);
    CHECK(frame.pixels != NULL && frame.width == WIDTH && frame.height == HEIGHT);
    CHECK(report.draws == 4u && report.drawn == 4u && report.degenerate == 0u);
    CHECK(report.vertices == 12u);
    /* the four triangle inferences only: no point, line or S32K one (S32K is covered by
     * test_gpu_pgraph_vtxtypes and the real-program test) */
    CHECK(report.used_inferences == (GPU_PGRAPH_INFER_ALL & ~(GPU_PGRAPH_INFER_POINT_SIZE |
                                                              GPU_PGRAPH_INFER_LINE_WIDTH |
                                                              GPU_PGRAPH_INFER_S32K_UNNORMALISED)));
    const char *png_path = getenv("TSFP_PGRAPH_PNG");
    if (png_path != NULL && frame.pixels != NULL) {
        (void)gpu_png_write_rgba(png_path, frame.pixels, frame.width, frame.height, frame.stride_bytes);
    }
    if (frame.pixels != NULL) {
        CHECK(pixel_is(&frame, 12u, 15u, 255u, 0u, 0u, 255u));  /* draw 1, red */
        CHECK(pixel_is(&frame, 48u, 48u, 255u, 255u, 0u, 255u)); /* draw 2, c[3] reached the device */
        CHECK(pixel_is(&frame, 16u, 11u, 0u, 0u, 0u, 0u));      /* draw 3 replaced draw 1's pixel */
        CHECK(pixel_is(&frame, 48u, 16u, 255u, 0u, 0u, 255u));  /* draw 4, D3DCOLOR order */
        CHECK(pixel_is(&frame, 16u, 48u, 51u, 51u, 51u, 255u)); /* uncovered: the clear colour */
        CHECK(pixel_is(&frame, 63u, 0u, 51u, 51u, 51u, 255u));
        const uint32_t covered = count_not(&frame, 51u, 51u, 51u, 255u);
        CHECK(covered > 1000u && covered < 2200u); /* four triangles, not blank and not full */
        uint32_t transparent = 0u;
        for (uint32_t y = 0u; y < HEIGHT; y++) {
            for (uint32_t x = 0u; x < WIDTH; x++) {
                transparent += pixel_is(&frame, x, y, 0u, 0u, 0u, 0u);
            }
        }
        CHECK(transparent > 20u && transparent < 120u); /* only draw 3's triangle */
    }

    /* flip_y is an exact row reversal of the same frame */
    gpu_pgraph_backend flipped_backend = backend;
    flipped_backend.flip_y = true;
    gpu_image flipped = {0};
    CHECK(gpu_pgraph_replay(pgraph, device, &flipped_backend, WIDTH, HEIGHT, clear, &flipped, &report) ==
          GPU_PGRAPH_OK);
    CHECK(flipped.pixels != NULL && frame.pixels != NULL);
    if (flipped.pixels != NULL && frame.pixels != NULL) {
        int reversed = 1;
        for (uint32_t y = 0u; y < HEIGHT; y++) {
            const uint8_t *a = frame.pixels + (size_t)y * frame.stride_bytes;
            const uint8_t *b = flipped.pixels + (size_t)(HEIGHT - 1u - y) * flipped.stride_bytes;
            reversed &= memcmp(a, b, WIDTH * 4u) == 0;
        }
        CHECK(reversed);
        CHECK(!pixel_is(&flipped, 12u, 15u, 255u, 0u, 0u, 255u)); /* draw 1 moved to the bottom */
        CHECK(pixel_is(&flipped, 12u, 48u, 255u, 0u, 0u, 255u));
    }
    gpu_image_free(&frame);
    gpu_image_free(&flipped);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
}

/* T441. The title's real stream never writes the transform execution mode (0x1E94). Without the inference the
 * replay refuses at the program, with GPU_PGRAPH_INFER_EXECUTION_MODE_UNWRITTEN it draws exactly the frame the
 * same stream draws with the mode written as 6, and says it depended on the inference. A written mode does not
 * claim the inference. */
static void test_unwritten_mode(gpu_device *device)
{
    printf("test_unwritten_mode (%s)\n", gpu_device_name(device));
    fill_memory();
    fake_guest guest = {MEMORY_BASE, memory, sizeof memory};
    stream_builder full = {0};
    build_stream(&full);
    stream_builder without = {0};
    for (size_t i = 0u; i < full.count; i++) {
        if (full.pairs[i].method != GPU_PGRAPH_EXECUTION_MODE) {
            stream_pair(&without, full.pairs[i].method, full.pairs[i].data);
        }
    }
    CHECK(without.count + 1u == full.count);
    static const float clear[4] = {0.2f, 0.2f, 0.2f, 1.0f};

    gpu_pgraph *written = gpu_pgraph_create();
    CHECK(gpu_pgraph_decode(written, full.pairs, full.count) == GPU_PGRAPH_OK);
    gpu_pgraph_backend backend = make_backend(&guest);
    backend.allowed_inferences = GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_EXECUTION_MODE_UNWRITTEN;
    gpu_image reference = {0};
    gpu_pgraph_report report;
    CHECK(gpu_pgraph_replay(written, device, &backend, WIDTH, HEIGHT, clear, &reference, &report) ==
          GPU_PGRAPH_OK);
    CHECK(report.drawn == 4u && (report.used_inferences & GPU_PGRAPH_INFER_EXECUTION_MODE_UNWRITTEN) == 0u);

    gpu_pgraph *unwritten = gpu_pgraph_create();
    CHECK(gpu_pgraph_decode(unwritten, without.pairs, without.count) == GPU_PGRAPH_OK);
    CHECK(!gpu_pgraph_state_now(unwritten)->execution_mode_set);
    gpu_pgraph_backend refused = make_backend(&guest);
    gpu_image none = {0};
    CHECK(gpu_pgraph_replay(unwritten, device, &refused, WIDTH, HEIGHT, clear, &none, &report) ==
          GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "never set") != NULL);
    CHECK(none.pixels == NULL);

    gpu_image inferred = {0};
    CHECK(gpu_pgraph_replay(unwritten, device, &backend, WIDTH, HEIGHT, clear, &inferred, &report) ==
          GPU_PGRAPH_OK);
    CHECK(report.drawn == 4u);
    CHECK((report.used_inferences & GPU_PGRAPH_INFER_EXECUTION_MODE_UNWRITTEN) != 0u);
    CHECK(reference.pixels != NULL && inferred.pixels != NULL);
    if (reference.pixels != NULL && inferred.pixels != NULL) {
        uint32_t differing = 0u;
        for (uint32_t y = 0u; y < HEIGHT; y++) {
            differing += memcmp(reference.pixels + (size_t)y * reference.stride_bytes,
                                inferred.pixels + (size_t)y * inferred.stride_bytes, WIDTH * 4u) != 0;
        }
        CHECK(differing == 0u);
        CHECK(count_not(&inferred, 51u, 51u, 51u, 255u) > 1000u); /* something was drawn */
    }
    gpu_image_free(&reference);
    gpu_image_free(&inferred);
    gpu_pgraph_destroy(written);
    gpu_pgraph_destroy(unwritten);
    stream_free(&full);
    stream_free(&without);
}

static void test_refusals(gpu_device *device)
{
    printf("test_refusals (%s)\n", gpu_device_name(device));
    fill_memory();
    fake_guest guest = {MEMORY_BASE, memory, sizeof memory};
    gpu_pgraph_backend backend = make_backend(&guest);
    static const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    gpu_image frame = {0};
    gpu_pgraph_report report;

    /* a program the table does not hold */
    gpu_pgraph *pgraph = gpu_pgraph_create();
    stream_builder stream = {0};
    build_stream(&stream);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    gpu_pgraph_backend other = backend;
    other.table = &other_table;
    CHECK(gpu_pgraph_replay(pgraph, device, &other, WIDTH, HEIGHT, clear, &frame, &report) ==
          GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(frame.pixels == NULL && report.drawn == 0u && report.failed_draw == 0u);
    CHECK(strstr(report.error, "sha256 3ebe9b7e7c96baecc4c329dd008c862963e3d94b1a62ff2636e179b8ddc456aa") != NULL);

    /* each inference withheld alone refuses the whole replay, and nothing is returned */
    static const uint32_t bits[] = {GPU_PGRAPH_INFER_PROGRAM_HEADER, GPU_PGRAPH_INFER_VIEWPORT_CONSTANTS,
                                    GPU_PGRAPH_INFER_COMPONENT_DEFAULTS, GPU_PGRAPH_INFER_D3DCOLOR_ORDER};
    for (size_t i = 0u; i < sizeof bits / sizeof bits[0]; i++) {
        gpu_pgraph_backend narrow = backend;
        narrow.allowed_inferences = GPU_PGRAPH_INFER_ALL & ~bits[i];
        const gpu_pgraph_result result =
            gpu_pgraph_replay(pgraph, device, &narrow, WIDTH, HEIGHT, clear, &frame, &report);
        /* the D3DCOLOR draws are 3 and 4, so the first two draws are rendered before the refusal */
        CHECK(result == GPU_PGRAPH_ERR_UNMEASURED);
        CHECK(frame.pixels == NULL);
        CHECK(strstr(report.error, "INFERRED") != NULL);
        CHECK(report.failed_draw == (bits[i] == GPU_PGRAPH_INFER_D3DCOLOR_ORDER ? 2u : 0u));
    }

    /* a module that cannot be loaded */
    gpu_pgraph_backend broken = backend;
    broken.load_module = NULL;
    CHECK(gpu_pgraph_replay(pgraph, device, &broken, WIDTH, HEIGHT, clear, &frame, &report) ==
          GPU_PGRAPH_ERR_MALFORMED);
    CHECK(frame.pixels == NULL && strstr(report.error, "could not be loaded") != NULL);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);

    /* what the title never emits is named and refused, not drawn as something else */
    static const uint32_t refused[] = {GPU_PGRAPH_OP_LINE_LOOP, GPU_PGRAPH_OP_QUAD_STRIP,
                                       GPU_PGRAPH_OP_POLYGON};
    for (size_t i = 0u; i < sizeof refused / sizeof refused[0]; i++) {
        pgraph = gpu_pgraph_create();
        stream_builder one = {0};
        stream_pair(&one, GPU_PGRAPH_EXECUTION_MODE, 6u);
        stream_program(&one, 0u, synthetic_program, 1u);
        stream_pair(&one, GPU_PGRAPH_PROGRAM_START, 0u);
        stream_array(&one, 1u, MEMORY_BASE, array_format(28u, 3u, GPU_PGRAPH_TYPE_F));
        stream_draw_arrays(&one, refused[i], 0u, 6u);
        CHECK(gpu_pgraph_decode(pgraph, one.pairs, one.count) == GPU_PGRAPH_OK);
        CHECK(gpu_pgraph_replay(pgraph, device, &backend, WIDTH, HEIGHT, clear, &frame, &report) ==
              GPU_PGRAPH_ERR_UNMEASURED);
        CHECK(frame.pixels == NULL && strstr(report.error, "triangle, point and line lists only") != NULL);
        stream_free(&one);
        gpu_pgraph_destroy(pgraph);
    }

    /* bad arguments */
    pgraph = gpu_pgraph_create();
    CHECK(gpu_pgraph_replay(pgraph, device, &backend, 0u, HEIGHT, clear, &frame, &report) ==
          GPU_PGRAPH_ERR_ARGUMENT);
    CHECK(gpu_pgraph_replay(NULL, device, &backend, WIDTH, HEIGHT, clear, &frame, &report) ==
          GPU_PGRAPH_ERR_ARGUMENT);
    /* an empty draw list is a clear frame, not an error */
    CHECK(gpu_pgraph_replay(pgraph, device, &backend, WIDTH, HEIGHT, clear, &frame, &report) == GPU_PGRAPH_OK);
    CHECK(frame.pixels != NULL && report.drawn == 0u && count_not(&frame, 0u, 0u, 0u, 255u) == 0u);
    gpu_image_free(&frame);
    gpu_pgraph_destroy(pgraph);
}


/* ---------------------------------------------------------------------------------------- */
/* T84b: points and lines */

#define POINT_VERTICES 5u

/* Clip x or y of the centre of pixel p on the 64 pixel target (viewport 0, 0, 64, 64). */
static float centre_of(uint32_t pixel)
{
    return ((float)pixel + 0.5f) / 32.0f - 1.0f;
}

static void put_vertex(uint32_t index, uint32_t pixel_x, uint32_t pixel_y)
{
    float_vertex vertex = {{centre_of(pixel_x), centre_of(pixel_y), 0.5f}, {0.0f, 0.0f, 1.0f, 1.0f}};
    memcpy(memory + index * sizeof vertex, &vertex, sizeof vertex);
}

static void begin_primitive_stream(stream_builder *stream)
{
    static const float c3_zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    static const float c4_size_one[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    stream_pair(stream, GPU_PGRAPH_EXECUTION_MODE, 6u);
    stream_program(stream, 0u, synthetic_program, 1u);
    stream_pair(stream, GPU_PGRAPH_PROGRAM_START, 0u);
    stream_constants(stream, 3u, c3_zero, 4u);
    stream_constants(stream, 4u, c4_size_one, 4u);
    stream_array(stream, 1u, MEMORY_BASE, array_format(28u, 3u, GPU_PGRAPH_TYPE_F));
    stream_array(stream, 2u, MEMORY_BASE + 12u, array_format(28u, 4u, GPU_PGRAPH_TYPE_F));
}

/* One draw of `op` over the first `count` vertices of `memory`, replayed with the point module. */
static gpu_pgraph_result replay_primitive(gpu_device *device, gpu_pgraph_backend backend, uint32_t op,
                                          uint32_t count, gpu_image *frame, gpu_pgraph_report *report)
{
    static const float clear[4] = {0.2f, 0.2f, 0.2f, 1.0f};
    fake_guest guest = {MEMORY_BASE, memory, sizeof memory};
    backend.context = &guest;
    backend.load_module = load_point_module;
    gpu_pgraph *pgraph = gpu_pgraph_create();
    stream_builder stream = {0};
    begin_primitive_stream(&stream);
    stream_draw_arrays(&stream, op, 0u, count);
    gpu_pgraph_result result = gpu_pgraph_decode(pgraph, stream.pairs, stream.count);
    if (result == GPU_PGRAPH_OK) {
        result = gpu_pgraph_replay(pgraph, device, &backend, WIDTH, HEIGHT, clear, frame, report);
    }
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
    return result;
}

static bool is_red(const gpu_image *image, uint32_t x, uint32_t y)
{
    return pixel_is(image, x, y, 255u, 0u, 0u, 255u);
}

static void test_points(gpu_device *device)
{
    printf("test_points (%s)\n", gpu_device_name(device));
    memset(memory, 0, sizeof memory);
    static const uint32_t spots[POINT_VERTICES][2] = {{10, 20}, {40, 8}, {33, 50}, {0, 0}, {63, 63}};
    for (uint32_t i = 0u; i < POINT_VERTICES; i++) {
        put_vertex(i, spots[i][0], spots[i][1]);
    }
    gpu_pgraph_backend backend = make_backend(NULL);
    gpu_image frame = {0};
    gpu_pgraph_report report;
    CHECK(replay_primitive(device, backend, GPU_PGRAPH_OP_POINTS, POINT_VERTICES, &frame, &report) ==
          GPU_PGRAPH_OK);
    CHECK(frame.pixels != NULL);
    CHECK(report.drawn == 1u && report.vertices == POINT_VERTICES);
    CHECK((report.used_inferences & GPU_PGRAPH_INFER_POINT_SIZE) != 0u);
    CHECK((report.used_inferences & GPU_PGRAPH_INFER_LINE_WIDTH) == 0u);
    if (frame.pixels != NULL) {
        const uint32_t covered = count_not(&frame, 51u, 51u, 51u, 255u);
        CHECK(covered == POINT_VERTICES); /* one pixel per point, nothing else */
        for (uint32_t i = 0u; i < POINT_VERTICES; i++) {
            CHECK(is_red(&frame, spots[i][0], spots[i][1]));
        }
        CHECK(pixel_is(&frame, 11u, 20u, 51u, 51u, 51u, 255u));
    }
    gpu_image_free(&frame);

    /* a point list needs no whole primitive: the same vertices as one point each, degenerate at 0 */
    CHECK(replay_primitive(device, backend, GPU_PGRAPH_OP_POINTS, 1u, &frame, &report) == GPU_PGRAPH_OK);
    CHECK(frame.pixels != NULL && count_not(&frame, 51u, 51u, 51u, 255u) == 1u && is_red(&frame, 10u, 20u));
    gpu_image_free(&frame);
}

static void test_lines(gpu_device *device)
{
    printf("test_lines (%s)\n", gpu_device_name(device));
    memset(memory, 0, sizeof memory);
    put_vertex(0u, 4u, 10u); /* horizontal, row 10 */
    put_vertex(1u, 20u, 10u);
    put_vertex(2u, 40u, 5u); /* vertical, column 40 */
    put_vertex(3u, 40u, 30u);
    put_vertex(4u, 50u, 50u); /* the odd vertex, no partner */
    gpu_pgraph_backend backend = make_backend(NULL);
    gpu_image frame = {0};
    gpu_pgraph_report report;
    CHECK(replay_primitive(device, backend, GPU_PGRAPH_OP_LINES, 5u, &frame, &report) == GPU_PGRAPH_OK);
    CHECK(frame.pixels != NULL);
    CHECK(report.drawn == 1u && report.vertices == 4u); /* the odd vertex is dropped */
    CHECK((report.used_inferences & GPU_PGRAPH_INFER_LINE_WIDTH) != 0u);
    CHECK((report.used_inferences & GPU_PGRAPH_INFER_POINT_SIZE) == 0u);
    if (frame.pixels != NULL) {
        uint32_t on_lines = 0u;
        uint32_t off_lines = 0u;
        for (uint32_t y = 0u; y < HEIGHT; y++) {
            for (uint32_t x = 0u; x < WIDTH; x++) {
                if (!pixel_is(&frame, x, y, 51u, 51u, 51u, 255u)) {
                    const bool on = (y == 10u && x >= 4u && x <= 20u) || (x == 40u && y >= 5u && y <= 30u);
                    on_lines += on;
                    off_lines += !on;
                }
            }
        }
        CHECK(on_lines > 0u);
        CHECK(off_lines == 0u);                    /* a 1 pixel line along a pixel row or column */
        CHECK(on_lines >= 41u && on_lines <= 43u); /* 17 + 26 pixels, an end point may fall out */
        for (uint32_t x = 5u; x < 20u; x++) {
            CHECK(is_red(&frame, x, 10u));
        }
        for (uint32_t y = 6u; y < 30u; y++) {
            CHECK(is_red(&frame, 40u, y));
        }
        CHECK(pixel_is(&frame, 50u, 50u, 51u, 51u, 51u, 255u)); /* the dropped vertex drew nothing */
    }
    gpu_image_free(&frame);

    /* an explicit line_width of 1.0 is the same frame and needs no inference */
    gpu_image stated = {0};
    gpu_pgraph_backend explicit_width = backend;
    explicit_width.line_width = 1.0f;
    explicit_width.allowed_inferences = GPU_PGRAPH_INFER_ALL & ~GPU_PGRAPH_INFER_LINE_WIDTH;
    CHECK(replay_primitive(device, backend, GPU_PGRAPH_OP_LINES, 4u, &frame, &report) == GPU_PGRAPH_OK);
    CHECK(replay_primitive(device, explicit_width, GPU_PGRAPH_OP_LINES, 4u, &stated, &report) ==
          GPU_PGRAPH_OK);
    CHECK((report.used_inferences & GPU_PGRAPH_INFER_LINE_WIDTH) == 0u);
    CHECK(frame.pixels != NULL && stated.pixels != NULL);
    if (frame.pixels != NULL && stated.pixels != NULL) {
        CHECK(count_not(&frame, 51u, 51u, 51u, 255u) > 0u);
        CHECK(memcmp(frame.pixels, stated.pixels, (size_t)frame.stride_bytes * HEIGHT) == 0);
    }
    gpu_image_free(&frame);
    gpu_image_free(&stated);

    /* LINE_STRIP joins consecutive vertices: 4 vertices, 3 segments, 6 sent to the device */
    memset(memory, 0, sizeof memory);
    put_vertex(0u, 4u, 60u);
    put_vertex(1u, 20u, 60u);
    put_vertex(2u, 20u, 44u);
    put_vertex(3u, 40u, 44u);
    CHECK(replay_primitive(device, backend, GPU_PGRAPH_OP_LINE_STRIP, 4u, &frame, &report) ==
          GPU_PGRAPH_OK);
    CHECK(frame.pixels != NULL && report.vertices == 6u);
    if (frame.pixels != NULL) {
        CHECK(count_not(&frame, 51u, 51u, 51u, 255u) >= 48u);
        CHECK(is_red(&frame, 12u, 60u));  /* segment 0 */
        CHECK(is_red(&frame, 20u, 52u));  /* segment 1 */
        CHECK(is_red(&frame, 30u, 44u));  /* segment 2 */
        CHECK(pixel_is(&frame, 30u, 60u, 51u, 51u, 51u, 255u)); /* no segment closes the strip */
        CHECK(pixel_is(&frame, 12u, 44u, 51u, 51u, 51u, 255u));
    }
    gpu_image_free(&frame);
}

static void test_point_line_refusals(gpu_device *device)
{
    printf("test_point_line_refusals (%s)\n", gpu_device_name(device));
    memset(memory, 0, sizeof memory);
    put_vertex(0u, 4u, 10u);
    put_vertex(1u, 20u, 10u);
    gpu_image frame = {0};
    gpu_pgraph_report report;
    gpu_pgraph_backend backend = make_backend(NULL);

    gpu_pgraph_backend no_point_size = backend;
    no_point_size.allowed_inferences = GPU_PGRAPH_INFER_ALL & ~GPU_PGRAPH_INFER_POINT_SIZE;
    CHECK(replay_primitive(device, no_point_size, GPU_PGRAPH_OP_POINTS, 2u, &frame, &report) ==
          GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(frame.pixels == NULL && strstr(report.error, "oPts") != NULL && report.failed_draw == 0u);
    /* withholding the point inference does not refuse lines, nor the other way round */
    CHECK(replay_primitive(device, no_point_size, GPU_PGRAPH_OP_LINES, 2u, &frame, &report) == GPU_PGRAPH_OK);
    gpu_image_free(&frame);

    gpu_pgraph_backend no_line_width = backend;
    no_line_width.allowed_inferences = GPU_PGRAPH_INFER_ALL & ~GPU_PGRAPH_INFER_LINE_WIDTH;
    CHECK(replay_primitive(device, no_line_width, GPU_PGRAPH_OP_LINES, 2u, &frame, &report) ==
          GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(frame.pixels == NULL && strstr(report.error, "line_width") != NULL);
    CHECK(replay_primitive(device, no_line_width, GPU_PGRAPH_OP_LINE_STRIP, 2u, &frame, &report) ==
          GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(replay_primitive(device, no_line_width, GPU_PGRAPH_OP_POINTS, 2u, &frame, &report) == GPU_PGRAPH_OK);
    gpu_image_free(&frame);

    /* gpu_vsh_render itself refuses a line width but 1.0 and a topology it does not know */
    {
        static const float zero_attributes[GPU_VSH_ATTRIBUTE_FLOATS] = {0.0f};
        static const float zero_constants[GPU_VSH_CONSTANT_ROWS * 4u] = {0.0f};
        static const float clear_colour[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        gpu_vsh_draw draw = {
            .words = point_vertex_words,
            .word_count = sizeof point_vertex_words / sizeof(uint32_t),
            .vertex_count = 1u,
            .attributes = zero_attributes,
            .constants = zero_constants,
            .topology = GPU_VSH_TOPOLOGY_LINE_LIST,
            .line_width = 2.0f,
        };
        CHECK(gpu_vsh_render(device, 8u, 8u, clear_colour, &draw, &frame) == GPU_ERR_ARGUMENT);
        draw.line_width = 0.0f;
        CHECK(gpu_vsh_render(device, 8u, 8u, clear_colour, &draw, &frame) == GPU_ERR_ARGUMENT);
        draw.topology = GPU_VSH_TOPOLOGY_LINE_LIST + 1u;
        draw.line_width = 1.0f;
        CHECK(gpu_vsh_render(device, 8u, 8u, clear_colour, &draw, &frame) == GPU_ERR_ARGUMENT);
        draw.topology = GPU_VSH_TOPOLOGY_LINE_LIST;
        CHECK(gpu_vsh_render(device, 8u, 8u, clear_colour, &draw, &frame) == GPU_OK);
        CHECK(frame.pixels != NULL && frame.width == 8u);
        gpu_image_free(&frame);
    }

    static const float widths[] = {2.0f, 0.5f, -1.0f};
    for (size_t i = 0u; i < sizeof widths / sizeof widths[0]; i++) {
        gpu_pgraph_backend wide = backend;
        wide.line_width = widths[i];
        CHECK(replay_primitive(device, wide, GPU_PGRAPH_OP_LINES, 2u, &frame, &report) ==
              GPU_PGRAPH_ERR_UNMEASURED);
        CHECK(frame.pixels == NULL && strstr(report.error, "wideLines") != NULL);
    }
}

/* No device: the index expansion and the assembly's refusals. */
static void test_expansion(void)
{
    printf("test_expansion\n");
    static const uint32_t indices[6] = {10u, 11u, 12u, 13u, 14u, 15u};
    uint32_t out[16];
    CHECK(gpu_pgraph_lineate(GPU_PGRAPH_OP_LINES, indices, 5u, out, 16u) == 4u);
    CHECK(out[0] == 10u && out[1] == 11u && out[2] == 12u && out[3] == 13u);
    CHECK(gpu_pgraph_lineate(GPU_PGRAPH_OP_LINES, indices, 1u, out, 16u) == 0u);
    CHECK(gpu_pgraph_lineate(GPU_PGRAPH_OP_LINE_STRIP, indices, 4u, out, 16u) == 6u);
    CHECK(out[0] == 10u && out[1] == 11u && out[2] == 11u && out[3] == 12u && out[4] == 12u &&
          out[5] == 13u);
    CHECK(gpu_pgraph_lineate(GPU_PGRAPH_OP_LINE_STRIP, indices, 1u, out, 16u) == 0u);
    CHECK(gpu_pgraph_lineate(GPU_PGRAPH_OP_LINE_STRIP, indices, 6u, out, 4u) == UINT32_MAX); /* full */
    CHECK(gpu_pgraph_lineate(GPU_PGRAPH_OP_LINE_LOOP, indices, 4u, out, 16u) == UINT32_MAX);
    CHECK(gpu_pgraph_lineate(GPU_PGRAPH_OP_TRIANGLES, indices, 6u, out, 16u) == UINT32_MAX);
    CHECK(gpu_pgraph_triangulate(GPU_PGRAPH_OP_LINES, indices, 6u, out, 16u) == UINT32_MAX);
    CHECK(gpu_pgraph_triangulate(GPU_PGRAPH_OP_POINTS, indices, 6u, out, 16u) == UINT32_MAX);

    /* assembly: topology, vertex count and the inference bits, without a device */
    memset(memory, 0, sizeof memory);
    for (uint32_t i = 0u; i < 5u; i++) {
        put_vertex(i, i * 3u, i * 3u);
    }
    static const struct {
        uint32_t op;
        uint32_t count;
        uint32_t topology;
        uint32_t vertices;
        uint32_t inference;
    } cases[] = {
        {GPU_PGRAPH_OP_POINTS, 5u, GPU_VSH_TOPOLOGY_POINT_LIST, 5u, GPU_PGRAPH_INFER_POINT_SIZE},
        {GPU_PGRAPH_OP_LINES, 5u, GPU_VSH_TOPOLOGY_LINE_LIST, 4u, GPU_PGRAPH_INFER_LINE_WIDTH},
        {GPU_PGRAPH_OP_LINE_STRIP, 5u, GPU_VSH_TOPOLOGY_LINE_LIST, 8u, GPU_PGRAPH_INFER_LINE_WIDTH},
        {GPU_PGRAPH_OP_TRIANGLES, 5u, GPU_VSH_TOPOLOGY_TRIANGLE_LIST, 3u, 0u},
    };
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i++) {
        fake_guest guest = {MEMORY_BASE, memory, sizeof memory};
        gpu_pgraph_backend backend = make_backend(&guest);
        gpu_pgraph *pgraph = gpu_pgraph_create();
        stream_builder stream = {0};
        begin_primitive_stream(&stream);
        stream_draw_arrays(&stream, cases[i].op, 0u, cases[i].count);
        CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
        gpu_pgraph_assembled assembled;
        gpu_pgraph_report report;
        CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &assembled, &report) == GPU_PGRAPH_OK);
        CHECK(assembled.topology == cases[i].topology);
        CHECK(assembled.vertex_count == cases[i].vertices);
        CHECK(assembled.attributes != NULL);
        const uint32_t both = GPU_PGRAPH_INFER_POINT_SIZE | GPU_PGRAPH_INFER_LINE_WIDTH;
        CHECK((assembled.used_inferences & both) == cases[i].inference);
        if (assembled.attributes != NULL && cases[i].op == GPU_PGRAPH_OP_LINE_STRIP) {
            /* vertex 1 of the strip is the end of segment 0 and the start of segment 1 */
            CHECK(memcmp(assembled.attributes + 1u * GPU_VSH_ATTRIBUTE_FLOATS + 4u,
                         assembled.attributes + 2u * GPU_VSH_ATTRIBUTE_FLOATS + 4u, 12u) == 0);
            CHECK(memcmp(assembled.attributes + 1u * GPU_VSH_ATTRIBUTE_FLOATS + 4u,
                         assembled.attributes + 0u * GPU_VSH_ATTRIBUTE_FLOATS + 4u, 12u) != 0);
        }
        gpu_pgraph_assembled_free(&assembled);
        backend.allowed_inferences = GPU_PGRAPH_INFER_ALL & ~cases[i].inference;
        const gpu_pgraph_result narrow = gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &assembled, &report);
        CHECK(narrow == (cases[i].inference != 0u ? GPU_PGRAPH_ERR_UNMEASURED : GPU_PGRAPH_OK));
        gpu_pgraph_assembled_free(&assembled);
        stream_free(&stream);
        gpu_pgraph_destroy(pgraph);
    }
}

/* T633 (2). `initial_pixels` is what a replayed pass starts from instead of the clear colour: every byte of it, the draws land on top, and it
 * cannot be combined with flip_y (the kept image would be mirrored a second time). NULL is the pass as it always was. */
static void test_initial_pixels(gpu_device *device)
{
    printf("test_initial_pixels (%s)\n", gpu_device_name(device));
    fill_memory();
    fake_guest guest = {MEMORY_BASE, memory, sizeof memory};
    gpu_pgraph_backend backend = make_backend(&guest);
    gpu_pgraph *pgraph = gpu_pgraph_create();
    stream_builder stream = {0};
    build_stream(&stream);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    static const float clear[4] = {0.2f, 0.2f, 0.2f, 1.0f};
    static uint8_t initial[WIDTH * HEIGHT * 4u];
    for (uint32_t i = 0u; i < sizeof initial; i++) {
        initial[i] = (uint8_t)((i * 13u + 5u) % 249u + 2u); /* 2 .. 250 */
    }
    backend.initial_pixels = initial;
    gpu_image frame = {0};
    gpu_pgraph_report report;
    /* no draw and no clear: the pass is the initial image, byte for byte (the last row included) */
    CHECK(gpu_pgraph_replay_pass(pgraph, device, &backend, WIDTH, HEIGHT, clear, 0u, 0u, 0u, 0u, &frame, &report) == GPU_PGRAPH_OK);
    CHECK(frame.pixels != NULL && frame.width == WIDTH && frame.height == HEIGHT && frame.stride_bytes == WIDTH * 4u);
    if (frame.pixels != NULL) {
        CHECK(memcmp(frame.pixels, initial, sizeof initial) == 0);
    }
    gpu_image_free(&frame);
    /* the draws land on top: a covered pixel is the draw's colour, an uncovered one is the initial pixel, not the clear colour */
    CHECK(gpu_pgraph_replay_pass(pgraph, device, &backend, WIDTH, HEIGHT, clear, 0u, gpu_pgraph_draw_count(pgraph), 0u, 0u, &frame, &report) == GPU_PGRAPH_OK);
    CHECK(frame.pixels != NULL);
    if (frame.pixels != NULL) {
        CHECK(pixel_is(&frame, 12u, 15u, 255u, 0u, 0u, 255u));
        CHECK(pixel_is(&frame, 48u, 48u, 255u, 255u, 0u, 255u));
        const size_t uncovered = ((size_t)48u * WIDTH + 16u) * 4u;
        CHECK(pixel_is(&frame, 16u, 48u, initial[uncovered], initial[uncovered + 1u], initial[uncovered + 2u], initial[uncovered + 3u]));
        CHECK(count_not(&frame, 51u, 51u, 51u, 255u) == WIDTH * HEIGHT); /* the clear colour is nowhere */
        const uint8_t *last_row = frame.pixels + (size_t)(HEIGHT - 1u) * frame.stride_bytes;
        uint32_t kept_last_row = 0u;
        for (uint32_t x = 0u; x < WIDTH; x++) {
            kept_last_row += memcmp(last_row + x * 4u, initial + ((size_t)(HEIGHT - 1u) * WIDTH + x) * 4u, 4u) == 0 ? 1u : 0u;
        }
        CHECK(kept_last_row > WIDTH / 2u); /* the draws cover little of the last row: the rest is the kept row, not memory nobody wrote */
    }
    gpu_image_free(&frame);
    /* with flip_y the kept image would be mirrored twice: refused by name before anything is drawn, nothing returned */
    gpu_pgraph_backend flipped = backend;
    flipped.flip_y = true;
    CHECK(gpu_pgraph_replay_pass(pgraph, device, &flipped, WIDTH, HEIGHT, clear, 0u, gpu_pgraph_draw_count(pgraph), 0u, 0u, &frame, &report) ==
          GPU_PGRAPH_ERR_ARGUMENT);
    CHECK(strstr(report.error, "initial image") != NULL && strstr(report.error, "flip_y") != NULL && frame.pixels == NULL);
    /* flip_y alone (no initial image) is the pass it always was */
    flipped.initial_pixels = NULL;
    CHECK(gpu_pgraph_replay_pass(pgraph, device, &flipped, WIDTH, HEIGHT, clear, 0u, gpu_pgraph_draw_count(pgraph), 0u, 0u, &frame, &report) == GPU_PGRAPH_OK);
    gpu_image_free(&frame);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
}

static int run_device(const char *selector)
{
    gpu_device *device = NULL;
    const gpu_result created = gpu_device_create_selected(selector, &device);
    if (created != GPU_OK) {
        printf("SKIP %s: %s\n", selector, gpu_result_string(created));
        return 0;
    }
    test_frame(device);
    test_initial_pixels(device);
    test_unwritten_mode(device);
    test_refusals(device);
    test_points(device);
    test_lines(device);
    test_point_line_refusals(device);
    gpu_device_destroy(device);
    return 1;
}

int main(void)
{
    test_expansion();
    if (failures != 0) {
        printf("%d checks, %d failures\n", checks, failures);
        return 1;
    }
    if (!gpu_vulkan_available()) {
        printf("SKIP: no Vulkan loader on this machine\n");
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
