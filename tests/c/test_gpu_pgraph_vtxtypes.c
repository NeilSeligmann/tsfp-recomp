/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T84d: the vertex array types the replay converts and the ones it keeps refusing. Needs no
 * Vulkan device (vertex assembly only). The real-program, on-device check is
 * tests/test_gpu_pgraph_vtxtypes.py, which also measures the claims below against the retail image.
 *
 *   S32K size 2   converted: two signed 16-bit integers, each to its own value as a float.
 *                 MEASURED: the title's builder (0x1E970) emits S32K only as size 2 at v0 and
 *                 v0 is read only by ARL. INFERRED: the scale (none) and the sign. Gated by
 *                 GPU_PGRAPH_INFER_S32K_UNNORMALISED.
 *   CMP size 1    T1204: one word, x low 11 bits / 1023, y next 11 / 1023, z top 10 / 511 (signed, no floor, T1206),
 *                 w = 1. INFERRED (xemu layout, HQ28), gated by GPU_PGRAPH_INFER_CMP_PACKED.
 *   everything else refused: S32K of another size, CMP of another size, S1, UB_OGL.
 *
 * Every equality is preceded by an assertion that there is something to compare.
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

#define MEMORY_BASE 0x00700000u

typedef struct {
    int16_t index[2];   /* v0: S32K x2 */
    float position[3];  /* v1: F x3 */
} skin_vertex;          /* 16 bytes, the shape of the title's flags 0x11 */

static const int16_t indices[3][2] = {{0, 0}, {4, -4}, {32767, -32768}};

static void fill(skin_vertex *vertices)
{
    for (uint32_t i = 0u; i < 3u; i++) {
        vertices[i].index[0] = indices[i][0];
        vertices[i].index[1] = indices[i][1];
        vertices[i].position[0] = (float)i;
        vertices[i].position[1] = (float)i * 2.0f;
        vertices[i].position[2] = 0.5f;
    }
}

static gpu_pgraph *one_draw(uint32_t slot, uint32_t address, uint32_t format)
{
    gpu_pgraph *pgraph = gpu_pgraph_create();
    stream_builder stream = {0};
    stream_array(&stream, slot, address, format);
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    stream_free(&stream);
    return pgraph;
}

static void test_s32k_values(void)
{
    printf("test_s32k_values\n");
    skin_vertex vertices[3];
    fill(vertices);
    fake_guest guest = {MEMORY_BASE, (uint8_t *)vertices, sizeof vertices};
    gpu_pgraph_backend backend = {0};
    backend.read_guest = fake_guest_read;
    backend.context = &guest;
    backend.allowed_inferences = GPU_PGRAPH_INFER_ALL;

    gpu_pgraph *pgraph = gpu_pgraph_create();
    stream_builder stream = {0};
    stream_array(&stream, 0u, MEMORY_BASE, array_format(sizeof(skin_vertex), 2u, GPU_PGRAPH_TYPE_S32K));
    stream_array(&stream, 1u, MEMORY_BASE + 4u, array_format(sizeof(skin_vertex), 3u, GPU_PGRAPH_TYPE_F));
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    gpu_pgraph_assembled assembled;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &assembled, &report) == GPU_PGRAPH_OK);
    CHECK(assembled.vertex_count == 3u && assembled.attributes != NULL);
    if (assembled.attributes != NULL && assembled.vertex_count == 3u) {
        for (uint32_t v = 0u; v < 3u; v++) {
            const float *slot = assembled.attributes + v * GPU_VSH_ATTRIBUTE_FLOATS;
            /* the integer itself: no division, the sign kept, the lanes in memory order */
            CHECK(slot[0] == (float)indices[v][0] && slot[1] == (float)indices[v][1]);
            /* the lanes the array does not supply read (0, 0, 0, 1), as for a short F array */
            CHECK(slot[2] == 0.0f && slot[3] == 1.0f);
            /* the F array in the same vertex is untouched and reads from its own offset */
            CHECK(slot[4] == (float)v && slot[5] == (float)v * 2.0f && slot[6] == 0.5f);
        }
        const float *last = assembled.attributes + 2u * GPU_VSH_ATTRIBUTE_FLOATS;
        CHECK(last[0] == 32767.0f && last[1] == -32768.0f);
        const float *middle = assembled.attributes + 1u * GPU_VSH_ATTRIBUTE_FLOATS;
        CHECK(middle[0] == 4.0f && middle[1] == -4.0f);
    }
    CHECK((assembled.used_inferences & GPU_PGRAPH_INFER_S32K_UNNORMALISED) != 0u);
    CHECK((assembled.used_inferences & GPU_PGRAPH_INFER_COMPONENT_DEFAULTS) != 0u);
    gpu_pgraph_assembled_free(&assembled);

    /* withheld alone, the inference refuses the draw and returns nothing */
    gpu_pgraph_backend strict = backend;
    strict.allowed_inferences = GPU_PGRAPH_INFER_ALL & ~GPU_PGRAPH_INFER_S32K_UNNORMALISED;
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &strict, &assembled, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "S32K") != NULL && strstr(report.error, "INFERRED") != NULL &&
          strstr(report.error, "not allowed") != NULL);
    CHECK(assembled.attributes == NULL);

    /* a short guest read names the slot and the 4 bytes of one S32K x2 element */
    fake_guest cut_guest = {MEMORY_BASE, (uint8_t *)vertices, 2u * sizeof(skin_vertex) + 2u};
    gpu_pgraph_backend cut = backend;
    cut.context = &cut_guest;
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &cut, &assembled, &report) == GPU_PGRAPH_ERR_MALFORMED);
    CHECK(strstr(report.error, "4 bytes") != NULL && strstr(report.error, "slot 0") != NULL);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
}

static void test_cmp_values(void)
{
    printf("test_cmp_values\n");
    /* x low 11 bits, y bits 11..21, z bits 22..31 (two's complement fields) */
    static const struct {
        uint32_t word;
        float expect[3];
    } cases[3] = {
        {0x000003FFu, {1023.0f / 1023.0f, 0.0f, 0.0f}},                   /* x max */
        {0x00000400u | (0x1FFu << 11) | (0x1FFu << 22),                   /* x -1024 stays -1024/1023 (no floor, xemu array path), y 511, z 511 */
         {-1024.0f / 1023.0f, 511.0f / 1023.0f, 1.0f}},
        {(0x7FFu << 11) | (0x200u << 22), {0.0f, -1.0f / 1023.0f, -512.0f / 511.0f}}, /* y -1, z -512 stays -512/511 */
    };
    uint32_t words[3];
    for (size_t i = 0u; i < 3u; i++) {
        words[i] = cases[i].word;
    }
    fake_guest guest = {MEMORY_BASE, (uint8_t *)words, sizeof words};
    gpu_pgraph_backend backend = {0};
    backend.read_guest = fake_guest_read;
    backend.context = &guest;
    backend.allowed_inferences = GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_CMP_PACKED;
    gpu_pgraph *pgraph = gpu_pgraph_create();
    stream_builder stream = {0};
    stream_array(&stream, 5u, MEMORY_BASE, array_format(4u, 1u, GPU_PGRAPH_TYPE_CMP));
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    gpu_pgraph_assembled assembled;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &assembled, &report) == GPU_PGRAPH_OK);
    CHECK(assembled.vertex_count == 3u && assembled.attributes != NULL);
    if (assembled.attributes != NULL && assembled.vertex_count == 3u) {
        for (uint32_t v = 0u; v < 3u; v++) {
            const float *slot = assembled.attributes + v * GPU_VSH_ATTRIBUTE_FLOATS + 5u * 4u;
            CHECK(slot[0] == cases[v].expect[0] && slot[1] == cases[v].expect[1] && slot[2] == cases[v].expect[2]);
            CHECK(slot[3] == 1.0f);
        }
    }
    CHECK((assembled.used_inferences & GPU_PGRAPH_INFER_CMP_PACKED) != 0u);
    gpu_pgraph_assembled_free(&assembled);
    gpu_pgraph_backend strict = backend;
    strict.allowed_inferences = GPU_PGRAPH_INFER_ALL; /* CMP_PACKED is not part of ALL */
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &strict, &assembled, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "CMP") != NULL);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
}

static void test_still_refused(void)
{
    printf("test_still_refused\n");
    skin_vertex vertices[3];
    fill(vertices);
    fake_guest guest = {MEMORY_BASE, (uint8_t *)vertices, sizeof vertices};
    gpu_pgraph_backend backend = {0};
    backend.read_guest = fake_guest_read;
    backend.context = &guest;
    backend.allowed_inferences = GPU_PGRAPH_INFER_ALL;
    static const struct {
        uint32_t size;
        uint32_t type;
        const char *name;
        const char *missing;
    } refused[] = {
        {1u, GPU_PGRAPH_TYPE_S32K, "S32K", "only size 2"},
        {3u, GPU_PGRAPH_TYPE_S32K, "S32K", "only size 2"},
        {4u, GPU_PGRAPH_TYPE_S32K, "S32K", "only size 2"},
        {2u, GPU_PGRAPH_TYPE_CMP, "CMP", "only the one word"},
        {3u, GPU_PGRAPH_TYPE_CMP, "CMP", "only the one word"},
        {2u, GPU_PGRAPH_TYPE_S1, "S1", "no emitter in the image"},
        {4u, GPU_PGRAPH_TYPE_S1, "S1", "no emitter in the image"},
        {4u, GPU_PGRAPH_TYPE_UB_OGL, "UB_OGL", "no emitter in the image"},
    };
    CHECK(sizeof refused / sizeof refused[0] == 8u);
    for (size_t i = 0u; i < sizeof refused / sizeof refused[0]; i++) {
        gpu_pgraph *pgraph = one_draw(0u, MEMORY_BASE, array_format(16u, refused[i].size, refused[i].type));
        gpu_pgraph_assembled assembled;
        gpu_pgraph_report report;
        memset(&report, 0, sizeof report);
        CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &assembled, &report) ==
              GPU_PGRAPH_ERR_UNMEASURED);
        CHECK(strlen(report.error) > 0u);
        CHECK(strstr(report.error, refused[i].name) != NULL);
        CHECK(strstr(report.error, "no measured conversion") != NULL);
        CHECK(strstr(report.error, refused[i].missing) != NULL);
        CHECK(assembled.attributes == NULL && assembled.vertex_count == 0u);
        gpu_pgraph_destroy(pgraph);
    }
}

/* T1255: a draw whose vertex bytes were captured at decode (the live path, the assembly takes its per slot fast path for vertices after
 * the first) must assemble to the exact floats the guest read path (fetch_attribute for every vertex) gives, for every type. */
#define MIXED_STRIDE 48u
#define MIXED_VERTICES 12u

static gpu_pgraph *mixed_draw(bool capture, fake_guest *guest)
{
    gpu_pgraph *pgraph = gpu_pgraph_create();
    if (capture) {
        gpu_pgraph_set_vertex_capture(pgraph, fake_guest_read, guest, 4096u);
    }
    stream_builder stream = {0};
    stream_array(&stream, 0u, MEMORY_BASE, array_format(MIXED_STRIDE, 4u, GPU_PGRAPH_TYPE_F));
    stream_array(&stream, 1u, MEMORY_BASE + 16u, array_format(MIXED_STRIDE, 4u, GPU_PGRAPH_TYPE_UB_D3D));
    stream_array(&stream, 2u, MEMORY_BASE + 20u, array_format(MIXED_STRIDE, 1u, GPU_PGRAPH_TYPE_CMP));
    stream_array(&stream, 3u, MEMORY_BASE + 24u, array_format(MIXED_STRIDE, 2u, GPU_PGRAPH_TYPE_S32K));
    stream_array(&stream, 4u, MEMORY_BASE + 28u, array_format(MIXED_STRIDE, 3u, GPU_PGRAPH_TYPE_F));
    stream_array(&stream, 5u, MEMORY_BASE + 40u, array_format(MIXED_STRIDE, 1u, GPU_PGRAPH_TYPE_F));
    stream_array(&stream, 6u, MEMORY_BASE + 44u, array_format(MIXED_STRIDE, 0u, GPU_PGRAPH_TYPE_F)); /* disabled: (0, 0, 0, 1) */
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, MIXED_VERTICES);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    stream_free(&stream);
    return pgraph;
}

static void test_captured_equals_guest_read(void)
{
    printf("test_captured_equals_guest_read\n");
    static uint8_t memory[MIXED_STRIDE * MIXED_VERTICES];
    uint32_t state = 0x12345678u;
    for (size_t i = 0u; i < sizeof memory; i++) {
        state = state * 1664525u + 1013904223u;
        memory[i] = (uint8_t)(state >> 24);
    }
    for (uint32_t v = 0u; v < MIXED_VERTICES; v++) { /* finite floats for slots 0, 4 and 5, so the comparison is on values */
        const float lanes[4] = {(float)v + 0.25f, -(float)v * 3.0f, 0.5f + (float)v, 7.0f};
        memcpy(memory + v * MIXED_STRIDE, lanes, 16u);
        memcpy(memory + v * MIXED_STRIDE + 28u, lanes, 12u);
        memcpy(memory + v * MIXED_STRIDE + 40u, lanes + 1, 4u);
    }
    fake_guest guest = {MEMORY_BASE, memory, sizeof memory};
    gpu_pgraph_backend backend = {0};
    backend.read_guest = fake_guest_read;
    backend.context = &guest;
    backend.allowed_inferences = GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_CMP_PACKED;
    gpu_pgraph *plain = mixed_draw(false, &guest);
    gpu_pgraph *captured = mixed_draw(true, &guest);
    CHECK(gpu_pgraph_draw_vertex_bytes(plain, 0u, 1u, NULL, NULL) == NULL);     /* the slow path reads the guest */
    CHECK(gpu_pgraph_draw_vertex_bytes(captured, 0u, 1u, NULL, NULL) != NULL);  /* the fast path reads the snapshot */
    gpu_pgraph_assembled slow, fast;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    CHECK(gpu_pgraph_assemble_draw(plain, 0u, &backend, &slow, &report) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_assemble_draw(captured, 0u, &backend, &fast, &report) == GPU_PGRAPH_OK);
    CHECK(slow.vertex_count == MIXED_VERTICES && fast.vertex_count == MIXED_VERTICES);
    CHECK(slow.attributes != NULL && fast.attributes != NULL);
    if (slow.attributes != NULL && fast.attributes != NULL) {
        const size_t bytes = (size_t)MIXED_VERTICES * GPU_VSH_ATTRIBUTE_FLOATS * sizeof(float);
        CHECK(memcmp(slow.attributes, fast.attributes, bytes) == 0);
        CHECK(slow.used_inferences == fast.used_inferences);
        /* something to compare: the colour, packed and integer slots of the last vertex hold the values worked out by hand */
        const uint32_t v = MIXED_VERTICES - 1u;
        const uint8_t *raw = memory + v * MIXED_STRIDE;
        const float *out = fast.attributes + v * GPU_VSH_ATTRIBUTE_FLOATS;
        CHECK(out[4] == (float)raw[18] / 255.0f && out[5] == (float)raw[17] / 255.0f && out[6] == (float)raw[16] / 255.0f &&
              out[7] == (float)raw[19] / 255.0f);
        const uint32_t packed = (uint32_t)raw[20] | ((uint32_t)raw[21] << 8) | ((uint32_t)raw[22] << 16) | ((uint32_t)raw[23] << 24);
        CHECK(out[8] == (float)((int32_t)(packed << 21) >> 21) / 1023.0f && out[9] == (float)((int32_t)(packed << 10) >> 21) / 1023.0f &&
              out[10] == (float)((int32_t)packed >> 22) / 511.0f && out[11] == 1.0f);
        CHECK(out[12] == (float)(int16_t)(raw[24] | (raw[25] << 8)) && out[13] == (float)(int16_t)(raw[26] | (raw[27] << 8)) &&
              out[14] == 0.0f && out[15] == 1.0f);
        CHECK(out[16] == (float)v + 0.25f && out[17] == -(float)v * 3.0f && out[18] == 0.5f + (float)v && out[19] == 1.0f);
        CHECK(out[20] == -(float)v * 3.0f && out[21] == 0.0f && out[23] == 1.0f);
        CHECK(out[24] == 0.0f && out[25] == 0.0f && out[26] == 0.0f && out[27] == 1.0f);
    }
    gpu_pgraph_assembled_free(&slow);
    gpu_pgraph_assembled_free(&fast);
    /* withheld inference: the first error is the same with and without the snapshot */
    gpu_pgraph_backend strict = backend;
    strict.allowed_inferences = GPU_PGRAPH_INFER_ALL; /* CMP_PACKED is not part of ALL */
    char slow_error[sizeof report.error], fast_error[sizeof report.error];
    CHECK(gpu_pgraph_assemble_draw(plain, 0u, &strict, &slow, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    snprintf(slow_error, sizeof slow_error, "%s", report.error);
    CHECK(gpu_pgraph_assemble_draw(captured, 0u, &strict, &fast, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    snprintf(fast_error, sizeof fast_error, "%s", report.error);
    CHECK(strstr(slow_error, "CMP") != NULL && strcmp(slow_error, fast_error) == 0);
    gpu_pgraph_destroy(plain);
    gpu_pgraph_destroy(captured);
}

int main(void)
{
    test_s32k_values();
    test_captured_equals_guest_read();
    test_cmp_values();
    test_still_refused();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 && checks > 40 ? 0 : 1;
}
