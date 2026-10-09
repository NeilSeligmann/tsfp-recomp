/* SPDX-License-Identifier: GPL-3.0-or-later
 * T793 Vulkan live render targets. See live_vk_target.h. */
#define _POSIX_C_SOURCE 200809L
#include "gpu_phase_timing.h"
#include "live_vk_target.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEVICE_FUNCTIONS(X)                                                                                              \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory)               \
    X(vkBindImageMemory) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory)     \
    X(vkMapMemory) X(vkUnmapMemory) X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkBeginCommandBuffer)          \
    X(vkEndCommandBuffer) X(vkCmdCopyImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyBufferToImage) X(vkCmdPipelineBarrier)  \
    X(vkCmdClearColorImage) X(vkCmdBlitImage) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkCreateImageView) X(vkDestroyImageView)

typedef struct {
    bool used;
    bool has_image;
    uint32_t data;
    live_target_desc desc;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view; /* created on first use by live_vk_target_texture_view, T791 */
    VkFormat vk_format;
    uint32_t texels;           /* image width: pitch / bytes per pixel */
} target_slot;

typedef struct {
    uint64_t number;
    uint32_t data, width, height, pitch;
    size_t bytes;
    uint8_t *pixels;
} media_front_snapshot;

struct live_vk_target_set {
    live_vk_target_device device;
#define DECLARE(name) PFN_##name name;
    DEVICE_FUNCTIONS(DECLARE)
#undef DECLARE
    live_target_registry *registry;
    live_texture_cache *cache;
    uint32_t allowed;
    target_slot slots[LIVE_TARGET_MAX_TARGETS];
    live_vk_target_stats stats;
    live_present_schedule schedule;
    media_front_snapshot media_queue[LIVE_PRESENT_QUEUE];
    uint32_t media_count;
    media_front_snapshot media_front;
    bool media_mode;
    live_vk_target_present_fn present;
    void *present_context;
    uint8_t *overlay_rgb;
    uint32_t overlay_width, overlay_height;
    bool overlay_active;
    uint64_t overlay_sequence;
    /* the composed front, valid for (front_data, front_generation, overlay_sequence_composed) */
    uint8_t *front_bgra;
    size_t front_capacity;
    uint32_t front_data;
    uint64_t front_generation;
    bool front_valid;
    uint8_t *frame_rgba;
    size_t frame_capacity;
    /* T849: the direct (swapchain pre-pass blit) route and its overlay texture */
    live_vk_target_direct_fn direct;
    void *direct_context;
    VkImage overlay_image;
    VkDeviceMemory overlay_memory;
    uint32_t overlay_image_width, overlay_image_height;
    uint64_t overlay_uploaded_sequence;
    bool overlay_uploaded;
    /* the layers of the last vblank, for live_vk_target_compose_last */
    bool last_valid, last_has_front, last_has_overlay;
    uint32_t last_front_data;
};

static uint64_t now_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static void media_snapshot_release(media_front_snapshot *snapshot)
{
    free(snapshot->pixels);
    memset(snapshot, 0, sizeof *snapshot);
}

static void media_queue_drop_oldest(live_vk_target_set *set)
{
    if (set->media_count == 0u) {
        return;
    }
    media_snapshot_release(&set->media_queue[0]);
    memmove(&set->media_queue[0], &set->media_queue[1], (LIVE_PRESENT_QUEUE - 1u) * sizeof set->media_queue[0]);
    memset(&set->media_queue[LIVE_PRESENT_QUEUE - 1u], 0, sizeof set->media_queue[0]);
    set->media_count--;
    set->stats.media_snapshot_drops++;
}

static live_vk_target_set *g_observer_set;

bool live_vk_target_device_from_native(const gpu_window_native *native, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                                       live_vk_target_device *out)
{
    if (native == NULL || out == NULL || get_instance_proc_addr == NULL || native->device == VK_NULL_HANDLE ||
        native->get_device_proc_addr == NULL || native->instance == VK_NULL_HANDLE) {
        return false;
    }
    PFN_vkGetPhysicalDeviceMemoryProperties properties = NULL;
    const PFN_vkVoidFunction raw = get_instance_proc_addr(native->instance, "vkGetPhysicalDeviceMemoryProperties");
    memcpy(&properties, &raw, sizeof properties);
    if (properties == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    out->device = native->device;
    out->queue = native->queue;
    out->queue_family = native->queue_family;
    out->command_pool = native->command_pool;
    out->get_device_proc_addr = native->get_device_proc_addr;
    properties(native->physical_device, &out->memory);
    return true;
}

/* --- device helpers --- */

static uint32_t memory_type(const live_vk_target_set *set, uint32_t bits, VkMemoryPropertyFlags wanted)
{
    for (uint32_t i = 0u; i < set->device.memory.memoryTypeCount; i++) {
        if ((bits & (1u << i)) != 0u && (set->device.memory.memoryTypes[i].propertyFlags & wanted) == wanted) {
            return i;
        }
    }
    return UINT32_MAX;
}

static bool device_fail(live_vk_target_set *set)
{
    set->stats.device_failures++;
    return false;
}

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t *mapped;
    size_t bytes;
} host_buffer;

static void host_buffer_free(live_vk_target_set *set, host_buffer *buffer)
{
    if (buffer->mapped != NULL) {
        set->vkUnmapMemory(set->device.device, buffer->memory);
    }
    if (buffer->buffer != VK_NULL_HANDLE) {
        set->vkDestroyBuffer(set->device.device, buffer->buffer, NULL);
    }
    if (buffer->memory != VK_NULL_HANDLE) {
        set->vkFreeMemory(set->device.device, buffer->memory, NULL);
    }
    memset(buffer, 0, sizeof *buffer);
}

static bool host_buffer_make(live_vk_target_set *set, host_buffer *buffer, size_t bytes)
{
    memset(buffer, 0, sizeof *buffer);
    const VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (set->vkCreateBuffer(set->device.device, &info, NULL, &buffer->buffer) != VK_SUCCESS) {
        return device_fail(set);
    }
    VkMemoryRequirements requirements;
    set->vkGetBufferMemoryRequirements(set->device.device, buffer->buffer, &requirements);
    const uint32_t type = memory_type(set, requirements.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    const VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = type};
    void *mapped = NULL;
    if (type == UINT32_MAX || set->vkAllocateMemory(set->device.device, &allocate, NULL, &buffer->memory) != VK_SUCCESS ||
        set->vkBindBufferMemory(set->device.device, buffer->buffer, buffer->memory, 0u) != VK_SUCCESS ||
        set->vkMapMemory(set->device.device, buffer->memory, 0u, bytes, 0u, &mapped) != VK_SUCCESS || mapped == NULL) {
        host_buffer_free(set, buffer);
        return device_fail(set);
    }
    buffer->mapped = mapped;
    buffer->bytes = bytes;
    return true;
}

static bool begin_commands(live_vk_target_set *set, VkCommandBuffer *command)
{
    const VkCommandBufferAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = set->device.command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1u};
    if (set->vkAllocateCommandBuffers(set->device.device, &allocate, command) != VK_SUCCESS) {
        return device_fail(set);
    }
    const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (set->vkBeginCommandBuffer(*command, &begin) != VK_SUCCESS) {
        set->vkFreeCommandBuffers(set->device.device, set->device.command_pool, 1u, command);
        return device_fail(set);
    }
    return true;
}

/* End, submit, wait for the queue and free. Every submission is synchronous: the staged path reads the result on the host. */
static bool finish_commands(live_vk_target_set *set, VkCommandBuffer command)
{
    bool ok = set->vkEndCommandBuffer(command) == VK_SUCCESS;
    if (ok) {
        const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1u, .pCommandBuffers = &command};
        const uint64_t phase_start = gpu_phase_now();
        ok = set->vkQueueSubmit(set->device.queue, 1u, &submit, VK_NULL_HANDLE) == VK_SUCCESS &&
             set->vkQueueWaitIdle(set->device.queue) == VK_SUCCESS;
        gpu_phase_add(GPU_PHASE_TARGET_SYNC, phase_start);
    }
    set->vkFreeCommandBuffers(set->device.device, set->device.command_pool, 1u, &command);
    return ok ? true : device_fail(set);
}

static void memory_barrier(live_vk_target_set *set, VkCommandBuffer command, VkAccessFlags from, VkAccessFlags to)
{
    const VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = from, .dstAccessMask = to};
    set->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 1u, &barrier, 0u,
                              NULL, 0u, NULL);
}

/* --- set --- */

live_vk_target_set *live_vk_target_create(const live_vk_target_device *device, uint32_t allowed, live_texture_cache *cache)
{
    if (device == NULL || device->device == VK_NULL_HANDLE || device->queue == VK_NULL_HANDLE ||
        device->command_pool == VK_NULL_HANDLE || device->get_device_proc_addr == NULL) {
        return NULL;
    }
    live_vk_target_set *set = calloc(1u, sizeof *set);
    if (set == NULL) {
        return NULL;
    }
    set->device = *device;
#define LOAD(name)                                                                       \
    do {                                                                                 \
        const PFN_vkVoidFunction raw = device->get_device_proc_addr(device->device, #name); \
        memcpy(&set->name, &raw, sizeof set->name);                                      \
        if (set->name == NULL) {                                                         \
            free(set);                                                                   \
            return NULL;                                                                 \
        }                                                                                \
    } while (0);
    DEVICE_FUNCTIONS(LOAD)
#undef LOAD
    set->registry = live_target_registry_create();
    if (set->registry == NULL) {
        free(set);
        return NULL;
    }
    set->cache = cache;
    set->allowed = allowed;
    live_present_init(&set->schedule, LIVE_PRESENT_DEFAULT_LATENCY);
    return set;
}

static void slot_release(live_vk_target_set *set, target_slot *slot)
{
    if (slot->view != VK_NULL_HANDLE) {
        set->vkDestroyImageView(set->device.device, slot->view, NULL);
    }
    if (slot->image != VK_NULL_HANDLE) {
        set->vkDestroyImage(set->device.device, slot->image, NULL);
    }
    if (slot->memory != VK_NULL_HANDLE) {
        set->vkFreeMemory(set->device.device, slot->memory, NULL);
    }
    memset(slot, 0, sizeof *slot);
}

static void overlay_texture_release(live_vk_target_set *set);

void live_vk_target_destroy(live_vk_target_set *set)
{
    if (set == NULL) {
        return;
    }
    if (g_observer_set == set) {
        g_observer_set = NULL;
    }
    (void)set->vkQueueWaitIdle(set->device.queue);
    for (size_t i = 0u; i < LIVE_TARGET_MAX_TARGETS; i++) {
        slot_release(set, &set->slots[i]);
    }
    live_target_registry_destroy(set->registry);
    overlay_texture_release(set);
    free(set->overlay_rgb);
    free(set->front_bgra);
    free(set->frame_rgba);
    free(set);
}

live_target_registry *live_vk_target_registry(live_vk_target_set *set)
{
    return set != NULL ? set->registry : NULL;
}

live_vk_target_stats live_vk_target_stats_get(const live_vk_target_set *set)
{
    return set != NULL ? set->stats : (live_vk_target_stats){0};
}

live_present_schedule live_vk_target_schedule(const live_vk_target_set *set)
{
    return set != NULL ? set->schedule : (live_present_schedule){0};
}

static target_slot *find_slot(const live_vk_target_set *set, uint32_t data)
{
    for (size_t i = 0u; i < LIVE_TARGET_MAX_TARGETS; i++) {
        if (set->slots[i].used && set->slots[i].data == data) {
            return (target_slot *)&set->slots[i];
        }
    }
    return NULL;
}

static bool image_format(uint32_t bytes_per_pixel, VkFormat *format)
{
    switch (bytes_per_pixel) {
    case 4u: *format = VK_FORMAT_B8G8R8A8_UNORM; return true;
    case 2u: *format = VK_FORMAT_R5G6B5_UNORM_PACK16; return true;
    case 1u: *format = VK_FORMAT_R8_UNORM; return true;
    default: return false;
    }
}

/* Create the image, bind device memory and zero fill it in layout GENERAL. */
static bool slot_create_image(live_vk_target_set *set, target_slot *slot)
{
    VkFormat format;
    if (!image_format(slot->desc.bytes_per_pixel, &format) || slot->desc.pitch % slot->desc.bytes_per_pixel != 0u) {
        return false;
    }
    slot->vk_format = format;
    slot->texels = slot->desc.pitch / slot->desc.bytes_per_pixel;
    const VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = format, .extent = {slot->texels, slot->desc.height, 1u}, .mipLevels = 1u, .arrayLayers = 1u,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (set->vkCreateImage(set->device.device, &info, NULL, &slot->image) != VK_SUCCESS) {
        return device_fail(set);
    }
    VkMemoryRequirements requirements;
    set->vkGetImageMemoryRequirements(set->device.device, slot->image, &requirements);
    uint32_t type = memory_type(set, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        type = memory_type(set, requirements.memoryTypeBits, 0u);
    }
    const VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = type};
    if (type == UINT32_MAX || set->vkAllocateMemory(set->device.device, &allocate, NULL, &slot->memory) != VK_SUCCESS ||
        set->vkBindImageMemory(set->device.device, slot->image, slot->memory, 0u) != VK_SUCCESS) {
        slot_release(set, slot);
        return device_fail(set);
    }
    VkCommandBuffer command;
    if (!begin_commands(set, &command)) {
        slot_release(set, slot);
        return false;
    }
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
    const VkImageMemoryBarrier to_general = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0u,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = slot->image, .subresourceRange = range};
    set->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, NULL, 0u,
                              NULL, 1u, &to_general);
    const VkClearColorValue zero = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}};
    set->vkCmdClearColorImage(command, slot->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1u, &range);
    if (!finish_commands(set, command)) {
        slot_release(set, slot);
        return false;
    }
    slot->has_image = true;
    set->stats.images_created++;
    return true;
}

live_vk_status live_vk_target_register(live_vk_target_set *set, uint32_t data, uint32_t format_word, uint32_t size_word,
                                       live_target_refusal *refusal)
{
    if (refusal != NULL) {
        *refusal = LIVE_TARGET_OK;
    }
    if (set == NULL) {
        return LIVE_VK_ARGUMENT;
    }
    const live_target_refusal result = live_target_register(set->registry, data, format_word, size_word, false);
    if (result != LIVE_TARGET_OK) {
        if (refusal != NULL) {
            *refusal = result;
        }
        return LIVE_VK_PLAN_REFUSED;
    }
    target_slot *slot = find_slot(set, data);
    if (slot != NULL) {
        return slot->has_image ? LIVE_VK_OK : LIVE_VK_NO_IMAGE;
    }
    for (size_t i = 0u; i < LIVE_TARGET_MAX_TARGETS && slot == NULL; i++) {
        if (!set->slots[i].used) {
            slot = &set->slots[i];
            slot->used = true;
            slot->data = data;
            slot->desc = *live_target_find(set->registry, data);
            if (!slot_create_image(set, slot)) {
                slot->used = true;  /* kept so a later blit names the missing image, not an unknown target */
                slot->has_image = false;
                return LIVE_VK_NO_IMAGE;
            }
            if (set->cache != NULL && slot->desc.swizzled && slot->desc.format == LIVE_TARGET_FORMAT_A8R8G8B8) {
                /* T1489: the image is in picture order, a swizzled texture header of the same size samples it as it is */
                if (live_texture_register_swizzled_target(set->cache, (uint32_t)i + 1u, data, slot->desc.width, slot->desc.height)) {
                    set->stats.texture_registrations++;
                } else {
                    set->stats.texture_registration_failures++;
                }
            } else if (set->cache != NULL && !slot->desc.swizzled && slot->desc.format == LIVE_TARGET_FORMAT_A8R8G8B8) {
                if (live_texture_register_target(set->cache, (uint32_t)i + 1u, data, slot->desc.width, slot->desc.height,
                                                 slot->desc.pitch)) {
                    set->stats.texture_registrations++;
                } else {
                    set->stats.texture_registration_failures++;
                }
            }
            return LIVE_VK_OK;
        }
    }
    return LIVE_VK_UNKNOWN_TARGET;
}

VkImageView live_vk_target_texture_view(live_vk_target_set *set, uint32_t texture_id)
{
    if (set == NULL || texture_id == 0u || texture_id > LIVE_TARGET_MAX_TARGETS) {
        return VK_NULL_HANDLE;
    }
    target_slot *slot = &set->slots[texture_id - 1u];
    if (!slot->used || !slot->has_image) {
        return VK_NULL_HANDLE;
    }
    if (slot->view == VK_NULL_HANDLE) {
        const VkImageViewCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = slot->image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = slot->vk_format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u}};
        if (set->vkCreateImageView(set->device.device, &info, NULL, &slot->view) != VK_SUCCESS) {
            slot->view = VK_NULL_HANDLE;
            (void)device_fail(set);
        }
    }
    return slot->view;
}

uint32_t live_vk_target_texture_id(const live_vk_target_set *set, uint32_t data)
{
    const target_slot *slot = set != NULL ? find_slot(set, data) : NULL;
    return slot != NULL && slot->has_image ? (uint32_t)(slot - set->slots) + 1u : 0u;
}

bool live_vk_target_has_image(const live_vk_target_set *set, uint32_t data)
{
    const target_slot *slot = set != NULL ? find_slot(set, data) : NULL;
    return slot != NULL && slot->has_image;
}

VkImage live_vk_target_image(const live_vk_target_set *set, uint32_t data, VkFormat *format, VkExtent2D *extent,
                             uint64_t *generation)
{
    const target_slot *slot = set != NULL ? find_slot(set, data) : NULL;
    if (slot == NULL || !slot->has_image) {
        return VK_NULL_HANDLE;
    }
    if (format != NULL) {
        *format = slot->vk_format;
    }
    if (extent != NULL) {
        *extent = (VkExtent2D){slot->texels, slot->desc.height};
    }
    if (generation != NULL) {
        *generation = live_target_generation(set->registry, data);
    }
    return slot->image;
}

void live_vk_target_note_written(live_vk_target_set *set, uint32_t data)
{
    if (set != NULL) {
        live_target_note_written(set->registry, data);
    }
}

/* --- whole image transfer --- */

static VkBufferImageCopy whole_image_copy(const target_slot *slot)
{
    return (VkBufferImageCopy){.bufferRowLength = slot->texels, .bufferImageHeight = slot->desc.height,
                               .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
                               .imageExtent = {slot->texels, slot->desc.height, 1u}};
}

static bool image_to_buffer(live_vk_target_set *set, const target_slot *slot, host_buffer *buffer)
{
    VkCommandBuffer command;
    if (!begin_commands(set, &command)) {
        return false;
    }
    memory_barrier(set, command, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    const VkBufferImageCopy region = whole_image_copy(slot);
    set->vkCmdCopyImageToBuffer(command, slot->image, VK_IMAGE_LAYOUT_GENERAL, buffer->buffer, 1u, &region);
    return finish_commands(set, command);
}

static bool buffer_to_image(live_vk_target_set *set, const target_slot *slot, host_buffer *buffer)
{
    VkCommandBuffer command;
    if (!begin_commands(set, &command)) {
        return false;
    }
    const VkBufferImageCopy region = whole_image_copy(slot);
    set->vkCmdCopyBufferToImage(command, buffer->buffer, slot->image, VK_IMAGE_LAYOUT_GENERAL, 1u, &region);
    return finish_commands(set, command);
}

static size_t image_bytes(const target_slot *slot)
{
    return (size_t)slot->desc.pitch * slot->desc.height;
}

live_vk_status live_vk_target_upload(live_vk_target_set *set, uint32_t data, const uint8_t *bytes, size_t length)
{
    target_slot *slot = set != NULL ? find_slot(set, data) : NULL;
    if (set == NULL || bytes == NULL) {
        return LIVE_VK_ARGUMENT;
    }
    if (slot == NULL) {
        return LIVE_VK_UNKNOWN_TARGET;
    }
    if (!slot->has_image) {
        return LIVE_VK_NO_IMAGE;
    }
    if (length != image_bytes(slot)) {
        return LIVE_VK_ARGUMENT;
    }
    host_buffer buffer;
    if (!host_buffer_make(set, &buffer, length)) {
        return LIVE_VK_DEVICE_FAILED;
    }
    memcpy(buffer.mapped, bytes, length);
    const bool ok = buffer_to_image(set, slot, &buffer);
    host_buffer_free(set, &buffer);
    if (!ok) {
        return LIVE_VK_DEVICE_FAILED;
    }
    live_target_note_written(set->registry, data);
    return LIVE_VK_OK;
}

live_vk_status live_vk_target_readback(live_vk_target_set *set, uint32_t data, uint8_t *bytes, size_t length)
{
    target_slot *slot = set != NULL ? find_slot(set, data) : NULL;
    if (set == NULL || bytes == NULL) {
        return LIVE_VK_ARGUMENT;
    }
    if (slot == NULL) {
        return LIVE_VK_UNKNOWN_TARGET;
    }
    if (!slot->has_image) {
        return LIVE_VK_NO_IMAGE;
    }
    if (length != image_bytes(slot)) {
        return LIVE_VK_ARGUMENT;
    }
    const uint64_t phase_start = gpu_phase_now();
    host_buffer buffer;
    if (!host_buffer_make(set, &buffer, length)) {
        return LIVE_VK_DEVICE_FAILED;
    }
    const bool ok = image_to_buffer(set, slot, &buffer);
    if (ok) {
        memcpy(bytes, buffer.mapped, length);
    }
    host_buffer_free(set, &buffer);
    gpu_phase_add(GPU_PHASE_READBACK, phase_start);
    return ok ? LIVE_VK_OK : LIVE_VK_DEVICE_FAILED;
}

/* --- blits --- */

live_target_blit live_vk_target_blit_from_copy(const gpu_pgraph_copy *copy)
{
    return (live_target_blit){.source_data = copy->source_offset,
                              .destination_data = copy->destination_offset,
                              .color_format = copy->color_format,
                              .source_pitch = copy->source_pitch,
                              .destination_pitch = copy->destination_pitch,
                              .in_x = copy->in_x,
                              .in_y = copy->in_y,
                              .out_x = copy->out_x,
                              .out_y = copy->out_y,
                              .width = copy->width,
                              .height = copy->height,
                              .operation = copy->operation};
}

static bool run_copy_image(live_vk_target_set *set, const target_slot *source, const target_slot *destination,
                           const live_target_blit_plan *plan)
{
    VkCommandBuffer command;
    if (!begin_commands(set, &command)) {
        return false;
    }
    const uint32_t bytes = plan->bytes_per_pixel;
    const VkImageCopy region = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
        .srcOffset = {(int32_t)((plan->source_offset % plan->source_pitch) / bytes),
                      (int32_t)(plan->source_offset / plan->source_pitch), 0},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
        .dstOffset = {(int32_t)((plan->destination_offset % plan->destination_pitch) / bytes),
                      (int32_t)(plan->destination_offset / plan->destination_pitch), 0},
        .extent = {plan->row_bytes / bytes, plan->rows, 1u}};
    memory_barrier(set, command, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    set->vkCmdCopyImage(command, source->image, VK_IMAGE_LAYOUT_GENERAL, destination->image, VK_IMAGE_LAYOUT_GENERAL, 1u,
                        &region);
    return finish_commands(set, command);
}

static bool run_staged(live_vk_target_set *set, const target_slot *source, const target_slot *destination,
                       const live_target_blit_plan *plan)
{
    host_buffer source_buffer, destination_buffer;
    const bool same = source == destination;
    if (!host_buffer_make(set, &source_buffer, image_bytes(source))) {
        return false;
    }
    bool ok = image_to_buffer(set, source, &source_buffer);
    memset(&destination_buffer, 0, sizeof destination_buffer);
    if (ok && !same) {
        ok = host_buffer_make(set, &destination_buffer, image_bytes(destination)) &&
             image_to_buffer(set, destination, &destination_buffer);
    }
    if (ok) {
        live_target_execute_plan(plan, source_buffer.mapped, same ? source_buffer.mapped : destination_buffer.mapped);
        ok = buffer_to_image(set, destination, same ? &source_buffer : &destination_buffer);
    }
    host_buffer_free(set, &source_buffer);
    host_buffer_free(set, &destination_buffer);
    return ok;
}

live_vk_status live_vk_target_blit(live_vk_target_set *set, const live_target_blit *blit, live_target_blit_plan *plan,
                                   live_target_refusal *refusal)
{
    if (refusal != NULL) {
        *refusal = LIVE_TARGET_OK;
    }
    if (set == NULL || blit == NULL) {
        return LIVE_VK_ARGUMENT;
    }
    live_target_blit_plan local;
    live_target_blit_plan *use = plan != NULL ? plan : &local;
    const live_target_refusal planned = live_target_plan_blit(set->registry, blit, set->allowed, use);
    if (planned != LIVE_TARGET_OK) {
        set->stats.refused_blits++;
        if (refusal != NULL) {
            *refusal = planned;
        }
        return LIVE_VK_PLAN_REFUSED;
    }
    if (use->empty) {
        set->stats.empty_blits++;
        return LIVE_VK_OK;
    }
    const target_slot *source = find_slot(set, blit->source_data);
    const target_slot *destination = find_slot(set, blit->destination_data);
    if (source == NULL || destination == NULL) {
        return LIVE_VK_UNKNOWN_TARGET;
    }
    if (!source->has_image || !destination->has_image) {
        return LIVE_VK_NO_IMAGE;
    }
    if (use->copy_image_ok) {
        if (!run_copy_image(set, source, destination, use)) {
            return LIVE_VK_DEVICE_FAILED;
        }
        set->stats.copy_image_blits++;
    } else {
        if (!run_staged(set, source, destination, use)) {
            return LIVE_VK_DEVICE_FAILED;
        }
        set->stats.staged_blits++;
    }
    live_target_note_written(set->registry, blit->destination_data);
    return LIVE_VK_OK;
}

size_t live_vk_target_apply_copies(live_vk_target_set *set, const gpu_pgraph_copy *copies, size_t count, size_t *first_failure,
                                   live_target_refusal *refusal)
{
    if (first_failure != NULL) {
        *first_failure = count;
    }
    size_t applied = 0u;
    for (size_t i = 0u; set != NULL && copies != NULL && i < count; i++) {
        const live_target_blit blit = live_vk_target_blit_from_copy(&copies[i]);
        if (live_vk_target_blit(set, &blit, NULL, refusal) != LIVE_VK_OK) {
            if (first_failure != NULL) {
                *first_failure = i;
            }
            break;
        }
        applied++;
    }
    return applied;
}

/* --- present --- */

void live_vk_target_set_present(live_vk_target_set *set, live_vk_target_present_fn present, void *context)
{
    if (set != NULL) {
        set->present = present;
        set->present_context = context;
    }
}

live_target_refusal live_vk_target_present_submit(live_vk_target_set *set, const d3d8_frame_record *record)
{
    if (set == NULL || record == NULL) {
        return LIVE_TARGET_REFUSE_ARGUMENT;
    }
    const live_present_event event = {.number = record->number, .interval = record->interval, .vblank = record->vblank,
                                      .data = record->data, .format_word = record->format_word,
                                      .size_word = record->size_word};
    const live_target_refusal result = live_present_submit(&set->schedule, set->registry, &event);
    if (result != LIVE_TARGET_OK) {
        set->stats.present_refused++;
        return result;
    }
    set->stats.presents_submitted++;
    (void)live_vk_target_register(set, record->data, record->format_word, record->size_word, NULL);
    return LIVE_TARGET_OK;
}

void live_vk_target_present_observer(const d3d8_frame_record *record)
{
    if (g_observer_set != NULL) {
        (void)live_vk_target_present_submit(g_observer_set, record);
    }
}

void live_vk_target_bind_observer(live_vk_target_set *set)
{
    g_observer_set = set;
}

void live_vk_target_overlay_submit(live_vk_target_set *set, uint32_t width, uint32_t height, const uint8_t *rgb)
{
    if (set == NULL || rgb == NULL || width == 0u || height == 0u || width > 4096u || height > 4096u) {
        return;
    }
    const size_t bytes = (size_t)width * height * 3u;
    uint8_t *copy = malloc(bytes);
    if (copy == NULL) {
        return;
    }
    memcpy(copy, rgb, bytes);
    free(set->overlay_rgb);
    set->overlay_rgb = copy;
    set->overlay_width = width;
    set->overlay_height = height;
    set->overlay_active = true;
    set->overlay_sequence++;
    set->stats.overlay_submitted++;
}

void live_vk_target_overlay_clear(live_vk_target_set *set)
{
    if (set != NULL && set->overlay_active) {
        set->overlay_active = false;
        set->overlay_sequence++;
    }
}

void live_vk_target_compose(const uint8_t *front_bgra, uint32_t front_stride, const uint8_t *overlay_rgb, uint32_t overlay_width,
                            uint32_t overlay_height, uint32_t out_width, uint32_t out_height, uint8_t *out_rgba)
{
    for (uint32_t y = 0u; y < out_height; y++) {
        uint8_t *row = out_rgba + (size_t)y * out_width * 4u;
        for (uint32_t x = 0u; x < out_width; x++) {
            uint8_t *pixel = row + (size_t)x * 4u;
            if (front_bgra != NULL) {
                const uint8_t *source = front_bgra + (size_t)y * front_stride + (size_t)x * 4u;
                pixel[0] = source[2];
                pixel[1] = source[1];
                pixel[2] = source[0];
            } else {
                pixel[0] = pixel[1] = pixel[2] = 0u;
            }
            pixel[3] = 0xFFu;
        }
    }
    if (overlay_rgb == NULL || overlay_width == 0u || overlay_height == 0u || out_width == 0u || out_height == 0u) {
        return;
    }
    /* the largest rectangle of the picture's aspect that fits, centred */
    uint32_t fit_width = out_width;
    uint32_t fit_height = (uint32_t)(((uint64_t)out_width * overlay_height) / overlay_width);
    if (fit_height > out_height) {
        fit_height = out_height;
        fit_width = (uint32_t)(((uint64_t)out_height * overlay_width) / overlay_height);
    }
    if (fit_width == 0u || fit_height == 0u) {
        return;
    }
    const uint32_t left = (out_width - fit_width) / 2u;
    const uint32_t top = (out_height - fit_height) / 2u;
    for (uint32_t y = 0u; y < fit_height; y++) {
        const uint32_t source_y = (uint32_t)(((uint64_t)y * overlay_height) / fit_height);
        for (uint32_t x = 0u; x < fit_width; x++) {
            const uint32_t source_x = (uint32_t)(((uint64_t)x * overlay_width) / fit_width);
            const uint8_t *source = overlay_rgb + ((size_t)source_y * overlay_width + source_x) * 3u;
            uint8_t *pixel = out_rgba + ((size_t)(top + y) * out_width + left + x) * 4u;
            pixel[0] = source[0];
            pixel[1] = source[1];
            pixel[2] = source[2];
            pixel[3] = 0xFFu;
        }
    }
}

static bool ensure(uint8_t **buffer, size_t *capacity, size_t bytes)
{
    if (*capacity >= bytes) {
        return true;
    }
    uint8_t *grown = realloc(*buffer, bytes);
    if (grown == NULL) {
        return false;
    }
    *buffer = grown;
    *capacity = bytes;
    return true;
}

/* Read the front buffer, once per (data, generation). False when it cannot be shown. */
static bool read_front(live_vk_target_set *set, uint32_t data, const target_slot **slot_out)
{
    const target_slot *slot = find_slot(set, data);
    if (slot == NULL || !slot->has_image || slot->desc.bytes_per_pixel != 4u || slot->desc.swizzled) {
        set->stats.front_unreadable++;
        return false;
    }
    *slot_out = slot;
    const uint64_t generation = live_target_generation(set->registry, data);
    if (set->front_valid && set->front_data == data && set->front_generation == generation) {
        set->stats.front_cache_hits++;
        return true;
    }
    const size_t bytes = image_bytes(slot);
    if (!ensure(&set->front_bgra, &set->front_capacity, bytes)) {
        set->stats.front_unreadable++;
        return false;
    }
    set->front_valid = false;
    if (live_vk_target_readback(set, data, set->front_bgra, bytes) != LIVE_VK_OK) {
        set->stats.front_unreadable++;
        return false;
    }
    set->front_valid = true;
    set->front_data = data;
    set->front_generation = generation;
    set->stats.front_readbacks++;
    return true;
}

live_target_refusal live_vk_target_present_submit_at(live_vk_target_set *set, const d3d8_frame_record *record,
                                                     uint64_t media_time_ns)
{
    if (set == NULL || record == NULL) {
        return LIVE_TARGET_REFUSE_ARGUMENT;
    }
    if (live_vk_target_register(set, record->data, record->format_word, record->size_word, NULL) != LIVE_VK_OK) {
        set->stats.present_refused++;
        return LIVE_TARGET_REFUSE_PRESENT_UNREGISTERABLE;
    }
    const target_slot *slot = NULL;
    if (!read_front(set, record->data, &slot)) {
        set->stats.present_refused++;
        return LIVE_TARGET_REFUSE_PRESENT_UNREGISTERABLE;
    }
    const size_t bytes = image_bytes(slot);
    uint8_t *pixels = malloc(bytes);
    if (pixels == NULL) {
        set->stats.present_refused++;
        return LIVE_TARGET_REFUSE_PRESENT_UNREGISTERABLE;
    }
    memcpy(pixels, set->front_bgra, bytes);
    if (set->schedule.count == LIVE_PRESENT_QUEUE && set->media_count != 0u) {
        media_queue_drop_oldest(set);
    }
    const live_present_event event = {.number = record->number,
                                      .interval = record->interval,
                                      .vblank = record->vblank,
                                      .media_time_ns = media_time_ns,
                                      .data = record->data,
                                      .format_word = record->format_word,
                                      .size_word = record->size_word};
    const live_target_refusal result = live_present_submit_media(&set->schedule, set->registry, &event);
    if (result != LIVE_TARGET_OK) {
        free(pixels);
        set->stats.present_refused++;
        return result;
    }
    set->media_queue[set->media_count++] = (media_front_snapshot){.number = record->number,
                                                                 .data = record->data,
                                                                 .width = slot->desc.width,
                                                                 .height = slot->desc.height,
                                                                 .pitch = slot->desc.pitch,
                                                                 .bytes = bytes,
                                                                 .pixels = pixels};
    set->media_mode = true;
    set->stats.presents_submitted++;
    set->stats.media_snapshots++;
    return LIVE_TARGET_OK;
}

/* --- T849: the direct route --- */

void live_vk_target_set_direct(live_vk_target_set *set, live_vk_target_direct_fn direct, void *context)
{
    if (set != NULL) {
        set->direct = direct;
        set->direct_context = context;
    }
}

static void overlay_texture_release(live_vk_target_set *set)
{
    if (set->overlay_image != VK_NULL_HANDLE) {
        set->vkDestroyImage(set->device.device, set->overlay_image, NULL);
    }
    if (set->overlay_memory != VK_NULL_HANDLE) {
        set->vkFreeMemory(set->device.device, set->overlay_memory, NULL);
    }
    set->overlay_image = VK_NULL_HANDLE;
    set->overlay_memory = VK_NULL_HANDLE;
    set->overlay_uploaded = false;
}

/* The overlay picture as an RGBA8 image (alpha 0xFF) in layout GENERAL, uploaded once per submitted picture. Synchronous,
 * its own command buffer, so it is legal while the swapchain command buffer is being recorded. */
static bool overlay_texture_sync(live_vk_target_set *set)
{
    if (set->overlay_rgb == NULL || set->overlay_width == 0u || set->overlay_height == 0u) {
        return false;
    }
    if (set->overlay_uploaded && set->overlay_uploaded_sequence == set->overlay_sequence) {
        return true;
    }
    const bool fresh = set->overlay_image == VK_NULL_HANDLE || set->overlay_image_width != set->overlay_width ||
                       set->overlay_image_height != set->overlay_height;
    if (fresh) {
        (void)set->vkQueueWaitIdle(set->device.queue);
        overlay_texture_release(set);
        const VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {set->overlay_width, set->overlay_height, 1u}, .mipLevels = 1u,
            .arrayLayers = 1u, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        if (set->vkCreateImage(set->device.device, &info, NULL, &set->overlay_image) != VK_SUCCESS) {
            set->overlay_image = VK_NULL_HANDLE;
            return device_fail(set);
        }
        VkMemoryRequirements requirements;
        set->vkGetImageMemoryRequirements(set->device.device, set->overlay_image, &requirements);
        uint32_t type = memory_type(set, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) {
            type = memory_type(set, requirements.memoryTypeBits, 0u);
        }
        const VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size, .memoryTypeIndex = type};
        if (type == UINT32_MAX || set->vkAllocateMemory(set->device.device, &allocate, NULL, &set->overlay_memory) != VK_SUCCESS ||
            set->vkBindImageMemory(set->device.device, set->overlay_image, set->overlay_memory, 0u) != VK_SUCCESS) {
            overlay_texture_release(set);
            return device_fail(set);
        }
        set->overlay_image_width = set->overlay_width;
        set->overlay_image_height = set->overlay_height;
    }
    const size_t texels = (size_t)set->overlay_width * set->overlay_height;
    host_buffer staging;
    if (!host_buffer_make(set, &staging, texels * 4u)) {
        return false;
    }
    for (size_t i = 0u; i < texels; i++) {
        staging.mapped[i * 4u] = set->overlay_rgb[i * 3u];
        staging.mapped[i * 4u + 1u] = set->overlay_rgb[i * 3u + 1u];
        staging.mapped[i * 4u + 2u] = set->overlay_rgb[i * 3u + 2u];
        staging.mapped[i * 4u + 3u] = 0xFFu;
    }
    VkCommandBuffer command;
    if (!begin_commands(set, &command)) {
        host_buffer_free(set, &staging);
        return false;
    }
    const VkImageMemoryBarrier to_general = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0u,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = set->overlay_image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u}};
    set->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, NULL, 0u, NULL,
                              1u, &to_general);
    const VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
        .imageExtent = {set->overlay_width, set->overlay_height, 1u}};
    set->vkCmdCopyBufferToImage(command, staging.buffer, set->overlay_image, VK_IMAGE_LAYOUT_GENERAL, 1u, &copy);
    const bool ok = finish_commands(set, command);
    host_buffer_free(set, &staging);
    if (!ok) {
        return false;
    }
    set->overlay_uploaded = true;
    set->overlay_uploaded_sequence = set->overlay_sequence;
    set->stats.overlay_uploads++;
    return true;
}

static bool front_blittable(const live_vk_target_set *set, uint32_t data, const target_slot **slot_out)
{
    const target_slot *slot = find_slot(set, data);
    if (slot == NULL || !slot->has_image || slot->desc.bytes_per_pixel != 4u || slot->desc.swizzled) {
        return false;
    }
    *slot_out = slot;
    return true;
}

static VkImageBlit blit_region(uint32_t source_width, uint32_t source_height, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    const VkImageSubresourceLayers layers = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
    return (VkImageBlit){.srcSubresource = layers, .srcOffsets = {{0, 0, 0}, {(int32_t)source_width, (int32_t)source_height, 1}},
                         .dstSubresource = layers, .dstOffsets = {{x0, y0, 0}, {x1, y1, 1}}};
}

bool live_vk_target_record_frame(live_vk_target_set *set, VkCommandBuffer command, VkImage destination, VkExtent2D extent,
                                 const live_present_frame *frame)
{
    if (set == NULL || command == VK_NULL_HANDLE || destination == VK_NULL_HANDLE || frame == NULL || extent.width == 0u ||
        extent.height == 0u) {
        return false;
    }
    bool has_front = false, has_overlay = false;
    for (uint32_t i = 0u; i < frame->layer_count; i++) {
        has_front = has_front || frame->layers[i] == LIVE_LAYER_FRONT;
        has_overlay = has_overlay || frame->layers[i] == LIVE_LAYER_OVERLAY;
    }
    const target_slot *front = NULL;
    if (has_front && !front_blittable(set, frame->front_data, &front)) {
        return false;
    }
    if (has_overlay && !overlay_texture_sync(set)) {
        return false;
    }
    /* everything the earlier submissions wrote (draws, blits, the overlay upload) is visible to the transfer reads below */
    const VkMemoryBarrier visible = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    set->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 1u, &visible, 0u,
                              NULL, 0u, NULL);
    uint32_t canvas_width = set->overlay_width, canvas_height = set->overlay_height;
    if (has_front) {
        canvas_width = front->desc.width;
        canvas_height = front->desc.height;
        const VkImageBlit region = blit_region(canvas_width, canvas_height, 0, 0, (int32_t)extent.width, (int32_t)extent.height);
        set->vkCmdBlitImage(command, front->image, VK_IMAGE_LAYOUT_GENERAL, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u,
                            &region, VK_FILTER_NEAREST);
    } else {
        const VkClearColorValue black = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}};
        const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
        set->vkCmdClearColorImage(command, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1u, &range);
    }
    if (has_overlay) {
        if (!has_front) {
            canvas_width = set->overlay_width;
            canvas_height = set->overlay_height;
        }
        /* the same letterbox rectangle as live_vk_target_compose, in canvas texels, then scaled to the destination */
        uint32_t fit_width = canvas_width;
        uint32_t fit_height = (uint32_t)(((uint64_t)canvas_width * set->overlay_height) / set->overlay_width);
        if (fit_height > canvas_height) {
            fit_height = canvas_height;
            fit_width = (uint32_t)(((uint64_t)canvas_height * set->overlay_width) / set->overlay_height);
        }
        if (fit_width != 0u && fit_height != 0u) {
            const uint32_t left = (canvas_width - fit_width) / 2u;
            const uint32_t top = (canvas_height - fit_height) / 2u;
            const VkMemoryBarrier ordered = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
            set->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 1u, &ordered,
                                      0u, NULL, 0u, NULL);
            const VkImageBlit region = blit_region(
                set->overlay_width, set->overlay_height, (int32_t)(((uint64_t)left * extent.width) / canvas_width),
                (int32_t)(((uint64_t)top * extent.height) / canvas_height),
                (int32_t)(((uint64_t)(left + fit_width) * extent.width) / canvas_width),
                (int32_t)(((uint64_t)(top + fit_height) * extent.height) / canvas_height));
            set->vkCmdBlitImage(command, set->overlay_image, VK_IMAGE_LAYOUT_GENERAL, destination,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &region, VK_FILTER_NEAREST);
        }
    }
    return true;
}

typedef struct {
    live_vk_target_set *set;
    const live_present_frame *frame;
} direct_job;

static bool direct_hook(const gpu_window_native *native, VkCommandBuffer command, VkImage destination, VkExtent2D extent,
                        VkFormat format, void *context)
{
    (void)native;
    (void)format;
    direct_job *job = context;
    return live_vk_target_record_frame(job->set, command, destination, extent, job->frame);
}

/* The readback route: front read back, composed on the CPU into set->frame_rgba (`*width` x `*height`). */
static bool compose_readback(live_vk_target_set *set, bool has_front, uint32_t front_data, bool has_overlay, uint32_t *width,
                             uint32_t *height)
{
    const target_slot *front = NULL;
    if (has_front && !read_front(set, front_data, &front)) {
        has_front = false;
    }
    *width = 640u;
    *height = 480u;
    if (has_front) {
        *width = front->desc.width;
        *height = front->desc.height;
    } else if (has_overlay) {
        *width = set->overlay_width;
        *height = set->overlay_height;
    }
    if (!ensure(&set->frame_rgba, &set->frame_capacity, (size_t)*width * *height * 4u)) {
        return false;
    }
    live_vk_target_compose(has_front ? set->front_bgra : NULL, has_front ? front->desc.pitch : 0u,
                           has_overlay ? set->overlay_rgb : NULL, set->overlay_width, set->overlay_height, *width, *height,
                           set->frame_rgba);
    return true;
}

static bool compose_media_snapshot(live_vk_target_set *set, bool has_front, bool has_overlay, uint32_t *width,
                                   uint32_t *height);

bool live_vk_target_compose_last(live_vk_target_set *set, const uint8_t **rgba, uint32_t *width, uint32_t *height)
{
    if (set == NULL || !set->last_valid || rgba == NULL || width == NULL || height == NULL) {
        return false;
    }
    const bool composed = set->media_mode
                              ? compose_media_snapshot(set, set->last_has_front, set->last_has_overlay, width, height)
                              : compose_readback(set, set->last_has_front, set->last_front_data, set->last_has_overlay,
                                                 width, height);
    if (!composed) {
        return false;
    }
    *rgba = set->frame_rgba;
    return true;
}

static bool media_front_select(live_vk_target_set *set, const live_present_frame *frame)
{
    if (!frame->front_new) {
        return set->media_front.pixels != NULL;
    }
    size_t selected = 0u;
    while (selected < set->media_count && set->media_queue[selected].number != frame->front_number) {
        selected++;
    }
    if (selected == set->media_count) {
        return false;
    }
    media_snapshot_release(&set->media_front);
    for (size_t i = 0u; i < selected; i++) {
        media_snapshot_release(&set->media_queue[i]);
    }
    set->media_front = set->media_queue[selected];
    const uint32_t remaining = set->media_count - (uint32_t)selected - 1u;
    memmove(set->media_queue, &set->media_queue[selected + 1u], (size_t)remaining * sizeof set->media_queue[0]);
    memset(&set->media_queue[remaining], 0, (size_t)(LIVE_PRESENT_QUEUE - remaining) * sizeof set->media_queue[0]);
    set->media_count = remaining;
    set->stats.media_frames_released++;
    return true;
}

static bool compose_media_snapshot(live_vk_target_set *set, bool has_front, bool has_overlay, uint32_t *width,
                                   uint32_t *height)
{
    const media_front_snapshot *front = has_front ? &set->media_front : NULL;
    if (front != NULL && front->pixels == NULL) {
        front = NULL;
    }
    *width = 640u;
    *height = 480u;
    if (front != NULL) {
        *width = front->width;
        *height = front->height;
    } else if (has_overlay) {
        *width = set->overlay_width;
        *height = set->overlay_height;
    }
    if (!ensure(&set->frame_rgba, &set->frame_capacity, (size_t)*width * *height * 4u)) {
        return false;
    }
    live_vk_target_compose(front != NULL ? front->pixels : NULL, front != NULL ? front->pitch : 0u,
                           has_overlay ? set->overlay_rgb : NULL, set->overlay_width, set->overlay_height, *width,
                           *height, set->frame_rgba);
    return true;
}

static bool target_vblank(live_vk_target_set *set, uint64_t vblank, bool media_clock_valid, uint64_t media_time_ns,
                          live_present_frame *frame)
{
    live_present_frame local;
    live_present_frame *use = frame != NULL ? frame : &local;
    if (set == NULL) {
        return false;
    }
    const uint64_t started = now_ns();
    if (set->media_mode) {
        live_present_media_vblank(&set->schedule, vblank, set->overlay_active, media_clock_valid, media_time_ns, use);
        if (use->front_new && !media_front_select(set, use)) {
            /* An event without its owned snapshot is refused; retaining an older image is safe. */
            set->stats.front_unreadable++;
            use->front_new = false;
            if (set->media_front.pixels == NULL) {
                use->layer_count = set->overlay_active ? 1u : 0u;
                if (set->overlay_active) use->layers[0] = LIVE_LAYER_OVERLAY;
            } else {
                use->front_data = set->media_front.data;
                use->front_number = set->media_front.number;
                use->front_held = true;
            }
        }
    } else {
        live_present_vblank(&set->schedule, vblank, set->overlay_active, use);
    }
    set->stats.vblanks++;
    bool has_front = false, has_overlay = false;
    for (uint32_t i = 0u; i < use->layer_count; i++) {
        has_front = has_front || use->layers[i] == LIVE_LAYER_FRONT;
        has_overlay = has_overlay || use->layers[i] == LIVE_LAYER_OVERLAY;
    }
    set->last_valid = true;
    set->last_has_front = has_front;
    set->last_has_overlay = has_overlay;
    set->last_front_data = use->front_data;
    bool presented = false;
    const target_slot *probe = NULL;
    if (set->direct != NULL && !set->media_mode && (!has_front || front_blittable(set, use->front_data, &probe))) {
        direct_job job = {set, use};
        presented = set->direct(set->direct_context, direct_hook, &job);
        if (presented) {
            set->stats.blit_frames++;
        } else {
            set->stats.direct_failures++;
        }
    } else if (set->present != NULL) {
        if (set->direct != NULL) {
            if (set->media_mode) set->stats.media_direct_fallbacks++;
            else set->stats.direct_fallbacks++;
        }
        uint32_t width = 0u, height = 0u;
        const bool composed = set->media_mode
                                  ? compose_media_snapshot(set, has_front, has_overlay, &width, &height)
                                  : compose_readback(set, has_front, use->front_data, has_overlay, &width, &height);
        if (composed) {
            presented = set->present(set->present_context, set->frame_rgba, width, height, width * 4u);
            if (presented) {
                set->stats.readback_frames++;
            } else {
                set->stats.present_callback_failures++;
            }
        }
    }
    if (has_overlay) {
        set->stats.overlay_composed++;
    }
    if (!has_front && !has_overlay) {
        set->stats.black_frames++;
    }
    const uint64_t finished = now_ns();
    set->stats.vblank_ns += finished - started;
    if (finished - started > set->stats.vblank_ns_max) {
        set->stats.vblank_ns_max = finished - started;
    }
    if (presented) {
        if (set->stats.frames_presented == 0u) {
            set->stats.first_frame_ns = finished;
        }
        set->stats.last_frame_ns = finished;
        set->stats.frames_presented++;
    }
    return presented;
}

bool live_vk_target_vblank(live_vk_target_set *set, uint64_t vblank, live_present_frame *frame)
{
    return target_vblank(set, vblank, false, 0u, frame);
}

bool live_vk_target_media_vblank(live_vk_target_set *set, uint64_t vblank, bool media_clock_valid,
                                 uint64_t media_time_ns, live_present_frame *frame)
{
    if (set != NULL) {
        set->media_mode = true;
    }
    return target_vblank(set, vblank, media_clock_valid, media_time_ns, frame);
}
