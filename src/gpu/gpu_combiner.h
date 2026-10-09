/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The register-combiner fragment stage of the pushbuffer replay (T75, fragment half, T84e). NO
 * VULKAN HERE, so every decision is testable on any machine. The draw is gpu_vsh_draw.h's
 * `gpu_vsh_fragment`, the glue is gpu_pgraph_replay.c.
 *
 * WHAT THIS FILE DOES. From the combiner shadow gpu_pgraph.c decoded (57 dwords, the pixel-shader
 * definition's own indices, see gpu_pgraph.h) it
 *
 *   1. checks the configuration the way tools/nv2a_combiner/config.py does (stage count, which
 *      registers a stage may read and write, the output operation code, a dot product on an alpha
 *      half) and works out which registers it READS, so that what it refuses is decided here and
 *      not by a failed lookup;
 *   2. names the translated fragment module by the SHA-256 of the 36 structure dwords
 *      (tools/nv2a_combiner/replay_modules.py structure_key is the same function in Python, and
 *      tests/test_gpu_combiner_replay.py proves they agree), so the module is found by its bytes
 *      and there is no nearest match;
 *   3. builds the 20 vec4 of the fragment constants block (factors and final constants).
 *
 * WHAT IS MEASURED. The method numbers and which word each one carries come from the image's own
 * writer (d3d8_shader.c pixel_shader_apply, d3d8_pixel_constants.c) and its render-state table.
 * The packing of a factor (A in the top byte) is what the title's packer 0x003D9520 writes.
 * The field layout of every word is docs/combiner-translator.md section 3 (two derivations).
 *
 * T1490: modes 3, 9 and 17 are INFERRED (GPU_COMBINER_INFER_TEXTURE_MODES), see docs/t1488-render-gaps.md.
 *
 * WHAT IS REFUSED, always (no inference bit lifts these):
 *   - a combiner that reads the fog register (3): the fog colour method is in no measured emitter;
 *   - a texture stage program (word 54) with any mode other than 0, 1 and (T1490, INFERRED) 3, 9, 17:
 *     a 2D texture sampled at (s / q, t / q), a cube map, and the texm3x2 pair, from a caller-provided texture;
 *   - a texture register (8..11) read without a test texture for that stage, and a test texture
 *     for a stage whose program says no texture;
 *   - a configuration the translator cannot translate (the same rules as config.decode).
 *
 * WHAT IS INFERRED, refused unless the caller allows the bit and reported when used:
 *   UNWRITTEN        a word the draw needs and the stream never wrote reads as 0
 *   STAGE_PROGRAM    word 54 never written: stage n samples a texture iff a test texture exists
 *   INITIAL_STATE    a stage reads spare0 / spare1 before any stage wrote it
 *   CONSTANT_BYTES   a factor word is ARGB, A in the top byte, each byte divided by 255
 *   COLOUR_RANGE     oD0 and oD1 reach the combiner as the vertex program wrote them, unclamped
 *   TEXTURE_SAMPLING a test texture is sampled nearest, clamped to the edge, row 0 on top
 *   TEXTURE_MODES    stage programs 3, 9 and 17 follow xemu's generated GLSL (T1490)
 */

#ifndef TSFP_GPU_COMBINER_H
#define TSFP_GPU_COMBINER_H

#include "gpu_pgraph.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GPU_COMBINER_INFER_UNWRITTEN 0x80u
#define GPU_COMBINER_INFER_STAGE_PROGRAM 0x100u
#define GPU_COMBINER_INFER_INITIAL_STATE 0x200u
#define GPU_COMBINER_INFER_CONSTANT_BYTES 0x400u
#define GPU_COMBINER_INFER_COLOUR_RANGE 0x800u
#define GPU_COMBINER_INFER_TEXTURE_SAMPLING 0x1000u
/* T1490: the stage programs 3 (cube map), 9 (DOT_ST) and 17 (DOTPRODUCT) of xemu psh.c, never compared with the original. */
/* Not in GPU_COMBINER_INFER_ALL and not in the backend's allowed_inferences: gpu_pgraph_resolve_fragment adds it when
 * `live_texture_modes` is set, and it never appears in used_inferences (see gpu_combiner_plan.uses_texture_modes). */
#define GPU_COMBINER_INFER_TEXTURE_MODES 0x80000000u
#define GPU_COMBINER_INFER_ALL 0x1F80u

#define GPU_COMBINER_TEXTURE_STAGES 4u
/* factor0[8], factor1[8], final_c0, final_c1, fog_color, alpha_ref: the std140 block of
 * tools/nv2a_combiner/glsl.py replay_fragment_shader, vec4 each. */
#define GPU_COMBINER_CONSTANT_VEC4S 20u
#define GPU_COMBINER_KEY_WORDS 38u
#define GPU_COMBINER_LEGACY_KEY_WORDS 36u /* the words hashed when the last two are zero (no dot stage) */
#define GPU_COMBINER_NAME_BYTES 80u

/* A caller-provided stand-in for the title's texture of one stage. NOT the title's texture: the
 * image's texture state is not decoded (T84e). RGBA8, row 0 on top. */
typedef struct {
    const uint8_t *rgba; /* NULL means no test texture for the stage */
    uint32_t width;
    uint32_t height;
    /* T510, a render target image bound as the stage's texture (all default to the stand-in behaviour). `linear`: sample
     * bilinear (the stream's measured filter 0x02062000) instead of nearest. `unnormalised`: the vertex program's texture
     * coordinate is in TEXELS (a hardware linear A8R8G8B8 texture, measured Format 0x00011229), the replay divides it by the
     * size. `refusal`: when `rgba` is NULL, the NAMED reason the stage has no texture (printed instead of the generic text). */
    bool linear;
    bool unnormalised;
    const char *refusal;
    bool repeat; /* T735: the stream's address word is U and V wrap 0x101 (INFERRED nxdk names), else clamp 0x303 */
    bool cube;   /* T1490: a six face cube map (Format bit 2), sampled by a samplerCube; the 2D modes refuse it, mode 3 requires it */
} gpu_combiner_texture;

typedef struct {
    char name[GPU_COMBINER_NAME_BYTES]; /* "combiner_<sha256 hex>", the module file stem */
    float constants[GPU_COMBINER_CONSTANT_VEC4S * 4u];
    uint32_t stage_count;
    bool uses_fog; /* Only the explicit v5 programmable-fog profile may set this. */
    uint32_t reads;           /* bit r: source register r is read (0..15) */
    uint32_t texture_stages;  /* bit n: stage n samples its test texture (cube stage: a cube sampler) */
    bool uses_texture_modes;  /* T1490: a stage program 3, 9 or 17 is in the plan */
    uint32_t cube_stages;     /* T1490: bit n: stage n is mode 3, its texture must be a cube map */
    uint32_t coordinate_stages; /* T1490: bit n: the module reads the coordinate varying oTn (a dot product stage reads one and samples none) */
    uint32_t used_inferences; /* GPU_COMBINER_INFER_* bits this draw depends on */
    uint32_t key[GPU_COMBINER_KEY_WORDS];
} gpu_combiner_plan;

/* The indices of the 36 structure dwords, ascending: the words that change the translation. The
 * factors (10..25), the final constants (43, 44) and the words of a mode this replay refuses
 * (42, 55, 56) are not among them. tools/nv2a_combiner/replay_modules.py STRUCTURE_INDICES. */
const uint8_t *gpu_combiner_key_indices(void);

/* Digest the key words into the module name `combiner_<hex>`. */
void gpu_combiner_name(const uint32_t key[GPU_COMBINER_KEY_WORDS], char out[GPU_COMBINER_NAME_BYTES]);

/* Decide one draw. GPU_PGRAPH_OK fills `plan`. Every refusal is GPU_PGRAPH_ERR_UNMEASURED (or
 * ERR_ARGUMENT) with a message in `error`. `allowed` is a mask of GPU_COMBINER_INFER_* bits. */
gpu_pgraph_result gpu_combiner_plan_build(const gpu_pgraph_state *state, uint32_t allowed,
                                          const gpu_combiner_texture textures[GPU_COMBINER_TEXTURE_STAGES],
                                          gpu_combiner_plan *plan, char *error, size_t error_size);

/* Explicit opt-in profile; legacy gpu_combiner_plan_build still refuses fog.
 * No new overlapping uint32 inference bit is allocated. Fog also requires
 * CONSTANT_BYTES and COLOUR_RANGE permissions plus actual fog state words. */
gpu_pgraph_result gpu_combiner_plan_build_profile(const gpu_pgraph_state *state, uint32_t allowed,
    const gpu_combiner_texture textures[GPU_COMBINER_TEXTURE_STAGES], bool live_fog,
    gpu_combiner_plan *plan, char *error, size_t error_size);

/* The Location of the interface variables of the FIRST entry point of a SPIR-V module, one bit per
 * location, for storage class 1 (Input) or 3 (Output). Only variables carrying a Location
 * decoration count (built-ins do not). False when the words are not a module, there is no entry
 * point, or a Location is 32 or more. */
bool gpu_spirv_interface_locations(const uint32_t *words, size_t word_count, uint32_t storage_class,
                                   uint32_t *out_mask);

/* T462. Rewrite a fragment module so each Input variable at a Location in `locations` becomes a Private variable that
 * starts as the NV2A's initial value of a vertex program output the program never wrote: (0, 0, 0, 1) for a vec4, 0 for
 * a float (the xboxdevwiki NV2A vertex shader page, INFERRED, no hardware run here). Only a float vec4 or float input
 * that is never reached through an access chain is rewritten. Returns a malloc'd copy in `*out` (free it), false when
 * nothing was rewritten (no such input, an unsupported shape, or no memory). */
bool gpu_spirv_default_inputs(const uint32_t *words, size_t word_count, uint32_t locations, uint32_t **out,
                              size_t *out_count);

/* T510. Rewrite a fragment module so the texture coordinate of each stage in `stages` (bit n: the sampler at binding 2 + n) is
 * divided by `size[n]` (width, height) before the sample: a hardware LINEAR texture takes its coordinate in texels, the
 * module's `texture()` call is normalised. The one `OpImageSampleImplicitLod` of the stage must take a vec2 coordinate straight
 * from the load of its sampler. Returns a malloc'd copy in `*out` (free it), false when a stage has no such sample, an unsupported
 * shape (a sample of another opcode, a coordinate that is not a vec2, no float or vec2 type) or no memory. */
bool gpu_spirv_texel_coordinates(const uint32_t *words, size_t word_count, uint32_t stages, const float size[][2],
                                 uint32_t **out, size_t *out_count);

#endif
