/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Several vertices per draw through a translated NV2A vertex shader, on the renderer's own
 * Vulkan device (T100d). See gpu_vsh_draw.h for the module interface.
 *
 * Two paths share the buffers, the descriptor set, the pipeline layout and the vertex input:
 *
 *   gpu_vsh_capture  point list, rasterizer discard, VK_EXT_transform_feedback readback of every
 *                    vertex's outputs. Exact floats, which is what a comparison with the
 *                    interpreter needs.
 *   gpu_vsh_render   an RGBA8 colour target, a triangle list and the fixed fragment stage. Pixels,
 *                    which is what proves the clip-space position and oD0 reach the rasteriser.
 *
 * Failures are reported as gpu_result and everything created is released on every path.
 */

#define VK_NO_PROTOTYPES

#include "gpu_vsh_draw.h"

#include "gpu_device_native.h"
#include "gpu_vsh_shaders.h"
#include "gpu_vsh_vulkan_map.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define SPIRV_MAGIC 0x07230203u
#define MAX_DIMENSION 8192u
#define VERTEX_STRIDE_BYTES (GPU_VSH_ATTRIBUTE_FLOATS * 4u)
#define CAPTURE_STRIDE_BYTES (GPU_VSH_CAPTURE_FLOATS * 4u)
#define CONSTANT_BYTES (GPU_VSH_CONSTANT_ROWS * 16u)
#define TARGET_FORMAT VK_FORMAT_R8G8B8A8_UNORM
#define DEPTH_FORMAT VK_FORMAT_D32_SFLOAT_S8_UINT

#define VSH_FUNCS(X) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) \
    X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool) \
    X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateRenderPass) \
    X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
    X(vkCmdBindVertexBuffers) X(vkCmdBindDescriptorSets) X(vkCmdDraw) X(vkCmdPipelineBarrier) \
    X(vkCmdCopyImageToBuffer) X(vkQueueSubmit) X(vkQueueWaitIdle) \
    X(vkCreateSampler) X(vkDestroySampler) X(vkCmdCopyBufferToImage)

#define VSH_DECLARE(name) PFN_##name name;

typedef struct {
    VSH_FUNCS(VSH_DECLARE)
    PFN_vkCmdBindTransformFeedbackBuffersEXT vkCmdBindTransformFeedbackBuffersEXT;
    PFN_vkCmdBeginTransformFeedbackEXT vkCmdBeginTransformFeedbackEXT;
    PFN_vkCmdEndTransformFeedbackEXT vkCmdEndTransformFeedbackEXT;
} vsh_functions;

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *map;
} vsh_buffer;

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkSampler sampler;
    vsh_buffer staging;
} vsh_texture;

typedef struct {
    vsh_buffer vertices;
    vsh_buffer constants;
    vsh_buffer fragment_constants;
    vsh_texture textures[GPU_VSH_FRAGMENT_TEXTURES];
    vsh_buffer capture; /* transform feedback target, or the colour readback */
    vsh_buffer destination; /* T267: the pixels the target starts as, staged for the upload */
    vsh_buffer depth_up;    /* T267: depth floats then stencil bytes going in */
    vsh_buffer depth_down;  /* and coming back */
    VkImage depth_image;
    VkDeviceMemory depth_memory;
    VkImageView depth_view;
    VkImage image;
    VkDeviceMemory image_memory;
    VkImageView view;
    VkShaderModule vertex_shader;
    VkShaderModule fragment_shader;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    VkPipelineLayout layout;
    VkRenderPass render_pass;
    VkFramebuffer framebuffer;
    VkPipeline pipeline;
    VkCommandBuffer commands;
} vsh_resources;

typedef struct {
    gpu_device_native native;
    vsh_functions fn;
    vsh_resources res;
} vsh_context;

static gpu_result load_functions(vsh_context *ctx)
{
#define VSH_LOAD(name)                                                                        \
    ctx->fn.name = (PFN_##name)ctx->native.get_device_proc_addr(ctx->native.device, #name);    \
    if (!ctx->fn.name) {                                                                      \
        return GPU_ERR_NO_DEVICE;                                                             \
    }
    VSH_FUNCS(VSH_LOAD)
#undef VSH_LOAD
    if (ctx->native.transform_feedback) {
        ctx->fn.vkCmdBindTransformFeedbackBuffersEXT =
            (PFN_vkCmdBindTransformFeedbackBuffersEXT)ctx->native.get_device_proc_addr(
                ctx->native.device, "vkCmdBindTransformFeedbackBuffersEXT");
        ctx->fn.vkCmdBeginTransformFeedbackEXT =
            (PFN_vkCmdBeginTransformFeedbackEXT)ctx->native.get_device_proc_addr(
                ctx->native.device, "vkCmdBeginTransformFeedbackEXT");
        ctx->fn.vkCmdEndTransformFeedbackEXT =
            (PFN_vkCmdEndTransformFeedbackEXT)ctx->native.get_device_proc_addr(
                ctx->native.device, "vkCmdEndTransformFeedbackEXT");
    }
    return GPU_OK;
}

static void release_buffer(vsh_context *ctx, vsh_buffer *buffer)
{
    const VkDevice device = ctx->native.device;
    if (buffer->map) {
        ctx->fn.vkUnmapMemory(device, buffer->memory);
    }
    if (buffer->buffer != VK_NULL_HANDLE) {
        ctx->fn.vkDestroyBuffer(device, buffer->buffer, NULL);
    }
    if (buffer->memory != VK_NULL_HANDLE) {
        ctx->fn.vkFreeMemory(device, buffer->memory, NULL);
    }
    memset(buffer, 0, sizeof *buffer);
}

static void release_all(vsh_context *ctx)
{
    const VkDevice device = ctx->native.device;
    vsh_resources *res = &ctx->res;
    for (uint32_t stage = 0u; stage < GPU_VSH_FRAGMENT_TEXTURES; stage++) {
        vsh_texture *texture = &res->textures[stage];
        if (texture->sampler != VK_NULL_HANDLE) {
            ctx->fn.vkDestroySampler(device, texture->sampler, NULL);
        }
        if (texture->view != VK_NULL_HANDLE) {
            ctx->fn.vkDestroyImageView(device, texture->view, NULL);
        }
        if (texture->image != VK_NULL_HANDLE) {
            ctx->fn.vkDestroyImage(device, texture->image, NULL);
        }
        if (texture->memory != VK_NULL_HANDLE) {
            ctx->fn.vkFreeMemory(device, texture->memory, NULL);
        }
        release_buffer(ctx, &texture->staging);
    }
    release_buffer(ctx, &res->fragment_constants);
    if (res->commands != VK_NULL_HANDLE) {
        ctx->fn.vkFreeCommandBuffers(device, ctx->native.command_pool, 1u, &res->commands);
    }
    if (res->pipeline != VK_NULL_HANDLE) {
        ctx->fn.vkDestroyPipeline(device, res->pipeline, NULL);
    }
    if (res->framebuffer != VK_NULL_HANDLE) {
        ctx->fn.vkDestroyFramebuffer(device, res->framebuffer, NULL);
    }
    if (res->render_pass != VK_NULL_HANDLE) {
        ctx->fn.vkDestroyRenderPass(device, res->render_pass, NULL);
    }
    if (res->layout != VK_NULL_HANDLE) {
        ctx->fn.vkDestroyPipelineLayout(device, res->layout, NULL);
    }
    if (res->pool != VK_NULL_HANDLE) {
        ctx->fn.vkDestroyDescriptorPool(device, res->pool, NULL);
    }
    if (res->set_layout != VK_NULL_HANDLE) {
        ctx->fn.vkDestroyDescriptorSetLayout(device, res->set_layout, NULL);
    }
    if (res->fragment_shader != VK_NULL_HANDLE) {
        ctx->fn.vkDestroyShaderModule(device, res->fragment_shader, NULL);
    }
    if (res->vertex_shader != VK_NULL_HANDLE) {
        ctx->fn.vkDestroyShaderModule(device, res->vertex_shader, NULL);
    }
    if (res->depth_view != VK_NULL_HANDLE) {
        ctx->fn.vkDestroyImageView(device, res->depth_view, NULL);
    }
    if (res->depth_image != VK_NULL_HANDLE) {
        ctx->fn.vkDestroyImage(device, res->depth_image, NULL);
    }
    if (res->depth_memory != VK_NULL_HANDLE) {
        ctx->fn.vkFreeMemory(device, res->depth_memory, NULL);
    }
    if (res->view != VK_NULL_HANDLE) {
        ctx->fn.vkDestroyImageView(device, res->view, NULL);
    }
    if (res->image != VK_NULL_HANDLE) {
        ctx->fn.vkDestroyImage(device, res->image, NULL);
    }
    if (res->image_memory != VK_NULL_HANDLE) {
        ctx->fn.vkFreeMemory(device, res->image_memory, NULL);
    }
    release_buffer(ctx, &res->capture);
    release_buffer(ctx, &res->destination);
    release_buffer(ctx, &res->depth_up);
    release_buffer(ctx, &res->depth_down);
    release_buffer(ctx, &res->constants);
    release_buffer(ctx, &res->vertices);
    memset(res, 0, sizeof *res);
}

static bool find_memory_type(const VkPhysicalDeviceMemoryProperties *properties,
                             uint32_t type_bits, VkMemoryPropertyFlags required, uint32_t *index)
{
    for (uint32_t candidate = 0u; candidate < properties->memoryTypeCount; candidate++) {
        if ((type_bits & (1u << candidate)) != 0u &&
            (properties->memoryTypes[candidate].propertyFlags & required) == required) {
            *index = candidate;
            return true;
        }
    }
    return false;
}

/* Host visible and coherent, mapped for the life of the buffer. */
static gpu_result make_buffer(vsh_context *ctx, vsh_buffer *buffer, VkDeviceSize size,
                              VkBufferUsageFlags usage)
{
    const VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (ctx->fn.vkCreateBuffer(ctx->native.device, &info, NULL, &buffer->buffer) != VK_SUCCESS) {
        return GPU_ERR_VULKAN;
    }
    VkMemoryRequirements requirements;
    ctx->fn.vkGetBufferMemoryRequirements(ctx->native.device, buffer->buffer, &requirements);
    uint32_t type = 0u;
    if (!find_memory_type(&ctx->native.memory_properties, requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &type)) {
        return GPU_ERR_NO_MEMORY_TYPE;
    }
    const VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    if (ctx->fn.vkAllocateMemory(ctx->native.device, &allocation, NULL, &buffer->memory) !=
        VK_SUCCESS) {
        return GPU_ERR_OUT_OF_MEMORY;
    }
    if (ctx->fn.vkBindBufferMemory(ctx->native.device, buffer->buffer, buffer->memory, 0u) !=
        VK_SUCCESS) {
        return GPU_ERR_VULKAN;
    }
    if (ctx->fn.vkMapMemory(ctx->native.device, buffer->memory, 0u, VK_WHOLE_SIZE, 0u,
                            &buffer->map) != VK_SUCCESS) {
        buffer->map = NULL;
        return GPU_ERR_VULKAN;
    }
    return GPU_OK;
}

static gpu_result make_shader(vsh_context *ctx, const uint32_t *words, size_t word_count,
                              VkShaderModule *module)
{
    if (!words || word_count < 5u || words[0] != SPIRV_MAGIC) {
        return GPU_ERR_ARGUMENT;
    }
    const VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = word_count * sizeof *words,
        .pCode = words,
    };
    return ctx->fn.vkCreateShaderModule(ctx->native.device, &info, NULL, module) == VK_SUCCESS
               ? GPU_OK : GPU_ERR_VULKAN;
}

/* The fragment stage's resources (T75): its constants block and one image, sampler and staging
 * buffer per stage that has a test texture. The upload itself is recorded later. */
static gpu_result make_texture(vsh_context *ctx, vsh_texture *texture, const gpu_vsh_texture *source)
{
    const VkDeviceSize bytes = (VkDeviceSize)source->width * source->height * 4u;
    gpu_result outcome = make_buffer(ctx, &texture->staging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (outcome != GPU_OK) {
        return outcome;
    }
    memcpy(texture->staging.map, source->rgba, (size_t)bytes);
    const VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = TARGET_FORMAT,
        .extent = { source->width, source->height, 1u },
        .mipLevels = 1u,
        .arrayLayers = 1u,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (ctx->fn.vkCreateImage(ctx->native.device, &image_info, NULL, &texture->image) != VK_SUCCESS) {
        return GPU_ERR_VULKAN;
    }
    VkMemoryRequirements requirements;
    ctx->fn.vkGetImageMemoryRequirements(ctx->native.device, texture->image, &requirements);
    uint32_t type = 0u;
    if (!find_memory_type(&ctx->native.memory_properties, requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
        return GPU_ERR_NO_MEMORY_TYPE;
    }
    const VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    if (ctx->fn.vkAllocateMemory(ctx->native.device, &allocation, NULL, &texture->memory) !=
            VK_SUCCESS ||
        ctx->fn.vkBindImageMemory(ctx->native.device, texture->image, texture->memory, 0u) !=
            VK_SUCCESS) {
        return GPU_ERR_VULKAN;
    }
    const VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = TARGET_FORMAT,
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1u,
                              .layerCount = 1u },
    };
    if (ctx->fn.vkCreateImageView(ctx->native.device, &view_info, NULL, &texture->view) !=
        VK_SUCCESS) {
        return GPU_ERR_VULKAN;
    }
    const VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = source->linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = source->linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = source->repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = source->repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 0.0f,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
    };
    return ctx->fn.vkCreateSampler(ctx->native.device, &sampler_info, NULL, &texture->sampler) ==
                   VK_SUCCESS
               ? GPU_OK : GPU_ERR_VULKAN;
}

static gpu_result make_fragment_resources(vsh_context *ctx, const gpu_vsh_fragment *fragment)
{
    gpu_result outcome = make_buffer(ctx, &ctx->res.fragment_constants,
                                     GPU_VSH_FRAGMENT_VEC4S * 16u, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    if (outcome != GPU_OK) {
        return outcome;
    }
    memcpy(ctx->res.fragment_constants.map, fragment->constants, GPU_VSH_FRAGMENT_VEC4S * 16u);
    for (uint32_t stage = 0u; outcome == GPU_OK && stage < GPU_VSH_FRAGMENT_TEXTURES; stage++) {
        if (fragment->textures[stage].rgba != NULL) {
            outcome = make_texture(ctx, &ctx->res.textures[stage], &fragment->textures[stage]);
        }
    }
    return outcome;
}

/* Copy each staging buffer into its image and leave the image readable by the fragment stage. */
static void record_texture_uploads(vsh_context *ctx, const gpu_vsh_fragment *fragment)
{
    const VkCommandBuffer commands = ctx->res.commands;
    for (uint32_t stage = 0u; stage < GPU_VSH_FRAGMENT_TEXTURES; stage++) {
        if (fragment->textures[stage].rgba == NULL) {
            continue;
        }
        const vsh_texture *texture = &ctx->res.textures[stage];
        const VkImageSubresourceRange range = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                .levelCount = 1u, .layerCount = 1u };
        const VkImageMemoryBarrier to_destination = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = texture->image,
            .subresourceRange = range,
        };
        ctx->fn.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, NULL, 0u, NULL, 1u,
                                     &to_destination);
        const VkBufferImageCopy copy = {
            .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1u },
            .imageExtent = { fragment->textures[stage].width, fragment->textures[stage].height, 1u },
        };
        ctx->fn.vkCmdCopyBufferToImage(commands, texture->staging.buffer, texture->image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &copy);
        const VkImageMemoryBarrier to_shader = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = texture->image,
            .subresourceRange = range,
        };
        ctx->fn.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0u, 0u, NULL, 0u, NULL,
                                     1u, &to_shader);
    }
}

#define MAX_BINDINGS (2u + GPU_VSH_FRAGMENT_TEXTURES)

/* The vertex constants block at set 0 binding 0 and, with a fragment stage, its constants block at
 * binding 1 and its samplers at 2 + stage: one descriptor set, one pipeline layout. */
static gpu_result make_layout(vsh_context *ctx, const gpu_vsh_fragment *fragment)
{
    vsh_resources *res = &ctx->res;
    VkDescriptorSetLayoutBinding bindings[MAX_BINDINGS];
    VkWriteDescriptorSet writes[MAX_BINDINGS];
    VkDescriptorImageInfo images[GPU_VSH_FRAGMENT_TEXTURES];
    uint32_t binding_count = 0u;
    uint32_t uniform_count = 1u;
    uint32_t sampler_count = 0u;
    bindings[binding_count++] = (VkDescriptorSetLayoutBinding){
        .binding = 0u,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1u,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
    };
    if (fragment != NULL) {
        uniform_count++;
        bindings[binding_count++] = (VkDescriptorSetLayoutBinding){
            .binding = 1u,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1u,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        };
        for (uint32_t stage = 0u; stage < GPU_VSH_FRAGMENT_TEXTURES; stage++) {
            if (fragment->textures[stage].rgba != NULL) {
                sampler_count++;
                bindings[binding_count++] = (VkDescriptorSetLayoutBinding){
                    .binding = 2u + stage,
                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    .descriptorCount = 1u,
                    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                };
            }
        }
    }
    const VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = binding_count,
        .pBindings = bindings,
    };
    if (ctx->fn.vkCreateDescriptorSetLayout(ctx->native.device, &set_info, NULL,
                                            &res->set_layout) != VK_SUCCESS) {
        return GPU_ERR_VULKAN;
    }
    const VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1u,
        .pSetLayouts = &res->set_layout,
    };
    if (ctx->fn.vkCreatePipelineLayout(ctx->native.device, &layout_info, NULL, &res->layout) !=
        VK_SUCCESS) {
        return GPU_ERR_VULKAN;
    }
    const VkDescriptorPoolSize pool_sizes[2] = {
        { .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = uniform_count },
        { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = sampler_count },
    };
    const VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1u,
        .poolSizeCount = sampler_count != 0u ? 2u : 1u,
        .pPoolSizes = pool_sizes,
    };
    if (ctx->fn.vkCreateDescriptorPool(ctx->native.device, &pool_info, NULL, &res->pool) !=
        VK_SUCCESS) {
        return GPU_ERR_VULKAN;
    }
    const VkDescriptorSetAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = res->pool,
        .descriptorSetCount = 1u,
        .pSetLayouts = &res->set_layout,
    };
    if (ctx->fn.vkAllocateDescriptorSets(ctx->native.device, &allocation, &res->set) !=
        VK_SUCCESS) {
        return GPU_ERR_VULKAN;
    }
    const VkDescriptorBufferInfo uniform = {
        .buffer = res->constants.buffer,
        .offset = 0u,
        .range = CONSTANT_BYTES,
    };
    const VkDescriptorBufferInfo fragment_uniform = {
        .buffer = res->fragment_constants.buffer,
        .offset = 0u,
        .range = GPU_VSH_FRAGMENT_VEC4S * 16u,
    };
    uint32_t write_count = 0u;
    writes[write_count++] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = res->set,
        .dstBinding = 0u,
        .descriptorCount = 1u,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .pBufferInfo = &uniform,
    };
    if (fragment != NULL) {
        writes[write_count++] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = res->set,
            .dstBinding = 1u,
            .descriptorCount = 1u,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &fragment_uniform,
        };
        for (uint32_t stage = 0u; stage < GPU_VSH_FRAGMENT_TEXTURES; stage++) {
            if (fragment->textures[stage].rgba == NULL) {
                continue;
            }
            images[stage] = (VkDescriptorImageInfo){
                .sampler = res->textures[stage].sampler,
                .imageView = res->textures[stage].view,
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            };
            writes[write_count++] = (VkWriteDescriptorSet){
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = res->set,
                .dstBinding = 2u + stage,
                .descriptorCount = 1u,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &images[stage],
            };
        }
    }
    ctx->fn.vkUpdateDescriptorSets(ctx->native.device, write_count, writes, 0u, NULL);
    return GPU_OK;
}

/* Binding 0 at stride 256 holding v0..v15 as vec4, location n at byte offset 16 * n. */
static void fill_vertex_input(VkVertexInputBindingDescription *binding,
                              VkVertexInputAttributeDescription attributes[GPU_VSH_INPUTS],
                              VkPipelineVertexInputStateCreateInfo *state)
{
    *binding = (VkVertexInputBindingDescription){
        .binding = 0u,
        .stride = VERTEX_STRIDE_BYTES,
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    for (uint32_t location = 0u; location < GPU_VSH_INPUTS; location++) {
        attributes[location] = (VkVertexInputAttributeDescription){
            .location = location,
            .binding = 0u,
            .format = VK_FORMAT_R32G32B32A32_SFLOAT,
            .offset = 16u * location,
        };
    }
    *state = (VkPipelineVertexInputStateCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1u,
        .pVertexBindingDescriptions = binding,
        .vertexAttributeDescriptionCount = GPU_VSH_INPUTS,
        .pVertexAttributeDescriptions = attributes,
    };
}

static gpu_result check_fragment(const gpu_vsh_fragment *fragment)
{
    if (fragment->words == NULL || fragment->word_count < 5u || fragment->constants == NULL) {
        return GPU_ERR_ARGUMENT;
    }
    for (uint32_t stage = 0u; stage < GPU_VSH_FRAGMENT_TEXTURES; stage++) {
        const gpu_vsh_texture *texture = &fragment->textures[stage];
        if (texture->rgba != NULL &&
            (texture->width == 0u || texture->height == 0u || texture->width > 4096u ||
             texture->height > 4096u)) {
            return GPU_ERR_ARGUMENT;
        }
    }
    return GPU_OK;
}

static gpu_result check_draw(const gpu_vsh_draw *draw)
{
    if (!draw || !draw->attributes || !draw->constants || draw->vertex_count == 0u ||
        draw->vertex_count > GPU_VSH_MAX_VERTICES) {
        return GPU_ERR_ARGUMENT;
    }
    if (draw->topology > GPU_VSH_TOPOLOGY_LINE_LIST ||
        (draw->topology == GPU_VSH_TOPOLOGY_LINE_LIST && draw->line_width != 1.0f)) {
        return GPU_ERR_ARGUMENT;
    }
    return draw->fragment != NULL ? check_fragment(draw->fragment) : GPU_OK;
}

/* Vertex buffer, constants block, descriptor set and pipeline layout, shared by both paths. */
static gpu_result prepare_common(vsh_context *ctx, const gpu_vsh_draw *draw,
                                 const gpu_vsh_fragment *fragment)
{
    gpu_result outcome = make_buffer(ctx, &ctx->res.vertices,
                                     (VkDeviceSize)draw->vertex_count * VERTEX_STRIDE_BYTES,
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (outcome != GPU_OK) {
        return outcome;
    }
    outcome = make_buffer(ctx, &ctx->res.constants, CONSTANT_BYTES,
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    if (outcome != GPU_OK) {
        return outcome;
    }
    memcpy(ctx->res.vertices.map, draw->attributes,
           (size_t)draw->vertex_count * VERTEX_STRIDE_BYTES);
    memcpy(ctx->res.constants.map, draw->constants, CONSTANT_BYTES);
    outcome = make_shader(ctx, draw->words, draw->word_count, &ctx->res.vertex_shader);
    if (outcome != GPU_OK) {
        return outcome;
    }
    if (fragment != NULL) {
        outcome = make_fragment_resources(ctx, fragment);
        if (outcome != GPU_OK) {
            return outcome;
        }
    }
    return make_layout(ctx, fragment);
}

static gpu_result submit_and_wait(vsh_context *ctx)
{
    const VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1u,
        .pCommandBuffers = &ctx->res.commands,
    };
    if (ctx->fn.vkQueueSubmit(ctx->native.queue, 1u, &submit, VK_NULL_HANDLE) != VK_SUCCESS) {
        return GPU_ERR_VULKAN;
    }
    return ctx->fn.vkQueueWaitIdle(ctx->native.queue) == VK_SUCCESS ? GPU_OK : GPU_ERR_VULKAN;
}

static gpu_result begin_commands(vsh_context *ctx)
{
    const VkCommandBufferAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = ctx->native.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1u,
    };
    if (ctx->fn.vkAllocateCommandBuffers(ctx->native.device, &allocation, &ctx->res.commands) !=
        VK_SUCCESS) {
        return GPU_ERR_VULKAN;
    }
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    return ctx->fn.vkBeginCommandBuffer(ctx->res.commands, &begin) == VK_SUCCESS ? GPU_OK
                                                                                 : GPU_ERR_VULKAN;
}

/* ------------------------------------------------------------------------ capture */

static gpu_result capture_pipeline(vsh_context *ctx)
{
    vsh_resources *res = &ctx->res;
    /* No attachments: the pass exists only because a graphics pipeline needs one. */
    const VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
    };
    const VkRenderPassCreateInfo pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .subpassCount = 1u,
        .pSubpasses = &subpass,
    };
    if (ctx->fn.vkCreateRenderPass(ctx->native.device, &pass_info, NULL, &res->render_pass) !=
        VK_SUCCESS) {
        return GPU_ERR_VULKAN;
    }
    const VkFramebufferCreateInfo framebuffer_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = res->render_pass,
        .width = 1u,
        .height = 1u,
        .layers = 1u,
    };
    if (ctx->fn.vkCreateFramebuffer(ctx->native.device, &framebuffer_info, NULL,
                                    &res->framebuffer) != VK_SUCCESS) {
        return GPU_ERR_VULKAN;
    }
    VkVertexInputBindingDescription binding;
    VkVertexInputAttributeDescription attributes[GPU_VSH_INPUTS];
    VkPipelineVertexInputStateCreateInfo vertex_input;
    fill_vertex_input(&binding, attributes, &vertex_input);
    const VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
    };
    const VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .rasterizerDiscardEnable = VK_TRUE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .lineWidth = 1.0f,
    };
    const VkPipelineShaderStageCreateInfo stage = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT,
        .module = res->vertex_shader,
        .pName = "main",
    };
    const VkGraphicsPipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 1u,
        .pStages = &stage,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly,
        .pRasterizationState = &raster,
        .layout = res->layout,
        .renderPass = res->render_pass,
    };
    return ctx->fn.vkCreateGraphicsPipelines(ctx->native.device, VK_NULL_HANDLE, 1u, &info, NULL,
                                             &res->pipeline) == VK_SUCCESS
               ? GPU_OK : GPU_ERR_VULKAN;
}

static gpu_result capture_run(gpu_device *device, const gpu_vsh_draw *draw, float *out_capture)
{
    vsh_context ctx;
    memset(&ctx, 0, sizeof ctx);
    gpu_device_get_native(device, &ctx.native);
    gpu_result outcome = load_functions(&ctx);
    if (outcome != GPU_OK) {
        return outcome;
    }
    const VkDeviceSize capture_bytes = (VkDeviceSize)draw->vertex_count * CAPTURE_STRIDE_BYTES;
    outcome = prepare_common(&ctx, draw, NULL);
    if (outcome == GPU_OK) {
        outcome = make_buffer(&ctx, &ctx.res.capture, capture_bytes,
                              VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT);
    }
    if (outcome == GPU_OK) {
        outcome = capture_pipeline(&ctx);
    }
    if (outcome == GPU_OK) {
        outcome = begin_commands(&ctx);
    }
    if (outcome != GPU_OK) {
        release_all(&ctx);
        return outcome;
    }
    uint32_t *words = ctx.res.capture.map;
    for (VkDeviceSize i = 0u; i < capture_bytes / 4u; i++) {
        words[i] = GPU_VSH_CAPTURE_SENTINEL_BITS;
    }

    vsh_functions *fn = &ctx.fn;
    const VkCommandBuffer commands = ctx.res.commands;
    const VkRenderPassBeginInfo pass_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = ctx.res.render_pass,
        .framebuffer = ctx.res.framebuffer,
        .renderArea = { .extent = { 1u, 1u } },
    };
    const VkDeviceSize no_offset = 0u;
    fn->vkCmdBeginRenderPass(commands, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    fn->vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx.res.pipeline);
    fn->vkCmdBindVertexBuffers(commands, 0u, 1u, &ctx.res.vertices.buffer, &no_offset);
    fn->vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx.res.layout, 0u, 1u,
                                &ctx.res.set, 0u, NULL);
    fn->vkCmdBindTransformFeedbackBuffersEXT(commands, 0u, 1u, &ctx.res.capture.buffer, &no_offset,
                                             &capture_bytes);
    fn->vkCmdBeginTransformFeedbackEXT(commands, 0u, 0u, NULL, NULL);
    fn->vkCmdDraw(commands, draw->vertex_count, 1u, 0u, 0u); /* ONE draw, every vertex */
    fn->vkCmdEndTransformFeedbackEXT(commands, 0u, 0u, NULL, NULL);
    fn->vkCmdEndRenderPass(commands);
    const VkMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    };
    fn->vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0u, 1u, &barrier, 0u, NULL, 0u, NULL);
    if (fn->vkEndCommandBuffer(commands) != VK_SUCCESS) {
        outcome = GPU_ERR_VULKAN;
    } else {
        outcome = submit_and_wait(&ctx);
    }
    if (outcome == GPU_OK) {
        memcpy(out_capture, ctx.res.capture.map, (size_t)capture_bytes);
    }
    release_all(&ctx);
    return outcome;
}

gpu_result gpu_vsh_capture(gpu_device *device, const gpu_vsh_draw *draw, float *out_capture)
{
    if (!device || !out_capture || check_draw(draw) != GPU_OK ||
        !gpu_device_has_transform_feedback(device)) {
        return GPU_ERR_ARGUMENT;
    }
    return capture_run(device, draw, out_capture);
}

/* ------------------------------------------------------------------------- render */

static gpu_result render_target(vsh_context *ctx, uint32_t width, uint32_t height, bool load,
                                bool depth)
{
    vsh_resources *res = &ctx->res;
    const VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = TARGET_FORMAT,
        .extent = { width, height, 1u },
        .mipLevels = 1u,
        .arrayLayers = 1u,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 (load ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0u),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (ctx->fn.vkCreateImage(ctx->native.device, &image_info, NULL, &res->image) != VK_SUCCESS) {
        return GPU_ERR_VULKAN;
    }
    VkMemoryRequirements requirements;
    ctx->fn.vkGetImageMemoryRequirements(ctx->native.device, res->image, &requirements);
    uint32_t type = 0u;
    if (!find_memory_type(&ctx->native.memory_properties, requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
        return GPU_ERR_NO_MEMORY_TYPE;
    }
    const VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    if (ctx->fn.vkAllocateMemory(ctx->native.device, &allocation, NULL, &res->image_memory) !=
            VK_SUCCESS ||
        ctx->fn.vkBindImageMemory(ctx->native.device, res->image, res->image_memory, 0u) !=
            VK_SUCCESS) {
        return GPU_ERR_VULKAN;
    }
    const VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = res->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = TARGET_FORMAT,
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1u,
                              .layerCount = 1u },
    };
    if (ctx->fn.vkCreateImageView(ctx->native.device, &view_info, NULL, &res->view) !=
        VK_SUCCESS) {
        return GPU_ERR_VULKAN;
    }
    if (depth) {
        /* T267: the depth and stencil image of a draw that tests either. Its contents are uploaded before the
         * pass and read back after it, so the pass LOADs them and leaves them in TRANSFER_SRC. */
        const VkImageCreateInfo depth_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = DEPTH_FORMAT,
            .extent = { width, height, 1u },
            .mipLevels = 1u,
            .arrayLayers = 1u,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        if (ctx->fn.vkCreateImage(ctx->native.device, &depth_info, NULL, &res->depth_image) !=
            VK_SUCCESS) {
            return GPU_ERR_VULKAN;
        }
        VkMemoryRequirements depth_requirements;
        ctx->fn.vkGetImageMemoryRequirements(ctx->native.device, res->depth_image, &depth_requirements);
        uint32_t depth_type = 0u;
        if (!find_memory_type(&ctx->native.memory_properties, depth_requirements.memoryTypeBits,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &depth_type)) {
            return GPU_ERR_NO_MEMORY_TYPE;
        }
        const VkMemoryAllocateInfo depth_allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = depth_requirements.size,
            .memoryTypeIndex = depth_type,
        };
        if (ctx->fn.vkAllocateMemory(ctx->native.device, &depth_allocation, NULL, &res->depth_memory) !=
                VK_SUCCESS ||
            ctx->fn.vkBindImageMemory(ctx->native.device, res->depth_image, res->depth_memory, 0u) !=
                VK_SUCCESS) {
            return GPU_ERR_VULKAN;
        }
        const VkImageViewCreateInfo depth_view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = res->depth_image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = DEPTH_FORMAT,
            .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                                  .levelCount = 1u, .layerCount = 1u },
        };
        if (ctx->fn.vkCreateImageView(ctx->native.device, &depth_view_info, NULL, &res->depth_view) !=
            VK_SUCCESS) {
            return GPU_ERR_VULKAN;
        }
    }
    /* The same pass shape as gpu_device.c render(): CLEAR on load, final layout TRANSFER_SRC. With
     * `load` (a draw that starts from given pixels, T267) the load op is LOAD and the pixels were
     * uploaded and moved to COLOR_ATTACHMENT_OPTIMAL before the pass. */
    const VkAttachmentDescription attachment = {
        .format = TARGET_FORMAT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = load ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    };
    const VkAttachmentReference reference = {
        .attachment = 0u,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    const VkAttachmentDescription depth_attachment = {
        .format = DEPTH_FORMAT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    };
    const VkAttachmentReference depth_reference = {
        .attachment = 1u,
        .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };
    const VkAttachmentDescription attachments[2] = { attachment, depth_attachment };
    const VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &reference,
        .pDepthStencilAttachment = depth ? &depth_reference : NULL,
    };
    const VkSubpassDependency dependency = {
        .srcSubpass = 0u,
        .dstSubpass = VK_SUBPASS_EXTERNAL,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        (depth ? VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT : 0u),
        .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                         (depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0u),
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
    };
    const VkRenderPassCreateInfo pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = depth ? 2u : 1u,
        .pAttachments = attachments,
        .subpassCount = 1u,
        .pSubpasses = &subpass,
        .dependencyCount = 1u,
        .pDependencies = &dependency,
    };
    if (ctx->fn.vkCreateRenderPass(ctx->native.device, &pass_info, NULL, &res->render_pass) !=
        VK_SUCCESS) {
        return GPU_ERR_VULKAN;
    }
    const VkImageView framebuffer_views[2] = { res->view, res->depth_view };
    const VkFramebufferCreateInfo framebuffer_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = res->render_pass,
        .attachmentCount = depth ? 2u : 1u,
        .pAttachments = framebuffer_views,
        .width = width,
        .height = height,
        .layers = 1u,
    };
    return ctx->fn.vkCreateFramebuffer(ctx->native.device, &framebuffer_info, NULL,
                                       &res->framebuffer) == VK_SUCCESS
               ? GPU_OK : GPU_ERR_VULKAN;
}

VkPrimitiveTopology gpu_vsh_vulkan_topology(uint32_t topology)
{
    switch (topology) {
    case GPU_VSH_TOPOLOGY_POINT_LIST: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case GPU_VSH_TOPOLOGY_LINE_LIST: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

VkPolygonMode gpu_vsh_vulkan_polygon_mode(uint32_t polygon_mode)
{
    switch (polygon_mode) {
    case GPU_VSH_POLYGON_LINE: return VK_POLYGON_MODE_LINE;
    case GPU_VSH_POLYGON_POINT: return VK_POLYGON_MODE_POINT;
    default: return VK_POLYGON_MODE_FILL;
    }
}

static gpu_result check_output(const gpu_vsh_draw *draw, uint32_t width, uint32_t height)
{
    const gpu_vsh_output *output = draw->output;
    if (output == NULL) {
        return GPU_OK;
    }
    if (output->cull_mode > GPU_VSH_CULL_BACK || output->polygon_mode > GPU_VSH_POLYGON_POINT) {
        return GPU_ERR_ARGUMENT;
    }
    if (output->blend && (output->blend_source >= GPU_VSH_BLEND_FACTORS ||
                          output->blend_destination >= GPU_VSH_BLEND_FACTORS ||
                          output->blend_equation >= GPU_VSH_BLEND_OPS)) {
        return GPU_ERR_ARGUMENT;
    }
    if ((output->color_write_disable & ~(GPU_VSH_CHANNEL_R | GPU_VSH_CHANNEL_G | GPU_VSH_CHANNEL_B |
                                         GPU_VSH_CHANNEL_A)) != 0u) {
        return GPU_ERR_ARGUMENT;
    }
    if (output->alpha_test && (output->alpha_func > GPU_VSH_COMPARE_ALWAYS ||
                               output->alpha_ref > 255u || draw->fragment != NULL)) {
        return GPU_ERR_ARGUMENT;
    }
    if (output->depth_test || output->stencil_test) {
        if (output->depth == NULL || output->stencil == NULL) {
            return GPU_ERR_ARGUMENT;
        }
    }
    if (output->depth_test && output->depth_func > GPU_VSH_COMPARE_ALWAYS) {
        return GPU_ERR_ARGUMENT;
    }
    if (output->depth_bias && (!output->depth_test || !isfinite(output->depth_bias_constant) ||
                               !isfinite(output->depth_bias_slope))) {
        return GPU_ERR_ARGUMENT;
    }
    if (output->stencil_test &&
        (output->stencil_func > GPU_VSH_COMPARE_ALWAYS || output->stencil_ref > 255u ||
         output->stencil_compare_mask > 255u || output->stencil_write_mask > 255u ||
         output->stencil_fail_op >= GPU_VSH_STENCIL_OPS ||
         output->stencil_zfail_op >= GPU_VSH_STENCIL_OPS ||
         output->stencil_zpass_op >= GPU_VSH_STENCIL_OPS)) {
        return GPU_ERR_ARGUMENT;
    }
    if (!output->scissor) {
        return GPU_OK;
    }
    if (output->scissor_x > width || output->scissor_width > width - output->scissor_x ||
        output->scissor_y > height || output->scissor_height > height - output->scissor_y) {
        return GPU_ERR_ARGUMENT;
    }
    return GPU_OK;
}

VkBlendFactor gpu_vsh_vulkan_blend_factor(uint32_t factor)
{
    switch (factor) {
    case GPU_VSH_BLEND_ZERO: return VK_BLEND_FACTOR_ZERO;
    case GPU_VSH_BLEND_ONE: return VK_BLEND_FACTOR_ONE;
    case GPU_VSH_BLEND_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
    case GPU_VSH_BLEND_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case GPU_VSH_BLEND_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case GPU_VSH_BLEND_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case GPU_VSH_BLEND_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
    case GPU_VSH_BLEND_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case GPU_VSH_BLEND_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
    case GPU_VSH_BLEND_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case GPU_VSH_BLEND_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case GPU_VSH_BLEND_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case GPU_VSH_BLEND_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case GPU_VSH_BLEND_CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    }
}

VkCompareOp gpu_vsh_vulkan_compare_op(uint32_t compare)
{
    switch (compare) {
    case GPU_VSH_COMPARE_NEVER: return VK_COMPARE_OP_NEVER;
    case GPU_VSH_COMPARE_LESS: return VK_COMPARE_OP_LESS;
    case GPU_VSH_COMPARE_EQUAL: return VK_COMPARE_OP_EQUAL;
    case GPU_VSH_COMPARE_LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case GPU_VSH_COMPARE_GREATER: return VK_COMPARE_OP_GREATER;
    case GPU_VSH_COMPARE_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    case GPU_VSH_COMPARE_GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

VkStencilOp gpu_vsh_vulkan_stencil_op(uint32_t operation)
{
    switch (operation) {
    case GPU_VSH_STENCIL_OP_ZERO: return VK_STENCIL_OP_ZERO;
    case GPU_VSH_STENCIL_OP_REPLACE: return VK_STENCIL_OP_REPLACE;
    case GPU_VSH_STENCIL_OP_INCREMENT_CLAMP: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case GPU_VSH_STENCIL_OP_DECREMENT_CLAMP: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case GPU_VSH_STENCIL_OP_INVERT: return VK_STENCIL_OP_INVERT;
    case GPU_VSH_STENCIL_OP_INCREMENT_WRAP: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case GPU_VSH_STENCIL_OP_DECREMENT_WRAP: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

VkBlendOp gpu_vsh_vulkan_blend_op(uint32_t equation)
{
    switch (equation) {
    case GPU_VSH_BLEND_OP_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
    case GPU_VSH_BLEND_OP_REVERSE_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case GPU_VSH_BLEND_OP_MIN: return VK_BLEND_OP_MIN;
    case GPU_VSH_BLEND_OP_MAX: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static gpu_result render_pipeline(vsh_context *ctx, const gpu_vsh_draw *draw, uint32_t width,
                                  uint32_t height)
{
    vsh_resources *res = &ctx->res;
    const gpu_vsh_fragment *fragment = draw->fragment;
    const bool alpha_test = draw->output != NULL && draw->output->alpha_test;
    gpu_result outcome = fragment != NULL
        ? make_shader(ctx, fragment->words, fragment->word_count, &res->fragment_shader)
        : alpha_test
            ? make_shader(ctx, gpu_shader_vsh_draw_alpha_frag,
                          sizeof gpu_shader_vsh_draw_alpha_frag / sizeof(uint32_t),
                          &res->fragment_shader)
            : make_shader(ctx, gpu_shader_vsh_draw_frag,
                          sizeof gpu_shader_vsh_draw_frag / sizeof(uint32_t), &res->fragment_shader);
    if (outcome != GPU_OK) {
        return outcome;
    }
    /* The alpha test's two specialisation constants: function (id 0) and reference (id 1). */
    const uint32_t alpha_values[2] = { alpha_test ? draw->output->alpha_func : 0u,
                                       alpha_test ? draw->output->alpha_ref : 0u };
    const VkSpecializationMapEntry alpha_entries[2] = {
        { 0u, 0u, sizeof(uint32_t) },
        { 1u, sizeof(uint32_t), sizeof(uint32_t) },
    };
    const VkSpecializationInfo alpha_specialization = {
        .mapEntryCount = 2u,
        .pMapEntries = alpha_entries,
        .dataSize = sizeof alpha_values,
        .pData = alpha_values,
    };
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = res->vertex_shader,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = res->fragment_shader,
            .pName = "main",
            .pSpecializationInfo = alpha_test ? &alpha_specialization : NULL,
        },
    };
    VkVertexInputBindingDescription binding;
    VkVertexInputAttributeDescription attributes[GPU_VSH_INPUTS];
    VkPipelineVertexInputStateCreateInfo vertex_input;
    fill_vertex_input(&binding, attributes, &vertex_input);
    const VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = gpu_vsh_vulkan_topology(draw->topology),
    };
    const VkViewport viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
    VkRect2D scissor = { { 0, 0 }, { width, height } };
    if (draw->output != NULL && draw->output->scissor) {
        scissor.offset.x = (int32_t)draw->output->scissor_x;
        scissor.offset.y = (int32_t)draw->output->scissor_y;
        scissor.extent.width = draw->output->scissor_width;
        scissor.extent.height = draw->output->scissor_height;
    }
    const VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1u,
        .pViewports = &viewport,
        .scissorCount = 1u,
        .pScissors = &scissor,
    };
    VkCullModeFlags cull_mode = VK_CULL_MODE_NONE;
    VkFrontFace front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    if (draw->output != NULL) {
        cull_mode = draw->output->cull_mode == GPU_VSH_CULL_FRONT ? VK_CULL_MODE_FRONT_BIT
                    : draw->output->cull_mode == GPU_VSH_CULL_BACK ? VK_CULL_MODE_BACK_BIT
                                                                   : VK_CULL_MODE_NONE;
        front_face = draw->output->front_clockwise ? VK_FRONT_FACE_CLOCKWISE
                                                   : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    }
    /* T502: only triangles are biased (see gpu_vsh_output), so lines and points never depend on an
     * implementation's rule for them. */
    const bool biased = draw->output != NULL && draw->output->depth_bias &&
                        draw->topology != GPU_VSH_TOPOLOGY_POINT_LIST &&
                        draw->topology != GPU_VSH_TOPOLOGY_LINE_LIST;
    /* T860: LINE and POINT need fillModeNonSolid, which the device creation enables when the device has it. */
    const uint32_t polygon_mode = draw->output != NULL ? draw->output->polygon_mode : GPU_VSH_POLYGON_FILL;
    if (polygon_mode != GPU_VSH_POLYGON_FILL && !ctx->native.fill_mode_non_solid) {
        return GPU_ERR_ARGUMENT;
    }
    const VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = gpu_vsh_vulkan_polygon_mode(polygon_mode),
        .depthBiasEnable = biased ? VK_TRUE : VK_FALSE,
        .depthBiasConstantFactor = biased ? draw->output->depth_bias_constant : 0.0f,
        .depthBiasSlopeFactor = biased ? draw->output->depth_bias_slope : 0.0f,
        .cullMode = cull_mode,
        .frontFace = front_face,
        .lineWidth = draw->topology == GPU_VSH_TOPOLOGY_LINE_LIST ? draw->line_width : 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .minSampleShading = 1.0f,
    };
    VkPipelineColorBlendAttachmentState blend_attachment = {
        .blendEnable = VK_FALSE,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1u,
        .pAttachments = &blend_attachment,
    };
    if (draw->output != NULL) {
        const gpu_vsh_output *output = draw->output;
        if ((output->color_write_disable & GPU_VSH_CHANNEL_R) != 0u) {
            blend_attachment.colorWriteMask &= ~(VkColorComponentFlags)VK_COLOR_COMPONENT_R_BIT;
        }
        if ((output->color_write_disable & GPU_VSH_CHANNEL_G) != 0u) {
            blend_attachment.colorWriteMask &= ~(VkColorComponentFlags)VK_COLOR_COMPONENT_G_BIT;
        }
        if ((output->color_write_disable & GPU_VSH_CHANNEL_B) != 0u) {
            blend_attachment.colorWriteMask &= ~(VkColorComponentFlags)VK_COLOR_COMPONENT_B_BIT;
        }
        if ((output->color_write_disable & GPU_VSH_CHANNEL_A) != 0u) {
            blend_attachment.colorWriteMask &= ~(VkColorComponentFlags)VK_COLOR_COMPONENT_A_BIT;
        }
        if (output->blend) {
            blend_attachment.blendEnable = VK_TRUE;
            blend_attachment.srcColorBlendFactor = gpu_vsh_vulkan_blend_factor(output->blend_source);
            blend_attachment.dstColorBlendFactor = gpu_vsh_vulkan_blend_factor(output->blend_destination);
            blend_attachment.colorBlendOp = gpu_vsh_vulkan_blend_op(output->blend_equation);
            blend_attachment.srcAlphaBlendFactor = gpu_vsh_vulkan_blend_factor(output->blend_source);
            blend_attachment.dstAlphaBlendFactor = gpu_vsh_vulkan_blend_factor(output->blend_destination);
            blend_attachment.alphaBlendOp = gpu_vsh_vulkan_blend_op(output->blend_equation);
            memcpy(blend.blendConstants, output->blend_constant, sizeof blend.blendConstants);
        }
    }
    const gpu_vsh_output *state = draw->output;
    const bool use_depth = state != NULL && (state->depth_test || state->stencil_test);
    VkPipelineDepthStencilStateCreateInfo depth_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
    };
    if (use_depth) {
        const VkStencilOpState stencil = {
            .failOp = gpu_vsh_vulkan_stencil_op(state->stencil_fail_op),
            .passOp = gpu_vsh_vulkan_stencil_op(state->stencil_zpass_op),
            .depthFailOp = gpu_vsh_vulkan_stencil_op(state->stencil_zfail_op),
            .compareOp = gpu_vsh_vulkan_compare_op(state->stencil_func),
            .compareMask = state->stencil_compare_mask,
            .writeMask = state->stencil_write_mask,
            .reference = state->stencil_ref,
        };
        depth_state.depthTestEnable = state->depth_test ? VK_TRUE : VK_FALSE;
        depth_state.depthWriteEnable = state->depth_test && state->depth_write ? VK_TRUE : VK_FALSE;
        depth_state.depthCompareOp = gpu_vsh_vulkan_compare_op(state->depth_func);
        depth_state.stencilTestEnable = state->stencil_test ? VK_TRUE : VK_FALSE;
        depth_state.front = stencil;
        depth_state.back = stencil;
        depth_state.maxDepthBounds = 1.0f;
    }
    const VkGraphicsPipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2u,
        .pStages = stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pDepthStencilState = use_depth ? &depth_state : NULL,
        .pColorBlendState = &blend,
        .layout = res->layout,
        .renderPass = res->render_pass,
    };
    return ctx->fn.vkCreateGraphicsPipelines(ctx->native.device, VK_NULL_HANDLE, 1u, &info, NULL,
                                             &res->pipeline) == VK_SUCCESS
               ? GPU_OK : GPU_ERR_VULKAN;
}

static gpu_result render_run(gpu_device *device, uint32_t width, uint32_t height,
                             const float clear_rgba[4], const gpu_vsh_draw *draw,
                             gpu_image *out_image)
{
    vsh_context ctx;
    memset(&ctx, 0, sizeof ctx);
    gpu_device_get_native(device, &ctx.native);
    gpu_result outcome = load_functions(&ctx);
    if (outcome != GPU_OK) {
        return outcome;
    }
    const VkDeviceSize readback_bytes = (VkDeviceSize)width * height * 4u;
    const uint8_t *destination = draw->output != NULL ? draw->output->destination : NULL;
    const bool use_depth =
        draw->output != NULL && (draw->output->depth_test || draw->output->stencil_test);
    const VkDeviceSize depth_bytes = (VkDeviceSize)width * height * 4u;
    const VkDeviceSize stencil_bytes = (VkDeviceSize)width * height;
    outcome = prepare_common(&ctx, draw, draw->fragment);
    if (outcome == GPU_OK && use_depth) {
        outcome = make_buffer(&ctx, &ctx.res.depth_up, depth_bytes + stencil_bytes,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        if (outcome == GPU_OK) {
            outcome = make_buffer(&ctx, &ctx.res.depth_down, depth_bytes + stencil_bytes,
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        }
        if (outcome == GPU_OK) {
            memcpy(ctx.res.depth_up.map, draw->output->depth, (size_t)depth_bytes);
            memcpy((uint8_t *)ctx.res.depth_up.map + depth_bytes, draw->output->stencil,
                   (size_t)stencil_bytes);
        }
    }
    if (outcome == GPU_OK && destination != NULL) {
        outcome = make_buffer(&ctx, &ctx.res.destination, readback_bytes,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        if (outcome == GPU_OK) {
            memcpy(ctx.res.destination.map, destination, (size_t)readback_bytes);
        }
    }
    if (outcome == GPU_OK) {
        outcome = render_target(&ctx, width, height, destination != NULL, use_depth);
    }
    if (outcome == GPU_OK) {
        outcome = render_pipeline(&ctx, draw, width, height);
    }
    if (outcome == GPU_OK) {
        outcome = make_buffer(&ctx, &ctx.res.capture, readback_bytes,
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }
    if (outcome == GPU_OK) {
        outcome = begin_commands(&ctx);
    }
    if (outcome != GPU_OK) {
        release_all(&ctx);
        return outcome;
    }

    vsh_functions *fn = &ctx.fn;
    const VkCommandBuffer commands = ctx.res.commands;
    if (draw->fragment != NULL) {
        record_texture_uploads(&ctx, draw->fragment);
    }
    if (destination != NULL) {
        /* Upload the starting pixels and hand the image to the pass as a colour attachment. */
        const VkImageSubresourceRange range = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                .levelCount = 1u, .layerCount = 1u };
        const VkImageMemoryBarrier to_destination = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = ctx.res.image,
            .subresourceRange = range,
        };
        fn->vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, NULL, 0u, NULL, 1u,
                                 &to_destination);
        const VkBufferImageCopy upload = {
            .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1u },
            .imageExtent = { width, height, 1u },
        };
        fn->vkCmdCopyBufferToImage(commands, ctx.res.destination.buffer, ctx.res.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &upload);
        const VkImageMemoryBarrier to_attachment = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = ctx.res.image,
            .subresourceRange = range,
        };
        fn->vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0u, 0u, NULL, 0u,
                                 NULL, 1u, &to_attachment);
    }
    if (use_depth) {
        /* The depth and stencil the draw starts from, then the image as a depth attachment. */
        const VkImageSubresourceRange depth_range = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
            .levelCount = 1u, .layerCount = 1u };
        const VkImageMemoryBarrier depth_to_destination = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = ctx.res.depth_image,
            .subresourceRange = depth_range,
        };
        fn->vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, NULL, 0u, NULL, 1u,
                                 &depth_to_destination);
        const VkBufferImageCopy depth_upload[2] = {
            {
                .bufferOffset = 0u,
                .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .layerCount = 1u },
                .imageExtent = { width, height, 1u },
            },
            {
                .bufferOffset = depth_bytes,
                .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT, .layerCount = 1u },
                .imageExtent = { width, height, 1u },
            },
        };
        fn->vkCmdCopyBufferToImage(commands, ctx.res.depth_up.buffer, ctx.res.depth_image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2u, depth_upload);
        const VkImageMemoryBarrier depth_to_attachment = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = ctx.res.depth_image,
            .subresourceRange = depth_range,
        };
        fn->vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                     VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                 0u, 0u, NULL, 0u, NULL, 1u, &depth_to_attachment);
    }
    const VkClearValue clear_value = {
        .color = { .float32 = { clear_rgba[0], clear_rgba[1], clear_rgba[2], clear_rgba[3] } },
    };
    const VkRenderPassBeginInfo pass_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = ctx.res.render_pass,
        .framebuffer = ctx.res.framebuffer,
        .renderArea = { .extent = { width, height } },
        .clearValueCount = destination != NULL ? 0u : 1u,
        .pClearValues = destination != NULL ? NULL : &clear_value,
    };
    const VkDeviceSize no_offset = 0u;
    fn->vkCmdBeginRenderPass(commands, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    fn->vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx.res.pipeline);
    fn->vkCmdBindVertexBuffers(commands, 0u, 1u, &ctx.res.vertices.buffer, &no_offset);
    fn->vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx.res.layout, 0u, 1u,
                                &ctx.res.set, 0u, NULL);
    fn->vkCmdDraw(commands, draw->vertex_count, 1u, 0u, 0u); /* ONE draw, every vertex */
    fn->vkCmdEndRenderPass(commands);
    const VkBufferImageCopy copy = {
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1u },
        .imageExtent = { width, height, 1u },
    };
    fn->vkCmdCopyImageToBuffer(commands, ctx.res.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               ctx.res.capture.buffer, 1u, &copy);
    if (use_depth) {
        const VkBufferImageCopy depth_readback[2] = {
            {
                .bufferOffset = 0u,
                .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .layerCount = 1u },
                .imageExtent = { width, height, 1u },
            },
            {
                .bufferOffset = depth_bytes,
                .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT, .layerCount = 1u },
                .imageExtent = { width, height, 1u },
            },
        };
        fn->vkCmdCopyImageToBuffer(commands, ctx.res.depth_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   ctx.res.depth_down.buffer, 2u, depth_readback);
        const VkBufferMemoryBarrier depth_host_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = ctx.res.depth_down.buffer,
            .size = VK_WHOLE_SIZE,
        };
        fn->vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                 0u, 0u, NULL, 1u, &depth_host_barrier, 0u, NULL);
    }
    const VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = ctx.res.capture.buffer,
        .size = VK_WHOLE_SIZE,
    };
    fn->vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0u, 0u, NULL, 1u, &host_barrier, 0u, NULL);
    if (fn->vkEndCommandBuffer(commands) != VK_SUCCESS) {
        outcome = GPU_ERR_VULKAN;
    } else {
        outcome = submit_and_wait(&ctx);
    }
    if (outcome == GPU_OK && use_depth) {
        memcpy(draw->output->depth, ctx.res.depth_down.map, (size_t)depth_bytes);
        memcpy(draw->output->stencil, (const uint8_t *)ctx.res.depth_down.map + depth_bytes,
               (size_t)stencil_bytes);
    }
    if (outcome == GPU_OK) {
        uint8_t *pixels = malloc((size_t)readback_bytes);
        if (!pixels) {
            outcome = GPU_ERR_OUT_OF_MEMORY;
        } else {
            memcpy(pixels, ctx.res.capture.map, (size_t)readback_bytes);
            out_image->pixels = pixels;
            out_image->width = width;
            out_image->height = height;
            out_image->stride_bytes = width * 4u;
        }
    }
    release_all(&ctx);
    return outcome;
}

gpu_result gpu_vsh_render(gpu_device *device, uint32_t width, uint32_t height,
                          const float clear_rgba[4], const gpu_vsh_draw *draw,
                          gpu_image *out_image)
{
    if (!device || !clear_rgba || !out_image || check_draw(draw) != GPU_OK || width == 0u ||
        height == 0u || width > MAX_DIMENSION || height > MAX_DIMENSION ||
        check_output(draw, width, height) != GPU_OK) {
        return GPU_ERR_ARGUMENT;
    }
    out_image->pixels = NULL;
    out_image->width = 0u;
    out_image->height = 0u;
    out_image->stride_bytes = 0u;
    return render_run(device, width, height, clear_rgba, draw, out_image);
}
