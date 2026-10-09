/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T792 Vulkan half: live_vk_texture on a real device. Needs SDL3 with a Vulkan capable video driver (Xvfb + llvmpipe is
 * enough), exits 77 (SKIP) without one, or 1 when TSFP_TEST_GPU_WINDOW_REQUIRED is set. A probe pipeline (fullscreen triangle,
 * nearest sampling at texel centres) renders the bound texture into an offscreen RGBA8 image that is read back and compared
 * byte for byte with the CPU decode. `--png <path>` also writes a 256x256 frame of a sampled swizzled DXT1 picture. */
#include "live_vk_texture.h"
#include "live_texture_watch.h"
#include "gpu_png.h"
#include "live_texture_probe_spv.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

#define FORMAT(color, exponent_u, exponent_v) \
    (((uint32_t)(exponent_v) << 24) | ((uint32_t)(exponent_u) << 20) | 0x10000u | ((uint32_t)(color) << 8) | 0x29u)
#define LINEAR_SIZE(width, height, pitch) \
    ((uint32_t)((width) - 1u) | ((uint32_t)((height) - 1u) << 12) | ((uint32_t)((pitch) / 64u - 1u) << 24))
#define NEAREST_FILTER 0x01010000u /* min 1, mag 1 */

/* --- guest memory stand-in ----------------------------------------------------------------------------------------- */
#define GUEST_BASE 0x1000u
#define GUEST_BYTES 0x40000u
static uint8_t guest[GUEST_BYTES];
static bool reader_fails;

static bool reader(void *context, uint32_t address, void *out, size_t bytes)
{
    (void)context;
    if (reader_fails || address < GUEST_BASE || (uint64_t)address + bytes > (uint64_t)GUEST_BASE + GUEST_BYTES) return false;
    memcpy(out, guest + (address - GUEST_BASE), bytes);
    return true;
}

static uint32_t rng_state = 0x1234567u;
static uint32_t next_random(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

static void fill_random(uint8_t *bytes, size_t count)
{
    for (size_t index = 0u; index < count; index++) bytes[index] = (uint8_t)next_random();
}

/* --- Vulkan probe ------------------------------------------------------------------------------------------------- */
#define PROBE_FUNCTIONS(X) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateRenderPass) \
    X(vkDestroyRenderPass) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw) \
    X(vkCmdCopyImageToBuffer) X(vkQueueSubmit) X(vkQueueWaitIdle)

typedef struct {
    gpu_window_native native;
    VkPhysicalDeviceMemoryProperties memory;
#define X(name) PFN_##name name;
    PROBE_FUNCTIONS(X)
#undef X
    VkRenderPass render_pass;
    VkPipelineLayout layout;
    VkPipeline pipeline;
} probe;

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkFramebuffer framebuffer;
    uint32_t width, height;
} probe_image;

static uint32_t find_memory(const probe *p, uint32_t bits, VkMemoryPropertyFlags wanted)
{
    for (uint32_t index = 0u; index < p->memory.memoryTypeCount; index++)
        if ((bits & (1u << index)) != 0u && (p->memory.memoryTypes[index].propertyFlags & wanted) == wanted) return index;
    return UINT32_MAX;
}

static bool probe_init(probe *p, const gpu_window_native *native, PFN_vkGetInstanceProcAddr gipa, VkDescriptorSetLayout set_layout)
{
    memset(p, 0, sizeof *p);
    p->native = *native;
#define X(name) { PFN_vkVoidFunction fn = native->get_device_proc_addr(native->device, #name); memcpy(&p->name, &fn, sizeof p->name); if (!p->name) return false; }
    PROBE_FUNCTIONS(X)
#undef X
    PFN_vkVoidFunction fn = gipa(native->instance, "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties get_properties;
    memcpy(&get_properties, &fn, sizeof get_properties);
    get_properties(native->physical_device, &p->memory);
    const VkAttachmentDescription attachment = {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkAttachmentReference reference = {0u, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1u, .pColorAttachments = &reference};
    const VkSubpassDependency dependency = {.srcSubpass = 0u, .dstSubpass = VK_SUBPASS_EXTERNAL,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT};
    const VkRenderPassCreateInfo pass_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1u, .pAttachments = &attachment,
        .subpassCount = 1u, .pSubpasses = &subpass, .dependencyCount = 1u, .pDependencies = &dependency};
    if (p->vkCreateRenderPass(native->device, &pass_info, NULL, &p->render_pass) != VK_SUCCESS) return false;
    const VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1u, .pSetLayouts = &set_layout};
    if (p->vkCreatePipelineLayout(native->device, &layout_info, NULL, &p->layout) != VK_SUCCESS) return false;
    VkShaderModule modules[2];
    const VkShaderModuleCreateInfo vert_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof probe_vert_spv, .pCode = probe_vert_spv};
    const VkShaderModuleCreateInfo frag_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof probe_frag_spv, .pCode = probe_frag_spv};
    if (p->vkCreateShaderModule(native->device, &vert_info, NULL, &modules[0]) != VK_SUCCESS ||
        p->vkCreateShaderModule(native->device, &frag_info, NULL, &modules[1]) != VK_SUCCESS) return false;
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = modules[0], .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = modules[1], .pName = "main"}};
    const VkPipelineVertexInputStateCreateInfo vertex_input = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    const VkPipelineViewportStateCreateInfo viewport = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1u, .scissorCount = 1u};
    const VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
    const VkPipelineMultisampleStateCreateInfo multisample = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    const VkPipelineColorBlendAttachmentState blend_attachment = {.colorWriteMask = 0xFu};
    const VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1u, .pAttachments = &blend_attachment};
    const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2u, .pDynamicStates = dynamic_states};
    const VkGraphicsPipelineCreateInfo pipeline_info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2u, .pStages = stages,
        .pVertexInputState = &vertex_input, .pInputAssemblyState = &assembly, .pViewportState = &viewport, .pRasterizationState = &raster,
        .pMultisampleState = &multisample, .pColorBlendState = &blend, .pDynamicState = &dynamic, .layout = p->layout, .renderPass = p->render_pass};
    const VkResult created = p->vkCreateGraphicsPipelines(native->device, VK_NULL_HANDLE, 1u, &pipeline_info, NULL, &p->pipeline);
    p->vkDestroyShaderModule(native->device, modules[0], NULL);
    p->vkDestroyShaderModule(native->device, modules[1], NULL);
    return created == VK_SUCCESS;
}

static bool probe_image_create(probe *p, probe_image *out, uint32_t width, uint32_t height)
{
    memset(out, 0, sizeof *out);
    out->width = width;
    out->height = height;
    const VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {width, height, 1u}, .mipLevels = 1u, .arrayLayers = 1u, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkMemoryRequirements requirements;
    if (p->vkCreateImage(p->native.device, &info, NULL, &out->image) != VK_SUCCESS) return false;
    p->vkGetImageMemoryRequirements(p->native.device, out->image, &requirements);
    uint32_t type = find_memory(p, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) type = find_memory(p, requirements.memoryTypeBits, 0u);
    const VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size, .memoryTypeIndex = type};
    if (p->vkAllocateMemory(p->native.device, &allocate, NULL, &out->memory) != VK_SUCCESS ||
        p->vkBindImageMemory(p->native.device, out->image, out->memory, 0u) != VK_SUCCESS) return false;
    const VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = out->image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u}};
    if (p->vkCreateImageView(p->native.device, &view_info, NULL, &out->view) != VK_SUCCESS) return false;
    const VkFramebufferCreateInfo frame_info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = p->render_pass, .attachmentCount = 1u,
        .pAttachments = &out->view, .width = width, .height = height, .layers = 1u};
    return p->vkCreateFramebuffer(p->native.device, &frame_info, NULL, &out->framebuffer) == VK_SUCCESS;
}

static void probe_image_destroy(probe *p, probe_image *image)
{
    p->vkDestroyFramebuffer(p->native.device, image->framebuffer, NULL);
    p->vkDestroyImageView(p->native.device, image->view, NULL);
    p->vkDestroyImage(p->native.device, image->image, NULL);
    p->vkFreeMemory(p->native.device, image->memory, NULL);
}

/* Sample `descriptor` over the whole of `target` (cleared to magenta first, so an unwritten pixel cannot pass) and read back. */
static bool probe_render(probe *p, VkDescriptorSet descriptor, probe_image *target, uint8_t *readback)
{
    const VkDeviceSize bytes = (VkDeviceSize)target->width * target->height * 4u;
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *mapped;
    const VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkMemoryRequirements requirements;
    if (p->vkCreateBuffer(p->native.device, &buffer_info, NULL, &buffer) != VK_SUCCESS) return false;
    p->vkGetBufferMemoryRequirements(p->native.device, buffer, &requirements);
    const VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size,
        .memoryTypeIndex = find_memory(p, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    if (p->vkAllocateMemory(p->native.device, &allocate, NULL, &memory) != VK_SUCCESS ||
        p->vkBindBufferMemory(p->native.device, buffer, memory, 0u) != VK_SUCCESS ||
        p->vkMapMemory(p->native.device, memory, 0u, VK_WHOLE_SIZE, 0u, &mapped) != VK_SUCCESS) return false;
    VkCommandBuffer command;
    const VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = p->native.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1u};
    if (p->vkAllocateCommandBuffers(p->native.device, &command_info, &command) != VK_SUCCESS) return false;
    const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    const VkClearValue clear = {.color = {{1.0f, 0.0f, 1.0f, 1.0f}}};
    const VkRenderPassBeginInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = p->render_pass, .framebuffer = target->framebuffer,
        .renderArea = {{0, 0}, {target->width, target->height}}, .clearValueCount = 1u, .pClearValues = &clear};
    const VkViewport viewport = {0.0f, 0.0f, (float)target->width, (float)target->height, 0.0f, 1.0f};
    const VkRect2D scissor = {{0, 0}, {target->width, target->height}};
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
    VkImageMemoryBarrier to_source = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_READ_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = target->image, .subresourceRange = range};
    VkImageMemoryBarrier to_sampled = to_source;
    to_sampled.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_sampled.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_sampled.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_sampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u}, .imageExtent = {target->width, target->height, 1u}};
    if (p->vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) return false;
    p->vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    p->vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
    p->vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0u, 1u, &descriptor, 0u, NULL);
    p->vkCmdSetViewport(command, 0u, 1u, &viewport);
    p->vkCmdSetScissor(command, 0u, 1u, &scissor);
    p->vkCmdDraw(command, 3u, 1u, 0u, 0u);
    p->vkCmdEndRenderPass(command);
    p->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, NULL, 0u, NULL, 1u, &to_source);
    p->vkCmdCopyImageToBuffer(command, target->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1u, &region);
    p->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0u, 0u, NULL, 0u, NULL, 1u, &to_sampled);
    if (p->vkEndCommandBuffer(command) != VK_SUCCESS) return false;
    const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1u, .pCommandBuffers = &command};
    const bool ok = p->vkQueueSubmit(p->native.queue, 1u, &submit, VK_NULL_HANDLE) == VK_SUCCESS && p->vkQueueWaitIdle(p->native.queue) == VK_SUCCESS;
    if (ok) memcpy(readback, mapped, (size_t)bytes);
    p->vkFreeCommandBuffers(p->native.device, p->native.command_pool, 1u, &command);
    p->vkUnmapMemory(p->native.device, memory);
    p->vkDestroyBuffer(p->native.device, buffer, NULL);
    p->vkFreeMemory(p->native.device, memory, NULL);
    return ok;
}

static size_t distinct_colors(const uint8_t *rgba, size_t pixels)
{
    uint32_t seen[64];
    size_t count = 0u;
    for (size_t index = 0u; index < pixels && count < 64u; index++) {
        uint32_t value;
        memcpy(&value, rgba + index * 4u, 4u);
        bool known = false;
        for (size_t other = 0u; other < count; other++) known = known || seen[other] == value;
        if (!known) seen[count++] = value;
    }
    return count;
}


/* A picture for the eye: a diagonal gradient with a bright disc and a white marker top left, DXT1 encoded (swizzled, 64x64). */
static void picture_texel(uint32_t x, uint32_t y, uint8_t out[3])
{
    const int dx = (int)x - 40, dy = (int)y - 36;
    if (x < 8u && y < 8u) { out[0] = out[1] = out[2] = 255u; return; }
    if (dx * dx + dy * dy < 14 * 14) { out[0] = 255u; out[1] = (uint8_t)(200 - dy * 4); out[2] = 40u; return; }
    out[0] = (uint8_t)(x * 4u); out[1] = (uint8_t)(y * 4u); out[2] = (uint8_t)(255u - (x + y) * 2u);
}

static uint16_t pack565(const uint8_t rgb[3])
{
    return (uint16_t)(((rgb[0] >> 3) << 11) | ((rgb[1] >> 2) << 5) | (rgb[2] >> 3));
}

static void encode_picture_dxt1(uint8_t *out)
{
    for (uint32_t by = 0u; by < 16u; by++)
        for (uint32_t bx = 0u; bx < 16u; bx++) {
            uint8_t texels[16][3];
            uint32_t low = 0u, high = 0u, best_low = 1000u, best_high = 0u;
            for (uint32_t index = 0u; index < 16u; index++) {
                picture_texel(bx * 4u + (index & 3u), by * 4u + (index >> 2), texels[index]);
                const uint32_t luma = texels[index][0] + texels[index][1] + texels[index][2];
                if (luma < best_low) { best_low = luma; low = index; }
                if (luma >= best_high) { best_high = luma; high = index; }
            }
            uint16_t c0 = pack565(texels[high]), c1 = pack565(texels[low]);
            if (c0 == c1) c0 = (uint16_t)(c1 + 1u); /* keep the 4 colour mode */
            if (c0 < c1) { const uint16_t swap = c0; c0 = c1; c1 = swap; }
            uint8_t palette[4][3];
            const uint16_t ends[2] = {c0, c1};
            for (uint32_t end = 0u; end < 2u; end++) {
                palette[end][0] = (uint8_t)(((ends[end] >> 11) & 31u) * 255u / 31u);
                palette[end][1] = (uint8_t)(((ends[end] >> 5) & 63u) * 255u / 63u);
                palette[end][2] = (uint8_t)((ends[end] & 31u) * 255u / 31u);
            }
            for (uint32_t channel = 0u; channel < 3u; channel++) {
                palette[2][channel] = (uint8_t)((2u * palette[0][channel] + palette[1][channel]) / 3u);
                palette[3][channel] = (uint8_t)((palette[0][channel] + 2u * palette[1][channel]) / 3u);
            }
            uint32_t indices = 0u;
            for (uint32_t index = 0u; index < 16u; index++) {
                uint32_t best = 0u, best_error = UINT32_MAX;
                for (uint32_t entry = 0u; entry < 4u; entry++) {
                    uint32_t error = 0u;
                    for (uint32_t channel = 0u; channel < 3u; channel++) {
                        const int difference = (int)texels[index][channel] - (int)palette[entry][channel];
                        error += (uint32_t)(difference * difference);
                    }
                    if (error < best_error) { best_error = error; best = entry; }
                }
                indices |= best << (2u * index);
            }
            uint8_t *block = out + (size_t)live_texture_swizzle_offset(bx, by, 16u, 16u) * 8u;
            block[0] = (uint8_t)c0; block[1] = (uint8_t)(c0 >> 8); block[2] = (uint8_t)c1; block[3] = (uint8_t)(c1 >> 8);
            for (uint32_t byte = 0u; byte < 4u; byte++) block[4u + byte] = (uint8_t)(indices >> (8u * byte));
        }
}

static int unavailable(const char *why)
{
    const char *required = getenv("TSFP_TEST_GPU_WINDOW_REQUIRED");
    fprintf(stderr, "%s: %s\n", required != NULL ? "FAIL" : "SKIP", why != NULL ? why : "no detail");
    return required != NULL ? 1 : 77;
}

/* --- device free checks ------------------------------------------------------------------------------------------- */
static void test_sampler_decode(void)
{
    const live_vk_sampler_desc mips = live_vk_sampler_decode_mips(0x303u, 0x02061F80u, (2u << 18) | (5u << 6), 6u);
    CHECK(mips.ok && !mips.measured && mips.mipmap_mode == VK_SAMPLER_MIPMAP_MODE_LINEAR &&
          mips.min_lod == 2.0f && mips.max_lod == 5.0f && mips.lod_bias == -0.5f);
    CHECK(live_vk_sampler_decode_mips(0x303u, 0x01030100u, 255u << 6, 6u).max_lod == 5.0f);
    CHECK(live_vk_sampler_decode_mips(0x303u, 0x01030100u, 5u << 6, 1u).max_lod == 0.0f);
    CHECK(!live_vk_sampler_decode_mips(0x303u, 0x01030000u, (5u << 18) | (2u << 6), 6u).ok);
    CHECK(!live_vk_sampler_decode_mips(0x303u, 0x01030000u, 0u, 0u).ok);
    const live_vk_sampler_desc wrap = live_vk_sampler_decode(0x00000101u, 0x02062000u);
    CHECK(wrap.ok && wrap.measured && wrap.address_u == VK_SAMPLER_ADDRESS_MODE_REPEAT && wrap.address_v == VK_SAMPLER_ADDRESS_MODE_REPEAT);
    CHECK(wrap.min_filter == VK_FILTER_LINEAR && wrap.mag_filter == VK_FILTER_LINEAR);
    const live_vk_sampler_desc lod0_wrap = live_vk_sampler_decode(0x00000101u, 0x02022000u);
    CHECK(lod0_wrap.ok && !lod0_wrap.measured && lod0_wrap.min_filter == VK_FILTER_LINEAR && lod0_wrap.mag_filter == VK_FILTER_LINEAR);
    const live_vk_sampler_desc lod0_clamp = live_vk_sampler_decode(0x00000303u, 0x02022000u);
    CHECK(lod0_clamp.ok && !lod0_clamp.measured && lod0_clamp.address_u == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    const live_vk_sampler_desc clamp = live_vk_sampler_decode(0x00000303u, 0x02062000u);
    CHECK(clamp.ok && clamp.measured && clamp.address_u == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE && clamp.address_v == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    const live_vk_sampler_desc mixed = live_vk_sampler_decode(0x00000402u, NEAREST_FILTER);
    CHECK(mixed.ok && !mixed.measured && mixed.address_u == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT && mixed.address_v == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
    CHECK(mixed.min_filter == VK_FILTER_NEAREST && mixed.mag_filter == VK_FILTER_NEAREST);
    /* T1489 (the T1488 pairs): each axis has its own mode, as in xemu (GL_TEXTURE_WRAP_S/T from ADDRU 0x7, ADDRV 0x700) and in Vulkan (addressModeU/V) */
    const live_vk_sampler_desc wrap_clamp = live_vk_sampler_decode(0x00000301u, NEAREST_FILTER);
    CHECK(wrap_clamp.ok && wrap_clamp.address_u == VK_SAMPLER_ADDRESS_MODE_REPEAT && wrap_clamp.address_v == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    const live_vk_sampler_desc clamp_wrap = live_vk_sampler_decode(0x00000103u, NEAREST_FILTER);
    CHECK(clamp_wrap.ok && clamp_wrap.address_u == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE && clamp_wrap.address_v == VK_SAMPLER_ADDRESS_MODE_REPEAT);
    CHECK(!live_vk_sampler_decode(0x00000311u, NEAREST_FILTER).ok); /* a cylinder wrap bit (xemu ignores it) stays refused */
    CHECK(live_vk_sampler_decode(0x00000501u, 0x04040000u).address_v == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    CHECK(live_vk_sampler_decode(0x00000101u, 0x04040000u).mag_filter == VK_FILTER_LINEAR);
    CHECK(live_vk_sampler_decode(0x00000101u, 0x01030000u).min_filter == VK_FILTER_NEAREST); /* mip nearest keeps the base nearest */
    CHECK(live_vk_sampler_decode(0x00000101u, 0x01050000u).min_filter == VK_FILTER_NEAREST && live_vk_sampler_decode(0x00000101u, 0x01050000u).mag_filter == VK_FILTER_NEAREST);
    CHECK(live_vk_sampler_decode(0x00000101u, 0x01070000u).min_filter == VK_FILTER_LINEAR);
    const live_vk_sampler_desc unbound = live_vk_sampler_decode(0u, 0x02062000u);
    CHECK(!unbound.ok && unbound.refusal != NULL && strcmp(unbound.refusal, "unbound address word") == 0);
    CHECK(!live_vk_sampler_decode(0x00000601u, 0x02062000u).ok && !live_vk_sampler_decode(0x00000106u, 0x02062000u).ok);
    CHECK(!live_vk_sampler_decode(0x00010101u, 0x02062000u).ok); /* P / cylinder wrap bits */
    CHECK(strcmp(live_vk_sampler_decode(0x00000106u, 0x02062000u).refusal, "address mode") == 0);
    CHECK(!live_vk_sampler_decode(0x00000101u, 0x02002000u).ok && !live_vk_sampler_decode(0x00000101u, 0x02082000u).ok); /* min 0 and 8 */
    CHECK(strcmp(live_vk_sampler_decode(0x00000101u, 0x02002000u).refusal, "min filter") == 0);
    CHECK(!live_vk_sampler_decode(0x00000101u, 0x02062001u).ok && !live_vk_sampler_decode(0x00000101u, 0x02063000u).ok); /* LOD bias */
    CHECK(!live_vk_sampler_decode(0x00000101u, 0x02862000u).ok); /* min filter bit 7 */
    CHECK(!live_vk_sampler_decode(0x00000101u, 0x12062000u).ok); /* top nibble */
    CHECK(!live_vk_sampler_decode(0x00000101u, 0x00062000u).ok && !live_vk_sampler_decode(0x00000101u, 0x03062000u).ok);
    CHECK(strcmp(live_vk_sampler_decode(0x00000101u, 0x03062000u).refusal, "mag filter") == 0);
    CHECK(live_vk_sampler_decode(0x00000101u, 0x02062000u).measured);
    CHECK(!live_vk_sampler_decode(0x00000202u, 0x02062000u).measured);
    CHECK(!live_vk_sampler_decode(0x00000303u, 0x01062000u).measured);
}

int main(int argc, char **argv)
{
    const char *png_path = NULL;
    for (int index = 1; index + 1 < argc; index++) if (strcmp(argv[index], "--png") == 0) png_path = argv[index + 1];
    test_sampler_decode();

    if (!SDL_Init(SDL_INIT_VIDEO)) return unavailable(SDL_GetError());
    SDL_Window *window = SDL_CreateWindow("live-vk-texture-test", 96, 64, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (window == NULL) { const int result = unavailable(SDL_GetError()); SDL_Quit(); return result; }
    const char *error = NULL;
    gpu_window *renderer = gpu_window_create(window, &error);
    if (renderer == NULL) { const int result = unavailable(error); SDL_DestroyWindow(window); SDL_Quit(); return result; }
    gpu_window_native native;
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    CHECK(gpu_window_get_native(renderer, &native) && gipa != NULL);
    printf("device %s\n", gpu_window_device_name(renderer));

    live_vk_texture_set *set = live_vk_texture_create(&native, gipa, &error);
    CHECK(set != NULL);
    if (set == NULL) { fprintf(stderr, "create: %s\n", error); return 1; }
    CHECK(live_vk_texture_create(&native, NULL, &error) == NULL && error != NULL);
    probe p;
    CHECK(probe_init(&p, &native, gipa, live_vk_texture_set_layout(set)));
    live_texture_cache *cache = calloc(1u, sizeof *cache);
    live_texture_cache_init(cache, false);
    live_texture_watch_reset();

    /* 1. a swizzled DXT1 64x64 texture: upload, sample, byte for byte against the CPU decode */
    const uint32_t dxt_bytes = 16u * 16u * 8u;
    fill_random(guest, dxt_bytes);
    live_texture_binding dxt = {0u, FORMAT(0x0C, 6, 6), 0u, GUEST_BASE, 0u};
    live_texture_result result;
    live_texture_lookup(cache, &dxt, reader, NULL, &result);
    CHECK(result.source == LIVE_TEXTURE_SOURCE_GUEST && result.needs_upload && result.width == 64u && result.height == 64u);
    uint8_t *expected = malloc(64u * 64u * 4u);
    uint8_t *actual = malloc(64u * 64u * 4u);
    live_texture_decode_dxt1_square(guest, 64u, expected);
    CHECK(distinct_colors(expected, 64u * 64u) > 16u); /* a uniform expectation would pass a blank sample */
    live_vk_sampler_desc nearest = live_vk_sampler_decode(0x00000303u, NEAREST_FILTER);
    live_vk_texture_bound bound;
    CHECK(live_vk_texture_bind(set, cache, &result, &nearest, &bound, &error));
    CHECK(bound.uploaded && bound.width == 64u && bound.image != VK_NULL_HANDLE && bound.descriptor != VK_NULL_HANDLE);
    CHECK(live_vk_texture_get_stats(set).uploads == 1u && live_vk_texture_get_stats(set).upload_bytes == 64u * 64u * 4u);
    probe_image target_a;
    CHECK(probe_image_create(&p, &target_a, 64u, 64u));
    memset(actual, 0xCC, 64u * 64u * 4u);
    CHECK(probe_render(&p, bound.descriptor, &target_a, actual));
    CHECK(memcmp(actual, expected, 64u * 64u * 4u) == 0);
    CHECK(memcmp(actual, result.rgba, 64u * 64u * 4u) == 0);

    /* 2. a second lookup does not upload again (no needs_upload, no new copy) */
    live_texture_lookup(cache, &dxt, reader, NULL, &result);
    CHECK(result.source == LIVE_TEXTURE_SOURCE_GUEST && !result.needs_upload && result.upload_bytes == 0u);
    CHECK(live_vk_texture_bind(set, cache, &result, &nearest, &bound, &error) && !bound.uploaded);
    CHECK(live_vk_texture_get_stats(set).uploads == 1u && live_vk_texture_get_stats(set).descriptor_writes == 1u);
    const VkDescriptorSet dxt_descriptor = bound.descriptor;
    /* the same texture with another sampler state rewrites its descriptor (and only then) */
    live_vk_sampler_desc bilinear = live_vk_sampler_decode(0x00000303u, 0x02062000u);
    live_vk_texture_bound other;
    CHECK(live_vk_texture_bind(set, cache, &result, &bilinear, &other, &error) && other.sampler != bound.sampler && other.descriptor == bound.descriptor);
    CHECK(live_vk_texture_get_stats(set).descriptor_writes == 2u && live_vk_texture_get_stats(set).sampler_creates == 2u);
    CHECK(live_vk_texture_bind(set, cache, &result, &nearest, &bound, &error));
    CHECK(live_vk_texture_get_stats(set).descriptor_writes == 3u && live_vk_texture_get_stats(set).sampler_creates == 2u);

    /* 3. the guest rewrites the texture: note, drain, re-decode, re-upload, sample the new bytes */
    uint8_t *before = malloc(64u * 64u * 4u);
    memcpy(before, expected, 64u * 64u * 4u);
    fill_random(guest, dxt_bytes);
    live_texture_watch_note(GUEST_BASE + 8u, 4u);
    CHECK(live_texture_watch_drain(cache) == 1u);
    live_texture_lookup(cache, &dxt, reader, NULL, &result);
    CHECK(result.needs_upload && result.generation == 2u);
    live_texture_decode_dxt1_square(guest, 64u, expected);
    CHECK(memcmp(before, expected, 64u * 64u * 4u) != 0);
    CHECK(live_vk_texture_bind(set, cache, &result, &nearest, &bound, &error) && bound.uploaded);
    memset(actual, 0xCC, 64u * 64u * 4u);
    CHECK(probe_render(&p, bound.descriptor, &target_a, actual));
    CHECK(memcmp(actual, expected, 64u * 64u * 4u) == 0);
    CHECK(live_vk_texture_get_stats(set).uploads == 2u && live_vk_texture_get_stats(set).image_creates == 1u);
    live_texture_lookup(cache, &dxt, reader, NULL, &result);
    CHECK(!result.needs_upload);

    /* 4. a non square linear A8R8G8B8 32x16 texture (BGRA bytes become RGBA), a new sampler, a different image size */
    const uint32_t linear_data = GUEST_BASE + 0x8000u;
    fill_random(guest + 0x8000u, 128u * 16u);
    live_texture_binding linear = {0u, FORMAT(0x12, 0, 0), LINEAR_SIZE(32u, 16u, 128u), linear_data, 0u};
    live_texture_lookup(cache, &linear, reader, NULL, &result);
    CHECK(result.source == LIVE_TEXTURE_SOURCE_GUEST && result.width == 32u && result.height == 16u);
    uint8_t linear_expected[32u * 16u * 4u];
    for (uint32_t y = 0u; y < 16u; y++)
        for (uint32_t x = 0u; x < 32u; x++) {
            const uint8_t *source = guest + 0x8000u + y * 128u + x * 4u;
            uint8_t *out = linear_expected + (y * 32u + x) * 4u;
            out[0] = source[2]; out[1] = source[1]; out[2] = source[0]; out[3] = source[3];
        }
    live_vk_sampler_desc wrap_nearest = live_vk_sampler_decode(0x00000101u, NEAREST_FILTER);
    CHECK(live_vk_texture_bind(set, cache, &result, &wrap_nearest, &bound, &error) && bound.uploaded && bound.width == 32u && bound.height == 16u);
    probe_image target_b;
    CHECK(probe_image_create(&p, &target_b, 32u, 16u));
    uint8_t linear_actual[32u * 16u * 4u];
    CHECK(probe_render(&p, bound.descriptor, &target_b, linear_actual));
    CHECK(memcmp(linear_actual, linear_expected, sizeof linear_expected) == 0);
    CHECK(distinct_colors(linear_expected, 32u * 16u) > 16u);
    CHECK(live_vk_texture_get_stats(set).sampler_creates == 3u && live_vk_texture_get_stats(set).image_creates == 2u);
    /* the same two states again reuse their samplers */
    live_texture_lookup(cache, &linear, reader, NULL, &result);
    CHECK(!result.needs_upload);
    CHECK(live_vk_texture_bind(set, cache, &result, &wrap_nearest, &bound, &error) && !bound.uploaded);
    CHECK(live_vk_texture_get_stats(set).sampler_creates == 3u);

    /* 5. a render target texture is sampled in place: target_a (the frame sampled in step 3) bound as a linear A8R8G8B8 */
    live_texture_binding target_binding = {0u, FORMAT(0x12, 0, 0), LINEAR_SIZE(64u, 64u, 256u), 0x20000u, 0u};
    CHECK(live_texture_register_target(cache, 7u, 0x20000u, 64u, 64u, 256u));
    live_texture_lookup(cache, &target_binding, reader, NULL, &result);
    CHECK(result.source == LIVE_TEXTURE_SOURCE_TARGET && result.target_id == 7u);
    const live_vk_texture_stats before_target = live_vk_texture_get_stats(set);
    CHECK(!live_vk_texture_bind(set, cache, &result, &nearest, &bound, &error) && strcmp(error, "render target view not registered") == 0);
    CHECK(live_vk_texture_register_target(set, 7u, target_a.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    CHECK(live_vk_texture_bind(set, cache, &result, &nearest, &bound, &error) && bound.view == target_a.view && !bound.uploaded);
    CHECK(bound.descriptor != dxt_descriptor && bound.descriptor != VK_NULL_HANDLE);
    probe_image target_c;
    CHECK(probe_image_create(&p, &target_c, 64u, 64u));
    uint8_t *sampled_target = malloc(64u * 64u * 4u);
    CHECK(probe_render(&p, bound.descriptor, &target_c, sampled_target));
    CHECK(memcmp(sampled_target, expected, 64u * 64u * 4u) == 0);
    CHECK(live_vk_texture_get_stats(set).uploads == before_target.uploads && live_vk_texture_get_stats(set).image_creates == before_target.image_creates &&
          live_vk_texture_get_stats(set).target_binds == 1u);
    /* registering an id again updates its slot in place (16 slots, the 17th id is refused) */
    probe_image target_d;
    CHECK(probe_image_create(&p, &target_d, 64u, 64u));
    CHECK(live_vk_texture_register_target(set, 7u, target_c.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    const uint64_t writes_before_view_change = live_vk_texture_get_stats(set).descriptor_writes;
    CHECK(live_vk_texture_bind(set, cache, &result, &nearest, &bound, &error) && bound.view == target_c.view);
    CHECK(live_vk_texture_get_stats(set).descriptor_writes == writes_before_view_change + 1u);
    for (uint32_t id = 100u; id < 115u; id++) CHECK(live_vk_texture_register_target(set, id, target_d.view, VK_IMAGE_LAYOUT_GENERAL));
    CHECK(!live_vk_texture_register_target(set, 115u, target_d.view, VK_IMAGE_LAYOUT_GENERAL));
    CHECK(!live_vk_texture_register_target(set, 7u, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL));
    live_vk_texture_clear_targets(set);
    CHECK(!live_vk_texture_bind(set, cache, &result, &nearest, &bound, &error));
    CHECK(live_vk_texture_register_target(set, 115u, target_d.view, VK_IMAGE_LAYOUT_GENERAL));
    live_vk_texture_clear_targets(set);

    /* 6. refusals stay named, nothing is bound for them, and they upload nothing */
    live_texture_binding inferred = {0u, FORMAT(0x11, 4, 4), 0u, GUEST_BASE, 0u};
    live_texture_lookup(cache, &inferred, reader, NULL, &result);
    CHECK(result.source == LIVE_TEXTURE_SOURCE_REFUSED);
    CHECK(!live_vk_texture_bind(set, cache, &result, &nearest, &bound, &error) && strcmp(error, "refused texture result") == 0);
    char line[400];
    CHECK(cache->census_count == 1u && live_texture_census_line(cache, 0u, line, sizeof line) && strstr(line, "inferred format") != NULL);
    live_vk_sampler_desc bad = live_vk_sampler_decode(0u, 0u);
    live_texture_lookup(cache, &dxt, reader, NULL, &result);
    CHECK(!live_vk_texture_bind(set, cache, &result, &bad, &bound, &error) && strcmp(error, "undecodable sampler state") == 0);

    /* 7. eviction: 64 more distinct 4x4 DXT1 bindings push the DXT1 64x64 entry out, looking it up again re-decodes into
     * whichever entry it lands in, and the image created there for another size is replaced */
    const uint32_t uploads_before = (uint32_t)live_vk_texture_get_stats(set).uploads;
    for (uint32_t index = 0u; index < LIVE_TEXTURE_CACHE_ENTRIES; index++) {
        live_texture_binding small = {0u, FORMAT(0x0C, 2, 2), 0u, GUEST_BASE + 0x10000u + index * 16u, 0u};
        live_texture_lookup(cache, &small, reader, NULL, &result);
        CHECK(result.source == LIVE_TEXTURE_SOURCE_GUEST && result.width == 4u);
        CHECK(live_vk_texture_bind(set, cache, &result, &nearest, &bound, &error) && bound.width == 4u && bound.height == 4u);
    }
    CHECK(live_vk_texture_get_stats(set).uploads - uploads_before == LIVE_TEXTURE_CACHE_ENTRIES);
    live_texture_lookup(cache, &dxt, reader, NULL, &result);
    CHECK(result.source == LIVE_TEXTURE_SOURCE_GUEST && result.needs_upload && result.width == 64u);
    CHECK(live_vk_texture_bind(set, cache, &result, &nearest, &bound, &error) && bound.uploaded);
    memset(actual, 0xCC, 64u * 64u * 4u);
    CHECK(probe_render(&p, bound.descriptor, &target_a, actual));
    CHECK(memcmp(actual, expected, 64u * 64u * 4u) == 0);

    /* 8. trim: an entry whose bytes cannot be re-read leaves the cache, its image goes with it */
    CHECK(live_vk_texture_trim(set, cache) == 0u);
    const uint64_t destroys_before = live_vk_texture_get_stats(set).image_destroys;
    live_texture_watch_note(GUEST_BASE, 8u);
    CHECK(live_texture_watch_drain(cache) == 1u);
    reader_fails = true;
    live_texture_lookup(cache, &dxt, reader, NULL, &result);
    reader_fails = false;
    CHECK(result.source == LIVE_TEXTURE_SOURCE_REFUSED && result.refusal != NULL && strcmp(result.refusal, "unreadable bytes") == 0);
    CHECK(live_vk_texture_trim(set, cache) == 1u);
    CHECK(live_vk_texture_get_stats(set).image_destroys == destroys_before + 1u);
    CHECK(live_vk_texture_trim(set, cache) == 0u);
    live_texture_lookup(cache, &dxt, reader, NULL, &result); /* it comes back with a fresh image */
    CHECK(result.source == LIVE_TEXTURE_SOURCE_GUEST && result.needs_upload);
    CHECK(live_vk_texture_bind(set, cache, &result, &nearest, &bound, &error) && bound.uploaded);
    memset(actual, 0xCC, 64u * 64u * 4u);
    CHECK(probe_render(&p, bound.descriptor, &target_a, actual));
    CHECK(memcmp(actual, expected, 64u * 64u * 4u) == 0);


    /* 8b. a second set on the same cache: the cache says the entry is uploaded, this set has no image yet and must make one */
    live_vk_texture_set *second_set = live_vk_texture_create(&native, gipa, &error);
    CHECK(second_set != NULL);
    live_texture_lookup(cache, &dxt, reader, NULL, &result);
    CHECK(result.source == LIVE_TEXTURE_SOURCE_GUEST && !result.needs_upload);
    CHECK(live_vk_texture_bind(second_set, cache, &result, &nearest, &bound, &error) && bound.uploaded);
    CHECK(live_vk_texture_get_stats(second_set).uploads == 1u && live_vk_texture_get_stats(second_set).image_creates == 1u);
    memset(actual, 0xCC, 64u * 64u * 4u);
    CHECK(probe_render(&p, bound.descriptor, &target_a, actual));
    CHECK(memcmp(actual, expected, 64u * 64u * 4u) == 0);
    live_vk_texture_destroy(second_set);
    live_vk_texture_destroy(NULL);

    /* 9. the picture: a real looking swizzled DXT1 sampled 4x larger (nearest) into a 256x256 frame, looked at as a PNG */
    encode_picture_dxt1(guest + 0x4000u);
    live_texture_binding picture = {0u, FORMAT(0x0C, 6, 6), 0u, GUEST_BASE + 0x4000u, 0u};
    live_texture_lookup(cache, &picture, reader, NULL, &result);
    CHECK(result.source == LIVE_TEXTURE_SOURCE_GUEST && result.needs_upload);
    uint8_t *picture_expected = malloc(64u * 64u * 4u);
    live_texture_decode_dxt1_square(guest + 0x4000u, 64u, picture_expected);
    CHECK(live_vk_texture_bind(set, cache, &result, &nearest, &bound, &error));
    probe_image target_big;
    CHECK(probe_image_create(&p, &target_big, 256u, 256u));
    uint8_t *big = malloc(256u * 256u * 4u);
    CHECK(probe_render(&p, bound.descriptor, &target_big, big));
    size_t big_mismatches = 0u;
    for (uint32_t y = 0u; y < 256u; y++)
        for (uint32_t x = 0u; x < 256u; x++)
            big_mismatches += memcmp(big + (y * 256u + x) * 4u, picture_expected + ((y / 4u) * 64u + x / 4u) * 4u, 4u) != 0;
    CHECK(big_mismatches == 0u);
    CHECK(distinct_colors(picture_expected, 64u * 64u) > 32u);
    CHECK(picture_expected[0] == 255u && picture_expected[1] == 255u && picture_expected[2] == 255u); /* white marker, row 0 on top */
    if (png_path != NULL) CHECK(gpu_png_write_rgba(png_path, big, 256u, 256u, 1024u));

    /* T919: actual minification (64 to16, positive implicit LOD) makes min=2 linear, not nearest.
     * With exactly one admitted mip level, min=6 has identical pixels: mip selection cannot invent
     * a second level. Random multicolour DXT pixels prevent a blank/uniform sampler from passing. */
    live_texture_lookup(cache, &dxt, reader, NULL, &result);
    CHECK(result.source == LIVE_TEXTURE_SOURCE_GUEST && result.width == 64u);
    probe_image minified;
    CHECK(probe_image_create(&p, &minified, 16u, 16u));
    uint8_t lod0_pixels[16u * 16u * 4u], mip_pixels[sizeof lod0_pixels], nearest_pixels[sizeof lod0_pixels];
    live_vk_sampler_desc lod0 = live_vk_sampler_decode(0x303u, 0x02022000u);
    CHECK(live_vk_texture_bind(set, cache, &result, &lod0, &bound, &error));
    CHECK(probe_render(&p, bound.descriptor, &minified, lod0_pixels));
    const VkSampler lod0_sampler = bound.sampler;
    CHECK(live_vk_texture_bind(set, cache, &result, &bilinear, &bound, &error));
    CHECK(bound.sampler == lod0_sampler); /* same effective single-level sampler */
    CHECK(probe_render(&p, bound.descriptor, &minified, mip_pixels));
    CHECK(memcmp(lod0_pixels, mip_pixels, sizeof lod0_pixels) == 0);
    CHECK(live_vk_texture_bind(set, cache, &result, &nearest, &bound, &error));
    CHECK(probe_render(&p, bound.descriptor, &minified, nearest_pixels));
    CHECK(memcmp(lod0_pixels, nearest_pixels, sizeof lod0_pixels) != 0);
    CHECK(distinct_colors(lod0_pixels, 16u * 16u) > 16u);
    uint32_t interpolation_errors = 0u;
    for (uint32_t y = 0u; y < 16u; y++) {
        for (uint32_t x = 0u; x < 16u; x++) {
            for (uint32_t channel = 0u; channel < 4u; channel++) {
                uint32_t sum = 0u;
                for (uint32_t dy = 1u; dy <= 2u; dy++)
                    for (uint32_t dx = 1u; dx <= 2u; dx++)
                        sum += expected[((4u * y + dy) * 64u + 4u * x + dx) * 4u + channel];
                const int difference = (int)lod0_pixels[(y * 16u + x) * 4u + channel] - (int)((sum + 2u) / 4u);
                interpolation_errors += difference < -1 || difference > 1;
            }
        }
    }
    CHECK(interpolation_errors == 0u);
    live_texture_binding multiple_levels = dxt;
    multiple_levels.format += 0x10000u;
    live_texture_plan multiple_plan;
    CHECK(!live_texture_plan_binding(&multiple_levels, false, &multiple_plan));
    CHECK(strcmp(multiple_plan.refusal, "inferred mip chain") == 0);

    /* Literal A8 levels: minification 128->16 selects LOD3; clamping and signed
     * half-level bias discriminate actual mip uploads from a flattened image. */
    const uint8_t alpha_levels[] = {16u, 48u, 80u, 112u, 144u, 176u};
    const uint32_t level_lengths[] = {16384u, 4096u, 1024u, 256u, 64u, 16u};
    uint32_t source_offset = 0u;
    for (uint32_t level = 0u; level < 6u; level++) {
        memset(guest + source_offset, alpha_levels[level], level_lengths[level]);
        source_offset += level_lengths[level];
    }
    cache->allow_inferred = true;
    live_texture_binding mip_binding = {.format = 0x07761929u, .data = GUEST_BASE};
    live_texture_lookup(cache, &mip_binding, reader, NULL, &result);
    CHECK(result.source == LIVE_TEXTURE_SOURCE_GUEST && result.levels == 6u && result.upload_bytes == 87360u);
    if (result.source != LIVE_TEXTURE_SOURCE_GUEST) return 1;
    const uint32_t filters[] = {0x01030000u, 0x01030100u, 0x01060180u, 0x01061F80u, 0x01020000u};
    const uint32_t controls[] = {5u << 6, 1u << 6, 5u << 6, 5u << 6, 5u << 6};
    const uint8_t expected_alpha[] = {112u, 48u, 160u, 96u, 16u};
    for (uint32_t trial = 0u; trial < 5u; trial++) {
        live_vk_sampler_desc mip_sampler = live_vk_sampler_decode_mips(0x303u, filters[trial], controls[trial], 6u);
        CHECK(mip_sampler.ok && !mip_sampler.measured);
        const bool mip_bound = live_vk_texture_bind(set, cache, &result, &mip_sampler, &bound, &error);
        CHECK(mip_bound);
        if (!mip_bound) return 1;
        CHECK(probe_render(&p, bound.descriptor, &minified, mip_pixels));
        for (uint32_t pixel = 0u; pixel < 256u; pixel++) {
            const int delta = (int)mip_pixels[pixel * 4u + 3u] - expected_alpha[trial];
            CHECK(mip_pixels[pixel * 4u] == 255u && mip_pixels[pixel * 4u + 1u] == 255u &&
                  mip_pixels[pixel * 4u + 2u] == 255u && delta >= -1 && delta <= 1);
        }
    }
    /* Last-level data participates in snapshots and dirty overlap. Clamp to5
     * reveals its mutation; a level-zero-only watch/upload cannot pass. */
    live_texture_mark_uploaded(cache, result.entry, result.generation);
    guest[21824u] = 204u;
    live_texture_note_write(cache, GUEST_BASE + 21824u, 16u);
    live_texture_lookup(cache, &mip_binding, reader, NULL, &result);
    CHECK(result.needs_upload);
    live_vk_sampler_desc tail = live_vk_sampler_decode_mips(0x303u, 0x01030000u, (5u << 18) | (5u << 6), 6u);
    CHECK(live_vk_texture_bind(set, cache, &result, &tail, &bound, &error));
    CHECK(probe_render(&p, bound.descriptor, &minified, mip_pixels));
    CHECK(mip_pixels[3] == 204u);
    mip_binding.mip_limit = 1u;
    live_texture_lookup(cache, &mip_binding, reader, NULL, &result);
    CHECK(result.levels == 1u && result.upload_bytes == 65536u);
    tail = live_vk_sampler_decode_mips(0x303u, 0x01030000u, 5u << 6, 1u);
    CHECK(live_vk_texture_bind(set, cache, &result, &tail, &bound, &error));
    CHECK(probe_render(&p, bound.descriptor, &minified, mip_pixels));
    CHECK(mip_pixels[3] == 16u);

    const live_vk_texture_stats stats = live_vk_texture_get_stats(set);
    printf("live vk texture: %d checks, %d failures, uploads=%llu (%llu bytes) images=%llu/%llu samplers=%llu descriptors=%llu targets=%llu\n",
           checks, failures, (unsigned long long)stats.uploads, (unsigned long long)stats.upload_bytes, (unsigned long long)stats.image_creates,
           (unsigned long long)stats.image_destroys, (unsigned long long)stats.sampler_creates, (unsigned long long)stats.descriptor_writes,
           (unsigned long long)stats.target_binds);

    probe_image_destroy(&p, &minified);
    p.vkQueueWaitIdle(native.queue);
    probe_image_destroy(&p, &target_a);
    probe_image_destroy(&p, &target_b);
    probe_image_destroy(&p, &target_c);
    probe_image_destroy(&p, &target_d);
    probe_image_destroy(&p, &target_big);
    p.vkDestroyPipeline(native.device, p.pipeline, NULL);
    p.vkDestroyPipelineLayout(native.device, p.layout, NULL);
    p.vkDestroyRenderPass(native.device, p.render_pass, NULL);
    live_vk_texture_destroy(set);
    live_texture_cache_free(cache);
    free(cache); free(expected); free(actual); free(before); free(sampled_target); free(picture_expected); free(big);
    gpu_window_destroy(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return failures == 0 ? 0 : 1;
}
