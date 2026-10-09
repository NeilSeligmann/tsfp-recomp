/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The one mapping of gpu_vsh_output's GPU_VSH_* codes to Vulkan enums, shared by gpu_vsh_draw.c (the replay) and
 * live_vk_pipeline.c (the live renderer, T791) so the two cannot disagree. Unknown codes map as gpu_vsh_draw.c always did
 * (check_output rejects them before they get here).
 */
#ifndef TSFP_GPU_VSH_VULKAN_MAP_H
#define TSFP_GPU_VSH_VULKAN_MAP_H

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "gpu_vsh_draw.h"

#include <vulkan/vulkan_core.h>

VkPrimitiveTopology gpu_vsh_vulkan_topology(uint32_t topology);
/* T860: GPU_VSH_POLYGON_* as the VkPolygonMode of a triangle pipeline (an unknown value is FILL, gpu_vsh_draw's check refuses it first). */
VkPolygonMode gpu_vsh_vulkan_polygon_mode(uint32_t polygon_mode);
VkBlendFactor gpu_vsh_vulkan_blend_factor(uint32_t factor);
VkCompareOp gpu_vsh_vulkan_compare_op(uint32_t compare);
VkStencilOp gpu_vsh_vulkan_stencil_op(uint32_t operation);
VkBlendOp gpu_vsh_vulkan_blend_op(uint32_t equation);

#endif
