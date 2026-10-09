/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Batch runner for the translated NV2A VERTEX shader form (T100). Driven by
 * tools/nv2a/vertex_device.py, compiled together with vkrun_core.c like vkrun.c.
 *
 *   vkvert [--zero-out] MANIFEST   one job per line: SPIRV IN OUT COUNT
 *   vkvert --info                  print the device and its capabilities
 *
 * IN and OUT are the same files vkrun uses (832 and 64 float32 per record) so one vector
 * file feeds both forms. The vertex shader is run once per record (a one-point draw with
 * rasterizer discard) and its xfb-decorated outputs are captured with
 * VK_EXT_transform_feedback, see vkrun_core.c. The device is the software one unless
 * VKRUN_DEVICE names another ("hardware", or a device-name substring such as RADV).
 */
#include <stdint.h>

#include "vkrun_core.h"

int main(int argc, char **argv)
{
    static const VkrunProfile profile = {
        .program = "vkvert",
        .usage = "usage: vkvert [--zero-out] MANIFEST | vkvert --info",
        .fixed_in_floats = 832u,
        .fixed_out_floats = 64u,
        .accept_zero_out = 1,
        .brief_info = 0,
        .fence_wait_ns = 120ull * 1000000000ull,
        .nonzero_exit_on_job_fail = 0,
        .stage = VKRUN_STAGE_VERTEX,
    };
    return vkrun_main(&profile, argc, argv);
}
