/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Headless Vulkan device for offscreen rendering.
 *
 * This is the HOST half of the graphics boundary and it is REAL, not scaffold: it
 * initialises a Vulkan instance and device, renders into an offscreen colour
 * target, and copies the result back to host memory where it can be written to a
 * PNG and looked at. Nothing here knows anything about Direct3D 8 or about the
 * NV2A -- translating the guest's state into draw calls is a separate problem and
 * lives behind d3d8_hle.h, which currently translates nothing.
 *
 * WHY OFFSCREEN ONLY, AND WHY THAT IS THE RIGHT CHOICE: the machine this was
 * built on has no GPU and no display, so every frame is produced by lavapipe (the
 * Mesa software rasteriser) into a buffer we read back. That is not a compromise
 * for the task that matters most right now, which is proving a frame is correct.
 * A frame you can read back is a frame you can assert on and a frame a human can
 * open, and both of those are worth more during a decompilation than a window.
 *
 * WHY THE LOADER IS dlopen'd RATHER THAN LINKED: the project must configure and
 * build on a machine with no Vulkan at all. Linking -lvulkan would make a missing
 * loader a link error, which turns an absent optional feature into a broken
 * clone. Resolving the loader at runtime instead means the only build-time
 * dependency is the Vulkan headers, and a machine with no driver gets a clean
 * GPU_ERR_NO_LOADER that callers and tests can skip on.
 *
 * Every entry point reports failure as a gpu_result rather than aborting or
 * logging-and-continuing, because a silent graphics failure produces a black
 * frame, and a black frame is indistinguishable from a frame that legitimately
 * drew nothing.
 */

#ifndef TSFP_GPU_DEVICE_H
#define TSFP_GPU_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Opaque: the Vulkan handles and the resolved function pointer table are an
 * implementation detail, and exposing them would drag vulkan.h into every
 * translation unit that merely wants to ask for a frame. */
typedef struct gpu_device gpu_device;

typedef enum {
    GPU_OK = 0,
    /* No Vulkan loader on this machine. Expected, and a reason to skip, not fail. */
    GPU_ERR_NO_LOADER,
    /* Loader present but it could not create an instance. */
    GPU_ERR_NO_INSTANCE,
    /* Instance created but zero physical devices. No ICD is installed. */
    GPU_ERR_NO_PHYSICAL_DEVICE,
    /* A physical device exists but has no queue family that can do graphics. */
    GPU_ERR_NO_GRAPHICS_QUEUE,
    GPU_ERR_NO_DEVICE,
    /* No memory type satisfying a required property set. */
    GPU_ERR_NO_MEMORY_TYPE,
    /* A Vulkan call returned something other than VK_SUCCESS. */
    GPU_ERR_VULKAN,
    GPU_ERR_OUT_OF_MEMORY,
    GPU_ERR_ARGUMENT,
} gpu_result;

/** A human-readable name for a result. Never NULL. */
const char *gpu_result_string(gpu_result result);

/**
 * True when gpu_device_create has any chance of succeeding.
 *
 * Separated out so a test can decide to skip before it has to interpret an error
 * code, which keeps "no Vulkan here" from reading like "Vulkan is broken here".
 */
bool gpu_vulkan_available(void);

/**
 * Tightly packed 8-bit RGBA pixels read back from the device.
 *
 * `stride_bytes` is carried explicitly even though the current readback path
 * always produces width*4. The row pitch of a mapped Vulkan allocation is the
 * driver's choice in general, so a consumer that assumes width*4 is a bug
 * waiting for the first driver that disagrees.
 */
typedef struct {
    uint8_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
} gpu_image;

/** Release pixels owned by a gpu_image and zero it. Safe on an already-freed image. */
void gpu_image_free(gpu_image *image);

/** Byte offset of a pixel, for callers asserting on specific coordinates. */
size_t gpu_image_offset(const gpu_image *image, uint32_t x, uint32_t y);

/**
 * Bring up an instance, pick a physical device and open it.
 *
 * On success `*out_device` owns everything and must be passed to
 * gpu_device_destroy. On failure `*out_device` is set to NULL, so a caller that
 * ignores the result still gets a NULL dereference rather than a half-built
 * device that appears to work.
 */
gpu_result gpu_device_create(gpu_device **out_device);

/**
 * gpu_device_create with a choice of physical device (T100d). `selector` is NULL or empty
 * for the first device with a graphics queue (what gpu_device_create does), "hardware"
 * for the first one that is not a software rasteriser, "software" for llvmpipe or
 * lavapipe, or a substring of the device name (for example "RADV"). A selector that
 * matches nothing returns GPU_ERR_NO_PHYSICAL_DEVICE.
 */
gpu_result gpu_device_create_selected(const char *selector, gpu_device **out_device);

/** True when VK_EXT_transform_feedback was enabled on this device (gpu_vsh_capture needs it). */
bool gpu_device_has_transform_feedback(const gpu_device *device);

void gpu_device_destroy(gpu_device *device);

/** The physical device's reported name, e.g. "llvmpipe". Never NULL for a live device. */
const char *gpu_device_name(const gpu_device *device);

/** The physical device's VK_API_VERSION, for diagnostics. */
uint32_t gpu_device_api_version(const gpu_device *device);

/**
 * Clear an offscreen target to `clear_rgba` and read it back.
 *
 * The clear is done as a render pass load operation rather than
 * vkCmdClearColorImage, so this exercises the same render pass, framebuffer and
 * submission path the triangle does. A clear that worked through a different code
 * path would prove less than it appears to.
 *
 * `clear_rgba` is four floats in 0..1 and must not be NULL. The target format is
 * VK_FORMAT_R8G8B8A8_UNORM, chosen over the _SRGB variant so that a component
 * reads back as round(value * 255) and a test can assert an exact byte. An sRGB
 * target would make every expected value a tolerance question.
 */
gpu_result gpu_render_clear(gpu_device *device, uint32_t width, uint32_t height,
                            const float clear_rgba[4], gpu_image *out_image);

/**
 * Clear, then draw one triangle with the embedded SPIR-V from gpu_shaders.h.
 *
 * This is the step that proves a pipeline, not just a device: shader modules,
 * a graphics pipeline, rasterisation, and per-vertex attribute interpolation
 * reaching the fragment stage. The triangle's apex is at the TOP of the returned
 * image and its three corners are pure red (apex), green (bottom right) and blue
 * (bottom left), which makes an upside-down or mirrored result obvious to a human
 * instead of merely non-background to an assertion.
 */
gpu_result gpu_render_triangle(gpu_device *device, uint32_t width, uint32_t height,
                               const float clear_rgba[4], gpu_image *out_image);

#endif /* TSFP_GPU_DEVICE_H */
