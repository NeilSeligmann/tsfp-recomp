/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gpu_phase_timing.h"
#include "live_vk_texture.h"
#include <stdlib.h>
#include <string.h>

#define DEVICE_FUNCTIONS(X) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateBuffer) X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCreateSampler) \
    X(vkDestroySampler) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool) \
    X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkAllocateCommandBuffers) \
    X(vkFreeCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) \
    X(vkCmdCopyBufferToImage) X(vkQueueSubmit) X(vkQueueWaitIdle)

#define GUEST_SLOTS LIVE_TEXTURE_CACHE_ENTRIES
#define TARGET_SLOTS LIVE_TEXTURE_TARGETS
#define SET_COUNT (GUEST_SLOTS + TARGET_SLOTS)
#define BATCH_PAGE_SETS 128u
#define BATCH_VERSION_LIMIT 65536u

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    uint32_t width, height, levels;
    bool cube; /* T1490: six layers, a CUBE view */
    bool held;
} image_slot;

typedef struct {
    VkImageView view;
    VkSampler sampler;
    VkImageLayout layout;
} descriptor_state;

typedef struct {
    VkDescriptorPool pool;
    VkDescriptorSet sets[BATCH_PAGE_SETS];
    bool allocated;
} descriptor_page;

typedef struct {
    descriptor_state key;
    VkDescriptorSet descriptor;
} batch_descriptor;

typedef struct {
    bool used;
    uint32_t id;
    VkImageView view;
    VkImageLayout layout;
} target_slot;

typedef struct {
    live_vk_sampler_desc key;
    VkSampler sampler;
} sampler_slot;

struct live_vk_texture_set {
    gpu_window_native native;
    VkPhysicalDeviceMemoryProperties memory_properties;
    float max_lod_bias;
    float max_sampler_anisotropy;
#define X(name) PFN_##name name;
    DEVICE_FUNCTIONS(X)
#undef X
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool pool;
    VkDescriptorSet sets[SET_COUNT];
    descriptor_state written[SET_COUNT];
    image_slot images[GUEST_SLOTS];
    bool batch_active;
    bool completion_retry_required;
    image_slot *retired;
    size_t retired_count, retired_capacity;
    descriptor_page *pages;
    size_t page_count, page_capacity;
    batch_descriptor *batch_descriptors;
    size_t descriptor_count, descriptor_capacity;
    target_slot targets[TARGET_SLOTS];
    sampler_slot samplers[LIVE_VK_TEXTURE_SAMPLERS];
    size_t sampler_count;
    VkBuffer staging;
    VkDeviceMemory staging_memory;
    void *staging_map;
    VkDeviceSize staging_size;
    live_vk_texture_stats stats;
};

/* --- sampler decode (the xemu register maps) ----------------------------------------------------------------------- */
static bool decode_address_mode(uint32_t mode, VkSamplerAddressMode *out)
{
    switch (mode) {
    case 1u: *out = VK_SAMPLER_ADDRESS_MODE_REPEAT; return true;
    case 2u: *out = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; return true;
    case 3u:
    case 5u: *out = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; return true;
    case 4u: *out = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; return true;
    default: return false;
    }
}

live_vk_sampler_desc live_vk_sampler_decode(uint32_t address_word, uint32_t filter_word)
{
    live_vk_sampler_desc desc = {0};
    if (address_word == 0u) {
        desc.refusal = "unbound address word";
        return desc;
    }
    if ((address_word & ~0x00000F0Fu) != 0u || !decode_address_mode(address_word & 0xFu, &desc.address_u) ||
        !decode_address_mode((address_word >> 8) & 0xFu, &desc.address_v)) {
        desc.refusal = "address mode";
        return desc;
    }
    const uint32_t min = (filter_word >> 16) & 0xFFu;
    const uint32_t mag = (filter_word >> 24) & 0xFu;
    if ((filter_word & 0x1FFFu) != 0u || (filter_word & 0xF0000000u) != 0u || min < 1u || min > 7u) {
        desc.refusal = "min filter";
        return desc;
    }
    if (mag != 1u && mag != 2u && mag != 4u) {
        desc.refusal = "mag filter";
        return desc;
    }
    /* xemu MIN_TENT_LOD0=2 is base-level linear; MIN_TENT_TENT_LOD=6 also interpolates mip levels.
     * Images admitted by live_texture have exactly one level. maxLod=0 below makes both linear at
     * that level, matching xemu's one-level Vulkan sampler. The new retail 0x02022000 stays INFERRED.
     * min 1,3,5 nearest, 2,4,6,7 linear (level 0 only: the mip part has nothing to select). */
    desc.min_filter = (min == 1u || min == 3u || min == 5u) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    desc.mag_filter = mag == 1u ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    desc.measured = (address_word == 0x00000101u || address_word == 0x00000303u) && filter_word == 0x02062000u;
    desc.ok = true;
    return desc;
}

bool live_vk_sampler_apply_control(live_vk_sampler_desc *desc, uint32_t control_word)
{
    if (!desc->ok) return false;
    /* xemu direct SET_TEXTURE_CONTROL0: enable, integer LOD clamps and MAX_ANISOTROPY.
     * Alpha kill bit2, colour key bits0..1 and all other effects remain refused. */
    if ((control_word & ~0x7FFFFFF0u) != 0u) {
        desc->ok = false;
        desc->refusal = "unsupported texture Control0 effect";
        return false;
    }
    desc->max_anisotropy = 1u << ((control_word >> 4) & 3u);
    if (desc->max_anisotropy > 1u) desc->measured = false;
    return true;
}

live_vk_sampler_desc live_vk_sampler_decode_mips(uint32_t address_word, uint32_t filter_word,
                                                uint32_t control_word, uint32_t levels)
{
    live_vk_sampler_desc desc = live_vk_sampler_decode(address_word, filter_word & ~0x1FFFu);
    if (!desc.ok) return desc;
    if (levels == 0u || levels > LIVE_TEXTURE_MAX_LEVELS) {
        desc.ok = false;
        desc.refusal = "mip count";
        return desc;
    }
    if (!live_vk_sampler_apply_control(&desc, control_word)) return desc;
    desc.measured = false;
    const uint32_t min = (filter_word >> 16) & 0xFFu;
    const bool enabled = levels > 1u && min != 1u && min != 2u && min != 7u;
    desc.mipmap_mode = enabled && (min == 5u || min == 6u) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    uint32_t low = (control_word & 0x3FFC0000u) >> 18;
    uint32_t high = (control_word & 0x0003FFC0u) >> 6;
    if (low >= levels) low = levels - 1u;
    if (high >= levels) high = levels - 1u;
    desc.min_lod = enabled ? (float)low : 0.0f;
    desc.max_lod = enabled ? (float)high : 0.0f;
    /* Vulkan requires minLod <= maxLod; unsupported guest ordering is explicit. */
    if (desc.min_lod > desc.max_lod) {
        desc.ok = false;
        desc.refusal = "reversed LOD clamps";
        return desc;
    }
    int32_t bias = (int32_t)(filter_word & 0x1FFFu);
    if ((bias & 0x1000) != 0) bias -= 0x2000;
    desc.lod_bias = (float)bias / 256.0f;
    return desc;
}

/* --- memory --------------------------------------------------------------------------------------------------------- */
static bool memory_type(const live_vk_texture_set *set, uint32_t bits, VkMemoryPropertyFlags wanted, uint32_t *out)
{
    for (uint32_t index = 0u; index < set->memory_properties.memoryTypeCount; index++) {
        if ((bits & (1u << index)) != 0u && (set->memory_properties.memoryTypes[index].propertyFlags & wanted) == wanted) {
            *out = index;
            return true;
        }
    }
    return false;
}

static void destroy_slot_image(live_vk_texture_set *set, image_slot *slot)
{
    if (slot->image == VK_NULL_HANDLE) {
        return;
    }
    set->vkDestroyImageView(set->native.device, slot->view, NULL);
    set->vkDestroyImage(set->native.device, slot->image, NULL);
    set->vkFreeMemory(set->native.device, slot->memory, NULL);
    *slot = (image_slot){0};
    set->stats.image_destroys++;
}

static bool create_slot_image(live_vk_texture_set *set, image_slot *slot, uint32_t width, uint32_t height, uint32_t levels, bool cube)
{
    const VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {width, height, 1u}, .mipLevels = levels, .arrayLayers = cube ? 6u : 1u, .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    image_slot fresh = {.width = width, .height = height, .levels = levels, .cube = cube};
    VkMemoryRequirements requirements;
    uint32_t type = 0u;
    if (set->vkCreateImage(set->native.device, &info, NULL, &fresh.image) != VK_SUCCESS) {
        return false;
    }
    set->vkGetImageMemoryRequirements(set->native.device, fresh.image, &requirements);
    if (!memory_type(set, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        !memory_type(set, requirements.memoryTypeBits, 0u, &type)) {
        set->vkDestroyImage(set->native.device, fresh.image, NULL);
        return false;
    }
    const VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size,
                                           .memoryTypeIndex = type};
    if (set->vkAllocateMemory(set->native.device, &allocate, NULL, &fresh.memory) != VK_SUCCESS) {
        set->vkDestroyImage(set->native.device, fresh.image, NULL);
        return false;
    }
    const VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = fresh.image, .viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, levels, 0u, cube ? 6u : 1u}};
    if (set->vkBindImageMemory(set->native.device, fresh.image, fresh.memory, 0u) != VK_SUCCESS ||
        set->vkCreateImageView(set->native.device, &view_info, NULL, &fresh.view) != VK_SUCCESS) {
        set->vkFreeMemory(set->native.device, fresh.memory, NULL);
        set->vkDestroyImage(set->native.device, fresh.image, NULL);
        return false;
    }
    *slot = fresh;
    set->stats.image_creates++;
    return true;
}

static bool ensure_staging(live_vk_texture_set *set, VkDeviceSize bytes)
{
    if (set->staging != VK_NULL_HANDLE && set->staging_size >= bytes) {
        return true;
    }
    if (set->staging != VK_NULL_HANDLE) {
        set->vkUnmapMemory(set->native.device, set->staging_memory);
        set->vkDestroyBuffer(set->native.device, set->staging, NULL);
        set->vkFreeMemory(set->native.device, set->staging_memory, NULL);
        set->staging = VK_NULL_HANDLE;
        set->staging_memory = VK_NULL_HANDLE;
        set->staging_map = NULL;
        set->staging_size = 0u;
    }
    const VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
                                     .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkMemoryRequirements requirements;
    uint32_t type = 0u;
    if (set->vkCreateBuffer(set->native.device, &info, NULL, &set->staging) != VK_SUCCESS) {
        set->staging = VK_NULL_HANDLE;
        return false;
    }
    set->vkGetBufferMemoryRequirements(set->native.device, set->staging, &requirements);
    const VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size};
    VkMemoryAllocateInfo with_type = allocate;
    if (!memory_type(set, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &type)) {
        goto fail;
    }
    with_type.memoryTypeIndex = type;
    if (set->vkAllocateMemory(set->native.device, &with_type, NULL, &set->staging_memory) != VK_SUCCESS) {
        set->staging_memory = VK_NULL_HANDLE;
        goto fail;
    }
    if (set->vkBindBufferMemory(set->native.device, set->staging, set->staging_memory, 0u) != VK_SUCCESS ||
        set->vkMapMemory(set->native.device, set->staging_memory, 0u, VK_WHOLE_SIZE, 0u, &set->staging_map) != VK_SUCCESS) {
        set->vkFreeMemory(set->native.device, set->staging_memory, NULL);
        set->staging_memory = VK_NULL_HANDLE;
        goto fail;
    }
    set->staging_size = bytes;
    return true;
fail:
    set->vkDestroyBuffer(set->native.device, set->staging, NULL);
    set->staging = VK_NULL_HANDLE;
    return false;
}

/* Copy `rgba` through the staging buffer into `slot` and leave it SHADER_READ_ONLY_OPTIMAL. Synchronous. */
static bool upload_image(live_vk_texture_set *set, const image_slot *slot, const uint8_t *rgba, const live_texture_plan *plan)
{
    const uint32_t bytes = plan->rgba_bytes;
    if (!ensure_staging(set, bytes)) {
        return false;
    }
    memcpy(set->staging_map, rgba, bytes);
    VkCommandBuffer command = VK_NULL_HANDLE;
    const VkCommandBufferAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                  .commandPool = set->native.command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                  .commandBufferCount = 1u};
    if (set->vkAllocateCommandBuffers(set->native.device, &allocate, &command) != VK_SUCCESS) {
        return false;
    }
    const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    const uint32_t faces = slot->cube ? 6u : 1u;
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, slot->levels, 0u, faces};
    VkImageMemoryBarrier to_transfer = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .image = slot->image, .subresourceRange = range};
    VkImageMemoryBarrier to_sampled = to_transfer;
    to_sampled.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_sampled.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_sampled.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_sampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkBufferImageCopy regions[LIVE_TEXTURE_MAX_LEVELS * 6u] = {0};
    for (uint32_t face = 0u; face < faces; face++) {
        for (uint32_t level = 0u; level < slot->levels; level++) {
            VkBufferImageCopy *region = &regions[face * slot->levels + level];
            region->bufferOffset = (VkDeviceSize)face * plan->face_rgba_bytes + plan->mip[level].rgba_offset;
            region->imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, level, face, 1u};
            region->imageExtent = (VkExtent3D){plan->mip[level].width, plan->mip[level].height, 1u};
        }
    }
    bool ok = set->vkBeginCommandBuffer(command, &begin) == VK_SUCCESS;
    if (ok) {
        /* T1264: draw runs are submitted without a wait, an earlier run may still sample this image: wait for them on the GPU
         * (ALL_COMMANDS source, the old TOP_OF_PIPE source waited for nothing and relied on the host idle). */
        to_transfer.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        set->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, NULL, 0u,
                                  NULL, 1u, &to_transfer);
        set->vkCmdCopyBufferToImage(command, set->staging, slot->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, slot->levels * faces, regions);
        set->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0u, 0u, NULL, 0u,
                                  NULL, 1u, &to_sampled);
        ok = set->vkEndCommandBuffer(command) == VK_SUCCESS;
    }
    if (ok) {
        const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1u, .pCommandBuffers = &command};
        const uint64_t phase_start = gpu_phase_now();
        ok = set->vkQueueSubmit(set->native.queue, 1u, &submit, VK_NULL_HANDLE) == VK_SUCCESS &&
             set->vkQueueWaitIdle(set->native.queue) == VK_SUCCESS;
        gpu_phase_add(GPU_PHASE_TEXTURE_SYNC, phase_start);
    }
    set->vkFreeCommandBuffers(set->native.device, set->native.command_pool, 1u, &command);
    return ok;
}

/* Host bookkeeping grows under a hard per-batch resource budget. Failure leaves
 * the old storage and all referenced Vulkan objects alive. */
static void *grow_array(void *old, size_t *capacity, size_t wanted, size_t maximum, size_t stride)
{
    if (wanted > maximum || wanted > SIZE_MAX / stride) return NULL;
    if (wanted <= *capacity) return old;
    size_t next = *capacity == 0u ? 64u : *capacity * 2u;
    if (next < wanted) next = wanted;
    if (next > maximum) next = maximum;
    void *grown = realloc(old, next * stride);
    if (grown != NULL) *capacity = next;
    return grown;
}

static bool reserve_retirement(live_vk_texture_set *set)
{
    image_slot *grown = grow_array(set->retired, &set->retired_capacity, set->retired_count + 1u,
                                  BATCH_VERSION_LIMIT, sizeof(*grown));
    if (grown == NULL) { set->stats.version_refusals++; return false; }
    set->retired = grown;
    return true;
}

static bool batch_descriptor_get(live_vk_texture_set *set, VkImageView view, VkSampler sampler,
                                 VkImageLayout layout, VkDescriptorSet *out)
{
    for (size_t i = 0u; i < set->descriptor_count; i++) {
        const batch_descriptor *entry = &set->batch_descriptors[i];
        if (entry->key.view == view && entry->key.sampler == sampler && entry->key.layout == layout) {
            *out = entry->descriptor;
            return true;
        }
    }
    batch_descriptor *records = grow_array(set->batch_descriptors, &set->descriptor_capacity,
                                           set->descriptor_count + 1u, BATCH_VERSION_LIMIT, sizeof(*records));
    if (records == NULL) { set->stats.version_refusals++; return false; }
    set->batch_descriptors = records;
    const size_t page_index = set->descriptor_count / BATCH_PAGE_SETS;
    if (page_index >= set->page_count) {
        descriptor_page *pages = grow_array(set->pages, &set->page_capacity, page_index + 1u,
                                            BATCH_VERSION_LIMIT / BATCH_PAGE_SETS, sizeof(*pages));
        if (pages == NULL) { set->stats.version_refusals++; return false; }
        set->pages = pages;
        memset(&pages[page_index], 0, sizeof pages[page_index]);
        set->page_count++;
    }
    descriptor_page *page = &set->pages[page_index];
    if (page->pool == VK_NULL_HANDLE) {
        const VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, BATCH_PAGE_SETS};
        const VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = BATCH_PAGE_SETS, .poolSizeCount = 1u, .pPoolSizes = &pool_size};
        if (set->vkCreateDescriptorPool(set->native.device, &pool_info, NULL, &page->pool) != VK_SUCCESS)
            return false;
    }
    if (!page->allocated) {
        VkDescriptorSetLayout layouts[BATCH_PAGE_SETS];
        for (size_t i = 0u; i < BATCH_PAGE_SETS; i++) layouts[i] = set->set_layout;
        const VkDescriptorSetAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = page->pool, .descriptorSetCount = BATCH_PAGE_SETS, .pSetLayouts = layouts};
        if (set->vkAllocateDescriptorSets(set->native.device, &allocate, page->sets) != VK_SUCCESS) {
            (void)set->vkResetDescriptorPool(set->native.device, page->pool, 0u);
            return false;
        }
        page->allocated = true;
    }
    const VkDescriptorSet descriptor = page->sets[set->descriptor_count % BATCH_PAGE_SETS];
    const VkDescriptorImageInfo image = {sampler, view, layout};
    const VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = descriptor,
        .descriptorCount = 1u, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &image};
    set->vkUpdateDescriptorSets(set->native.device, 1u, &write, 0u, NULL);
    set->batch_descriptors[set->descriptor_count++] = (batch_descriptor){{view, sampler, layout}, descriptor};
    if (set->stats.descriptor_versions_peak < set->descriptor_count)
        set->stats.descriptor_versions_peak = set->descriptor_count;
    set->stats.descriptor_writes++;
    *out = descriptor;
    return true;
}

bool live_vk_texture_begin_batch(live_vk_texture_set *set)
{
    if (set == NULL || set->batch_active) return false;
    for (size_t i = 0u; i < GUEST_SLOTS; i++) set->images[i].held = false;
    set->batch_active = true;
    set->stats.batch_begins++;
    return true;
}

bool live_vk_texture_end_batch(live_vk_texture_set *set)
{
    /* Caller has submitted and completed every command buffer referencing this
     * batch. QueueWaitIdle alone cannot establish submission of an open buffer. */
    if (set == NULL || !set->batch_active) return false;
    const uint64_t phase_start = gpu_phase_now();
    const bool idle_failed = set->vkQueueWaitIdle(set->native.queue) != VK_SUCCESS;
    gpu_phase_add(GPU_PHASE_TEXTURE_SYNC, phase_start);
    if (idle_failed) {
        set->completion_retry_required = true;
        set->stats.completion_failures++;
        return false;
    }
    for (size_t i = 0u; i < set->page_count; i++) {
        if (set->pages[i].pool == VK_NULL_HANDLE) continue;
        if (set->vkResetDescriptorPool(set->native.device, set->pages[i].pool, 0u) != VK_SUCCESS) {
            /* Earlier pages may already have reset successfully, invalidating sets still named
             * by batch_descriptors. Drop every key and refuse binds until a complete retry resets
             * all pages; a failed page's post-error contents are not assumed reusable. */
            set->descriptor_count = 0u;
            set->completion_retry_required = true;
            set->stats.completion_failures++;
            return false;
        }
        set->pages[i].allocated = false;
    }
    for (size_t i = 0u; i < set->retired_count; i++) destroy_slot_image(set, &set->retired[i]);
    set->retired_count = 0u;
    set->descriptor_count = 0u;
    set->completion_retry_required = false;
    for (size_t i = 0u; i < GUEST_SLOTS; i++) set->images[i].held = false;
    set->batch_active = false;
    set->stats.batch_completions++;
    return true;
}

bool live_vk_texture_batch_active(const live_vk_texture_set *set)
{
    return set != NULL && set->batch_active;
}

/* --- samplers and descriptors --------------------------------------------------------------------------------------- */
static bool same_sampler(const live_vk_sampler_desc *a, const live_vk_sampler_desc *b)
{
    return a->address_u == b->address_u && a->address_v == b->address_v && a->min_filter == b->min_filter &&
           a->mag_filter == b->mag_filter && a->mipmap_mode == b->mipmap_mode &&
           a->min_lod == b->min_lod && a->max_lod == b->max_lod && a->lod_bias == b->lod_bias &&
           a->max_anisotropy == b->max_anisotropy;
}

static VkSampler find_sampler(live_vk_texture_set *set, const live_vk_sampler_desc *desc)
{
    if (desc->max_anisotropy > 1u && !set->native.sampler_anisotropy) return VK_NULL_HANDLE;
    for (size_t index = 0u; index < set->sampler_count; index++) {
        if (same_sampler(&set->samplers[index].key, desc)) {
            return set->samplers[index].sampler;
        }
    }
    if (set->sampler_count == LIVE_VK_TEXTURE_SAMPLERS) {
        return VK_NULL_HANDLE;
    }
    const VkSamplerCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = desc->mag_filter, .minFilter = desc->min_filter,
        .mipmapMode = desc->mipmap_mode, .addressModeU = desc->address_u, .addressModeV = desc->address_v,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .minLod = desc->min_lod, .maxLod = desc->max_lod,
        .mipLodBias = desc->lod_bias < -set->max_lod_bias ? -set->max_lod_bias :
                      desc->lod_bias > set->max_lod_bias ? set->max_lod_bias : desc->lod_bias,
        .anisotropyEnable = desc->max_anisotropy > 1u ? VK_TRUE : VK_FALSE,
        .maxAnisotropy = (float)desc->max_anisotropy > set->max_sampler_anisotropy ? set->max_sampler_anisotropy :
                         desc->max_anisotropy > 1u ? (float)desc->max_anisotropy : 1.0f,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
    VkSampler sampler = VK_NULL_HANDLE;
    if (set->vkCreateSampler(set->native.device, &info, NULL, &sampler) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    set->samplers[set->sampler_count++] = (sampler_slot){*desc, sampler};
    set->stats.sampler_creates++;
    return sampler;
}

static bool write_descriptor(live_vk_texture_set *set, size_t index, VkImageView view, VkSampler sampler, VkImageLayout layout)
{
    descriptor_state *state = &set->written[index];
    if (state->view == view && state->sampler == sampler && state->layout == layout) {
        return true;
    }
    const uint64_t phase_start = gpu_phase_now();
    const bool idle_failed = set->vkQueueWaitIdle(set->native.queue) != VK_SUCCESS;
    gpu_phase_add(GPU_PHASE_TEXTURE_SYNC, phase_start);
    if (idle_failed) {
        return false;
    }
    const VkDescriptorImageInfo image = {sampler, view, layout};
    const VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set->sets[index],
                                        .descriptorCount = 1u, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                        .pImageInfo = &image};
    set->vkUpdateDescriptorSets(set->native.device, 1u, &write, 0u, NULL);
    *state = (descriptor_state){view, sampler, layout};
    set->stats.descriptor_writes++;
    return true;
}

/* --- public --------------------------------------------------------------------------------------------------------- */
live_vk_texture_set *live_vk_texture_create(const gpu_window_native *native, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                                            const char **error)
{
    const char *ignored = NULL;
    if (error == NULL) {
        error = &ignored;
    }
    if (native == NULL || native->device == VK_NULL_HANDLE || native->queue == VK_NULL_HANDLE ||
        native->command_pool == VK_NULL_HANDLE || native->get_device_proc_addr == NULL || get_instance_proc_addr == NULL) {
        *error = "incomplete native device view";
        return NULL;
    }
    live_vk_texture_set *set = calloc(1u, sizeof *set);
    if (set == NULL) {
        *error = "out of memory";
        return NULL;
    }
    set->native = *native;
#define X(name) do { PFN_vkVoidFunction fn = native->get_device_proc_addr(native->device, #name); \
        memcpy(&set->name, &fn, sizeof set->name); if (set->name == NULL) { *error = "missing device function " #name; goto fail; } } while (0);
    DEVICE_FUNCTIONS(X)
#undef X
    PFN_vkVoidFunction properties_fn = get_instance_proc_addr(native->instance, "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties get_properties = NULL;
    memcpy(&get_properties, &properties_fn, sizeof get_properties);
    if (get_properties == NULL) {
        *error = "missing vkGetPhysicalDeviceMemoryProperties";
        goto fail;
    }
    get_properties(native->physical_device, &set->memory_properties);
    properties_fn = get_instance_proc_addr(native->instance, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceProperties get_limits = NULL;
    memcpy(&get_limits, &properties_fn, sizeof get_limits);
    if (get_limits == NULL) { *error = "missing vkGetPhysicalDeviceProperties"; goto fail; }
    VkPhysicalDeviceProperties properties;
    get_limits(native->physical_device, &properties);
    set->max_lod_bias = properties.limits.maxSamplerLodBias;
    set->max_sampler_anisotropy = properties.limits.maxSamplerAnisotropy;
    const VkDescriptorSetLayoutBinding binding = {.binding = 0u, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                  .descriptorCount = 1u, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    const VkDescriptorSetLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                         .bindingCount = 1u, .pBindings = &binding};
    const VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SET_COUNT};
    const VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = SET_COUNT,
                                                  .poolSizeCount = 1u, .pPoolSizes = &pool_size};
    if (set->vkCreateDescriptorSetLayout(native->device, &layout_info, NULL, &set->set_layout) != VK_SUCCESS ||
        set->vkCreateDescriptorPool(native->device, &pool_info, NULL, &set->pool) != VK_SUCCESS) {
        *error = "descriptor pool or layout creation failed";
        goto fail;
    }
    VkDescriptorSetLayout layouts[SET_COUNT];
    for (size_t index = 0u; index < SET_COUNT; index++) {
        layouts[index] = set->set_layout;
    }
    const VkDescriptorSetAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = set->pool,
                                                  .descriptorSetCount = SET_COUNT, .pSetLayouts = layouts};
    if (set->vkAllocateDescriptorSets(native->device, &allocate, set->sets) != VK_SUCCESS) {
        *error = "descriptor set allocation failed";
        goto fail;
    }
    return set;
fail:
    live_vk_texture_destroy(set);
    return NULL;
}

void live_vk_texture_destroy(live_vk_texture_set *set)
{
    if (set == NULL) {
        return;
    }
    if (set->vkQueueWaitIdle != NULL) {
        (void)set->vkQueueWaitIdle(set->native.queue);
    }
    if (set->vkDestroyImage != NULL) {
        for (size_t index = 0u; index < GUEST_SLOTS; index++) {
            destroy_slot_image(set, &set->images[index]);
        }
        for (size_t index = 0u; index < set->retired_count; index++) destroy_slot_image(set, &set->retired[index]);
        for (size_t index = 0u; index < set->page_count; index++)
            if (set->pages[index].pool != VK_NULL_HANDLE)
                set->vkDestroyDescriptorPool(set->native.device, set->pages[index].pool, NULL);
        for (size_t index = 0u; index < set->sampler_count; index++) {
            set->vkDestroySampler(set->native.device, set->samplers[index].sampler, NULL);
        }
        if (set->staging != VK_NULL_HANDLE) {
            set->vkUnmapMemory(set->native.device, set->staging_memory);
            set->vkDestroyBuffer(set->native.device, set->staging, NULL);
            set->vkFreeMemory(set->native.device, set->staging_memory, NULL);
        }
        if (set->pool != VK_NULL_HANDLE) {
            set->vkDestroyDescriptorPool(set->native.device, set->pool, NULL);
        }
        if (set->set_layout != VK_NULL_HANDLE) {
            set->vkDestroyDescriptorSetLayout(set->native.device, set->set_layout, NULL);
        }
    }
    free(set->retired);
    free(set->pages);
    free(set->batch_descriptors);
    free(set);
}

VkDescriptorSetLayout live_vk_texture_set_layout(const live_vk_texture_set *set)
{
    return set->set_layout;
}

live_vk_texture_stats live_vk_texture_get_stats(const live_vk_texture_set *set)
{
    return set->stats;
}

bool live_vk_texture_register_target(live_vk_texture_set *set, uint32_t id, VkImageView view, VkImageLayout layout)
{
    if (view == VK_NULL_HANDLE) {
        return false;
    }
    size_t free_index = TARGET_SLOTS;
    for (size_t index = 0u; index < TARGET_SLOTS; index++) {
        if (set->targets[index].used && set->targets[index].id == id) {
            set->targets[index].view = view;
            set->targets[index].layout = layout;
            return true;
        }
        if (!set->targets[index].used && free_index == TARGET_SLOTS) {
            free_index = index;
        }
    }
    if (free_index == TARGET_SLOTS) {
        return false;
    }
    set->targets[free_index] = (target_slot){true, id, view, layout};
    return true;
}

void live_vk_texture_clear_targets(live_vk_texture_set *set)
{
    memset(set->targets, 0, sizeof set->targets);
}

bool live_vk_texture_bind(live_vk_texture_set *set, live_texture_cache *cache, const live_texture_result *result,
                          const live_vk_sampler_desc *sampler, live_vk_texture_bound *bound, const char **error)
{
    const char *ignored = NULL;
    if (error == NULL) {
        error = &ignored;
    }
    memset(bound, 0, sizeof *bound);
    if (set->completion_retry_required) {
        *error = "descriptor batch completion retry required";
        return false;
    }
    if (!sampler->ok) {
        *error = "undecodable sampler state";
        return false;
    }
    if (sampler->max_anisotropy > 1u && !set->native.sampler_anisotropy) {
        *error = "sampler anisotropy requested but not enabled on the logical device";
        return false;
    }
    VkSampler vk_sampler = find_sampler(set, sampler);
    if (vk_sampler == VK_NULL_HANDLE) {
        *error = "sampler creation failed or the sampler table is full";
        return false;
    }
    size_t descriptor_index;
    VkImageView view;
    VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (result->source == LIVE_TEXTURE_SOURCE_GUEST) {
        if (result->entry >= GUEST_SLOTS || result->rgba == NULL || result->width == 0u || result->height == 0u ||
            result->levels == 0u || result->levels > LIVE_TEXTURE_MAX_LEVELS) {
            *error = "malformed guest texture result";
            return false;
        }
        const live_texture_plan *checked_plan = &cache->entries[result->entry].plan;
        if (!checked_plan->ok || checked_plan->width != result->width || checked_plan->height != result->height ||
            checked_plan->levels != result->levels || checked_plan->rgba_bytes == 0u || checked_plan->cube != result->cube) {
            *error = "guest texture result does not match its validated plan";
            return false;
        }
        image_slot *slot = &set->images[result->entry];
        const bool resized = slot->image != VK_NULL_HANDLE && (slot->width != result->width || slot->height != result->height || slot->levels != result->levels || slot->cube != result->cube);
        const bool versioned = set->batch_active && slot->held && (resized || result->needs_upload);
        if (versioned) {
            if (!reserve_retirement(set)) { *error = "texture version bookkeeping exhausted"; return false; }
            image_slot fresh = {0};
            if (!create_slot_image(set, &fresh, result->width, result->height, result->levels, result->cube)) {
                *error = "texture version image creation failed";
                return false;
            }
            if (!upload_image(set, &fresh, result->rgba, &cache->entries[result->entry].plan)) {
                destroy_slot_image(set, &fresh);
                *error = "texture version upload failed";
                return false;
            }
            set->retired[set->retired_count++] = *slot;
            if (set->stats.retired_images_peak < set->retired_count)
                set->stats.retired_images_peak = set->retired_count;
            *slot = fresh;
            set->written[result->entry] = (descriptor_state){0};
            live_texture_mark_uploaded(cache, result->entry, result->generation);
            set->stats.uploads++;
            set->stats.upload_bytes += cache->entries[result->entry].plan.rgba_bytes;
            bound->uploaded = true;
        } else if (resized) {
            (void)set->vkQueueWaitIdle(set->native.queue);
            destroy_slot_image(set, slot);
            set->written[result->entry] = (descriptor_state){0};
        }
        const bool missing = slot->image == VK_NULL_HANDLE;
        if (missing && !create_slot_image(set, slot, result->width, result->height, result->levels, result->cube)) {
            *error = "image creation failed";
            return false;
        }
        if (!versioned && (missing || result->needs_upload)) {
            const live_texture_plan *plan = &cache->entries[result->entry].plan;
            const uint32_t bytes = plan->rgba_bytes;
            if (!upload_image(set, slot, result->rgba, plan)) {
                *error = "texture upload failed";
                return false;
            }
            live_texture_mark_uploaded(cache, result->entry, result->generation);
            set->stats.uploads++;
            set->stats.upload_bytes += bytes;
            bound->uploaded = true;
        }
        descriptor_index = result->entry;
        view = slot->view;
        bound->image = slot->image;
        bound->width = slot->width;
        bound->height = slot->height;
    } else if (result->source == LIVE_TEXTURE_SOURCE_TARGET) {
        const target_slot *target = NULL;
        size_t target_index = 0u;
        for (; target_index < TARGET_SLOTS; target_index++) {
            if (set->targets[target_index].used && set->targets[target_index].id == result->target_id) {
                target = &set->targets[target_index];
                break;
            }
        }
        if (target == NULL) {
            *error = "render target view not registered";
            return false;
        }
        descriptor_index = GUEST_SLOTS + target_index;
        view = target->view;
        layout = target->layout;
        bound->width = result->width;
        bound->height = result->height;
        set->stats.target_binds++;
    } else {
        *error = "refused texture result";
        return false;
    }
    if (set->batch_active) {
        if (!batch_descriptor_get(set, view, vk_sampler, layout, &bound->descriptor)) {
            *error = "immutable batch descriptor allocation failed";
            return false;
        }
        if (result->source == LIVE_TEXTURE_SOURCE_GUEST) set->images[result->entry].held = true;
    } else {
        if (!write_descriptor(set, descriptor_index, view, vk_sampler, layout)) {
            *error = "descriptor update failed";
            return false;
        }
        bound->descriptor = set->sets[descriptor_index];
    }
    bound->view = view;
    bound->sampler = vk_sampler;
    bound->layout = layout;
    return true;
}

size_t live_vk_texture_trim(live_vk_texture_set *set, const live_texture_cache *cache)
{
    if (set->batch_active) return 0u; /* Defer eviction until all recorded users complete. */
    size_t destroyed = 0u;
    bool idle = false;
    for (size_t index = 0u; index < GUEST_SLOTS; index++) {
        if (set->images[index].image != VK_NULL_HANDLE && !cache->entries[index].in_use) {
            if (!idle) {
                (void)set->vkQueueWaitIdle(set->native.queue);
                idle = true;
            }
            destroy_slot_image(set, &set->images[index]);
            set->written[index] = (descriptor_state){0};
            destroyed++;
        }
    }
    return destroyed;
}
