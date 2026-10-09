/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See gpu_device.h for why this renders offscreen and why the loader is dlopen'd.
 */

/* Must precede the Vulkan header. Without it vulkan_core.h declares the whole API
 * as extern functions, and a typo that referenced one directly would link against
 * the loader we deliberately are not linking against. */
#define VK_NO_PROTOTYPES

#include "gpu_device.h"

#include "gpu_device_native.h"
#include "gpu_shaders.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan_core.h>

/* The function pointer tables.
 *
 * Grouped by the scope each must be resolved from: global entry points come from
 * vkGetInstanceProcAddr with a NULL instance, instance entry points need the
 * instance, and device entry points go through vkGetDeviceProcAddr so the loader
 * can hand back the driver's function instead of a dispatching trampoline. Doing
 * it with macro lists rather than forty near-identical assignments means a
 * forgotten symbol is impossible rather than merely unlikely.
 */
#define GPU_GLOBAL_FUNCS(X) \
    X(vkCreateInstance) \
    X(vkEnumerateInstanceExtensionProperties)

#define GPU_INSTANCE_FUNCS(X) \
    X(vkDestroyInstance) \
    X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkEnumerateDeviceExtensionProperties) \
    X(vkCreateDevice) \
    X(vkGetDeviceProcAddr)

#define GPU_DEVICE_FUNCS(X) \
    X(vkDestroyDevice) \
    X(vkGetDeviceQueue) \
    X(vkDeviceWaitIdle) \
    X(vkQueueSubmit) \
    X(vkQueueWaitIdle) \
    X(vkCreateImage) \
    X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) \
    X(vkBindImageMemory) \
    X(vkCreateImageView) \
    X(vkDestroyImageView) \
    X(vkCreateBuffer) \
    X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) \
    X(vkAllocateMemory) \
    X(vkFreeMemory) \
    X(vkMapMemory) \
    X(vkUnmapMemory) \
    X(vkCreateRenderPass) \
    X(vkDestroyRenderPass) \
    X(vkCreateFramebuffer) \
    X(vkDestroyFramebuffer) \
    X(vkCreateShaderModule) \
    X(vkDestroyShaderModule) \
    X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) \
    X(vkDestroyPipeline) \
    X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) \
    X(vkAllocateCommandBuffers) \
    X(vkFreeCommandBuffers) \
    X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) \
    X(vkCmdBeginRenderPass) \
    X(vkCmdEndRenderPass) \
    X(vkCmdBindPipeline) \
    X(vkCmdDraw) \
    X(vkCmdPipelineBarrier) \
    X(vkCmdCopyImageToBuffer)

#define GPU_DECLARE_FUNC(name) PFN_##name name;

struct gpu_device {
    void *loader_handle;
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
    GPU_GLOBAL_FUNCS(GPU_DECLARE_FUNC)
    GPU_INSTANCE_FUNCS(GPU_DECLARE_FUNC)
    GPU_DEVICE_FUNCS(GPU_DECLARE_FUNC)

    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool command_pool;
    VkPhysicalDeviceMemoryProperties memory_properties;
    uint32_t api_version;
    bool transform_feedback; /* VK_EXT_transform_feedback enabled (T100d) */
    bool negative_viewport;  /* VK_KHR_maintenance1 enabled (T828): a viewport may have a negative height */
    bool occlusion_query_precise;
    bool sampler_anisotropy; /* T1051: actually enabled samplerAnisotropy. */
    bool large_points;
    bool fill_mode_non_solid; /* T860: fillModeNonSolid enabled */
    char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
};

/* The target format. UNORM rather than SRGB so a cleared component reads back as
 * exactly round(value * 255) and a test can assert a byte instead of a range. */
#define GPU_TARGET_FORMAT VK_FORMAT_R8G8B8A8_UNORM
#define GPU_BYTES_PER_PIXEL 4u

/* A ceiling on a single offscreen target. The readback allocation is
 * width * height * 4 bytes on a software rasteriser, so an absurd size is far
 * more likely to be a bug in a caller's arithmetic than a real request. */
#define GPU_MAX_DIMENSION 8192u

const char *gpu_result_string(gpu_result result)
{
    switch (result) {
    case GPU_OK:                      return "ok";
    case GPU_ERR_NO_LOADER:           return "no Vulkan loader on this machine";
    case GPU_ERR_NO_INSTANCE:         return "vkCreateInstance failed";
    case GPU_ERR_NO_PHYSICAL_DEVICE:  return "no Vulkan physical device (no ICD installed)";
    case GPU_ERR_NO_GRAPHICS_QUEUE:   return "no queue family supports graphics";
    case GPU_ERR_NO_DEVICE:           return "vkCreateDevice failed";
    case GPU_ERR_NO_MEMORY_TYPE:      return "no memory type with the required properties";
    case GPU_ERR_VULKAN:              return "a Vulkan call failed";
    case GPU_ERR_OUT_OF_MEMORY:       return "host allocation failed";
    case GPU_ERR_ARGUMENT:            return "invalid argument";
    }
    return "unknown gpu_result";
}

/* dlsym yields an object pointer and C forbids converting one to a function
 * pointer, which -Wpedantic rightly complains about. Copying the bits is the
 * portable spelling of the same intent. */
static PFN_vkVoidFunction load_symbol(void *handle, const char *symbol)
{
    void *address = dlsym(handle, symbol);
    if (!address) {
        return NULL;
    }
    PFN_vkVoidFunction function = NULL;
    memcpy(&function, &address, sizeof function);
    return function;
}

static void *open_loader(void)
{
    /* The versioned name first. It is the one the Vulkan loader guarantees to
     * install, whereas the bare .so is part of the development package and is
     * absent on a machine that can run Vulkan but not build against it. */
    static const char *const candidates[] = { "libvulkan.so.1", "libvulkan.so" };
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        void *handle = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
        if (handle) {
            return handle;
        }
    }
    return NULL;
}

bool gpu_vulkan_available(void)
{
    void *handle = open_loader();
    if (!handle) {
        return false;
    }
    bool have_entry = load_symbol(handle, "vkGetInstanceProcAddr") != NULL;
    dlclose(handle);
    return have_entry;
}

static bool find_memory_type(const VkPhysicalDeviceMemoryProperties *properties,
                             uint32_t type_bits, VkMemoryPropertyFlags required,
                             uint32_t *out_index)
{
    for (uint32_t index = 0; index < properties->memoryTypeCount; index++) {
        if ((type_bits & (1u << index)) == 0u) {
            continue;
        }
        if ((properties->memoryTypes[index].propertyFlags & required) == required) {
            *out_index = index;
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Device bring-up
 * ------------------------------------------------------------------------ */

static gpu_result load_instance_and_device_funcs(gpu_device *device)
{
#define GPU_LOAD_INSTANCE(name)                                                  \
    device->name = (PFN_##name)device->vkGetInstanceProcAddr(device->instance, #name); \
    if (!device->name) {                                                         \
        return GPU_ERR_NO_INSTANCE;                                              \
    }
    GPU_INSTANCE_FUNCS(GPU_LOAD_INSTANCE)
#undef GPU_LOAD_INSTANCE
    return GPU_OK;
}

static gpu_result load_device_funcs(gpu_device *device)
{
#define GPU_LOAD_DEVICE(name)                                                    \
    device->name = (PFN_##name)device->vkGetDeviceProcAddr(device->device, #name);\
    if (!device->name) {                                                         \
        return GPU_ERR_NO_DEVICE;                                                \
    }
    GPU_DEVICE_FUNCS(GPU_LOAD_DEVICE)
#undef GPU_LOAD_DEVICE
    return GPU_OK;
}

/* A software rasteriser by device type or by name, the same rule tools/nv2a/vkrun_core.c uses. */
static bool properties_are_software(const VkPhysicalDeviceProperties *properties)
{
    return properties->deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
           strstr(properties->deviceName, "llvmpipe") != NULL ||
           strstr(properties->deviceName, "lavapipe") != NULL;
}

/* NULL or empty: any device. "hardware": any device that is not software. "software": the
 * software one. Anything else: a substring of the device name (for example "RADV"). */
static bool selector_matches(const char *selector, const VkPhysicalDeviceProperties *properties)
{
    if (selector == NULL || selector[0] == '\0') {
        return true;
    }
    if (strcmp(selector, "hardware") == 0) {
        return !properties_are_software(properties);
    }
    if (strcmp(selector, "software") == 0) {
        return properties_are_software(properties);
    }
    return strstr(properties->deviceName, selector) != NULL;
}

static gpu_result pick_physical_device(gpu_device *device, const char *selector)
{
    uint32_t count = 0;
    if (device->vkEnumeratePhysicalDevices(device->instance, &count, NULL) != VK_SUCCESS) {
        return GPU_ERR_VULKAN;
    }
    if (count == 0u) {
        return GPU_ERR_NO_PHYSICAL_DEVICE;
    }

    VkPhysicalDevice *devices = calloc(count, sizeof *devices);
    if (!devices) {
        return GPU_ERR_OUT_OF_MEMORY;
    }
    if (device->vkEnumeratePhysicalDevices(device->instance, &count, devices) != VK_SUCCESS) {
        free(devices);
        return GPU_ERR_VULKAN;
    }

    /* First device with a graphics queue that matches the selector wins. There is
     * deliberately no scoring by device type: a caller that cares names the device
     * (T100d), and inventing a preference order for the rest would be untested code on
     * the critical path. */
    gpu_result outcome = GPU_ERR_NO_PHYSICAL_DEVICE;
    for (uint32_t i = 0; i < count && outcome != GPU_OK; i++) {
        VkPhysicalDeviceProperties candidate;
        device->vkGetPhysicalDeviceProperties(devices[i], &candidate);
        if (candidate.apiVersion < VK_API_VERSION_1_1 || !selector_matches(selector, &candidate)) {
            continue;
        }
        if (outcome == GPU_ERR_NO_PHYSICAL_DEVICE) {
            outcome = GPU_ERR_NO_GRAPHICS_QUEUE;
        }
        uint32_t family_count = 0;
        device->vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &family_count, NULL);
        if (family_count == 0u) {
            continue;
        }
        VkQueueFamilyProperties *families = calloc(family_count, sizeof *families);
        if (!families) {
            free(devices);
            return GPU_ERR_OUT_OF_MEMORY;
        }
        device->vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &family_count, families);
        for (uint32_t family = 0; family < family_count; family++) {
            if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u) {
                device->physical_device = devices[i];
                device->queue_family = family;
                outcome = GPU_OK;
                break;
            }
        }
        free(families);
    }

    free(devices);
    if (outcome != GPU_OK) {
        return outcome;
    }

    VkPhysicalDeviceProperties properties;
    device->vkGetPhysicalDeviceProperties(device->physical_device, &properties);
    device->api_version = properties.apiVersion;
    /* deviceName is a fixed-size array in the struct, so a plain copy of the whole
     * field cannot truncate and cannot leave us unterminated unless the driver
     * already violated the spec. Force the terminator anyway. */
    memcpy(device->name, properties.deviceName, sizeof device->name);
    device->name[sizeof device->name - 1] = '\0';

    device->vkGetPhysicalDeviceMemoryProperties(device->physical_device,
                                                &device->memory_properties);
    return GPU_OK;
}

gpu_result gpu_device_create(gpu_device **out_device)
{
    return gpu_device_create_selected(NULL, out_device);
}

/* True when the instance lists `name` among its extensions. */
static bool instance_has_extension(const gpu_device *device, const char *name)
{
    uint32_t count = 0u;
    if (device->vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) != VK_SUCCESS || count == 0u) {
        return false;
    }
    VkExtensionProperties *properties = calloc(count, sizeof *properties);
    if (!properties) {
        return false;
    }
    bool found = false;
    if (device->vkEnumerateInstanceExtensionProperties(NULL, &count, properties) == VK_SUCCESS) {
        for (uint32_t i = 0u; i < count && !found; i++) {
            found = strcmp(properties[i].extensionName, name) == 0;
        }
    }
    free(properties);
    return found;
}

static bool physical_device_has_extension(const gpu_device *device, const char *name)
{
    uint32_t count = 0u;
    if (device->vkEnumerateDeviceExtensionProperties(device->physical_device, NULL, &count, NULL) !=
            VK_SUCCESS || count == 0u) {
        return false;
    }
    VkExtensionProperties *properties = calloc(count, sizeof *properties);
    if (!properties) {
        return false;
    }
    bool found = false;
    if (device->vkEnumerateDeviceExtensionProperties(device->physical_device, NULL, &count,
                                                     properties) == VK_SUCCESS) {
        for (uint32_t i = 0u; i < count && !found; i++) {
            found = strcmp(properties[i].extensionName, name) == 0;
        }
    }
    free(properties);
    return found;
}

gpu_result gpu_device_create_selected(const char *selector, gpu_device **out_device)
{
    if (!out_device) {
        return GPU_ERR_ARGUMENT;
    }
    *out_device = NULL;

    gpu_device *device = calloc(1, sizeof *device);
    if (!device) {
        return GPU_ERR_OUT_OF_MEMORY;
    }

    device->loader_handle = open_loader();
    if (!device->loader_handle) {
        free(device);
        return GPU_ERR_NO_LOADER;
    }
    device->vkGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)load_symbol(device->loader_handle, "vkGetInstanceProcAddr");
    if (!device->vkGetInstanceProcAddr) {
        dlclose(device->loader_handle);
        free(device);
        return GPU_ERR_NO_LOADER;
    }

#define GPU_LOAD_GLOBAL(name)                                                    \
    device->name = (PFN_##name)device->vkGetInstanceProcAddr(NULL, #name);       \
    if (!device->name) {                                                         \
        dlclose(device->loader_handle);                                          \
        free(device);                                                            \
        return GPU_ERR_NO_LOADER;                                                \
    }
    GPU_GLOBAL_FUNCS(GPU_LOAD_GLOBAL)
#undef GPU_LOAD_GLOBAL

    const VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "tsfp-recomp",
        .applicationVersion = 1u,
        .pEngineName = "tsfp-gpu",
        .engineVersion = 1u,
        /* 1.0 on purpose. Nothing here needs a later core version, and asking for
         * one would exclude an older loader for no gain. */
        .apiVersion = VK_API_VERSION_1_1,
    };
    /* No surface extension: offscreen rendering needs none, and a surface extension
     * is exactly what would fail on a machine with no display. The one instance
     * extension asked for is VK_KHR_get_physical_device_properties2, only when the
     * loader lists it, because VK_EXT_transform_feedback (T100d) depends on it. */
    static const char *const properties2_extension = "VK_KHR_get_physical_device_properties2";
    const bool want_properties2 = instance_has_extension(device, properties2_extension);
    const VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
        .enabledExtensionCount = want_properties2 ? 1u : 0u,
        .ppEnabledExtensionNames = want_properties2 ? &properties2_extension : NULL,
    };
    if (device->vkCreateInstance(&instance_info, NULL, &device->instance) != VK_SUCCESS) {
        dlclose(device->loader_handle);
        free(device);
        return GPU_ERR_NO_INSTANCE;
    }

    gpu_result outcome = load_instance_and_device_funcs(device);
    if (outcome != GPU_OK) {
        gpu_device_destroy(device);
        return outcome;
    }

    outcome = pick_physical_device(device, selector);
    if (outcome != GPU_OK) {
        gpu_device_destroy(device);
        return outcome;
    }

    const float queue_priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = device->queue_family,
        .queueCount = 1u,
        .pQueuePriorities = &queue_priority,
    };
    /* VK_KHR_maintenance1 is optional too (T828): it makes a negative viewport height legal, which the live renderer uses
     * for flip_y. Part of every attempt below when the device lists it. */
    static const char *const maintenance_extension = "VK_KHR_maintenance1";
    const bool want_maintenance = physical_device_has_extension(device, maintenance_extension);
    static const char *const feedback_extension = "VK_EXT_transform_feedback";
    const char *extensions[2];
    uint32_t extension_count = 0u;
    if (want_maintenance) {
        extensions[extension_count++] = maintenance_extension;
    }
    /* T860: fillModeNonSolid (a LINE or POINT polygon mode) is asked for when the device has it. */
    VkPhysicalDeviceFeatures supported_features;
    memset(&supported_features, 0, sizeof supported_features);
    device->vkGetPhysicalDeviceFeatures(device->physical_device, &supported_features);
    VkPhysicalDeviceFeatures enabled_features;
    memset(&enabled_features, 0, sizeof enabled_features);
    enabled_features.samplerAnisotropy = supported_features.samplerAnisotropy;
    enabled_features.largePoints = supported_features.largePoints;
    enabled_features.fillModeNonSolid = supported_features.fillModeNonSolid;
    enabled_features.occlusionQueryPrecise = supported_features.occlusionQueryPrecise;
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1u,
        .pQueueCreateInfos = &queue_info,
        .pEnabledFeatures = &enabled_features,
        .enabledExtensionCount = extension_count,
        .ppEnabledExtensionNames = extension_count != 0u ? extensions : NULL,
    };
    /* Transform feedback is optional: it is how gpu_vsh_capture reads vertex-stage outputs
     * back exactly (T100d). Asked for only when the instance and the device list it, and
     * dropped again if the driver refuses the feature, so a device without it still comes up. */
    VkPhysicalDeviceTransformFeedbackFeaturesEXT feedback_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,
        .transformFeedback = VK_TRUE,
    };
    VkResult created = VK_ERROR_EXTENSION_NOT_PRESENT;
    if (want_properties2 && physical_device_has_extension(device, feedback_extension)) {
        VkDeviceCreateInfo with_feedback = device_info;
        const char *with_feedback_names[2] = {extensions[0], feedback_extension};
        if (!want_maintenance) {
            with_feedback_names[0] = feedback_extension;
        }
        with_feedback.pNext = &feedback_features;
        with_feedback.enabledExtensionCount = extension_count + 1u;
        with_feedback.ppEnabledExtensionNames = with_feedback_names;
        created = device->vkCreateDevice(device->physical_device, &with_feedback, NULL,
                                         &device->device);
        device->transform_feedback = created == VK_SUCCESS;
    }
    if (created != VK_SUCCESS) {
        device->device = VK_NULL_HANDLE;
        created = device->vkCreateDevice(device->physical_device, &device_info, NULL,
                                         &device->device);
    }
    device->negative_viewport = created == VK_SUCCESS && want_maintenance;
    device->sampler_anisotropy = created == VK_SUCCESS && enabled_features.samplerAnisotropy == VK_TRUE;
    device->large_points = created == VK_SUCCESS && enabled_features.largePoints == VK_TRUE;
    device->fill_mode_non_solid = created == VK_SUCCESS && enabled_features.fillModeNonSolid == VK_TRUE;
    device->occlusion_query_precise = created == VK_SUCCESS && enabled_features.occlusionQueryPrecise == VK_TRUE;
    if (created != VK_SUCCESS) {
        device->device = VK_NULL_HANDLE;
        gpu_device_destroy(device);
        return GPU_ERR_NO_DEVICE;
    }

    outcome = load_device_funcs(device);
    if (outcome != GPU_OK) {
        gpu_device_destroy(device);
        return outcome;
    }

    device->vkGetDeviceQueue(device->device, device->queue_family, 0u, &device->queue);

    const VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = device->queue_family,
    };
    if (device->vkCreateCommandPool(device->device, &pool_info, NULL,
                                    &device->command_pool) != VK_SUCCESS) {
        gpu_device_destroy(device);
        return GPU_ERR_VULKAN;
    }

    *out_device = device;
    return GPU_OK;
}

void gpu_device_destroy(gpu_device *device)
{
    if (!device) {
        return;
    }
    /* Teardown runs on partially built devices too, because every failure path in
     * gpu_device_create funnels here. Each handle is therefore checked rather than
     * assumed, and so is the function pointer that destroys it. */
    if (device->device != VK_NULL_HANDLE) {
        if (device->vkDeviceWaitIdle) {
            device->vkDeviceWaitIdle(device->device);
        }
        if (device->command_pool != VK_NULL_HANDLE && device->vkDestroyCommandPool) {
            device->vkDestroyCommandPool(device->device, device->command_pool, NULL);
        }
        if (device->vkDestroyDevice) {
            device->vkDestroyDevice(device->device, NULL);
        }
    }
    if (device->instance != VK_NULL_HANDLE && device->vkDestroyInstance) {
        device->vkDestroyInstance(device->instance, NULL);
    }
    if (device->loader_handle) {
        dlclose(device->loader_handle);
    }
    free(device);
}

const char *gpu_device_name(const gpu_device *device)
{
    return device ? device->name : "";
}

uint32_t gpu_device_api_version(const gpu_device *device)
{
    return device ? device->api_version : 0u;
}

/* ---------------------------------------------------------------------------
 * Images
 * ------------------------------------------------------------------------ */

void gpu_image_free(gpu_image *image)
{
    if (!image) {
        return;
    }
    free(image->pixels);
    image->pixels = NULL;
    image->width = 0u;
    image->height = 0u;
    image->stride_bytes = 0u;
}

size_t gpu_image_offset(const gpu_image *image, uint32_t x, uint32_t y)
{
    if (!image || x >= image->width || y >= image->height) {
        return 0u;
    }
    return (size_t)y * (size_t)image->stride_bytes + (size_t)x * GPU_BYTES_PER_PIXEL;
}

/* ---------------------------------------------------------------------------
 * The render path
 * ------------------------------------------------------------------------ */

/* Everything one frame needs. Bundled so the single cleanup label can release it
 * without thirty nested conditionals, which on a path with this many creation
 * steps is the difference between obviously-correct and plausibly-leaking. */
typedef struct {
    VkImage image;
    VkDeviceMemory image_memory;
    VkImageView view;
    VkRenderPass render_pass;
    VkFramebuffer framebuffer;
    VkBuffer readback;
    VkDeviceMemory readback_memory;
    VkShaderModule vertex_shader;
    VkShaderModule fragment_shader;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkCommandBuffer commands;
} frame_resources;

static void frame_release(gpu_device *device, frame_resources *frame)
{
    if (frame->commands != VK_NULL_HANDLE) {
        device->vkFreeCommandBuffers(device->device, device->command_pool, 1u,
                                     &frame->commands);
    }
    if (frame->pipeline != VK_NULL_HANDLE) {
        device->vkDestroyPipeline(device->device, frame->pipeline, NULL);
    }
    if (frame->layout != VK_NULL_HANDLE) {
        device->vkDestroyPipelineLayout(device->device, frame->layout, NULL);
    }
    if (frame->fragment_shader != VK_NULL_HANDLE) {
        device->vkDestroyShaderModule(device->device, frame->fragment_shader, NULL);
    }
    if (frame->vertex_shader != VK_NULL_HANDLE) {
        device->vkDestroyShaderModule(device->device, frame->vertex_shader, NULL);
    }
    if (frame->readback != VK_NULL_HANDLE) {
        device->vkDestroyBuffer(device->device, frame->readback, NULL);
    }
    if (frame->readback_memory != VK_NULL_HANDLE) {
        device->vkFreeMemory(device->device, frame->readback_memory, NULL);
    }
    if (frame->framebuffer != VK_NULL_HANDLE) {
        device->vkDestroyFramebuffer(device->device, frame->framebuffer, NULL);
    }
    if (frame->render_pass != VK_NULL_HANDLE) {
        device->vkDestroyRenderPass(device->device, frame->render_pass, NULL);
    }
    if (frame->view != VK_NULL_HANDLE) {
        device->vkDestroyImageView(device->device, frame->view, NULL);
    }
    if (frame->image != VK_NULL_HANDLE) {
        device->vkDestroyImage(device->device, frame->image, NULL);
    }
    if (frame->image_memory != VK_NULL_HANDLE) {
        device->vkFreeMemory(device->device, frame->image_memory, NULL);
    }
}

static gpu_result create_shader(gpu_device *device, const uint32_t *words,
                                size_t word_count, VkShaderModule *out_module)
{
    /* A truncated or byte-swapped regeneration of gpu_shaders.h would otherwise
     * surface as a driver-specific failure or, worse, a blank frame. Check the
     * magic here so the complaint names the actual problem. */
    if (word_count == 0u || words[0] != GPU_SPIRV_MAGIC) {
        return GPU_ERR_ARGUMENT;
    }
    const VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = word_count * sizeof *words,
        .pCode = words,
    };
    if (device->vkCreateShaderModule(device->device, &info, NULL, out_module) != VK_SUCCESS) {
        return GPU_ERR_VULKAN;
    }
    return GPU_OK;
}

static gpu_result create_pipeline(gpu_device *device, uint32_t width, uint32_t height,
                                  frame_resources *frame)
{
    gpu_result outcome = create_shader(device, gpu_shader_triangle_vert,
                                      sizeof gpu_shader_triangle_vert / sizeof(uint32_t),
                                      &frame->vertex_shader);
    if (outcome != GPU_OK) {
        return outcome;
    }
    outcome = create_shader(device, gpu_shader_triangle_frag,
                            sizeof gpu_shader_triangle_frag / sizeof(uint32_t),
                            &frame->fragment_shader);
    if (outcome != GPU_OK) {
        return outcome;
    }

    /* Empty layout: the shaders take no descriptors and no push constants, because
     * the vertex data is baked into the vertex shader. */
    const VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    };
    if (device->vkCreatePipelineLayout(device->device, &layout_info, NULL,
                                       &frame->layout) != VK_SUCCESS) {
        return GPU_ERR_VULKAN;
    }

    const VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = frame->vertex_shader,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = frame->fragment_shader,
            .pName = "main",
        },
    };

    /* No bindings and no attributes: gl_VertexIndex is the only input. */
    const VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    const VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };

    const VkViewport viewport = {
        .x = 0.0f,
        .y = 0.0f,
        .width = (float)width,
        .height = (float)height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    const VkRect2D scissor = {
        .offset = { 0, 0 },
        .extent = { width, height },
    };
    /* Baked into the pipeline rather than set dynamically. The pipeline is built
     * per frame anyway, so dynamic state would add a code path without removing
     * any work. */
    const VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1u,
        .pViewports = &viewport,
        .scissorCount = 1u,
        .pScissors = &scissor,
    };

    /* Culling off. The triangle's winding is a property of the hardcoded vertex
     * order, and a culled triangle looks exactly like a pipeline that never ran,
     * which would make this proof ambiguous for no benefit. */
    const VkPipelineRasterizationStateCreateInfo rasterisation = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .minSampleShading = 1.0f,
    };
    /* Blending off, so a fragment's colour lands in the target unmodified and an
     * expected pixel value is a statement about the shader rather than about the
     * blend equation. */
    const VkPipelineColorBlendAttachmentState blend_attachment = {
        .blendEnable = VK_FALSE,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1u,
        .pAttachments = &blend_attachment,
    };

    const VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2u,
        .pStages = stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterisation,
        .pMultisampleState = &multisample,
        .pColorBlendState = &blend,
        .layout = frame->layout,
        .renderPass = frame->render_pass,
        .subpass = 0u,
    };
    if (device->vkCreateGraphicsPipelines(device->device, VK_NULL_HANDLE, 1u,
                                          &pipeline_info, NULL,
                                          &frame->pipeline) != VK_SUCCESS) {
        return GPU_ERR_VULKAN;
    }
    return GPU_OK;
}

static gpu_result render(gpu_device *device, uint32_t width, uint32_t height,
                         const float clear_rgba[4], bool draw_triangle,
                         gpu_image *out_image)
{
    if (!device || !clear_rgba || !out_image) {
        return GPU_ERR_ARGUMENT;
    }
    if (width == 0u || height == 0u ||
        width > GPU_MAX_DIMENSION || height > GPU_MAX_DIMENSION) {
        return GPU_ERR_ARGUMENT;
    }

    out_image->pixels = NULL;
    out_image->width = 0u;
    out_image->height = 0u;
    out_image->stride_bytes = 0u;

    frame_resources frame = { 0 };
    gpu_result outcome = GPU_ERR_VULKAN;
    const VkDeviceSize readback_size =
        (VkDeviceSize)width * (VkDeviceSize)height * GPU_BYTES_PER_PIXEL;

    /* OPTIMAL tiling with a buffer copy for readback, rather than a linear-tiled
     * attachment mapped directly. Linear colour attachments are an optional
     * feature per format, so the direct route would work on lavapipe and fail on
     * the first real driver -- exactly the kind of thing that is cheap to get
     * right now and expensive to discover later. */
    const VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = GPU_TARGET_FORMAT,
        .extent = { width, height, 1u },
        .mipLevels = 1u,
        .arrayLayers = 1u,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (device->vkCreateImage(device->device, &image_info, NULL, &frame.image) != VK_SUCCESS) {
        goto cleanup;
    }

    VkMemoryRequirements image_requirements;
    device->vkGetImageMemoryRequirements(device->device, frame.image, &image_requirements);
    uint32_t image_memory_type = 0u;
    if (!find_memory_type(&device->memory_properties, image_requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &image_memory_type)) {
        outcome = GPU_ERR_NO_MEMORY_TYPE;
        goto cleanup;
    }
    const VkMemoryAllocateInfo image_allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = image_requirements.size,
        .memoryTypeIndex = image_memory_type,
    };
    if (device->vkAllocateMemory(device->device, &image_allocation, NULL,
                                 &frame.image_memory) != VK_SUCCESS) {
        goto cleanup;
    }
    if (device->vkBindImageMemory(device->device, frame.image, frame.image_memory, 0u)
        != VK_SUCCESS) {
        goto cleanup;
    }

    const VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = frame.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = GPU_TARGET_FORMAT,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1u,
            .layerCount = 1u,
        },
    };
    if (device->vkCreateImageView(device->device, &view_info, NULL, &frame.view)
        != VK_SUCCESS) {
        goto cleanup;
    }

    /* loadOp CLEAR is what performs the clear, for both the clear-only and the
     * triangle path. finalLayout TRANSFER_SRC_OPTIMAL lets the render pass do the
     * transition the readback copy needs, so no separate barrier is required. */
    const VkAttachmentDescription attachment = {
        .format = GPU_TARGET_FORMAT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    };
    const VkAttachmentReference colour_reference = {
        .attachment = 0u,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    const VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &colour_reference,
    };
    /* Makes the colour writes visible to the transfer that reads them. The
     * implicit external dependency does not cover this, and without it the
     * readback is a race that happens to pass on a software rasteriser. */
    const VkSubpassDependency dependency = {
        .srcSubpass = 0u,
        .dstSubpass = VK_SUBPASS_EXTERNAL,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
    };
    const VkRenderPassCreateInfo render_pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1u,
        .pAttachments = &attachment,
        .subpassCount = 1u,
        .pSubpasses = &subpass,
        .dependencyCount = 1u,
        .pDependencies = &dependency,
    };
    if (device->vkCreateRenderPass(device->device, &render_pass_info, NULL,
                                   &frame.render_pass) != VK_SUCCESS) {
        goto cleanup;
    }

    const VkFramebufferCreateInfo framebuffer_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = frame.render_pass,
        .attachmentCount = 1u,
        .pAttachments = &frame.view,
        .width = width,
        .height = height,
        .layers = 1u,
    };
    if (device->vkCreateFramebuffer(device->device, &framebuffer_info, NULL,
                                    &frame.framebuffer) != VK_SUCCESS) {
        goto cleanup;
    }

    if (draw_triangle) {
        outcome = create_pipeline(device, width, height, &frame);
        if (outcome != GPU_OK) {
            goto cleanup;
        }
        outcome = GPU_ERR_VULKAN;
    }

    const VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = readback_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (device->vkCreateBuffer(device->device, &buffer_info, NULL, &frame.readback)
        != VK_SUCCESS) {
        goto cleanup;
    }
    VkMemoryRequirements buffer_requirements;
    device->vkGetBufferMemoryRequirements(device->device, frame.readback,
                                          &buffer_requirements);
    uint32_t buffer_memory_type = 0u;
    /* HOST_COHERENT as well as HOST_VISIBLE, so no explicit cache invalidate is
     * needed before reading the mapping. */
    if (!find_memory_type(&device->memory_properties, buffer_requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &buffer_memory_type)) {
        outcome = GPU_ERR_NO_MEMORY_TYPE;
        goto cleanup;
    }
    const VkMemoryAllocateInfo buffer_allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = buffer_requirements.size,
        .memoryTypeIndex = buffer_memory_type,
    };
    if (device->vkAllocateMemory(device->device, &buffer_allocation, NULL,
                                 &frame.readback_memory) != VK_SUCCESS) {
        goto cleanup;
    }
    if (device->vkBindBufferMemory(device->device, frame.readback,
                                   frame.readback_memory, 0u) != VK_SUCCESS) {
        goto cleanup;
    }

    const VkCommandBufferAllocateInfo command_allocation = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = device->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1u,
    };
    if (device->vkAllocateCommandBuffers(device->device, &command_allocation,
                                         &frame.commands) != VK_SUCCESS) {
        goto cleanup;
    }

    const VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (device->vkBeginCommandBuffer(frame.commands, &begin_info) != VK_SUCCESS) {
        goto cleanup;
    }

    const VkClearValue clear_value = {
        .color = { .float32 = { clear_rgba[0], clear_rgba[1], clear_rgba[2], clear_rgba[3] } },
    };
    const VkRenderPassBeginInfo pass_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = frame.render_pass,
        .framebuffer = frame.framebuffer,
        .renderArea = { .offset = { 0, 0 }, .extent = { width, height } },
        .clearValueCount = 1u,
        .pClearValues = &clear_value,
    };
    device->vkCmdBeginRenderPass(frame.commands, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    if (draw_triangle) {
        device->vkCmdBindPipeline(frame.commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  frame.pipeline);
        device->vkCmdDraw(frame.commands, 3u, 1u, 0u, 0u);
    }
    device->vkCmdEndRenderPass(frame.commands);

    const VkBufferImageCopy copy = {
        .bufferOffset = 0u,
        /* Zero means "tightly packed to the image extent", which is what makes the
         * returned stride exactly width * 4. */
        .bufferRowLength = 0u,
        .bufferImageHeight = 0u,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0u,
            .baseArrayLayer = 0u,
            .layerCount = 1u,
        },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { width, height, 1u },
    };
    device->vkCmdCopyImageToBuffer(frame.commands, frame.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   frame.readback, 1u, &copy);

    /* Host visibility of the transfer writes. Coherent memory removes the need to
     * invalidate a cache, but it does not remove the need to order the write
     * against the host read. */
    const VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = frame.readback,
        .offset = 0u,
        .size = VK_WHOLE_SIZE,
    };
    device->vkCmdPipelineBarrier(frame.commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, 0u, 0u, NULL, 1u,
                                 &host_barrier, 0u, NULL);

    if (device->vkEndCommandBuffer(frame.commands) != VK_SUCCESS) {
        goto cleanup;
    }

    const VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1u,
        .pCommandBuffers = &frame.commands,
    };
    if (device->vkQueueSubmit(device->queue, 1u, &submit, VK_NULL_HANDLE) != VK_SUCCESS) {
        goto cleanup;
    }
    /* vkQueueWaitIdle rather than a fence. There is exactly one submission in
     * flight and nothing to overlap it with, so a fence would be ceremony. */
    if (device->vkQueueWaitIdle(device->queue) != VK_SUCCESS) {
        goto cleanup;
    }

    void *mapping = NULL;
    if (device->vkMapMemory(device->device, frame.readback_memory, 0u, readback_size,
                            0u, &mapping) != VK_SUCCESS) {
        goto cleanup;
    }
    uint8_t *pixels = malloc((size_t)readback_size);
    if (!pixels) {
        device->vkUnmapMemory(device->device, frame.readback_memory);
        outcome = GPU_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    memcpy(pixels, mapping, (size_t)readback_size);
    device->vkUnmapMemory(device->device, frame.readback_memory);

    out_image->pixels = pixels;
    out_image->width = width;
    out_image->height = height;
    out_image->stride_bytes = width * GPU_BYTES_PER_PIXEL;
    outcome = GPU_OK;

cleanup:
    frame_release(device, &frame);
    return outcome;
}

gpu_result gpu_render_clear(gpu_device *device, uint32_t width, uint32_t height,
                            const float clear_rgba[4], gpu_image *out_image)
{
    return render(device, width, height, clear_rgba, false, out_image);
}

gpu_result gpu_render_triangle(gpu_device *device, uint32_t width, uint32_t height,
                               const float clear_rgba[4], gpu_image *out_image)
{
    return render(device, width, height, clear_rgba, true, out_image);
}

bool gpu_device_has_transform_feedback(const gpu_device *device)
{
    return device != NULL && device->transform_feedback;
}

void gpu_device_get_native(const gpu_device *device, gpu_device_native *out_native)
{
    out_native->get_instance_proc_addr = device->vkGetInstanceProcAddr;
    out_native->get_device_proc_addr = device->vkGetDeviceProcAddr;
    out_native->instance = device->instance;
    out_native->physical_device = device->physical_device;
    out_native->device = device->device;
    out_native->queue = device->queue;
    out_native->queue_family = device->queue_family;
    out_native->command_pool = device->command_pool;
    out_native->memory_properties = device->memory_properties;
    out_native->transform_feedback = device->transform_feedback;
    out_native->negative_viewport = device->negative_viewport;
    out_native->sampler_anisotropy = device->sampler_anisotropy;
    out_native->large_points = device->large_points;
    out_native->fill_mode_non_solid = device->fill_mode_non_solid;
    out_native->occlusion_query_precise = device->occlusion_query_precise;
}
