/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T791 test rig shared by test_live_vk_pipeline.c and test_live_vk_bind.c: a minimal render target (image, framebuffer, command
 * buffer, readback) on a gpu_device, because the production target is T793's. render_live clears, begins a frame on the renderer,
 * draws every draw of a model into the window kind (colour only pass) or the offscreen kind (colour plus depth) and reads the
 * colour back. The includer defines CHECK and includes live_vk_scenes.h (WIDTH, HEIGHT) first.
 */
#ifndef TSFP_LIVE_VK_RIG_H
#define TSFP_LIVE_VK_RIG_H

#include "gpu_device.h"
#include "gpu_device_native.h"
#include "live_vk_pipeline.h"

/* --- a minimal render target -------------------------------------------------------------- */

#define TEST_FUNCS(X) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) \
    X(vkDestroyImageView) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateRenderPass) X(vkDestroyRenderPass) \
    X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdPipelineBarrier) X(vkCmdClearColorImage) \
    X(vkCmdClearDepthStencilImage) X(vkCmdCopyImageToBuffer) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkCreateBuffer) \
    X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) \
    X(vkMapMemory)

#define DECLARE(name) PFN_##name name;
typedef struct {
    TEST_FUNCS(DECLARE)
} test_functions;
#undef DECLARE

typedef struct {
    gpu_device_native native;
    live_vk_device device;
    test_functions fn;
    VkRenderPass colour_pass; /* the "swapchain" pass: colour only, RGBA8 */
} rig;

__attribute__((unused)) static bool find_memory(const rig *r, uint32_t bits, VkMemoryPropertyFlags flags, uint32_t *index)
{
    for (uint32_t i = 0u; i < r->native.memory_properties.memoryTypeCount; i++) {
        if ((bits & (1u << i)) != 0u && (r->native.memory_properties.memoryTypes[i].propertyFlags & flags) == flags) {
            *index = i;
            return true;
        }
    }
    return false;
}

__attribute__((unused)) static bool make_image(rig *r, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImage *image,
                       VkDeviceMemory *image_memory, VkImageView *view)
{
    const VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                    .format = format, .extent = {WIDTH, HEIGHT, 1u}, .mipLevels = 1u, .arrayLayers = 1u,
                                    .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (r->fn.vkCreateImage(r->native.device, &info, NULL, image) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements requirements;
    r->fn.vkGetImageMemoryRequirements(r->native.device, *image, &requirements);
    uint32_t type = 0u;
    if (!find_memory(r, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        !find_memory(r, requirements.memoryTypeBits, 0u, &type)) {
        return false;
    }
    const VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                             .allocationSize = requirements.size, .memoryTypeIndex = type};
    if (r->fn.vkAllocateMemory(r->native.device, &allocation, NULL, image_memory) != VK_SUCCESS ||
        r->fn.vkBindImageMemory(r->native.device, *image, *image_memory, 0u) != VK_SUCCESS) {
        return false;
    }
    const VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *image,
                                             .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
                                             .subresourceRange = {aspect, 0u, 1u, 0u, 1u}};
    return r->fn.vkCreateImageView(r->native.device, &view_info, NULL, view) == VK_SUCCESS;
}

__attribute__((unused)) static bool rig_init(rig *r, gpu_device *device)
{
    memset(r, 0, sizeof *r);
    gpu_device_get_native(device, &r->native);
    CHECK(live_vk_device_from_gpu_device(&r->native, &r->device));
#define LOAD(name)                                                                                  \
    r->fn.name = (PFN_##name)r->native.get_device_proc_addr(r->native.device, #name);               \
    if (r->fn.name == NULL) {                                                                       \
        return false;                                                                               \
    }
    TEST_FUNCS(LOAD)
#undef LOAD
    const VkAttachmentDescription attachment = {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference reference = {0u, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                          .colorAttachmentCount = 1u, .pColorAttachments = &reference};
    const VkRenderPassCreateInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1u,
                                         .pAttachments = &attachment, .subpassCount = 1u, .pSubpasses = &subpass};
    return r->fn.vkCreateRenderPass(r->native.device, &info, NULL, &r->colour_pass) == VK_SUCCESS;
}

/* One frame into a fresh WIDTH x HEIGHT target: clear, begin_frame, draws [0, count), read back. `kind` picks the pass. */
typedef struct {
    gpu_image image;
    uint32_t drawn, refused;
    uint32_t clears_applied, clears_refused; /* T828: CLEAR_SURFACE events, applied in order with the draws */
    char first_refusal[LIVE_PIPELINE_REASON_BYTES];
    char first_clear_refusal[LIVE_PIPELINE_REASON_BYTES];
} frame_result;

typedef struct {
    VkCommandBuffer command;
} offscreen_target;

__attribute__((unused)) static bool offscreen_begin(void *context, size_t draw, const gpu_pgraph_state *state, live_vk_draw_target *out,
                            char *error, size_t error_bytes)
{
    (void)draw;
    (void)state;
    (void)error;
    (void)error_bytes;
    *out = (live_vk_draw_target){((offscreen_target *)context)->command, LIVE_VK_PASS_OFFSCREEN, WIDTH, HEIGHT};
    return true;
}

__attribute__((unused)) static void render_live(rig *r, live_vk_renderer *renderer, live_vk_pass_kind kind, const gpu_pgraph *model,
                        const float clear[4], frame_result *out)
{
    memset(out, 0, sizeof *out);
    VkImage image = VK_NULL_HANDLE, depth_image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE, depth_memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE, depth_view = VK_NULL_HANDLE;
    const bool with_depth = kind == LIVE_VK_PASS_OFFSCREEN;
    CHECK(make_image(r, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, &image, &image_memory, &view));
    if (with_depth) {
        CHECK(make_image(r, VK_FORMAT_D32_SFLOAT_S8_UINT,
                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, &depth_image, &depth_memory, &depth_view));
    }
    const VkRenderPass pass = with_depth ? live_vk_renderer_offscreen_pass(renderer) : r->colour_pass;
    const VkImageView views[2] = {view, depth_view};
    const VkFramebufferCreateInfo framebuffer_info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = pass, .attachmentCount = with_depth ? 2u : 1u, .pAttachments = views, .width = WIDTH,
        .height = HEIGHT, .layers = 1u};
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    CHECK(r->fn.vkCreateFramebuffer(r->native.device, &framebuffer_info, NULL, &framebuffer) == VK_SUCCESS);
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory = VK_NULL_HANDLE;
    void *mapped = NULL;
    const VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = WIDTH * HEIGHT * 4u,
                                            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    CHECK(r->fn.vkCreateBuffer(r->native.device, &buffer_info, NULL, &readback) == VK_SUCCESS);
    VkMemoryRequirements requirements;
    r->fn.vkGetBufferMemoryRequirements(r->native.device, readback, &requirements);
    uint32_t type = 0u;
    CHECK(find_memory(r, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &type));
    const VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                             .allocationSize = requirements.size, .memoryTypeIndex = type};
    CHECK(r->fn.vkAllocateMemory(r->native.device, &allocation, NULL, &readback_memory) == VK_SUCCESS);
    CHECK(r->fn.vkBindBufferMemory(r->native.device, readback, readback_memory, 0u) == VK_SUCCESS);
    CHECK(r->fn.vkMapMemory(r->native.device, readback_memory, 0u, VK_WHOLE_SIZE, 0u, &mapped) == VK_SUCCESS);
    VkCommandBuffer command = VK_NULL_HANDLE;
    const VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = r->native.command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1u};
    CHECK(r->fn.vkAllocateCommandBuffers(r->native.device, &command_info, &command) == VK_SUCCESS);
    const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(r->fn.vkBeginCommandBuffer(command, &begin) == VK_SUCCESS);
    const VkImageSubresourceRange colour_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
    const VkImageSubresourceRange depth_range = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0u, 1u, 0u, 1u};
    VkImageMemoryBarrier to_clear[2];
    uint32_t barrier_count = 0u;
    to_clear[barrier_count++] = (VkImageMemoryBarrier){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image, .subresourceRange = colour_range};
    if (with_depth) {
        to_clear[barrier_count] = to_clear[0];
        to_clear[barrier_count].image = depth_image;
        to_clear[barrier_count].subresourceRange = depth_range;
        barrier_count++;
    }
    r->fn.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, NULL,
                               0u, NULL, barrier_count, to_clear);
    const VkClearColorValue colour = {.float32 = {clear[0], clear[1], clear[2], clear[3]}};
    r->fn.vkCmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1u, &colour_range);
    if (with_depth) {
        const VkClearDepthStencilValue depth = {1.0f, 0u}; /* the replay's depth and stencil start */
        r->fn.vkCmdClearDepthStencilImage(command, depth_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &depth, 1u,
                                          &depth_range);
    }
    VkImageMemoryBarrier to_attachment[2];
    to_attachment[0] = to_clear[0];
    to_attachment[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_attachment[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_attachment[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_attachment[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (with_depth) {
        to_attachment[1] = to_clear[1];
        to_attachment[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_attachment[1].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        to_attachment[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_attachment[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    r->fn.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                               0u, 0u, NULL, 0u, NULL, barrier_count, to_attachment);
    const VkRenderPassBeginInfo pass_begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = pass,
        .framebuffer = framebuffer, .renderArea = {{0, 0}, {WIDTH, HEIGHT}}};
    r->fn.vkCmdBeginRenderPass(command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    offscreen_target target = {command};
    const live_vk_target_hook offscreen_hook = {offscreen_begin, NULL, &target};
    if (with_depth) {
        live_vk_renderer_set_target_hook(renderer, &offscreen_hook);
    }
    live_vk_renderer_begin_frame(renderer);
    const size_t draws = gpu_pgraph_draw_count(model);
    const size_t clears = gpu_pgraph_clear_count(model);
    size_t next_clear = 0u;
    const live_vk_draw_target clear_target = {command, kind, WIDTH, HEIGHT};
    for (size_t index = 0u; index <= draws; index++) {
        /* the clears that precede draw `index` (the ones after the last draw at index == draws), the replay's order */
        while (next_clear < clears && (index == draws || gpu_pgraph_clear_at(model, next_clear)->before_draw <= index)) {
            char clear_error[LIVE_PIPELINE_REASON_BYTES] = "";
            if (live_vk_renderer_clear(renderer, model, next_clear++, &clear_target, clear_error, sizeof clear_error)) {
                out->clears_applied++;
            } else if (out->clears_refused++ == 0u) {
                snprintf(out->first_clear_refusal, sizeof out->first_clear_refusal, "%s", clear_error);
            }
        }
        if (index == draws) {
            break;
        }
        char error[LIVE_PIPELINE_REASON_BYTES] = "";
        if (live_vk_renderer_draw(renderer, model, index, command, WIDTH, HEIGHT, error, sizeof error)) {
            out->drawn++;
        } else {
            if (out->refused++ == 0u) {
                snprintf(out->first_refusal, sizeof out->first_refusal, "%s", error);
            }
        }
    }
    if (with_depth) {
        live_vk_renderer_set_target_hook(renderer, NULL);
    }
    r->fn.vkCmdEndRenderPass(command);
    VkImageMemoryBarrier to_copy = to_attachment[0];
    to_copy.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_copy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_copy.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    r->fn.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0u, 0u, NULL, 0u, NULL, 1u, &to_copy);
    const VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
                                    .imageExtent = {WIDTH, HEIGHT, 1u}};
    r->fn.vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1u, &copy);
    CHECK(r->fn.vkEndCommandBuffer(command) == VK_SUCCESS);
    const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1u, .pCommandBuffers = &command};
    CHECK(r->fn.vkQueueSubmit(r->native.queue, 1u, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
    CHECK(r->fn.vkQueueWaitIdle(r->native.queue) == VK_SUCCESS);
    out->image.width = WIDTH;
    out->image.height = HEIGHT;
    out->image.stride_bytes = WIDTH * 4u;
    out->image.pixels = malloc(WIDTH * HEIGHT * 4u);
    if (out->image.pixels != NULL && mapped != NULL) {
        memcpy(out->image.pixels, mapped, WIDTH * HEIGHT * 4u);
    }
    r->fn.vkFreeCommandBuffers(r->native.device, r->native.command_pool, 1u, &command);
    r->fn.vkDestroyBuffer(r->native.device, readback, NULL);
    r->fn.vkFreeMemory(r->native.device, readback_memory, NULL);
    r->fn.vkDestroyFramebuffer(r->native.device, framebuffer, NULL);
    r->fn.vkDestroyImageView(r->native.device, view, NULL);
    r->fn.vkDestroyImage(r->native.device, image, NULL);
    r->fn.vkFreeMemory(r->native.device, image_memory, NULL);
    if (with_depth) {
        r->fn.vkDestroyImageView(r->native.device, depth_view, NULL);
        r->fn.vkDestroyImage(r->native.device, depth_image, NULL);
        r->fn.vkFreeMemory(r->native.device, depth_memory, NULL);
    }
}

__attribute__((unused)) static size_t count_differing(const gpu_image *a, const gpu_image *b)
{
    size_t different = 0u;
    for (size_t pixel = 0u; pixel < (size_t)WIDTH * HEIGHT; pixel++) {
        different += memcmp(a->pixels + pixel * 4u, b->pixels + pixel * 4u, 4u) != 0;
    }
    return different;
}

__attribute__((unused)) static size_t count_covered(const gpu_image *image, const float clear[4])
{
    uint8_t expected[4];
    for (int lane = 0; lane < 4; lane++) {
        expected[lane] = (uint8_t)(clear[lane] * 255.0f + 0.5f);
    }
    size_t covered = 0u;
    for (size_t pixel = 0u; pixel < (size_t)WIDTH * HEIGHT; pixel++) {
        covered += memcmp(image->pixels + pixel * 4u, expected, 4u) != 0;
    }
    return covered;
}

#endif
