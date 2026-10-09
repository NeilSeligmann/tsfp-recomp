/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_LIVE_VK_TEXTURE_H
#define TSFP_GPU_LIVE_VK_TEXTURE_H
/* T792, Vulkan half: the decoded RGBA8 textures of live_texture.{c,h} as VkImages on the device of gpu_window (T790).
 * One image slot per live_texture cache entry (64), staged upload only when the plan says `needs_upload` (full overwrite,
 * synchronous: its own one time command buffer, queue idle, so it never touches the per frame command buffer), samplers
 * cached per decoded NV2A address/filter state, one combined image sampler descriptor set per slot, destruction when the cache
 * entry is no longer in use. Render target textures (T793) are sampled in place: the owner registers the target's view and
 * the layout it is in when sampled, nothing is copied.
 * Descriptor layout: set N binding 0, COMBINED_IMAGE_SAMPLER, fragment stage (`live_vk_texture_set_layout`). A descriptor set is
 * rewritten only after the queue went idle, so a set is never changed under a submitted command buffer, but a draw recorded
 * EARLIER in the same frame sees an upload made later in that frame, and two stages sharing one cache entry with different
 * samplers share one descriptor (the later sampler wins): known limits, a per (entry, sampler) set is the fix if a title needs it. */
#include <stdbool.h>
#include <stdint.h>
#include "gpu_window.h"
#include "live_texture.h"

#define LIVE_VK_TEXTURE_SAMPLERS 256u

typedef struct live_vk_texture_set live_vk_texture_set;

/* The sampler state one texture stage asks for. Decoded from the stage's address word (0x1B08 + 0x40 stage) and filter word
 * (0x1B14 + 0x40 stage). Only 0x00000101 (U and V wrap), 0x00000303 (clamp to edge) with filter 0x02062000 are MEASURED
 * (gpu_pgraph_replay resolve_texture); every other decodable word follows the xemu register maps and is INFERRED. The legacy decoder selects
 * level0 only; the context-aware decoder uses active levels and Control0 clamps. Mip chains remain
 * INFERRED/opt-in, as does 0x02022000. */
typedef struct {
    bool ok, measured;
    VkSamplerAddressMode address_u, address_v;
    VkFilter min_filter, mag_filter;
    VkSamplerMipmapMode mipmap_mode;
    float min_lod, max_lod, lod_bias;
    uint32_t max_anisotropy; /* 0/1 legacy isotropic, otherwise xemu Control0 field 2/4/8. */
    const char *refusal; /* stable string when !ok */
} live_vk_sampler_desc;

live_vk_sampler_desc live_vk_sampler_decode(uint32_t address_word, uint32_t filter_word);
/* Apply Control0 sampling state (including single-level images). Unknown effects refuse. */
bool live_vk_sampler_apply_control(live_vk_sampler_desc *desc, uint32_t control_word);
/* Multi-level state follows xemu, remains INFERRED. levels is the active image count. */
live_vk_sampler_desc live_vk_sampler_decode_mips(uint32_t address_word, uint32_t filter_word,
                                                uint32_t control_word, uint32_t levels);

/* What a draw binds for one texture stage. */
typedef struct {
    VkDescriptorSet descriptor;
    VkImage image;
    VkImageView view;
    VkSampler sampler;
    VkImageLayout layout; /* the layout `view` is sampled in (T791 writes its own descriptor from view, sampler and this) */
    uint32_t width, height;
    bool uploaded; /* this call uploaded the image */
} live_vk_texture_bound;

typedef struct {
    uint64_t batch_begins, batch_completions, retired_images_peak, descriptor_versions_peak;
    uint64_t version_refusals, completion_failures;
    uint64_t uploads, upload_bytes, image_creates, image_destroys, sampler_creates, descriptor_writes, target_binds;
} live_vk_texture_stats;

/* `get_instance_proc_addr` is only used for vkGetPhysicalDeviceMemoryProperties (the native view has no instance entry). */
live_vk_texture_set *live_vk_texture_create(const gpu_window_native *native, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                                            const char **error);
void live_vk_texture_destroy(live_vk_texture_set *set);
VkDescriptorSetLayout live_vk_texture_set_layout(const live_vk_texture_set *set);
live_vk_texture_stats live_vk_texture_get_stats(const live_vk_texture_set *set);

/* Protect every returned image/descriptor until submitted work completes. End may
 * ONLY run after all referenced command buffers are submitted and completed (or
 * discarded permanently). QueueWaitIdle alone cannot prove an open buffer was
 * submitted. Failure retains active versions; nested begin/idle end refuses. A
 * queue-idle or descriptor-pool reset failure refuses binds until end succeeds
 * on retry; partial pool reset also invalidates the batch descriptor map. Outside
 * a batch the existing sequential bind contract remains in effect. */
bool live_vk_texture_begin_batch(live_vk_texture_set *set);
bool live_vk_texture_end_batch(live_vk_texture_set *set);
bool live_vk_texture_batch_active(const live_vk_texture_set *set);

/* Resolve a `live_texture_lookup` result (GUEST or TARGET) to a bound descriptor, uploading first when needed. False with
 * *error set (a static string) for a refused result, an unregistered target or a Vulkan failure. */
bool live_vk_texture_bind(live_vk_texture_set *set, live_texture_cache *cache, const live_texture_result *result,
                          const live_vk_sampler_desc *sampler, live_vk_texture_bound *bound, const char **error);
/* Destroy the image of every slot whose cache entry is no longer in use (eviction, unreadable bytes). Returns the count. */
size_t live_vk_texture_trim(live_vk_texture_set *set, const live_texture_cache *cache);

/* T793's render target: `view` is sampled in place in `layout` (normally SHADER_READ_ONLY_OPTIMAL). The id is the one given to
 * live_texture_register_target. The owner keeps the view alive until live_vk_texture_clear_targets. */
bool live_vk_texture_register_target(live_vk_texture_set *set, uint32_t id, VkImageView view, VkImageLayout layout);
void live_vk_texture_clear_targets(live_vk_texture_set *set);
#endif
