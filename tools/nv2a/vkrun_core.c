/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shared implementation of the batch compute runners (see vkrun_core.h). One
 * job per manifest line: load a SPIR-V compute shader, feed it COUNT input
 * vectors through storage buffer binding 0, read COUNT output vectors back
 * from binding 1, with push constant `uint count` and 64 invocations per
 * group. A failing job prints "FAIL <index> <reason>" and the batch carries
 * on, otherwise "OK <index>".
 *
 * The device is by default the SOFTWARE Vulkan implementation (lavapipe), selected by
 * CPU device type or by llvmpipe/lavapipe in the name, and never a GPU: a hardware
 * driver would make the comparisons irreproducible.
 *
 * Opt-in exception (T98, tools/nv2a/corners.py): when the environment variable
 * VKRUN_DEVICE is set, the first device of ANY type whose name contains that text is
 * used instead, and the lavapipe-only ICD default is not forced, so a second device
 * (for example RADV) can be compared with lavapipe. T100 adds the value "hardware",
 * the first device that is not software, so a caller need not know the GPU's name.
 * Unset, behaviour is unchanged.
 *
 * VKRUN_STAGE_VERTEX (T100): the same manifest line (`SPIRV IN OUT COUNT`, 832 floats
 * in and 64 out per record) runs the VERTEX shader form. Per record: the 16 input
 * vec4s become one vertex of a point-list draw (vertex buffer, stride 256, attribute
 * location i at offset 16 * i), the 192 constant vec4s become a std140 uniform block at
 * set 0 binding 0 selected by a dynamic offset, rasterizer discard is on, and
 * VK_EXT_transform_feedback writes whatever the shader declared with xfb_buffer 0 and
 * xfb_stride 256 into out[record * 64]. The shader chooses xfb_offset, the Python
 * driver uses 16 * NV2A output address so the result has the compute shader's layout.
 * Slots the shader does not capture keep the NaN sentinel (or zero with --zero-out).
 */
#define _POSIX_C_SOURCE 200112L

#include "vkrun_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

#define GROUP_SIZE 64u
#define SENTINEL_BITS 0x7fc0dead
#define DEFAULT_ICD "/usr/share/vulkan/icd.d/lvp_icd.json"
#define LINE_MAX_BYTES 4096
#define MAX_JOB_FLOATS (1u << 24)

typedef struct {
    VkrunStage stage;
    const char *device_select; /* VKRUN_DEVICE, NULL: software only, see device_matches() */
    VkInstance instance;
    VkPhysicalDevice physical;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memory;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout pipeline_layout;
    VkDescriptorPool pool;
    VkCommandPool command_pool;
    VkCommandBuffer command_buffer;
    VkFence fence;
    VkRenderPass render_pass;   /* vertex stage only */
    VkFramebuffer framebuffer;  /* vertex stage only */
    int transform_feedback;     /* device has the extension and the feature */
} Context;

typedef struct {
    VkShaderModule module;
    VkPipeline pipeline;
    VkBuffer buffers[3];
    VkDeviceMemory memories[3];
    VkDescriptorSet set;
} Job;

static char failure[256];

static int fail(const char *what, VkResult result)
{
    snprintf(failure, sizeof failure, "%s (VkResult %d)", what, (int)result);
    return -1;
}

static int fail_text(const char *what)
{
    snprintf(failure, sizeof failure, "%s", what);
    return -1;
}

static int device_is_software(const VkPhysicalDeviceProperties *props)
{
    return props->deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
           strstr(props->deviceName, "llvmpipe") != NULL ||
           strstr(props->deviceName, "lavapipe") != NULL;
}

static const char *requested_device(void)
{
    const char *text = getenv("VKRUN_DEVICE");
    return (text != NULL && text[0] != '\0') ? text : NULL;
}

/* NULL selects the software device, "hardware" any other, else a name substring. */
static int device_matches(const VkPhysicalDeviceProperties *props, const char *select)
{
    if (select == NULL) {
        return device_is_software(props);
    }
    if (strcmp(select, "hardware") == 0) {
        return !device_is_software(props);
    }
    return strstr(props->deviceName, select) != NULL;
}

static int create_instance(Context *ctx)
{
    if (ctx->device_select == NULL && getenv("VK_ICD_FILENAMES") == NULL &&
        getenv("VK_DRIVER_FILES") == NULL) {
        setenv("VK_ICD_FILENAMES", DEFAULT_ICD, 1);
    }
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "nv2a-vkrun",
                             .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &app};
    VkResult result = vkCreateInstance(&info, NULL, &ctx->instance);
    return result == VK_SUCCESS ? 0 : fail("vkCreateInstance", result);
}

static int pick_device(Context *ctx)
{
    uint32_t count = 0;
    VkResult result = vkEnumeratePhysicalDevices(ctx->instance, &count, NULL);
    if (result != VK_SUCCESS || count == 0) {
        return fail("no Vulkan physical device", result);
    }
    VkPhysicalDevice *devices = calloc(count, sizeof *devices);
    if (devices == NULL) {
        return fail_text("out of memory");
    }
    vkEnumeratePhysicalDevices(ctx->instance, &count, devices);
    int found = 0;
    for (uint32_t i = 0; i < count && !found; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[i], &props);
        if (device_matches(&props, ctx->device_select)) {
            ctx->physical = devices[i];
            ctx->properties = props;
            found = 1;
        }
    }
    free(devices);
    if (!found) {
        return fail_text(ctx->device_select == NULL
                             ? "no llvmpipe/lavapipe software device among the enumerated ones"
                             : "no device matching VKRUN_DEVICE among the enumerated ones");
    }
    vkGetPhysicalDeviceMemoryProperties(ctx->physical, &ctx->memory);
    return 0;
}

static int find_memory_type(const Context *ctx, uint32_t allowed_bits)
{
    const VkMemoryPropertyFlags want =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < ctx->memory.memoryTypeCount; i++) {
        if ((allowed_bits & (1u << i)) &&
            (ctx->memory.memoryTypes[i].propertyFlags & want) == want) {
            return (int)i;
        }
    }
    return -1;
}

static int has_extension(const Context *ctx, const char *name);

static int create_device(Context *ctx)
{
    const VkQueueFlags wanted_queue =
        ctx->stage == VKRUN_STAGE_VERTEX ? VK_QUEUE_GRAPHICS_BIT : VK_QUEUE_COMPUTE_BIT;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count ? count : 1, sizeof *families);
    if (families == NULL) {
        return fail_text("out of memory");
    }
    vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical, &count, families);
    int family = -1;
    for (uint32_t i = 0; i < count; i++) {
        if (families[i].queueFlags & wanted_queue) {
            family = (int)i;
            break;
        }
    }
    free(families);
    if (family < 0) {
        return fail_text(ctx->stage == VKRUN_STAGE_VERTEX ? "no graphics queue family"
                                                          : "no compute queue family");
    }
    ctx->queue_family = (uint32_t)family;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                          .queueFamilyIndex = ctx->queue_family,
                                          .queueCount = 1,
                                          .pQueuePriorities = &priority};
    static const char *const xfb_extension = "VK_EXT_transform_feedback";
    VkPhysicalDeviceTransformFeedbackFeaturesEXT xfb_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &queue_info};
    if (has_extension(ctx, xfb_extension)) {
        VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                              .pNext = &xfb_features};
        vkGetPhysicalDeviceFeatures2(ctx->physical, &features);
        ctx->transform_feedback = xfb_features.transformFeedback == VK_TRUE;
    }
    if (ctx->stage == VKRUN_STAGE_VERTEX) {
        if (!ctx->transform_feedback) {
            return fail_text("device lacks VK_EXT_transform_feedback (transformFeedback feature)");
        }
        xfb_features.pNext = NULL;
        info.pNext = &xfb_features;
        info.enabledExtensionCount = 1;
        info.ppEnabledExtensionNames = &xfb_extension;
    }
    VkResult result = vkCreateDevice(ctx->physical, &info, NULL, &ctx->device);
    if (result != VK_SUCCESS) {
        return fail("vkCreateDevice", result);
    }
    vkGetDeviceQueue(ctx->device, ctx->queue_family, 0, &ctx->queue);
    return 0;
}

static int create_vertex_layouts(Context *ctx)
{
    VkDescriptorSetLayoutBinding binding = {.binding = 0,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                            .descriptorCount = 1,
                                            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT};
    VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding};
    VkResult result = vkCreateDescriptorSetLayout(ctx->device, &set_info, NULL, &ctx->set_layout);
    if (result != VK_SUCCESS) {
        return fail("vkCreateDescriptorSetLayout", result);
    }
    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                              .setLayoutCount = 1,
                                              .pSetLayouts = &ctx->set_layout};
    result = vkCreatePipelineLayout(ctx->device, &layout_info, NULL, &ctx->pipeline_layout);
    if (result != VK_SUCCESS) {
        return fail("vkCreatePipelineLayout", result);
    }
    VkDescriptorPoolSize pool_size = {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                      .descriptorCount = 1};
    VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                            .maxSets = 1,
                                            .poolSizeCount = 1,
                                            .pPoolSizes = &pool_size};
    result = vkCreateDescriptorPool(ctx->device, &pool_info, NULL, &ctx->pool);
    if (result != VK_SUCCESS) {
        return fail("vkCreateDescriptorPool", result);
    }
    /* A render pass with no attachments: rasterizer discard still needs one. */
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS};
    VkRenderPassCreateInfo pass_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                        .subpassCount = 1,
                                        .pSubpasses = &subpass};
    result = vkCreateRenderPass(ctx->device, &pass_info, NULL, &ctx->render_pass);
    if (result != VK_SUCCESS) {
        return fail("vkCreateRenderPass", result);
    }
    VkFramebufferCreateInfo frame_info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                          .renderPass = ctx->render_pass,
                                          .width = 1,
                                          .height = 1,
                                          .layers = 1};
    result = vkCreateFramebuffer(ctx->device, &frame_info, NULL, &ctx->framebuffer);
    return result == VK_SUCCESS ? 0 : fail("vkCreateFramebuffer", result);
}

static int create_compute_layouts(Context *ctx)
{
    VkDescriptorSetLayoutBinding bindings[2];
    for (uint32_t i = 0; i < 2; i++) {
        bindings[i] = (VkDescriptorSetLayoutBinding){.binding = i,
                                                     .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                     .descriptorCount = 1,
                                                     .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    }
    VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = bindings};
    VkResult result = vkCreateDescriptorSetLayout(ctx->device, &set_info, NULL, &ctx->set_layout);
    if (result != VK_SUCCESS) {
        return fail("vkCreateDescriptorSetLayout", result);
    }
    VkPushConstantRange range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(uint32_t)};
    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                              .setLayoutCount = 1,
                                              .pSetLayouts = &ctx->set_layout,
                                              .pushConstantRangeCount = 1,
                                              .pPushConstantRanges = &range};
    result = vkCreatePipelineLayout(ctx->device, &layout_info, NULL, &ctx->pipeline_layout);
    if (result != VK_SUCCESS) {
        return fail("vkCreatePipelineLayout", result);
    }
    VkDescriptorPoolSize pool_size = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2};
    VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                            .maxSets = 1,
                                            .poolSizeCount = 1,
                                            .pPoolSizes = &pool_size};
    result = vkCreateDescriptorPool(ctx->device, &pool_info, NULL, &ctx->pool);
    return result == VK_SUCCESS ? 0 : fail("vkCreateDescriptorPool", result);
}

static int create_fixed_objects(Context *ctx)
{
    int layouts = ctx->stage == VKRUN_STAGE_VERTEX ? create_vertex_layouts(ctx)
                                                   : create_compute_layouts(ctx);
    if (layouts != 0) {
        return layouts;
    }
    VkResult result;
    VkCommandPoolCreateInfo command_pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                                 .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                                 .queueFamilyIndex = ctx->queue_family};
    result = vkCreateCommandPool(ctx->device, &command_pool_info, NULL, &ctx->command_pool);
    if (result != VK_SUCCESS) {
        return fail("vkCreateCommandPool", result);
    }
    VkCommandBufferAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                         .commandPool = ctx->command_pool,
                                         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                         .commandBufferCount = 1};
    result = vkAllocateCommandBuffers(ctx->device, &alloc, &ctx->command_buffer);
    if (result != VK_SUCCESS) {
        return fail("vkAllocateCommandBuffers", result);
    }
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    result = vkCreateFence(ctx->device, &fence_info, NULL, &ctx->fence);
    return result == VK_SUCCESS ? 0 : fail("vkCreateFence", result);
}

static void destroy_context(Context *ctx)
{
    if (ctx->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(ctx->device);
        vkDestroyFence(ctx->device, ctx->fence, NULL);
        vkDestroyCommandPool(ctx->device, ctx->command_pool, NULL);
        vkDestroyFramebuffer(ctx->device, ctx->framebuffer, NULL);
        vkDestroyRenderPass(ctx->device, ctx->render_pass, NULL);
        vkDestroyDescriptorPool(ctx->device, ctx->pool, NULL);
        vkDestroyPipelineLayout(ctx->device, ctx->pipeline_layout, NULL);
        vkDestroyDescriptorSetLayout(ctx->device, ctx->set_layout, NULL);
        vkDestroyDevice(ctx->device, NULL);
    }
    if (ctx->instance != VK_NULL_HANDLE) {
        vkDestroyInstance(ctx->instance, NULL);
    }
}

static int init_context(Context *ctx, VkrunStage stage)
{
    memset(ctx, 0, sizeof *ctx);
    ctx->stage = stage;
    ctx->device_select = requested_device();
    if (create_instance(ctx) != 0 || pick_device(ctx) != 0 || create_device(ctx) != 0 ||
        create_fixed_objects(ctx) != 0) {
        destroy_context(ctx);
        return -1;
    }
    return 0;
}

static int has_extension(const Context *ctx, const char *name)
{
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(ctx->physical, NULL, &count, NULL);
    VkExtensionProperties *list = calloc(count ? count : 1, sizeof *list);
    if (list == NULL) {
        return 0;
    }
    vkEnumerateDeviceExtensionProperties(ctx->physical, NULL, &count, list);
    int found = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(list[i].extensionName, name) == 0) {
            found = 1;
        }
    }
    free(list);
    return found;
}

static void print_info(const Context *ctx, int brief)
{
    if (brief) {
        printf("%s\n", ctx->properties.deviceName);
        return;
    }
    VkPhysicalDeviceVulkan12Properties props12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 props2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                          .pNext = &props12};
    int have12 = ctx->properties.apiVersion >= VK_API_VERSION_1_2;
    if (have12) {
        vkGetPhysicalDeviceProperties2(ctx->physical, &props2);
    }
    printf("device: %s\n", ctx->properties.deviceName);
    printf("device_type: %s\n", device_is_software(&ctx->properties) ? "software" : "hardware");
    printf("transform_feedback: %d\n", ctx->transform_feedback);
    printf("api_version: %u.%u.%u\n", VK_VERSION_MAJOR(ctx->properties.apiVersion),
           VK_VERSION_MINOR(ctx->properties.apiVersion),
           VK_VERSION_PATCH(ctx->properties.apiVersion));
    printf("max_storage_buffer_range: %u\n", ctx->properties.limits.maxStorageBufferRange);
    printf("extension_VK_KHR_shader_float_controls: %d\n",
           has_extension(ctx, "VK_KHR_shader_float_controls"));
    if (have12) {
        printf("driver: %s\n", props12.driverName);
        printf("shaderSignedZeroInfNanPreserveFloat32: %d\n",
               (int)props12.shaderSignedZeroInfNanPreserveFloat32);
        printf("shaderDenormFlushToZeroFloat32: %d\n", (int)props12.shaderDenormFlushToZeroFloat32);
        printf("shaderDenormPreserveFloat32: %d\n", (int)props12.shaderDenormPreserveFloat32);
    } else {
        printf("vulkan12_properties: unavailable\n");
    }
}

static void destroy_job(const Context *ctx, Job *job)
{
    vkDestroyPipeline(ctx->device, job->pipeline, NULL);
    vkDestroyShaderModule(ctx->device, job->module, NULL);
    for (int i = 0; i < 3; i++) {
        vkDestroyBuffer(ctx->device, job->buffers[i], NULL);
        vkFreeMemory(ctx->device, job->memories[i], NULL);
    }
    if (job->set != VK_NULL_HANDLE) {
        vkResetDescriptorPool(ctx->device, ctx->pool, 0);
    }
    memset(job, 0, sizeof *job);
}

static int read_file(const char *path, void **data, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return fail_text("cannot open input file");
    }
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length < 0) {
        fclose(file);
        return fail_text("cannot size input file");
    }
    *data = malloc(length ? (size_t)length : 1);
    if (*data == NULL) {
        fclose(file);
        return fail_text("out of memory");
    }
    if (fread(*data, 1, (size_t)length, file) != (size_t)length) {
        fclose(file);
        free(*data);
        *data = NULL;
        return fail_text("short read");
    }
    fclose(file);
    *size = (size_t)length;
    return 0;
}

static int make_buffer(const Context *ctx, Job *job, int slot, VkDeviceSize size,
                       VkBufferUsageFlags usage, void **mapped)
{
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size,
                               .usage = usage,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkResult result = vkCreateBuffer(ctx->device, &info, NULL, &job->buffers[slot]);
    if (result != VK_SUCCESS) {
        return fail("vkCreateBuffer", result);
    }
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(ctx->device, job->buffers[slot], &requirements);
    int type = find_memory_type(ctx, requirements.memoryTypeBits);
    if (type < 0) {
        return fail_text("no host-visible coherent memory type");
    }
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                  .allocationSize = requirements.size,
                                  .memoryTypeIndex = (uint32_t)type};
    result = vkAllocateMemory(ctx->device, &alloc, NULL, &job->memories[slot]);
    if (result != VK_SUCCESS) {
        return fail("vkAllocateMemory", result);
    }
    result = vkBindBufferMemory(ctx->device, job->buffers[slot], job->memories[slot], 0);
    if (result != VK_SUCCESS) {
        return fail("vkBindBufferMemory", result);
    }
    result = vkMapMemory(ctx->device, job->memories[slot], 0, VK_WHOLE_SIZE, 0, mapped);
    return result == VK_SUCCESS ? 0 : fail("vkMapMemory", result);
}

static int make_pipeline(const Context *ctx, Job *job, const uint32_t *code, size_t size)
{
    if (size < 20 || size % 4 != 0 || code[0] != 0x07230203u) {
        return fail_text("not a SPIR-V binary (size or magic number)");
    }
    VkShaderModuleCreateInfo module_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                            .codeSize = size,
                                            .pCode = code};
    VkResult result = vkCreateShaderModule(ctx->device, &module_info, NULL, &job->module);
    if (result != VK_SUCCESS) {
        return fail("vkCreateShaderModule", result);
    }
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = job->module,
                  .pName = "main"},
        .layout = ctx->pipeline_layout};
    result = vkCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &pipeline_info, NULL,
                                      &job->pipeline);
    return result == VK_SUCCESS ? 0 : fail("vkCreateComputePipelines", result);
}

static int dispatch(const Context *ctx, Job *job, uint32_t count, uint64_t wait_ns)
{
    VkDescriptorSetAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                         .descriptorPool = ctx->pool,
                                         .descriptorSetCount = 1,
                                         .pSetLayouts = &ctx->set_layout};
    VkResult result = vkAllocateDescriptorSets(ctx->device, &alloc, &job->set);
    if (result != VK_SUCCESS) {
        job->set = VK_NULL_HANDLE;
        return fail("vkAllocateDescriptorSets", result);
    }
    VkDescriptorBufferInfo buffer_infos[2];
    VkWriteDescriptorSet writes[2];
    for (uint32_t i = 0; i < 2; i++) {
        buffer_infos[i] = (VkDescriptorBufferInfo){.buffer = job->buffers[i], .offset = 0,
                                                   .range = VK_WHOLE_SIZE};
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = job->set,
                                           .dstBinding = i,
                                           .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           .pBufferInfo = &buffer_infos[i]};
    }
    vkUpdateDescriptorSets(ctx->device, 2, writes, 0, NULL);

    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkResetCommandBuffer(ctx->command_buffer, 0);
    result = vkBeginCommandBuffer(ctx->command_buffer, &begin);
    if (result != VK_SUCCESS) {
        return fail("vkBeginCommandBuffer", result);
    }
    vkCmdBindPipeline(ctx->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, job->pipeline);
    vkCmdBindDescriptorSets(ctx->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            ctx->pipeline_layout, 0, 1, &job->set, 0, NULL);
    vkCmdPushConstants(ctx->command_buffer, ctx->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof count, &count);
    vkCmdDispatch(ctx->command_buffer, (count + GROUP_SIZE - 1) / GROUP_SIZE, 1, 1);
    /* Make the shader writes visible to the host read after the fence. */
    VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                               .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(ctx->command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
    result = vkEndCommandBuffer(ctx->command_buffer);
    if (result != VK_SUCCESS) {
        return fail("vkEndCommandBuffer", result);
    }
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                           .commandBufferCount = 1,
                           .pCommandBuffers = &ctx->command_buffer};
    vkResetFences(ctx->device, 1, &ctx->fence);
    result = vkQueueSubmit(ctx->queue, 1, &submit, ctx->fence);
    if (result != VK_SUCCESS) {
        return fail("vkQueueSubmit", result);
    }
    result = vkWaitForFences(ctx->device, 1, &ctx->fence, VK_TRUE, wait_ns);
    return result == VK_SUCCESS ? 0 : fail("vkWaitForFences", result);
}

static int write_output(const char *path, const void *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return fail_text("cannot open output file");
    }
    size_t written = fwrite(data, 1, size, file);
    int closed = fclose(file);
    return (written == size && closed == 0) ? 0 : fail_text("short write");
}

static int run_job(const VkrunProfile *profile, const Context *ctx, const char *spirv_path,
                   const char *in_path, const char *out_path, uint32_t count,
                   uint32_t in_floats, uint32_t out_floats, int zero_out)
{
    Job job;
    memset(&job, 0, sizeof job);
    void *code = NULL;
    void *input = NULL;
    size_t code_size = 0;
    size_t input_size = 0;
    int status = -1;

    const uint64_t in_bytes = (uint64_t)count * in_floats * sizeof(float);
    const uint64_t out_bytes = (uint64_t)count * out_floats * sizeof(float);
    if (count == 0 || count > GROUP_SIZE * 65535u) {
        return fail_text("COUNT must be 1..4194240");
    }
    if (in_bytes > ctx->properties.limits.maxStorageBufferRange) {
        return fail_text("input exceeds maxStorageBufferRange");
    }
    if (read_file(spirv_path, &code, &code_size) != 0 || read_file(in_path, &input, &input_size) != 0) {
        goto done;
    }
    if (input_size != in_bytes) {
        snprintf(failure, sizeof failure, "input file size is not COUNT * %u float32", in_floats);
        goto done;
    }
    void *in_map = NULL;
    void *out_map = NULL;
    if (make_pipeline(ctx, &job, code, code_size) != 0 ||
        make_buffer(ctx, &job, 0, in_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &in_map) != 0 ||
        make_buffer(ctx, &job, 1, out_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &out_map) != 0) {
        goto done;
    }
    memcpy(in_map, input, in_bytes);
    if (zero_out) {
        memset(out_map, 0, out_bytes);
    } else {
        uint32_t *words = out_map;
        for (uint64_t i = 0; i < out_bytes / 4; i++) {
            words[i] = SENTINEL_BITS;
        }
    }
    if (dispatch(ctx, &job, count, profile->fence_wait_ns) != 0) {
        goto done;
    }
    status = write_output(out_path, out_map, out_bytes);
done:
    destroy_job(ctx, &job);
    free(code);
    free(input);
    return status;
}

/* ---- vertex stage (T100) ---------------------------------------------------------- */

#define VERTEX_INPUTS 16u
#define VERTEX_BYTES (VERTEX_INPUTS * 16u)  /* v0..v15 as vec4, one vertex */
#define CONSTANT_ROWS 192u
#define CONSTANT_BYTES (CONSTANT_ROWS * 16u)
#define CAPTURE_STRIDE 256u                 /* the shader's xfb_stride, 64 floats */
#define MAX_VERTEX_JOB_RECORDS 65536u

typedef struct {
    PFN_vkCmdBindTransformFeedbackBuffersEXT bind_buffers;
    PFN_vkCmdBeginTransformFeedbackEXT begin;
    PFN_vkCmdEndTransformFeedbackEXT end;
} XfbFunctions;

static int load_xfb(const Context *ctx, XfbFunctions *xfb)
{
    xfb->bind_buffers = (PFN_vkCmdBindTransformFeedbackBuffersEXT)vkGetDeviceProcAddr(
        ctx->device, "vkCmdBindTransformFeedbackBuffersEXT");
    xfb->begin = (PFN_vkCmdBeginTransformFeedbackEXT)vkGetDeviceProcAddr(
        ctx->device, "vkCmdBeginTransformFeedbackEXT");
    xfb->end = (PFN_vkCmdEndTransformFeedbackEXT)vkGetDeviceProcAddr(
        ctx->device, "vkCmdEndTransformFeedbackEXT");
    return (xfb->bind_buffers && xfb->begin && xfb->end)
               ? 0 : fail_text("transform feedback entry points not found");
}

static int make_vertex_pipeline(const Context *ctx, Job *job, const uint32_t *code, size_t size)
{
    if (size < 20 || size % 4 != 0 || code[0] != 0x07230203u) {
        return fail_text("not a SPIR-V binary (size or magic number)");
    }
    VkShaderModuleCreateInfo module_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                            .codeSize = size,
                                            .pCode = code};
    VkResult result = vkCreateShaderModule(ctx->device, &module_info, NULL, &job->module);
    if (result != VK_SUCCESS) {
        return fail("vkCreateShaderModule", result);
    }
    VkVertexInputBindingDescription binding = {
        .binding = 0, .stride = VERTEX_BYTES, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[VERTEX_INPUTS];
    for (uint32_t i = 0; i < VERTEX_INPUTS; i++) {
        attributes[i] = (VkVertexInputAttributeDescription){
            .location = i, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 16u * i};
    }
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = VERTEX_INPUTS,
        .pVertexAttributeDescriptions = attributes};
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST};
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .rasterizerDiscardEnable = VK_TRUE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .lineWidth = 1.0f};
    VkPipelineShaderStageCreateInfo stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                             .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                             .module = job->module,
                                             .pName = "main"};
    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 1,
        .pStages = &stage,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly,
        .pRasterizationState = &raster,
        .layout = ctx->pipeline_layout,
        .renderPass = ctx->render_pass,
        .subpass = 0};
    result = vkCreateGraphicsPipelines(ctx->device, VK_NULL_HANDLE, 1, &pipeline_info, NULL,
                                       &job->pipeline);
    return result == VK_SUCCESS ? 0 : fail("vkCreateGraphicsPipelines", result);
}

/* One draw of one point per record, each with its own constants and capture slot. */
static int draw_records(const Context *ctx, Job *job, uint32_t count, uint64_t wait_ns)
{
    XfbFunctions xfb;
    if (load_xfb(ctx, &xfb) != 0) {
        return -1;
    }
    VkDescriptorSetAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                         .descriptorPool = ctx->pool,
                                         .descriptorSetCount = 1,
                                         .pSetLayouts = &ctx->set_layout};
    VkResult result = vkAllocateDescriptorSets(ctx->device, &alloc, &job->set);
    if (result != VK_SUCCESS) {
        job->set = VK_NULL_HANDLE;
        return fail("vkAllocateDescriptorSets", result);
    }
    VkDescriptorBufferInfo uniform = {.buffer = job->buffers[1], .offset = 0, .range = CONSTANT_BYTES};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = job->set,
                                  .dstBinding = 0,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                  .pBufferInfo = &uniform};
    vkUpdateDescriptorSets(ctx->device, 1, &write, 0, NULL);

    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkResetCommandBuffer(ctx->command_buffer, 0);
    result = vkBeginCommandBuffer(ctx->command_buffer, &begin);
    if (result != VK_SUCCESS) {
        return fail("vkBeginCommandBuffer", result);
    }
    VkRenderPassBeginInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = ctx->render_pass,
                                  .framebuffer = ctx->framebuffer,
                                  .renderArea = {.extent = {1, 1}}};
    vkCmdBeginRenderPass(ctx->command_buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(ctx->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, job->pipeline);
    const VkDeviceSize no_offset = 0;
    vkCmdBindVertexBuffers(ctx->command_buffer, 0, 1, &job->buffers[0], &no_offset);
    const VkDeviceSize capture_size = CAPTURE_STRIDE;
    for (uint32_t record = 0; record < count; record++) {
        const uint32_t dynamic_offset = record * CONSTANT_BYTES;
        vkCmdBindDescriptorSets(ctx->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                ctx->pipeline_layout, 0, 1, &job->set, 1, &dynamic_offset);
        const VkDeviceSize capture_offset = (VkDeviceSize)record * CAPTURE_STRIDE;
        xfb.bind_buffers(ctx->command_buffer, 0, 1, &job->buffers[2], &capture_offset, &capture_size);
        xfb.begin(ctx->command_buffer, 0, 0, NULL, NULL);
        vkCmdDraw(ctx->command_buffer, 1, 1, record, 0);
        xfb.end(ctx->command_buffer, 0, 0, NULL, NULL);
    }
    vkCmdEndRenderPass(ctx->command_buffer);
    VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                               .srcAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT,
                               .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(ctx->command_buffer, VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
    result = vkEndCommandBuffer(ctx->command_buffer);
    if (result != VK_SUCCESS) {
        return fail("vkEndCommandBuffer", result);
    }
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                           .commandBufferCount = 1,
                           .pCommandBuffers = &ctx->command_buffer};
    vkResetFences(ctx->device, 1, &ctx->fence);
    result = vkQueueSubmit(ctx->queue, 1, &submit, ctx->fence);
    if (result != VK_SUCCESS) {
        return fail("vkQueueSubmit", result);
    }
    result = vkWaitForFences(ctx->device, 1, &ctx->fence, VK_TRUE, wait_ns);
    return result == VK_SUCCESS ? 0 : fail("vkWaitForFences", result);
}

static int run_vertex_job(const VkrunProfile *profile, const Context *ctx, const char *spirv_path,
                          const char *in_path, const char *out_path, uint32_t count, int zero_out)
{
    Job job;
    memset(&job, 0, sizeof job);
    void *code = NULL;
    void *input = NULL;
    size_t code_size = 0;
    size_t input_size = 0;
    int status = -1;
    const uint32_t in_floats = profile->fixed_in_floats;
    const uint32_t out_floats = profile->fixed_out_floats;
    if (in_floats != (VERTEX_BYTES + CONSTANT_BYTES) / 4u || out_floats != CAPTURE_STRIDE / 4u) {
        return fail_text("the vertex stage needs fixed 832 in and 64 out floats per record");
    }
    if (count == 0 || count > MAX_VERTEX_JOB_RECORDS) {
        return fail_text("COUNT must be 1..65536 for the vertex stage");
    }
    const uint64_t in_bytes = (uint64_t)count * in_floats * sizeof(float);
    const uint64_t out_bytes = (uint64_t)count * out_floats * sizeof(float);
    if (read_file(spirv_path, &code, &code_size) != 0 || read_file(in_path, &input, &input_size) != 0) {
        goto done;
    }
    if (input_size != in_bytes) {
        snprintf(failure, sizeof failure, "input file size is not COUNT * %u float32", in_floats);
        goto done;
    }
    void *vertex_map = NULL;
    void *uniform_map = NULL;
    void *capture_map = NULL;
    if (make_vertex_pipeline(ctx, &job, code, code_size) != 0 ||
        make_buffer(ctx, &job, 0, (uint64_t)count * VERTEX_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                    &vertex_map) != 0 ||
        make_buffer(ctx, &job, 1, (uint64_t)count * CONSTANT_BYTES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                    &uniform_map) != 0 ||
        make_buffer(ctx, &job, 2, out_bytes, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT,
                    &capture_map) != 0) {
        goto done;
    }
    for (uint32_t record = 0; record < count; record++) {
        const char *row = (const char *)input + (uint64_t)record * in_floats * sizeof(float);
        memcpy((char *)vertex_map + (uint64_t)record * VERTEX_BYTES, row, VERTEX_BYTES);
        memcpy((char *)uniform_map + (uint64_t)record * CONSTANT_BYTES, row + VERTEX_BYTES,
               CONSTANT_BYTES);
    }
    if (zero_out) {
        memset(capture_map, 0, out_bytes);
    } else {
        uint32_t *words = capture_map;
        for (uint64_t i = 0; i < out_bytes / 4; i++) {
            words[i] = SENTINEL_BITS;
        }
    }
    if (draw_records(ctx, &job, count, profile->fence_wait_ns) != 0) {
        goto done;
    }
    status = write_output(out_path, capture_map, out_bytes);
done:
    destroy_job(ctx, &job);
    free(code);
    free(input);
    return status;
}

/* Parse one decimal token into value. Returns 0 on success. */
static int parse_number(const char *text, unsigned long *value)
{
    char *end = NULL;
    if (text == NULL) {
        return -1;
    }
    *value = strtoul(text, &end, 10);
    return (end == text || *end != '\0') ? -1 : 0;
}

static int run_manifest(const VkrunProfile *profile, const Context *ctx, const char *manifest,
                        int zero_out)
{
    FILE *file = fopen(manifest, "r");
    if (file == NULL) {
        fprintf(stderr, "%s: cannot open manifest %s\n", profile->program, manifest);
        return 2;
    }
    char line[LINE_MAX_BYTES];
    unsigned index = 0;
    unsigned failures = 0;
    while (fgets(line, sizeof line, file) != NULL) {
        char *spirv = strtok(line, "\t \r\n");
        if (spirv == NULL) {
            continue;
        }
        char *in_path = strtok(NULL, "\t \r\n");
        char *out_path = strtok(NULL, "\t \r\n");
        unsigned long count = 0;
        unsigned long in_floats = profile->fixed_in_floats;
        unsigned long out_floats = profile->fixed_out_floats;
        int malformed = out_path == NULL || parse_number(strtok(NULL, "\t \r\n"), &count) != 0;
        if (!malformed && profile->fixed_in_floats == 0) {
            malformed = parse_number(strtok(NULL, "\t \r\n"), &in_floats) != 0 ||
                        parse_number(strtok(NULL, "\t \r\n"), &out_floats) != 0 ||
                        in_floats == 0 || in_floats > MAX_JOB_FLOATS ||
                        out_floats == 0 || out_floats > MAX_JOB_FLOATS;
        }
        if (malformed) {
            printf("FAIL %u malformed manifest line\n", index);
            failures++;
        } else if ((profile->stage == VKRUN_STAGE_VERTEX
                        ? run_vertex_job(profile, ctx, spirv, in_path, out_path, (uint32_t)count,
                                         zero_out)
                        : run_job(profile, ctx, spirv, in_path, out_path, (uint32_t)count,
                                  (uint32_t)in_floats, (uint32_t)out_floats, zero_out)) == 0) {
            printf("OK %u\n", index);
        } else {
            printf("FAIL %u %s\n", index, failure);
            failures++;
        }
        fflush(stdout);
        index++;
    }
    fclose(file);
    return (failures && profile->nonzero_exit_on_job_fail) ? 1 : 0;
}

int vkrun_main(const VkrunProfile *profile, int argc, char **argv)
{
    int info = 0;
    int zero_out = 0;
    const char *manifest = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--info") == 0) {
            info = 1;
        } else if (profile->accept_zero_out && strcmp(argv[i], "--zero-out") == 0) {
            zero_out = 1;
        } else if (manifest == NULL) {
            manifest = argv[i];
        } else {
            manifest = NULL;
            info = 0;
            break;
        }
    }
    if (!info && manifest == NULL) {
        fprintf(stderr, "%s\n", profile->usage);
        return 2;
    }
    Context ctx;
    if (init_context(&ctx, profile->stage) != 0) {
        fprintf(stderr, "%s: %s\n", profile->program, failure);
        return 1;
    }
    int status = 0;
    if (info) {
        print_info(&ctx, profile->brief_info);
    } else {
        status = run_manifest(profile, &ctx, manifest, zero_out);
    }
    destroy_context(&ctx);
    return status;
}
