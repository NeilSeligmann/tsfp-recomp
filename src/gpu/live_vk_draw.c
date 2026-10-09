/* SPDX-License-Identifier: GPL-3.0-or-later
 * T829 live draws into T793 target images. See live_vk_draw.h. */
#include "gpu_phase_timing.h"
#include "live_draw_dump.h"
#include "live_vk_draw.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DRAW_FUNCTIONS(X)                                                                                                \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory)               \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateFramebuffer) X(vkDestroyFramebuffer)       \
    X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer)                    \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdPipelineBarrier) X(vkCmdClearDepthStencilImage)                 \
    X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkCmdCopyImage)

#define DEPTH_FORMAT VK_FORMAT_D32_SFLOAT_S8_UINT
#define NO_RUN ((size_t)-1)
#define RETIRED_MAX 256u /* submitted run command buffers waiting for the next sync before they can be freed */

static bool g_sync_each_run; /* T1264: --live-present-sync, the old behaviour: submit and vkQueueWaitIdle after every run */

void live_vk_draw_set_sync_each_run(bool enable)
{
    g_sync_each_run = enable;
}

typedef struct {
    bool used;
    uint32_t data;
    uint32_t texture_id;
    uint32_t width, height;
    VkImageView colour_view; /* owned by the target set */
    VkImage depth;
    VkDeviceMemory depth_memory;
    VkImageView depth_view;
    VkFramebuffer framebuffer;
    uint64_t cleared_epoch;
    /* T1206: the feedback snapshot, a copy of the colour image sampled instead of the target a draw renders into */
    VkImage snapshot;
    VkDeviceMemory snapshot_memory;
    VkImageView snapshot_view;
    uint64_t snapshot_epoch;
    size_t snapshot_draw;
    bool snapshot_valid;
} draw_entry;

struct live_vk_draw {
    live_vk_device device;
#define DECLARE(name) PFN_##name name;
    DRAW_FUNCTIONS(DECLARE)
#undef DECLARE
    live_vk_renderer *renderer;
    live_vk_target_set *targets;
    VkRenderPass pass;
    draw_entry entries[LIVE_TARGET_MAX_TARGETS];
    uint64_t epoch;
    size_t open;
    VkCommandBuffer command;
    VkCommandBuffer retired[RETIRED_MAX]; /* T1264: submitted, not yet known complete (no vkFreeCommandBuffers while in flight) */
    size_t retired_count;
    uint32_t current_id;
    size_t current_draw;
    live_vk_draw_stats stats;
};

static void set_reason(char *out, size_t bytes, const char *format, ...) __attribute__((format(printf, 3, 4)));
static void set_reason(char *out, size_t bytes, const char *format, ...)
{
    if (out == NULL || bytes == 0u) {
        return;
    }
    va_list args;
    va_start(args, format);
    (void)vsnprintf(out, bytes, format, args);
    va_end(args);
}

static bool find_memory(const live_vk_draw *draw, uint32_t bits, VkMemoryPropertyFlags flags, uint32_t *index)
{
    for (uint32_t i = 0u; i < draw->device.memory_properties.memoryTypeCount; i++) {
        if ((bits & (1u << i)) != 0u && (draw->device.memory_properties.memoryTypes[i].propertyFlags & flags) == flags) {
            *index = i;
            return true;
        }
    }
    return false;
}

static void entry_free(live_vk_draw *draw, draw_entry *entry)
{
    const VkDevice device = draw->device.device;
    if (entry->framebuffer != VK_NULL_HANDLE) {
        draw->vkDestroyFramebuffer(device, entry->framebuffer, NULL);
    }
    if (entry->snapshot_view != VK_NULL_HANDLE) {
        draw->vkDestroyImageView(device, entry->snapshot_view, NULL);
    }
    if (entry->snapshot != VK_NULL_HANDLE) {
        draw->vkDestroyImage(device, entry->snapshot, NULL);
    }
    if (entry->snapshot_memory != VK_NULL_HANDLE) {
        draw->vkFreeMemory(device, entry->snapshot_memory, NULL);
    }
    if (entry->depth_view != VK_NULL_HANDLE) {
        draw->vkDestroyImageView(device, entry->depth_view, NULL);
    }
    if (entry->depth != VK_NULL_HANDLE) {
        draw->vkDestroyImage(device, entry->depth, NULL);
    }
    if (entry->depth_memory != VK_NULL_HANDLE) {
        draw->vkFreeMemory(device, entry->depth_memory, NULL);
    }
    memset(entry, 0, sizeof *entry);
}

static bool entry_build(live_vk_draw *draw, draw_entry *entry, uint32_t data)
{
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {0u, 0u};
    uint64_t generation = 0u;
    const VkImage image = live_vk_target_image(draw->targets, data, &format, &extent, &generation);
    entry->texture_id = live_vk_target_texture_id(draw->targets, data);
    entry->colour_view = live_vk_target_texture_view(draw->targets, entry->texture_id);
    if (image == VK_NULL_HANDLE || format != VK_FORMAT_B8G8R8A8_UNORM || entry->colour_view == VK_NULL_HANDLE ||
        extent.width == 0u || extent.height == 0u) {
        return false;
    }
    entry->data = data;
    entry->width = extent.width;
    entry->height = extent.height;
    const VkDevice device = draw->device.device;
    const VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = DEPTH_FORMAT, .extent = {extent.width, extent.height, 1u}, .mipLevels = 1u, .arrayLayers = 1u,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkMemoryRequirements requirements;
    uint32_t type = 0u;
    bool ok = draw->vkCreateImage(device, &info, NULL, &entry->depth) == VK_SUCCESS;
    if (ok) {
        draw->vkGetImageMemoryRequirements(device, entry->depth, &requirements);
        ok = find_memory(draw, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) ||
             find_memory(draw, requirements.memoryTypeBits, 0u, &type);
    }
    if (ok) {
        const VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                                 .allocationSize = requirements.size, .memoryTypeIndex = type};
        ok = draw->vkAllocateMemory(device, &allocation, NULL, &entry->depth_memory) == VK_SUCCESS &&
             draw->vkBindImageMemory(device, entry->depth, entry->depth_memory, 0u) == VK_SUCCESS;
    }
    if (ok) {
        const VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = entry->depth,
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = DEPTH_FORMAT,
            .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0u, 1u, 0u, 1u}};
        ok = draw->vkCreateImageView(device, &view, NULL, &entry->depth_view) == VK_SUCCESS;
    }
    if (ok) {
        const VkImageView views[2] = {entry->colour_view, entry->depth_view};
        const VkFramebufferCreateInfo framebuffer = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = draw->pass, .attachmentCount = 2u, .pAttachments = views, .width = extent.width,
            .height = extent.height, .layers = 1u};
        ok = draw->vkCreateFramebuffer(device, &framebuffer, NULL, &entry->framebuffer) == VK_SUCCESS;
    }
    if (!ok) {
        draw->stats.device_failures++;
        entry_free(draw, entry);
    } else {
        entry->used = true;
    }
    return ok;
}

/* Which target and why not. `*index` is the entry, built on first use. */
typedef struct {
    uint32_t format, pitch, colour_offset; /* 0x0208, 0x020C, 0x0210 */
    bool written;                          /* all three were written */
} surface_words;

static surface_words words_of_state(const gpu_pgraph_state *state)
{
    return (surface_words){state->output[GPU_PGRAPH_OUT_SURFACE_FORMAT], state->output[GPU_PGRAPH_OUT_SURFACE_PITCH],
                           state->output[GPU_PGRAPH_OUT_SURFACE_COLOR_OFFSET],
                           state->output_written[GPU_PGRAPH_OUT_SURFACE_FORMAT] &&
                               state->output_written[GPU_PGRAPH_OUT_SURFACE_PITCH] &&
                               state->output_written[GPU_PGRAPH_OUT_SURFACE_COLOR_OFFSET]};
}

static bool resolve_target(live_vk_draw *draw, const surface_words *words, size_t *index, char *reason, size_t reason_bytes)
{
    live_draw_dump_line("S format=%08x pitch=%08x data=%07x written=%d", (unsigned)words->format, (unsigned)words->pitch,
                        (unsigned)(words->colour_offset & 0x0FFFFFFFu), (int)words->written); /* T1489: the target a draw or clear names (TSFP_LIVE_DRAW_DUMP) */
    if (!words->written) {
        set_reason(reason, reason_bytes,
                   "the draw's render target is unknown: the surface format, pitch or colour offset (0x0208, 0x020C, 0x0210) was never written");
        return false;
    }
    const uint32_t format = words->format;
    const uint32_t colour = format & 0xFu;
    if (colour != 8u) {
        set_reason(reason, reason_bytes,
                   "surface colour format %u (0x0208 = 0x%08X) is not A8R8G8B8 (8), the one colour format the output state decode accepts",
                   (unsigned)colour, (unsigned)format);
        return false;
    }
    const uint32_t type = (format >> 8) & 0xFu;
    if ((type != 1u && type != 2u) || ((format >> 4) & 0xFu) > 2u || ((format >> 4) & 0xFu) == 1u) {
        set_reason(reason, reason_bytes,
                   "surface format 0x%08X is not the pitch (type 1) or swizzled (type 2) layout with zeta 0 or Z24S8 (2): another layout or zeta is not drawn live",
                   (unsigned)format);
        return false;
    }
    const uint32_t data = words->colour_offset & 0x0FFFFFFFu;
    const live_target_desc *desc = live_target_find(live_vk_target_registry(draw->targets), data);
    if (desc == NULL) {
        set_reason(reason, reason_bytes, "the draw's render target 0x%07X (surface colour offset 0x0210) is not a registered target with an image",
                   (unsigned)data);
        return false;
    }
    /* T1489: a swizzled type (2) draws into the registered swizzled target of the same log2 size, in picture order (the image is linear,
     * xemu renders a swizzled surface into a linear GL texture and swizzles on download), a pitch type into a linear one. */
    if (desc->swizzled != (type == 2u)) {
        set_reason(reason, reason_bytes, "surface format 0x%08X is the %s layout but render target 0x%07X was registered %s", (unsigned)format,
                   type == 2u ? "swizzled" : "pitch", (unsigned)data, desc->swizzled ? "swizzled" : "with a pitch");
        return false;
    }
    const uint32_t exponent_u = (format >> 16) & 0xFFu, exponent_v = (format >> 24) & 0xFFu;
    if (type == 2u && (exponent_u > 15u || exponent_v > 15u || desc->width != (1u << exponent_u) || desc->height != (1u << exponent_v))) {
        set_reason(reason, reason_bytes, "swizzled surface format 0x%08X names log2 size %u x %u (bits 16 and 24) but render target 0x%07X is %ux%u",
                   (unsigned)format, (unsigned)exponent_u, (unsigned)exponent_v, (unsigned)data, (unsigned)desc->width, (unsigned)desc->height);
        return false;
    }
    if ((desc->format != LIVE_TARGET_FORMAT_A8R8G8B8 && desc->format != LIVE_TARGET_FORMAT_X8R8G8B8)) {
        set_reason(reason, reason_bytes, "render target 0x%07X is registered as %u byte%s per pixel%s, not a linear BGRA8 target", (unsigned)data,
                   (unsigned)desc->bytes_per_pixel, desc->bytes_per_pixel == 1u ? "" : "s", desc->swizzled ? " swizzled" : "");
        return false;
    }
    const uint32_t pitch = words->pitch & 0xFFFFu;
    if (type == 1u && pitch != desc->pitch) {
        set_reason(reason, reason_bytes, "surface pitch %u (0x020C) is not the pitch %u render target 0x%07X was registered with "
                   "(T848: surface format 0x%08X, zeta pitch %u)",
                   (unsigned)pitch, (unsigned)desc->pitch, (unsigned)data, (unsigned)format, (unsigned)(words->pitch >> 16));
        return false;
    }
    for (size_t i = 0u; i < LIVE_TARGET_MAX_TARGETS; i++) {
        if (draw->entries[i].used && draw->entries[i].data == data) {
            *index = i;
            return true;
        }
    }
    for (size_t i = 0u; i < LIVE_TARGET_MAX_TARGETS; i++) {
        if (!draw->entries[i].used) {
            if (!entry_build(draw, &draw->entries[i], data)) {
                set_reason(reason, reason_bytes, "the draw target image resources of render target 0x%07X could not be created", (unsigned)data);
                return false;
            }
            *index = i;
            return true;
        }
    }
    set_reason(reason, reason_bytes, "no free draw target entry");
    return false;
}

static bool open_run(live_vk_draw *draw, size_t index)
{
    draw_entry *entry = &draw->entries[index];
    const VkCommandBufferAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = draw->device.command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1u};
    if (draw->vkAllocateCommandBuffers(draw->device.device, &allocation, &draw->command) != VK_SUCCESS) {
        draw->command = VK_NULL_HANDLE;
        draw->stats.device_failures++;
        return false;
    }
    const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (draw->vkBeginCommandBuffer(draw->command, &begin) != VK_SUCCESS) {
        draw->vkFreeCommandBuffers(draw->device.device, draw->device.command_pool, 1u, &draw->command);
        draw->command = VK_NULL_HANDLE;
        draw->stats.device_failures++;
        return false;
    }
    const VkMemoryBarrier everything = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    draw->vkCmdPipelineBarrier(draw->command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 1u,
                               &everything, 0u, NULL, 0u, NULL); /* T1264: runs are not waited for one by one */
    if (entry->cleared_epoch != draw->epoch) {
        const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0u, 1u, 0u, 1u};
        VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = entry->depth, .subresourceRange = range};
        draw->vkCmdPipelineBarrier(draw->command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, NULL,
                                   0u, NULL, 1u, &barrier);
        const VkClearDepthStencilValue depth = {1.0f, 0u}; /* the replay's pass starts here */
        draw->vkCmdClearDepthStencilImage(draw->command, entry->depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &depth, 1u, &range);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        draw->vkCmdPipelineBarrier(draw->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0u, 0u,
                                   NULL, 0u, NULL, 1u, &barrier);
        entry->cleared_epoch = draw->epoch;
        draw->stats.depth_clears++;
    }
    const VkRenderPassBeginInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = draw->pass,
        .framebuffer = entry->framebuffer, .renderArea = {{0, 0}, {entry->width, entry->height}}};
    draw->vkCmdBeginRenderPass(draw->command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    draw->open = index;
    return true;
}

/* T1264: wait for every submitted run, then free their command buffers. A no-op when nothing is in flight. */
bool live_vk_draw_sync(live_vk_draw *draw)
{
    if (draw == NULL || draw->retired_count == 0u) {
        return true;
    }
    const uint64_t phase_start = gpu_phase_now();
    const bool ok = draw->vkQueueWaitIdle(draw->device.queue) == VK_SUCCESS;
    gpu_phase_add(GPU_PHASE_DRAW_SYNC, phase_start);
    draw->vkFreeCommandBuffers(draw->device.device, draw->device.command_pool, (uint32_t)draw->retired_count, draw->retired);
    draw->retired_count = 0u;
    if (!ok) {
        draw->stats.device_failures++;
    }
    return ok;
}

/* The run is submitted and NOT waited for: the queue executes in submission order, every run begins with a full memory barrier
 * (open_run) and the render pass external dependencies cover the target, so the next run sees its writes. What must not touch the
 * run's resources while it is in flight (the arena and the descriptor pool reset, a texture image rewrite, a command buffer free,
 * a host read of a target) waits first: live_vk_draw_sync from the frame start, the texture upload barrier (ALL_COMMANDS source),
 * and every synchronous submit that ends in vkQueueWaitIdle (which also retires these runs). */
bool live_vk_draw_flush(live_vk_draw *draw)
{
    if (draw == NULL || draw->open == NO_RUN) {
        return true;
    }
    const draw_entry *entry = &draw->entries[draw->open];
    const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1u, .pCommandBuffers = &draw->command};
    draw->vkCmdEndRenderPass(draw->command);
    const uint64_t phase_start = gpu_phase_now();
    bool ok = draw->vkEndCommandBuffer(draw->command) == VK_SUCCESS &&
              draw->vkQueueSubmit(draw->device.queue, 1u, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
    if (ok && (g_sync_each_run || draw->retired_count == RETIRED_MAX)) {
        draw->retired[draw->retired_count++] = draw->command;
        gpu_phase_add(GPU_PHASE_DRAW_FLUSH, phase_start);
        ok = live_vk_draw_sync(draw);
    } else if (ok) {
        draw->retired[draw->retired_count++] = draw->command;
        gpu_phase_add(GPU_PHASE_DRAW_FLUSH, phase_start);
    } else {
        gpu_phase_add(GPU_PHASE_DRAW_FLUSH, phase_start);
        (void)draw->vkQueueWaitIdle(draw->device.queue);
        draw->vkFreeCommandBuffers(draw->device.device, draw->device.command_pool, 1u, &draw->command);
    }
    draw->command = VK_NULL_HANDLE;
    live_vk_target_note_written(draw->targets, entry->data);
    draw->open = NO_RUN;
    draw->stats.runs++;
    if (!ok) {
        draw->stats.device_failures++;
    }
    return ok;
}

/* Select the target the surface words of `state` name and open (or keep) its run. */
static bool begin_run(live_vk_draw *draw, size_t draw_index, const surface_words *words, live_vk_draw_target *out, char *error,
                      size_t error_bytes)
{
    size_t index = 0u;
    if (!resolve_target(draw, words, &index, error, error_bytes)) {
        draw->stats.refused++;
        return false;
    }
    if (draw->open != index) {
        if (!live_vk_draw_flush(draw) || !open_run(draw, index)) {
            set_reason(error, error_bytes, "the draw target command buffer of render target 0x%07X could not be recorded or submitted",
                       (unsigned)draw->entries[index].data);
            return false;
        }
    }
    draw->current_id = draw->entries[index].texture_id;
    draw->current_draw = draw_index;
    *out = (live_vk_draw_target){draw->command, LIVE_VK_PASS_TARGET, draw->entries[index].width, draw->entries[index].height};
    return true;
}

static bool begin_draw(void *context, size_t draw_index, const gpu_pgraph_state *state, live_vk_draw_target *out, char *error,
                       size_t error_bytes)
{
    live_vk_draw *draw = context;
    const surface_words words = words_of_state(state);
    if (!begin_run(draw, draw_index, &words, out, error, error_bytes)) {
        return false;
    }
    draw->stats.draws++;
    return true;
}

bool live_vk_draw_apply_clear(live_vk_draw *draw, const gpu_pgraph *model, size_t clear_index, char *error, size_t error_bytes)
{
    if (draw == NULL || model == NULL || clear_index >= gpu_pgraph_clear_count(model)) {
        set_reason(error, error_bytes, "live_vk_draw_apply_clear: missing argument or a clear outside the list");
        return false;
    }
    /* T848: the clear names its own render target, the surface words as they stood at the CLEAR_SURFACE (not a draw's snapshot:
     * a frame with no draw has none, and a SetRenderTarget between the clear and the next draw would name the wrong one). */
    const gpu_pgraph_clear *event = gpu_pgraph_clear_at(model, clear_index);
    const surface_words words = {event->surface_format, event->surface_pitch, event->surface_color_offset, event->surface_written};
    live_vk_draw_target target;
    if (!begin_run(draw, SIZE_MAX, &words, &target, error, error_bytes)) {
        draw->stats.clears_refused++;
        return false;
    }
    if (!live_vk_renderer_clear(draw->renderer, model, clear_index, &target, error, error_bytes)) {
        draw->stats.clears_refused++;
        return false;
    }
    draw->stats.clears++;
    return true;
}

VkCommandBuffer live_vk_draw_current_command(const live_vk_draw *draw)
{
    return draw != NULL && draw->open != NO_RUN ? draw->command : VK_NULL_HANDLE;
}

static bool snapshot_build(live_vk_draw *draw, draw_entry *entry)
{
    const VkDevice device = draw->device.device;
    const VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM, .extent = {entry->width, entry->height, 1u}, .mipLevels = 1u, .arrayLayers = 1u,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkMemoryRequirements requirements;
    uint32_t type = 0u;
    bool ok = draw->vkCreateImage(device, &info, NULL, &entry->snapshot) == VK_SUCCESS;
    if (ok) {
        draw->vkGetImageMemoryRequirements(device, entry->snapshot, &requirements);
        ok = find_memory(draw, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) ||
             find_memory(draw, requirements.memoryTypeBits, 0u, &type);
    }
    if (ok) {
        const VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                                 .allocationSize = requirements.size, .memoryTypeIndex = type};
        ok = draw->vkAllocateMemory(device, &allocation, NULL, &entry->snapshot_memory) == VK_SUCCESS &&
             draw->vkBindImageMemory(device, entry->snapshot, entry->snapshot_memory, 0u) == VK_SUCCESS;
    }
    if (ok) {
        const VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = entry->snapshot,
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u}};
        ok = draw->vkCreateImageView(device, &view, NULL, &entry->snapshot_view) == VK_SUCCESS;
    }
    return ok;
}

bool live_vk_draw_snapshot(live_vk_draw *draw, size_t draw_index, uint32_t texture_id, VkImageView *view, char *error,
                           size_t error_bytes)
{
    if (draw == NULL || view == NULL || draw->open == NO_RUN || draw->entries[draw->open].texture_id != texture_id) {
        set_reason(error, error_bytes, "the feedback snapshot: render target %u is not the target of the open draw run",
                   (unsigned)texture_id);
        return false;
    }
    const size_t index = draw->open;
    draw_entry *entry = &draw->entries[index];
    if (entry->snapshot_valid && entry->snapshot_epoch == draw->epoch && entry->snapshot_draw == draw_index) {
        *view = entry->snapshot_view; /* another stage of the same draw */
        return true;
    }
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {0u, 0u};
    uint64_t generation = 0u;
    const VkImage source = live_vk_target_image(draw->targets, entry->data, &format, &extent, &generation);
    if (source == VK_NULL_HANDLE || (entry->snapshot == VK_NULL_HANDLE && !snapshot_build(draw, entry))) {
        draw->stats.device_failures++;
        set_reason(error, error_bytes, "the feedback snapshot image of render target %u could not be created", (unsigned)texture_id);
        return false;
    }
    /* T1264: the copy is recorded into the open run's command buffer between two passes (no submit, no vkQueueWaitIdle: the old
     * path closed the run, submitted the copy alone and waited, twice the round trips per feedback draw). The pass loads colour and
     * depth (LOAD_OP_LOAD), so ending and beginning it again keeps both. */
    VkCommandBuffer command = draw->command;
    draw->vkCmdEndRenderPass(command);
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
    VkImageMemoryBarrier barrier[2] = {
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = source, .subresourceRange = range},
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = entry->snapshot, .subresourceRange = range}};
    draw->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, NULL, 0u, NULL,
                               2u, barrier);
    const VkImageCopy copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
                              .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
                              .extent = {entry->width, entry->height, 1u}};
    draw->vkCmdCopyImage(command, source, VK_IMAGE_LAYOUT_GENERAL, entry->snapshot, VK_IMAGE_LAYOUT_GENERAL, 1u, &copy);
    const VkImageMemoryBarrier after = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = entry->snapshot, .subresourceRange = range};
    draw->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 0u, NULL, 0u, NULL,
                               1u, &after);
    const VkRenderPassBeginInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = draw->pass,
        .framebuffer = entry->framebuffer, .renderArea = {{0, 0}, {entry->width, entry->height}}};
    draw->vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    entry->snapshot_valid = true;
    entry->snapshot_epoch = draw->epoch;
    entry->snapshot_draw = draw_index;
    draw->stats.snapshots++;
    *view = entry->snapshot_view;
    return true;
}

live_vk_target_hook live_vk_draw_hook(live_vk_draw *draw)
{
    return (live_vk_target_hook){begin_draw, NULL, draw};
}

void live_vk_draw_begin_frame(live_vk_draw *draw)
{
    (void)live_vk_draw_flush(draw);
    (void)live_vk_draw_sync(draw);
    draw->epoch++;
}

uint32_t live_vk_draw_current_texture_id(const live_vk_draw *draw)
{
    return draw->current_id;
}

live_vk_draw_stats live_vk_draw_stats_get(const live_vk_draw *draw)
{
    return draw->stats;
}

live_vk_draw *live_vk_draw_create(const live_vk_device *device, live_vk_renderer *renderer, live_vk_target_set *targets,
                                  char *error, size_t error_bytes)
{
    if (device == NULL || renderer == NULL || targets == NULL || device->device == VK_NULL_HANDLE ||
        device->get_device_proc_addr == NULL || live_vk_renderer_target_pass(renderer) == VK_NULL_HANDLE) {
        set_reason(error, error_bytes, "live_vk_draw_create: missing device, renderer, target set or target pass");
        return NULL;
    }
    live_vk_draw *draw = calloc(1u, sizeof *draw);
    if (draw == NULL) {
        set_reason(error, error_bytes, "out of memory");
        return NULL;
    }
    draw->device = *device;
    draw->renderer = renderer;
    draw->targets = targets;
    draw->pass = live_vk_renderer_target_pass(renderer);
    draw->open = NO_RUN;
    draw->epoch = 1u;
    bool ok = true;
#define LOAD(name)                                                                                           \
    {                                                                                                        \
        const PFN_vkVoidFunction raw = device->get_device_proc_addr(device->device, #name);                  \
        memcpy(&draw->name, &raw, sizeof draw->name);                                                        \
        ok = ok && draw->name != NULL;                                                                       \
    }
    DRAW_FUNCTIONS(LOAD)
#undef LOAD
    if (!ok) {
        set_reason(error, error_bytes, "a Vulkan device function the live target draws need is missing");
        free(draw);
        return NULL;
    }
    return draw;
}

void live_vk_draw_destroy(live_vk_draw *draw)
{
    if (draw == NULL) {
        return;
    }
    (void)live_vk_draw_flush(draw);
    (void)live_vk_draw_sync(draw);
    for (size_t i = 0u; i < LIVE_TARGET_MAX_TARGETS; i++) {
        if (draw->entries[i].used) {
            entry_free(draw, &draw->entries[i]);
        }
    }
    free(draw);
}
