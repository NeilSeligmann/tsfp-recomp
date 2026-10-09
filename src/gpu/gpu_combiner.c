/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See gpu_combiner.h for what is decided here and what is refused. The register and field rules
 * mirror tools/nv2a_combiner/config.py (decode, _check_half), and tests/test_gpu_combiner_replay.py
 * proves the two agree over random configurations and over single-field mutations of them.
 */

#include "gpu_fog.h"
#include "gpu_combiner.h"

#include "gpu_sha256.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#define IDX_DOT_MAPPING 55u
#define IDX_OTHER_INPUT 56u
#define DOT_MAPPING_KEY_MASK 0x00000FFFu
#define OTHER_INPUT_KEY_MASK 0x00FF0000u

#define TEX_CUBE 3u
#define TEX_DOT_ST 9u
#define TEX_DOT_PRODUCT 17u

#define REG_ZERO 0u
#define REG_C0 1u
#define REG_C1 2u
#define REG_FOG 3u
#define REG_V0 4u
#define REG_V1 5u
#define REG_SPARE0 12u
#define REG_SPARE1 13u
#define REG_SUM 14u
#define REG_PROD 15u

#define SOURCES_GENERAL 0x3F3Fu     /* 0..5 and 8..13 */
#define DESTINATIONS_GENERAL 0x3F31u /* 0, 4, 5 and 8..13 */
#define SOURCES_FINAL 0xFF3Fu       /* general, 14 and 15 */

#define COMPONENT_RGB 1u
#define COMPONENT_ALPHA 2u

#define CONTROL_MUX_MSB (1u << 8)
#define CONTROL_FACTOR0_EACH (1u << 12)
#define CONTROL_FACTOR1_EACH (1u << 16)
#define CONTROL_KEY_MASK (0xFFu | CONTROL_MUX_MSB | CONTROL_FACTOR0_EACH | CONTROL_FACTOR1_EACH)
#define OUTPUT_KEY_MASK 0x000FFFFFu
#define FINAL1_KEY_MASK 0xFFFFFFE0u

#define OUT_CD_DOT (1u << 12)
#define OUT_AB_DOT (1u << 13)
#define OUT_MUX (1u << 14)
#define OUT_BLUE_TO_ALPHA_CD (1u << 18)
#define OUT_BLUE_TO_ALPHA_AB (1u << 19)

/* The 38 words that change the translation, ascending. The last two (T1490) are zero unless a dot stage is kept. */
static const uint8_t key_indices[GPU_COMBINER_KEY_WORDS] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9,                                /* alpha input, final 0 and 1 */
    26, 27, 28, 29, 30, 31, 32, 33,                              /* alpha output */
    34, 35, 36, 37, 38, 39, 40, 41,                              /* colour input */
    45, 46, 47, 48, 49, 50, 51, 52, 53, 54,                      /* colour output, control, program */
    55, 56,                                                      /* dot mapping, other stage input (T1490) */
};

const uint8_t *gpu_combiner_key_indices(void)
{
    return key_indices;
}

void gpu_combiner_name(const uint32_t key[GPU_COMBINER_KEY_WORDS], char out[GPU_COMBINER_NAME_BYTES])
{
    uint8_t bytes[GPU_COMBINER_KEY_WORDS * 4u];
    /* a key without a dot stage hashes its first 36 words, so the module names of before T1490 are unchanged */
    const uint32_t words = key[GPU_COMBINER_LEGACY_KEY_WORDS] == 0u && key[GPU_COMBINER_LEGACY_KEY_WORDS + 1u] == 0u
                               ? GPU_COMBINER_LEGACY_KEY_WORDS : GPU_COMBINER_KEY_WORDS;
    for (uint32_t i = 0u; i < words; i++) {
        bytes[i * 4u] = (uint8_t)key[i];
        bytes[i * 4u + 1u] = (uint8_t)(key[i] >> 8);
        bytes[i * 4u + 2u] = (uint8_t)(key[i] >> 16);
        bytes[i * 4u + 3u] = (uint8_t)(key[i] >> 24);
    }
    uint8_t digest[32];
    char hex[65];
    gpu_sha256(bytes, words * 4u, digest);
    gpu_sha256_hex(digest, hex);
    snprintf(out, GPU_COMBINER_NAME_BYTES, "combiner_%s", hex);
}

typedef struct {
    char *error;
    size_t error_size;
    uint32_t reads;
    uint32_t fog_components; /* RGB requires colour; alpha uses generated fog factor only. */
    uint32_t defined[16];       /* components written so far, spare registers only */
    bool initial_r0_alpha_read; /* spare0 alpha read before any write: t0.a or 1.0 */
    bool initial_state_read;    /* any OTHER read of a spare register before a write */
    uint32_t factor0_reads;     /* bit k: factor0[k] is read */
    uint32_t factor1_reads;
    bool final_c0_read;
    bool final_c1_read;
} analysis;

static gpu_pgraph_result refuse(analysis *an, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static gpu_pgraph_result refuse(analysis *an, const char *format, ...)
{
    if (an->error != NULL && an->error_size != 0u) {
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(an->error, an->error_size, format, arguments);
        va_end(arguments);
    }
    return GPU_PGRAPH_ERR_UNMEASURED;
}

static void note_read(analysis *an, uint32_t reg, uint32_t components)
{
    an->reads |= 1u << reg;
    if (reg == REG_FOG) an->fog_components |= components;
    if (reg != REG_SPARE0 && reg != REG_SPARE1) {
        return;
    }
    const uint32_t missing = components & ~an->defined[reg];
    if (missing == 0u) {
        return;
    }
    if (reg == REG_SPARE0 && missing == COMPONENT_ALPHA) {
        an->initial_r0_alpha_read = true;
    } else if (reg == REG_SPARE0 && (missing & COMPONENT_ALPHA) != 0u) {
        an->initial_r0_alpha_read = true;
        an->initial_state_read = true;
    } else {
        an->initial_state_read = true;
    }
}

static uint32_t operand_register(uint32_t word, unsigned slot)
{
    return (word >> (24u - 8u * slot)) & 0xFu;
}

static uint32_t operand_components(uint32_t word, unsigned slot)
{
    return ((word >> (24u - 8u * slot)) & 0x10u) != 0u ? COMPONENT_ALPHA : COMPONENT_RGB;
}

/* One half stage. The reads are noted at the stage's start state, the writes returned. */
static gpu_pgraph_result half_reads(analysis *an, uint32_t stage, bool rgb, uint32_t input_word,
                                    uint32_t output_word, uint32_t control, uint32_t writes[16])
{
    for (unsigned slot = 0u; slot < 4u; slot++) {
        const uint32_t reg = operand_register(input_word, slot);
        if (((SOURCES_GENERAL >> reg) & 1u) == 0u) {
            return refuse(an, "stage %u %s half reads register %u, which a general stage cannot",
                          (unsigned)stage, rgb ? "colour" : "alpha", (unsigned)reg);
        }
        note_read(an, reg, operand_components(input_word, slot));
        if (reg == REG_C0) {
            an->factor0_reads |= 1u << ((control & CONTROL_FACTOR0_EACH) != 0u ? stage : 0u);
        } else if (reg == REG_C1) {
            an->factor1_reads |= 1u << ((control & CONTROL_FACTOR1_EACH) != 0u ? stage : 0u);
        }
    }
    const uint32_t operation = (output_word >> 15) & 7u;
    if (operation == 5u || operation == 7u) {
        return refuse(an, "stage %u output operation %u is invalid (x4 or x1/2 with a bias)",
                      (unsigned)stage, (unsigned)operation);
    }
    if (!rgb && (output_word & (OUT_AB_DOT | OUT_CD_DOT | OUT_BLUE_TO_ALPHA_AB | OUT_BLUE_TO_ALPHA_CD)) != 0u) {
        return refuse(an, "stage %u alpha half sets a dot product or blue-to-alpha flag",
                      (unsigned)stage);
    }
    const uint32_t cd = output_word & 0xFu;
    const uint32_t ab = (output_word >> 4) & 0xFu;
    const uint32_t sum = (output_word >> 8) & 0xFu;
    const uint32_t destinations[3] = {cd, ab, sum};
    for (unsigned i = 0u; i < 3u; i++) {
        if (((DESTINATIONS_GENERAL >> destinations[i]) & 1u) == 0u) {
            return refuse(an, "stage %u writes register %u, which is neither a colour nor a spare",
                          (unsigned)stage, (unsigned)destinations[i]);
        }
    }
    if ((output_word & OUT_MUX) != 0u) {
        note_read(an, REG_SPARE0, COMPONENT_ALPHA); /* the mux select is spare0 alpha */
    }
    if (rgb) {
        if (cd != REG_ZERO) {
            writes[cd] |= COMPONENT_RGB | ((output_word & OUT_BLUE_TO_ALPHA_CD) != 0u ? COMPONENT_ALPHA : 0u);
        }
        if (ab != REG_ZERO) {
            writes[ab] |= COMPONENT_RGB | ((output_word & OUT_BLUE_TO_ALPHA_AB) != 0u ? COMPONENT_ALPHA : 0u);
        }
        if (sum != REG_ZERO) {
            writes[sum] |= COMPONENT_RGB;
        }
    } else {
        for (unsigned i = 0u; i < 3u; i++) {
            if (destinations[i] != REG_ZERO) {
                writes[destinations[i]] |= COMPONENT_ALPHA;
            }
        }
    }
    return GPU_PGRAPH_OK;
}

/* A final-combiner source. The sum register reads spare0 and the secondary colour, the product
 * reads E and F. */
static void final_read(analysis *an, uint32_t reg, uint32_t components, uint32_t word1)
{
    note_read(an, reg, components);
    if (reg == REG_C0) {
        an->final_c0_read = true;
    } else if (reg == REG_C1) {
        an->final_c1_read = true;
    } else if (reg == REG_SUM) {
        note_read(an, REG_SPARE0, COMPONENT_RGB);
        note_read(an, REG_V1, COMPONENT_RGB);
    } else if (reg == REG_PROD) {
        for (unsigned slot = 0u; slot < 2u; slot++) {
            const uint32_t source = operand_register(word1, slot);
            note_read(an, source, operand_components(word1, slot));
            if (source == REG_C0) {
                an->final_c0_read = true;
            } else if (source == REG_C1) {
                an->final_c1_read = true;
            } else if (source == REG_SUM) {
                note_read(an, REG_SPARE0, COMPONENT_RGB);
                note_read(an, REG_V1, COMPONENT_RGB);
            }
        }
    }
}

static gpu_pgraph_result analyse(analysis *an, const uint32_t w[GPU_PGRAPH_COMBINER_WORDS],
                                 uint32_t count)
{
    const uint32_t control = w[IDX_CONTROL];
    for (uint32_t stage = 0u; stage < count; stage++) {
        uint32_t writes[16] = {0};
        gpu_pgraph_result result = half_reads(an, stage, true, w[IDX_COLOR_ICW + stage],
                                              w[IDX_COLOR_OCW + stage], control, writes);
        if (result == GPU_PGRAPH_OK) {
            result = half_reads(an, stage, false, w[stage], w[IDX_ALPHA_OCW + stage], control, writes);
        }
        if (result != GPU_PGRAPH_OK) {
            return result;
        }
        for (unsigned reg = 0u; reg < 16u; reg++) {
            an->defined[reg] |= writes[reg];
        }
    }
    const uint32_t word0 = w[IDX_FINAL0];
    const uint32_t word1 = w[IDX_FINAL1];
    for (unsigned slot = 0u; slot < 4u; slot++) {
        const uint32_t reg = operand_register(word0, slot);
        if (((SOURCES_FINAL >> reg) & 1u) == 0u) {
            return refuse(an, "the final combiner reads register %u", (unsigned)reg);
        }
    }
    for (unsigned slot = 0u; slot < 3u; slot++) {
        const uint32_t reg = operand_register(word1, slot);
        if (((SOURCES_FINAL >> reg) & 1u) == 0u) {
            return refuse(an, "the final combiner reads register %u", (unsigned)reg);
        }
    }
    if (operand_register(word1, 0) == REG_PROD || operand_register(word1, 1) == REG_PROD) {
        return refuse(an, "the final combiner E or F reads the E times F product");
    }
    for (unsigned slot = 0u; slot < 4u; slot++) {
        final_read(an, operand_register(word0, slot), operand_components(word0, slot), word1);
    }
    final_read(an, operand_register(word1, 2), operand_components(word1, 2), word1);
    return GPU_PGRAPH_OK;
}

static float factor_byte(uint32_t word, unsigned shift)
{
    return (float)((word >> shift) & 0xFFu) / 255.0f;
}

static void unpack_constant(uint32_t word, float *out)
{
    out[0] = factor_byte(word, 16u); /* R */
    out[1] = factor_byte(word, 8u);  /* G */
    out[2] = factor_byte(word, 0u);  /* B */
    out[3] = factor_byte(word, 24u); /* A, the top byte */
}

gpu_pgraph_result gpu_combiner_plan_build_profile(const gpu_pgraph_state *state, uint32_t allowed,
                                          const gpu_combiner_texture textures[GPU_COMBINER_TEXTURE_STAGES],
                                          bool live_fog, gpu_combiner_plan *plan, char *error, size_t error_size)
{
    if (error != NULL && error_size != 0u) {
        error[0] = '\0';
    }
    if (state == NULL || plan == NULL || textures == NULL) {
        return GPU_PGRAPH_ERR_ARGUMENT;
    }
    memset(plan, 0, sizeof *plan);
    analysis an;
    memset(&an, 0, sizeof an);
    an.error = error;
    an.error_size = error_size;
    if (!state->combiner_captured) {
        return refuse(&an, "the model did not decode combiner methods (gpu_pgraph_set_combiner)");
    }
    uint32_t w[GPU_PGRAPH_COMBINER_WORDS];
    memcpy(w, state->combiner, sizeof w);
    uint32_t unwritten_needed = 0u;
    const char *unwritten_first = NULL;
    unsigned unwritten_index = 0u;
#define NEED(index, what)                                                                     \
    do {                                                                                      \
        if (!state->combiner_written[(index)]) {                                              \
            if (unwritten_needed == 0u) {                                                     \
                unwritten_first = (what);                                                     \
                unwritten_index = (unsigned)(index);                                          \
            }                                                                                 \
            unwritten_needed++;                                                               \
            w[(index)] = 0u;                                                                  \
        }                                                                                     \
    } while (0)
    NEED(IDX_CONTROL, "the combiner control word");
    const uint32_t count = w[IDX_CONTROL] & 0xFFu;
    if (count < 1u || count > 8u) {
        return refuse(&an, "combiner stage count %u is outside 1 to 8%s", (unsigned)count,
                      unwritten_needed != 0u ? " (the control word 0x1E60 was never written)" : "");
    }
    for (uint32_t stage = 0u; stage < count; stage++) {
        NEED(stage, "an alpha input word");
        NEED(IDX_ALPHA_OCW + stage, "an alpha output word");
        NEED(IDX_COLOR_ICW + stage, "a colour input word");
        NEED(IDX_COLOR_OCW + stage, "a colour output word");
    }
    NEED(IDX_FINAL0, "final combiner word 0");
    NEED(IDX_FINAL1, "final combiner word 1");
    gpu_pgraph_result result = analyse(&an, w, count);
    if (result != GPU_PGRAPH_OK) {
        return result;
    }
    for (uint32_t bank = 0u; bank < 2u; bank++) {
        const uint32_t reads = bank == 0u ? an.factor0_reads : an.factor1_reads;
        for (uint32_t k = 0u; k < 8u; k++) {
            if ((reads >> k) & 1u) {
                NEED((bank == 0u ? IDX_FACTOR0 : IDX_FACTOR1) + k,
                     bank == 0u ? "a factor0 constant" : "a factor1 constant");
            }
        }
    }
    if (an.final_c0_read) {
        NEED(IDX_FINAL_C0, "final combiner constant 0");
    }
    if (an.final_c1_read) {
        NEED(IDX_FINAL_C1, "final combiner constant 1");
    }
    if ((an.reads & (1u << REG_FOG)) != 0u) {
        if (!live_fog) return refuse(&an, "the combiner reads fog register (3) without the explicit live-fog profile");
        if ((allowed & (GPU_COMBINER_INFER_CONSTANT_BYTES | GPU_COMBINER_INFER_COLOUR_RANGE)) !=
            (GPU_COMBINER_INFER_CONSTANT_BYTES | GPU_COMBINER_INFER_COLOUR_RANGE))
            return refuse(&an, "INFERRED fog colour/factor requires constant-byte and colour-range permission");
        if ((an.fog_components & COMPONENT_RGB) != 0u &&
            !state->output_written[GPU_PGRAPH_OUT_FOG_COLOR])
            return refuse(&an, "fog colour method (0x02A8) was never written");
        gpu_fog_control fog;
        char reason[160];
        if (!gpu_fog_decode(state, &fog, reason, sizeof reason)) return refuse(&an, "%s", reason);
        plan->uses_fog = true;
    }

    /* Texture stages. Modes 0 (none) and 1 (2D projective) are always modelled, 3 (cube map), 9 (DOT_ST) and 17 (DOTPRODUCT) when
     * GPU_COMBINER_INFER_TEXTURE_MODES is allowed (T1490, xemu psh.c PS_TEXTUREMODES_*). A stage the combiner does not read
     * and no kept dot stage depends on is dropped from the program (xemu computes it and the result is unused), except the
     * modes with a side effect (5 clip plane, 10 DOT_ZW), which are refused. The same walk as replay_modules.py kept_stages. */
    uint32_t reads_texture = (an.reads >> 8) & 0xFu;
    const bool program_written = state->combiner_written[IDX_STAGE_PROGRAM];
    uint32_t modes[GPU_COMBINER_TEXTURE_STAGES];
    uint32_t inferences = 0u;
    for (uint32_t stage = 0u; stage < GPU_COMBINER_TEXTURE_STAGES; stage++) {
        modes[stage] = program_written ? (w[IDX_STAGE_PROGRAM] >> (5u * stage)) & 0x1Fu : textures[stage].rgba != NULL ? 1u : 0u;
    }
    if (an.initial_r0_alpha_read) {
        if (modes[0] != 0u) {
            reads_texture |= 1u; /* spare0 alpha starts as t0.a */
        } else {
            an.initial_state_read = true; /* and starts as 1.0, INFERRED */
        }
    }
    if (reads_texture != 0u && !program_written) {
        inferences |= GPU_COMBINER_INFER_STAGE_PROGRAM;
    }
    uint32_t needed = reads_texture;
    for (uint32_t stage = 0u; stage < GPU_COMBINER_TEXTURE_STAGES; stage++) {
        if (((needed >> stage) & 1u) != 0u && (modes[stage] == TEX_DOT_ST || modes[stage] == TEX_DOT_PRODUCT)) {
            NEED(IDX_DOT_MAPPING, "the dot product input mapping (0x1E74)");
            NEED(IDX_OTHER_INPUT, "the other stage input word (0x1E78)");
            break;
        }
    }
    for (uint32_t stage = GPU_COMBINER_TEXTURE_STAGES - 1u; stage >= 1u; stage--) {
        if (((needed >> stage) & 1u) != 0u && (modes[stage] == TEX_DOT_ST || modes[stage] == TEX_DOT_PRODUCT)) {
            const uint32_t input = stage < 2u ? 0u : (w[IDX_OTHER_INPUT] >> (16u + 4u * (stage - 2u))) & 0xFu;
            if (input >= stage) {
                return refuse(&an, "texture stage %u (mode %u) takes its input from stage %u, which is not an earlier stage",
                              (unsigned)stage, (unsigned)modes[stage], (unsigned)input);
            }
            needed |= 1u << input;
            if (modes[stage] == TEX_DOT_ST) {
                needed |= 1u << (stage - 1u);
            }
        }
    }
    uint32_t kept[GPU_COMBINER_TEXTURE_STAGES];
    for (uint32_t stage = 0u; stage < GPU_COMBINER_TEXTURE_STAGES; stage++) {
        kept[stage] = ((needed >> stage) & 1u) != 0u ? modes[stage] : 0u;
        const uint32_t raw = modes[stage];
        if ((raw == 5u || raw == 10u || raw > 18u) && program_written) {
            return refuse(&an, "texture stage %u program mode %u is not modelled (clip plane, DOT_ZW and unknown modes have effects beyond t%u)",
                          (unsigned)stage, (unsigned)raw, (unsigned)stage);
        }
        if (kept[stage] > 1u && kept[stage] != TEX_CUBE && kept[stage] != TEX_DOT_ST && kept[stage] != TEX_DOT_PRODUCT) {
            return refuse(&an,
                          "texture stage %u program mode %u is not modelled: only 0 (none), 1 (a 2D texture) and, INFERRED, "
                          "3 (cube map), 9 (DOT_ST) and 17 (DOTPRODUCT) are",
                          (unsigned)stage, (unsigned)kept[stage]);
        }
        if (kept[stage] > 1u) {
            if ((allowed & GPU_COMBINER_INFER_TEXTURE_MODES) == 0u) {
                return refuse(&an, "texture stage %u program mode %u (%s) is INFERRED from xemu psh.c and not allowed", (unsigned)stage,
                              (unsigned)kept[stage], kept[stage] == TEX_CUBE ? "cube map" : kept[stage] == TEX_DOT_ST ? "DOT_ST" : "DOTPRODUCT");
            }
            inferences |= GPU_COMBINER_INFER_TEXTURE_MODES;
        }
    }
    for (uint32_t stage = 0u; stage < GPU_COMBINER_TEXTURE_STAGES; stage++) {
        /* xemu psh.c asserts: DOTPRODUCT only at stage 1 or 2, DOT_ST at stage 2 or 3 after a dot stage */
        if (kept[stage] == TEX_DOT_PRODUCT && stage != 1u && stage != 2u) {
            return refuse(&an, "texture stage %u is DOTPRODUCT (17): only stages 1 and 2 are (xemu psh.c)", (unsigned)stage);
        }
        if (kept[stage] == TEX_DOT_ST && (stage < 2u || (kept[stage - 1u] != TEX_DOT_ST && kept[stage - 1u] != TEX_DOT_PRODUCT))) {
            return refuse(&an, "texture stage %u is DOT_ST (9) without a dot product stage before it (xemu psh.c needs dot%u)",
                          (unsigned)stage, (unsigned)(stage - 1u));
        }
    }
    uint32_t effective_program = 0u;
    for (uint32_t stage = 0u; stage < GPU_COMBINER_TEXTURE_STAGES; stage++) {
        if (((needed >> stage) & 1u) == 0u) {
            continue;
        }
        if (kept[stage] == 0u && !program_written) {
            if (textures[stage].refusal != NULL) {
                return refuse(&an, "the combiner reads texture register t%u: %s", (unsigned)stage, textures[stage].refusal);
            }
            return refuse(&an,
                          "the combiner reads texture register t%u: textures are not modelled, and "
                          "no test texture was supplied for stage %u",
                          (unsigned)stage, (unsigned)stage);
        }
        if (kept[stage] == 0u) {
            /* T1207: a written program of "none" is a defined stage. xemu's psh.c emits
             * `vec4 tN = vec4(0.0, 0.0, 0.0, 1.0)` for PS_TEXTUREMODES_NONE, the generator here does the same
             * (TEX_NONE), so no texture is bound or sampled and the stage keeps its bit clear. xemu-level. */
            continue;
        }
        if (kept[stage] == TEX_DOT_PRODUCT) {
            /* t = vec4(0.0) and dotN = dot(oTN.xyz, mapped input): the coordinate only, no texture */
            effective_program |= kept[stage] << (5u * stage);
            plan->coordinate_stages |= 1u << stage;
            plan->uses_texture_modes = true;
            continue;
        }
        const gpu_combiner_texture *texture = &textures[stage];
        if (texture->rgba == NULL && texture->refusal != NULL) {
            return refuse(&an, "the combiner reads texture register t%u: %s", (unsigned)stage, texture->refusal);
        }
        if (texture->rgba == NULL) {
            return refuse(&an,
                          "the combiner reads texture register t%u: textures are not modelled, and "
                          "no test texture was supplied for stage %u",
                          (unsigned)stage, (unsigned)stage);
        }
        if (texture->width == 0u || texture->height == 0u || texture->width > 4096u ||
            texture->height > 4096u) {
            return refuse(&an, "the test texture of stage %u is %ux%u, outside 1..4096",
                          (unsigned)stage, (unsigned)texture->width, (unsigned)texture->height);
        }
        if (texture->cube != (kept[stage] == TEX_CUBE)) {
            return refuse(&an, "texture stage %u program mode %u needs %s but the bound texture is %s", (unsigned)stage,
                          (unsigned)kept[stage], kept[stage] == TEX_CUBE ? "a cube map" : "a 2D texture",
                          texture->cube ? "a cube map" : "2D");
        }
        effective_program |= kept[stage] << (5u * stage);
        plan->texture_stages |= 1u << stage;
        plan->coordinate_stages |= 1u << stage;
        if (kept[stage] == TEX_CUBE) {
            plan->cube_stages |= 1u << stage;
        }
        if (kept[stage] > 1u) {
            plan->uses_texture_modes = true;
        }
        inferences |= GPU_COMBINER_INFER_TEXTURE_SAMPLING;
    }
    w[IDX_STAGE_PROGRAM] = effective_program;

    if (unwritten_needed != 0u) {
        inferences |= GPU_COMBINER_INFER_UNWRITTEN;
    }
    if (an.initial_state_read) {
        inferences |= GPU_COMBINER_INFER_INITIAL_STATE;
    }
    if (an.factor0_reads != 0u || an.factor1_reads != 0u || an.final_c0_read || an.final_c1_read) {
        inferences |= GPU_COMBINER_INFER_CONSTANT_BYTES;
    }
    if ((an.reads & ((1u << REG_V0) | (1u << REG_V1))) != 0u) {
        inferences |= GPU_COMBINER_INFER_COLOUR_RANGE;
    }
    const uint32_t refused = inferences & ~allowed;
    if (refused != 0u) {
        static const struct {
            uint32_t bit;
            const char *what;
        } names[] = {
            {GPU_COMBINER_INFER_UNWRITTEN, "reading a combiner word the stream never wrote as 0"},
            {GPU_COMBINER_INFER_STAGE_PROGRAM,
             "taking the never-written texture stage program from the test textures"},
            {GPU_COMBINER_INFER_INITIAL_STATE, "a stage reading a spare register before any write"},
            {GPU_COMBINER_INFER_CONSTANT_BYTES, "reading a factor word as ARGB bytes over 255"},
            {GPU_COMBINER_INFER_COLOUR_RANGE, "taking oD0 and oD1 into the combiner unclamped"},
            {GPU_COMBINER_INFER_TEXTURE_SAMPLING,
             "sampling a test texture nearest, clamped, row 0 on top"},
        };
        for (size_t i = 0u; i < sizeof names / sizeof names[0]; i++) {
            if ((refused & names[i].bit) != 0u) {
                if (error != NULL && error_size != 0u) {
                    if (names[i].bit == GPU_COMBINER_INFER_UNWRITTEN && unwritten_first != NULL) {
                        snprintf(error, error_size,
                                 "%s is INFERRED and not allowed (first: %s, word %u, was never written)",
                                 names[i].what, unwritten_first, unwritten_index);
                    } else {
                        snprintf(error, error_size, "%s is INFERRED and not allowed", names[i].what);
                    }
                }
                return GPU_PGRAPH_ERR_UNMEASURED;
            }
        }
    }

    /* The key: only the words that change the translation, the unused stages zeroed. */
    uint32_t canonical[GPU_PGRAPH_COMBINER_WORDS];
    memcpy(canonical, w, sizeof canonical);
    for (uint32_t stage = count; stage < 8u; stage++) {
        canonical[stage] = 0u;
        canonical[IDX_ALPHA_OCW + stage] = 0u;
        canonical[IDX_COLOR_ICW + stage] = 0u;
        canonical[IDX_COLOR_OCW + stage] = 0u;
    }
    for (uint32_t stage = 0u; stage < count; stage++) {
        canonical[IDX_ALPHA_OCW + stage] &= OUTPUT_KEY_MASK;
        canonical[IDX_COLOR_OCW + stage] &= OUTPUT_KEY_MASK;
    }
    canonical[IDX_FINAL1] &= FINAL1_KEY_MASK;
    canonical[IDX_CONTROL] &= CONTROL_KEY_MASK;
    {
        bool dot = false;
        for (uint32_t stage = 0u; stage < GPU_COMBINER_TEXTURE_STAGES; stage++) {
            dot = dot || ((effective_program >> (5u * stage)) & 0x1Fu) == TEX_DOT_ST ||
                  ((effective_program >> (5u * stage)) & 0x1Fu) == TEX_DOT_PRODUCT;
        }
        canonical[IDX_DOT_MAPPING] = dot ? canonical[IDX_DOT_MAPPING] & DOT_MAPPING_KEY_MASK : 0u;
        canonical[IDX_OTHER_INPUT] = dot ? canonical[IDX_OTHER_INPUT] & OTHER_INPUT_KEY_MASK : 0u;
    }
    for (uint32_t i = 0u; i < GPU_COMBINER_KEY_WORDS; i++) {
        plan->key[i] = canonical[key_indices[i]];
    }
    gpu_combiner_name(plan->key, plan->name);

    for (uint32_t k = 0u; k < 8u; k++) {
        if (state->combiner_written[IDX_FACTOR0 + k]) {
            unpack_constant(w[IDX_FACTOR0 + k], plan->constants + k * 4u);
        }
        if (state->combiner_written[IDX_FACTOR1 + k]) {
            unpack_constant(w[IDX_FACTOR1 + k], plan->constants + (8u + k) * 4u);
        }
    }
    if (state->combiner_written[IDX_FINAL_C0]) {
        unpack_constant(w[IDX_FINAL_C0], plan->constants + 16u * 4u);
    }
    if (state->combiner_written[IDX_FINAL_C1]) {
        unpack_constant(w[IDX_FINAL_C1], plan->constants + 17u * 4u);
    }
    if (plan->uses_fog && state->output_written[GPU_PGRAPH_OUT_FOG_COLOR]) {
        const uint32_t colour = state->output[GPU_PGRAPH_OUT_FOG_COLOR];
        /* Original 003D7010 converts ARGB to method ABGR, unlike factor words. */
        plan->constants[18u * 4u] = factor_byte(colour, 0u);
        plan->constants[18u * 4u + 1u] = factor_byte(colour, 8u);
        plan->constants[18u * 4u + 2u] = factor_byte(colour, 16u);
        plan->constants[18u * 4u + 3u] = factor_byte(colour, 24u);
    }
    if (plan->uses_fog)
        inferences |= GPU_COMBINER_INFER_CONSTANT_BYTES | GPU_COMBINER_INFER_COLOUR_RANGE;
    plan->stage_count = count;
    plan->reads = an.reads;
    plan->used_inferences = inferences & ~GPU_COMBINER_INFER_TEXTURE_MODES;
    return GPU_PGRAPH_OK;
}

/* --- SPIR-V interface scan ----------------------------------------------------------------- */

#define SPV_MAGIC 0x07230203u
#define SPV_OP_ENTRY_POINT 15u
#define SPV_OP_VARIABLE 59u
#define SPV_OP_DECORATE 71u
#define SPV_DECORATION_LOCATION 30u
#define SPV_MAX_IDS (1u << 20)

bool gpu_spirv_interface_locations(const uint32_t *words, size_t word_count, uint32_t storage_class,
                                   uint32_t *out_mask)
{
    if (words == NULL || out_mask == NULL || word_count < 5u || words[0] != SPV_MAGIC) {
        return false;
    }
    *out_mask = 0u;
    const uint32_t bound = words[3];
    if (bound == 0u || bound > SPV_MAX_IDS) {
        return false;
    }
    /* Two passes over the instructions: find the first entry point's interface, then match its
     * variables against their decorations. Ids are looked up linearly, the modules are small. */
    const uint32_t *interface_ids = NULL;
    size_t interface_count = 0u;
    for (size_t at = 5u; at < word_count;) {
        const uint32_t opcode = words[at] & 0xFFFFu;
        const size_t length = words[at] >> 16;
        if (length == 0u || at + length > word_count) {
            return false;
        }
        if (opcode == SPV_OP_ENTRY_POINT && interface_ids == NULL) {
            size_t name_at = at + 3u;
            bool terminated = false;
            while (name_at < at + length) {
                const uint32_t chunk = words[name_at++];
                if ((chunk & 0xFF000000u) == 0u || (chunk & 0x00FF0000u) == 0u ||
                    (chunk & 0x0000FF00u) == 0u || (chunk & 0x000000FFu) == 0u) {
                    terminated = true;
                    break;
                }
            }
            if (!terminated) {
                return false;
            }
            interface_ids = words + name_at;
            interface_count = at + length - name_at;
            if (interface_count == 0u) {
                interface_ids = words; /* an entry point with an empty interface still counts */
            }
            at += length;
            continue;
        }
        at += length;
    }
    if (interface_ids == NULL) {
        return false;
    }
    for (size_t i = 0u; i < interface_count; i++) {
        const uint32_t id = interface_ids[i];
        bool is_class = false;
        for (size_t at = 5u; at < word_count; at += words[at] >> 16) {
            const uint32_t opcode = words[at] & 0xFFFFu;
            if (opcode == SPV_OP_VARIABLE && words[at + 2u] == id) {
                is_class = words[at + 3u] == storage_class;
                break;
            }
        }
        if (!is_class) {
            continue;
        }
        for (size_t at = 5u; at < word_count; at += words[at] >> 16) {
            const uint32_t opcode = words[at] & 0xFFFFu;
            if (opcode == SPV_OP_DECORATE && words[at + 1u] == id &&
                words[at + 2u] == SPV_DECORATION_LOCATION) {
                if (words[at + 3u] >= 32u) {
                    return false;
                }
                *out_mask |= 1u << words[at + 3u];
            }
        }
    }
    return true;
}

/* --- SPIR-V default for unwritten varyings (T462) ------------------------------------------- */

#define SPV_OP_TYPE_FLOAT 22u
#define SPV_OP_TYPE_VECTOR 23u
#define SPV_OP_TYPE_POINTER 32u
#define SPV_OP_CONSTANT 43u
#define SPV_OP_CONSTANT_COMPOSITE 44u
#define SPV_OP_ACCESS_CHAIN 65u
#define SPV_OP_IN_BOUNDS_ACCESS_CHAIN 66u
#define SPV_STORAGE_INPUT 1u
#define SPV_STORAGE_PRIVATE 6u
#define SPV_VERSION_1_4 0x00010400u
#define SPV_MAX_DEFAULTED 8u

typedef struct {
    uint32_t id;
    uint32_t pointer_type;
    uint32_t pointee;
    uint32_t component; /* the float type id of a vector or scalar pointee */
    bool vector;
} defaulted_input;

static const uint32_t *find_definition(const uint32_t *words, size_t word_count, uint32_t opcode, uint32_t id,
                                       size_t id_slot)
{
    for (size_t at = 5u; at < word_count; at += words[at] >> 16) {
        if ((words[at] & 0xFFFFu) == opcode && (words[at] >> 16) > id_slot && words[at + id_slot] == id) {
            return words + at;
        }
    }
    return NULL;
}

bool gpu_spirv_default_inputs(const uint32_t *words, size_t word_count, uint32_t locations, uint32_t **out,
                              size_t *out_count)
{
    if (words == NULL || out == NULL || out_count == NULL || word_count < 5u || words[0] != SPV_MAGIC ||
        words[3] == 0u || words[3] > SPV_MAX_IDS) {
        return false;
    }
    *out = NULL;
    *out_count = 0u;
    defaulted_input inputs[SPV_MAX_DEFAULTED];
    size_t input_count = 0u;
    for (size_t at = 5u; at < word_count;) {
        const size_t length = words[at] >> 16;
        if (length == 0u || word_count - at < length) {
            return false;
        }
        if ((words[at] & 0xFFFFu) == SPV_OP_DECORATE && length >= 4u && words[at + 2u] == SPV_DECORATION_LOCATION &&
            words[at + 3u] < 32u && ((locations >> words[at + 3u]) & 1u) != 0u) {
            const uint32_t *variable = find_definition(words, word_count, SPV_OP_VARIABLE, words[at + 1u], 2u);
            if (variable == NULL) {
                return false;
            }
            if (variable[3] != SPV_STORAGE_INPUT) {
                at += length; /* an Output may share the Location number */
                continue;
            }
            if (input_count == SPV_MAX_DEFAULTED) {
                return false;
            }
            defaulted_input *input = &inputs[input_count++];
            input->id = variable[2];
            input->pointer_type = variable[1];
            const uint32_t *pointer = find_definition(words, word_count, SPV_OP_TYPE_POINTER, input->pointer_type, 1u);
            if (pointer == NULL) {
                return false;
            }
            input->pointee = pointer[3];
            const uint32_t *vector = find_definition(words, word_count, SPV_OP_TYPE_VECTOR, input->pointee, 1u);
            const uint32_t *scalar = find_definition(words, word_count, SPV_OP_TYPE_FLOAT, input->pointee, 1u);
            input->vector = vector != NULL;
            if (vector != NULL && vector[3] == 4u) {
                input->component = vector[2];
            } else if (scalar != NULL) {
                input->component = input->pointee;
            } else {
                return false; /* only a float vec4 or a float is defaulted */
            }
        }
        at += length;
    }
    if (input_count == 0u) {
        return false;
    }
    /* A component access straight on the input would keep its Input pointer type: not rewritten. */
    for (size_t at = 5u; at < word_count; at += words[at] >> 16) {
        const uint32_t opcode = words[at] & 0xFFFFu;
        if (opcode == SPV_OP_ACCESS_CHAIN || opcode == SPV_OP_IN_BOUNDS_ACCESS_CHAIN) {
            for (size_t i = 0u; i < input_count; i++) {
                if (words[at + 3u] == inputs[i].id) {
                    return false;
                }
            }
        }
    }
    uint32_t *result = malloc((word_count + 24u * input_count) * sizeof *result);
    if (result == NULL) {
        return false;
    }
    uint32_t bound = words[3];
    size_t used = 5u;
    memcpy(result, words, 5u * sizeof *result);
    for (size_t at = 5u; at < word_count;) {
        const uint32_t opcode = words[at] & 0xFFFFu;
        const size_t length = words[at] >> 16;
        bool drop = false;
        if (opcode == SPV_OP_DECORATE) {
            for (size_t i = 0u; i < input_count; i++) {
                drop = drop || words[at + 1u] == inputs[i].id;
            }
        }
        if (opcode == SPV_OP_ENTRY_POINT && words[1] < SPV_VERSION_1_4) {
            /* Before SPIR-V 1.4 the interface lists Input and Output variables only, and a Private one is not one. */
            size_t name_at = at + 1u + 2u;
            while (name_at < at + length) {
                const uint32_t chunk = words[name_at++];
                if ((chunk & 0xFF000000u) == 0u || (chunk & 0x00FF0000u) == 0u || (chunk & 0x0000FF00u) == 0u ||
                    (chunk & 0x000000FFu) == 0u) {
                    break;
                }
            }
            const size_t head = name_at - at;
            const size_t first = used;
            memcpy(result + used, words + at, head * sizeof *result);
            used += head;
            for (size_t word = name_at; word < at + length; word++) {
                bool removed = false;
                for (size_t i = 0u; i < input_count; i++) {
                    removed = removed || words[word] == inputs[i].id;
                }
                if (!removed) {
                    result[used++] = words[word];
                }
            }
            result[first] = (words[at] & 0xFFFFu) | ((uint32_t)(used - first) << 16);
            at += length;
            continue;
        }
        if (opcode == SPV_OP_VARIABLE) {
            const defaulted_input *input = NULL;
            for (size_t i = 0u; i < input_count; i++) {
                if (words[at + 2u] == inputs[i].id) {
                    input = &inputs[i];
                }
            }
            if (input != NULL) {
                const uint32_t zero = bound++;
                result[used++] = (4u << 16) | SPV_OP_CONSTANT;
                result[used++] = input->component;
                result[used++] = zero;
                result[used++] = 0x00000000u; /* 0.0f */
                uint32_t initial = zero;
                if (input->vector) {
                    const uint32_t one = bound++;
                    result[used++] = (4u << 16) | SPV_OP_CONSTANT;
                    result[used++] = input->component;
                    result[used++] = one;
                    result[used++] = 0x3F800000u; /* 1.0f */
                    initial = bound++;
                    result[used++] = (7u << 16) | SPV_OP_CONSTANT_COMPOSITE;
                    result[used++] = input->pointee;
                    result[used++] = initial;
                    result[used++] = zero;
                    result[used++] = zero;
                    result[used++] = zero;
                    result[used++] = one;
                }
                const uint32_t pointer = bound++;
                result[used++] = (4u << 16) | SPV_OP_TYPE_POINTER;
                result[used++] = pointer;
                result[used++] = SPV_STORAGE_PRIVATE;
                result[used++] = input->pointee;
                result[used++] = (5u << 16) | SPV_OP_VARIABLE;
                result[used++] = pointer;
                result[used++] = input->id;
                result[used++] = SPV_STORAGE_PRIVATE;
                result[used++] = initial;
                at += length;
                continue;
            }
        }
        if (!drop) {
            memcpy(result + used, words + at, length * sizeof *result);
            used += length;
        }
        at += length;
    }
    result[3] = bound;
    *out = result;
    *out_count = used;
    return true;
}

/* --- SPIR-V texel coordinates (T510) -------------------------------------------------------- */

#define SPV_OP_LOAD 61u
#define SPV_OP_FUNCTION 54u
#define SPV_OP_FDIV 136u
#define SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD 87u
#define SPV_DECORATION_BINDING 33u
#define SPV_STORAGE_UNIFORM_CONSTANT 0u

typedef struct {
    uint32_t load; /* the result id of the OpLoad of the stage's sampler variable */
    uint32_t divisor; /* the OpConstantComposite id of (width, height) */
    bool active;
} texel_stage;

/* Is `id` the result of an instruction whose result type is `type`? */
static bool id_has_type(const uint32_t *words, size_t word_count, uint32_t id, uint32_t type)
{
    for (size_t at = 5u; at < word_count; at += words[at] >> 16) {
        if ((words[at] >> 16) >= 3u && words[at + 1u] == type && words[at + 2u] == id) {
            return true;
        }
    }
    return false;
}

bool gpu_spirv_texel_coordinates(const uint32_t *words, size_t word_count, uint32_t stages, const float size[][2],
                                 uint32_t **out, size_t *out_count)
{
    if (words == NULL || size == NULL || out == NULL || out_count == NULL || word_count < 5u || words[0] != SPV_MAGIC ||
        words[3] == 0u || words[3] > SPV_MAX_IDS || stages == 0u || (stages & ~0xFu) != 0u) {
        return false;
    }
    *out = NULL;
    *out_count = 0u;
    texel_stage found[4] = {{0u, 0u, false}, {0u, 0u, false}, {0u, 0u, false}, {0u, 0u, false}};
    uint32_t float_type = 0u;
    uint32_t vector_type = 0u;
    size_t first_function = 0u;
    for (size_t at = 5u; at < word_count;) {
        const uint32_t opcode = words[at] & 0xFFFFu;
        const size_t length = words[at] >> 16;
        if (length == 0u || word_count - at < length) {
            return false;
        }
        if (opcode == SPV_OP_TYPE_FLOAT && length >= 3u && words[at + 2u] == 32u && float_type == 0u) {
            float_type = words[at + 1u];
        }
        if (opcode == SPV_OP_FUNCTION && first_function == 0u) {
            first_function = at;
        }
        at += length;
    }
    if (float_type == 0u || first_function == 0u) {
        return false;
    }
    for (size_t at = 5u; at < first_function; at += words[at] >> 16) {
        if ((words[at] & 0xFFFFu) == SPV_OP_TYPE_VECTOR && words[at + 2u] == float_type && words[at + 3u] == 2u) {
            vector_type = words[at + 1u];
            break;
        }
    }
    if (vector_type == 0u) {
        return false;
    }
    for (uint32_t stage = 0u; stage < 4u; stage++) {
        if (((stages >> stage) & 1u) == 0u) {
            continue;
        }
        uint32_t variable = 0u;
        for (size_t at = 5u; at < first_function; at += words[at] >> 16) {
            if ((words[at] & 0xFFFFu) == SPV_OP_DECORATE && (words[at] >> 16) >= 4u &&
                words[at + 2u] == SPV_DECORATION_BINDING && words[at + 3u] == 2u + stage) {
                const uint32_t *definition = find_definition(words, word_count, SPV_OP_VARIABLE, words[at + 1u], 2u);
                if (definition != NULL && definition[3] == SPV_STORAGE_UNIFORM_CONSTANT) {
                    variable = definition[2];
                }
            }
        }
        if (variable == 0u) {
            return false;
        }
        for (size_t at = first_function; at < word_count; at += words[at] >> 16) {
            if ((words[at] & 0xFFFFu) == SPV_OP_LOAD && (words[at] >> 16) >= 4u && words[at + 3u] == variable) {
                if (found[stage].active) {
                    return false; /* the sampler is loaded twice: not the shape this rewrite knows */
                }
                found[stage].active = true;
                found[stage].load = words[at + 2u];
            }
        }
        if (!found[stage].active || size[stage][0] < 1.0f || size[stage][1] < 1.0f) {
            return false;
        }
    }
    /* Every use of a stage's loaded sampler must be the one sample the rewrite divides the coordinate of. */
    size_t samples = 0u;
    for (size_t at = first_function; at < word_count; at += words[at] >> 16) {
        const uint32_t opcode = words[at] & 0xFFFFu;
        const size_t length = words[at] >> 16;
        for (uint32_t stage = 0u; stage < 4u; stage++) {
            if (!found[stage].active) {
                continue;
            }
            for (size_t operand = 1u; operand < length; operand++) {
                if (opcode == SPV_OP_LOAD && operand == 2u) {
                    continue; /* the result id of the load itself */
                }
                if (words[at + operand] != found[stage].load) {
                    continue;
                }
                if (opcode == SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD && operand == 3u && length >= 5u) {
                    if (!id_has_type(words, word_count, words[at + 4u], vector_type)) {
                        return false; /* the coordinate is not a vec2 */
                    }
                    samples++;
                } else if (!(opcode == SPV_OP_LOAD && operand == 2u)) {
                    return false;
                }
            }
        }
    }
    if (samples == 0u) {
        return false;
    }
    uint32_t bound = words[3];
    uint32_t *result = malloc((word_count + 13u * 4u + 5u * samples) * sizeof *result);
    if (result == NULL) {
        return false;
    }
    size_t used = 0u;
    memcpy(result, words, first_function * sizeof *result);
    used = first_function;
    for (uint32_t stage = 0u; stage < 4u; stage++) {
        if (!found[stage].active) {
            continue;
        }
        uint32_t bits[2];
        memcpy(&bits[0], &size[stage][0], sizeof bits[0]);
        memcpy(&bits[1], &size[stage][1], sizeof bits[1]);
        const uint32_t width = bound++;
        const uint32_t height = bound++;
        found[stage].divisor = bound++;
        result[used++] = (4u << 16) | SPV_OP_CONSTANT;
        result[used++] = float_type;
        result[used++] = width;
        result[used++] = bits[0];
        result[used++] = (4u << 16) | SPV_OP_CONSTANT;
        result[used++] = float_type;
        result[used++] = height;
        result[used++] = bits[1];
        result[used++] = (5u << 16) | SPV_OP_CONSTANT_COMPOSITE;
        result[used++] = vector_type;
        result[used++] = found[stage].divisor;
        result[used++] = width;
        result[used++] = height;
    }
    for (size_t at = first_function; at < word_count;) {
        const uint32_t opcode = words[at] & 0xFFFFu;
        const size_t length = words[at] >> 16;
        const texel_stage *match = NULL;
        if (opcode == SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD && length >= 5u) {
            for (uint32_t stage = 0u; stage < 4u; stage++) {
                if (found[stage].active && words[at + 3u] == found[stage].load) {
                    match = &found[stage];
                }
            }
        }
        if (match == NULL) {
            memcpy(result + used, words + at, length * sizeof *result);
            used += length;
            at += length;
            continue;
        }
        const uint32_t divided = bound++;
        result[used++] = (5u << 16) | SPV_OP_FDIV;
        result[used++] = vector_type;
        result[used++] = divided;
        result[used++] = words[at + 4u];
        result[used++] = match->divisor;
        memcpy(result + used, words + at, length * sizeof *result);
        result[used + 4u] = divided;
        used += length;
        at += length;
    }
    result[3] = bound;
    *out = result;
    *out_count = used;
    return true;
}

gpu_pgraph_result gpu_combiner_plan_build(const gpu_pgraph_state *state, uint32_t allowed,
    const gpu_combiner_texture textures[GPU_COMBINER_TEXTURE_STAGES],
    gpu_combiner_plan *plan, char *error, size_t error_size)
{
    return gpu_combiner_plan_build_profile(state, allowed, textures, false, plan, error, error_size);
}
