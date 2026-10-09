/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_DEVICE_NATIVE_H
#define TSFP_GPU_DEVICE_NATIVE_H

/* The Vulkan handles behind a gpu_device, for the renderer's own modules that build their
 * pipelines on the same device (gpu_vsh_draw.c, T100d). Not for callers outside src/gpu:
 * gpu_device.h deliberately keeps vulkan.h away from them.
 *
 * Every includer must define VK_NO_PROTOTYPES before this header, so a stray direct call
 * to a Vulkan function fails to link instead of binding to a loader we do not link. */
#ifndef VK_NO_PROTOTYPES
#error "define VK_NO_PROTOTYPES before including gpu_device_native.h"
#endif

#include "gpu_device.h"

#include <vulkan/vulkan_core.h>

typedef struct {
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    PFN_vkGetDeviceProcAddr get_device_proc_addr;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool command_pool;
    VkPhysicalDeviceMemoryProperties memory_properties;
    bool transform_feedback;
    bool negative_viewport; /* VK_KHR_maintenance1 enabled: VkViewport.height may be negative (T828) */
    bool occlusion_query_precise; /* T998: precise sample counts supported and enabled. */
    bool sampler_anisotropy; /* T1051: samplerAnisotropy enabled on this logical device. */
    bool fill_mode_non_solid; /* T860: the fillModeNonSolid feature is enabled, a LINE or POINT polygon mode can be drawn */
    bool large_points; /* T1039: largePoints actually supported and enabled. */
} gpu_device_native;

void gpu_device_get_native(const gpu_device *device, gpu_device_native *out_native);

#endif
