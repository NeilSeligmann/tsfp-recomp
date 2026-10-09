/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shared core of the two batch compute runners used for shader validation:
 * tools/nv2a/vkrun.c (vertex programs) and tools/nv2a_combiner/vkeval.c
 * (register combiners). Each entry point fills in a VkrunProfile with the few
 * things that genuinely differ and calls vkrun_main(); everything Vulkan is
 * in vkrun_core.c, compiled together with the entry file.
 *
 * T100 added a second shader stage: VKRUN_STAGE_VERTEX runs the translated VERTEX
 * shader form on one draw per record with rasterizer discard, and reads gl_Position and
 * the varyings back through VK_EXT_transform_feedback. The manifest, buffers' contents
 * is described in vkrun_core.c. Device choice is the VKRUN_DEVICE variable (T98, T100).
 */
#ifndef TSFP_NV2A_VKRUN_CORE_H
#define TSFP_NV2A_VKRUN_CORE_H

#include <stdint.h>

typedef enum {
    VKRUN_STAGE_COMPUTE = 0, /* default: a compute shader over storage buffers */
    VKRUN_STAGE_VERTEX = 1   /* a vertex shader, captured by transform feedback */
} VkrunStage;

typedef struct {
    /* Name used in error messages ("vkrun" or "vkeval"). */
    const char *program;
    /* One-line usage text printed on a bad command line (exit 2). */
    const char *usage;
    /* Nonzero: every manifest line is `SPIRV IN OUT COUNT` and these are the
     * per-invocation input/output widths in float32s. Zero: every line is
     * `SPIRV IN OUT COUNT IN_FLOATS OUT_FLOATS` and carries its own widths. */
    uint32_t fixed_in_floats;
    uint32_t fixed_out_floats;
    /* Accept --zero-out (pre-fill the output buffer with zeros instead of the
     * NaN sentinel that exposes slots a shader never wrote). */
    int accept_zero_out;
    /* --info prints just the device name instead of the full float-controls
     * block. */
    int brief_info;
    /* How long one dispatch may take before the job fails. */
    uint64_t fence_wait_ns;
    /* Exit 1 when any job printed FAIL. vkrun always exits 0 for a processed
     * manifest (its Python driver reads the per-line verdicts), vkeval's
     * driver keys off the exit status. */
    int nonzero_exit_on_job_fail;
    /* VKRUN_STAGE_COMPUTE (zero) or VKRUN_STAGE_VERTEX. The vertex stage needs fixed
     * widths (832 in, 64 out) and a graphics queue plus VK_EXT_transform_feedback. */
    VkrunStage stage;
} VkrunProfile;

int vkrun_main(const VkrunProfile *profile, int argc, char **argv);

#endif
