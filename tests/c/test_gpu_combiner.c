/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gpu_combiner (T75, fragment half): the register-combiner state decoded from a recorded stream,
 * the plan and every refusal of gpu_combiner.c, and the combiner fragment stage drawn on every
 * Vulkan device that exists. The decoder and plan parts need no device and always run. The draw
 * parts print a stated SKIP per absent device, and when nothing ran the test exits 77.
 *
 * The combiner words are hand-built from docs/combiner-translator.md section 3 (the same four
 * configurations as tools/nv2a_combiner/replay_fixtures.py, which compiled gpu_combiner_words.h from
 * them). The COMBINER_NAME_* macros are the SHA-256 names the PYTHON key computed, so a C key that
 * disagrees with the words or with the Python function fails here.
 *
 * Frames are 64 x 64, clear 0.2 grey (51). One triangle with oD0 from the hand-written vertex module
 * of the replay suites (oD0 = v2.zyxw + c[3], v2 here is white, so oD0 is white):
 *   default stage         white (255, 255, 255, 255): the fixed stage writes oD0
 *   combiner pass         the same bytes as the default stage, over the whole frame
 *   combiner multiply     r0 = c0 * c1: (128, 96, 64, 255) from factor words 0x00FF8040 and 0x0080C0FF
 *   combiner final        E * F of the final constants 0x80FF4020, 0x00808080: (128, 32, 16, 128)
 */
#include "gpu_combiner.h"
#include "gpu_device.h"
#include "gpu_pgraph.h"
#include "gpu_pgraph_replay.h"
#include "gpu_pgraph_test_support.h"
#include "gpu_vsh_draw.h"

#include "gpu_combiner_words.h"
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
#define WORDS GPU_PGRAPH_COMBINER_WORDS
#define ALL_BITS (GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_COMBINER_ALL)

typedef struct {
    uint32_t index;
    uint32_t value;
} entry;

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

static const entry pass_entries[] = {{34u, 0xC4200000u}, {0u, 0xD4300000u}, {45u, 0xC0u},
                                     {26u, 0xC0u},       {53u, 0x11101u},   {8u, 0x0Cu},
                                     {9u, 0x1C80u}};
static const entry multiply_entries[] = {{34u, 0x01020000u}, {0u, 0xD4300000u}, {45u, 0xC0u},
                                         {26u, 0xC0u},       {53u, 0x11101u},   {8u, 0x0Cu},
                                         {9u, 0x1C80u},      {10u, 0x00FF8040u}, {18u, 0x0080C0FFu}};
static const entry final_entries[] = {{53u, 0x11101u}, {8u, 0x0Fu}, {9u, 0x01021180u},
                                      {43u, 0x80FF4020u}, {44u, 0x00808080u}};
static const entry twovary_entries[] = {{34u, 0x04050000u}, {0u, 0xD4300000u}, {45u, 0xC0u},
                                        {26u, 0xC0u},       {53u, 0x11101u},   {8u, 0x0Cu},
                                        {9u, 0x1C80u}};

static void build_words(const entry *entries, size_t count, uint32_t words[WORDS])
{
    memset(words, 0, WORDS * sizeof words[0]);
    for (size_t i = 0u; i < count; i++) {
        words[entries[i].index] = entries[i].value;
    }
}

static gpu_pgraph *model(bool combiner)
{
    gpu_pgraph *pgraph = gpu_pgraph_create();
    gpu_pgraph_set_combiner(pgraph, combiner);
    return pgraph;
}

/* A state holding `words`, as the decoder would after the writer ran. */
static void load_state(gpu_pgraph *pgraph, const uint32_t words[WORDS])
{
    stream_builder stream = {0};
    stream_pixel_shader(&stream, words);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    stream_free(&stream);
}

/* --- no device ----------------------------------------------------------------------------- */

static void test_method_map(void)
{
    printf("test_method_map\n");
    uint32_t seen[WORDS] = {0};
    uint32_t mapped = 0u;
    for (uint32_t method = 0u; method < 0x2000u; method++) {
        const int index = gpu_pgraph_combiner_index(method);
        if (index >= 0) {
            CHECK((uint32_t)index < WORDS);
            seen[index]++;
            mapped++;
        }
    }
    CHECK(mapped == WORDS); /* non-empty, and every method maps to one word */
    for (uint32_t i = 0u; i < WORDS; i++) {
        CHECK(seen[i] == 1u);
    }
    static const struct {
        uint32_t method;
        int index;
    } known[] = {
        {0x0260u, 0},  {0x027Cu, 7},  {0x0288u, 8},  {0x028Cu, 9},  {0x0A60u, 10}, {0x0A7Cu, 17},
        {0x0A80u, 18}, {0x0AA0u, 26}, {0x0AC0u, 34}, {0x0ADCu, 41}, {0x17F8u, 42}, {0x1E20u, 43},
        {0x1E24u, 44}, {0x1E40u, 45}, {0x1E5Cu, 52}, {0x1E60u, 53}, {0x1E70u, 54}, {0x1E74u, 55},
        {0x1E78u, 56},
    };
    for (size_t i = 0u; i < COUNT(known); i++) {
        CHECK(gpu_pgraph_combiner_index(known[i].method) == known[i].index);
    }
    /* neighbours that must NOT map: the begin/end method sits next to 0x17F8 */
    static const uint32_t neighbours[] = {0x0280u, 0x0284u, 0x0290u, 0x0A5Cu, 0x0AE0u, 0x17FCu,
                                          0x1E28u, 0x1E3Cu, 0x1E64u, 0x1E6Cu, 0x1E7Cu, 0x0261u};
    for (size_t i = 0u; i < COUNT(neighbours); i++) {
        CHECK(gpu_pgraph_combiner_index(neighbours[i]) == -1);
    }
}

static void test_decoder(void)
{
    printf("test_decoder\n");
    uint32_t words[WORDS];
    build_words(multiply_entries, COUNT(multiply_entries), words);
    stream_builder stream = {0};
    stream_pixel_shader(&stream, words);

    /* OFF (the default): every combiner method is unhandled, and strict refuses the first */
    gpu_pgraph *off = model(false);
    CHECK(gpu_pgraph_decode(off, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_unhandled_count(off) > 0u);
    CHECK(gpu_pgraph_get_stats(off).pairs_handled == 0u);
    CHECK(!gpu_pgraph_state_now(off)->combiner_captured);
    CHECK(gpu_pgraph_state_now(off)->combiner[34] == 0u);
    gpu_pgraph_destroy(off);
    gpu_pgraph *strict = model(false);
    gpu_pgraph_set_strict(strict, true);
    CHECK(gpu_pgraph_decode(strict, stream.pairs, stream.count) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(gpu_pgraph_error(strict), "0x0260") != NULL);
    gpu_pgraph_destroy(strict);

    /* ON: held in the shadow, handled, even in strict mode */
    gpu_pgraph *on = model(true);
    gpu_pgraph_set_strict(on, true);
    CHECK(gpu_pgraph_decode(on, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    const gpu_pgraph_state *state = gpu_pgraph_state_now(on);
    CHECK(state->combiner_captured);
    CHECK(gpu_pgraph_unhandled_count(on) == 0u);
    CHECK(gpu_pgraph_get_stats(on).pairs_handled == stream.count);
    uint32_t written = 0u;
    for (uint32_t i = 0u; i < WORDS; i++) {
        written += state->combiner_written[i] ? 1u : 0u;
        CHECK(state->combiner[i] == words[i]);
    }
    /* all but word 54, which only the texture-stage emitter (not ported) writes, the final words
     * included because their OR is nonzero */
    CHECK(written == WORDS - 1u && !state->combiner_written[54]);
    /* the writer omits the final pair when both words are zero */
    uint32_t quiet[WORDS];
    memset(quiet, 0, sizeof quiet);
    stream_builder quiet_stream = {0};
    stream_pixel_shader(&quiet_stream, quiet);
    gpu_pgraph *quiet_model = model(true);
    CHECK(gpu_pgraph_decode(quiet_model, quiet_stream.pairs, quiet_stream.count) == GPU_PGRAPH_OK);
    CHECK(!gpu_pgraph_state_now(quiet_model)->combiner_written[8]);
    CHECK(!gpu_pgraph_state_now(quiet_model)->combiner_written[9]);
    CHECK(gpu_pgraph_state_now(quiet_model)->combiner_written[7]);
    gpu_pgraph_destroy(quiet_model);
    stream_free(&quiet_stream);

    /* a constant update lands in the same word the writer's block did */
    stream_builder update = {0};
    stream_pair(&update, 0x0A60u + 0x20u * 1u + 4u * 0u, 0x11223344u); /* factor1, stage 0 */
    stream_pair(&update, 0x1E24u, 0x55667788u);                         /* final constant 1 */
    CHECK(gpu_pgraph_decode(on, update.pairs, update.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_state_now(on)->combiner[18] == 0x11223344u);
    CHECK(gpu_pgraph_state_now(on)->combiner[44] == 0x55667788u);
    stream_free(&update);

    /* a snapshot taken at a draw END keeps the words of that moment */
    stream_builder draw = {0};
    stream_draw_arrays(&draw, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    stream_pair(&draw, 0x0A60u, 0xAABBCCDDu);
    stream_draw_arrays(&draw, GPU_PGRAPH_OP_TRIANGLES, 3u, 3u);
    CHECK(gpu_pgraph_decode(on, draw.pairs, draw.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_count(on) == 2u);
    CHECK(gpu_pgraph_snapshot_count(on) == 2u);
    CHECK(gpu_pgraph_snapshot(on, 0u)->combiner[10] == 0x00FF8040u);
    CHECK(gpu_pgraph_snapshot(on, 1u)->combiner[10] == 0xAABBCCDDu);
    stream_free(&draw);

    /* a change inside a BEGIN_END bracket is refused, as vertex state is */
    stream_builder inside = {0};
    stream_pair(&inside, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_TRIANGLES);
    stream_pair(&inside, 0x0A60u, 1u);
    CHECK(gpu_pgraph_decode(on, inside.pairs, inside.count) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(gpu_pgraph_error(on), "BEGIN_END") != NULL);
    stream_free(&inside);

    /* reset forgets the words and keeps the opt-in */
    gpu_pgraph_reset(on);
    CHECK(gpu_pgraph_state_now(on)->combiner_captured);
    CHECK(!gpu_pgraph_state_now(on)->combiner_written[10]);
    CHECK(gpu_pgraph_state_now(on)->combiner[10] == 0u);
    gpu_pgraph_destroy(on);
    stream_free(&stream);
}

static gpu_pgraph_result plan_of(const uint32_t words[WORDS], uint32_t allowed,
                                 const gpu_combiner_texture *textures, gpu_combiner_plan *plan,
                                 char *error, size_t error_size)
{
    static const gpu_combiner_texture none[GPU_COMBINER_TEXTURE_STAGES] = {{0}};
    gpu_pgraph *pgraph = model(true);
    load_state(pgraph, words);
    const gpu_pgraph_result result = gpu_combiner_plan_build(
        gpu_pgraph_state_now(pgraph), allowed, textures != NULL ? textures : none, plan, error,
        error_size);
    gpu_pgraph_destroy(pgraph);
    return result;
}

static void test_names_and_constants(void)
{
    printf("test_names_and_constants\n");
    static const struct {
        const entry *entries;
        size_t count;
        const char *name;
        uint32_t reads_must;   /* registers that must be read */
        uint32_t reads_never;  /* registers that must not be */
        uint32_t inferences;
    } cases[] = {
        {pass_entries, COUNT(pass_entries), COMBINER_NAME_PASS, (1u << 4) | (1u << 12),
         (1u << 5) | (1u << 1) | (1u << 8), GPU_PGRAPH_INFER_COMBINER_COLOUR_RANGE},
        {multiply_entries, COUNT(multiply_entries), COMBINER_NAME_MULTIPLY,
         (1u << 1) | (1u << 2) | (1u << 4), (1u << 5) | (1u << 8),
         GPU_PGRAPH_INFER_COMBINER_COLOUR_RANGE | GPU_PGRAPH_INFER_COMBINER_CONSTANT_BYTES},
        {final_entries, COUNT(final_entries), COMBINER_NAME_FINAL, (1u << 15) | (1u << 1) | (1u << 2),
         (1u << 4) | (1u << 5) | (1u << 12), GPU_PGRAPH_INFER_COMBINER_CONSTANT_BYTES},
        {twovary_entries, COUNT(twovary_entries), COMBINER_NAME_TWOVARY, (1u << 4) | (1u << 5),
         (1u << 1) | (1u << 8), GPU_PGRAPH_INFER_COMBINER_COLOUR_RANGE},
    };
    const char *seen[COUNT(cases)];
    for (size_t i = 0u; i < COUNT(cases); i++) {
        uint32_t words[WORDS];
        build_words(cases[i].entries, cases[i].count, words);
        gpu_combiner_plan plan;
        char error[200];
        CHECK(plan_of(words, GPU_COMBINER_INFER_ALL, NULL, &plan, error, sizeof error) == GPU_PGRAPH_OK);
        CHECK(strcmp(plan.name, cases[i].name) == 0); /* the Python key, byte for byte */
        CHECK(plan.stage_count == 1u);
        CHECK((plan.reads & cases[i].reads_must) == cases[i].reads_must);
        CHECK((plan.reads & cases[i].reads_never) == 0u);
        CHECK(plan.used_inferences == cases[i].inferences);
        CHECK(plan.texture_stages == 0u);
        seen[i] = cases[i].name;
    }
    for (size_t i = 0u; i < COUNT(cases); i++) {
        for (size_t j = i + 1u; j < COUNT(cases); j++) {
            CHECK(strcmp(seen[i], seen[j]) != 0);
        }
    }

    /* the factor words reach the constants block as RGBA = (byte 2, 1, 0, 3) / 255 */
    uint32_t words[WORDS];
    build_words(multiply_entries, COUNT(multiply_entries), words);
    gpu_combiner_plan plan;
    char error[200];
    CHECK(plan_of(words, GPU_COMBINER_INFER_ALL, NULL, &plan, error, sizeof error) == GPU_PGRAPH_OK);
    CHECK(plan.constants[0] == 255.0f / 255.0f && plan.constants[1] == 128.0f / 255.0f &&
          plan.constants[2] == 64.0f / 255.0f && plan.constants[3] == 0.0f);   /* factor0[0], 0x00FF8040 */
    CHECK(plan.constants[8u * 4u + 0u] == 128.0f / 255.0f && plan.constants[8u * 4u + 1u] == 192.0f / 255.0f &&
          plan.constants[8u * 4u + 2u] == 255.0f / 255.0f && plan.constants[8u * 4u + 3u] == 0.0f);
    build_words(final_entries, COUNT(final_entries), words);
    CHECK(plan_of(words, GPU_COMBINER_INFER_ALL, NULL, &plan, error, sizeof error) == GPU_PGRAPH_OK);
    CHECK(plan.constants[16u * 4u + 0u] == 255.0f / 255.0f && plan.constants[16u * 4u + 1u] == 64.0f / 255.0f &&
          plan.constants[16u * 4u + 2u] == 32.0f / 255.0f && plan.constants[16u * 4u + 3u] == 128.0f / 255.0f);
    CHECK(plan.constants[17u * 4u + 0u] == 128.0f / 255.0f && plan.constants[17u * 4u + 3u] == 0.0f);

    /* the key follows the structure words and ignores everything else */
    build_words(multiply_entries, COUNT(multiply_entries), words);
    char base[GPU_COMBINER_NAME_BYTES];
    CHECK(plan_of(words, GPU_COMBINER_INFER_ALL, NULL, &plan, error, sizeof error) == GPU_PGRAPH_OK);
    snprintf(base, sizeof base, "%s", plan.name);
    static const struct {
        uint32_t index;
        uint32_t xor_bits;
        bool changes;
        const char *what;
    } flips[] = {
        {0u, 0x20000000u, true, "alpha input mapping"},
        {34u, 0x20000000u, true, "colour input mapping"},
        {26u, 0x00010000u, true, "alpha output scale"},
        {45u, 0x00010000u, true, "colour output scale"},
        {8u, 0x20000000u, true, "final word 0"},
        {9u, 0x20000000u, true, "final word 1"},
        {9u, 0x00000080u, true, "final clamp flag"},
        {53u, 0x00000100u, true, "control mux bit"},
        {53u, 0x00001000u, true, "control factor0 each bit"},
        {10u, 0xFFFFFFFFu, false, "factor0 value"},
        {18u, 0xFFFFFFFFu, false, "factor1 value"},
        {43u, 0xFFFFFFFFu, false, "final constant 0"},
        {44u, 0xFFFFFFFFu, false, "final constant 1"},
        {42u, 0xFFFFFFFFu, false, "clip plane mode (a mode the replay refuses)"},
        {55u, 0xFFFFFFFFu, false, "dot mapping"},
        {56u, 0xFFFFFFFFu, false, "other stage input"},
        {1u, 0xFFFFFFFFu, false, "alpha input of an unused stage"},
        {27u, 0xFFFFFFFFu, false, "alpha output of an unused stage"},
        {35u, 0xFFFFFFFFu, false, "colour input of an unused stage"},
        {46u, 0xFFFFFFFFu, false, "colour output of an unused stage"},
        {9u, 0x0000001Fu, false, "final word 1 low bits the translator ignores"},
        {45u, 0xFFF00000u, false, "colour output high bits"},
        {53u, 0xFFFEE000u, false, "control bits outside count, mux and the factor flags"},
    };
    for (size_t i = 0u; i < COUNT(flips); i++) {
        uint32_t flipped[WORDS];
        memcpy(flipped, words, sizeof flipped);
        flipped[flips[i].index] ^= flips[i].xor_bits;
        gpu_combiner_plan other;
        const gpu_pgraph_result result =
            plan_of(flipped, GPU_COMBINER_INFER_ALL, NULL, &other, error, sizeof error);
        if (result != GPU_PGRAPH_OK) {
            printf("  case %s refused: %s\n", flips[i].what, error);
        }
        CHECK(result == GPU_PGRAPH_OK);
        CHECK((strcmp(other.name, base) != 0) == flips[i].changes);
    }
}

typedef struct {
    const char *what;
    uint32_t index;      /* word to set, ignored when `unwrite` */
    uint32_t value;
    int unwrite;         /* drop the word from the stream (never written) */
    uint32_t allowed;
    const char *expect;  /* substring of the refusal */
} refusal;

static void test_refusals(void)
{
    printf("test_refusals\n");
    static const refusal cases[] = {
        {"control word zero", 53u, 0u, 0, ALL_BITS, "stage count 0"},
        {"nine stages", 53u, 0x11109u, 0, ALL_BITS, "stage count 9"},
        {"fog register as a source", 34u, 0x03020000u, 0, ALL_BITS, "fog register"},
        {"register 6 as a source", 34u, 0x06000000u, 0, ALL_BITS, "register 6"},
        {"register 14 as a source", 34u, 0x0E000000u, 0, ALL_BITS, "register 14"},
        {"destination 14", 45u, 0x0E00u, 0, ALL_BITS, "neither a colour nor a spare"},
        {"x4 with a bias", 45u, 0xC0u | (5u << 15), 0, ALL_BITS, "operation 5"},
        {"dot product on the alpha half", 26u, 0xC0u | (1u << 13), 0, ALL_BITS, "alpha half"},
        {"final reads 7", 8u, 0x07u, 0, ALL_BITS, "final combiner reads register 7"},
        {"final E reads the product", 9u, 0x0F001C80u, 0, ALL_BITS, "product"},
        {"texture read, no texture", 34u, 0x08200000u, 0, ALL_BITS, "no test texture"},
        {"a stage 1 texture read", 34u, 0x09200000u, 0, ALL_BITS, "no test texture"},
        {"program mode 5 (clip plane)", 54u, 5u, 0, ALL_BITS, "mode 5 is not modelled"},
        {"program mode 7 on stage 1 is dropped only when unread, mode 10 never", 54u, 10u << 5, 0, ALL_BITS, "mode 10 is not modelled"},
        {"colour range withheld", 0u, 0xD4300000u, 0, ALL_BITS & ~GPU_PGRAPH_INFER_COMBINER_COLOUR_RANGE,
         "oD0 and oD1 into the combiner unclamped"},
        {"alpha output never written, bit withheld", 26u, 0u, 1,
         ALL_BITS & ~GPU_PGRAPH_INFER_COMBINER_UNWRITTEN, "alpha output word"},
        {"final word never written, bit withheld", 9u, 0u, 1,
         ALL_BITS & ~GPU_PGRAPH_INFER_COMBINER_UNWRITTEN, "final combiner word 1"},
        {"spare1 read before any write", 34u, 0x0D200000u, 0, ALL_BITS & ~GPU_PGRAPH_INFER_COMBINER_INITIAL_STATE,
         "before any write"},
        {"model not capturing", 0u, 0u, 0, ALL_BITS, "did not decode"},
    };
    for (size_t i = 0u; i < COUNT(cases); i++) {
        uint32_t words[WORDS];
        build_words(pass_entries, COUNT(pass_entries), words);
        if (!cases[i].unwrite) {
            words[cases[i].index] = cases[i].value;
        }
        if (strcmp(cases[i].what, "model not capturing") == 0) {
            gpu_pgraph *off = model(false);
            gpu_combiner_plan plan;
            char error[200];
            static const gpu_combiner_texture none[GPU_COMBINER_TEXTURE_STAGES] = {{0}};
            CHECK(gpu_combiner_plan_build(gpu_pgraph_state_now(off), ALL_BITS, none, &plan, error,
                                          sizeof error) == GPU_PGRAPH_ERR_UNMEASURED);
            CHECK(strstr(error, cases[i].expect) != NULL);
            gpu_pgraph_destroy(off);
            continue;
        }
        gpu_pgraph *pgraph = model(true);
        stream_builder stream = {0};
        stream_pixel_shader(&stream, words);
        if (cases[i].unwrite) {
            /* drop the pair that writes the word: rebuild without it */
            stream_builder cut = {0};
            for (size_t pair = 0u; pair < stream.count; pair++) {
                if (gpu_pgraph_combiner_index(stream.pairs[pair].method) != (int)cases[i].index) {
                    stream_pair(&cut, stream.pairs[pair].method, stream.pairs[pair].data);
                }
            }
            CHECK(cut.count + 1u == stream.count);
            stream_free(&stream);
            stream = cut;
        }
        if (cases[i].index == 54u && !cases[i].unwrite) {
            /* the texture-stage emitter, which the port does not have, writes 0x1E70 separately */
            stream_pair(&stream, 0x1E70u, cases[i].value);
        }
        CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
        gpu_combiner_plan plan;
        char error[200];
        static const gpu_combiner_texture none[GPU_COMBINER_TEXTURE_STAGES] = {{0}};
        const gpu_pgraph_result result =
            gpu_combiner_plan_build(gpu_pgraph_state_now(pgraph), cases[i].allowed, none, &plan, error,
                                    sizeof error);
        if (result == GPU_PGRAPH_OK || strstr(error, cases[i].expect) == NULL) {
            printf("  case \"%s\": result %d, error \"%s\", wanted \"%s\"\n", cases[i].what, (int)result,
                   error, cases[i].expect);
        }
        CHECK(result == GPU_PGRAPH_ERR_UNMEASURED);
        CHECK(error[0] != '\0');
        CHECK(strstr(error, cases[i].expect) != NULL);
        stream_free(&stream);
        gpu_pgraph_destroy(pgraph);
    }
}

/* --- T1490: stage programs 3, 9 and 17 ------------------------------------------------------ */

/* The plan of a combiner reading t<read> (spare0 = t<read> rgb, final D = spare0) under the given stage program, dot mapping and other
 * stage input words, with the program written the way the texture-stage emitter writes it (0x1E70). */
static gpu_pgraph_result modes_plan(uint32_t read, uint32_t program, uint32_t mapping, uint32_t other, uint32_t allowed,
                                    const gpu_combiner_texture textures[GPU_COMBINER_TEXTURE_STAGES], gpu_combiner_plan *plan,
                                    char *error, size_t error_size)
{
    uint32_t words[WORDS];
    memset(words, 0, sizeof words);
    words[0] = 0xD4300000u;
    words[45] = 0xC0u;
    words[26] = 0xC0u;
    words[53] = 0x11101u;
    words[8] = 0x0Cu;
    words[9] = 0x1C80u;
    words[34] = ((8u + read) << 24) | 0x00200000u;
    words[55] = mapping;
    words[56] = other;
    gpu_pgraph *pgraph = model(true);
    stream_builder stream = {0};
    stream_pixel_shader(&stream, words);
    stream_pair(&stream, 0x1E70u, program);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    stream_free(&stream);
    const gpu_pgraph_result result = gpu_combiner_plan_build(gpu_pgraph_state_now(pgraph), allowed, textures, plan, error, error_size);
    gpu_pgraph_destroy(pgraph);
    return result;
}

#define MODES_ALLOWED (GPU_COMBINER_INFER_ALL | GPU_COMBINER_INFER_TEXTURE_MODES)
/* the names tools/nv2a_combiner/replay_modules.py gives the same configurations (tests/fixtures/t1490_modes/make_modules.py manifest) */
#define NAME_MODES_CONTROL "combiner_507f93a2463492e5ae295f7706cbd0469a12e81f6a54c0b716fad23e6dfaaa3e"
#define NAME_MODES_CUBE "combiner_b8005cf0a1057378c99365b005846bdcf9042776613fb16ed4ff178a8b9e03ae"
#define NAME_MODES_DOT_PAIR "combiner_fb1e2b4a9de06b5626f1ecb9668439ccdfd7a1d30252fbcb38993b0cc9772ea0"
#define NAME_MODES_DOT_ZERO "combiner_ec57db5048485da6c4c6a43039e99ae7fd045e05fd8413b16d020775ceaa3613"
#define NAME_MODES_DOT_STAGE0 "combiner_d4b52bfcdcb55eda32957993f1b09898e103eb67becd3ca0ade3bb843d3a4cbd"

static void expect_refusal_text(uint32_t read, uint32_t program, uint32_t mapping, uint32_t other, uint32_t allowed,
                                const gpu_combiner_texture textures[GPU_COMBINER_TEXTURE_STAGES], const char *text)
{
    gpu_combiner_plan plan;
    char error[300];
    const gpu_pgraph_result result = modes_plan(read, program, mapping, other, allowed, textures, &plan, error, sizeof error);
    if (result == GPU_PGRAPH_OK || strstr(error, text) == NULL) {
        printf("  program 0x%X read t%u: result %d, error \"%s\", wanted \"%s\"\n", (unsigned)program, (unsigned)read, (int)result, error, text);
    }
    CHECK(result == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(error, text) != NULL);
}

static void test_texture_modes(void)
{
    printf("test_texture_modes\n");
    static const uint8_t texel[4] = {1u, 2u, 3u, 4u};
    gpu_combiner_texture flat[GPU_COMBINER_TEXTURE_STAGES], cube[GPU_COMBINER_TEXTURE_STAGES];
    memset(flat, 0, sizeof flat);
    memset(cube, 0, sizeof cube);
    for (uint32_t stage = 0u; stage < GPU_COMBINER_TEXTURE_STAGES; stage++) {
        flat[stage].rgba = texel;
        flat[stage].width = flat[stage].height = 1u;
        cube[stage] = flat[stage];
        cube[stage].cube = true;
    }
    gpu_combiner_plan plan;
    char error[300];
    /* the control: mode 1 keeps its name, stays out of the new inference and has no cube or dot stage */
    CHECK(modes_plan(0u, 1u, 0u, 0u, GPU_COMBINER_INFER_ALL, flat, &plan, error, sizeof error) == GPU_PGRAPH_OK);
    CHECK(strcmp(plan.name, NAME_MODES_CONTROL) == 0 && plan.texture_stages == 1u && plan.coordinate_stages == 1u && plan.cube_stages == 0u);
    CHECK(!plan.uses_texture_modes && plan.key[36] == 0u && plan.key[37] == 0u);
    /* a cube map in stage 1, INFERRED: refused unless the bit is allowed, and only with a cube map bound */
    expect_refusal_text(1u, 3u << 5, 0u, 0u, GPU_COMBINER_INFER_ALL, cube, "INFERRED from xemu psh.c and not allowed");
    CHECK(modes_plan(1u, 3u << 5, 0u, 0u, MODES_ALLOWED, cube, &plan, error, sizeof error) == GPU_PGRAPH_OK);
    CHECK(strcmp(plan.name, NAME_MODES_CUBE) == 0 && plan.texture_stages == 2u && plan.cube_stages == 2u && plan.coordinate_stages == 2u);
    CHECK(plan.uses_texture_modes && (plan.used_inferences & GPU_COMBINER_INFER_TEXTURE_MODES) == 0u);
    expect_refusal_text(1u, 3u << 5, 0u, 0u, MODES_ALLOWED, flat, "needs a cube map but the bound texture is 2D");
    expect_refusal_text(0u, 1u, 0u, 0u, MODES_ALLOWED, cube, "needs a 2D texture but the bound texture is a cube map");
    /* the title's texm3x2 pair, program 0x4C421 = (1, 1, 17, 9): only t3 is read, stage 2 (DOTPRODUCT) needs no texture, stage 1 is the input of
     * both dot stages (kept, sampled), stage 0 is dropped (its texture may be anything, even missing) */
    gpu_combiner_texture pair[GPU_COMBINER_TEXTURE_STAGES];
    memset(pair, 0, sizeof pair);
    pair[1] = flat[1];
    pair[3] = flat[3];
    CHECK(modes_plan(3u, 0x4C421u, 0x110u, 0x00110000u, MODES_ALLOWED, pair, &plan, error, sizeof error) == GPU_PGRAPH_OK);
    CHECK(strcmp(plan.name, NAME_MODES_DOT_PAIR) == 0);
    CHECK(plan.texture_stages == ((1u << 1) | (1u << 3)) && plan.coordinate_stages == ((1u << 1) | (1u << 2) | (1u << 3)) && plan.cube_stages == 0u);
    CHECK(plan.key[36] == 0x110u && plan.key[37] == 0x00110000u && plan.key[(sizeof plan.key / sizeof plan.key[0]) - 3u] != 0u);
    /* the dot mapping is part of the name (another mapping is another module) */
    CHECK(modes_plan(3u, 0x4C421u, 0x000u, 0x00110000u, MODES_ALLOWED, pair, &plan, error, sizeof error) == GPU_PGRAPH_OK);
    CHECK(strcmp(plan.name, NAME_MODES_DOT_ZERO) == 0);
    /* the other definition of the title, 0x4C401: stage 1 is none, the normal map is stage 0 */
    gpu_combiner_texture from_zero[GPU_COMBINER_TEXTURE_STAGES];
    memset(from_zero, 0, sizeof from_zero);
    from_zero[0] = flat[0];
    from_zero[3] = flat[3];
    CHECK(modes_plan(3u, 0x4C401u, 0x110u, 0u, MODES_ALLOWED, from_zero, &plan, error, sizeof error) == GPU_PGRAPH_OK);
    CHECK(strcmp(plan.name, NAME_MODES_DOT_STAGE0) == 0 && plan.texture_stages == ((1u << 0) | (1u << 3)));
    /* the dependency really is needed: without the normal map the draw is refused by name, with the lookup in stage 3 missing as well */
    memset(pair, 0, sizeof pair);
    pair[3] = flat[3];
    expect_refusal_text(3u, 0x4C421u, 0x110u, 0x00110000u, MODES_ALLOWED, pair, "t1");
    /* programs xemu's psh.c asserts on */
    expect_refusal_text(3u, 17u << 15, 0u, 0u, MODES_ALLOWED, flat, "only stages 1 and 2");           /* DOTPRODUCT at stage 3 */
    expect_refusal_text(1u, 9u << 5, 0u, 0u, MODES_ALLOWED, flat, "without a dot product stage");      /* DOT_ST at stage 1 */
    expect_refusal_text(3u, (1u << 10) | (9u << 15), 0u, 0u, MODES_ALLOWED, flat, "without a dot product stage"); /* after a 2D stage */
    expect_refusal_text(3u, 0x4C421u, 0x110u, 0x00300000u, MODES_ALLOWED, flat, "not an earlier stage"); /* stage 3 reading stage 3 */
    /* an unread stage is dropped (xemu computes it and nothing reads it), except a clip plane (5) and DOT_ZW (10) */
    CHECK(modes_plan(0u, 1u | (2u << 10), 0u, 0u, GPU_COMBINER_INFER_ALL, flat, &plan, error, sizeof error) == GPU_PGRAPH_OK);
    CHECK(strcmp(plan.name, NAME_MODES_CONTROL) == 0 && plan.texture_stages == 1u);
    CHECK(modes_plan(0u, 1u | (3u << 5), 0u, 0u, GPU_COMBINER_INFER_ALL, flat, &plan, error, sizeof error) == GPU_PGRAPH_OK);
    CHECK(strcmp(plan.name, NAME_MODES_CONTROL) == 0 && !plan.uses_texture_modes);
    expect_refusal_text(0u, 1u | (5u << 10), 0u, 0u, MODES_ALLOWED, flat, "mode 5 is not modelled");
    expect_refusal_text(0u, 1u | (10u << 10), 0u, 0u, MODES_ALLOWED, flat, "mode 10 is not modelled");
    /* a READ stage of a mode xemu has and this plan does not (bump env map 6) is still refused by name */
    expect_refusal_text(1u, 6u << 5, 0u, 0u, MODES_ALLOWED, flat, "program mode 6 is not modelled");
    expect_refusal_text(1u, 2u << 5, 0u, 0u, MODES_ALLOWED, flat, "program mode 2 is not modelled"); /* 3D projective */
    expect_refusal_text(1u, 4u << 5, 0u, 0u, MODES_ALLOWED, flat, "program mode 4 is not modelled"); /* pass through */
}

static void test_allowed_paths(void)
{
    printf("test_allowed_paths\n");
    char error[200];
    gpu_combiner_plan plan;
    uint32_t words[WORDS];

    /* an unwritten alpha output word is taken as 0 only with the inference, and says so */
    build_words(pass_entries, COUNT(pass_entries), words);
    gpu_pgraph *pgraph = model(true);
    stream_builder stream = {0};
    stream_pixel_shader(&stream, words);
    stream_builder cut = {0};
    for (size_t pair = 0u; pair < stream.count; pair++) {
        if (gpu_pgraph_combiner_index(stream.pairs[pair].method) != 26) {
            stream_pair(&cut, stream.pairs[pair].method, stream.pairs[pair].data);
        }
    }
    CHECK(gpu_pgraph_decode(pgraph, cut.pairs, cut.count) == GPU_PGRAPH_OK);
    static const gpu_combiner_texture none[GPU_COMBINER_TEXTURE_STAGES] = {{0}};
    CHECK(gpu_combiner_plan_build(gpu_pgraph_state_now(pgraph), ALL_BITS, none, &plan, error,
                                  sizeof error) == GPU_PGRAPH_OK);
    CHECK((plan.used_inferences & GPU_PGRAPH_INFER_COMBINER_UNWRITTEN) != 0u);
    CHECK(strcmp(plan.name, COMBINER_NAME_PASS) != 0); /* the zero word is in the key */
    gpu_pgraph_destroy(pgraph);
    stream_free(&cut);
    stream_free(&stream);

    /* a spare register read before any stage wrote it: flagged, and a positive control for the
     * writes that DO define it (the pass fixture's final reads spare0 after stage 0 wrote it) */
    build_words(pass_entries, COUNT(pass_entries), words);
    words[34] = 0x0D200000u; /* A = r1, never written */
    CHECK(plan_of(words, GPU_COMBINER_INFER_ALL, NULL, &plan, error, sizeof error) == GPU_PGRAPH_OK);
    CHECK((plan.used_inferences & GPU_PGRAPH_INFER_COMBINER_INITIAL_STATE) != 0u);
    build_words(pass_entries, COUNT(pass_entries), words);
    CHECK(plan_of(words, GPU_COMBINER_INFER_ALL, NULL, &plan, error, sizeof error) == GPU_PGRAPH_OK);
    CHECK((plan.used_inferences & GPU_PGRAPH_INFER_COMBINER_INITIAL_STATE) == 0u);
    /* a mux reads spare0 alpha at the stage start: untextured, that is the 1.0 initial state */
    words[45] = 0xC0u | (1u << 14);
    CHECK(plan_of(words, GPU_COMBINER_INFER_ALL, NULL, &plan, error, sizeof error) == GPU_PGRAPH_OK);
    CHECK((plan.used_inferences & GPU_PGRAPH_INFER_COMBINER_INITIAL_STATE) != 0u);

    /* the sum register reads spare0 and the secondary colour: a final D = SUM with a stage that
     * writes nothing reads both, spare0 before any stage wrote it */
    memset(words, 0, sizeof words);
    words[53] = 0x11101u;
    words[8] = 0x0Eu;
    words[9] = 0x80u;
    CHECK(plan_of(words, GPU_COMBINER_INFER_ALL, NULL, &plan, error, sizeof error) == GPU_PGRAPH_OK);
    CHECK((plan.reads & ((1u << 14) | (1u << 12) | (1u << 5))) == ((1u << 14) | (1u << 12) | (1u << 5)));
    CHECK((plan.used_inferences & GPU_PGRAPH_INFER_COMBINER_INITIAL_STATE) != 0u);
    CHECK((plan.used_inferences & GPU_PGRAPH_INFER_COMBINER_COLOUR_RANGE) != 0u);

    /* a texture register: refused without a test texture, drawn with one, and the stage program
     * is then a flagged inference unless the stream wrote it */
    static const uint8_t texel[4] = {1u, 2u, 3u, 255u};
    gpu_combiner_texture textures[GPU_COMBINER_TEXTURE_STAGES] = {{0}};
    textures[0].rgba = texel;
    textures[0].width = 1u;
    textures[0].height = 1u;
    build_words(pass_entries, COUNT(pass_entries), words);
    words[34] = 0x08200000u; /* A = t0 */
    CHECK(plan_of(words, ALL_BITS, NULL, &plan, error, sizeof error) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(error, "no test texture") != NULL);
    CHECK(plan_of(words, ALL_BITS, textures, &plan, error, sizeof error) == GPU_PGRAPH_OK);
    CHECK(plan.texture_stages == 1u);
    CHECK((plan.used_inferences & GPU_PGRAPH_INFER_COMBINER_STAGE_PROGRAM) != 0u);
    CHECK((plan.used_inferences & GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING) != 0u);
    CHECK(plan_of(words, ALL_BITS & ~GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING, textures, &plan, error,
                  sizeof error) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(plan_of(words, ALL_BITS & ~GPU_PGRAPH_INFER_COMBINER_STAGE_PROGRAM, textures, &plan, error,
                  sizeof error) == GPU_PGRAPH_ERR_UNMEASURED);
    char textured_name[GPU_COMBINER_NAME_BYTES];
    CHECK(plan_of(words, ALL_BITS, textures, &plan, error, sizeof error) == GPU_PGRAPH_OK);
    snprintf(textured_name, sizeof textured_name, "%s", plan.name);
    CHECK(strcmp(textured_name, COMBINER_NAME_PASS) != 0);
    /* the same with the stream's own program word, mode 1 on stage 0: no stage-program inference */
    {
        gpu_pgraph *programmed = model(true);
        stream_builder program_stream = {0};
        stream_pixel_shader(&program_stream, words);
        stream_pair(&program_stream, 0x1E70u, 1u);
        CHECK(gpu_pgraph_decode(programmed, program_stream.pairs, program_stream.count) == GPU_PGRAPH_OK);
        CHECK(gpu_combiner_plan_build(gpu_pgraph_state_now(programmed), ALL_BITS, textures, &plan, error,
                                      sizeof error) == GPU_PGRAPH_OK);
        CHECK((plan.used_inferences & GPU_PGRAPH_INFER_COMBINER_STAGE_PROGRAM) == 0u);
        CHECK(strcmp(plan.name, textured_name) == 0); /* same configuration, same module */
        /* the program says mode 1 and no test texture exists for the stage: refused */
        CHECK(gpu_combiner_plan_build(gpu_pgraph_state_now(programmed), ALL_BITS, none, &plan, error,
                                      sizeof error) == GPU_PGRAPH_ERR_UNMEASURED);
        CHECK(strstr(error, "no test texture") != NULL);
        /* T1207: the program says "none" for the stage the combiner reads: defined as (0,0,0,1) like xemu's
         * PS_TEXTUREMODES_NONE, no texture is sampled (stage bit clear), even when one was supplied */
        gpu_pgraph *none_model = model(true);
        stream_builder none_stream = {0};
        stream_pixel_shader(&none_stream, words);
        stream_pair(&none_stream, 0x1E70u, 0u);
        CHECK(gpu_pgraph_decode(none_model, none_stream.pairs, none_stream.count) == GPU_PGRAPH_OK);
        CHECK(gpu_combiner_plan_build(gpu_pgraph_state_now(none_model), ALL_BITS, textures, &plan, error,
                                      sizeof error) == GPU_PGRAPH_OK);
        CHECK(plan.texture_stages == 0u);
        CHECK(strcmp(plan.name, textured_name) != 0); /* a different module from the sampled one */
        CHECK(gpu_combiner_plan_build(gpu_pgraph_state_now(none_model), ALL_BITS, none, &plan, error,
                                      sizeof error) == GPU_PGRAPH_OK); /* and it needs no test texture at all */
        CHECK(plan.texture_stages == 0u);
        /* a texture of the wrong size */
        gpu_combiner_texture empty[GPU_COMBINER_TEXTURE_STAGES] = {{0}};
        empty[0].rgba = texel;
        CHECK(gpu_combiner_plan_build(gpu_pgraph_state_now(programmed), ALL_BITS, empty, &plan, error,
                                      sizeof error) == GPU_PGRAPH_ERR_UNMEASURED);
        CHECK(strstr(error, "outside 1..4096") != NULL);
        gpu_pgraph_destroy(none_model);
        gpu_pgraph_destroy(programmed);
        stream_free(&none_stream);
        stream_free(&program_stream);
    }
    /* a test texture nobody reads does not change the module or the plan */
    build_words(pass_entries, COUNT(pass_entries), words);
    CHECK(plan_of(words, ALL_BITS, textures, &plan, error, sizeof error) == GPU_PGRAPH_OK);
    CHECK(strcmp(plan.name, COMBINER_NAME_PASS) == 0 && plan.texture_stages == 0u);
}

static void test_spirv_scan(void)
{
    printf("test_spirv_scan\n");
    uint32_t mask = 0xFFu;
    CHECK(gpu_spirv_interface_locations(draw_vertex_words, sizeof draw_vertex_words / 4u, 3u, &mask));
    CHECK(mask == 0x1u); /* oD0 at location 0, gl_Position is a built-in and not counted */
    CHECK(gpu_spirv_interface_locations(draw_vertex_words, sizeof draw_vertex_words / 4u, 1u, &mask));
    CHECK(mask == 0x6u); /* v1 and v2 at locations 1 and 2 */
    static const struct {
        const uint32_t *words;
        size_t count;
        uint32_t inputs;
    } fragments[] = {
        {combiner_words_pass, sizeof combiner_words_pass / 4u, 0x1u},
        {combiner_words_multiply, sizeof combiner_words_multiply / 4u, 0x1u},
        {combiner_words_final, sizeof combiner_words_final / 4u, 0x0u},
        {combiner_words_twovary, sizeof combiner_words_twovary / 4u, 0x3u},
        {tex_probe_words, sizeof tex_probe_words / 4u, 0x0u},
    };
    for (size_t i = 0u; i < COUNT(fragments); i++) {
        mask = 0xFFu;
        CHECK(gpu_spirv_interface_locations(fragments[i].words, fragments[i].count, 1u, &mask));
        CHECK(mask == fragments[i].inputs);
        mask = 0xFFu;
        CHECK(gpu_spirv_interface_locations(fragments[i].words, fragments[i].count, 3u, &mask));
        CHECK(mask == 0x1u); /* frag_color */
    }
    const uint32_t garbage[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    CHECK(!gpu_spirv_interface_locations(garbage, 8u, 1u, &mask));
    CHECK(!gpu_spirv_interface_locations(draw_vertex_words, 3u, 1u, &mask));
    CHECK(!gpu_spirv_interface_locations(NULL, 0u, 1u, &mask));
}

/* --- device ------------------------------------------------------------------------------- */

#define MEMORY_BASE 0x00600000u

typedef struct {
    float position[3];
    float v2[4];
} float_vertex;

static uint8_t memory[0x100];
static const char *const vertex_names[] = {SYNTHETIC_PROGRAM_NAME};
static const struct gpu_vsh_table vertex_table = {0u, 0u, NULL, 0u, NULL, 1u, vertex_names};
static const char *const fragment_names[] = {COMBINER_NAME_PASS, COMBINER_NAME_MULTIPLY,
                                             COMBINER_NAME_FINAL, COMBINER_NAME_TWOVARY};
static const struct gpu_vsh_table fragment_table = {0u, 0u, NULL, 0u, NULL, 4u, fragment_names};

static bool load_vertex(void *context, uint32_t module, const uint32_t **words, size_t *word_count)
{
    (void)context;
    if (module != 0u) {
        return false;
    }
    *words = draw_vertex_words;
    *word_count = sizeof draw_vertex_words / sizeof(uint32_t);
    return true;
}

static bool load_fragment(void *context, uint32_t module, const uint32_t **words, size_t *word_count)
{
    (void)context;
    switch (module) {
    case 0u: *words = combiner_words_pass; *word_count = sizeof combiner_words_pass / 4u; return true;
    case 1u: *words = combiner_words_multiply; *word_count = sizeof combiner_words_multiply / 4u; return true;
    case 2u: *words = combiner_words_final; *word_count = sizeof combiner_words_final / 4u; return true;
    case 3u: *words = combiner_words_twovary; *word_count = sizeof combiner_words_twovary / 4u; return true;
    default: return false;
    }
}

static void fill_memory(void)
{
    static const float corners[3][2] = {{-0.9f, -0.9f}, {0.9f, -0.9f}, {0.0f, 0.9f}};
    memset(memory, 0, sizeof memory);
    for (uint32_t i = 0u; i < 3u; i++) {
        float_vertex vertex = {{corners[i][0], corners[i][1], 0.5f}, {1.0f, 1.0f, 1.0f, 1.0f}};
        memcpy(memory + i * sizeof vertex, &vertex, sizeof vertex);
    }
}

static gpu_pgraph_backend make_backend(fake_guest *guest, bool combiner)
{
    gpu_pgraph_backend backend = {0};
    backend.table = &vertex_table;
    backend.read_guest = fake_guest_read;
    backend.load_module = load_vertex;
    backend.context = guest;
    backend.allowed_inferences = ALL_BITS;
    backend.combiner = combiner;
    backend.fragment_table = &fragment_table;
    backend.load_fragment_module = load_fragment;
    return backend;
}

/* One triangle through the synthetic vertex program, the combiner words first. */
static void build_draw(stream_builder *stream, const uint32_t *combiner_words)
{
    static const float offset[4] = {32.0f, 32.0f, 0.0f, 0.0f};
    static const float scale[4] = {32.0f, -32.0f, 1.0f, 0.0f};
    static const float c3_zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    stream_viewport(stream, offset, scale);
    stream_pair(stream, GPU_PGRAPH_EXECUTION_MODE, 6u);
    stream_program(stream, 0u, synthetic_program, 1u);
    stream_pair(stream, GPU_PGRAPH_PROGRAM_START, 0u);
    stream_constants(stream, 3u, c3_zero, 4u);
    stream_array(stream, 1u, MEMORY_BASE, array_format(28u, 3u, GPU_PGRAPH_TYPE_F));
    stream_array(stream, 2u, MEMORY_BASE + 12u, array_format(28u, 4u, GPU_PGRAPH_TYPE_F));
    if (combiner_words != NULL) {
        stream_pixel_shader(stream, combiner_words);
    }
    stream_draw_arrays(stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
}

static bool pixel_is(const gpu_image *image, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b,
                     uint8_t a)
{
    const uint8_t *p = image->pixels + gpu_image_offset(image, x, y);
    return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}

static gpu_pgraph_result replay(gpu_device *device, const uint32_t *combiner_words, bool combiner,
                                uint32_t allowed, gpu_image *frame, gpu_pgraph_report *report)
{
    static const float clear[4] = {0.2f, 0.2f, 0.2f, 1.0f};
    fake_guest guest = {MEMORY_BASE, memory, sizeof memory};
    gpu_pgraph_backend backend = make_backend(&guest, combiner);
    backend.allowed_inferences = allowed;
    gpu_pgraph *pgraph = model(combiner);
    stream_builder stream = {0};
    build_draw(&stream, combiner_words);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_count(pgraph) == 1u);
    const gpu_pgraph_result result =
        gpu_pgraph_replay(pgraph, device, &backend, WIDTH, HEIGHT, clear, frame, report);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
    return result;
}

static void test_draws(gpu_device *device)
{
    printf("test_draws (%s)\n", gpu_device_name(device));
    fill_memory();
    uint32_t pass_words[WORDS], multiply_words[WORDS], final_words[WORDS], twovary_words[WORDS];
    build_words(pass_entries, COUNT(pass_entries), pass_words);
    build_words(multiply_entries, COUNT(multiply_entries), multiply_words);
    build_words(final_entries, COUNT(final_entries), final_words);
    build_words(twovary_entries, COUNT(twovary_entries), twovary_words);
    gpu_pgraph_report report;

    /* the default stage: oD0 unchanged, and the combiner methods in the stream change nothing */
    gpu_image fixed = {0};
    CHECK(replay(device, NULL, false, GPU_PGRAPH_INFER_ALL, &fixed, &report) == GPU_PGRAPH_OK);
    CHECK(fixed.pixels != NULL && report.drawn == 1u);
    gpu_image ignored = {0};
    CHECK(replay(device, multiply_words, false, GPU_PGRAPH_INFER_ALL, &ignored, &report) == GPU_PGRAPH_OK);
    CHECK(ignored.pixels != NULL && fixed.pixels != NULL &&
          memcmp(ignored.pixels, fixed.pixels, (size_t)WIDTH * HEIGHT * 4u) == 0);
    CHECK((report.used_inferences & GPU_PGRAPH_INFER_COMBINER_ALL) == 0u);
    if (fixed.pixels != NULL) {
        CHECK(pixel_is(&fixed, 32u, 40u, 255u, 255u, 255u, 255u));
        CHECK(pixel_is(&fixed, 2u, 2u, 51u, 51u, 51u, 255u));
    }

    /* pass: through the combiner, the same frame byte for byte */
    gpu_image passed = {0};
    CHECK(replay(device, pass_words, true, ALL_BITS, &passed, &report) == GPU_PGRAPH_OK);
    CHECK(passed.pixels != NULL && report.drawn == 1u);
    CHECK((report.used_inferences & GPU_PGRAPH_INFER_COMBINER_COLOUR_RANGE) != 0u);
    CHECK((report.used_inferences & GPU_PGRAPH_INFER_COMBINER_CONSTANT_BYTES) == 0u);
    if (passed.pixels != NULL && fixed.pixels != NULL) {
        CHECK(memcmp(passed.pixels, fixed.pixels, (size_t)WIDTH * HEIGHT * 4u) == 0);
    }

    gpu_image multiplied = {0};
    CHECK(replay(device, multiply_words, true, ALL_BITS, &multiplied, &report) == GPU_PGRAPH_OK);
    CHECK((report.used_inferences & GPU_PGRAPH_INFER_COMBINER_CONSTANT_BYTES) != 0u);
    if (multiplied.pixels != NULL) {
        CHECK(pixel_is(&multiplied, 32u, 40u, 128u, 96u, 64u, 255u)); /* c0 * c1, per channel */
        CHECK(pixel_is(&multiplied, 2u, 2u, 51u, 51u, 51u, 255u));
    }
    gpu_image finalised = {0};
    CHECK(replay(device, final_words, true, ALL_BITS, &finalised, &report) == GPU_PGRAPH_OK);
    if (finalised.pixels != NULL) {
        CHECK(pixel_is(&finalised, 32u, 40u, 128u, 32u, 16u, 128u)); /* final E * F, final c0 alpha */
        CHECK(pixel_is(&finalised, 2u, 2u, 51u, 51u, 51u, 255u));
    }
    CHECK(multiplied.pixels != NULL && finalised.pixels != NULL &&
          memcmp(multiplied.pixels, finalised.pixels, (size_t)WIDTH * HEIGHT * 4u) != 0);

    /* a constant update after the writer reaches the pixels: draw twice, factor0[0] changed in
     * between (red byte 0xFF -> 0x80), the second draw covers the first */
    {
        static const float clear[4] = {0.2f, 0.2f, 0.2f, 1.0f};
        fake_guest guest = {MEMORY_BASE, memory, sizeof memory};
        gpu_pgraph_backend backend = make_backend(&guest, true);
        gpu_pgraph *pgraph = model(true);
        stream_builder stream = {0};
        build_draw(&stream, multiply_words);
        stream_pair(&stream, 0x0A60u, 0x00808040u);
        stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
        CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
        CHECK(gpu_pgraph_draw_count(pgraph) == 2u && gpu_pgraph_snapshot_count(pgraph) == 2u);
        gpu_image updated = {0};
        CHECK(gpu_pgraph_replay(pgraph, device, &backend, WIDTH, HEIGHT, clear, &updated, &report) ==
              GPU_PGRAPH_OK);
        CHECK(report.drawn == 2u);
        CHECK(updated.pixels != NULL && pixel_is(&updated, 32u, 40u, 64u, 96u, 64u, 255u));
        gpu_image_free(&updated);
        stream_free(&stream);
        gpu_pgraph_destroy(pgraph);
    }

    /* refusals */
    gpu_image none = {0};
    CHECK(replay(device, twovary_words, true, ALL_BITS, &none, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(none.pixels == NULL && report.failed_draw == 0u);
    CHECK(strstr(report.error, "varyings 0x3") != NULL && strstr(report.error, "writes only 0x1") != NULL);
    CHECK(replay(device, pass_words, true, GPU_PGRAPH_INFER_ALL, &none, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "INFERRED and not allowed") != NULL);
    CHECK(replay(device, NULL, true, ALL_BITS, &none, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "never written") != NULL || strstr(report.error, "stage count") != NULL);
    uint32_t fog_words[WORDS];
    memcpy(fog_words, pass_words, sizeof fog_words);
    fog_words[34] = 0x03200000u;
    CHECK(replay(device, fog_words, true, ALL_BITS, &none, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "fog register") != NULL);
    uint32_t unknown_words[WORDS];
    memcpy(unknown_words, pass_words, sizeof unknown_words);
    unknown_words[45] = 0xC0u | (1u << 16); /* x2 scale: a valid configuration with no module */
    CHECK(replay(device, unknown_words, true, ALL_BITS, &none, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "no translated module") != NULL && strstr(report.error, "combiner_") != NULL);
    CHECK(none.pixels == NULL);

    gpu_image_free(&fixed);
    gpu_image_free(&ignored);
    gpu_image_free(&passed);
    gpu_image_free(&multiplied);
    gpu_image_free(&finalised);
}

/* The texture plumbing of gpu_vsh_render: constants at binding 1, a sampler at binding 2. */
static void test_texture(gpu_device *device)
{
    printf("test_texture (%s)\n", gpu_device_name(device));
    static const uint8_t texels[2u * 2u * 4u] = {
        10u, 20u, 30u, 255u,  255u, 200u, 100u, 255u, /* row 0 */
        1u,  2u,  3u,  255u,  4u,   5u,   6u,   255u, /* row 1 */
    };
    float attributes[3u * GPU_VSH_ATTRIBUTE_FLOATS];
    memset(attributes, 0, sizeof attributes);
    static const float corners[3][4] = {{-1.0f, -1.0f, 0.5f, 1.0f}, {3.0f, -1.0f, 0.5f, 1.0f},
                                        {-1.0f, 3.0f, 0.5f, 1.0f}};
    for (uint32_t vertex = 0u; vertex < 3u; vertex++) {
        memcpy(attributes + vertex * GPU_VSH_ATTRIBUTE_FLOATS + 4u, corners[vertex], sizeof corners[vertex]);
        const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        memcpy(attributes + vertex * GPU_VSH_ATTRIBUTE_FLOATS + 8u, white, sizeof white);
    }
    float vertex_constants[GPU_VSH_CONSTANT_ROWS * 4u] = {0};
    float fragment_constants[GPU_VSH_FRAGMENT_VEC4S * 4u] = {0};
    fragment_constants[0] = 0.25f;
    fragment_constants[1] = 1.0f;
    fragment_constants[2] = 1.0f;
    fragment_constants[3] = 1.0f;
    gpu_vsh_fragment fragment = {
        .words = tex_probe_words,
        .word_count = sizeof tex_probe_words / sizeof(uint32_t),
        .constants = fragment_constants,
    };
    fragment.textures[0].rgba = texels;
    fragment.textures[0].width = 2u;
    fragment.textures[0].height = 2u;
    gpu_vsh_draw draw = {
        .words = draw_vertex_words,
        .word_count = sizeof draw_vertex_words / sizeof(uint32_t),
        .vertex_count = 3u,
        .attributes = attributes,
        .constants = vertex_constants,
        .fragment = &fragment,
    };
    static const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    gpu_image image = {0};
    CHECK(gpu_vsh_render(device, 8u, 8u, clear, &draw, &image) == GPU_OK);
    CHECK(image.pixels != NULL);
    if (image.pixels != NULL) {
        /* the probe reads a = texel (1, 0) by clamping past the right edge (row 0 on top) and b =
         * texel (1, 1) by a nearest lookup at (0.55, 0.75): (a.r, a.g, b.b, a.a) times the
         * constants block's first vec4 (0.25, 1, 1, 1) */
        CHECK(pixel_is(&image, 4u, 4u, 64u, 200u, 6u, 255u));
        CHECK(pixel_is(&image, 0u, 0u, 64u, 200u, 6u, 255u));
        CHECK(pixel_is(&image, 7u, 7u, 64u, 200u, 6u, 255u));
    }
    gpu_image_free(&image);

    /* a different texel (1, 0) under the sample point, so the result follows the texture */
    uint8_t other[2u * 2u * 4u];
    memcpy(other, texels, sizeof other);
    other[4] = 40u;
    other[5] = 60u;
    other[6] = 80u;
    fragment.textures[0].rgba = other;
    CHECK(gpu_vsh_render(device, 8u, 8u, clear, &draw, &image) == GPU_OK);
    CHECK(image.pixels != NULL && pixel_is(&image, 4u, 4u, 10u, 60u, 6u, 255u));
    gpu_image_free(&image);

    /* argument checks */
    fragment.textures[0].width = 0u;
    CHECK(gpu_vsh_render(device, 8u, 8u, clear, &draw, &image) == GPU_ERR_ARGUMENT);
    fragment.textures[0].width = 2u;
    fragment.constants = NULL;
    CHECK(gpu_vsh_render(device, 8u, 8u, clear, &draw, &image) == GPU_ERR_ARGUMENT);
    fragment.constants = fragment_constants;
    fragment.words = NULL;
    CHECK(gpu_vsh_render(device, 8u, 8u, clear, &draw, &image) == GPU_ERR_ARGUMENT);
}

static int run_device(const char *selector)
{
    gpu_device *device = NULL;
    const gpu_result created = gpu_device_create_selected(selector, &device);
    if (created != GPU_OK) {
        printf("SKIP %s: %s\n", selector, gpu_result_string(created));
        return 0;
    }
    test_draws(device);
    test_texture(device);
    gpu_device_destroy(device);
    return 1;
}

/* --- T847: a combiner configuration in no table goes to the maker ---------------------------- */

typedef struct {
    unsigned calls;
    bool answer;
    bool register_module;
    bool fragment;
    size_t bytes;
    uint8_t block[GPU_PGRAPH_COMBINER_BLOCK_BYTES];
    char name[GPU_COMBINER_NAME_BYTES];
    const char *names[4];
    struct gpu_vsh_table table;
} maker_probe;

static bool probe_make(void *context, bool fragment, const char *name, const uint8_t *bytes, size_t byte_count, char *error,
                       size_t error_bytes)
{
    maker_probe *probe = context;
    probe->calls++;
    probe->fragment = fragment;
    probe->bytes = byte_count;
    snprintf(probe->name, sizeof probe->name, "%s", name);
    if (byte_count == sizeof probe->block) {
        memcpy(probe->block, bytes, byte_count);
    }
    if (!probe->answer) {
        snprintf(error, error_bytes, "refused by the probe");
        return false;
    }
    if (probe->register_module) {
        probe->names[probe->table.module_count] = probe->name;
        probe->table.module_count++;
    }
    return true;
}

static void test_make_module(void)
{
    printf("test_make_module\n");
    uint32_t words[WORDS];
    build_words(multiply_entries, COUNT(multiply_entries), words);
    words[GPU_PGRAPH_COMBINER_WORDS - 1u] = 0xA5A5A5A5u; /* the last word is part of the definition block too */
    gpu_pgraph *pgraph = model(true);
    load_state(pgraph, words);
    maker_probe probe;
    memset(&probe, 0, sizeof probe);
    probe.table.module_names = probe.names;
    gpu_pgraph_backend backend = {0};
    backend.allowed_inferences = ALL_BITS;
    backend.combiner = true;
    backend.fragment_table = &probe.table;
    backend.load_fragment_module = load_fragment;
    gpu_pgraph_fragment fragment;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);

    /* no maker: a miss is the named refusal, as before */
    CHECK(gpu_pgraph_resolve_fragment(gpu_pgraph_state_now(pgraph), &backend, &fragment, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "no translated module in the fragment table") != NULL && strstr(report.error, "translating") == NULL);
    CHECK(probe.calls == 0u);

    /* a maker that refuses: the refusal carries its message */
    backend.make_module = probe_make;
    backend.make_module_context = &probe;
    probe.answer = false;
    CHECK(gpu_pgraph_resolve_fragment(gpu_pgraph_state_now(pgraph), &backend, &fragment, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(probe.calls == 1u && probe.fragment);
    CHECK(strstr(report.error, "and translating it failed: refused by the probe") != NULL);

    /* a maker that says yes but does not register the module: the retry misses, still a refusal */
    probe.answer = true;
    CHECK(gpu_pgraph_resolve_fragment(gpu_pgraph_state_now(pgraph), &backend, &fragment, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(probe.calls == 2u);

    /* a maker that registers it: the draw resolves to the new module, the block is the 57 words and three zeros */
    probe.register_module = true;
    CHECK(gpu_pgraph_resolve_fragment(gpu_pgraph_state_now(pgraph), &backend, &fragment, &report) == GPU_PGRAPH_OK);
    CHECK(probe.calls == 3u && fragment.module == 0u && strcmp(probe.name, fragment.plan.name) == 0);
    CHECK(strcmp(fragment.plan.name, COMBINER_NAME_MULTIPLY) == 0);
    CHECK(probe.bytes == GPU_PGRAPH_COMBINER_BLOCK_BYTES);
    bool block_ok = true;
    for (uint32_t word = 0u; word < GPU_PGRAPH_COMBINER_WORDS; word++) {
        const uint32_t read = (uint32_t)probe.block[word * 4u] | (uint32_t)probe.block[word * 4u + 1u] << 8 |
                              (uint32_t)probe.block[word * 4u + 2u] << 16 | (uint32_t)probe.block[word * 4u + 3u] << 24;
        block_ok = block_ok && read == words[word];
    }
    for (uint32_t byte = GPU_PGRAPH_COMBINER_WORDS * 4u; byte < GPU_PGRAPH_COMBINER_BLOCK_BYTES; byte++) {
        block_ok = block_ok && probe.block[byte] == 0u;
    }
    CHECK(block_ok);

    /* a table hit never reaches the maker */
    backend.fragment_table = &fragment_table;
    CHECK(gpu_pgraph_resolve_fragment(gpu_pgraph_state_now(pgraph), &backend, &fragment, &report) == GPU_PGRAPH_OK);
    CHECK(probe.calls == 3u && fragment.module == 1u);
    gpu_pgraph_destroy(pgraph);
}

/* T888: the `_alpha` module's function constant (plan.constants vec4 19 y) for each alpha function. The expectations are written out
 * from the GL enum order the NV2A shares (0x200 NEVER, LESS, EQUAL, LEQUAL, GREATER, NOTEQUAL, 0x206 GEQUAL), which is the index
 * chain tools/nv2a_combiner/glsl.py compiles into the module (0 never, 1 <, 2 ==, 3 <=, 4 >, 5 !=, 6 >=), not read from the code's
 * own subtraction. ALWAYS (0x207) and the disabled test stay in the plain module. */
static void test_alpha_module_constants(void)
{
    printf("test_alpha_module_constants\n");
    static const struct {
        uint32_t function;
        float index;
    } expected[] = {{0x0200u, 0.0f}, {0x0201u, 1.0f}, {0x0202u, 2.0f}, {0x0203u, 3.0f},
                    {0x0204u, 4.0f}, {0x0205u, 5.0f}, {0x0206u, 6.0f}};
    CHECK(COUNT(expected) == 7u);
    uint32_t words[WORDS];
    build_words(multiply_entries, COUNT(multiply_entries), words);
    maker_probe probe;
    for (size_t i = 0u; i < COUNT(expected); i++) {
        const uint32_t reference = 0x40u + (uint32_t)i * 0x11u;
        gpu_pgraph *pgraph = model(true);
        gpu_pgraph_set_output_groups(pgraph, GPU_PGRAPH_OUTPUT_ALPHA_TEST);
        stream_builder stream = {0};
        stream_pixel_shader(&stream, words);
        stream_pair(&stream, 0x0300u, 1u);
        stream_pair(&stream, 0x033Cu, expected[i].function);
        stream_pair(&stream, 0x0340u, reference);
        CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
        stream_free(&stream);
        memset(&probe, 0, sizeof probe);
        probe.table.module_names = probe.names;
        probe.answer = true;
        probe.register_module = true;
        gpu_pgraph_backend backend = {0};
        backend.allowed_inferences = ALL_BITS;
        backend.combiner = true;
        backend.output_groups = GPU_PGRAPH_OUTPUT_ALPHA_TEST;
        backend.fragment_table = &probe.table;
        backend.load_fragment_module = load_fragment;
        backend.make_module = probe_make;
        backend.make_module_context = &probe;
        gpu_pgraph_fragment fragment;
        gpu_pgraph_report report;
        memset(&report, 0, sizeof report);
        CHECK(gpu_pgraph_resolve_fragment(gpu_pgraph_state_now(pgraph), &backend, &fragment, &report) == GPU_PGRAPH_OK);
        CHECK(fragment.alpha_in_module);
        CHECK(strcmp(fragment.plan.name, COMBINER_NAME_MULTIPLY "_alpha") == 0);
        CHECK(fragment.plan.constants[19u * 4u] == (float)reference);
        CHECK(fragment.plan.constants[19u * 4u + 1u] == expected[i].index);
        gpu_pgraph_destroy(pgraph);
    }
}

int main(void)
{
    test_method_map();
    test_decoder();
    test_names_and_constants();
    test_refusals();
    test_allowed_paths();
    test_texture_modes();
    test_alpha_module_constants();
    test_spirv_scan();
    test_make_module();
    if (failures != 0) {
        printf("%d checks, %d failures\n", checks, failures);
        return 1;
    }
    if (!gpu_vulkan_available()) {
        printf("SKIP: no Vulkan loader on this machine (the decoder and plan checks ran)\n");
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
