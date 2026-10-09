/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Batch compute runner that evaluates translated combiner shaders on the SOFTWARE
 * Vulkan device (lavapipe). Compiled on demand by tools/nv2a_combiner/validate.py
 * together with tools/nv2a/vkrun_core.c, the implementation shared with the
 * vertex-program runner tools/nv2a/vkrun.c.
 *
 *   vkeval --info                   print the device name
 *   vkeval MANIFEST                 one job per line:
 *                                   SPIRV_PATH IN_PATH OUT_PATH COUNT IN_FLOATS OUT_FLOATS
 *
 * Per job IN holds COUNT * IN_FLOATS little-endian float32 and OUT receives COUNT *
 * OUT_FLOATS. Binding 0 is the input storage buffer, binding 1 the output, one push
 * constant `uint count`, 64 invocations per group. The output is pre-filled with a NaN
 * so a shader that skips a slot is visible. A failing job prints "FAIL <index> <why>" and
 * the batch carries on, and any failure makes the exit status 1: the Python driver keys
 * off the exit status rather than the per-line verdicts. A malformed manifest line is
 * reported as a failing job (the pre-consolidation runner skipped it silently).
 */
#include "../nv2a/vkrun_core.h"

int main(int argc, char **argv)
{
    static const VkrunProfile profile = {
        .program = "vkeval",
        .usage = "usage: vkeval --info | MANIFEST",
        .fixed_in_floats = 0u,
        .fixed_out_floats = 0u,
        .accept_zero_out = 0,
        .brief_info = 1,
        .fence_wait_ns = 60ull * 1000000000ull,
        .nonzero_exit_on_job_fail = 1,
    };
    return vkrun_main(&profile, argc, argv);
}
