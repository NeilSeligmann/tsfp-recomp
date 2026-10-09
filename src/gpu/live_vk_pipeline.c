/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See live_vk_pipeline.h and docs/live-pipeline.md. The decisions are live_pipeline.c's (the replay's resolvers), this file
 * builds the Vulkan objects for them and records the draw. Pipelines are built the way gpu_vsh_draw.c's render_pipeline builds
 * its own (same vertex input, same fixed fragment stages, same enum mapping gpu_vsh_vulkan_map.h), with viewport, scissor,
 * blend constants, stencil reference and depth bias DYNAMIC, so one pipeline serves every draw with the same live_pipeline_key.
 */
#define VK_NO_PROTOTYPES

#define _POSIX_C_SOURCE 200809L
#include "live_vk_pipeline.h"
#include "gpu_phase_timing.h"
#include "live_draw_dump.h"
#include "gpu_fog.h"
#include "live_vk_query.h"

#include "gpu_combiner.h"
#include "live_vk_clear_shaders.h"
#include "gpu_vsh_shaders.h"
#include "gpu_vsh_vulkan_map.h"

#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TARGET_FORMAT VK_FORMAT_R8G8B8A8_UNORM
#define GUEST_TARGET_FORMAT VK_FORMAT_B8G8R8A8_UNORM /* T829: the byte order of a guest A8R8G8B8 target */
#define DEPTH_FORMAT VK_FORMAT_D32_SFLOAT_S8_UINT
#define VERTEX_STRIDE_BYTES (GPU_VSH_ATTRIBUTE_FLOATS * 4u)
#define CONSTANT_FILE_BYTES (GPU_VSH_CONSTANT_ROWS * 16u)
#define CONSTANT_BYTES (CONSTANT_FILE_BYTES + 32u)
#define FRAGMENT_BYTES (GPU_VSH_FRAGMENT_VEC4S * 16u)
#define ARENA_ALIGNMENT 256u /* a multiple of every minUniformBufferOffsetAlignment the spec allows */
#define LAYOUT_MASKS (1u << LIVE_VK_TEXTURE_STAGES)

#define LIVE_VK_FUNCS(X) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) \
    X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool) \
    X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateRenderPass) X(vkDestroyRenderPass) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) \
    X(vkCmdBindDescriptorSets) X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetBlendConstants) \
    X(vkCmdSetStencilReference) X(vkCmdSetDepthBias) X(vkCmdClearAttachments) X(vkCmdPushConstants)

#define LIVE_VK_DECLARE(name) PFN_##name name;
typedef struct {
    LIVE_VK_FUNCS(LIVE_VK_DECLARE)
} live_vk_functions;
#undef LIVE_VK_DECLARE

/* A built pipeline, the handle the cache holds. */
typedef struct {
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkDescriptorSetLayout set_layout;
    uint32_t texture_stages;
    uint32_t rewrite_inferences; /* the fragment module rewrite the device stage applied, 0 when none */
    bool stencil;                /* the stencil reference is a dynamic state of this pipeline */
    bool biased;                 /* so is the depth bias */
} live_vk_pipeline;

typedef struct {
    live_vk_renderer *renderer;
    live_vk_pass_kind kind;
} live_vk_pass_context;

struct live_vk_renderer {
    live_vk_device device;
    /* T1247: optional, loaded without failing a device that lacks them */
    PFN_vkCreatePipelineCache create_pipeline_cache;
    PFN_vkDestroyPipelineCache destroy_pipeline_cache;
    PFN_vkGetPipelineCacheData get_pipeline_cache_data;
    VkPipelineCache pipeline_cache;
    char pipeline_cache_path[512];
    uint64_t pipeline_cache_saved_at; /* stats.pipeline_create_calls when the file was last written */
    live_vk_query *query;
    bool query_enabled;
    uint32_t query_slot;
    live_vk_functions fn;
    gpu_pgraph_backend backend;
    VkRenderPass passes[LIVE_VK_PASS_COUNT];
    bool owns_passes;
    VkDescriptorSetLayout set_layouts[LAYOUT_MASKS + 1u];
    VkPipelineLayout pipeline_layouts[LAYOUT_MASKS + 1u];
    VkDescriptorPool pool;
    /* T1339: the draw arena is a chain of host visible blocks, grown on demand (never a refusal for capacity unless the device
     * itself refuses a block). A draw's vertex, constant and fragment bytes always share one block. begin_frame rewinds every block. */
    live_vk_arena_block arena_blocks[LIVE_VK_ARENA_MAX_BLOCKS];
    uint32_t arena_block_count; /* blocks allocated (kept across frames) */
    uint32_t arena_block;       /* the block draws are taking from this frame */
    size_t arena_block_bytes;   /* size of each block made from now on */
    uint32_t arena_max_blocks;  /* LIVE_VK_ARENA_MAX_BLOCKS, or TSFP_LIVE_ARENA_MAX_BLOCKS (1 = the pre T1339 single 48 MiB arena, a control) */
    size_t arena_used;          /* bytes taken this frame over all blocks (padding of skipped block tails excluded) */
    uint64_t frame_index;       /* begin_frame calls so far */
    uint32_t frame_refused;     /* draws refused in the frame in progress */
    uint32_t frame_draws;
    live_pipeline_cache caches[LIVE_VK_PASS_COUNT];
    live_vk_pass_context contexts[LIVE_VK_PASS_COUNT];
    live_pipeline_census census;
    live_vk_stats stats;
    live_vk_texture_hook textures;
    live_vk_target_hook targets;
    VkCommandBuffer (*target_command)(void *context); /* T1206 */
    void *target_command_context;
    bool flip_y;                     /* T828: the backend's flip_y, applied as a negative viewport (r->backend.flip_y is false) */
    VkPipelineLayout clear_layout;   /* T828: push constant colour, no sets */
    VkPipeline clear_pipelines[LIVE_VK_PASS_COUNT][16]; /* by pass kind and colour write mask, built when first needed */
    char clear_refusal[LIVE_PIPELINE_REASON_BYTES];
};

/* --- adapters ---------------------------------------------------------------------------- */

bool live_vk_device_from_window(const gpu_window_native *native, live_vk_device *out)
{
    if (native == NULL || out == NULL || native->device == VK_NULL_HANDLE || native->get_device_proc_addr == NULL ||
        native->queue == VK_NULL_HANDLE || native->command_pool == VK_NULL_HANDLE) {
        return false;
    }
    *out = (live_vk_device){
        .get_device_proc_addr = native->get_device_proc_addr,
        .device = native->device,
        .queue = native->queue,
        .queue_family = native->queue_family,
        .command_pool = native->command_pool,
        .memory_properties = native->memory_properties,
        .negative_viewport = native->negative_viewport,
        .fill_mode_non_solid = native->fill_mode_non_solid,
        .occlusion_query_precise = native->occlusion_query_precise,
        .large_points = native->large_points,
    };
    return true;
}

bool live_vk_device_from_gpu_device(const gpu_device_native *native, live_vk_device *out)
{
    if (native == NULL || out == NULL || native->device == VK_NULL_HANDLE || native->get_device_proc_addr == NULL) {
        return false;
    }
    *out = (live_vk_device){
        .get_device_proc_addr = native->get_device_proc_addr,
        .device = native->device,
        .queue = native->queue,
        .queue_family = native->queue_family,
        .command_pool = native->command_pool,
        .memory_properties = native->memory_properties,
        .negative_viewport = native->negative_viewport,
        .fill_mode_non_solid = native->fill_mode_non_solid,
        .occlusion_query_precise = native->occlusion_query_precise,
        .large_points = native->large_points,
    };
    return true;
}

/* --- setup ------------------------------------------------------------------------------- */

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

/* vkCreateGraphicsPipelines through the renderer's pipeline cache (NULL handle: none), timed for the report. */
static VkResult create_pipeline(live_vk_renderer *r, const VkGraphicsPipelineCreateInfo *info, VkPipeline *pipeline)
{
    const uint64_t start = monotonic_ns();
    const VkResult result = r->fn.vkCreateGraphicsPipelines(r->device.device, r->pipeline_cache, 1u, info, NULL, pipeline);
    const uint64_t spent = monotonic_ns() - start;
    gpu_phase_add(GPU_PHASE_PIPELINE_CREATE, gpu_phase_now() - spent); /* T1289: for the guest frame trace */
    if (result == VK_SUCCESS) {
        r->stats.pipeline_create_calls++;
        r->stats.pipeline_create_ns += spent;
        if (spent > r->stats.pipeline_create_worst_ns) {
            r->stats.pipeline_create_worst_ns = spent;
        }
    }
    return result;
}

static bool load_functions(live_vk_renderer *r)
{
#define LIVE_VK_LOAD(name)                                                                                  \
    r->fn.name = (PFN_##name)r->device.get_device_proc_addr(r->device.device, #name);                       \
    if (r->fn.name == NULL) {                                                                               \
        return false;                                                                                       \
    }
    LIVE_VK_FUNCS(LIVE_VK_LOAD)
#undef LIVE_VK_LOAD
    return true;
}

static void set_error(char *error, size_t error_bytes, const char *format, ...) __attribute__((format(printf, 3, 4)));

#include <stdarg.h>
static void set_error(char *error, size_t error_bytes, const char *format, ...)
{
    if (error == NULL || error_bytes == 0u) {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, error_bytes, format, arguments);
    va_end(arguments);
}

static bool find_memory_type(const VkPhysicalDeviceMemoryProperties *properties, uint32_t type_bits,
                             VkMemoryPropertyFlags required, uint32_t *index)
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

VkFormat live_vk_pass_colour_format(live_vk_pass_kind kind)
{
    return kind == LIVE_VK_PASS_TARGET ? GUEST_TARGET_FORMAT : TARGET_FORMAT;
}

/* Colour (attachment 0) and D32S8 (attachment 1), LOAD and STORE both. OFFSCREEN: RGBA8, layouts left attachment optimal,
 * compatible with gpu_vsh_draw.c's pass (same formats and sample count). TARGET (T829): BGRA8, the colour layout GENERAL,
 * which is the layout of every T793 target image (so a draw leaves it sampleable and blittable with no transition). */
static bool make_pass(live_vk_renderer *r, live_vk_pass_kind kind)
{
    const VkFormat colour_format = live_vk_pass_colour_format(kind);
    const bool guest = kind == LIVE_VK_PASS_TARGET;
    const VkImageLayout colour_layout = guest ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const VkAttachmentDescription attachments[2] = {
        {
            .format = colour_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = colour_layout,
            .finalLayout = colour_layout,
        },
        {
            .format = DEPTH_FORMAT,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        },
    };
    const VkAttachmentReference color = {0u, colour_layout};
    const VkAttachmentReference depth = {1u, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    const VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &color,
        .pDepthStencilAttachment = &depth,
    };
    const VkPipelineStageFlags stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    const VkSubpassDependency dependencies[2] = {
        {VK_SUBPASS_EXTERNAL, 0u, stages | VK_PIPELINE_STAGE_TRANSFER_BIT, stages,
         VK_ACCESS_TRANSFER_WRITE_BIT | access, access, 0u},
        {0u, VK_SUBPASS_EXTERNAL, stages, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
         access, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT, 0u},
    };
    const VkRenderPassCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2u,
        .pAttachments = attachments,
        .subpassCount = 1u,
        .pSubpasses = &subpass,
        .dependencyCount = 2u,
        .pDependencies = dependencies,
    };
    return r->fn.vkCreateRenderPass(r->device.device, &info, NULL, &r->passes[kind]) == VK_SUCCESS;
}

/* Set 0: binding 0 the vertex constants, binding 1 the combiner block (only with a combiner), 2 + n a sampler per sampled
 * stage. One layout per sampled stage mask with a combiner, plus one without, built when first needed. */
static bool build_layout(live_vk_renderer *r, uint32_t texture_stages, bool combiner, VkPipelineLayout *layout,
                         VkDescriptorSetLayout *set_layout)
{
    VkDescriptorSetLayoutBinding bindings[2u + LIVE_VK_TEXTURE_STAGES];
    uint32_t count = 0u;
    bindings[count++] = (VkDescriptorSetLayoutBinding){0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
                                                       VK_SHADER_STAGE_VERTEX_BIT, NULL};
    if (combiner) {
        bindings[count++] = (VkDescriptorSetLayoutBinding){1u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
                                                           VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        for (uint32_t stage = 0u; stage < LIVE_VK_TEXTURE_STAGES; stage++) {
            if ((texture_stages >> stage) & 1u) {
                bindings[count++] = (VkDescriptorSetLayoutBinding){2u + stage, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                                   1u, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
            }
        }
    }
    const VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = count,
        .pBindings = bindings,
    };
    if (r->fn.vkCreateDescriptorSetLayout(r->device.device, &set_info, NULL, set_layout) != VK_SUCCESS) {
        return false;
    }
    const VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1u,
        .pSetLayouts = set_layout,
    };
    if (r->fn.vkCreatePipelineLayout(r->device.device, &layout_info, NULL, layout) != VK_SUCCESS) {
        r->fn.vkDestroyDescriptorSetLayout(r->device.device, *set_layout, NULL);
        *set_layout = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

/* Layout slots: [0 .. 15] with a combiner by sampled mask, [16] no combiner. */
#define PLAIN_SLOT LAYOUT_MASKS
static bool get_layout(live_vk_renderer *r, uint32_t texture_stages, bool combiner, VkPipelineLayout *layout,
                       VkDescriptorSetLayout *set_layout)
{
    const uint32_t slot = combiner ? (texture_stages & (LAYOUT_MASKS - 1u)) : PLAIN_SLOT;
    if (r->pipeline_layouts[slot] == VK_NULL_HANDLE &&
        !build_layout(r, combiner ? texture_stages : 0u, combiner, &r->pipeline_layouts[slot], &r->set_layouts[slot])) {
        return false;
    }
    *layout = r->pipeline_layouts[slot];
    *set_layout = r->set_layouts[slot];
    return true;
}

/* --- pipeline create and destroy (the live_pipeline_ops) ---------------------------------- */

static bool make_module(live_vk_renderer *r, const uint32_t *words, size_t word_count, VkShaderModule *module)
{
    const VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = word_count * sizeof(uint32_t),
        .pCode = words,
    };
    return r->fn.vkCreateShaderModule(r->device.device, &info, NULL, module) == VK_SUCCESS;
}

static bool pipeline_create(void *context, const live_pipeline_description *description, const live_pipeline_key *key,
                            void **handle, char *error, size_t error_bytes)
{
    (void)key;
    const live_vk_pass_context *pass = context;
    live_vk_renderer *r = pass->renderer;
    const VkRenderPass render_pass = r->passes[pass->kind];
    const gpu_vsh_output *output = &description->output.output;
    const bool active = description->output.active;
    const bool needs_depth = active && (output->depth_test || output->stencil_test);
    if (render_pass == VK_NULL_HANDLE) {
        set_error(error, error_bytes, "no render pass for the %s target kind",
                  pass->kind == LIVE_VK_PASS_WINDOW ? "window" : "offscreen");
        return false;
    }
    if (needs_depth && pass->kind == LIVE_VK_PASS_WINDOW) {
        set_error(error, error_bytes,
                  "the draw tests depth or stencil but the swapchain pass has no depth attachment (needs an offscreen target)");
        return false;
    }
    if (r->backend.load_module == NULL) {
        set_error(error, error_bytes, "the backend has no vertex module loader");
        return false;
    }
    const uint32_t *words = NULL;
    size_t word_count = 0u;
    if (!r->backend.load_module(r->backend.context, description->program.module, &words, &word_count)) {
        set_error(error, error_bytes, "the module of program %s could not be loaded", description->program.digest);
        return false;
    }
    uint32_t provided = 0u;
    if (description->has_fragment && !gpu_spirv_interface_locations(words, word_count, 3u, &provided)) {
        set_error(error, error_bytes, "the interface of the vertex module of program %s could not be read",
                  description->program.digest);
        return false;
    }
    VkShaderModule vertex_module = VK_NULL_HANDLE;
    VkShaderModule fragment_module = VK_NULL_HANDLE;
    if (!make_module(r, words, word_count, &vertex_module)) {
        set_error(error, error_bytes, "vkCreateShaderModule failed for the vertex module of program %s",
                  description->program.digest);
        return false;
    }
    uint32_t rewrite = 0u;
    uint32_t *defaulted = NULL;
    uint32_t *texeled = NULL;
    bool built = false;
    live_vk_pipeline *result = calloc(1u, sizeof *result);
    if (result == NULL) {
        r->fn.vkDestroyShaderModule(r->device.device, vertex_module, NULL);
        set_error(error, error_bytes, "out of memory");
        return false;
    }
    do {
        if (description->has_fragment) {
            const uint32_t *fragment_words = NULL;
            size_t fragment_count = 0u;
            if (r->backend.load_fragment_module == NULL ||
                !r->backend.load_fragment_module(r->backend.context, description->fragment.module, &fragment_words,
                                                 &fragment_count)) {
                set_error(error, error_bytes, "the module %s of the combiner stage could not be loaded",
                          description->fragment.plan.name);
                break;
            }
            uint32_t needed = 0u;
            if (!gpu_spirv_interface_locations(fragment_words, fragment_count, 1u, &needed)) {
                set_error(error, error_bytes, "the interface of the combiner module %s could not be read",
                          description->fragment.plan.name);
                break;
            }
            if ((needed & ~provided) != 0u) {
                size_t defaulted_count = 0u;
                if ((r->backend.allowed_inferences & GPU_PGRAPH_INFER_OUTPUT_UNWRITTEN_VARYING) != 0u &&
                    gpu_spirv_default_inputs(fragment_words, fragment_count, needed & ~provided, &defaulted,
                                             &defaulted_count)) {
                    rewrite |= GPU_PGRAPH_INFER_OUTPUT_UNWRITTEN_VARYING;
                    fragment_words = defaulted;
                    fragment_count = defaulted_count;
                } else {
                    set_error(error, error_bytes,
                              "the combiner reads varyings 0x%X but the program %s writes only 0x%X (one bit per "
                              "location)%s",
                              (unsigned)needed, description->program.digest, (unsigned)provided,
                              (r->backend.allowed_inferences & GPU_PGRAPH_INFER_OUTPUT_UNWRITTEN_VARYING) != 0u
                                  ? ", and the fragment module's input could not be defaulted"
                                  : "");
                    break;
                }
            }
            if (description->texel_stages != 0u) {
                float texel_size[LIVE_VK_TEXTURE_STAGES][2];
                for (uint32_t stage = 0u; stage < LIVE_VK_TEXTURE_STAGES; stage++) {
                    texel_size[stage][0] = (float)description->texel_size[stage][0];
                    texel_size[stage][1] = (float)description->texel_size[stage][1];
                }
                size_t texeled_count = 0u;
                if (!gpu_spirv_texel_coordinates(fragment_words, fragment_count, description->texel_stages, texel_size,
                                                 &texeled, &texeled_count)) {
                    set_error(error, error_bytes,
                              "the combiner module %s cannot take texel coordinates for texture stages 0x%X (its sample is "
                              "not one OpImageSampleImplicitLod of a vec2 straight from the sampler)",
                              description->fragment.plan.name, (unsigned)description->texel_stages);
                    break;
                }
                fragment_words = texeled;
                fragment_count = texeled_count;
            }
            if (!make_module(r, fragment_words, fragment_count, &fragment_module)) {
                set_error(error, error_bytes, "vkCreateShaderModule failed for the combiner module %s",
                          description->fragment.plan.name);
                break;
            }
        } else {
            const bool alpha = active && output->alpha_test;
            const uint32_t *fixed = alpha ? gpu_shader_vsh_draw_alpha_frag : gpu_shader_vsh_draw_frag;
            const size_t fixed_count = alpha ? sizeof gpu_shader_vsh_draw_alpha_frag / sizeof(uint32_t)
                                             : sizeof gpu_shader_vsh_draw_frag / sizeof(uint32_t);
            if (!make_module(r, fixed, fixed_count, &fragment_module)) {
                set_error(error, error_bytes, "vkCreateShaderModule failed for the fixed fragment stage");
                break;
            }
        }
        const uint32_t texture_stages = description->has_fragment ? description->fragment.plan.texture_stages : 0u;
        if (!get_layout(r, texture_stages, description->has_fragment, &result->layout, &result->set_layout)) {
            set_error(error, error_bytes, "the descriptor set or pipeline layout could not be created");
            break;
        }
        result->texture_stages = texture_stages;
        result->rewrite_inferences = rewrite;
        const bool alpha_test = active && output->alpha_test;
        const uint32_t alpha_values[2] = {alpha_test ? output->alpha_func : 0u, alpha_test ? output->alpha_ref : 0u};
        const VkSpecializationMapEntry alpha_entries[2] = {{0u, 0u, sizeof(uint32_t)}, {1u, sizeof(uint32_t), sizeof(uint32_t)}};
        const VkSpecializationInfo alpha_specialization = {2u, alpha_entries, sizeof alpha_values, alpha_values};
        const VkPipelineShaderStageCreateInfo stages[2] = {
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_VERTEX_BIT,
                .module = vertex_module,
                .pName = "main",
            },
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = fragment_module,
                .pName = "main",
                .pSpecializationInfo = alpha_test ? &alpha_specialization : NULL,
            },
        };
        VkVertexInputAttributeDescription attributes[GPU_VSH_INPUTS];
        for (uint32_t location = 0u; location < GPU_VSH_INPUTS; location++) {
            attributes[location] = (VkVertexInputAttributeDescription){location, 0u, VK_FORMAT_R32G32B32A32_SFLOAT,
                                                                       16u * location};
        }
        const VkVertexInputBindingDescription binding = {0u, VERTEX_STRIDE_BYTES, VK_VERTEX_INPUT_RATE_VERTEX};
        const VkPipelineVertexInputStateCreateInfo vertex_input = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount = 1u,
            .pVertexBindingDescriptions = &binding,
            .vertexAttributeDescriptionCount = GPU_VSH_INPUTS,
            .pVertexAttributeDescriptions = attributes,
        };
        const VkPipelineInputAssemblyStateCreateInfo assembly = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = gpu_vsh_vulkan_topology(description->topology),
        };
        const VkPipelineViewportStateCreateInfo viewport_state = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1u,
            .scissorCount = 1u,
        };
        VkCullModeFlags cull_mode = VK_CULL_MODE_NONE;
        VkFrontFace front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        if (active) {
            cull_mode = output->cull_mode == GPU_VSH_CULL_FRONT  ? VK_CULL_MODE_FRONT_BIT
                        : output->cull_mode == GPU_VSH_CULL_BACK ? VK_CULL_MODE_BACK_BIT
                                                                 : VK_CULL_MODE_NONE;
            front_face = output->front_clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        }
        /* T502: only triangles are biased, a line or point never depends on an implementation's rule for it */
        result->biased = active && output->depth_bias && description->topology != GPU_VSH_TOPOLOGY_POINT_LIST &&
                         description->topology != GPU_VSH_TOPOLOGY_LINE_LIST;
        result->stencil = active && output->stencil_test;
        const uint32_t polygon_mode = active ? output->polygon_mode : GPU_VSH_POLYGON_FILL;
        if (polygon_mode != GPU_VSH_POLYGON_FILL && !r->device.fill_mode_non_solid) {
            set_error(error, error_bytes,
                      "the front polygon mode is %s, which needs the fillModeNonSolid device feature this device does not have",
                      polygon_mode == GPU_VSH_POLYGON_LINE ? "LINE (0x1B01)" : "POINT (0x1B00)");
            break;
        }
        const VkPipelineRasterizationStateCreateInfo raster = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = gpu_vsh_vulkan_polygon_mode(polygon_mode),
            .depthBiasEnable = result->biased ? VK_TRUE : VK_FALSE,
            .cullMode = cull_mode,
            .frontFace = front_face,
            .lineWidth = 1.0f, /* gpu_pgraph_resolve_primitive accepted only 1.0 */
        };
        const VkPipelineMultisampleStateCreateInfo multisample = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
            .minSampleShading = 1.0f,
        };
        VkPipelineColorBlendAttachmentState blend_attachment = {
            .blendEnable = VK_FALSE,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                              VK_COLOR_COMPONENT_A_BIT,
        };
        if (active) {
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
            }
        }
        const VkPipelineColorBlendStateCreateInfo blend = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1u,
            .pAttachments = &blend_attachment,
        };
        /* A subpass with a depth attachment needs a depth stencil state, off when the draw does not test. */
        VkPipelineDepthStencilStateCreateInfo depth_state = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .maxDepthBounds = 1.0f,
        };
        if (needs_depth) {
            const VkStencilOpState stencil = {
                .failOp = gpu_vsh_vulkan_stencil_op(output->stencil_fail_op),
                .passOp = gpu_vsh_vulkan_stencil_op(output->stencil_zpass_op),
                .depthFailOp = gpu_vsh_vulkan_stencil_op(output->stencil_zfail_op),
                .compareOp = gpu_vsh_vulkan_compare_op(output->stencil_func),
                .compareMask = output->stencil_compare_mask,
                .writeMask = output->stencil_write_mask,
                .reference = 0u, /* dynamic */
            };
            depth_state.depthTestEnable = output->depth_test ? VK_TRUE : VK_FALSE;
            depth_state.depthWriteEnable = output->depth_test && output->depth_write ? VK_TRUE : VK_FALSE;
            depth_state.depthCompareOp = gpu_vsh_vulkan_compare_op(output->depth_func);
            depth_state.stencilTestEnable = output->stencil_test ? VK_TRUE : VK_FALSE;
            depth_state.front = stencil;
            depth_state.back = stencil;
        }
        VkDynamicState dynamic_states[5];
        uint32_t dynamic_count = 0u;
        dynamic_states[dynamic_count++] = VK_DYNAMIC_STATE_VIEWPORT;
        dynamic_states[dynamic_count++] = VK_DYNAMIC_STATE_SCISSOR;
        dynamic_states[dynamic_count++] = VK_DYNAMIC_STATE_BLEND_CONSTANTS;
        if (result->stencil) {
            dynamic_states[dynamic_count++] = VK_DYNAMIC_STATE_STENCIL_REFERENCE;
        }
        if (result->biased) {
            dynamic_states[dynamic_count++] = VK_DYNAMIC_STATE_DEPTH_BIAS;
        }
        const VkPipelineDynamicStateCreateInfo dynamic = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = dynamic_count,
            .pDynamicStates = dynamic_states,
        };
        const VkGraphicsPipelineCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount = 2u,
            .pStages = stages,
            .pVertexInputState = &vertex_input,
            .pInputAssemblyState = &assembly,
            .pViewportState = &viewport_state,
            .pRasterizationState = &raster,
            .pMultisampleState = &multisample,
            .pDepthStencilState = pass->kind != LIVE_VK_PASS_WINDOW ? &depth_state : NULL,
            .pColorBlendState = &blend,
            .pDynamicState = &dynamic,
            .layout = result->layout,
            .renderPass = render_pass,
        };
        if (create_pipeline(r, &info, &result->pipeline) != VK_SUCCESS) {
            set_error(error, error_bytes, "vkCreateGraphicsPipelines failed");
            break;
        }
        built = true;
    } while (0);
    free(defaulted);
    free(texeled);
    r->fn.vkDestroyShaderModule(r->device.device, vertex_module, NULL);
    if (fragment_module != VK_NULL_HANDLE) {
        r->fn.vkDestroyShaderModule(r->device.device, fragment_module, NULL);
    }
    if (!built) {
        free(result);
        return false;
    }
    r->stats.pipelines_created++;
    *handle = result;
    return true;
}

static void pipeline_destroy(void *context, void *handle)
{
    const live_vk_pass_context *pass = context;
    live_vk_pipeline *pipeline = handle;
    pass->renderer->fn.vkDestroyPipeline(pass->renderer->device.device, pipeline->pipeline, NULL);
    free(pipeline);
}

live_pipeline_ops live_vk_renderer_pipeline_ops(live_vk_renderer *renderer, live_vk_pass_kind kind)
{
    return (live_pipeline_ops){pipeline_create, pipeline_destroy, &renderer->contexts[kind]};
}

/* --- renderer ---------------------------------------------------------------------------- */

static void free_arena_block(live_vk_renderer *r, live_vk_arena_block *block)
{
    if (block->map != NULL) {
        r->fn.vkUnmapMemory(r->device.device, block->memory);
    }
    if (block->buffer != VK_NULL_HANDLE) {
        r->fn.vkDestroyBuffer(r->device.device, block->buffer, NULL);
    }
    if (block->memory != VK_NULL_HANDLE) {
        r->fn.vkFreeMemory(r->device.device, block->memory, NULL);
    }
    *block = (live_vk_arena_block){0};
}

/* Make block `r->arena_block_count` of `bytes`. false (nothing kept) when the device refuses it. */
static bool make_arena(live_vk_renderer *r, size_t bytes)
{
    if (r->arena_block_count >= r->arena_max_blocks) {
        return false;
    }
    live_vk_arena_block *block = &r->arena_blocks[r->arena_block_count];
    const VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (r->fn.vkCreateBuffer(r->device.device, &info, NULL, &block->buffer) != VK_SUCCESS) {
        free_arena_block(r, block);
        return false;
    }
    VkMemoryRequirements requirements;
    r->fn.vkGetBufferMemoryRequirements(r->device.device, block->buffer, &requirements);
    uint32_t type = 0u;
    if (!find_memory_type(&r->device.memory_properties, requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &type)) {
        free_arena_block(r, block);
        return false;
    }
    const VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    void *mapped = NULL;
    if (r->fn.vkAllocateMemory(r->device.device, &allocation, NULL, &block->memory) != VK_SUCCESS ||
        r->fn.vkBindBufferMemory(r->device.device, block->buffer, block->memory, 0u) != VK_SUCCESS ||
        r->fn.vkMapMemory(r->device.device, block->memory, 0u, VK_WHOLE_SIZE, 0u, &mapped) != VK_SUCCESS) {
        free_arena_block(r, block);
        return false;
    }
    block->map = mapped;
    block->bytes = bytes;
    block->used = 0u;
    r->arena_block_count++;
    return true;
}

static bool make_pool(live_vk_renderer *r)
{
    const VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2u * LIVE_VK_MAX_DRAWS_PER_FRAME},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, LIVE_VK_TEXTURE_STAGES * LIVE_VK_MAX_DRAWS_PER_FRAME},
    };
    const VkDescriptorPoolCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = LIVE_VK_MAX_DRAWS_PER_FRAME,
        .poolSizeCount = 2u,
        .pPoolSizes = sizes,
    };
    return r->fn.vkCreateDescriptorPool(r->device.device, &info, NULL, &r->pool) == VK_SUCCESS;
}

bool live_vk_renderer_pipeline_cache_open(live_vk_renderer *r, const char *path)
{
    if (r == NULL || path == NULL || strlen(path) >= sizeof r->pipeline_cache_path || r->pipeline_cache != VK_NULL_HANDLE) {
        return false;
    }
    r->create_pipeline_cache = (PFN_vkCreatePipelineCache)r->device.get_device_proc_addr(r->device.device, "vkCreatePipelineCache");
    r->destroy_pipeline_cache = (PFN_vkDestroyPipelineCache)r->device.get_device_proc_addr(r->device.device, "vkDestroyPipelineCache");
    r->get_pipeline_cache_data = (PFN_vkGetPipelineCacheData)r->device.get_device_proc_addr(r->device.device, "vkGetPipelineCacheData");
    if (r->create_pipeline_cache == NULL || r->destroy_pipeline_cache == NULL || r->get_pipeline_cache_data == NULL) {
        return false;
    }
    void *data = NULL;
    size_t size = 0u;
    FILE *file = fopen(path, "rb");
    if (file != NULL) {
        if (fseek(file, 0, SEEK_END) == 0) {
            const long length = ftell(file);
            if (length > 0 && length <= (long)(256u << 20) && fseek(file, 0, SEEK_SET) == 0) {
                data = malloc((size_t)length);
                if (data != NULL && fread(data, 1u, (size_t)length, file) == (size_t)length) {
                    size = (size_t)length;
                } else {
                    free(data);
                    data = NULL;
                }
            }
        }
        fclose(file);
    }
    const VkPipelineCacheCreateInfo info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = size, .pInitialData = data};
    VkResult made = r->create_pipeline_cache(r->device.device, &info, NULL, &r->pipeline_cache);
    if (made != VK_SUCCESS && size != 0u) { /* a blob the driver refuses outright: start empty */
        const VkPipelineCacheCreateInfo empty = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        size = 0u;
        made = r->create_pipeline_cache(r->device.device, &empty, NULL, &r->pipeline_cache);
    }
    free(data);
    if (made != VK_SUCCESS) {
        r->pipeline_cache = VK_NULL_HANDLE;
        return false;
    }
    snprintf(r->pipeline_cache_path, sizeof r->pipeline_cache_path, "%s", path);
    r->stats.pipeline_cache_loaded_bytes = size;
    r->pipeline_cache_saved_at = r->stats.pipeline_create_calls;
    return true;
}

bool live_vk_renderer_pipeline_cache_save(live_vk_renderer *r)
{
    if (r == NULL || r->pipeline_cache == VK_NULL_HANDLE || r->pipeline_cache_path[0] == '\0' ||
        r->stats.pipeline_create_calls == r->pipeline_cache_saved_at) {
        return false;
    }
    size_t size = 0u;
    if (r->get_pipeline_cache_data(r->device.device, r->pipeline_cache, &size, NULL) != VK_SUCCESS || size == 0u) {
        return false;
    }
    void *data = malloc(size);
    if (data == NULL) {
        return false;
    }
    bool written = false;
    if (r->get_pipeline_cache_data(r->device.device, r->pipeline_cache, &size, data) == VK_SUCCESS) {
        char temporary[sizeof r->pipeline_cache_path + 32u];
        snprintf(temporary, sizeof temporary, "%s.%ld.tmp", r->pipeline_cache_path, (long)getpid());
        FILE *file = fopen(temporary, "wb");
        if (file != NULL) {
            written = fwrite(data, 1u, size, file) == size;
            written = fclose(file) == 0 && written;
            written = written && rename(temporary, r->pipeline_cache_path) == 0;
            if (!written) {
                unlink(temporary);
            }
        }
    }
    free(data);
    if (written) {
        r->pipeline_cache_saved_at = r->stats.pipeline_create_calls;
        r->stats.pipeline_cache_saved_bytes = size;
        r->stats.pipeline_cache_saves++;
    }
    return written;
}

void live_vk_renderer_destroy(live_vk_renderer *r)
{
    if (r == NULL) {
        return;
    }
    (void)live_vk_renderer_pipeline_cache_save(r);
    for (int kind = 0; kind < LIVE_VK_PASS_COUNT; kind++) {
        live_pipeline_cache_destroy(&r->caches[kind]);
    }
    live_pipeline_census_free(&r->census);
    for (int kind = 0; kind < LIVE_VK_PASS_COUNT; kind++) {
        for (uint32_t mask = 0u; mask < 16u; mask++) {
            if (r->clear_pipelines[kind][mask] != VK_NULL_HANDLE) {
                r->fn.vkDestroyPipeline(r->device.device, r->clear_pipelines[kind][mask], NULL);
            }
        }
    }
    if (r->clear_layout != VK_NULL_HANDLE) {
        r->fn.vkDestroyPipelineLayout(r->device.device, r->clear_layout, NULL);
    }
    for (uint32_t slot = 0u; slot <= PLAIN_SLOT; slot++) {
        if (r->pipeline_layouts[slot] != VK_NULL_HANDLE) {
            r->fn.vkDestroyPipelineLayout(r->device.device, r->pipeline_layouts[slot], NULL);
        }
        if (r->set_layouts[slot] != VK_NULL_HANDLE) {
            r->fn.vkDestroyDescriptorSetLayout(r->device.device, r->set_layouts[slot], NULL);
        }
    }
    if (r->pool != VK_NULL_HANDLE) {
        r->fn.vkDestroyDescriptorPool(r->device.device, r->pool, NULL);
    }
    for (uint32_t block = 0u; block < LIVE_VK_ARENA_MAX_BLOCKS; block++) {
        free_arena_block(r, &r->arena_blocks[block]);
    }
    for (int kind = LIVE_VK_PASS_OFFSCREEN; r->owns_passes && kind < LIVE_VK_PASS_COUNT; kind++) {
        if (r->passes[kind] != VK_NULL_HANDLE) {
            r->fn.vkDestroyRenderPass(r->device.device, r->passes[kind], NULL);
        }
    }
    if (r->pipeline_cache != VK_NULL_HANDLE) {
        r->destroy_pipeline_cache(r->device.device, r->pipeline_cache, NULL);
    }
    free(r);
}

live_vk_renderer *live_vk_renderer_create(const live_vk_device *device, VkRenderPass window_pass,
                                          const gpu_pgraph_backend *backend, char *error, size_t error_bytes)
{
    if (device == NULL || backend == NULL || device->device == VK_NULL_HANDLE || device->get_device_proc_addr == NULL) {
        set_error(error, error_bytes, "live_vk_renderer_create: missing device or backend");
        return NULL;
    }
    if (backend->flip_y && !device->negative_viewport) {
        set_error(error, error_bytes,
                  "flip_y needs a negative viewport height and the device did not enable VK_KHR_maintenance1");
        return NULL;
    }
    live_vk_renderer *r = calloc(1u, sizeof *r);
    if (r == NULL) {
        set_error(error, error_bytes, "out of memory");
        return NULL;
    }
    r->device = *device;
    r->backend = *backend;
    /* T828: the renderer draws straight into the finished image (a negative viewport height reverses the rows), so every
     * resolver runs as if flip_y were off: the rectangle, scissor and winding of the finished image are the unmirrored ones. */
    r->flip_y = backend->flip_y;
    r->backend.flip_y = false;
    r->passes[LIVE_VK_PASS_WINDOW] = window_pass;
    live_pipeline_census_init(&r->census);
    r->arena_block_bytes = LIVE_VK_ARENA_BYTES;
    r->arena_max_blocks = LIVE_VK_ARENA_MAX_BLOCKS;
    const char *block_bytes = getenv("TSFP_LIVE_ARENA_BLOCK_BYTES"); /* a test knob: small blocks make every frame grow the arena */
    if (block_bytes != NULL && atol(block_bytes) >= 1048576L) {
        r->arena_block_bytes = (size_t)atol(block_bytes);
    }
    const char *max_blocks = getenv("TSFP_LIVE_ARENA_MAX_BLOCKS");
    if (max_blocks != NULL && atoi(max_blocks) >= 1 && (unsigned)atoi(max_blocks) < LIVE_VK_ARENA_MAX_BLOCKS) {
        r->arena_max_blocks = (uint32_t)atoi(max_blocks);
    }
    if (!load_functions(r)) {
        set_error(error, error_bytes, "a Vulkan device function the live renderer needs is missing");
    } else if ((r->owns_passes = true) && (!make_pass(r, LIVE_VK_PASS_OFFSCREEN) || !make_pass(r, LIVE_VK_PASS_TARGET))) {
        set_error(error, error_bytes, "the offscreen or the guest target render pass could not be created");
    } else if (!make_arena(r, r->arena_block_bytes)) {
        set_error(error, error_bytes, "the %u byte draw arena could not be created", (unsigned)LIVE_VK_ARENA_BYTES);
    } else if (!make_pool(r)) {
        set_error(error, error_bytes, "the descriptor pool could not be created");
    } else {
        for (int kind = 0; kind < LIVE_VK_PASS_COUNT; kind++) {
            r->contexts[kind] = (live_vk_pass_context){r, (live_vk_pass_kind)kind};
            const live_pipeline_ops ops = live_vk_renderer_pipeline_ops(r, (live_vk_pass_kind)kind);
            live_pipeline_cache_init(&r->caches[kind], &ops);
        }
        return r;
    }
    live_vk_renderer_destroy(r);
    return NULL;
}

void live_vk_renderer_set_texture_hook(live_vk_renderer *r, const live_vk_texture_hook *hook)
{
    r->textures = hook != NULL ? *hook : (live_vk_texture_hook){0};
}

void live_vk_renderer_set_target_command(live_vk_renderer *r, VkCommandBuffer (*command)(void *context), void *context)
{
    r->target_command = command;
    r->target_command_context = context;
}

void live_vk_renderer_set_target_hook(live_vk_renderer *r, const live_vk_target_hook *hook)
{
    r->targets = hook != NULL ? *hook : (live_vk_target_hook){0};
}

VkRenderPass live_vk_renderer_offscreen_pass(const live_vk_renderer *r)
{
    return r->passes[LIVE_VK_PASS_OFFSCREEN];
}

VkRenderPass live_vk_renderer_target_pass(const live_vk_renderer *r)
{
    return r->passes[LIVE_VK_PASS_TARGET];
}

void live_vk_renderer_set_query(live_vk_renderer *r, live_vk_query *query, bool enabled)
{
    r->query = query;
    r->query_enabled = enabled;
}

uint32_t live_vk_renderer_query_slot(const live_vk_renderer *r)
{
    return r != NULL ? r->query_slot : UINT32_MAX;
}

bool live_vk_renderer_set_arena_limits(live_vk_renderer *r, size_t block_bytes, uint32_t max_blocks)
{
    if (r == NULL || block_bytes == 0u || max_blocks == 0u || max_blocks > LIVE_VK_ARENA_MAX_BLOCKS || r->arena_used != 0u ||
        r->frame_index != 0u) {
        return false;
    }
    while (r->arena_block_count != 0u) {
        free_arena_block(r, &r->arena_blocks[--r->arena_block_count]);
    }
    r->arena_block_bytes = block_bytes;
    r->arena_max_blocks = max_blocks;
    return make_arena(r, block_bytes);
}

static void close_frame_stats(live_vk_renderer *r)
{
    if ((uint64_t)r->arena_used > r->stats.arena_peak) {
        r->stats.arena_peak = r->arena_used;
    }
    if (r->frame_refused != 0u) {
        r->stats.frames_with_refusals++;
        if (r->stats.first_refused_frame == 0u) {
            r->stats.first_refused_frame = r->frame_index; /* 1 based */
        }
        if (r->frame_refused > r->stats.most_refused_in_frame) {
            r->stats.most_refused_in_frame = r->frame_refused;
        }
    }
}

void live_vk_renderer_begin_frame(live_vk_renderer *r)
{
    close_frame_stats(r);
    r->frame_refused = 0u;
    r->frame_index++;
    r->arena_used = 0u;
    r->arena_block = 0u;
    for (uint32_t block = 0u; block < r->arena_block_count; block++) {
        r->arena_blocks[block].used = 0u;
    }
    r->frame_draws = 0u;
    if (r->pool != VK_NULL_HANDLE) {
        (void)r->fn.vkResetDescriptorPool(r->device.device, r->pool, 0u);
    }
}

live_vk_stats live_vk_renderer_stats(const live_vk_renderer *r)
{
    live_vk_renderer copy = *r; /* the frame in progress counts, without closing it */
    close_frame_stats(&copy);
    live_vk_stats stats = copy.stats;
    stats.arena_blocks = r->arena_block_count;
    for (uint32_t block = 0u; block < r->arena_block_count; block++) {
        stats.arena_capacity += r->arena_blocks[block].bytes;
    }
    stats.frames = r->frame_index;
    return stats;
}

const live_pipeline_census *live_vk_renderer_census(const live_vk_renderer *r)
{
    return &r->census;
}

const live_pipeline_cache *live_vk_renderer_cache(const live_vk_renderer *r, live_vk_pass_kind kind)
{
    return &r->caches[kind];
}

/* --- the draw ---------------------------------------------------------------------------- */

static size_t align_arena(size_t bytes)
{
    return (bytes + ARENA_ALIGNMENT - 1u) & ~(size_t)(ARENA_ALIGNMENT - 1u);
}

/* T1339: room for one draw (`vertex_bytes`, the constants and, when `fragment`, the fragment constants) in ONE block, the next
 * block made when the current one is full. false only when a single draw is larger than a block or the device refuses a new
 * block. The offsets are into *block. */
static bool arena_take_draw(live_vk_renderer *r, size_t vertex_bytes, bool fragment, live_vk_arena_block **block,
                            size_t *vertex_offset, size_t *constant_offset, size_t *fragment_offset)
{
    const size_t vertex_span = align_arena(vertex_bytes);
    const size_t constant_span = align_arena(CONSTANT_BYTES);
    const size_t total = vertex_span + constant_span + (fragment ? align_arena(FRAGMENT_BYTES) : 0u);
    for (;;) {
        /* a draw larger than the usual block gets a block of its own size (a 65536 vertex draw is 16 MiB of vertices) */
        if (r->arena_block >= r->arena_block_count && !make_arena(r, total > r->arena_block_bytes ? total : r->arena_block_bytes)) {
            return false;
        }
        live_vk_arena_block *current = &r->arena_blocks[r->arena_block];
        if (current->bytes - current->used >= total) {
            *block = current;
            *vertex_offset = current->used;
            *constant_offset = current->used + vertex_span;
            *fragment_offset = *constant_offset + constant_span;
            current->used += total;
            r->arena_used += total;
            return true;
        }
        r->arena_block++;
    }
}

static bool refuse(live_vk_renderer *r, size_t draw, live_pipeline_stage stage, bool selected, const char *reason,
                   char *error, size_t error_bytes)
{
    char kept[LIVE_PIPELINE_REASON_BYTES];
    snprintf(kept, sizeof kept, "%s", reason); /* `reason` may be `error` itself */
    reason = kept;
    r->stats.refused++;
    r->frame_refused++;
    live_draw_dump_line("R draw=%zu stage=%d selected=%d reason=%s", draw, (int)stage, selected ? 1 : 0, reason);
    if (selected) {
        live_pipeline_census_refuse_selected(&r->census, draw, stage, GPU_PGRAPH_ERR_DEVICE, reason);
    } else {
        live_pipeline_census_refuse(&r->census, draw, stage, GPU_PGRAPH_ERR_DEVICE, reason);
    }
    set_error(error, error_bytes, "%s", reason);
    return false;
}

/* T1226: one full draw (program, constants, the first expanded triangles) as JSON for tools/nv2a/interp.py, TSFP_LIVE_DRAW_DUMP_FULL=PATH,
 * the first culled strip of at least 200 vertices. Observation only. */
static void dump_full(const gpu_pgraph_state *state, const gpu_pgraph_assembled *assembled, uint32_t cull_enable, uint32_t primitive)
{
    static unsigned written;
    const char *path = getenv("TSFP_LIVE_DRAW_DUMP_FULL");
    const char *limit_text = getenv("TSFP_LIVE_DRAW_DUMP_FULL_COUNT");
    const unsigned limit = limit_text != NULL ? (unsigned)atoi(limit_text) : 1u;
    if (written >= limit || path == NULL || cull_enable != 1u || primitive != 6u || assembled->vertex_count < 200u) return;
    static unsigned seen;
    const char *skip = getenv("TSFP_LIVE_DRAW_DUMP_FULL_SKIP");
    if (seen++ < (skip != NULL ? (unsigned)atoi(skip) : 0u)) return;
    FILE *file = fopen(path, "a");
    if (file == NULL) return;
    written++;
    fprintf(file, "{\"program\":[");
    uint32_t slot = state->program_start;
    for (; slot < GPU_PGRAPH_PROGRAM_SLOTS; slot++) {
        fprintf(file, "%s[%u,%u,%u,%u]", slot == state->program_start ? "" : ",", (unsigned)state->program[slot * 4u],
                (unsigned)state->program[slot * 4u + 1u], (unsigned)state->program[slot * 4u + 2u],
                (unsigned)state->program[slot * 4u + 3u]);
        if ((state->program[slot * 4u + 3u] & 1u) != 0u) break;
    }
    fprintf(file, "],\"constants\":[");
    for (uint32_t i = 0u; i < 192u * 4u; i++) {
        uint32_t word;
        memcpy(&word, &assembled->constants[i], sizeof word);
        fprintf(file, "%s%u", i == 0u ? "" : ",", (unsigned)word);
    }
    fprintf(file, "],\"vertices\":[");
    const uint32_t count = assembled->vertex_count < 360u ? assembled->vertex_count : 360u;
    for (uint32_t v = 0u; v < count; v++) {
        fprintf(file, "%s[", v == 0u ? "" : ",");
        for (uint32_t i = 0u; i < 16u * 4u; i++) {
            uint32_t word;
            memcpy(&word, &assembled->attributes[(size_t)v * 64u + i], sizeof word);
            fprintf(file, "%s%u", i == 0u ? "" : ",", (unsigned)word);
        }
        fprintf(file, "]");
    }
    fprintf(file, "]}\n");
    fclose(file);
}

/* T1226: one D line per accepted draw (see live_draw_dump.h). */
static void dump_draw(size_t draw_index, const gpu_pgraph_draw *draw, const gpu_pgraph_state *state,
                      const gpu_pgraph_assembled *assembled, const live_pipeline_selection *selection)
{
    uint32_t program_hash = 2166136261u;
    for (uint32_t i = 0u; i < GPU_PGRAPH_PROGRAM_SLOTS * 4u; i++) {
        program_hash = (program_hash ^ state->program[i]) * 16777619u;
    }
#define OUTW(index) (state->output_written[index] ? (unsigned)state->output[index] : 0xFFFFFFFFu)
    live_draw_dump_line("D draw=%zu prim=%u idx=%u verts=%u vp=%08x cull=%x/%x/%x depth=%x/%x/%x blend=%x/%x/%x alpha=%x/%x/%x "
                        "light=%x/%x/%x fog=%x poff=%x/%x/%x pm=%x/%x tex=%x/%x/%x/%x frag=%d",
                        draw_index, (unsigned)draw->primitive, (unsigned)draw->index_count, (unsigned)assembled->vertex_count,
                        (unsigned)program_hash, OUTW(GPU_PGRAPH_OUT_CULL_ENABLE), OUTW(GPU_PGRAPH_OUT_CULL_FACE),
                        OUTW(GPU_PGRAPH_OUT_FRONT_FACE), OUTW(GPU_PGRAPH_OUT_DEPTH_ENABLE), OUTW(GPU_PGRAPH_OUT_DEPTH_FUNC),
                        OUTW(GPU_PGRAPH_OUT_DEPTH_MASK), OUTW(GPU_PGRAPH_OUT_BLEND_ENABLE), OUTW(GPU_PGRAPH_OUT_BLEND_SFACTOR),
                        OUTW(GPU_PGRAPH_OUT_BLEND_DFACTOR), OUTW(GPU_PGRAPH_OUT_ALPHA_TEST_ENABLE),
                        OUTW(GPU_PGRAPH_OUT_ALPHA_FUNC), OUTW(GPU_PGRAPH_OUT_ALPHA_REF), OUTW(GPU_PGRAPH_OUT_LIGHTING_ENABLE),
                        OUTW(GPU_PGRAPH_OUT_LIGHT_CONTROL), OUTW(GPU_PGRAPH_OUT_LIGHT_ENABLE_MASK),
                        OUTW(GPU_PGRAPH_OUT_FOG_ENABLE), OUTW(GPU_PGRAPH_OUT_POLY_OFFSET_FILL),
                        OUTW(GPU_PGRAPH_OUT_POLY_OFFSET_SCALE), OUTW(GPU_PGRAPH_OUT_POLY_OFFSET_BIAS),
                        OUTW(GPU_PGRAPH_OUT_FRONT_POLYGON_MODE), OUTW(GPU_PGRAPH_OUT_BACK_POLYGON_MODE),
                        OUTW(GPU_PGRAPH_OUT_TEXTURE_CONTROL0), OUTW(GPU_PGRAPH_OUT_TEXTURE_CONTROL0 + 1),
                        OUTW(GPU_PGRAPH_OUT_TEXTURE_CONTROL0 + 2), OUTW(GPU_PGRAPH_OUT_TEXTURE_CONTROL0 + 3),
                        selection->description.has_fragment ? (int)((const live_vk_pipeline *)selection->handle)->texture_stages : -1);
#undef OUTW
    live_draw_dump_line("O draw=%zu active=%d cull_mode=%d front_cw=%d", draw_index,
                        selection->description.output.active ? 1 : 0, (int)selection->description.output.output.cull_mode,
                        selection->description.output.output.front_clockwise ? 1 : 0);
    live_draw_dump_line("V draw=%zu c58=%g,%g,%g,%g c59=%g,%g,%g,%g", draw_index, (double)assembled->constants[58 * 4],
                        (double)assembled->constants[58 * 4 + 1], (double)assembled->constants[58 * 4 + 2],
                        (double)assembled->constants[58 * 4 + 3], (double)assembled->constants[59 * 4],
                        (double)assembled->constants[59 * 4 + 1], (double)assembled->constants[59 * 4 + 2],
                        (double)assembled->constants[59 * 4 + 3]);
    for (uint32_t slot = 0u; slot < GPU_PGRAPH_ATTRIBUTES; slot++) {
        const gpu_pgraph_array *array = &draw->arrays[slot];
        if (!array->format_set) continue;
        const gpu_pgraph_format format = gpu_pgraph_decode_format(array->format);
        if (format.size == 0u) continue;
        const float *first = assembled->attributes + (size_t)slot * 4u;
        live_draw_dump_line("A draw=%zu slot=%u type=%u size=%u stride=%u first=%g,%g,%g,%g", draw_index, (unsigned)slot,
                            (unsigned)format.type, (unsigned)format.size, (unsigned)format.stride, (double)first[0],
                            (double)first[1], (double)first[2], (double)first[3]);
    }
}

static bool record(live_vk_renderer *r, const gpu_pgraph *pgraph, size_t draw_index, const gpu_pgraph_state *state,
                   const live_vk_draw_target *target, char *error, size_t error_bytes)
{
    r->query_slot = UINT32_MAX;
    const gpu_pgraph_draw *draw = gpu_pgraph_draw_at(pgraph, draw_index);
    live_pipeline_selection selection;
    /* The draw hook's extent can differ from the swapchain. Replay records the viewport opt-in
     * in allowed_inferences; preserve an explicit backend setting too. */
    gpu_pgraph_backend draw_backend =
        live_pipeline_draw_backend(&r->backend, draw, target->width, target->height);
    const gpu_pgraph_result selected = live_pipeline_select(&r->caches[target->pass_kind], &r->census, state,
                                                            draw->primitive, draw_index, &r->backend, target->width,
                                                            target->height, &selection);
    if (selected != GPU_PGRAPH_OK) {
        const live_pipeline_refusal *entry = &r->census.refusals[r->census.count - 1u];
        r->stats.refused++;
        set_error(error, error_bytes, "%s", r->census.count != 0u && entry->draw == draw_index ? entry->reason
                                                                                              : "draw refused");
        return false;
    }
    char reason[LIVE_PIPELINE_REASON_BYTES];
    gpu_pgraph_assembled assembled;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    if (gpu_pgraph_assemble_draw(pgraph, draw_index, &draw_backend, &assembled, &report) != GPU_PGRAPH_OK) {
        return refuse(r, draw_index, LIVE_STAGE_VERTEX, true, report.error, error, error_bytes);
    }
    if (assembled.vertex_count == 0u) {
        r->stats.degenerate++;
        gpu_pgraph_assembled_free(&assembled);
        return true;
    }
    if (live_draw_dump_file() != NULL) {
        dump_draw(draw_index, draw, state, &assembled, &selection);
    }
    dump_full(state, &assembled, state->output_written[GPU_PGRAPH_OUT_CULL_ENABLE] ? state->output[GPU_PGRAPH_OUT_CULL_ENABLE] : 0u,
              draw->primitive);
    const live_vk_pipeline *pipeline = selection.handle;
    live_vk_texture_binding bindings[LIVE_VK_TEXTURE_STAGES];
    memset(bindings, 0, sizeof bindings);
    for (uint32_t stage = 0u; stage < LIVE_VK_TEXTURE_STAGES; stage++) {
        if (((pipeline->texture_stages >> stage) & 1u) == 0u) {
            continue;
        }
        reason[0] = '\0';
        if (r->textures.bind == NULL) {
            snprintf(reason, sizeof reason,
                     "the combiner samples texture stage %u and no texture hook is installed (T792): no stand-in is drawn",
                     (unsigned)stage);
        } else if (!r->textures.bind(r->textures.context, draw_index, state, stage, &bindings[stage], reason,
                                     sizeof reason)) {
            if (reason[0] == '\0') {
                snprintf(reason, sizeof reason, "the texture hook refused stage %u", (unsigned)stage);
            }
        } else if (bindings[stage].view == VK_NULL_HANDLE || bindings[stage].sampler == VK_NULL_HANDLE) {
            snprintf(reason, sizeof reason, "the texture hook gave no view or sampler for stage %u", (unsigned)stage);
        }
        if (reason[0] != '\0') {
            gpu_pgraph_assembled_free(&assembled);
            return refuse(r, draw_index, LIVE_STAGE_DEVICE, true, reason, error, error_bytes);
        }
    }
    float raster[4] = {(float)target->width, (float)target->height, 16777215.0f,
                       0.0f};
    if (state->output_written[GPU_PGRAPH_OUT_CLIP_MAX]) {
      const uint32_t word = state->output[GPU_PGRAPH_OUT_CLIP_MAX];
      memcpy(&raster[2], &word, sizeof word);
    }
    if (r->backend.live_raster_modules &&
        !(raster[2] > 0.0f && raster[2] <= FLT_MAX)) {
      gpu_pgraph_assembled_free(&assembled);
      return refuse(r, draw_index, LIVE_STAGE_DEVICE, true,
                    "live raster depth clip maximum is not positive and finite",
                    error, error_bytes);
    }
    if (r->backend.live_raster_modules) {
        /* Pinned xemu SET_POINT_SIZE accepts 0..511 eighth-pixels; vsh.c
         * overrides programmable oPts when point parameters are disabled.
         * The existing fixed-state resolver refuses enabled point parameters. */
        const uint32_t point_word = state->output_written[GPU_PGRAPH_OUT_POINT_SIZE]
                                        ? state->output[GPU_PGRAPH_OUT_POINT_SIZE] : 0u;
        if (point_word > 511u) {
            gpu_pgraph_assembled_free(&assembled);
            return refuse(r, draw_index, LIVE_STAGE_DEVICE, true,
                          "live fixed point size is outside the xemu 0..511 state domain",
                          error, error_bytes);
        }
        raster[3] = point_word == 0u ? 1.0f : (float)point_word / 8.0f;
        if (!r->device.large_points && raster[3] != 1.0f) {
            gpu_pgraph_assembled_free(&assembled);
            return refuse(r, draw_index, LIVE_STAGE_DEVICE, true,
                          "live point size needs the unsupported largePoints feature",
                          error, error_bytes);
        }
    }
    const size_t vertex_bytes = (size_t)assembled.vertex_count * VERTEX_STRIDE_BYTES;
    gpu_fog_control fog = {0};
    if (selection.description.has_fragment && selection.description.fragment.plan.uses_fog) {
        if (!r->backend.live_fog_modules || !gpu_fog_decode(state, &fog, reason, sizeof reason)) {
            gpu_pgraph_assembled_free(&assembled);
            return refuse(r, draw_index, LIVE_STAGE_DEVICE, true,
                          r->backend.live_fog_modules ? reason : "fog module profile is not enabled", error, error_bytes);
        }
    }
    const size_t saved_arena = r->arena_used;
    live_vk_arena_block *block = NULL;
    size_t vertex_offset = 0u;
    size_t constant_offset = 0u;
    size_t fragment_offset = 0u;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!arena_take_draw(r, vertex_bytes, selection.description.has_fragment, &block, &vertex_offset, &constant_offset,
                         &fragment_offset)) {
        r->arena_used = saved_arena;
        gpu_pgraph_assembled_free(&assembled);
        return refuse(r, draw_index, LIVE_STAGE_DEVICE, true,
                      "the per frame draw arena is full (the device refused a new block or the block limit was reached)", error,
                      error_bytes);
    }
    const VkDescriptorSetAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = r->pool,
        .descriptorSetCount = 1u,
        .pSetLayouts = &pipeline->set_layout,
    };
    if (r->frame_draws >= LIVE_VK_MAX_DRAWS_PER_FRAME ||
        r->fn.vkAllocateDescriptorSets(r->device.device, &allocation, &set) != VK_SUCCESS) {
        r->arena_used = saved_arena;
        block->used = vertex_offset; /* give this draw's span back */
        gpu_pgraph_assembled_free(&assembled);
        return refuse(r, draw_index, LIVE_STAGE_DEVICE, true, "the per frame descriptor pool is exhausted", error,
                      error_bytes);
    }
    r->frame_draws++;
    memcpy(block->map + vertex_offset, assembled.attributes, vertex_bytes);
    memcpy(block->map + constant_offset, assembled.constants,
           CONSTANT_FILE_BYTES);
    memcpy(block->map + constant_offset + CONSTANT_FILE_BYTES, raster,
           sizeof raster);
    memcpy(block->map + constant_offset + CONSTANT_FILE_BYTES + 16u, &fog, sizeof fog);
    if (selection.description.has_fragment) {
        memcpy(block->map + fragment_offset, selection.description.fragment.plan.constants, FRAGMENT_BYTES);
    }
    VkWriteDescriptorSet writes[2u + LIVE_VK_TEXTURE_STAGES];
    VkDescriptorBufferInfo buffers[2];
    VkDescriptorImageInfo images[LIVE_VK_TEXTURE_STAGES];
    uint32_t write_count = 0u;
    buffers[0] = (VkDescriptorBufferInfo){block->buffer, constant_offset, CONSTANT_BYTES};
    writes[write_count++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                  .dstSet = set,
                                                  .dstBinding = 0u,
                                                  .descriptorCount = 1u,
                                                  .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                  .pBufferInfo = &buffers[0]};
    if (selection.description.has_fragment) {
        buffers[1] = (VkDescriptorBufferInfo){block->buffer, fragment_offset, FRAGMENT_BYTES};
        writes[write_count++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                      .dstSet = set,
                                                      .dstBinding = 1u,
                                                      .descriptorCount = 1u,
                                                      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                      .pBufferInfo = &buffers[1]};
        for (uint32_t stage = 0u; stage < LIVE_VK_TEXTURE_STAGES; stage++) {
            if (((pipeline->texture_stages >> stage) & 1u) != 0u) {
                images[stage] = (VkDescriptorImageInfo){bindings[stage].sampler, bindings[stage].view,
                                                        bindings[stage].layout != VK_IMAGE_LAYOUT_UNDEFINED
                                                            ? bindings[stage].layout
                                                            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                writes[write_count++] = (VkWriteDescriptorSet){
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = set,
                    .dstBinding = 2u + stage,
                    .descriptorCount = 1u,
                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    .pImageInfo = &images[stage]};
            }
        }
    }
    r->fn.vkUpdateDescriptorSets(r->device.device, write_count, writes, 0u, NULL);
    VkCommandBuffer command = target->command_buffer;
    if (r->target_command != NULL && target->pass_kind == LIVE_VK_PASS_TARGET) {
        const VkCommandBuffer current = r->target_command(r->target_command_context);
        if (current != VK_NULL_HANDLE) {
            command = current; /* T1206: a feedback snapshot reopened the run */
        }
    }
    const live_pipeline_dynamic *dynamic = &selection.dynamic;
    const VkViewport viewport = {0.0f, r->flip_y ? (float)target->height : 0.0f, (float)target->width,
                                 r->flip_y ? -(float)target->height : (float)target->height, 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, {target->width, target->height}};
    if (dynamic->scissor) {
        scissor.offset.x = (int32_t)dynamic->scissor_x;
        scissor.offset.y = (int32_t)dynamic->scissor_y;
        scissor.extent.width = dynamic->scissor_width;
        scissor.extent.height = dynamic->scissor_height;
    }
    const VkDeviceSize vertex_offset_bytes = vertex_offset;
    r->fn.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);
    r->fn.vkCmdSetViewport(command, 0u, 1u, &viewport);
    r->fn.vkCmdSetScissor(command, 0u, 1u, &scissor);
    r->fn.vkCmdSetBlendConstants(command, dynamic->blend_constant);
    if (pipeline->stencil) {
        r->fn.vkCmdSetStencilReference(command, VK_STENCIL_FACE_FRONT_AND_BACK, dynamic->stencil_ref);
    }
    if (pipeline->biased) {
        r->fn.vkCmdSetDepthBias(command, dynamic->depth_bias_constant, 0.0f, dynamic->depth_bias_slope);
    }
    r->fn.vkCmdBindVertexBuffers(command, 0u, 1u, &block->buffer, &vertex_offset_bytes);
    r->fn.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layout, 0u, 1u, &set, 0u, NULL);
    if (r->query_enabled && !live_vk_query_begin(r->query, command, &r->query_slot)) {
        gpu_pgraph_assembled_free(&assembled);
        return refuse(r, draw_index, LIVE_STAGE_DEVICE, false, "precise visibility query unavailable or full", error, error_bytes);
    }
    /* xemu GL reference gl/draw.c: no colour writes and no depth/stencil test is
     * a no-op draw. Preserve the real GPU query bracket, with no raster work,
     * so its completed result matches the measured xemu zero-sample control. */
    const gpu_vsh_output *query_output = &selection.description.output.output;
    const bool query_nop = r->query_enabled && selection.description.output.active &&
                           (query_output->color_write_disable & 15u) == 15u &&
                           !query_output->depth_test && !query_output->stencil_test;
    if (!query_nop) r->fn.vkCmdDraw(command, assembled.vertex_count, 1u, 0u, 0u);
    if (r->query_enabled) live_vk_query_end(r->query, command, r->query_slot);
    r->stats.drawn++;
    r->stats.vertices += assembled.vertex_count;
    r->stats.used_inferences |= selection.description.used_inferences | assembled.used_inferences |
                                pipeline->rewrite_inferences;
    r->census.used_inferences |= assembled.used_inferences | pipeline->rewrite_inferences;
    gpu_pgraph_assembled_free(&assembled);
    return true;
}

bool live_vk_renderer_draw(live_vk_renderer *r, const gpu_pgraph *pgraph, size_t draw_index,
                           VkCommandBuffer window_command_buffer, uint32_t width, uint32_t height, char *error,
                           size_t error_bytes)
{
    if (r == NULL || pgraph == NULL || draw_index >= gpu_pgraph_draw_count(pgraph)) {
        set_error(error, error_bytes, "live_vk_renderer_draw: missing argument or a draw outside the list");
        return false;
    }
    r->query_slot = UINT32_MAX;
    const gpu_pgraph_draw *draw = gpu_pgraph_draw_at(pgraph, draw_index);
    const gpu_pgraph_state *state = gpu_pgraph_snapshot(pgraph, draw->snapshot);
    live_vk_draw_target target = {window_command_buffer, LIVE_VK_PASS_WINDOW, width, height};
    bool hooked = false;
    if (r->targets.begin != NULL) {
        live_vk_draw_target given = {0};
        char reason[LIVE_PIPELINE_REASON_BYTES] = "";
        if (!r->targets.begin(r->targets.context, draw_index, state, &given, reason, sizeof reason)) {
            if (reason[0] == '\0') {
                snprintf(reason, sizeof reason, "the target hook refused the draw");
            }
            return refuse(r, draw_index, LIVE_STAGE_DEVICE, false, reason, error, error_bytes);
        }
        if (given.command_buffer != VK_NULL_HANDLE) {
            target = given;
            hooked = true;
        }
    }
    bool ok;
    if (target.command_buffer == VK_NULL_HANDLE || target.width == 0u || target.height == 0u) {
        ok = refuse(r, draw_index, LIVE_STAGE_DEVICE, false, "no command buffer or no target size for the draw", error,
                    error_bytes);
    } else if (r->passes[target.pass_kind] == VK_NULL_HANDLE) {
        ok = refuse(r, draw_index, LIVE_STAGE_DEVICE, false, "the target kind has no render pass", error, error_bytes);
    } else {
        ok = record(r, pgraph, draw_index, state, &target, error, error_bytes);
    }
    if (hooked && r->targets.end != NULL) {
        r->targets.end(r->targets.context, draw_index, &target);
    }
    return ok;
}

/* --- clears (T828) ----------------------------------------------------------------------- */

/* A full target triangle with `mask` as the colour write mask, for a colour clear whose channel mask is partial. */
static VkPipeline get_clear_pipeline(live_vk_renderer *r, live_vk_pass_kind kind, uint32_t mask)
{
    if (r->clear_pipelines[kind][mask] != VK_NULL_HANDLE) {
        return r->clear_pipelines[kind][mask];
    }
    if (r->clear_layout == VK_NULL_HANDLE) {
        const VkPushConstantRange range = {VK_SHADER_STAGE_FRAGMENT_BIT, 0u, 16u};
        const VkPipelineLayoutCreateInfo layout_info = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pushConstantRangeCount = 1u,
            .pPushConstantRanges = &range,
        };
        if (r->fn.vkCreatePipelineLayout(r->device.device, &layout_info, NULL, &r->clear_layout) != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }
    }
    VkShaderModule vertex_module = VK_NULL_HANDLE;
    VkShaderModule fragment_module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (make_module(r, live_vk_clear_vertex_words, sizeof live_vk_clear_vertex_words / sizeof(uint32_t), &vertex_module) &&
        make_module(r, live_vk_clear_fragment_words, sizeof live_vk_clear_fragment_words / sizeof(uint32_t),
                    &fragment_module)) {
        const VkPipelineShaderStageCreateInfo stages[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0u, VK_SHADER_STAGE_VERTEX_BIT, vertex_module,
             "main", NULL},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0u, VK_SHADER_STAGE_FRAGMENT_BIT, fragment_module,
             "main", NULL},
        };
        const VkPipelineVertexInputStateCreateInfo vertex_input = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        const VkPipelineInputAssemblyStateCreateInfo assembly = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        };
        const VkPipelineViewportStateCreateInfo viewport_state = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1u,
            .scissorCount = 1u,
        };
        const VkPipelineRasterizationStateCreateInfo raster = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .lineWidth = 1.0f,
        };
        const VkPipelineMultisampleStateCreateInfo multisample = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        };
        const VkPipelineDepthStencilStateCreateInfo depth_state = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        };
        const VkPipelineColorBlendAttachmentState attachment = {.colorWriteMask = mask};
        const VkPipelineColorBlendStateCreateInfo blend = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1u,
            .pAttachments = &attachment,
        };
        const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        const VkPipelineDynamicStateCreateInfo dynamic = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2u,
            .pDynamicStates = dynamic_states,
        };
        const VkGraphicsPipelineCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount = 2u,
            .pStages = stages,
            .pVertexInputState = &vertex_input,
            .pInputAssemblyState = &assembly,
            .pViewportState = &viewport_state,
            .pRasterizationState = &raster,
            .pMultisampleState = &multisample,
            .pDepthStencilState = kind != LIVE_VK_PASS_WINDOW ? &depth_state : NULL,
            .pColorBlendState = &blend,
            .pDynamicState = &dynamic,
            .layout = r->clear_layout,
            .renderPass = r->passes[kind],
        };
        if (create_pipeline(r, &info, &pipeline) != VK_SUCCESS) {
            pipeline = VK_NULL_HANDLE;
        }
    }
    if (vertex_module != VK_NULL_HANDLE) {
        r->fn.vkDestroyShaderModule(r->device.device, vertex_module, NULL);
    }
    if (fragment_module != VK_NULL_HANDLE) {
        r->fn.vkDestroyShaderModule(r->device.device, fragment_module, NULL);
    }
    r->clear_pipelines[kind][mask] = pipeline;
    return pipeline;
}

static bool refuse_clear(live_vk_renderer *r, const char *reason, char *error, size_t error_bytes)
{
    snprintf(r->clear_refusal, sizeof r->clear_refusal, "%s", reason);
    r->stats.clears_refused++;
    set_error(error, error_bytes, "%s", r->clear_refusal);
    return false;
}

bool live_vk_renderer_clear(live_vk_renderer *r, const gpu_pgraph *pgraph, size_t clear_index,
                            const live_vk_draw_target *target, char *error, size_t error_bytes)
{
    if (r == NULL || pgraph == NULL || target == NULL || clear_index >= gpu_pgraph_clear_count(pgraph)) {
        set_error(error, error_bytes, "live_vk_renderer_clear: missing argument or a clear outside the list");
        return false;
    }
    if (target->command_buffer == VK_NULL_HANDLE || target->width == 0u || target->height == 0u ||
        r->passes[target->pass_kind] == VK_NULL_HANDLE) {
        return refuse_clear(r, "no command buffer, target size or render pass for the clear", error, error_bytes);
    }
    if ((r->backend.output_groups & GPU_PGRAPH_OUTPUT_CLEAR) == 0u) {
        return refuse_clear(r,
                            "the stream cleared (0x1D94) but the replay's output_groups does not enable the clear "
                            "group: applying nothing would silently ignore state the title set",
                            error, error_bytes);
    }
    gpu_pgraph_clear_resolved resolved;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    if (gpu_pgraph_resolve_clear(gpu_pgraph_clear_at(pgraph, clear_index), &r->backend, target->width, target->height,
                                 &resolved, &report) != GPU_PGRAPH_OK) {
        return refuse_clear(r, report.error, error, error_bytes);
    }
    r->stats.used_inferences |= resolved.used_inferences;
    r->census.used_inferences |= resolved.used_inferences;
    r->stats.clears_applied++;
    const VkClearRect rect = {
        .rect = {{(int32_t)resolved.x_min, (int32_t)resolved.y_min},
                 {resolved.x_max - resolved.x_min + 1u, resolved.y_max - resolved.y_min + 1u}},
        .baseArrayLayer = 0u,
        .layerCount = 1u,
    };
    const uint32_t all_channels = GPU_VSH_CHANNEL_R | GPU_VSH_CHANNEL_G | GPU_VSH_CHANNEL_B | GPU_VSH_CHANNEL_A;
    float colour[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (uint32_t lane = 0u; lane < 4u; lane++) {
        colour[lane] = (float)resolved.rgba[lane] / 255.0f;
    }
    VkClearAttachment attachments[2];
    uint32_t count = 0u;
    bool whole_colour = resolved.colour && resolved.channels == all_channels;
    if (whole_colour) {
        attachments[count] = (VkClearAttachment){.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0u};
        memcpy(attachments[count].clearValue.color.float32, colour, sizeof colour);
        count++;
    }
    bool ok = true;
    if (resolved.depth || resolved.stencil) {
        if (target->pass_kind == LIVE_VK_PASS_WINDOW) {
            ok = refuse_clear(r,
                              "the clear writes depth or stencil but the swapchain pass has no depth attachment (needs an "
                              "offscreen target)",
                              error, error_bytes);
        } else {
            VkClearAttachment depth_attachment = {0};
            depth_attachment.aspectMask = (resolved.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0u) |
                                          (resolved.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
            depth_attachment.clearValue.depthStencil.depth = resolved.z;
            depth_attachment.clearValue.depthStencil.stencil = resolved.stencil_value;
            attachments[count++] = depth_attachment;
        }
    }
    if (count != 0u) {
        r->fn.vkCmdClearAttachments(target->command_buffer, count, attachments, 1u, &rect);
    }
    if (resolved.colour && !whole_colour && resolved.channels != 0u) {
        const VkPipeline pipeline = get_clear_pipeline(r, target->pass_kind, resolved.channels & 0xFu);
        if (pipeline == VK_NULL_HANDLE) {
            return refuse_clear(r, "the masked colour clear pipeline could not be created", error, error_bytes);
        }
        const VkViewport viewport = {0.0f, 0.0f, (float)target->width, (float)target->height, 0.0f, 1.0f};
        r->fn.vkCmdBindPipeline(target->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        r->fn.vkCmdSetViewport(target->command_buffer, 0u, 1u, &viewport);
        r->fn.vkCmdSetScissor(target->command_buffer, 0u, 1u, &rect.rect);
        r->fn.vkCmdPushConstants(target->command_buffer, r->clear_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0u, sizeof colour,
                                 colour);
        r->fn.vkCmdDraw(target->command_buffer, 3u, 1u, 0u, 0u);
        r->stats.clears_masked++;
    }
    return ok;
}

const char *live_vk_renderer_clear_refusal(const live_vk_renderer *r)
{
    return r->clear_refusal;
}
