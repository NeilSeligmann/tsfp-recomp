/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Edges of gpu_combiner.c (plan, refusals, inferences, SPIR-V interface scan) that the mutation
 * sweep tools/mutate/sets/gpu_combiner.py (T259) found UNPINNED by test_gpu_combiner and by the
 * C-versus-Python parity in tests/test_gpu_combiner_replay.py. No Vulkan device is needed.
 *
 * Configurations are built from `cfg`: every word the plan REQUIRES for a stage count is present
 * and zero, so the base plan is legal with no inference at all. A test then changes the one word
 * whose handling it pins, or removes the one word whose absence must be inferred. Streams are
 * written method by method, because stream_pixel_shader writes the words a title would, and a
 * test of an UNWRITTEN word must be able to leave it out.
 */
#include "gpu_combiner.h"
#include "gpu_combiner_words.h"
#include "gpu_standin_words.h"
#include "gpu_pgraph.h"
#include "gpu_pgraph_test_support.h"

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

#define WORDS GPU_PGRAPH_COMBINER_WORDS
#define EVERYTHING GPU_COMBINER_INFER_ALL
#define IDX_FINAL0 8u
#define IDX_FINAL1 9u
#define IDX_FACTOR0 10u
#define IDX_FACTOR1 18u
#define IDX_ALPHA_OCW 26u
#define IDX_COLOR_ICW 34u
#define IDX_FINAL_C0 43u
#define IDX_FINAL_C1 44u
#define IDX_COLOR_OCW 45u
#define IDX_CONTROL 53u
#define IDX_STAGE_PROGRAM 54u
#define REG_C0 1u
#define REG_C1 2u
#define REG_SPARE0 12u
#define REG_SUM 14u
#define REG_PROD 15u
#define EACH0 (1u << 12)
#define EACH1 (1u << 16)

typedef struct {
    uint32_t value[WORDS];
    bool written[WORDS];
} cfg;

static void cfg_set(cfg *c, uint32_t index, uint32_t value)
{
    c->value[index] = value;
    c->written[index] = true;
}

static void cfg_base(cfg *c, uint32_t stages, uint32_t control_flags)
{
    memset(c, 0, sizeof *c);
    cfg_set(c, IDX_CONTROL, stages | control_flags);
    for (uint32_t stage = 0u; stage < stages; stage++) {
        cfg_set(c, stage, 0u);
        cfg_set(c, IDX_ALPHA_OCW + stage, 0u);
        cfg_set(c, IDX_COLOR_ICW + stage, 0u);
        cfg_set(c, IDX_COLOR_OCW + stage, 0u);
    }
    cfg_set(c, IDX_FINAL0, 0u);
    cfg_set(c, IDX_FINAL1, 0u);
}

static uint32_t method_of(uint32_t index)
{
    for (uint32_t method = 0u; method < 0x2000u; method++) {
        if (gpu_pgraph_combiner_index(method) == (int)index) {
            return method;
        }
    }
    return 0u;
}

static gpu_pgraph_result plan_cfg(const cfg *c, uint32_t allowed, const gpu_combiner_texture *textures,
                                  gpu_combiner_plan *plan, char *error, size_t error_size)
{
    static const gpu_combiner_texture none[GPU_COMBINER_TEXTURE_STAGES] = {{0}};
    gpu_pgraph *pgraph = gpu_pgraph_create();
    gpu_pgraph_set_combiner(pgraph, true);
    stream_builder stream = {0};
    for (uint32_t index = 0u; index < WORDS; index++) {
        if (c->written[index]) {
            stream_pair(&stream, method_of(index), c->value[index]);
        }
    }
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    const gpu_pgraph_result result = gpu_combiner_plan_build(
        gpu_pgraph_state_now(pgraph), allowed, textures != NULL ? textures : none, plan, error, error_size);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
    return result;
}

/* The plan is refused (UNMEASURED) with `text` in the reason. */
static void expect_refused(const cfg *c, uint32_t allowed, const gpu_combiner_texture *textures,
                           const char *text, int line)
{
    gpu_combiner_plan plan;
    char error[400];
    const gpu_pgraph_result result = plan_cfg(c, allowed, textures, &plan, error, sizeof error);
    checks++;
    if (result != GPU_PGRAPH_ERR_UNMEASURED || strstr(error, text) == NULL) {
        failures++;
        printf("  FAIL line %d: expected a refusal containing \"%s\", got result %d \"%s\"\n", line, text,
               (int)result, error);
    }
}

static void expect_planned(const cfg *c, uint32_t allowed, const gpu_combiner_texture *textures,
                           gpu_combiner_plan *plan, int line)
{
    char error[400];
    const gpu_pgraph_result result = plan_cfg(c, allowed, textures, plan, error, sizeof error);
    checks++;
    if (result != GPU_PGRAPH_OK) {
        failures++;
        printf("  FAIL line %d: expected a plan, got result %d \"%s\"\n", line, (int)result, error);
    }
}

#define REFUSED(c, allowed, textures, text) expect_refused((c), (allowed), (textures), (text), __LINE__)
#define PLANNED(c, allowed, textures, plan) expect_planned((c), (allowed), (textures), (plan), __LINE__)

static void test_base_is_legal(void)
{
    printf("test_base_is_legal\n");
    cfg c;
    cfg_base(&c, 1u, 0u);
    gpu_combiner_plan plan;
    PLANNED(&c, 0u, NULL, &plan);
    CHECK(plan.used_inferences == 0u && plan.stage_count == 1u && plan.texture_stages == 0u);
    cfg_base(&c, 8u, 0u);
    PLANNED(&c, 0u, NULL, &plan);
    CHECK(plan.stage_count == 8u && plan.used_inferences == 0u);
    cfg_base(&c, 8u, 0u);
    c.value[IDX_CONTROL] = 9u;
    REFUSED(&c, EVERYTHING, NULL, "outside 1 to 8");
    c.value[IDX_CONTROL] = 0u;
    REFUSED(&c, EVERYTHING, NULL, "outside 1 to 8");
}

static void test_null_arguments(void)
{
    printf("test_null_arguments\n");
    cfg c;
    cfg_base(&c, 1u, 0u);
    gpu_pgraph *pgraph = gpu_pgraph_create();
    gpu_pgraph_set_combiner(pgraph, true);
    gpu_combiner_plan plan;
    static const gpu_combiner_texture none[GPU_COMBINER_TEXTURE_STAGES] = {{0}};
    CHECK(gpu_combiner_plan_build(gpu_pgraph_state_now(pgraph), 0u, NULL, &plan, NULL, 0u) ==
          GPU_PGRAPH_ERR_ARGUMENT);
    CHECK(gpu_combiner_plan_build(NULL, 0u, none, &plan, NULL, 0u) == GPU_PGRAPH_ERR_ARGUMENT);
    CHECK(gpu_combiner_plan_build(gpu_pgraph_state_now(pgraph), 0u, none, NULL, NULL, 0u) ==
          GPU_PGRAPH_ERR_ARGUMENT);
    gpu_pgraph_destroy(pgraph);
}

/* ---- which factor words a read requires ------------------------------------------------- */

static void check_factor_reads(uint32_t reg, uint32_t each_flag, uint32_t factor_base, uint32_t word_after)
{
    char needle[32];
    snprintf(needle, sizeof needle, "word %u, was", word_after);
    const uint32_t not_unwritten = EVERYTHING & ~GPU_COMBINER_INFER_UNWRITTEN;
    cfg c;
    gpu_combiner_plan plan;
    /* two stages read the constant, only the first one's factor word is written. With a factor
     * word per stage the second stage needs its own (unwritten, inferred as zero) */
    cfg_base(&c, 2u, each_flag);
    cfg_set(&c, IDX_COLOR_ICW, reg << 24);
    cfg_set(&c, IDX_COLOR_ICW + 1u, reg << 24);
    cfg_set(&c, factor_base, 0x11223344u);
    REFUSED(&c, not_unwritten, NULL, needle);
    PLANNED(&c, EVERYTHING, NULL, &plan);
    CHECK((plan.used_inferences & GPU_COMBINER_INFER_UNWRITTEN) != 0u);
    /* with ONE shared constant both stages read factor word 0, which is written: legal */
    cfg_base(&c, 2u, 0u);
    cfg_set(&c, IDX_COLOR_ICW, reg << 24);
    cfg_set(&c, IDX_COLOR_ICW + 1u, reg << 24);
    cfg_set(&c, factor_base, 0x11223344u);
    PLANNED(&c, not_unwritten, NULL, &plan);
    CHECK((plan.used_inferences & GPU_COMBINER_INFER_UNWRITTEN) == 0u);
}

static void test_factor_reads(void)
{
    printf("test_factor_reads\n");
    check_factor_reads(REG_C0, EACH0, IDX_FACTOR0, IDX_FACTOR0 + 1u);
    check_factor_reads(REG_C1, EACH1, IDX_FACTOR1, IDX_FACTOR1 + 1u);
    /* the eighth stage's constant is required too */
    cfg c;
    gpu_combiner_plan plan;
    cfg_base(&c, 8u, EACH0);
    cfg_set(&c, IDX_COLOR_ICW + 7u, REG_C0 << 24);
    REFUSED(&c, EVERYTHING & ~GPU_COMBINER_INFER_UNWRITTEN, NULL, "word 17, was");
    cfg_set(&c, IDX_FACTOR0 + 7u, 0x80402010u);
    PLANNED(&c, EVERYTHING & ~GPU_COMBINER_INFER_UNWRITTEN, NULL, &plan);
    CHECK(plan.stage_count == 8u);
    /* the factor0 bank and the factor1 bank are not interchangeable */
    cfg_base(&c, 1u, 0u);
    cfg_set(&c, IDX_COLOR_ICW, REG_C0 << 24);
    cfg_set(&c, IDX_FACTOR1, 0x01020304u); /* the WRONG bank written */
    REFUSED(&c, EVERYTHING & ~GPU_COMBINER_INFER_UNWRITTEN, NULL, "word 10, was");
    cfg_base(&c, 1u, 0u);
    cfg_set(&c, IDX_COLOR_ICW, REG_C1 << 24);
    cfg_set(&c, IDX_FACTOR0, 0x01020304u);
    REFUSED(&c, EVERYTHING & ~GPU_COMBINER_INFER_UNWRITTEN, NULL, "word 18, was");
}

/* ---- every required word is inferred when it was never written ---------------------------- */

static void test_unwritten_words(void)
{
    printf("test_unwritten_words\n");
    static const uint32_t required[] = {0u, IDX_ALPHA_OCW, IDX_COLOR_ICW, IDX_COLOR_OCW, IDX_FINAL0,
                                        IDX_FINAL1, IDX_CONTROL};
    for (size_t i = 0u; i < sizeof required / sizeof required[0]; i++) {
        cfg c;
        cfg_base(&c, 1u, 0u);
        c.written[required[i]] = false;
        if (required[i] == IDX_CONTROL) {
            /* an unwritten control word is a stage count of 0: refused for that reason */
            REFUSED(&c, EVERYTHING, NULL, "never written");
            continue;
        }
        char needle[32];
        snprintf(needle, sizeof needle, "word %u, was", required[i]);
        REFUSED(&c, EVERYTHING & ~GPU_COMBINER_INFER_UNWRITTEN, NULL, needle);
        gpu_combiner_plan plan;
        PLANNED(&c, EVERYTHING, NULL, &plan);
        CHECK((plan.used_inferences & GPU_COMBINER_INFER_UNWRITTEN) != 0u);
    }
    /* the second stage's words too (the loop must reach the last stage) */
    static const uint32_t second[] = {1u, IDX_ALPHA_OCW + 1u, IDX_COLOR_ICW + 1u, IDX_COLOR_OCW + 1u};
    for (size_t i = 0u; i < sizeof second / sizeof second[0]; i++) {
        cfg c;
        cfg_base(&c, 2u, 0u);
        c.written[second[i]] = false;
        char needle[32];
        snprintf(needle, sizeof needle, "word %u, was", second[i]);
        REFUSED(&c, EVERYTHING & ~GPU_COMBINER_INFER_UNWRITTEN, NULL, needle);
    }
    /* the final constants are required only when read */
    cfg c;
    cfg_base(&c, 1u, 0u);
    gpu_combiner_plan plan;
    PLANNED(&c, 0u, NULL, &plan);
    cfg_set(&c, IDX_FINAL0, REG_C0 << 24);
    REFUSED(&c, EVERYTHING & ~GPU_COMBINER_INFER_UNWRITTEN, NULL, "word 43, was");
    cfg_set(&c, IDX_FINAL_C0, 0x01020304u);
    PLANNED(&c, EVERYTHING & ~GPU_COMBINER_INFER_UNWRITTEN, NULL, &plan);
    cfg_base(&c, 1u, 0u);
    cfg_set(&c, IDX_FINAL0, REG_C1 << 24);
    REFUSED(&c, EVERYTHING & ~GPU_COMBINER_INFER_UNWRITTEN, NULL, "word 44, was");
    cfg_set(&c, IDX_FINAL_C1, 0x01020304u);
    PLANNED(&c, EVERYTHING & ~GPU_COMBINER_INFER_UNWRITTEN, NULL, &plan);
}

/* ---- the E times F product ------------------------------------------------------------ */

static void test_product_operands(void)
{
    printf("test_product_operands\n");
    const uint32_t not_unwritten = EVERYTHING & ~GPU_COMBINER_INFER_UNWRITTEN;
    gpu_combiner_plan plan;
    cfg c;
    /* D reads the product, E and F are what it multiplies: their constants are required */
    cfg_base(&c, 1u, 0u);
    cfg_set(&c, IDX_FINAL0, REG_PROD);
    cfg_set(&c, IDX_FINAL1, REG_C0 << 24);
    REFUSED(&c, not_unwritten, NULL, "word 43, was");
    cfg_set(&c, IDX_FINAL1, REG_C1 << 24);
    REFUSED(&c, not_unwritten, NULL, "word 44, was");
    cfg_set(&c, IDX_FINAL1, REG_C1 << 16); /* F, not E */
    REFUSED(&c, not_unwritten, NULL, "word 44, was");
    cfg_set(&c, IDX_FINAL1, REG_C0 << 24);
    cfg_set(&c, IDX_FINAL_C0, 0x01020304u);
    PLANNED(&c, not_unwritten, NULL, &plan);
    /* E through the sum register reads spare0 (never written by a stage here) and oD1 */
    cfg_base(&c, 1u, 0u);
    cfg_set(&c, IDX_FINAL0, REG_PROD);
    cfg_set(&c, IDX_FINAL1, REG_SUM << 24);
    PLANNED(&c, EVERYTHING, NULL, &plan);
    CHECK((plan.used_inferences & GPU_COMBINER_INFER_INITIAL_STATE) != 0u);
    CHECK((plan.used_inferences & GPU_COMBINER_INFER_COLOUR_RANGE) != 0u);
    REFUSED(&c, EVERYTHING & ~GPU_COMBINER_INFER_INITIAL_STATE, NULL, "before any write");
    REFUSED(&c, EVERYTHING & ~GPU_COMBINER_INFER_COLOUR_RANGE, NULL, "unclamped");
    /* E or F reading the product is refused, and so is G reading a register no source has */
    cfg_base(&c, 1u, 0u);
    cfg_set(&c, IDX_FINAL1, REG_PROD << 24);
    REFUSED(&c, EVERYTHING, NULL, "E times F product");
    cfg_set(&c, IDX_FINAL1, REG_PROD << 16);
    REFUSED(&c, EVERYTHING, NULL, "E times F product");
    cfg_set(&c, IDX_FINAL1, 6u << 8);
    REFUSED(&c, EVERYTHING, NULL, "final combiner reads register 6");
    cfg_set(&c, IDX_FINAL1, 7u << 16);
    REFUSED(&c, EVERYTHING, NULL, "final combiner reads register 7");
    cfg_set(&c, IDX_FINAL1, REG_PROD << 8); /* G may read the product */
    PLANNED(&c, EVERYTHING, NULL, &plan);
}

/* ---- the alpha half and blue-to-alpha ---------------------------------------------------- */

static void test_alpha_and_blue_to_alpha(void)
{
    printf("test_alpha_and_blue_to_alpha\n");
    gpu_combiner_plan plan;
    cfg c;
    /* an alpha half may not set a dot product or a blue-to-alpha flag, each one on its own */
    static const uint32_t flags[] = {1u << 12, 1u << 13, 1u << 18, 1u << 19};
    for (size_t i = 0u; i < sizeof flags / sizeof flags[0]; i++) {
        cfg_base(&c, 1u, 0u);
        cfg_set(&c, IDX_ALPHA_OCW, flags[i]);
        REFUSED(&c, EVERYTHING, NULL, "dot product or blue-to-alpha");
        /* the colour half takes the same flag */
        cfg_base(&c, 1u, 0u);
        cfg_set(&c, IDX_COLOR_OCW, flags[i]);
        PLANNED(&c, EVERYTHING, NULL, &plan);
    }
    /* blue-to-alpha defines the alpha of the destination, so reading it is not a read before a write */
    const uint32_t no_initial = EVERYTHING & ~GPU_COMBINER_INFER_INITIAL_STATE;
    cfg_base(&c, 1u, 0u);
    cfg_set(&c, IDX_FINAL1, (REG_SPARE0 | 0x10u) << 8); /* G = spare0 alpha */
    REFUSED(&c, no_initial, NULL, "before any write");
    cfg_set(&c, IDX_COLOR_OCW, (REG_SPARE0 << 4) | (1u << 19)); /* AB -> spare0, blue to alpha */
    PLANNED(&c, no_initial, NULL, &plan);
    cfg_set(&c, IDX_COLOR_OCW, REG_SPARE0 | (1u << 18)); /* CD -> spare0, blue to alpha */
    PLANNED(&c, no_initial, NULL, &plan);
    cfg_set(&c, IDX_COLOR_OCW, REG_SPARE0 << 4); /* AB -> spare0 without the flag */
    REFUSED(&c, no_initial, NULL, "before any write");
}

/* ---- the constant bytes inference ------------------------------------------------------- */

static void test_constant_bytes_inference(void)
{
    printf("test_constant_bytes_inference\n");
    const uint32_t not_bytes = EVERYTHING & ~GPU_COMBINER_INFER_CONSTANT_BYTES;
    gpu_combiner_plan plan;
    cfg c;
    static const struct {
        uint32_t reg;
        bool final_stage;
        uint32_t factor;
    } reads[] = {
        {REG_C0, false, IDX_FACTOR0}, {REG_C1, false, IDX_FACTOR1},
        {REG_C0, true, IDX_FINAL_C0}, {REG_C1, true, IDX_FINAL_C1},
    };
    for (size_t i = 0u; i < sizeof reads / sizeof reads[0]; i++) {
        cfg_base(&c, 1u, 0u);
        cfg_set(&c, reads[i].final_stage ? IDX_FINAL0 : IDX_COLOR_ICW, reads[i].reg << 24);
        cfg_set(&c, reads[i].factor, 0x11223344u);
        REFUSED(&c, not_bytes, NULL, "ARGB bytes");
        PLANNED(&c, EVERYTHING, NULL, &plan);
        CHECK((plan.used_inferences & GPU_COMBINER_INFER_CONSTANT_BYTES) != 0u);
    }
    cfg_base(&c, 1u, 0u);
    PLANNED(&c, not_bytes, NULL, &plan); /* no constant read: nothing to infer */
}

/* ---- textures ---------------------------------------------------------------------------- */

static void test_textures(void)
{
    printf("test_textures\n");
    static const uint8_t texels[16] = {0};
    gpu_combiner_plan plan;
    cfg c;
    cfg_base(&c, 1u, 0u);
    cfg_set(&c, IDX_COLOR_ICW, 8u << 24); /* t0 */
    cfg_set(&c, IDX_STAGE_PROGRAM, 1u);
    gpu_combiner_texture textures[GPU_COMBINER_TEXTURE_STAGES] = {{texels, 4096u, 1u, false, false, NULL, false, false}};
    PLANNED(&c, EVERYTHING, textures, &plan);
    CHECK(plan.texture_stages == 1u);
    textures[0].width = 1u;
    textures[0].height = 4096u;
    PLANNED(&c, EVERYTHING, textures, &plan);
    static const struct {
        uint32_t width;
        uint32_t height;
    } outside[] = {{4097u, 1u}, {1u, 4097u}, {0u, 1u}, {1u, 0u}};
    for (size_t i = 0u; i < sizeof outside / sizeof outside[0]; i++) {
        textures[0].width = outside[i].width;
        textures[0].height = outside[i].height;
        REFUSED(&c, EVERYTHING, textures, "outside 1..4096");
    }
    /* stage program modes: 0 and 1 only */
    textures[0].width = 2u;
    textures[0].height = 2u;
    cfg_set(&c, IDX_STAGE_PROGRAM, 2u);
    REFUSED(&c, EVERYTHING, textures, "program mode 2");
    /* spare0 alpha before any write starts as t0.a: stage 0's texture, not stage 1's */
    cfg_base(&c, 1u, 0u);
    cfg_set(&c, IDX_FINAL1, (REG_SPARE0 | 0x10u) << 8);
    cfg_set(&c, IDX_STAGE_PROGRAM, 1u);
    PLANNED(&c, EVERYTHING, textures, &plan);
    CHECK(plan.texture_stages == 1u);
    /* a stage with no image and a NAMED cause says the cause and the right stage, with the program written (stage 1: 1 << 5) and
     * without it, and one with no cause says the generic text */
    gpu_combiner_texture named[GPU_COMBINER_TEXTURE_STAGES] = {{0}};
    named[1].refusal = "the named cause";
    cfg_base(&c, 1u, 0u);
    cfg_set(&c, IDX_COLOR_ICW, 9u << 24); /* t1 */
    REFUSED(&c, EVERYTHING, named, "texture register t1: the named cause");
    cfg_set(&c, IDX_STAGE_PROGRAM, 1u << 5);
    REFUSED(&c, EVERYTHING, named, "texture register t1: the named cause");
    named[1].refusal = NULL;
    REFUSED(&c, EVERYTHING, named, "no test texture was supplied for stage 1");
    cfg_base(&c, 1u, 0u);
    cfg_set(&c, IDX_COLOR_ICW, 9u << 24);
    REFUSED(&c, EVERYTHING, named, "no test texture was supplied for stage 1");
}

/* ---- the SPIR-V interface scan on hand-built modules ---------------------------------- */

#define STORAGE_INPUT 1u
#define STORAGE_OUTPUT 3u

static size_t put(uint32_t *words, size_t at, uint32_t opcode, const uint32_t *operands, uint32_t count)
{
    words[at] = ((count + 1u) << 16) | opcode;
    for (uint32_t i = 0u; i < count; i++) {
        words[at + 1u + i] = operands[i];
    }
    return at + count + 1u;
}

static size_t header(uint32_t *words, uint32_t bound)
{
    words[0] = 0x07230203u;
    words[1] = 0x00010000u;
    words[2] = 0u;
    words[3] = bound;
    words[4] = 0u;
    return 5u;
}

/* One fragment entry point "main" whose interface is variable 2 (storage class `storage`) with
 * Location `location`: 19 words. */
static size_t one_entry(uint32_t *words, uint32_t storage, uint32_t location)
{
    size_t at = header(words, 10u);
    const uint32_t entry[] = {4u, 1u, 0x6E69616Du, 0u, 2u}; /* Fragment, id 1, "main", interface 2 */
    at = put(words, at, 15u, entry, 5u);
    const uint32_t variable[] = {3u, 2u, storage};
    at = put(words, at, 59u, variable, 3u);
    const uint32_t decorate[] = {2u, 30u, location};
    return put(words, at, 71u, decorate, 3u);
}

static void test_spirv_scan_by_hand(void)
{
    printf("test_spirv_scan_by_hand\n");
    uint32_t words[64];
    uint32_t mask = 0xFFu;
    size_t count = one_entry(words, STORAGE_INPUT, 0u);
    CHECK(count == 19u);
    CHECK(gpu_spirv_interface_locations(words, count, STORAGE_INPUT, &mask) && mask == 1u);
    CHECK(gpu_spirv_interface_locations(words, count, STORAGE_OUTPUT, &mask) && mask == 0u);
    /* the last representable location, and the first that is not */
    count = one_entry(words, STORAGE_INPUT, 31u);
    CHECK(gpu_spirv_interface_locations(words, count, STORAGE_INPUT, &mask) && mask == 0x80000000u);
    count = one_entry(words, STORAGE_INPUT, 32u);
    CHECK(!gpu_spirv_interface_locations(words, count, STORAGE_INPUT, &mask));

    /* a buffer without the magic is not scanned, whatever else it holds */
    count = one_entry(words, STORAGE_INPUT, 0u);
    words[0] ^= 1u;
    CHECK(!gpu_spirv_interface_locations(words, count, STORAGE_INPUT, &mask));
    /* an id bound of 0 is invalid */
    count = one_entry(words, STORAGE_INPUT, 0u);
    words[3] = 0u;
    CHECK(!gpu_spirv_interface_locations(words, count, STORAGE_INPUT, &mask));
    /* the last instruction runs one word past the end of the buffer */
    count = one_entry(words, STORAGE_INPUT, 0u);
    CHECK(!gpu_spirv_interface_locations(words, count - 1u, STORAGE_INPUT, &mask));
    /* an instruction with length 0 can never advance */
    count = header(words, 10u);
    words[count++] = 0u;
    words[count++] = 0u;
    CHECK(!gpu_spirv_interface_locations(words, count, STORAGE_INPUT, &mask));

    /* two entry points: the FIRST one's interface is the module's */
    size_t at = header(words, 10u);
    const uint32_t first[] = {4u, 1u, 0x6E69616Du, 0u, 2u};
    at = put(words, at, 15u, first, 5u);
    const uint32_t second[] = {4u, 4u, 0x6E69616Du, 0u, 3u};
    at = put(words, at, 15u, second, 5u);
    const uint32_t var_a[] = {7u, 2u, STORAGE_INPUT};
    at = put(words, at, 59u, var_a, 3u);
    const uint32_t var_b[] = {7u, 3u, STORAGE_INPUT};
    at = put(words, at, 59u, var_b, 3u);
    const uint32_t loc_a[] = {2u, 30u, 0u};
    at = put(words, at, 71u, loc_a, 3u);
    const uint32_t loc_b[] = {3u, 30u, 1u};
    at = put(words, at, 71u, loc_b, 3u);
    CHECK(gpu_spirv_interface_locations(words, at, STORAGE_INPUT, &mask) && mask == 0x1u);

    /* the name starts at word 3 of the entry point: an entry whose name word is "\x03" lists only
     * variable 2, and must not pick up variable 3 (Location 5) from its own name word */
    at = header(words, 10u);
    const uint32_t odd[] = {4u, 1u, 3u, 2u}; /* Fragment, id 1, name word 3, interface 2 */
    at = put(words, at, 15u, odd, 4u);
    const uint32_t odd_a[] = {7u, 2u, STORAGE_INPUT};
    at = put(words, at, 59u, odd_a, 3u);
    const uint32_t odd_b[] = {7u, 3u, STORAGE_INPUT};
    at = put(words, at, 59u, odd_b, 3u);
    const uint32_t odd_loc_a[] = {2u, 30u, 0u};
    at = put(words, at, 71u, odd_loc_a, 3u);
    const uint32_t odd_loc_b[] = {3u, 30u, 5u};
    at = put(words, at, 71u, odd_loc_b, 3u);
    CHECK(gpu_spirv_interface_locations(words, at, STORAGE_INPUT, &mask) && mask == 0x1u);
}

/* --- T510: texel coordinates ---------------------------------------------------------------------------------------------- */

#define SPV_OP_CONSTANT 43u
#define SPV_OP_CONSTANT_COMPOSITE 44u
#define SPV_OP_FDIV 136u
#define SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD 87u
#define SPV_OP_FUNCTION 54u
#define SPV_OP_LOAD 61u

static size_t find_op(const uint32_t *words, size_t count, uint32_t opcode, size_t from)
{
    for (size_t at = from; at < count; at += words[at] >> 16) {
        if ((words[at] & 0xFFFFu) == opcode) {
            return at;
        }
    }
    return SIZE_MAX;
}

/* The textured combiner's one sample, its coordinate divided by (width, height): the module the replay samples a render target
 * with. The dump (TSFP_TEXEL_DUMP=path) is what tests/test_texel_coordinates.py runs spirv-val over. */
static void test_texel_coordinates(void)
{
    printf("test_texel_coordinates\n");
    const size_t count = sizeof combiner_words_textured / sizeof combiner_words_textured[0];
    const float size[4][2] = {{128.0f, 64.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}};
    uint32_t *out = NULL;
    size_t out_count = 0u;
    CHECK(gpu_spirv_texel_coordinates(combiner_words_textured, count, 1u, size, &out, &out_count));
    CHECK(out != NULL && out_count == count + 13u + 5u);
    if (out != NULL && out_count == count + 18u) {
        CHECK(out[0] == combiner_words_textured[0] && out[1] == combiner_words_textured[1] && out[2] == combiner_words_textured[2]);
        CHECK(out[3] == combiner_words_textured[3] + 4u); /* width, height, the pair, the quotient */
        const size_t function = find_op(out, out_count, SPV_OP_FUNCTION, 5u);
        CHECK(function != SIZE_MAX && function >= 5u + 13u);
        /* the new constants end the global section: 128.0 and 64.0 as floats, then the pair, then the original function */
        const size_t block = function - 13u;
        const uint32_t composite_id = out[block + 10u];
        CHECK(out[block] == ((4u << 16) | SPV_OP_CONSTANT) && out[block + 3u] == 0x43000000u); /* 128.0f */
        CHECK(out[block + 4u] == ((4u << 16) | SPV_OP_CONSTANT) && out[block + 7u] == 0x42800000u); /* 64.0f */
        CHECK(out[block + 1u] == out[block + 5u]); /* both are the module's float type */
        CHECK(out[block + 8u] == ((5u << 16) | SPV_OP_CONSTANT_COMPOSITE) && out[block + 11u] == out[block + 2u] &&
              out[block + 12u] == out[block + 6u]);
        const size_t sample = find_op(out, out_count, SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD, function);
        const size_t divide = sample == SIZE_MAX ? SIZE_MAX : sample - 5u; /* the module has FDivs of its own: the one before the sample */
        CHECK(sample != SIZE_MAX && (out[divide] & 0xFFFFu) == SPV_OP_FDIV);
        if (sample != SIZE_MAX && (out[divide] & 0xFFFFu) == SPV_OP_FDIV) {
            CHECK((out[divide] >> 16) == 5u && out[divide + 4u] == composite_id); /* divided by the pair */
            CHECK(out[sample + 4u] == out[divide + 2u]);                          /* the sample reads the quotient */
            CHECK(out[divide + 1u] == out[block + 9u]);                           /* and it is a vec2 */
            /* nothing else moved: drop the 18 inserted words, put the coordinate back, and the module is the original */
            uint32_t *back = malloc(count * sizeof *back);
            CHECK(back != NULL);
            if (back != NULL) {
                memcpy(back, out, block * sizeof *back);
                memcpy(back + block, out + function, (divide - function) * sizeof *back);
                memcpy(back + block + (divide - function), out + sample, (out_count - sample) * sizeof *back);
                const size_t used = block + (divide - function) + (out_count - sample);
                CHECK(used == count);
                if (used == count) {
                    back[3] = combiner_words_textured[3];
                    const size_t original = find_op(combiner_words_textured, count, SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD, 5u);
                    const size_t moved = find_op(back, count, SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD, 5u);
                    CHECK(original == moved && original != SIZE_MAX);
                    if (original == moved && original != SIZE_MAX) {
                        back[moved + 4u] = combiner_words_textured[original + 4u];
                        CHECK(memcmp(back, combiner_words_textured, count * sizeof *back) == 0);
                    }
                }
                free(back);
            }
        }
        const char *dump = getenv("TSFP_TEXEL_DUMP");
        if (dump != NULL) {
            FILE *file = fopen(dump, "wb");
            CHECK(file != NULL && fwrite(out, sizeof *out, out_count, file) == out_count);
            if (file != NULL) {
                fclose(file);
            }
        }
    }
    free(out);

    /* refusals, each false and nothing allocated */
    out = NULL;
    out_count = 7u;
    CHECK(!gpu_spirv_texel_coordinates(combiner_words_textured, count, 0u, size, &out, &out_count) && out == NULL);
    CHECK(!gpu_spirv_texel_coordinates(combiner_words_textured, count, 0x10u, size, &out, &out_count) && out == NULL);
    CHECK(!gpu_spirv_texel_coordinates(combiner_words_textured, count, 2u, size, &out, &out_count) && out == NULL); /* no stage 1 */
    const float zero_edge[4][2] = {{0.0f, 64.0f}};
    CHECK(!gpu_spirv_texel_coordinates(combiner_words_textured, count, 1u, zero_edge, &out, &out_count) && out == NULL);
    const float zero_height[4][2] = {{128.0f, 0.0f}};
    CHECK(!gpu_spirv_texel_coordinates(combiner_words_textured, count, 1u, zero_height, &out, &out_count) && out == NULL);
    CHECK(!gpu_spirv_texel_coordinates(combiner_words_pass, sizeof combiner_words_pass / sizeof combiner_words_pass[0], 1u,
                                       size, &out, &out_count) && out == NULL); /* a module with no sampler */
    CHECK(!gpu_spirv_texel_coordinates(NULL, count, 1u, size, &out, &out_count));
    CHECK(!gpu_spirv_texel_coordinates(combiner_words_textured, 4u, 1u, size, &out, &out_count));
    CHECK(!gpu_spirv_texel_coordinates(combiner_words_textured, count, 1u, NULL, &out, &out_count));
    CHECK(!gpu_spirv_texel_coordinates(combiner_words_textured, count, 1u, size, NULL, &out_count));
    CHECK(!gpu_spirv_texel_coordinates(combiner_words_textured, count, 1u, size, &out, NULL));
    uint32_t *bad = malloc(count * sizeof *bad);
    CHECK(bad != NULL);
    if (bad != NULL) {
        memcpy(bad, combiner_words_textured, count * sizeof *bad);
        bad[0] ^= 1u; /* not a module */
        CHECK(!gpu_spirv_texel_coordinates(bad, count, 1u, size, &out, &out_count));
        bad[0] = combiner_words_textured[0];
        bad[3] = 0u; /* no id bound */
        CHECK(!gpu_spirv_texel_coordinates(bad, count, 1u, size, &out, &out_count));
        bad[3] = combiner_words_textured[3];
        const size_t sample = find_op(bad, count, SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD, 5u);
        CHECK(sample != SIZE_MAX);
        if (sample != SIZE_MAX) {
            bad[sample + 4u] = bad[sample + 2u]; /* a coordinate that is a vec4 (the sample's own result) */
            CHECK(!gpu_spirv_texel_coordinates(bad, count, 1u, size, &out, &out_count));
            bad[sample] = (bad[sample] & 0xFFFF0000u) | 88u; /* another sample opcode: the rewrite does not know it */
            bad[sample + 4u] = combiner_words_textured[sample + 4u];
            CHECK(!gpu_spirv_texel_coordinates(bad, count, 1u, size, &out, &out_count));
            bad[sample] = combiner_words_textured[sample];
            /* the loaded sampler read by nothing: no sample to divide the coordinate of */
            bad[sample + 3u] = bad[sample + 4u];
            CHECK(!gpu_spirv_texel_coordinates(bad, count, 1u, size, &out, &out_count));
            bad[sample + 3u] = combiner_words_textured[sample + 3u];
            /* the loaded sampler read by the sample AND by another instruction (the operand of a second load): refused */
            const uint32_t sampler_load = bad[sample + 3u];
            const size_t function = find_op(bad, count, SPV_OP_FUNCTION, 5u);
            bool patched = false;
            for (size_t at = function; at < count && !patched; at += bad[at] >> 16) {
                if ((bad[at] & 0xFFFFu) == SPV_OP_LOAD && bad[at + 2u] != sampler_load) {
                    const uint32_t kept = bad[at + 3u];
                    bad[at + 3u] = sampler_load;
                    CHECK(!gpu_spirv_texel_coordinates(bad, count, 1u, size, &out, &out_count));
                    bad[at + 3u] = kept;
                    patched = true;
                }
            }
            CHECK(patched);
        }
        /* an id bound past the limit, a stage bit past 3 beside a good one, and an instruction of no length */
        bad[3] = (1u << 20) + 1u;
        CHECK(!gpu_spirv_texel_coordinates(bad, count, 1u, size, &out, &out_count));
        bad[3] = combiner_words_textured[3];
        CHECK(!gpu_spirv_texel_coordinates(bad, count, 0x11u, size, &out, &out_count));
        bad[5] = combiner_words_textured[5] & 0xFFFFu;
        CHECK(!gpu_spirv_texel_coordinates(bad, count, 1u, size, &out, &out_count));
        bad[5] = combiner_words_textured[5];
        /* a refusal past the argument checks leaves the caller's outputs empty, whatever they held */
        uint32_t sentinel = 0u;
        out = &sentinel;
        out_count = 7u;
        CHECK(!gpu_spirv_texel_coordinates(bad, count, 2u, size, &out, &out_count));
        CHECK(out == NULL && out_count == 0u);
        /* a one texel texture is the smallest the divide takes */
        const float one[4][2] = {{1.0f, 1.0f}};
        CHECK(gpu_spirv_texel_coordinates(bad, count, 1u, one, &out, &out_count) && out != NULL && out_count == count + 18u);
        free(out);
        out = NULL;
        free(bad);
    }
}

int main(void)
{
    test_base_is_legal();
    test_null_arguments();
    test_factor_reads();
    test_unwritten_words();
    test_product_operands();
    test_alpha_and_blue_to_alpha();
    test_constant_bytes_inference();
    test_textures();
    test_spirv_scan_by_hand();
    test_texel_coordinates();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 && checks > 100 ? 0 : 1;
}
