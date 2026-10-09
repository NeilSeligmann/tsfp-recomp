/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T793, the Vulkan half of the live render targets: one VkImage per registry target (live_target.h), the CopyRects blit
 * executed on the device, and the front buffer composed with the movie overlay and handed to the swapchain at the modelled
 * vblank. It takes the device through a small struct (gpu_window_native or any VkDevice), loads every entry point through
 * vkGetDeviceProcAddr and links nothing, so it builds and is tested without SDL.
 *
 * TARGETS. live_vk_target_register decodes the header words with live_target_register (same refusals, same generation
 * counter) and creates the image: pitch / bytes per pixel texels wide (a row may be as wide as the pitch, HQ58's clamp),
 * height rows, format B8G8R8A8_UNORM (A8R8G8B8 and X8R8G8B8, byte identical to the guest layout), R5G6B5_UNORM_PACK16 or R8_UNORM.
 * Every image lives in VK_IMAGE_LAYOUT_GENERAL, zero filled at creation (the CPU image of the registry starts zeroed too), so
 * a blit, a sampler read (T792) and a colour attachment write (T791) need no layout tracking. A linear A8R8G8B8 target is also
 * registered with the texture cache (live_texture_register_target), id = slot + 1, so the T792 sampler sees the same target
 * the blit writes. The generation is the registry's: a blit, live_vk_target_note_written (a draw) and live_vk_target_upload bump it.
 *
 * BLITS. live_vk_target_blit plans with live_target_plan_blit (HQ58 xemu level rules, every refusal counted by reason) and
 * then runs the plan: a plain vkCmdCopyImage when plan.copy_image_ok (distinct or disjoint, A8R8G8B8, no alpha patch), else a
 * STAGED copy: both images are read back to host buffers, the reference executor live_target_execute_plan runs over them (rows
 * ascending, one buffered row, the alpha patch) and the destination goes back. The staged path is the semantic definition
 * (a 3 row overlap smears, format 7 forces alpha 0xFF) and is byte-identical to the CPU reference by construction, the
 * copy_image path is what the tests prove equal to it. The staged path moves whole images, it is for the rare shapes.
 *
 * PRESENT. live_vk_target_present_submit queues the Swap's front buffer (d3d8_frame_record, the d3d8_present observer's
 * argument) in a live_present_schedule, live_vk_target_vblank runs one modelled vblank: live_present_vblank picks the layers
 * [front, overlay], the front is read back (cached by target generation), converted to RGBA with alpha forced opaque (the
 * scanout ignores alpha, INFERRED), the overlay picture (live_vk_target_overlay_submit, the T760 sink's RGB24) is letterboxed
 * over it with nearest sampling (the largest rectangle of the picture's aspect, the front shows in the bars, INFERRED, the
 * hardware overlay rectangle is not modelled) and the result goes to the present callback (gpu_window_present_pixels). With no
 * layer the callback gets a black frame. The readback + upload is the reference route. T849 adds the direct route
 * (live_vk_target_set_direct, gpu_window_present_blit): the front image and an overlay texture are blitted straight into the
 * acquired swapchain image, opt-in, the readback route stays as reference and fallback.
 *
 * Nothing here is installed by default. Every device failure is counted, never swallowed.
 */
#ifndef TSFP_GPU_LIVE_VK_TARGET_H
#define TSFP_GPU_LIVE_VK_TARGET_H

#include "d3d8_present.h"
#include "gpu_pgraph.h"
#include "gpu_window.h"
#include "live_target.h"
#include "live_texture.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool command_pool;
    VkPhysicalDeviceMemoryProperties memory;
    PFN_vkGetDeviceProcAddr get_device_proc_addr;
} live_vk_target_device;

/** Fill `out` from the window's native seam. `get_instance_proc_addr` (SDL_Vulkan_GetVkGetInstanceProcAddr) is only used to
 * read the physical device's memory properties. False when an argument is missing. */
bool live_vk_target_device_from_native(const gpu_window_native *native, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                                       live_vk_target_device *out);

typedef enum {
    LIVE_VK_OK = 0,
    LIVE_VK_ARGUMENT,
    LIVE_VK_PLAN_REFUSED,   /* live_target_refusal says why, counted in the registry census */
    LIVE_VK_NO_IMAGE,       /* the target is registered but has no image (an earlier device failure) */
    LIVE_VK_DEVICE_FAILED,  /* a Vulkan call failed, counted in device_failures */
    LIVE_VK_UNKNOWN_TARGET
} live_vk_status;

typedef struct {
    uint64_t images_created;
    uint64_t copy_image_blits;
    uint64_t staged_blits;
    uint64_t empty_blits;
    uint64_t refused_blits;
    uint64_t device_failures;
    uint64_t texture_registrations;
    uint64_t texture_registration_failures;
    uint64_t presents_submitted;
    uint64_t present_refused;      /* the header words did not decode */
    uint64_t vblanks;
    uint64_t frames_presented;     /* present callback returned true */
    uint64_t present_callback_failures;
    uint64_t front_readbacks;
    uint64_t front_cache_hits;
    uint64_t front_unreadable;     /* front target without an image or with a format the scanout cannot show */
    uint64_t overlay_submitted;
    uint64_t overlay_composed;
    uint64_t black_frames;
    /* T849 */
    uint64_t blit_frames;          /* presented by the direct (swapchain pre-pass blit) route */
    uint64_t readback_frames;      /* presented by the readback route */
    uint64_t direct_fallbacks;     /* a front the blit cannot read (not 4 byte linear): that vblank took the readback route */
    uint64_t direct_failures;      /* the direct present failed or its hook refused */
    uint64_t overlay_uploads;      /* movie pictures uploaded to the overlay texture */
    uint64_t vblank_ns;            /* wall time spent inside live_vk_target_vblank (monotonic clock), summed */
    uint64_t vblank_ns_max;
    uint64_t first_frame_ns;       /* monotonic clock when the first / the latest frame finished presenting */
    uint64_t last_frame_ns;
    uint64_t media_snapshots;
    uint64_t media_snapshot_drops;
    uint64_t media_frames_released;
    uint64_t media_direct_fallbacks;
} live_vk_target_stats;

typedef struct live_vk_target_set live_vk_target_set;

/** `cache` (may be NULL) receives linear A8R8G8B8 targets for the T792 sampler. `allowed` is LIVE_TARGET_INFER_*. */
live_vk_target_set *live_vk_target_create(const live_vk_target_device *device, uint32_t allowed, live_texture_cache *cache);
void live_vk_target_destroy(live_vk_target_set *set);
live_target_registry *live_vk_target_registry(live_vk_target_set *set);
live_vk_target_stats live_vk_target_stats_get(const live_vk_target_set *set);

/** Register or confirm a target and create its image. `refusal` (may be NULL) gets the registry refusal. */
live_vk_status live_vk_target_register(live_vk_target_set *set, uint32_t data, uint32_t format_word, uint32_t size_word,
                                       live_target_refusal *refusal);
bool live_vk_target_has_image(const live_vk_target_set *set, uint32_t data);
/** The image, its format, extent (texels, pitch / bytes per pixel wide) and the registry generation. VK_NULL_HANDLE when unknown. */
VkImage live_vk_target_image(const live_vk_target_set *set, uint32_t data, VkFormat *format, VkExtent2D *extent,
                             uint64_t *generation);
/** The image view of the target a `live_texture_register_target` id names (id = slot + 1, what `live_texture_result.target_id`
 * carries), created on first use and destroyed with the target. The image stays in layout GENERAL, so a sampler reads it in
 * VK_IMAGE_LAYOUT_GENERAL. VK_NULL_HANDLE for an unknown id or a target without an image. T791 registers it with the texture set. */
VkImageView live_vk_target_texture_view(live_vk_target_set *set, uint32_t texture_id);
/** The id `live_texture_register_target` and `live_vk_target_texture_view` use for the target (slot + 1), 0 when unknown or without an image. */
uint32_t live_vk_target_texture_id(const live_vk_target_set *set, uint32_t data);
/** A draw or an external write changed the target (bumps the shared generation). */
void live_vk_target_note_written(live_vk_target_set *set, uint32_t data);
/** Write the whole image from host bytes laid out with the target's pitch (pitch * height bytes). Bumps the generation. */
live_vk_status live_vk_target_upload(live_vk_target_set *set, uint32_t data, const uint8_t *bytes, size_t length);
/** Read the whole image back, pitch * height bytes. */
live_vk_status live_vk_target_readback(live_vk_target_set *set, uint32_t data, uint8_t *bytes, size_t length);

/** The CopyRects hook-up: a gpu_pgraph copy event as a planner blit. */
live_target_blit live_vk_target_blit_from_copy(const gpu_pgraph_copy *copy);
/** Plan and execute one blit. `plan` (may be NULL) gets the plan, `refusal` (may be NULL) the planner's reason. */
live_vk_status live_vk_target_blit(live_vk_target_set *set, const live_target_blit *blit, live_target_blit_plan *plan,
                                   live_target_refusal *refusal);
/** Every copy event of the stream, in order, stopping at the first refusal. Returns the events applied (empty ones count);
 * `first_failure` (may be NULL) gets the index of the refused one, or `count` when none. */
size_t live_vk_target_apply_copies(live_vk_target_set *set, const gpu_pgraph_copy *copies, size_t count, size_t *first_failure,
                                   live_target_refusal *refusal);

/* --- present --- */

/** Hands one RGBA8 frame to the swapchain (gpu_window_present_pixels). Returns false on failure. */
typedef bool (*live_vk_target_present_fn)(void *context, const uint8_t *rgba, uint32_t width, uint32_t height,
                                          uint32_t stride_bytes);
void live_vk_target_set_present(live_vk_target_set *set, live_vk_target_present_fn present, void *context);

/** Queue a Swap's front buffer (registers and creates its image). */
live_target_refusal live_vk_target_present_submit(live_vk_target_set *set, const d3d8_frame_record *record);
/** Playback-mode submit: capture immutable front pixels and release only once media_time_ns is reached by the audio clock. */
live_target_refusal live_vk_target_present_submit_at(live_vk_target_set *set, const d3d8_frame_record *record,
                                                     uint64_t media_time_ns);
/** The d3d8_present observer: submits to the set bound with live_vk_target_bind_observer (NULL unbinds). */
void live_vk_target_present_observer(const d3d8_frame_record *record);
void live_vk_target_bind_observer(live_vk_target_set *set);

/** T849: the direct route. `direct` presents one frame whose pixels a gpu_window_blit_hook records with transfer commands
 * (gpu_window_present_blit is that function: pass the window as `context`). With it set, a vblank whose front buffer is a 4 byte
 * linear target takes this route (front and overlay texture blitted straight into the acquired swapchain image, no readback, no
 * CPU compose); anything else, and every vblank with `direct` NULL, takes the readback route above, which stays the reference. */
typedef bool (*live_vk_target_direct_fn)(void *context, gpu_window_blit_hook hook, void *hook_context);
void live_vk_target_set_direct(live_vk_target_set *set, live_vk_target_direct_fn direct, void *context);
/** Record the layers of `frame` into `destination` (an image in VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL with `extent`) with
 * vkCmdBlitImage (nearest) into `command`: the front scaled over the whole destination, then the overlay texture over its
 * letterbox rectangle (the compositor's, scaled), black when there is no front. Uploads the overlay picture first when it is new
 * (its own synchronous submission). False when a layer cannot be blitted. Identical to the readback route (RGB, alpha not shown)
 * when destination and the front have the same size and the overlay scale to the canvas is an integer or 1; other scales differ
 * by Vulkan's texel centre sampling. */
bool live_vk_target_record_frame(live_vk_target_set *set, VkCommandBuffer command, VkImage destination, VkExtent2D extent,
                                 const live_present_frame *frame);
/** The readback route's composition of the last vblank's layers (current target contents), for read_pixel and capture when the
 * direct route kept no CPU copy. `*rgba` points at an internal buffer valid until the next call. */
bool live_vk_target_compose_last(live_vk_target_set *set, const uint8_t **rgba, uint32_t *width, uint32_t *height);

/** Latch one movie overlay picture (RGB24, copied), the T760 sink's picture. */
void live_vk_target_overlay_submit(live_vk_target_set *set, uint32_t width, uint32_t height, const uint8_t *rgb);
void live_vk_target_overlay_clear(live_vk_target_set *set);

/** One modelled vblank. Returns true when a frame was handed to the present callback. `frame` (may be NULL) gets the layers. */
bool live_vk_target_vblank(live_vk_target_set *set, uint64_t vblank, live_present_frame *frame);
/** Playback-mode vblank: front images are snapshotted at submit and gated by the audio media clock. */
bool live_vk_target_media_vblank(live_vk_target_set *set, uint64_t vblank, bool media_clock_valid,
                                 uint64_t media_time_ns, live_present_frame *frame);
live_present_schedule live_vk_target_schedule(const live_vk_target_set *set);

/** Pure compositor: `front_bgra` (stride bytes per row, may be NULL) with alpha forced to 0xFF, then `overlay_rgb` (tight RGB24,
 * may be NULL) letterboxed above it. `out_rgba` is out_width * out_height * 4 bytes. With no front the canvas is black. */
void live_vk_target_compose(const uint8_t *front_bgra, uint32_t front_stride, const uint8_t *overlay_rgb, uint32_t overlay_width,
                            uint32_t overlay_height, uint32_t out_width, uint32_t out_height, uint8_t *out_rgba);

#endif /* TSFP_GPU_LIVE_VK_TARGET_H */
