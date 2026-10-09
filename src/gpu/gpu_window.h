/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_WINDOW_H
#define TSFP_GPU_WINDOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

typedef struct SDL_Window SDL_Window;
typedef struct gpu_window gpu_window;

typedef struct {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool command_pool;
    VkRenderPass render_pass;
    VkCommandBuffer command_buffer; /* valid only while the per-frame pass is recording */
    PFN_vkGetDeviceProcAddr get_device_proc_addr;
    VkExtent2D extent;
    VkFormat format;
    VkPhysicalDeviceMemoryProperties memory_properties; /* T791: for the draw arena, texture and target images */
    bool negative_viewport; /* T828: VK_KHR_maintenance1 is enabled, VkViewport.height may be negative */
    bool occlusion_query_precise; /* T998: precise sample counts supported and enabled. */
    bool sampler_anisotropy; /* T1051: samplerAnisotropy enabled on this logical device. */
    bool fill_mode_non_solid; /* T860: fillModeNonSolid is enabled (LINE and POINT polygon modes) */
    uint32_t swapchain_image_count;
    const VkImage *swapchain_images;
    bool large_points; /* T1039: largePoints actually supported and enabled. */
} gpu_window_native;

typedef struct {
    uint64_t frames;
    uint64_t presented;
    uint64_t acquire_failures;
    uint64_t swapchain_recreations; /* T1339: out of date or suboptimal swapchains replaced */
    uint64_t present_failures;
    uint64_t draw_packets;
    uint64_t draw_methods;
    uint64_t draw_refusals;
    uint64_t blit_presented; /* T849: frames presented through gpu_window_present_blit */
    uint64_t blit_refusals;  /* the hook refused, a black frame was presented */
} gpu_window_stats;

typedef bool (*gpu_window_frame_hook)(const gpu_window_native *native, void *context);
/* Runs only after a submitted swapchain command buffer completes (queue idle). */
typedef bool (*gpu_window_complete_hook)(void *context);
void gpu_window_set_complete_hook(gpu_window *renderer, gpu_window_complete_hook hook, void *context);

/* Create the Vulkan instance, device and swapchain for an SDL_WINDOW_VULKAN window. */
gpu_window *gpu_window_create(SDL_Window *window, const char **error);

/* Clear and present one swapchain image. Draw methods observed since the previous frame are
 * counted and consumed in that same command-buffer frame; T791 attaches pipeline work here. */
bool gpu_window_present(gpu_window *renderer, const float clear_rgba[4]);
/* Present an RGBA8 image produced by the live gpu_device replay into the current swapchain frame.
 * Pixels are copied/scaled into the acquired image before it is presented. */
bool gpu_window_present_pixels(gpu_window *renderer, const uint8_t *rgba, uint32_t width,
                               uint32_t height, uint32_t stride_bytes, const float clear_rgba[4]);

/* T849: the swapchain pre-pass blit seam. Called outside any render pass with the acquired image in
 * VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL (contents undefined): record vkCmdBlitImage / vkCmdCopyImage / clears into `command`
 * (native->command_buffer is the same buffer), the image goes to PRESENT_SRC afterwards. Return false to refuse. */
typedef bool (*gpu_window_blit_hook)(const gpu_window_native *native, VkCommandBuffer command, VkImage destination,
                                     VkExtent2D extent, VkFormat format, void *context);
/* True when the swapchain images accept transfer writes (the blit seam is usable). */
bool gpu_window_can_blit(const gpu_window *renderer);
/* Present one frame whose pixels the hook produced with transfer commands. False when the hook refused (black frame shown). */
bool gpu_window_present_blit(gpu_window *renderer, gpu_window_blit_hook hook, void *context);
/* Evidence and tests: copy the final image of every later present (either route) into a host buffer. False when the swapchain
 * has no TRANSFER_SRC usage. gpu_window_read_capture converts the last presented frame to RGBA8 (extent x extent * 4 bytes). */
bool gpu_window_set_capture(gpu_window *renderer, bool enable);
/* T1267: the swapchain extent (the size gpu_window_read_capture returns), 0x0 for a NULL window. */
void gpu_window_extent(const gpu_window *renderer, uint32_t *width, uint32_t *height);
bool gpu_window_read_capture(const gpu_window *renderer, uint8_t *rgba, size_t capacity, uint32_t *width, uint32_t *height);

/* Feed one decoded live method/data pair. This is an in-memory stream feed; no recording file. */
void gpu_window_feed_method(gpu_window *renderer, uint32_t method, uint32_t data);
/* Device and in-progress command-buffer seam for the live draw pipeline and texture uploader. */
bool gpu_window_get_native(const gpu_window *renderer, gpu_window_native *out_native);
/* Called inside each clear render pass before presentation. A draw backend records its work
 * into the provided command buffer. Return false to refuse this frame. */
void gpu_window_set_frame_hook(gpu_window *renderer, gpu_window_frame_hook hook, void *context);

/* T1262: true restores vkQueueWaitIdle after every present (the pre T1262 behaviour, --live-present-sync). Default false. */
void gpu_window_set_present_sync(bool enable);
gpu_window_stats gpu_window_get_stats(const gpu_window *renderer);
const char *gpu_window_device_name(const gpu_window *renderer);
void gpu_window_destroy(gpu_window *renderer);

#endif
