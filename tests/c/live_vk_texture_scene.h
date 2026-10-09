/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T791 test fixtures shared by test_live_vk_bind.c and test_live_vk_frame.c: a combiner draw that samples one guest texture
 * (swizzled or linear A8R8G8B8 2 x 2) through a one sample module registered in slot 4 of the fragment table under the plan's
 * own name. The includer defines CHECK and includes live_vk_scenes.h and live_vk_bind.h first.
 */
#ifndef TSFP_LIVE_VK_TEXTURE_SCENE_H
#define TSFP_LIVE_VK_TEXTURE_SCENE_H

#include "gpu_combiner_words.h"
#include "live_vk_texel_module.h"

#define TEXTURE_OFFSET 0x300u
#define TEXTURE_DATA (MEMORY_BASE + TEXTURE_OFFSET)
#define SWIZZLED_A8R8G8B8_2X2 0x01110621u /* colour 0x06, one level, 2D, exponents 1 x 1, Size word 0 */
#define LINEAR_A8R8G8B8 0x00011229u /* the xemu measured linear texture */
#define WRAP_BOTH 0x00000101u
#define CLAMP_BOTH 0x00000303u
#define BILINEAR 0x02062000u

#include "gpu_combiner_words.h"
#include "live_vk_texel_module.h"

static const char *probe_names[5];
static const struct gpu_vsh_table probe_table = {0u, 0u, NULL, 0u, NULL, 5u, probe_names};

__attribute__((unused)) static bool load_probe_module(void *context, uint32_t module, const uint32_t **words, size_t *word_count)
{
    if (module == 4u) {
        *words = texel_probe_words;
        *word_count = sizeof texel_probe_words / sizeof(uint32_t);
        return true;
    }
    return load_fragment_module(context, module, words, word_count);
}

static bool linear_texture; /* the binding the title "holds": linear A8R8G8B8 (texel coordinates) or swizzled */
static bool wide_texture;   /* linear 16 x 8 (pitch 64, tight) instead of 2 x 2, with `target_binding` it names a registered target */
static bool swizzled_target_binding; /* T1489: the binding is a swizzled A8R8G8B8 header (0x06610629: 1 level, 64 x 64, no Size word) at SWIZZLED_TARGET_DATA */
#define SWIZZLED_TARGET_DATA 0x0500000u
static bool target_binding; /* the binding names the render target COPY_SOURCE (same pixels as the guest copy) */
#define WIDE_SIZE_WORD (15u | (7u << 12))
#define WIDE_TARGET_DATA 0x0100000u

/* Two rows of two BGRA texels: swizzled 2 x 2 is row major in memory, linear has a 64 byte pitch. */
__attribute__((unused)) static void write_texels(const uint8_t bgra[16])
{
    memset(memory + TEXTURE_OFFSET, 0, sizeof memory - TEXTURE_OFFSET);
    if (linear_texture) {
        memcpy(memory + TEXTURE_OFFSET, bgra, 8u);
        memcpy(memory + TEXTURE_OFFSET + 64u, bgra + 8u, 8u);
    } else {
        memcpy(memory + TEXTURE_OFFSET, bgra, 16u);
    }
}

/* The A = t0 combiner words of the combiner test's pass configuration. */
__attribute__((unused)) static void texture_words(uint32_t words[COMBINER_WORDS])
{
    static const struct {
        uint32_t index, value;
    } entries[] = {{34u, 0x08200000u}, {0u, 0xD4300000u}, {45u, 0xC0u}, {26u, 0xC0u}, {53u, 0x11101u}, {8u, 0x0Cu}, {9u, 0x1C80u},
       {10u, 0x00FF8040u}};
    memset(words, 0, COMBINER_WORDS * sizeof words[0]);
    for (size_t i = 0u; i < sizeof entries / sizeof entries[0]; i++) {
        words[entries[i].index] = entries[i].value;
    }
}

__attribute__((unused)) static gpu_pgraph *textured_model(bool write_sampler, uint32_t address, uint32_t filter)
{
    gpu_pgraph *model = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(model, ALL_GROUPS | GPU_PGRAPH_OUTPUT_TEXTURE);
    gpu_pgraph_set_combiner(model, true);
    stream_builder stream = {0};
    stream_setup(&stream);
    uint32_t words[COMBINER_WORDS];
    texture_words(words);
    stream_pixel_shader(&stream, words);
    if (write_sampler) {
        stream_pair(&stream, 0x1B08u, address);
        stream_pair(&stream, 0x1B14u, filter);
    }
    stream_draw(&stream, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
    CHECK(gpu_pgraph_decode(model, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    stream_free(&stream);
    return model;
}

__attribute__((unused)) static bool binding_of(void *context, size_t draw, uint32_t stage, live_texture_binding *out, const char **refusal)
{
    (void)context;
    (void)draw;
    if (stage != 0u) {
        *refusal = "stage not bound by the title";
        return false;
    }
    if (swizzled_target_binding) {
        *out = (live_texture_binding){.header = 0u, .format = 0x06610629u, .size_word = 0u, .data = SWIZZLED_TARGET_DATA};
        return true;
    }
    if (wide_texture) {
        *out = (live_texture_binding){.header = 0u, .format = LINEAR_A8R8G8B8, .size_word = WIDE_SIZE_WORD,
                                      .data = target_binding ? WIDE_TARGET_DATA : TEXTURE_DATA};
        return true;
    }
    *out = linear_texture ? (live_texture_binding){.header = 0u, .format = LINEAR_A8R8G8B8, .size_word = 0x00001001u, .data = TEXTURE_DATA}
                          : (live_texture_binding){.header = 0u, .format = SWIZZLED_A8R8G8B8_2X2, .size_word = 0u, .data = TEXTURE_DATA};
    return true;
}

__attribute__((unused)) static gpu_pgraph_backend probe_backend(fake_guest *guest)
{
    gpu_pgraph_backend backend = make_backend_for(SCENE_COMBINER_FINAL, guest, EVERYTHING);
    backend.output_groups |= GPU_PGRAPH_OUTPUT_TEXTURE;
    backend.fragment_table = &probe_table;
    backend.load_fragment_module = load_probe_module;
    return backend;
}

/* The plan name of the textured configuration is the module's name in the fragment table (slot 4 is the probe). */
static char textured_name[GPU_COMBINER_NAME_BYTES];

__attribute__((unused)) static void prepare_probe_names(const gpu_pgraph *model)
{
    gpu_combiner_plan plan;
    char error[256] = "";
    static const uint8_t one_texel[4] = {1u, 2u, 3u, 255u};
    gpu_combiner_texture textures[GPU_COMBINER_TEXTURE_STAGES] = {{0}};
    textures[0].rgba = one_texel;
    textures[0].width = 1u;
    textures[0].height = 1u;
    CHECK(gpu_combiner_plan_build(gpu_pgraph_state_now(model), EVERYTHING, textures, &plan, error, sizeof error) == GPU_PGRAPH_OK);
    snprintf(textured_name, sizeof textured_name, "%s", plan.name);
    probe_names[0] = COMBINER_NAME_PASS;
    probe_names[1] = COMBINER_NAME_MULTIPLY;
    probe_names[2] = COMBINER_NAME_FINAL;
    probe_names[3] = COMBINER_NAME_TWOVARY;
    probe_names[4] = textured_name;
}

#endif
