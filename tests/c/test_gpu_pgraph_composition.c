/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T578 (end of file): the BLIT group, the 2D engine blit CopyRects emits, from the decoder down to the rectangle copy.
 *
 * T462 and T541: the decoder and replay pieces the Swap flag 1 copy composition and the SetRenderTarget packet
 * need, none of which needs a device. The SURFACE, FIXED and TEXTURE groups (words pinned to the measured values,
 * each refusal named), the IMMEDIATE group (SET_VERTEX_DATA2F_M vertices inside a bracket) down to the vertices the
 * replay assembles, and the SPIR-V rewrite that gives an unwritten varying its initial value. The values are the
 * ones the retail stream carries (docs/d3d8-copy-composition.md, tools/xmv_replay.py). Every equality is preceded by
 * an assertion that there is something to compare.
 */
#include "gpu_combiner.h"
#include "gpu_pgraph.h"
#include "gpu_pgraph_replay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(condition)                                                          \
    do {                                                                          \
        checks++;                                                                 \
        if (!(condition)) {                                                       \
            failures++;                                                           \
            printf("  FAIL line %d: %s\n", __LINE__, #condition);                 \
        }                                                                         \
    } while (0)

typedef struct {
    gpu_pgraph_command items[512];
    size_t count;
} stream;

static void add(stream *s, uint32_t method, uint32_t data)
{
    s->items[s->count].method = method;
    s->items[s->count].data = data;
    s->count++;
}

static gpu_pgraph *model(uint32_t groups, bool strict)
{
    gpu_pgraph *pgraph = gpu_pgraph_create();
    CHECK(pgraph != NULL);
    gpu_pgraph_set_strict(pgraph, strict);
    gpu_pgraph_set_output_groups(pgraph, groups);
    return pgraph;
}

static gpu_pgraph_result decode_one(gpu_pgraph *pgraph, uint32_t method, uint32_t data)
{
    const gpu_pgraph_command command = {method, data};
    return gpu_pgraph_decode(pgraph, &command, 1u);
}

/* --- groups ------------------------------------------------------------------------------- */

typedef struct {
    uint32_t method;
    gpu_pgraph_output_word word;
    uint32_t group;
} word_row;

static void test_word_table(void)
{
    printf("test_word_table\n");
    static const word_row rows[] = {
        {0x0208u, GPU_PGRAPH_OUT_SURFACE_FORMAT, GPU_PGRAPH_OUTPUT_SURFACE},
        {0x020Cu, GPU_PGRAPH_OUT_SURFACE_PITCH, GPU_PGRAPH_OUTPUT_SURFACE},
        {0x0210u, GPU_PGRAPH_OUT_SURFACE_COLOR_OFFSET, GPU_PGRAPH_OUTPUT_SURFACE},
        {0x0214u, GPU_PGRAPH_OUT_SURFACE_ZETA_OFFSET, GPU_PGRAPH_OUTPUT_SURFACE},
        {0x0290u, GPU_PGRAPH_OUT_CONTROL0, GPU_PGRAPH_OUTPUT_SURFACE},
        {0x0394u, GPU_PGRAPH_OUT_CLIP_MIN, GPU_PGRAPH_OUTPUT_SURFACE},
        {0x0398u, GPU_PGRAPH_OUT_CLIP_MAX, GPU_PGRAPH_OUTPUT_SURFACE},
        {0x1D7Cu, GPU_PGRAPH_OUT_ANTI_ALIASING, GPU_PGRAPH_OUTPUT_SURFACE},
        {0x0314u, GPU_PGRAPH_OUT_LIGHTING_ENABLE, GPU_PGRAPH_OUTPUT_FIXED},
        {0x0294u, GPU_PGRAPH_OUT_LIGHT_CONTROL, GPU_PGRAPH_OUTPUT_FIXED},
        {0x03BCu, GPU_PGRAPH_OUT_LIGHT_ENABLE_MASK, GPU_PGRAPH_OUTPUT_FIXED},
        {0x03B8u, GPU_PGRAPH_OUT_SPECULAR_ENABLE, GPU_PGRAPH_OUTPUT_FIXED},
        {0x02A4u, GPU_PGRAPH_OUT_FOG_ENABLE, GPU_PGRAPH_OUTPUT_FIXED},
        {0x0318u, GPU_PGRAPH_OUT_POINT_PARAMS_ENABLE, GPU_PGRAPH_OUTPUT_FIXED},
        {0x031Cu, GPU_PGRAPH_OUT_POINT_SMOOTH_ENABLE, GPU_PGRAPH_OUTPUT_FIXED},
        {0x043Cu, GPU_PGRAPH_OUT_POINT_SIZE, GPU_PGRAPH_OUTPUT_FIXED},
        {0x038Cu, GPU_PGRAPH_OUT_FRONT_POLYGON_MODE, GPU_PGRAPH_OUTPUT_FIXED},
        {0x0390u, GPU_PGRAPH_OUT_BACK_POLYGON_MODE, GPU_PGRAPH_OUTPUT_FIXED},
        {0x1D84u, GPU_PGRAPH_OUT_ZCULL_ENABLE, GPU_PGRAPH_OUTPUT_FIXED},
    };
    CHECK(sizeof rows / sizeof rows[0] == 19u);
    for (size_t i = 0u; i < sizeof rows / sizeof rows[0]; i++) {
        uint32_t group = 0u;
        CHECK(gpu_pgraph_output_index(rows[i].method, &group) == (int)rows[i].word);
        CHECK(group == rows[i].group && gpu_pgraph_output_group(rows[i].word) == rows[i].group);
        /* group off: unhandled, strict refuses it, lenient counts it */
        gpu_pgraph *strict = model(GPU_PGRAPH_OUTPUT_ALL_MEASURED & ~rows[i].group, true);
        CHECK(decode_one(strict, rows[i].method, 0u) == GPU_PGRAPH_ERR_UNMEASURED);
        gpu_pgraph_destroy(strict);
        gpu_pgraph *lenient = model(0u, false);
        CHECK(decode_one(lenient, rows[i].method, 0u) == GPU_PGRAPH_OK && gpu_pgraph_unhandled_count(lenient) == 1u);
        gpu_pgraph_destroy(lenient);
        /* group on: shadowed raw, written flag set, no unhandled method */
        gpu_pgraph *on = model(rows[i].group, true);
        CHECK(decode_one(on, rows[i].method, 0x1234u + (uint32_t)i) == GPU_PGRAPH_OK);
        const gpu_pgraph_state *state = gpu_pgraph_state_now(on);
        CHECK(state->output_written[rows[i].word] && state->output[rows[i].word] == 0x1234u + (uint32_t)i);
        CHECK(gpu_pgraph_unhandled_count(on) == 0u);
        gpu_pgraph_destroy(on);
    }
    /* the texture stage words: stage s address, control 0 and filter, bump environment of stages 1 to 3 */
    for (uint32_t stage = 0u; stage < 4u; stage++) {
        const uint32_t methods[3] = {0x1B08u + 0x40u * stage, 0x1B0Cu + 0x40u * stage, 0x1B14u + 0x40u * stage};
        const int words[3] = {GPU_PGRAPH_OUT_TEXTURE_ADDRESS + (int)stage, GPU_PGRAPH_OUT_TEXTURE_CONTROL0 + (int)stage,
                              GPU_PGRAPH_OUT_TEXTURE_FILTER + (int)stage};
        for (size_t i = 0u; i < 3u; i++) {
            uint32_t group = 0u;
            CHECK(gpu_pgraph_output_index(methods[i], &group) == words[i] && group == GPU_PGRAPH_OUTPUT_TEXTURE);
        }
    }
    for (uint32_t index = 0u; index < 18u; index++) {
        const uint32_t method = 0x1B28u + 0x40u * (index / 6u + 1u) + 4u * (index % 6u);
        uint32_t group = 0u;
        CHECK(gpu_pgraph_output_index(method, &group) == GPU_PGRAPH_OUT_TEXTURE_BUMP + (int)index &&
              group == GPU_PGRAPH_OUTPUT_TEXTURE);
    }
    CHECK(gpu_pgraph_output_index(0x1B68u, NULL) == GPU_PGRAPH_OUT_TEXTURE_BUMP); /* the first bump word the stream has */
    CHECK(gpu_pgraph_output_index(0x1BFCu, NULL) == GPU_PGRAPH_OUT_COUNT - 1);    /* the last */
    CHECK(gpu_pgraph_output_index(0x1B28u, NULL) == -1);                          /* stage 0 has no bump words */
    CHECK(gpu_pgraph_output_index(0x1B00u, NULL) == -1 && gpu_pgraph_output_index(0x1B04u, NULL) == -1);
    CHECK((GPU_PGRAPH_OUTPUT_ALL_MEASURED & (GPU_PGRAPH_OUTPUT_SURFACE | GPU_PGRAPH_OUTPUT_FIXED |
                                             GPU_PGRAPH_OUTPUT_TEXTURE | GPU_PGRAPH_OUTPUT_IMMEDIATE)) ==
          (GPU_PGRAPH_OUTPUT_SURFACE | GPU_PGRAPH_OUTPUT_FIXED | GPU_PGRAPH_OUTPUT_TEXTURE | GPU_PGRAPH_OUTPUT_IMMEDIATE));
    /* a bracket refuses them like every other output word */
    gpu_pgraph *bracket = model(GPU_PGRAPH_OUTPUT_FIXED, true);
    CHECK(decode_one(bracket, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_TRIANGLES) == GPU_PGRAPH_OK);
    CHECK(decode_one(bracket, 0x0314u, 0u) == GPU_PGRAPH_ERR_UNMEASURED);
    gpu_pgraph_destroy(bracket);
}

/* T1289: gpu_pgraph_output_index answers through a direct table. Every method of the range gives the answer of the table scan it replaced:
 * each of the output words is found by exactly one method, a word with its own group, nothing unaligned or out of range matches. */
static void test_output_index_is_a_bijection(void)
{
    printf("test_output_index_is_a_bijection\n");
    int hits[GPU_PGRAPH_OUT_COUNT];
    for (int word = 0; word < (int)GPU_PGRAPH_OUT_COUNT; word++) {
        hits[word] = 0;
    }
    int found = 0;
    for (uint32_t method = 0u; method < 0x4000u; method++) {
        uint32_t group = 0xFFFFFFFFu;
        const int word = gpu_pgraph_output_index(method, &group);
        if (word < 0) {
            CHECK(group == 0xFFFFFFFFu); /* a miss leaves the group alone */
            continue;
        }
        found++;
        CHECK((method & 3u) == 0u && word < (int)GPU_PGRAPH_OUT_COUNT);
        CHECK(group == gpu_pgraph_output_group((uint32_t)word));
        if (word < (int)GPU_PGRAPH_OUT_COUNT) {
            hits[word]++;
        }
    }
    CHECK(found == (int)GPU_PGRAPH_OUT_COUNT);
    for (int word = 0; word < (int)GPU_PGRAPH_OUT_COUNT; word++) {
        CHECK(hits[word] == 1);
    }
    CHECK(gpu_pgraph_output_index(0x1BFDu, NULL) == -1 && gpu_pgraph_output_index(0x2000u, NULL) == -1 &&
          gpu_pgraph_output_index(0xFFFFFFFCu, NULL) == -1);
}

static void test_sync_pairs(void)
{
    printf("test_sync_pairs\n");
    gpu_pgraph *on = model(GPU_PGRAPH_OUTPUT_SURFACE, true);
    CHECK(decode_one(on, GPU_PGRAPH_NO_OPERATION, 0u) == GPU_PGRAPH_OK);
    CHECK(decode_one(on, GPU_PGRAPH_WAIT_FOR_IDLE, 0u) == GPU_PGRAPH_OK);
    const gpu_pgraph_stats stats = gpu_pgraph_get_stats(on);
    CHECK(stats.sync_pairs == 2u && stats.pairs == 2u && stats.pairs_handled == 2u);
    CHECK(decode_one(on, GPU_PGRAPH_NO_OPERATION, 1u) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(gpu_pgraph_error(on), "other than 0") != NULL);
    gpu_pgraph_destroy(on);
    gpu_pgraph *wait = model(GPU_PGRAPH_OUTPUT_SURFACE, true);
    CHECK(decode_one(wait, GPU_PGRAPH_WAIT_FOR_IDLE, 4u) == GPU_PGRAPH_ERR_UNMEASURED);
    gpu_pgraph_destroy(wait);
    gpu_pgraph *bracket = model(GPU_PGRAPH_OUTPUT_SURFACE, true);
    CHECK(decode_one(bracket, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_TRIANGLES) == GPU_PGRAPH_OK);
    CHECK(decode_one(bracket, GPU_PGRAPH_NO_OPERATION, 0u) == GPU_PGRAPH_ERR_UNMEASURED);
    gpu_pgraph_destroy(bracket);
    gpu_pgraph *off = model(GPU_PGRAPH_OUTPUT_ALL_MEASURED & ~GPU_PGRAPH_OUTPUT_SURFACE, true);
    CHECK(decode_one(off, GPU_PGRAPH_NO_OPERATION, 0u) == GPU_PGRAPH_ERR_UNMEASURED); /* group off: as before */
    gpu_pgraph_destroy(off);
}

/* The state the retail composition holds, as the replay resolves it. */
static void measured_state(gpu_pgraph *pgraph)
{
    static const uint32_t words[][2] = {
        {0x0208u, 0x00000128u}, {0x020Cu, 0x0A000A00u}, {0x0210u, 0x004A0000u}, {0x0214u, 0u},
        {0x0290u, 0x00100001u}, {0x0394u, 0u}, {0x0398u, 0x4B7FFFFFu}, {0x1D7Cu, 0xFFFF0000u},
        {0x0314u, 0u}, {0x0294u, 0x00020001u}, {0x03BCu, 0u}, {0x03B8u, 0u}, {0x02A4u, 0u},
        {0x0318u, 0u}, {0x031Cu, 0u}, {0x043Cu, 0u}, {0x038Cu, 0x1B02u}, {0x0390u, 0x1B02u}, {0x1D84u, 1u},
        {0x1B08u, 0x303u}, {0x1B0Cu, 0x4003FFC0u}, {0x1B14u, 0x02062000u},
        {0x1B48u, 0u}, {0x1B4Cu, 0x0003FFC0u}, {0x1B54u, 0x02062000u},
        {0x1B68u, 0u}, {0x1B6Cu, 0u}, {0x1B70u, 0u}, {0x1B74u, 0u}, {0x1B78u, 0u}, {0x1B7Cu, 0u},
    };
    for (size_t i = 0u; i < sizeof words / sizeof words[0]; i++) {
        CHECK(decode_one(pgraph, words[i][0], words[i][1]) == GPU_PGRAPH_OK);
    }
}

static gpu_pgraph_backend backend_with(uint32_t groups, uint32_t inferences)
{
    gpu_pgraph_backend backend;
    memset(&backend, 0, sizeof backend);
    backend.output_groups = groups;
    backend.allowed_inferences = inferences;
    return backend;
}

static gpu_pgraph_result resolve(const gpu_pgraph *pgraph, const gpu_pgraph_backend *backend, gpu_pgraph_output *out,
                                 gpu_pgraph_report *report)
{
    return gpu_pgraph_resolve_output(gpu_pgraph_state_now(pgraph), backend, 640u, 480u, out, report);
}

static void test_measured_values_resolve(void)
{
    printf("test_measured_values_resolve\n");
    gpu_pgraph *pgraph = model(GPU_PGRAPH_OUTPUT_ALL_MEASURED, true);
    measured_state(pgraph);
    const uint32_t groups = GPU_PGRAPH_OUTPUT_SURFACE | GPU_PGRAPH_OUTPUT_FIXED | GPU_PGRAPH_OUTPUT_TEXTURE;
    gpu_pgraph_output out;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    gpu_pgraph_backend allowed = backend_with(groups, GPU_PGRAPH_INFER_OUTPUT_PINNED_STATE);
    CHECK(resolve(pgraph, &allowed, &out, &report) == GPU_PGRAPH_OK);
    CHECK((out.used_inferences & GPU_PGRAPH_INFER_OUTPUT_PINNED_STATE) != 0u);
    CHECK(!out.active); /* the draw is what it always was: nothing the replay applies */
    /* without the inference each group names it and refuses */
    gpu_pgraph_backend refused = backend_with(groups, 0u);
    CHECK(resolve(pgraph, &refused, &out, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "surface, fixed-function and texture stage words") != NULL);
    /* a stream that wrote a group the backend did not enable is refused, naming it */
    const struct {
        uint32_t group;
        const char *name;
    } missing[] = {
        {GPU_PGRAPH_OUTPUT_SURFACE, "surface"}, {GPU_PGRAPH_OUTPUT_FIXED, "fixed-function"},
        {GPU_PGRAPH_OUTPUT_TEXTURE, "texture stage"},
    };
    for (size_t i = 0u; i < 3u; i++) {
        gpu_pgraph_backend partial = backend_with(groups & ~missing[i].group, GPU_PGRAPH_INFER_OUTPUT_PINNED_STATE);
        CHECK(resolve(pgraph, &partial, &out, &report) == GPU_PGRAPH_ERR_UNMEASURED);
        CHECK(strstr(report.error, missing[i].name) != NULL && strstr(report.error, "does not enable it") != NULL);
    }
    gpu_pgraph_destroy(pgraph);
}

static void test_refusals(void)
{
    printf("test_refusals\n");
    static const struct {
        uint32_t method;
        uint32_t value;
        const char *text;
    } bad[] = {
        {0x0208u, 0x00000127u, "not A8R8G8B8"},           /* colour 7 */
        {0x0208u, 0x00000118u, "not A8R8G8B8"},           /* zeta 1 (Z16) */
        {0x0208u, 0x00000228u, "not A8R8G8B8"},           /* swizzle layout */
        {0x0208u, 0x00001128u, "not A8R8G8B8"},           /* antialiasing */
        {0x0208u, 0x09090128u, "not A8R8G8B8"},           /* a swizzle size field */
        {0x0290u, 0x00100000u, "CONTROL0"},
        {0x0290u, 0x00110001u, "CONTROL0"},
        {0x0394u, 0x3F800000u, "depth clip minimum"},
        {0x0398u, 0x3F800000u, "depth clip maximum"},
        {0x1D7Cu, 0xFFFF0001u, "anti-aliasing"},
        {0x1D7Cu, 0x55550000u, "anti-aliasing"},
        {0x1D7Cu, 0x00000001u, "anti-aliasing"},
        {0x0314u, 1u, "lighting enable"},
        {0x03B8u, 1u, "specular enable"},
        {0x03BCu, 1u, "light enable mask"},
        {0x02A4u, 1u, "fog enable"},
        {0x0318u, 1u, "point parameters"},
        {0x031Cu, 1u, "point smooth"},
        {0x038Cu, 0x1B01u, "front polygon mode"},
        {0x0390u, 0x1B00u, "back polygon mode"},
        {0x0294u, 0x00040001u, "light control"},
        {0x1D84u, 4u, "z-cull"},
        {0x1B08u, 0x00000202u, "texture stage 0 address"},
        {0x1B88u, 0x00000303u + 0x10000u, "texture stage 2 address"},
        {0x1B0Cu, 0x4003FFC4u, "texture stage 0 control"},
        {0x1B0Cu, 0x4003FF80u, "texture stage 0 control"}, /* a lower maximum LOD clamp */
        {0x1B0Cu, 0x4007FFC0u, "texture stage 0 control"}, /* a minimum LOD clamp */
        {0x1B4Cu, 0x0003FFC0u + 0x40000000u * 2u, "texture stage 1 control"},
        {0x1BD4u, 0x02062001u, "texture stage 3 filter"},
        {0x1B68u, 1u, "bump environment word 0x1B68 of texture stage 1"},
        {0x1BFCu, 0x3F800000u, "bump environment word 0x1BFC of texture stage 3"},
        {0x1BB8u, 0x3F800000u, "bump environment word 0x1BB8 of texture stage 2"},
    };
    CHECK(sizeof bad / sizeof bad[0] == 32u);
    const uint32_t groups = GPU_PGRAPH_OUTPUT_SURFACE | GPU_PGRAPH_OUTPUT_FIXED | GPU_PGRAPH_OUTPUT_TEXTURE;
    for (size_t i = 0u; i < sizeof bad / sizeof bad[0]; i++) {
        gpu_pgraph *pgraph = model(GPU_PGRAPH_OUTPUT_ALL_MEASURED, true);
        CHECK(decode_one(pgraph, bad[i].method, bad[i].value) == GPU_PGRAPH_OK); /* the decoder shadows any value */
        gpu_pgraph_backend backend = backend_with(groups, GPU_PGRAPH_INFER_OUTPUT_PINNED_STATE);
        gpu_pgraph_output out;
        gpu_pgraph_report report;
        memset(&report, 0, sizeof report);
        CHECK(resolve(pgraph, &backend, &out, &report) == GPU_PGRAPH_ERR_UNMEASURED);
        CHECK(strstr(report.error, bad[i].text) != NULL);
        if (strstr(report.error, bad[i].text) == NULL) {
            printf("    row %zu: %s\n", i, report.error);
        }
        CHECK(!out.active && out.used_inferences == 0u); /* a refusal leaves nothing half resolved */
        gpu_pgraph_destroy(pgraph);
    }
    /* the measured alternatives in the same slots resolve */
    static const struct {
        uint32_t method;
        uint32_t value;
    } good[] = {
        {0x0294u, 0x00000000u}, {0x0294u, 0x00030001u}, {0x1D84u, 0u}, {0x1D84u, 3u}, {0x1B08u, 0u}, {0x1B08u, 0x101u},
        {0x1B0Cu, 0x0003FFC0u}, {0x043Cu, 0x40000000u}, /* point size: shadowed, any value */
        {0x1D7Cu, 0x00000000u}, {0x1D7Cu, 0xFFFF0000u}, /* both measured sample masks */
    };
    for (size_t i = 0u; i < sizeof good / sizeof good[0]; i++) {
        gpu_pgraph *pgraph = model(GPU_PGRAPH_OUTPUT_ALL_MEASURED, true);
        CHECK(decode_one(pgraph, good[i].method, good[i].value) == GPU_PGRAPH_OK);
        gpu_pgraph_backend backend = backend_with(groups, GPU_PGRAPH_INFER_OUTPUT_PINNED_STATE);
        gpu_pgraph_output out;
        gpu_pgraph_report report;
        memset(&report, 0, sizeof report);
        CHECK(resolve(pgraph, &backend, &out, &report) == GPU_PGRAPH_OK);
        gpu_pgraph_destroy(pgraph);
    }
    /* a stream that wrote any one of the words needs the inference, not only a stream that wrote them all */
    static const struct {
        uint32_t method;
        uint32_t value;
    } alone[] = {
        {0x0208u, 0x00000128u}, {0x020Cu, 0x0A000A00u}, {0x0210u, 0x004A0000u}, {0x0290u, 0x00100001u},
        {0x1D7Cu, 0xFFFF0000u}, {0x043Cu, 0u}, {0x0314u, 0u}, {0x1B14u, 0x02062000u}, {0x1B68u, 0u},
    };
    for (size_t i = 0u; i < sizeof alone / sizeof alone[0]; i++) {
        gpu_pgraph *one = model(GPU_PGRAPH_OUTPUT_ALL_MEASURED, true);
        CHECK(decode_one(one, alone[i].method, alone[i].value) == GPU_PGRAPH_OK);
        gpu_pgraph_backend lacking = backend_with(groups, 0u);
        gpu_pgraph_output lone_out;
        gpu_pgraph_report lone_report;
        memset(&lone_report, 0, sizeof lone_report);
        CHECK(resolve(one, &lacking, &lone_out, &lone_report) == GPU_PGRAPH_ERR_UNMEASURED);
        CHECK(strstr(lone_report.error, "each at the value measured in the recorded stream") != NULL);
        gpu_pgraph_destroy(one);
    }
    /* nothing written: no inference needed, nothing resolved */
    gpu_pgraph *quiet = model(GPU_PGRAPH_OUTPUT_ALL_MEASURED, true);
    gpu_pgraph_backend none = backend_with(groups, 0u);
    gpu_pgraph_output out;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    CHECK(resolve(quiet, &none, &out, &report) == GPU_PGRAPH_OK && out.used_inferences == 0u);
    gpu_pgraph_destroy(quiet);
}

/* T919: admit only the observed LOD0-linear word, and only through explicit texture inference.
 * Every stage and already-supported address pair uses the same gate; unknown adjacent words refuse. */
static void test_title_lod0_filter(void)
{
    const uint32_t addresses[] = {0u, 0x101u, 0x303u};
    for (uint32_t stage = 0u; stage < 4u; stage++) {
        for (size_t address = 0u; address < sizeof addresses / sizeof addresses[0]; address++) {
            gpu_pgraph *pgraph = model(GPU_PGRAPH_OUTPUT_TEXTURE, true);
            CHECK(decode_one(pgraph, 0x1B08u + 0x40u * stage, addresses[address]) == GPU_PGRAPH_OK);
            CHECK(decode_one(pgraph, 0x1B14u + 0x40u * stage, 0x02022000u) == GPU_PGRAPH_OK);
            gpu_pgraph_output out;
            gpu_pgraph_report report = {0};
            gpu_pgraph_backend allowed = backend_with(GPU_PGRAPH_OUTPUT_TEXTURE,
                GPU_PGRAPH_INFER_OUTPUT_PINNED_STATE | GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING);
            CHECK(resolve(pgraph, &allowed, &out, &report) == GPU_PGRAPH_OK);
            CHECK((out.used_inferences & GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING) != 0u);
            gpu_pgraph_backend pinned_only = backend_with(GPU_PGRAPH_OUTPUT_TEXTURE, GPU_PGRAPH_INFER_OUTPUT_PINNED_STATE);
            CHECK(resolve(pgraph, &pinned_only, &out, &report) == GPU_PGRAPH_ERR_UNMEASURED);
            CHECK(strstr(report.error, "0x02022000") != NULL && strstr(report.error, "INFERRED") != NULL);
            const uint32_t unknown[] = {0x02012000u, 0x02042000u, 0x02022001u, 0x01022000u, 0x12022000u, 0x02024000u};
            for (size_t index = 0u; index < sizeof unknown / sizeof unknown[0]; index++) {
                CHECK(decode_one(pgraph, 0x1B14u + 0x40u * stage, unknown[index]) == GPU_PGRAPH_OK);
                CHECK(resolve(pgraph, &allowed, &out, &report) == GPU_PGRAPH_ERR_UNMEASURED);
                CHECK(strstr(report.error, "filter") != NULL);
            }
            gpu_pgraph_destroy(pgraph);
        }
    }
}

/* T1488: the title's U wrap, V clamp pair 0x301 (and 0x103) is admitted only with the texture sampling inference. */
static void test_t1266_mixed_wrap_clamp_address(void)
{
    const uint32_t addresses[] = {0x00000301u, 0x00000103u};
    for (size_t i = 0u; i < sizeof addresses / sizeof addresses[0]; i++) {
        gpu_pgraph *pgraph = model(GPU_PGRAPH_OUTPUT_TEXTURE, true);
        CHECK(decode_one(pgraph, 0x1B08u, addresses[i]) == GPU_PGRAPH_OK);
        gpu_pgraph_output out;
        gpu_pgraph_report report = {0};
        gpu_pgraph_backend allowed = backend_with(GPU_PGRAPH_OUTPUT_TEXTURE,
            GPU_PGRAPH_INFER_OUTPUT_PINNED_STATE | GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING);
        CHECK(resolve(pgraph, &allowed, &out, &report) == GPU_PGRAPH_OK);
        CHECK((out.used_inferences & GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING) != 0u);
        gpu_pgraph_backend pinned_only = backend_with(GPU_PGRAPH_OUTPUT_TEXTURE, GPU_PGRAPH_INFER_OUTPUT_PINNED_STATE);
        CHECK(resolve(pgraph, &pinned_only, &out, &report) == GPU_PGRAPH_ERR_UNMEASURED);
        gpu_pgraph_destroy(pgraph);
    }
}

/* T927: texture permission does not replace the independent output-state permission. */
static void test_t927_lod0_filter_requires_output_permission(void)
{
    gpu_pgraph *pgraph = model(GPU_PGRAPH_OUTPUT_TEXTURE, true);
    CHECK(decode_one(pgraph, 0x1B08u, 0x00000303u) == GPU_PGRAPH_OK);
    CHECK(decode_one(pgraph, 0x1B14u, 0x02022000u) == GPU_PGRAPH_OK);
    gpu_pgraph_output out;
    gpu_pgraph_report report = {0};
    gpu_pgraph_backend texture_only = backend_with(
        GPU_PGRAPH_OUTPUT_TEXTURE, GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING);
    CHECK(resolve(pgraph, &texture_only, &out, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "surface, fixed-function and texture stage words") != NULL);
    CHECK(strstr(report.error, "INFERRED and not allowed") != NULL);
    gpu_pgraph_destroy(pgraph);
}

/* --- immediate vertices --------------------------------------------------------------------- */

static uint32_t bits(float value)
{
    uint32_t out;
    memcpy(&out, &value, sizeof out);
    return out;
}

static void vertex(stream *s, float texture_x, float texture_y, float x, float y)
{
    add(s, 0x18C8u, bits(texture_x)); /* slot 9, the texture coordinate */
    add(s, 0x18CCu, bits(texture_y));
    add(s, 0x1880u, bits(x)); /* slot 0, the position: this write emits the vertex */
    add(s, 0x1884u, bits(y));
}

/* The composition's triangle: (0, 0), (4W, 0), (0, 4H) with the texture coordinate equal to the position. */
static void composition_triangle(stream *s)
{
    add(s, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_TRIANGLES);
    vertex(s, 0.0f, 0.0f, 0.0f, 0.0f);
    vertex(s, 2560.0f, 0.0f, 2560.0f, 0.0f);
    vertex(s, 0.0f, 1920.0f, 0.0f, 1920.0f);
    add(s, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_END);
}

static float float_at(const uint8_t *bytes, uint32_t index)
{
    float value;
    memcpy(&value, bytes + index * 4u, sizeof value);
    return value;
}

static void test_immediate_draw(void)
{
    printf("test_immediate_draw\n");
    gpu_pgraph *pgraph = model(GPU_PGRAPH_OUTPUT_IMMEDIATE, true);
    stream s = {0};
    composition_triangle(&s);
    CHECK(s.count == 14u);
    CHECK(gpu_pgraph_decode(pgraph, s.items, s.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_count(pgraph) == 1u);
    const gpu_pgraph_draw *draw = gpu_pgraph_draw_at(pgraph, 0u);
    CHECK(draw != NULL && draw->inline_vertices && draw->vertices_captured);
    if (draw == NULL) {
        gpu_pgraph_destroy(pgraph);
        return;
    }
    CHECK(draw->primitive == GPU_PGRAPH_OP_TRIANGLES && draw->index_count == 3u);
    const uint32_t *indices = gpu_pgraph_indices(pgraph) + draw->first_index;
    CHECK(indices[0] == 0u && indices[1] == 1u && indices[2] == 2u);
    const gpu_pgraph_stats stats = gpu_pgraph_get_stats(pgraph);
    CHECK(stats.inline_vertices == 3u && stats.inline_draws == 1u && stats.draws == 1u && stats.pairs_handled == 14u);
    /* slots 0 and 9 are 2-component float arrays of 3 vertices, every other slot is disabled */
    for (uint32_t slot = 0u; slot < GPU_PGRAPH_ATTRIBUTES; slot++) {
        const gpu_pgraph_format format = gpu_pgraph_decode_format(draw->arrays[slot].format);
        if (slot == 0u || slot == 9u) {
            CHECK(draw->arrays[slot].format_set && draw->arrays[slot].address_set);
            CHECK(format.type == GPU_PGRAPH_TYPE_F && format.size == 2u && format.stride == 8u);
            CHECK(draw->vertices[slot].captured && draw->vertices[slot].bytes == 24u);
            uint32_t address = 99u;
            uint32_t length = 0u;
            const uint8_t *bytes = gpu_pgraph_draw_vertex_bytes(pgraph, 0u, slot, &address, &length);
            CHECK(bytes != NULL && address == 0u && length == 24u);
            if (bytes != NULL) {
                CHECK(float_at(bytes, 0u) == 0.0f && float_at(bytes, 1u) == 0.0f);
                CHECK(float_at(bytes, 2u) == 2560.0f && float_at(bytes, 3u) == 0.0f);
                CHECK(float_at(bytes, 4u) == 0.0f && float_at(bytes, 5u) == 1920.0f);
            }
        } else {
            CHECK(draw->arrays[slot].format_set && format.size == 0u && !draw->vertices[slot].captured);
            CHECK(gpu_pgraph_draw_vertex_bytes(pgraph, 0u, slot, NULL, NULL) == NULL);
        }
    }
    /* the replay's assembly turns it into the (x, y, 0, 1) vertices of the program's v0 and v9 */
    gpu_pgraph_backend backend = backend_with(GPU_PGRAPH_OUTPUT_IMMEDIATE,
                                              GPU_PGRAPH_INFER_COMPONENT_DEFAULTS | GPU_PGRAPH_INFER_OUTPUT_IMMEDIATE_VERTEX);
    gpu_pgraph_assembled assembled;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &assembled, &report) == GPU_PGRAPH_OK);
    CHECK(assembled.vertex_count == 3u && assembled.topology == GPU_VSH_TOPOLOGY_TRIANGLE_LIST);
    CHECK((assembled.used_inferences & GPU_PGRAPH_INFER_OUTPUT_IMMEDIATE_VERTEX) != 0u);
    if (assembled.attributes != NULL && assembled.vertex_count == 3u) {
        const float *second = assembled.attributes + 1u * GPU_VSH_ATTRIBUTE_FLOATS;
        CHECK(second[0] == 2560.0f && second[1] == 0.0f && second[2] == 0.0f && second[3] == 1.0f); /* v0 */
        CHECK(second[9 * 4] == 2560.0f && second[9 * 4 + 1] == 0.0f && second[9 * 4 + 3] == 1.0f); /* v9 */
        const float *third = assembled.attributes + 2u * GPU_VSH_ATTRIBUTE_FLOATS;
        CHECK(third[1] == 1920.0f && third[9 * 4 + 1] == 1920.0f);
        CHECK(second[4] == 0.0f && second[7] == 1.0f); /* a disabled slot: (0, 0, 0, 1) */
    }
    gpu_pgraph_assembled_free(&assembled);
    /* without the group in the backend, or without the inference, the draw is refused naming it */
    gpu_pgraph_backend no_group = backend_with(0u, GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_OUTPUT_ALL);
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &no_group, &assembled, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "immediate group") != NULL);
    gpu_pgraph_backend no_inference = backend_with(GPU_PGRAPH_OUTPUT_IMMEDIATE, GPU_PGRAPH_INFER_ALL);
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &no_inference, &assembled, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "immediate vertex") != NULL);
    /* the latched values persist: a second bracket that writes only the position reuses slot 9 */
    CHECK(gpu_pgraph_begin_frame(pgraph) == GPU_PGRAPH_OK && gpu_pgraph_draw_count(pgraph) == 0u);
    stream second_bracket = {0};
    add(&second_bracket, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_TRIANGLES);
    for (uint32_t i = 0u; i < 3u; i++) {
        add(&second_bracket, 0x1880u, bits((float)i));
        add(&second_bracket, 0x1884u, bits(1.0f));
    }
    add(&second_bracket, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_END);
    CHECK(gpu_pgraph_decode(pgraph, second_bracket.items, second_bracket.count) == GPU_PGRAPH_OK);
    draw = gpu_pgraph_draw_at(pgraph, 0u);
    CHECK(draw != NULL && draw->inline_vertices && draw->vertices[9].captured && draw->vertices[0].captured);
    const uint8_t *tc = gpu_pgraph_draw_vertex_bytes(pgraph, 0u, 9u, NULL, NULL);
    CHECK(tc != NULL && float_at(tc, 4u) == 0.0f && float_at(tc, 5u) == 1920.0f); /* the last texcoord written */
    /* gpu_pgraph_reset forgets the latched attributes: the position alone makes a draw with no texture coordinate slot */
    gpu_pgraph_reset(pgraph);
    CHECK(decode_one(pgraph, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_TRIANGLES) == GPU_PGRAPH_OK);
    for (uint32_t i = 0u; i < 3u; i++) {
        CHECK(decode_one(pgraph, 0x1880u, bits((float)i)) == GPU_PGRAPH_OK);
        CHECK(decode_one(pgraph, 0x1884u, 0u) == GPU_PGRAPH_OK);
    }
    CHECK(decode_one(pgraph, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_END) == GPU_PGRAPH_OK);
    draw = gpu_pgraph_draw_at(pgraph, 0u);
    CHECK(draw != NULL && draw->vertices[0].captured && !draw->vertices[9].captured);
    gpu_pgraph_destroy(pgraph);
}

static void test_immediate_refusals(void)
{
    printf("test_immediate_refusals\n");
    /* group off: the four methods are unhandled as before, strict refuses */
    gpu_pgraph *strict = model(GPU_PGRAPH_OUTPUT_ALL_MEASURED & ~GPU_PGRAPH_OUTPUT_IMMEDIATE, true);
    CHECK(decode_one(strict, 0x1880u, 0u) == GPU_PGRAPH_ERR_UNMEASURED);
    gpu_pgraph_destroy(strict);
    gpu_pgraph *lenient = model(0u, false);
    stream s = {0};
    composition_triangle(&s);
    CHECK(gpu_pgraph_decode(lenient, s.items, s.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_unhandled_count(lenient) == 4u && gpu_pgraph_draw_count(lenient) == 0u);
    CHECK(gpu_pgraph_get_stats(lenient).empty_brackets == 1u);
    gpu_pgraph_destroy(lenient);

    gpu_pgraph *outside = model(GPU_PGRAPH_OUTPUT_IMMEDIATE, false);
    CHECK(decode_one(outside, 0x18C8u, 0u) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(gpu_pgraph_error(outside), "outside a BEGIN_END bracket") != NULL);
    gpu_pgraph_destroy(outside);

    gpu_pgraph *no_x = model(GPU_PGRAPH_OUTPUT_IMMEDIATE, false);
    CHECK(decode_one(no_x, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_TRIANGLES) == GPU_PGRAPH_OK);
    CHECK(decode_one(no_x, 0x18CCu, 0u) == GPU_PGRAPH_ERR_MALFORMED);
    CHECK(strstr(gpu_pgraph_error(no_x), "no x word") != NULL);
    gpu_pgraph_destroy(no_x);

    /* a slot first written after the first vertex */
    gpu_pgraph *late = model(GPU_PGRAPH_OUTPUT_IMMEDIATE, false);
    stream late_stream = {0};
    add(&late_stream, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_TRIANGLES);
    add(&late_stream, 0x1880u, 0u);
    add(&late_stream, 0x1884u, 0u);
    CHECK(gpu_pgraph_decode(late, late_stream.items, late_stream.count) == GPU_PGRAPH_OK);
    CHECK(decode_one(late, 0x18C8u, 0u) == GPU_PGRAPH_OK && decode_one(late, 0x18CCu, 0u) == GPU_PGRAPH_OK);
    CHECK(decode_one(late, 0x1880u, 0u) == GPU_PGRAPH_OK);
    CHECK(decode_one(late, 0x1884u, 0u) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(gpu_pgraph_error(late), "first written after the first vertex") != NULL);
    gpu_pgraph_destroy(late);

    /* array vertices and immediate vertices never share a bracket, in either order */
    gpu_pgraph *array_first = model(GPU_PGRAPH_OUTPUT_IMMEDIATE, false);
    CHECK(decode_one(array_first, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_TRIANGLES) == GPU_PGRAPH_OK);
    CHECK(decode_one(array_first, GPU_PGRAPH_DRAW_ARRAYS, (2u << 24)) == GPU_PGRAPH_OK);
    CHECK(decode_one(array_first, 0x1880u, 0u) == GPU_PGRAPH_OK);
    CHECK(decode_one(array_first, 0x1884u, 0u) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(gpu_pgraph_error(array_first), "array vertices") != NULL);
    gpu_pgraph_destroy(array_first);
    gpu_pgraph *inline_first = model(GPU_PGRAPH_OUTPUT_IMMEDIATE, false);
    CHECK(decode_one(inline_first, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_TRIANGLES) == GPU_PGRAPH_OK);
    CHECK(decode_one(inline_first, 0x1880u, 0u) == GPU_PGRAPH_OK && decode_one(inline_first, 0x1884u, 0u) == GPU_PGRAPH_OK);
    CHECK(decode_one(inline_first, GPU_PGRAPH_DRAW_ARRAYS, (2u << 24)) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(gpu_pgraph_error(inline_first), "immediate vertices") != NULL);
    gpu_pgraph_destroy(inline_first);

    /* the vertex bound */
    gpu_pgraph *bound = model(GPU_PGRAPH_OUTPUT_IMMEDIATE, false);
    CHECK(decode_one(bound, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_POINTS) == GPU_PGRAPH_OK);
    gpu_pgraph_result last = GPU_PGRAPH_OK;
    uint32_t emitted = 0u;
    while (last == GPU_PGRAPH_OK && emitted < GPU_PGRAPH_MAX_INLINE_VERTICES + 2u) {
        last = decode_one(bound, 0x1880u, 0u);
        if (last == GPU_PGRAPH_OK) {
            last = decode_one(bound, 0x1884u, 0u);
        }
        emitted++;
    }
    CHECK(last == GPU_PGRAPH_ERR_FULL && emitted == GPU_PGRAPH_MAX_INLINE_VERTICES + 1u);
    gpu_pgraph_destroy(bound);

    /* a bracket whose attributes never wrote the position is an empty bracket, not a draw */
    gpu_pgraph *texture_only = model(GPU_PGRAPH_OUTPUT_IMMEDIATE, true);
    stream t = {0};
    add(&t, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_TRIANGLES);
    add(&t, 0x18C8u, 0u);
    add(&t, 0x18CCu, 0u);
    add(&t, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_END);
    CHECK(gpu_pgraph_decode(texture_only, t.items, t.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_count(texture_only) == 0u && gpu_pgraph_get_stats(texture_only).empty_brackets == 1u);
    gpu_pgraph_destroy(texture_only);
}

/* --- the unwritten varying ------------------------------------------------------------------ */

/* A fragment module by hand: `layout(location = 0) in vec4 a; layout(location = 3) in vec4 b; out vec4 c = a + b;`. */
static size_t build_module(uint32_t *w, bool access_chain)
{
    size_t n = 0u;
#define OP(count, code) (w[n++] = ((uint32_t)(count) << 16) | (code))
    w[n++] = 0x07230203u;
    w[n++] = 0x00010000u;
    w[n++] = 0u;
    w[n++] = 16u; /* bound */
    w[n++] = 0u;
    OP(2, 17); w[n++] = 1u;                                                       /* Capability Shader */
    OP(3, 14); w[n++] = 0u; w[n++] = 1u;                                          /* MemoryModel Logical GLSL450 */
    OP(8, 15);                                                                    /* EntryPoint Fragment %main "main" %a %b %c */
    w[n++] = 4u; w[n++] = 4u; w[n++] = 0x6E69616Du; w[n++] = 0u;
    w[n++] = 5u; w[n++] = 6u; w[n++] = 7u;
    OP(3, 16); w[n++] = 4u; w[n++] = 7u;                                          /* ExecutionMode OriginUpperLeft */
    OP(4, 71); w[n++] = 5u; w[n++] = 30u; w[n++] = 0u;                            /* a Location 0 */
    OP(4, 71); w[n++] = 6u; w[n++] = 30u; w[n++] = 3u;                            /* b Location 3 */
    OP(4, 71); w[n++] = 7u; w[n++] = 30u; w[n++] = 0u;                            /* c Location 0 (an Output) */
    OP(2, 19); w[n++] = 2u;                                                       /* %void */
    OP(3, 33); w[n++] = 3u; w[n++] = 2u;                                          /* %fn */
    OP(3, 22); w[n++] = 8u; w[n++] = 32u;                                         /* %float */
    OP(4, 23); w[n++] = 9u; w[n++] = 8u; w[n++] = 4u;                             /* %v4 */
    OP(4, 32); w[n++] = 10u; w[n++] = 1u; w[n++] = 9u;                            /* ptr Input v4 */
    OP(4, 32); w[n++] = 11u; w[n++] = 3u; w[n++] = 9u;                            /* ptr Output v4 */
    OP(4, 59); w[n++] = 10u; w[n++] = 5u; w[n++] = 1u;                            /* a */
    OP(4, 59); w[n++] = 10u; w[n++] = 6u; w[n++] = 1u;                            /* b */
    OP(4, 59); w[n++] = 11u; w[n++] = 7u; w[n++] = 3u;                            /* c */
    OP(5, 54); w[n++] = 2u; w[n++] = 4u; w[n++] = 0u; w[n++] = 3u;                /* Function %main */
    OP(2, 248); w[n++] = 12u;                                                     /* Label */
    OP(4, 61); w[n++] = 9u; w[n++] = 13u; w[n++] = 5u;                            /* %13 = Load a */
    if (access_chain) {
        OP(5, 65); w[n++] = 10u; w[n++] = 14u; w[n++] = 5u; w[n++] = 5u;          /* AccessChain on a: unsupported */
    }
    OP(3, 62); w[n++] = 7u; w[n++] = 13u;                                         /* Store c */
    OP(1, 253);                                                                   /* Return */
    OP(1, 56);                                                                    /* FunctionEnd */
#undef OP
    return n;
}

static void test_default_inputs(void)
{
    printf("test_default_inputs\n");
    uint32_t module[128];
    size_t length = build_module(module, false);
    uint32_t mask = 0u;
    CHECK(gpu_spirv_interface_locations(module, length, 1u, &mask) && mask == 0x9u); /* a at 0, b at 3 */
    uint32_t *patched = NULL;
    size_t patched_length = 0u;
    /* location 0 is also the OUTPUT's number: only the Input variable is rewritten */
    CHECK(gpu_spirv_default_inputs(module, length, 0x1u, &patched, &patched_length));
    CHECK(patched != NULL && patched_length > length);
    if (patched != NULL) {
        CHECK(gpu_spirv_interface_locations(patched, patched_length, 1u, &mask) && mask == 0x8u); /* only b is left */
        CHECK(gpu_spirv_interface_locations(patched, patched_length, 3u, &mask) && mask == 0x1u); /* the output stays */
        CHECK(patched[3] > module[3]);                                                            /* ids were added */
        size_t private_variables = 0u;
        size_t composites = 0u;
        for (size_t at = 5u; at < patched_length; at += patched[at] >> 16) {
            const uint32_t opcode = patched[at] & 0xFFFFu;
            if (opcode == 59u && patched[at + 3u] == 6u) {
                private_variables++;
                CHECK((patched[at] >> 16) == 5u && patched[at + 2u] == 5u); /* a with an initialiser */
            }
            if (opcode == 44u) {
                composites++;
                CHECK(patched[at + 6u] != patched[at + 5u]); /* the last component (1.0) is not the zeros */
            }
            CHECK(!(opcode == 71u && patched[at + 1u] == 5u)); /* no decoration is left on the rewritten input */
        }
        CHECK(private_variables == 1u && composites == 1u);
        for (size_t at = 5u; at < patched_length; at += patched[at] >> 16) {
            if ((patched[at] & 0xFFFFu) == 59u && patched[at + 3u] == 6u) {
                bool pointer_is_private = false;
                for (size_t other = 5u; other < patched_length; other += patched[other] >> 16) {
                    if ((patched[other] & 0xFFFFu) == 32u && patched[other + 1u] == patched[at + 1u]) {
                        pointer_is_private = patched[other + 2u] == 6u;
                    }
                }
                CHECK(pointer_is_private); /* the variable's pointer type is a Private pointer too */
            }
        }
        /* every id a type, constant or variable defines is defined once and below the bound, and the module holds the
         * two float constants 0.0 and 1.0 the composite is made of */
        uint32_t seen[64];
        size_t seen_count = 0u;
        bool has_zero = false;
        bool has_one = false;
        for (size_t at = 5u; at < patched_length; at += patched[at] >> 16) {
            const uint32_t opcode = patched[at] & 0xFFFFu;
            const bool types = opcode == 19u || opcode == 22u || opcode == 23u || opcode == 32u || opcode == 33u;
            const bool values = opcode == 43u || opcode == 44u || opcode == 59u;
            if (types || values) {
                const uint32_t id = patched[at + (types ? 1u : 2u)];
                CHECK(id < patched[3]);
                for (size_t i = 0u; i < seen_count; i++) {
                    CHECK(seen[i] != id);
                }
                CHECK(seen_count < 64u);
                seen[seen_count++] = id;
            }
            if (opcode == 43u && (patched[at] >> 16) == 4u) {
                has_zero = has_zero || patched[at + 3u] == 0x00000000u;
                has_one = has_one || patched[at + 3u] == 0x3F800000u;
            }
        }
        CHECK(has_zero && has_one);
        /* the entry point no longer names the rewritten variable (before SPIR-V 1.4) */
        for (size_t at = 5u; at < patched_length; at += patched[at] >> 16) {
            if ((patched[at] & 0xFFFFu) == 15u) {
                CHECK((patched[at] >> 16) == 7u); /* 8 words minus the one removed */
                CHECK(patched[at + 5u] == 6u && patched[at + 6u] == 7u);
            }
        }
    }
    free(patched);
    /* both inputs at once */
    patched = NULL;
    CHECK(gpu_spirv_default_inputs(module, length, 0x9u, &patched, &patched_length));
    if (patched != NULL) {
        CHECK(gpu_spirv_interface_locations(patched, patched_length, 1u, &mask) && mask == 0u);
    }
    free(patched);
    /* refusals: nothing matches, a location no input has, an access chain on the input, not a module */
    patched = NULL;
    CHECK(!gpu_spirv_default_inputs(module, length, 0x2u, &patched, &patched_length) && patched == NULL);
    CHECK(!gpu_spirv_default_inputs(module, length, 0u, &patched, &patched_length));
    uint32_t chain[128];
    const size_t chain_length = build_module(chain, true);
    CHECK(!gpu_spirv_default_inputs(chain, chain_length, 0x1u, &patched, &patched_length) && patched == NULL);
    module[0] = 0u;
    CHECK(!gpu_spirv_default_inputs(module, length, 0x1u, &patched, &patched_length));
}

/* --- T578: the 2D engine blit CopyRects emits ---------------------------------------------------------------------- */

/* The words of the retail boot's CopyRects(GetBackBuffer(0), NULL, 0, GetBackBuffer(-1), NULL), as the recorder saw them: the
 * state packet 0x86308 (source and destination Data words), 0x86300 (colour format 0xA, both pitches 2560) and the blit packet
 * 0xC4300 (points (0, 0) and (0, 0), size 640 x 480). Subchannel 3 is the context surfaces 2D object, 2 the image blit. The two
 * Data words and the pair numbers 162334 to 162340 are the ones MEASURED in the retail boot (T578, `--profile-calls` with the
 * bridge lift, the owner's teardown at 0x23230): back buffer 0x0374000, front buffer 0x04A0000. */
#define BOOT_BACK_DATA 0x00374000u
#define BOOT_FRONT_DATA 0x004A0000u

static gpu_pgraph_result other(gpu_pgraph *pgraph, uint32_t subchannel, uint32_t method, uint32_t data)
{
    return gpu_pgraph_decode_other_subchannel(pgraph, subchannel, method, data);
}

static void boot_blit_state(gpu_pgraph *pgraph, uint32_t source, uint32_t destination)
{
    CHECK(other(pgraph, 3u, 0x308u, source) == GPU_PGRAPH_OK);
    CHECK(other(pgraph, 3u, 0x30Cu, destination) == GPU_PGRAPH_OK);
    CHECK(other(pgraph, 3u, 0x300u, 0xAu) == GPU_PGRAPH_OK);
    CHECK(other(pgraph, 3u, 0x304u, 0x0A000A00u) == GPU_PGRAPH_OK);
}

static void test_blit_group(void)
{
    printf("test_blit_group\n");
    /* Group off (the default, and every other group on): the first packet word is refused as before, naming the subchannel. */
    gpu_pgraph *off = model(GPU_PGRAPH_OUTPUT_ALL_MEASURED & ~GPU_PGRAPH_OUTPUT_BLIT, true);
    CHECK(other(off, 3u, 0x308u, BOOT_BACK_DATA) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(gpu_pgraph_error(off), "subchannel 3") != NULL &&
          strstr(gpu_pgraph_error(off), "only the 3D subchannel 0 is decoded") != NULL);
    CHECK(other(off, 2u, 0x308u, 0x01E00280u) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(gpu_pgraph_get_stats(off).copies == 0u && gpu_pgraph_copy_count(off) == 0u);
    gpu_pgraph_destroy(off);
    gpu_pgraph *none = model(0u, false);
    CHECK(other(none, 3u, 0x300u, 0xAu) == GPU_PGRAPH_ERR_UNMEASURED);
    gpu_pgraph_destroy(none);
    CHECK((GPU_PGRAPH_OUTPUT_ALL_MEASURED & GPU_PGRAPH_OUTPUT_BLIT) != 0u && GPU_PGRAPH_OUTPUT_BLIT == 0x2000u);
    CHECK((GPU_PGRAPH_INFER_OUTPUT_ALL & GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL) != 0u);

    /* The boot's packet, word for word. */
    gpu_pgraph *pgraph = model(GPU_PGRAPH_OUTPUT_BLIT, true);
    boot_blit_state(pgraph, BOOT_BACK_DATA, BOOT_FRONT_DATA);
    CHECK(gpu_pgraph_copy_count(pgraph) == 0u); /* the state words alone run nothing */
    CHECK(other(pgraph, 2u, 0x300u, 0u) == GPU_PGRAPH_OK);
    CHECK(other(pgraph, 2u, 0x304u, 0u) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_copy_count(pgraph) == 0u); /* the points alone run nothing: the SIZE write does */
    CHECK(other(pgraph, 2u, 0x308u, 0x01E00280u) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_copy_count(pgraph) == 1u);
    const gpu_pgraph_copy *copy = gpu_pgraph_copy_at(pgraph, 0u);
    CHECK(copy != NULL);
    if (copy != NULL) {
        CHECK(copy->source_offset == BOOT_BACK_DATA && copy->destination_offset == BOOT_FRONT_DATA);
        CHECK(copy->color_format == 0xAu && copy->source_pitch == 2560u && copy->destination_pitch == 2560u);
        CHECK(copy->in_x == 0u && copy->in_y == 0u && copy->out_x == 0u && copy->out_y == 0u);
        CHECK(copy->width == 640u && copy->height == 480u && copy->operation == 3u);
        CHECK(copy->before_draw == 0u && copy->before_clear == 0u && copy->command == 6u);
    }
    CHECK(gpu_pgraph_copy_at(pgraph, 1u) == NULL);
    gpu_pgraph_stats stats = gpu_pgraph_get_stats(pgraph);
    CHECK(stats.copies == 1u && stats.copies_empty == 0u && stats.blit_state_pairs == 7u);
    CHECK(stats.pairs == 7u && stats.pairs_handled == 7u);
    /* the rectangle moves, the state words persist across a frame, the list does not */
    CHECK(gpu_pgraph_begin_frame(pgraph) == GPU_PGRAPH_OK && gpu_pgraph_copy_count(pgraph) == 0u);
    CHECK(other(pgraph, 2u, 0x300u, (4u << 16) | 3u) == GPU_PGRAPH_OK);
    CHECK(other(pgraph, 2u, 0x304u, (20u << 16) | 10u) == GPU_PGRAPH_OK);
    CHECK(other(pgraph, 2u, 0x308u, (7u << 16) | 5u) == GPU_PGRAPH_OK);
    copy = gpu_pgraph_copy_at(pgraph, 0u);
    CHECK(gpu_pgraph_copy_count(pgraph) == 1u && copy != NULL);
    if (copy != NULL) {
        CHECK(copy->in_x == 3u && copy->in_y == 4u && copy->out_x == 10u && copy->out_y == 20u);
        CHECK(copy->width == 5u && copy->height == 7u && copy->color_format == 0xAu && copy->source_pitch == 2560u);
        CHECK(copy->source_offset == BOOT_BACK_DATA && copy->command == 9u);
    }
    /* a zero width or height is kept as an event (nothing moves) and counted */
    CHECK(other(pgraph, 2u, 0x308u, 0x00000000u) == GPU_PGRAPH_OK && gpu_pgraph_copy_count(pgraph) == 2u);
    CHECK(other(pgraph, 2u, 0x308u, 0x00010000u) == GPU_PGRAPH_OK && gpu_pgraph_copy_count(pgraph) == 3u);
    stats = gpu_pgraph_get_stats(pgraph);
    CHECK(stats.copies == 4u && stats.copies_empty == 2u);
    /* a reset forgets the registers: the next blit has no state */
    gpu_pgraph_reset(pgraph);
    CHECK(gpu_pgraph_copy_count(pgraph) == 0u);
    CHECK(other(pgraph, 2u, 0x308u, 0x00010001u) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(gpu_pgraph_error(pgraph), "before the surface offsets") != NULL);
    gpu_pgraph_destroy(pgraph);
}

typedef struct {
    uint32_t subchannel;
    uint32_t method;
    uint32_t good;
    uint32_t bad;
    const char *name;
} init_word;

static void test_blit_init_words(void)
{
    printf("test_blit_init_words\n");
    /* The original CreateDevice init (0x003DA407 headers, classes pushed at 0x003DAEFE: handle 0x10 is class 0x9F on subchannel 2,
     * handle 0x11 is class 0x62 on subchannel 3), accepted at the measured values only. The port's CreateDevice does not write
     * them, so the recorded stream never holds them. */
    static const init_word words[] = {
        {2u, 0x0000u, 0x10u, 0x11u, "SET_OBJECT on subchannel 2"},
        {3u, 0x0000u, 0x11u, 0x10u, "SET_OBJECT on subchannel 3"},
        {3u, 0x0184u, 3u, 4u, "the source DMA context"},
        {3u, 0x0188u, 0xBu, 3u, "the destination DMA context"},
        {2u, 0x0184u, 0x19u, 0u, "a blit context"},
        {2u, 0x0188u, 0x19u, 0u, "a blit context"},
        {2u, 0x018Cu, 0x19u, 0u, "a blit context"},
        {2u, 0x0190u, 0x19u, 0u, "a blit context"},
        {2u, 0x0194u, 0x19u, 0u, "a blit context"},
        {2u, 0x0198u, 0x19u, 0u, "a blit context"},
        {2u, 0x019Cu, 0x11u, 0x10u, "the blit's surfaces context"},
        {2u, 0x02FCu, 3u, 2u, "the blit operation"},
    };
    for (size_t i = 0u; i < sizeof words / sizeof words[0]; i++) {
        gpu_pgraph *pgraph = model(GPU_PGRAPH_OUTPUT_BLIT, true);
        CHECK(other(pgraph, words[i].subchannel, words[i].method, words[i].bad) == GPU_PGRAPH_ERR_UNMEASURED);
        CHECK(strstr(gpu_pgraph_error(pgraph), words[i].name) != NULL && strstr(gpu_pgraph_error(pgraph), "CreateDevice") != NULL);
        CHECK(gpu_pgraph_get_stats(pgraph).pairs == 0u);
        CHECK(other(pgraph, words[i].subchannel, words[i].method, words[i].good) == GPU_PGRAPH_OK);
        CHECK(gpu_pgraph_get_stats(pgraph).pairs == 1u && gpu_pgraph_get_stats(pgraph).blit_state_pairs == 1u);
        gpu_pgraph_destroy(pgraph);
    }
    /* every other method of the two classes is refused, subchannels 1 and 5 stay refused (the fence's 0x310 aside) */
    gpu_pgraph *pgraph = model(GPU_PGRAPH_OUTPUT_ALL_MEASURED, true);
    static const uint32_t unknown_3[] = {0x0004u, 0x0180u, 0x018Cu, 0x0310u, 0x02FCu, 0x0314u};
    for (size_t i = 0u; i < sizeof unknown_3 / sizeof unknown_3[0]; i++) {
        CHECK(other(pgraph, 3u, unknown_3[i], 0u) == GPU_PGRAPH_ERR_UNMEASURED);
        CHECK(strstr(gpu_pgraph_error(pgraph), "subchannel 3 (context surfaces 2D, class 0x62)") != NULL);
    }
    static const uint32_t unknown_2[] = {0x0180u, 0x01A0u, 0x02F8u, 0x0310u, 0x030Cu, 0x0314u};
    for (size_t i = 0u; i < sizeof unknown_2 / sizeof unknown_2[0]; i++) {
        CHECK(other(pgraph, 2u, unknown_2[i], 0u) == GPU_PGRAPH_ERR_UNMEASURED);
        CHECK(strstr(gpu_pgraph_error(pgraph), "subchannel 2 (image blit, class 0x9F)") != NULL);
    }
    CHECK(other(pgraph, 1u, 0x0180u, 7u) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(gpu_pgraph_error(pgraph), "only the 3D subchannel 0 is decoded") != NULL);
    CHECK(other(pgraph, 4u, 0x0300u, 0u) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(other(pgraph, 5u, 0x0300u, 0u) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(other(pgraph, 5u, GPU_PGRAPH_SOFTWARE_FENCE_NOTIFY, 0x1234u) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_get_stats(pgraph).software_methods == 1u);
    gpu_pgraph_destroy(pgraph);
}

static void test_blit_refusals(void)
{
    printf("test_blit_refusals\n");
    /* each state word missing: the SIZE write is refused naming the missing state */
    for (uint32_t skip = 0u; skip < 6u; skip++) {
        gpu_pgraph *pgraph = model(GPU_PGRAPH_OUTPUT_BLIT, true);
        static const struct {
            uint32_t subchannel;
            uint32_t method;
            uint32_t data;
        } writes[6] = {
            {3u, 0x308u, 0x00100000u}, {3u, 0x30Cu, 0x00200000u}, {3u, 0x300u, 0xAu},
            {3u, 0x304u, 0x0A000A00u}, {2u, 0x300u, 0u}, {2u, 0x304u, 0u},
        };
        for (uint32_t i = 0u; i < 6u; i++) {
            if (i != skip) {
                CHECK(other(pgraph, writes[i].subchannel, writes[i].method, writes[i].data) == GPU_PGRAPH_OK);
            }
        }
        CHECK(other(pgraph, 2u, 0x308u, 0x00010001u) == GPU_PGRAPH_ERR_UNMEASURED);
        CHECK(strstr(gpu_pgraph_error(pgraph), "before the surface offsets") != NULL);
        CHECK(gpu_pgraph_copy_count(pgraph) == 0u);
        gpu_pgraph_destroy(pgraph);
    }
    /* formats: only 1, 4 and 0xA are decoded */
    static const uint32_t refused_formats[] = {0u, 2u, 3u, 5u, 6u, 7u, 8u, 9u, 0xBu, 0xCu, 0x10u, 0xAAu};
    for (size_t i = 0u; i < sizeof refused_formats / sizeof refused_formats[0]; i++) {
        gpu_pgraph *pgraph = model(GPU_PGRAPH_OUTPUT_BLIT, true);
        boot_blit_state(pgraph, 0x00100000u, 0x00200000u);
        CHECK(other(pgraph, 3u, 0x300u, refused_formats[i]) == GPU_PGRAPH_OK);
        CHECK(other(pgraph, 2u, 0x300u, 0u) == GPU_PGRAPH_OK && other(pgraph, 2u, 0x304u, 0u) == GPU_PGRAPH_OK);
        CHECK(other(pgraph, 2u, 0x308u, 0x00010001u) == GPU_PGRAPH_ERR_UNMEASURED);
        CHECK(strstr(gpu_pgraph_error(pgraph), "colour format") != NULL && gpu_pgraph_copy_count(pgraph) == 0u);
        gpu_pgraph_destroy(pgraph);
    }
    static const uint32_t decoded_formats[] = {1u, 4u, 0xAu};
    for (size_t i = 0u; i < sizeof decoded_formats / sizeof decoded_formats[0]; i++) {
        gpu_pgraph *pgraph = model(GPU_PGRAPH_OUTPUT_BLIT, true);
        boot_blit_state(pgraph, 0x00100000u, 0x00200000u);
        CHECK(other(pgraph, 3u, 0x300u, decoded_formats[i]) == GPU_PGRAPH_OK);
        CHECK(other(pgraph, 2u, 0x300u, 0u) == GPU_PGRAPH_OK && other(pgraph, 2u, 0x304u, 0u) == GPU_PGRAPH_OK);
        CHECK(other(pgraph, 2u, 0x308u, 0x00010001u) == GPU_PGRAPH_OK && gpu_pgraph_copy_count(pgraph) == 1u);
        gpu_pgraph_destroy(pgraph);
    }
    /* rectangles: pitch, row width and point range */
    struct {
        uint32_t pitch;
        uint32_t in;
        uint32_t out;
        uint32_t size;
        const char *what;
    } rects[] = {
        {0x0A000000u, 0u, 0u, 0x00010001u, "zero pitch"},
        {0x00000A00u, 0u, 0u, 0x00010001u, "zero pitch"},
        {0x0A000A00u, 0u, 0u, 0x00010281u, "wider than a pitch"},          /* 641 x 4 > 2560 */
        {0x0A000500u, 0u, 0u, 0x00010141u, "wider than a pitch"},          /* 321 x 4 > 1280 */
        {0x0A000A00u, 0xFFFFu, 0u, 0x00010002u, "65535"},                   /* in x 65535 + 2 */
        {0x0A000A00u, 0u, 0xFFFF0000u, 0x00020001u, "65535"},               /* out y 65535 + 2 */
        {0x0A000A00u, 0xFFFF0000u, 0u, 0x00020001u, "65535"},               /* in y 65535 + 2 */
        {0x0A000A00u, 0u, 0xFFFFu, 0x00010002u, "65535"},                   /* out x 65535 + 2 */
        {0x05000A00u, 0u, 0u, 0x00010141u, "wider than a pitch"},          /* 321 x 4 > 1280, destination */
    };
    for (size_t i = 0u; i < sizeof rects / sizeof rects[0]; i++) {
        gpu_pgraph *pgraph = model(GPU_PGRAPH_OUTPUT_BLIT, true);
        boot_blit_state(pgraph, 0x00100000u, 0x00200000u);
        CHECK(other(pgraph, 3u, 0x304u, rects[i].pitch) == GPU_PGRAPH_OK);
        CHECK(other(pgraph, 2u, 0x300u, rects[i].in) == GPU_PGRAPH_OK && other(pgraph, 2u, 0x304u, rects[i].out) == GPU_PGRAPH_OK);
        CHECK(other(pgraph, 2u, 0x308u, rects[i].size) == GPU_PGRAPH_ERR_UNMEASURED);
        CHECK(strstr(gpu_pgraph_error(pgraph), rects[i].what) != NULL && gpu_pgraph_copy_count(pgraph) == 0u);
        gpu_pgraph_destroy(pgraph);
    }
    /* an exact fit is fine: 640 pixels of 4 bytes on a 2560 pitch, and a 1 byte row of 2560 on the byte path */
    gpu_pgraph *fit = model(GPU_PGRAPH_OUTPUT_BLIT, true);
    boot_blit_state(fit, 0x00100000u, 0x00200000u);
    CHECK(other(fit, 2u, 0x300u, 0u) == GPU_PGRAPH_OK && other(fit, 2u, 0x304u, 0u) == GPU_PGRAPH_OK);
    CHECK(other(fit, 2u, 0x308u, 0x00010280u) == GPU_PGRAPH_OK && gpu_pgraph_copy_count(fit) == 1u);
    CHECK(other(fit, 3u, 0x300u, 1u) == GPU_PGRAPH_OK && other(fit, 2u, 0x308u, 0x00010A00u) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_copy_count(fit) == 2u);
    CHECK(other(fit, 3u, 0x300u, 4u) == GPU_PGRAPH_OK && other(fit, 2u, 0x308u, 0x00010500u) == GPU_PGRAPH_OK); /* 1280 x 2 */
    CHECK(gpu_pgraph_copy_count(fit) == 3u && gpu_pgraph_copy_at(fit, 2u)->width == 1280u);
    /* a width past 12 bits survives intact, a Y8 row of 18432 bytes on a 20480 pitch */
    CHECK(other(fit, 3u, 0x300u, 1u) == GPU_PGRAPH_OK && other(fit, 3u, 0x304u, 0x50005000u) == GPU_PGRAPH_OK);
    CHECK(other(fit, 2u, 0x308u, 0x00014800u) == GPU_PGRAPH_OK && gpu_pgraph_copy_count(fit) == 4u);
    CHECK(gpu_pgraph_copy_at(fit, 3u)->width == 0x4800u && gpu_pgraph_copy_at(fit, 3u)->height == 1u);
    /* an exact fit in the narrower pitch on each side, with different pitches, offsets and points: nothing is swapped */
    CHECK(other(fit, 3u, 0x300u, 0xAu) == GPU_PGRAPH_OK && other(fit, 3u, 0x304u, 0x08000500u) == GPU_PGRAPH_OK);
    CHECK(other(fit, 3u, 0x308u, 0x00123000u) == GPU_PGRAPH_OK && other(fit, 3u, 0x30Cu, 0x00456000u) == GPU_PGRAPH_OK);
    CHECK(other(fit, 2u, 0x300u, (9u << 16) | 8u) == GPU_PGRAPH_OK && other(fit, 2u, 0x304u, (12u << 16) | 11u) == GPU_PGRAPH_OK);
    CHECK(other(fit, 2u, 0x308u, (3u << 16) | 320u) == GPU_PGRAPH_OK && gpu_pgraph_copy_count(fit) == 5u);
    const gpu_pgraph_copy *odd = gpu_pgraph_copy_at(fit, 4u);
    CHECK(odd != NULL);
    if (odd != NULL) {
        CHECK(odd->source_pitch == 0x500u && odd->destination_pitch == 0x800u);
        CHECK(odd->source_offset == 0x00123000u && odd->destination_offset == 0x00456000u && odd->color_format == 0xAu);
        CHECK(odd->in_x == 8u && odd->in_y == 9u && odd->out_x == 11u && odd->out_y == 12u);
        CHECK(odd->width == 320u && odd->height == 3u);
    }
    CHECK(gpu_pgraph_copy_at(fit, 5u) == NULL);
    /* an offset past the 28 bit Data word */
    CHECK(other(fit, 3u, 0x308u, 0x10000000u) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(gpu_pgraph_error(fit), "28 bits") != NULL);
    CHECK(other(fit, 3u, 0x30Cu, 0x80000000u) == GPU_PGRAPH_ERR_UNMEASURED);
    gpu_pgraph_destroy(fit);
    /* inside a bracket */
    gpu_pgraph *bracket = model(GPU_PGRAPH_OUTPUT_BLIT, true);
    CHECK(decode_one(bracket, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_TRIANGLES) == GPU_PGRAPH_OK);
    CHECK(other(bracket, 3u, 0x308u, 0x00100000u) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(gpu_pgraph_error(bracket), "inside a BEGIN_END bracket") != NULL);
    gpu_pgraph_destroy(bracket);
    /* the list is bounded */
    gpu_pgraph *full = model(GPU_PGRAPH_OUTPUT_BLIT, true);
    boot_blit_state(full, 0x00100000u, 0x00200000u);
    CHECK(other(full, 2u, 0x300u, 0u) == GPU_PGRAPH_OK && other(full, 2u, 0x304u, 0u) == GPU_PGRAPH_OK);
    for (uint32_t i = 0u; i < GPU_PGRAPH_MAX_COPIES; i++) {
        CHECK(other(full, 2u, 0x308u, 0x00010001u) == GPU_PGRAPH_OK);
    }
    CHECK(gpu_pgraph_copy_count(full) == GPU_PGRAPH_MAX_COPIES);
    CHECK(other(full, 2u, 0x308u, 0x00010001u) == GPU_PGRAPH_ERR_FULL);
    gpu_pgraph_destroy(full);
}

/* A blit sits at its place among the draws and the clears: a bracket before it counts as a draw. */
static void test_blit_order(void)
{
    printf("test_blit_order\n");
    gpu_pgraph *pgraph = model(GPU_PGRAPH_OUTPUT_BLIT | GPU_PGRAPH_OUTPUT_IMMEDIATE | GPU_PGRAPH_OUTPUT_CLEAR, true);
    static const uint32_t two_vertices[][2] = {
        {GPU_PGRAPH_VERTEX_DATA2F + 8u * 9u, 0u}, {GPU_PGRAPH_VERTEX_DATA2F + 8u * 9u + 4u, 0u},
        {GPU_PGRAPH_VERTEX_DATA2F, 0u}, {GPU_PGRAPH_VERTEX_DATA2F + 4u, 0u},
    };
    CHECK(decode_one(pgraph, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_POINTS) == GPU_PGRAPH_OK);
    for (size_t i = 0u; i < 4u; i++) {
        CHECK(decode_one(pgraph, two_vertices[i][0], two_vertices[i][1]) == GPU_PGRAPH_OK);
    }
    CHECK(decode_one(pgraph, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_END) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_count(pgraph) == 1u);
    CHECK(decode_one(pgraph, GPU_PGRAPH_CLEAR_SURFACE, 0xF0u) == GPU_PGRAPH_OK && gpu_pgraph_clear_count(pgraph) == 1u);
    boot_blit_state(pgraph, 0x00100000u, 0x00200000u);
    CHECK(other(pgraph, 2u, 0x300u, 0u) == GPU_PGRAPH_OK && other(pgraph, 2u, 0x304u, 0u) == GPU_PGRAPH_OK);
    CHECK(other(pgraph, 2u, 0x308u, 0x00010001u) == GPU_PGRAPH_OK);
    const gpu_pgraph_copy *copy = gpu_pgraph_copy_at(pgraph, 0u);
    CHECK(copy != NULL && copy->before_draw == 1u && copy->before_clear == 1u);
    CHECK(copy != NULL && copy->command == gpu_pgraph_get_stats(pgraph).pairs - 1u);
    gpu_pgraph_destroy(pgraph);
}

/* --- the copy over two images (device free) -------------------------------------------------------------------------- */

static gpu_image make_image(uint32_t width, uint32_t height, uint8_t seed)
{
    gpu_image image;
    memset(&image, 0, sizeof image);
    image.width = width;
    image.height = height;
    image.stride_bytes = width * 4u;
    image.pixels = malloc((size_t)image.stride_bytes * height);
    CHECK(image.pixels != NULL);
    for (uint32_t y = 0u; y < height; y++) {
        for (uint32_t x = 0u; x < width; x++) {
            uint8_t *pixel = image.pixels + (size_t)y * image.stride_bytes + (size_t)x * 4u;
            pixel[0] = (uint8_t)(seed + x);
            pixel[1] = (uint8_t)(seed + y);
            pixel[2] = (uint8_t)(seed ^ (x * 7u + y * 3u));
            pixel[3] = 255u;
        }
    }
    return image;
}

static const uint8_t *pixel_at(const gpu_image *image, uint32_t x, uint32_t y)
{
    return image->pixels + (size_t)y * image->stride_bytes + (size_t)x * 4u;
}

static gpu_pgraph_copy make_copy(uint32_t width, uint32_t height, uint32_t in_x, uint32_t in_y, uint32_t out_x,
                                 uint32_t out_y, uint32_t source_pitch, uint32_t destination_pitch)
{
    gpu_pgraph_copy copy;
    memset(&copy, 0, sizeof copy);
    copy.source_offset = 0x00100000u;
    copy.destination_offset = 0x00200000u;
    copy.color_format = GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8;
    copy.source_pitch = source_pitch;
    copy.destination_pitch = destination_pitch;
    copy.in_x = in_x;
    copy.in_y = in_y;
    copy.out_x = out_x;
    copy.out_y = out_y;
    copy.width = width;
    copy.height = height;
    copy.operation = GPU_PGRAPH_BLIT_OPERATION_SRCCOPY;
    return copy;
}

static gpu_pgraph_backend blit_backend(uint32_t groups, uint32_t inferences)
{
    gpu_pgraph_backend backend;
    memset(&backend, 0, sizeof backend);
    backend.output_groups = groups;
    backend.allowed_inferences = inferences;
    return backend;
}

static void test_replay_copy(void)
{
    printf("test_replay_copy\n");
    gpu_image source = make_image(16u, 12u, 10u);
    gpu_image destination = make_image(20u, 14u, 99u);
    gpu_image untouched = make_image(20u, 14u, 99u);
    gpu_pgraph_backend backend = blit_backend(GPU_PGRAPH_OUTPUT_BLIT, GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL);
    gpu_pgraph_report report;
    uint32_t used = 0u;

    /* a rectangle from (3, 2) of the source to (5, 4) of the destination, row by row */
    gpu_pgraph_copy copy = make_copy(6u, 5u, 3u, 2u, 5u, 4u, 64u, 80u);
    memset(&report, 0, sizeof report);
    CHECK(gpu_pgraph_replay_copy(&copy, &backend, &source, &destination, &used, &report) == GPU_PGRAPH_OK);
    CHECK(used == GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL);
    uint32_t inside = 0u;
    uint32_t outside_changed = 0u;
    for (uint32_t y = 0u; y < destination.height; y++) {
        for (uint32_t x = 0u; x < destination.width; x++) {
            const bool in_rectangle = x >= 5u && x < 11u && y >= 4u && y < 9u;
            if (in_rectangle) {
                CHECK(memcmp(pixel_at(&destination, x, y), pixel_at(&source, x - 5u + 3u, y - 4u + 2u), 4u) == 0);
                inside++;
            } else if (memcmp(pixel_at(&destination, x, y), pixel_at(&untouched, x, y), 4u) != 0) {
                outside_changed++;
            }
        }
    }
    CHECK(inside == 30u && outside_changed == 0u); /* something was compared, and nothing outside moved */
    CHECK(memcmp(pixel_at(&destination, 5u, 4u), pixel_at(&untouched, 5u, 4u), 4u) != 0); /* it really changed */

    /* flip_y: the rows of both images are reversed, the rectangle rows follow */
    gpu_image flipped = make_image(20u, 14u, 99u);
    backend.flip_y = true;
    CHECK(gpu_pgraph_replay_copy(&copy, &backend, &source, &flipped, &used, &report) == GPU_PGRAPH_OK);
    for (uint32_t row = 0u; row < 5u; row++) {
        for (uint32_t x = 0u; x < 6u; x++) {
            CHECK(memcmp(pixel_at(&flipped, 5u + x, 14u - 1u - (4u + row)),
                         pixel_at(&source, 3u + x, 12u - 1u - (2u + row)), 4u) == 0);
        }
    }
    CHECK(memcmp(pixel_at(&flipped, 5u, 4u), pixel_at(&destination, 5u, 4u), 4u) != 0); /* not the unflipped rows */
    backend.flip_y = false;

    /* the refusals, each by name and none touching the destination */
    gpu_image before = make_image(20u, 14u, 99u);
    gpu_image candidate = make_image(20u, 14u, 99u);
    struct {
        gpu_pgraph_copy copy;
        gpu_pgraph_backend backend;
        const char *what;
    } refusals[6];
    memset(refusals, 0, sizeof refusals);
    for (size_t i = 0u; i < 6u; i++) {
        refusals[i].copy = make_copy(6u, 5u, 3u, 2u, 5u, 4u, 64u, 80u);
        refusals[i].backend = blit_backend(GPU_PGRAPH_OUTPUT_BLIT, GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL);
    }
    refusals[0].backend = blit_backend(GPU_PGRAPH_OUTPUT_ALL_MEASURED & ~GPU_PGRAPH_OUTPUT_BLIT, GPU_PGRAPH_INFER_OUTPUT_ALL);
    refusals[0].what = "does not enable the blit group";
    refusals[1].copy.color_format = GPU_PGRAPH_BLIT_FORMAT_Y8;
    refusals[1].what = "A8R8G8B8 images";
    refusals[2].copy.source_pitch = 4096u;
    refusals[2].what = "tightly packed";
    refusals[3].copy.out_x = 15u; /* 15 + 6 > 20 */
    refusals[3].what = "reaches past an image";
    refusals[4].backend = blit_backend(GPU_PGRAPH_OUTPUT_BLIT, GPU_PGRAPH_INFER_ALL);
    refusals[4].what = "INFERRED and not allowed";
    refusals[5].copy.operation = 2u;
    refusals[5].what = "only SRCCOPY";
    for (size_t i = 0u; i < 6u; i++) {
        memset(&report, 0, sizeof report);
        used = 0u;
        CHECK(gpu_pgraph_replay_copy(&refusals[i].copy, &refusals[i].backend, &source, &candidate, &used, &report) ==
              GPU_PGRAPH_ERR_UNMEASURED);
        CHECK(strstr(report.error, refusals[i].what) != NULL);
        CHECK(used == 0u && memcmp(candidate.pixels, before.pixels, (size_t)before.stride_bytes * before.height) == 0);
    }
    /* each axis of each rectangle past its image is refused by name, and the exact fit on all four axes is copied */
    static const struct {
        uint32_t in_x, in_y, out_x, out_y;
    } past[4] = {{1u, 0u, 4u, 2u}, {0u, 1u, 4u, 2u}, {0u, 0u, 5u, 2u}, {0u, 0u, 4u, 3u}};
    for (size_t i = 0u; i < 4u; i++) {
        copy = make_copy(16u, 12u, past[i].in_x, past[i].in_y, past[i].out_x, past[i].out_y, 64u, 80u);
        memset(&report, 0, sizeof report);
        CHECK(gpu_pgraph_replay_copy(&copy, &backend, &source, &candidate, &used, &report) == GPU_PGRAPH_ERR_UNMEASURED);
        CHECK(strstr(report.error, "reaches past an image") != NULL);
        CHECK(memcmp(candidate.pixels, before.pixels, (size_t)before.stride_bytes * before.height) == 0);
    }
    copy = make_copy(16u, 12u, 0u, 0u, 4u, 2u, 64u, 80u);
    CHECK(gpu_pgraph_replay_copy(&copy, &backend, &source, &candidate, &used, &report) == GPU_PGRAPH_OK);
    CHECK(memcmp(pixel_at(&candidate, 4u, 2u), pixel_at(&source, 0u, 0u), 4u) == 0 &&
          memcmp(pixel_at(&candidate, 19u, 13u), pixel_at(&source, 15u, 11u), 4u) == 0);
    /* a destination pitch that is not the image's, and an image whose stride is not width * 4 */
    copy = make_copy(6u, 5u, 3u, 2u, 5u, 4u, 64u, 64u);
    memset(&report, 0, sizeof report);
    CHECK(gpu_pgraph_replay_copy(&copy, &backend, &source, &candidate, &used, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "tightly packed") != NULL);
    copy = make_copy(6u, 5u, 3u, 2u, 5u, 4u, 64u, 80u);
    gpu_image padded = candidate;
    padded.stride_bytes = 96u;
    memset(&report, 0, sizeof report);
    CHECK(gpu_pgraph_replay_copy(&copy, &backend, &source, &padded, &used, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "tightly packed") != NULL);
    padded = source;
    padded.stride_bytes = 72u;
    CHECK(gpu_pgraph_replay_copy(&copy, &backend, &padded, &candidate, &used, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    /* the source rectangle past the source image, and the rows of 16 pixels against a 20 pixel destination pitch */
    copy = make_copy(6u, 5u, 12u, 2u, 5u, 4u, 64u, 80u);
    memset(&report, 0, sizeof report);
    CHECK(gpu_pgraph_replay_copy(&copy, &backend, &source, &candidate, &used, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "reaches past an image") != NULL);
    copy = make_copy(6u, 5u, 3u, 2u, 5u, 4u, 80u, 80u);
    memset(&report, 0, sizeof report);
    CHECK(gpu_pgraph_replay_copy(&copy, &backend, &source, &candidate, &used, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "tightly packed") != NULL);

    /* an empty rectangle moves nothing and uses no inference, whatever the images are */
    copy = make_copy(0u, 5u, 3u, 2u, 5u, 4u, 64u, 80u);
    used = 0u;
    CHECK(gpu_pgraph_replay_copy(&copy, &backend, NULL, NULL, &used, &report) == GPU_PGRAPH_OK && used == 0u);
    copy = make_copy(6u, 0u, 3u, 2u, 5u, 4u, 64u, 80u);
    CHECK(gpu_pgraph_replay_copy(&copy, &backend, NULL, NULL, &used, &report) == GPU_PGRAPH_OK && used == 0u);
    gpu_pgraph_backend no_group = blit_backend(0u, GPU_PGRAPH_INFER_OUTPUT_ALL);
    CHECK(gpu_pgraph_replay_copy(&copy, &no_group, NULL, NULL, &used, &report) == GPU_PGRAPH_ERR_UNMEASURED);

    /* one image as both sides: disjoint rectangles copy, overlapping ones are refused */
    gpu_image single = make_image(16u, 12u, 40u);
    gpu_image single_before = make_image(16u, 12u, 40u);
    copy = make_copy(4u, 3u, 0u, 0u, 8u, 6u, 64u, 64u);
    CHECK(gpu_pgraph_replay_copy(&copy, &backend, &single, &single, &used, &report) == GPU_PGRAPH_OK);
    for (uint32_t y = 0u; y < 3u; y++) {
        for (uint32_t x = 0u; x < 4u; x++) {
            CHECK(memcmp(pixel_at(&single, 8u + x, 6u + y), pixel_at(&single_before, x, y), 4u) == 0);
        }
    }
    CHECK(memcmp(pixel_at(&single, 8u, 6u), pixel_at(&single_before, 8u, 6u), 4u) != 0);
    copy = make_copy(4u, 3u, 0u, 0u, 3u, 2u, 64u, 64u);
    memset(&report, 0, sizeof report);
    CHECK(gpu_pgraph_replay_copy(&copy, &backend, &single, &single, &used, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "overlap") != NULL);
    /* overlap on one axis only (the other disjoint) is not an overlap, and neither is an adjacent rectangle */
    static const struct {
        uint32_t out_x, out_y;
        bool refused;
    } neighbours[] = {{4u, 0u, false}, {0u, 3u, false}, {3u, 0u, true}, {0u, 2u, true}, {4u, 3u, false}, {3u, 2u, true}};
    for (size_t i = 0u; i < sizeof neighbours / sizeof neighbours[0]; i++) {
        copy = make_copy(4u, 3u, 0u, 0u, neighbours[i].out_x, neighbours[i].out_y, 64u, 64u);
        memset(&report, 0, sizeof report);
        CHECK((gpu_pgraph_replay_copy(&copy, &backend, &single, &single, &used, &report) == GPU_PGRAPH_ERR_UNMEASURED) ==
              neighbours[i].refused);
    }
    /* the source starting exactly where the destination ends (sharing an edge) on one axis, the other axis overlapping */
    copy = make_copy(4u, 3u, 8u, 6u, 4u, 6u, 64u, 64u);
    CHECK(gpu_pgraph_replay_copy(&copy, &backend, &single, &single, &used, &report) == GPU_PGRAPH_OK);
    copy = make_copy(4u, 3u, 8u, 6u, 8u, 3u, 64u, 64u);
    CHECK(gpu_pgraph_replay_copy(&copy, &backend, &single, &single, &used, &report) == GPU_PGRAPH_OK);
    /* and the mirror: the destination rectangle before the source in x and in y */
    copy = make_copy(4u, 3u, 8u, 6u, 4u, 3u, 64u, 64u);
    CHECK(gpu_pgraph_replay_copy(&copy, &backend, &single, &single, &used, &report) == GPU_PGRAPH_OK);
    copy = make_copy(4u, 3u, 8u, 6u, 5u, 4u, 64u, 64u);
    CHECK(gpu_pgraph_replay_copy(&copy, &backend, &single, &single, &used, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    /* the same rectangles on two images that merely have equal size are fine */
    CHECK(gpu_pgraph_replay_copy(&copy, &backend, &single_before, &single, &used, &report) == GPU_PGRAPH_OK);

    CHECK(gpu_pgraph_replay_copy(NULL, &backend, &source, &destination, &used, &report) == GPU_PGRAPH_ERR_ARGUMENT);
    copy = make_copy(6u, 5u, 3u, 2u, 5u, 4u, 64u, 80u);
    CHECK(gpu_pgraph_replay_copy(&copy, &backend, NULL, &destination, &used, &report) == GPU_PGRAPH_ERR_ARGUMENT);
    free(source.pixels);
    free(destination.pixels);
    free(untouched.pixels);
    free(flipped.pixels);
    free(before.pixels);
    free(candidate.pixels);
    free(single.pixels);
    free(single_before.pixels);
}

int main(void)
{
    test_word_table();
    test_sync_pairs();
    test_measured_values_resolve();
    test_output_index_is_a_bijection();
    test_title_lod0_filter();
    test_t927_lod0_filter_requires_output_permission();
    test_t1266_mixed_wrap_clamp_address();
    test_refusals();
    test_immediate_draw();
    test_immediate_refusals();
    test_default_inputs();
    test_blit_group();
    test_blit_init_words();
    test_blit_refusals();
    test_blit_order();
    test_replay_copy();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
