/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Batch compute runner for NV2A vertex-program validation on the SOFTWARE Vulkan
 * device (lavapipe). Driven by tools/nv2a/validate.py, which compiles it on demand
 * together with vkrun_core.c, the implementation shared with the register-combiner
 * runner tools/nv2a_combiner/vkeval.c.
 *
 *   vkrun [--zero-out] MANIFEST     one job per line: SPIRV_PATH IN_PATH OUT_PATH COUNT
 *   vkrun --info                    print the device and its float-controls support
 *
 * Per job: IN is COUNT * 832 little-endian float32, OUT is written as COUNT * 64.
 * The shader layout is fixed: binding 0 = input storage buffer, binding 1 = output
 * storage buffer, push constant `uint count`, 64 invocations per group. A job that
 * fails prints "FAIL <index> <reason>" and the batch carries on, otherwise "OK <index>".
 * The exit status stays 0 for a processed manifest: the Python driver reads the
 * per-line verdicts.
 *
 * The output buffer is pre-filled with a NaN sentinel (or zeros with --zero-out) so a
 * shader that leaves a slot unwritten is visible rather than silently reading as zero.
 */
#include <stdint.h>

#include "vkrun_core.h"

int main(int argc, char **argv)
{
    static const VkrunProfile profile = {
        .program = "vkrun",
        .usage = "usage: vkrun [--zero-out] MANIFEST | vkrun --info",
        .fixed_in_floats = 832u,
        .fixed_out_floats = 64u,
        .accept_zero_out = 1,
        .brief_info = 0,
        .fence_wait_ns = UINT64_MAX,
        .nonzero_exit_on_job_fail = 0,
    };
    return vkrun_main(&profile, argc, argv);
}
