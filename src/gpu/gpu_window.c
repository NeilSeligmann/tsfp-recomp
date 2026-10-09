/* SPDX-License-Identifier: GPL-3.0-or-later */
#define VK_NO_PROTOTYPES
#include "gpu_phase_timing.h"
#include <vulkan/vulkan_core.h>

#include "gpu_window.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W_GLOBAL(X) X(vkCreateInstance)
#define W_INSTANCE(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceFeatures) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkCreateDevice) X(vkDestroySurfaceKHR) X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties)
#define W_DEVICE(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkQueuePresentKHR) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateRenderPass) X(vkDestroyRenderPass) \
    X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateCommandPool) X(vkDestroyCommandPool) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) \
    X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyImageToBuffer) X(vkCmdClearColorImage) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkQueueWaitIdle) \
    X(vkAcquireNextImageKHR)

struct gpu_window {
    SDL_Window *window;
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    VkPhysicalDeviceMemoryProperties memory_properties;
    bool can_upload;
    bool blit_exact;              /* T1267: UNORM swapchain format and opaque composite alpha, the two conditions under which a vkCmdBlitImage shows the readback route's pixels */
    bool can_capture; /* T849: the swapchain images also have TRANSFER_SRC */
    bool capturing;
    VkBuffer capture_buffer;
    VkDeviceMemory capture_memory;
    bool negative_viewport; /* T828: VK_KHR_maintenance1 enabled */
    bool occlusion_query_precise;
    bool sampler_anisotropy; /* T1051: actually enabled samplerAnisotropy. */
    bool large_points;
    bool fill_mode_non_solid; /* T860 */
    VkRenderPass render_pass;
    VkCommandPool command_pool;
    uint32_t image_count;
    VkImage *images;
    VkImageView *views;
    VkFramebuffer *framebuffers;
    VkCommandBuffer *commands;
    VkSemaphore acquired;
    VkSemaphore finished;
    uint32_t finished_count;      /* T1339: semaphores in finished_images (the image count of the swapchain they were made for) */
    VkFormat pass_format;         /* T1339: the format render_pass was made for, kept across a swapchain recreation */
    VkSemaphore *finished_images; /* T1262: one render finished semaphore per swapchain image, so a present never waits for the queue to go idle */
    VkBuffer staging;             /* T1262: the readback route's host staging buffer, kept between presents */
    VkDeviceMemory staging_memory;
    void *staging_mapped;
    size_t staging_bytes;
    uint32_t *scale_x; /* T1262: source column of every window column, valid for scale_x_width -> scale_x_extent */
    uint32_t scale_x_width, scale_x_extent;
    VkFence fence;
    VkCommandBuffer current_command_buffer;
    bool recording_frame;
    uint64_t pending_packets;
    uint64_t pending_methods;
    gpu_window_stats stats;
    gpu_window_frame_hook frame_hook;
    void *frame_hook_context;
    gpu_window_complete_hook complete_hook;
    void *complete_hook_context;
    char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
#define W_FIELD(name) PFN_##name name;
    W_GLOBAL(W_FIELD)
    W_INSTANCE(W_FIELD)
    W_DEVICE(W_FIELD)
#undef W_FIELD
};

static bool surface_format(gpu_window *r, VkSurfaceFormatKHR *out)
{
    uint32_t count = 0;
    if (r->vkGetPhysicalDeviceSurfaceFormatsKHR(r->physical, r->surface, &count, NULL) != VK_SUCCESS || count == 0u)
        return false;
    VkSurfaceFormatKHR *formats = calloc(count, sizeof *formats);
    if (formats == NULL) return false;
    const VkResult result = r->vkGetPhysicalDeviceSurfaceFormatsKHR(r->physical, r->surface, &count, formats);
    if (result == VK_SUCCESS) {
        *out = formats[0];
        for (uint32_t i = 0; i < count; i++) {
            if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
                *out = formats[i];
                break;
            }
        }
    }
    free(formats);
    return result == VK_SUCCESS;
}

static void release_swapchain(gpu_window *r, bool keep_render_pass)
{
    if (r->device != VK_NULL_HANDLE && r->vkDeviceWaitIdle != NULL) (void)r->vkDeviceWaitIdle(r->device);
    for (uint32_t i = 0; i < r->image_count; i++) {
        if (r->framebuffers != NULL && r->framebuffers[i] != VK_NULL_HANDLE)
            if (r->vkDestroyFramebuffer != NULL) r->vkDestroyFramebuffer(r->device, r->framebuffers[i], NULL);
        if (r->views != NULL && r->views[i] != VK_NULL_HANDLE)
            if (r->vkDestroyImageView != NULL) r->vkDestroyImageView(r->device, r->views[i], NULL);
    }
    if (r->commands != NULL && r->command_pool != VK_NULL_HANDLE && r->vkFreeCommandBuffers != NULL)
        r->vkFreeCommandBuffers(r->device, r->command_pool, r->image_count, r->commands);
    free(r->images); free(r->views); free(r->framebuffers); free(r->commands);
    r->images = NULL; r->views = NULL; r->framebuffers = NULL; r->commands = NULL; r->image_count = 0u;
    if (!keep_render_pass && r->render_pass != VK_NULL_HANDLE && r->vkDestroyRenderPass != NULL) {
        r->vkDestroyRenderPass(r->device, r->render_pass, NULL);
        r->render_pass = VK_NULL_HANDLE;
    }
    if (r->swapchain != VK_NULL_HANDLE && r->vkDestroySwapchainKHR != NULL)
        r->vkDestroySwapchainKHR(r->device, r->swapchain, NULL);
    r->swapchain = VK_NULL_HANDLE;
}

static bool create_swapchain(gpu_window *r)
{
    VkSurfaceCapabilitiesKHR caps;
    if (r->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->physical, r->surface, &caps) != VK_SUCCESS) return false;
    VkSurfaceFormatKHR chosen;
    if (!surface_format(r, &chosen)) return false;
    if (chosen.format == VK_FORMAT_UNDEFINED) chosen.format = VK_FORMAT_B8G8R8A8_UNORM;
    r->format = chosen.format;
    if (caps.currentExtent.width != UINT32_MAX) r->extent = caps.currentExtent;
    else {
        int width = 0, height = 0;
        if (!SDL_GetWindowSizeInPixels(r->window, &width, &height) || width <= 0 || height <= 0) return false;
        r->extent.width = (uint32_t)width; r->extent.height = (uint32_t)height;
        if (r->extent.width < caps.minImageExtent.width) r->extent.width = caps.minImageExtent.width;
        if (r->extent.height < caps.minImageExtent.height) r->extent.height = caps.minImageExtent.height;
        if (r->extent.width > caps.maxImageExtent.width) r->extent.width = caps.maxImageExtent.width;
        if (r->extent.height > caps.maxImageExtent.height) r->extent.height = caps.maxImageExtent.height;
    }
    uint32_t count = caps.minImageCount + 1u;
    if (caps.maxImageCount != 0u && count > caps.maxImageCount) count = caps.maxImageCount;
    VkCompositeAlphaFlagBitsKHR composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    const VkCompositeAlphaFlagBitsKHR alpha_modes[] = {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
    bool alpha_found = false;
    for (size_t i = 0; i < sizeof alpha_modes / sizeof alpha_modes[0]; i++) {
        if ((caps.supportedCompositeAlpha & alpha_modes[i]) != 0u) { composite = alpha_modes[i]; alpha_found = true; break; }
    }
    if (!alpha_found) return false;
    VkSwapchainCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = r->surface, .minImageCount = count, .imageFormat = chosen.format,
        .imageColorSpace = chosen.colorSpace, .imageExtent = r->extent, .imageArrayLayers = 1u,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0u ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0u) |
            ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0u ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0u),
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform, .compositeAlpha = composite,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE,
    };
    if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0u ||
        r->vkCreateSwapchainKHR(r->device, &info, NULL, &r->swapchain) != VK_SUCCESS) return false;
    r->blit_exact = (chosen.format == VK_FORMAT_B8G8R8A8_UNORM || chosen.format == VK_FORMAT_R8G8B8A8_UNORM) &&
                    composite == VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    r->can_upload = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0u;
    r->can_capture = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0u;
    if (r->vkGetSwapchainImagesKHR(r->device, r->swapchain, &r->image_count, NULL) != VK_SUCCESS || r->image_count == 0u)
        return false;
    r->images = calloc(r->image_count, sizeof *r->images);
    r->views = calloc(r->image_count, sizeof *r->views);
    r->framebuffers = calloc(r->image_count, sizeof *r->framebuffers);
    r->commands = calloc(r->image_count, sizeof *r->commands);
    if (r->images == NULL || r->views == NULL || r->framebuffers == NULL || r->commands == NULL) return false;
    if (r->vkGetSwapchainImagesKHR(r->device, r->swapchain, &r->image_count, r->images) != VK_SUCCESS) return false;
    VkAttachmentDescription attachment = {
        .format = r->format, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentReference color = {.attachment = 0u, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    .colorAttachmentCount = 1u, .pColorAttachments = &color};
    VkRenderPassCreateInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1u, .pAttachments = &attachment, .subpassCount = 1u, .pSubpasses = &subpass};
    if (r->render_pass != VK_NULL_HANDLE && r->pass_format != r->format) {
        /* T1339: a recreated swapchain with another format cannot reuse the pass (the live renderer's window pipelines were made for the old one) */
        r->vkDestroyRenderPass(r->device, r->render_pass, NULL);
        r->render_pass = VK_NULL_HANDLE;
        fprintf(stderr, "gpu window: swapchain format changed on recreation, the window render pass was remade\n");
    }
    if (r->render_pass == VK_NULL_HANDLE) {
        if (r->vkCreateRenderPass(r->device, &pass, NULL, &r->render_pass) != VK_SUCCESS) return false;
        r->pass_format = r->format;
    }
    for (uint32_t i = 0; i < r->image_count; i++) {
        VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = r->images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = r->format,
            .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u}};
        if (r->vkCreateImageView(r->device, &view, NULL, &r->views[i]) != VK_SUCCESS) return false;
        VkFramebufferCreateInfo fb = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = r->render_pass, .attachmentCount = 1u, .pAttachments = &r->views[i],
            .width = r->extent.width, .height = r->extent.height, .layers = 1u};
        if (r->vkCreateFramebuffer(r->device, &fb, NULL, &r->framebuffers[i]) != VK_SUCCESS) return false;
    }
    VkCommandBufferAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = r->command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = r->image_count};
    if (r->vkAllocateCommandBuffers(r->device, &alloc, r->commands) != VK_SUCCESS) return false;
    return true;
}

gpu_window *gpu_window_create(SDL_Window *window, const char **error)
{
    if (error != NULL) *error = NULL;
    if (window == NULL) { if (error) *error = "Vulkan window is null"; return NULL; }
    gpu_window *r = calloc(1, sizeof *r);
    if (r == NULL) { if (error) *error = "Vulkan window allocation failed"; return NULL; }
    r->window = window;
    Uint32 ext_count = 0u;
    const char *const *extensions = SDL_Vulkan_GetInstanceExtensions(&ext_count);
    if (extensions == NULL) { if (error) *error = SDL_GetError(); free(r); return NULL; }
    PFN_vkGetInstanceProcAddr gipa = NULL;
    SDL_FunctionPointer sdl_gipa = SDL_Vulkan_GetVkGetInstanceProcAddr();
    memcpy(&gipa, &sdl_gipa, sizeof gipa);
    PFN_vkCreateInstance create_instance = NULL;
    if (gipa != NULL) {
        PFN_vkVoidFunction p = gipa(VK_NULL_HANDLE, "vkCreateInstance");
        memcpy(&create_instance, &p, sizeof create_instance);
    }
    if (create_instance == NULL) { if (error) *error = "SDL3 could not load Vulkan"; free(r); return NULL; }
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "tsfp-recomp", .applicationVersion = 1u, .pEngineName = "tsfp", .engineVersion = 1u,
        .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
        .enabledExtensionCount = ext_count, .ppEnabledExtensionNames = extensions};
    if (create_instance(&ci, NULL, &r->instance) != VK_SUCCESS) {
        if (error) *error = "vkCreateInstance for SDL surface failed"; free(r); return NULL;
    }
    /* Load instance functions using a temporary minimal field before the complete table. */
    PFN_vkVoidFunction p_destroy = gipa(r->instance, "vkDestroyInstance");
    memcpy(&r->vkDestroyInstance, &p_destroy, sizeof r->vkDestroyInstance);
#define W_LOAD_INSTANCE(name) do { PFN_vkVoidFunction p = gipa(r->instance, #name); memcpy(&r->name, &p, sizeof r->name); if (!r->name) goto fail; } while (0);
    W_INSTANCE(W_LOAD_INSTANCE);
#undef W_LOAD_INSTANCE
    if (!SDL_Vulkan_CreateSurface(window, r->instance, NULL, &r->surface)) { if (error) *error = SDL_GetError(); goto fail; }
    uint32_t count = 0u;
    if (r->vkEnumeratePhysicalDevices(r->instance, &count, NULL) != VK_SUCCESS || count == 0u) {
        if (error) *error = "no Vulkan physical devices"; goto fail;
    }
    VkPhysicalDevice *devices = calloc(count, sizeof *devices);
    if (devices == NULL) { if (error) *error = "Vulkan device list allocation failed"; goto fail; }
    if (r->vkEnumeratePhysicalDevices(r->instance, &count, devices) != VK_SUCCESS) { free(devices); if (error) *error = "vkEnumeratePhysicalDevices failed"; goto fail; }
    bool found = false;
    for (uint32_t d = 0; d < count && !found; d++) {
        VkPhysicalDeviceProperties candidate;
        r->vkGetPhysicalDeviceProperties(devices[d],&candidate);
        if (candidate.apiVersion < VK_API_VERSION_1_1) continue;
        uint32_t queues = 0u;
        r->vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &queues, NULL);
        VkQueueFamilyProperties *props = calloc(queues, sizeof *props);
        if (props == NULL) continue;
        r->vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &queues, props);
        for (uint32_t q = 0; q < queues; q++) {
            VkBool32 supported = VK_FALSE;
            if ((props[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u &&
                r->vkGetPhysicalDeviceSurfaceSupportKHR(devices[d], q, r->surface, &supported) == VK_SUCCESS && supported) {
                r->physical = devices[d]; r->family = q; found = true; break;
            }
        }
        free(props);
    }
    free(devices);
    if (!found) { if (error) *error = "no Vulkan 1.1 graphics queue can present to the SDL surface"; goto fail; }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = r->family, .queueCount = 1u, .pQueuePriorities = &priority};
    /* VK_KHR_maintenance1 (T828) makes a negative viewport height legal: asked for when the device lists it. */
    const char *device_extensions[2] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, "VK_KHR_maintenance1"};
    uint32_t extension_total = 0u;
    bool has_maintenance = false;
    if (r->vkEnumerateDeviceExtensionProperties(r->physical, NULL, &extension_total, NULL) == VK_SUCCESS && extension_total != 0u) {
        VkExtensionProperties *listed = calloc(extension_total, sizeof *listed);
        if (listed != NULL) {
            if (r->vkEnumerateDeviceExtensionProperties(r->physical, NULL, &extension_total, listed) == VK_SUCCESS) {
                for (uint32_t e = 0; e < extension_total; e++) {
                    has_maintenance = has_maintenance || strcmp(listed[e].extensionName, device_extensions[1]) == 0;
                }
            }
            free(listed);
        }
    }
    /* T860: fillModeNonSolid (LINE and POINT polygon modes) when the device has it. */
    VkPhysicalDeviceFeatures supported_features;
    memset(&supported_features, 0, sizeof supported_features);
    r->vkGetPhysicalDeviceFeatures(r->physical, &supported_features);
    VkPhysicalDeviceFeatures enabled_features;
    memset(&enabled_features, 0, sizeof enabled_features);
    enabled_features.samplerAnisotropy = supported_features.samplerAnisotropy;
    enabled_features.largePoints = supported_features.largePoints;
    enabled_features.fillModeNonSolid = supported_features.fillModeNonSolid;
    enabled_features.occlusionQueryPrecise = supported_features.occlusionQueryPrecise;
    r->sampler_anisotropy = enabled_features.samplerAnisotropy == VK_TRUE;
    r->fill_mode_non_solid = supported_features.fillModeNonSolid == VK_TRUE;
    r->occlusion_query_precise = supported_features.occlusionQueryPrecise == VK_TRUE;
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1u, .pQueueCreateInfos = &qi, .enabledExtensionCount = has_maintenance ? 2u : 1u,
        .ppEnabledExtensionNames = device_extensions, .pEnabledFeatures = &enabled_features};
    PFN_vkCreateDevice create_device = NULL;
    PFN_vkVoidFunction p_create_device = gipa(r->instance, "vkCreateDevice");
    memcpy(&create_device, &p_create_device, sizeof create_device);
    r->negative_viewport = has_maintenance;
    if (create_device != NULL && has_maintenance && create_device(r->physical, &di, NULL, &r->device) != VK_SUCCESS) {
        di.enabledExtensionCount = 1u;
        r->negative_viewport = false;
        r->device = VK_NULL_HANDLE;
    }
    if (create_device == NULL || (r->device == VK_NULL_HANDLE && create_device(r->physical, &di, NULL, &r->device) != VK_SUCCESS)) {
        if (error) *error = "vkCreateDevice for SDL surface failed"; goto fail;
    }
    PFN_vkVoidFunction p_gdq = gipa(r->instance, "vkGetDeviceProcAddr");
    PFN_vkGetDeviceProcAddr gdpa = NULL; memcpy(&gdpa, &p_gdq, sizeof gdpa);
    r->vkGetDeviceProcAddr = gdpa;
#define W_LOAD_DEVICE(name) do { PFN_vkVoidFunction p = gdpa(r->device, #name); memcpy(&r->name, &p, sizeof r->name); if (!r->name) goto fail; } while (0);
    W_DEVICE(W_LOAD_DEVICE);
#undef W_LOAD_DEVICE
    r->vkGetDeviceQueue(r->device, r->family, 0u, &r->queue);
    VkPhysicalDeviceProperties device_properties;
    r->vkGetPhysicalDeviceProperties(r->physical, &device_properties);
    (void)snprintf(r->device_name, sizeof r->device_name, "%s", device_properties.deviceName);
    r->vkGetPhysicalDeviceMemoryProperties(r->physical, &r->memory_properties);
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = r->family};
    if (r->vkCreateCommandPool(r->device, &pool, NULL, &r->command_pool) != VK_SUCCESS || !create_swapchain(r)) {
        if (error) *error = "could not create the Vulkan swapchain"; goto fail;
    }
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    if (r->vkCreateSemaphore(r->device, &si, NULL, &r->acquired) != VK_SUCCESS ||
        r->vkCreateSemaphore(r->device, &si, NULL, &r->finished) != VK_SUCCESS ||
        r->vkCreateFence(r->device, &fi, NULL, &r->fence) != VK_SUCCESS) {
        if (error) *error = "could not create Vulkan frame synchronization"; goto fail;
    }
    r->finished_images = calloc(r->image_count != 0u ? r->image_count : 1u, sizeof *r->finished_images);
    if (r->finished_images == NULL) {
        if (error) *error = "could not allocate the per image semaphores"; goto fail;
    }
    for (uint32_t i = 0u; i < r->image_count; i++) {
        if (r->vkCreateSemaphore(r->device, &si, NULL, &r->finished_images[i]) != VK_SUCCESS) {
            if (error) *error = "could not create Vulkan frame synchronization"; goto fail;
        }
        r->finished_count = i + 1u;
    }
    /* Keep the SDL-provided loader alive for the surface lifetime. */
    if (error) *error = NULL;
    return r;
fail:
    gpu_window_destroy(r);
    return NULL;
}

static uint32_t host_memory_type(const VkPhysicalDeviceMemoryProperties *properties, uint32_t bits)
{
    for (uint32_t i = 0; i < properties->memoryTypeCount; i++) {
        const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((bits & (1u << i)) != 0u && (properties->memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    }
    return UINT32_MAX;
}

static void image_barrier(gpu_window *r, VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                          VkAccessFlags from_access, VkAccessFlags to_access, VkPipelineStageFlags from_stage,
                          VkPipelineStageFlags to_stage)
{
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = from_access, .dstAccessMask = to_access, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u}};
    r->vkCmdPipelineBarrier(cmd, from_stage, to_stage, 0u, 0u, NULL, 0u, NULL, 1u, &barrier);
}

/* Last step of every frame: the image goes to PRESENT_SRC. With capture on (T849, tests and the evidence path) the final image
 * is first copied into the capture buffer in the same command buffer, so both present routes can be compared byte for byte. */
static void finish_to_present(gpu_window *r, VkCommandBuffer cmd, VkImage image, VkImageLayout layout,
                              VkAccessFlags access, VkPipelineStageFlags stage)
{
    if (r->capturing && r->capture_buffer != VK_NULL_HANDLE) {
        image_barrier(r, cmd, image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, access, VK_ACCESS_TRANSFER_READ_BIT,
                      stage, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy = {.bufferRowLength = r->extent.width, .bufferImageHeight = r->extent.height,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
            .imageExtent = {r->extent.width, r->extent.height, 1u}};
        r->vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r->capture_buffer, 1u, &copy);
        image_barrier(r, cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_ACCESS_TRANSFER_READ_BIT, 0u, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        return;
    }
    image_barrier(r, cmd, image, layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, access, 0u, stage,
                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

/* T1262: the present no longer ends in vkQueueWaitIdle. The submit's fence (waited at the start of the next present) retires the command
 * buffer, the acquire semaphore (waited by that submit) and the staging buffer, and every swapchain image has its own render finished
 * semaphore, which the image's next acquire proves consumed. --live-present-sync (gpu_window_set_present_sync) restores the wait for
 * bisecting: the old behaviour made the presenter thread, and through it the guest thread, wait for the frame to leave the queue. */
static bool s_present_sync;

void gpu_window_set_present_sync(bool enable)
{
    s_present_sync = enable;
}

static bool wait_previous_present(gpu_window *r)
{
    const uint64_t start = gpu_phase_now();
    const bool good = r->vkWaitForFences(r->device, 1u, &r->fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
    gpu_phase_add(GPU_PHASE_PRESENT_FENCE_WAIT, start);
    return good;
}

/* T1339: the swapchain went out of date (a resize, a mode change, a compositor event) or the surface changed. Without this the first
 * VK_ERROR_OUT_OF_DATE_KHR failed every later acquire for ever, the guest ran on and the window never drew again. The old swapchain is
 * retired, a new one is made for the surface's current extent, the render pass is kept (the live renderer's window pipelines use it),
 * the per image semaphores and the acquire semaphore are remade (a failed acquire or present leaves them in an undefined state). */
static bool recreate_swapchain(gpu_window *r)
{
    (void)r->vkDeviceWaitIdle(r->device);
    release_swapchain(r, true);
    if (!create_swapchain(r)) return false;
    for (uint32_t i = 0u; i < r->finished_count; i++) {
        if (r->finished_images[i] != VK_NULL_HANDLE) r->vkDestroySemaphore(r->device, r->finished_images[i], NULL);
    }
    free(r->finished_images);
    r->finished_images = calloc(r->image_count, sizeof *r->finished_images);
    r->finished_count = 0u;
    if (r->finished_images == NULL) return false;
    const VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0u; i < r->image_count; i++) {
        if (r->vkCreateSemaphore(r->device, &si, NULL, &r->finished_images[i]) != VK_SUCCESS) return false;
        r->finished_count = i + 1u;
    }
    r->vkDestroySemaphore(r->device, r->acquired, NULL);
    r->acquired = VK_NULL_HANDLE;
    if (r->vkCreateSemaphore(r->device, &si, NULL, &r->acquired) != VK_SUCCESS) return false;
    r->stats.swapchain_recreations++;
    fprintf(stderr, "gpu window: swapchain recreated (%u), %ux%u, %u images\n", (unsigned)r->stats.swapchain_recreations,
            (unsigned)r->extent.width, (unsigned)r->extent.height, (unsigned)r->image_count);
    return true;
}

static bool acquire_image(gpu_window *r, uint32_t *image)
{
    const uint64_t start = gpu_phase_now();
    VkResult acquired = r->vkAcquireNextImageKHR(r->device, r->swapchain, UINT64_MAX, r->acquired, VK_NULL_HANDLE, image);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR && recreate_swapchain(r)) {
        acquired = r->vkAcquireNextImageKHR(r->device, r->swapchain, UINT64_MAX, r->acquired, VK_NULL_HANDLE, image);
    }
    gpu_phase_add(GPU_PHASE_PRESENT_ACQUIRE, start);
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        if (r->stats.acquire_failures++ == 0u) {
            fprintf(stderr, "gpu window: vkAcquireNextImageKHR failed (VkResult %d), the window cannot draw until it succeeds\n", (int)acquired);
        }
        return false;
    }
    return true;
}

/* Submit `cmd` and present `image`. The caller has waited for the previous submit's fence. */
static bool submit_and_present(gpu_window *r, VkCommandBuffer cmd, uint32_t image, VkPipelineStageFlags wait_stage, bool blit)
{
    VkSemaphore finished = r->finished_images != NULL && image < r->image_count ? r->finished_images[image] : r->finished;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1u,
        .pWaitSemaphores = &r->acquired, .pWaitDstStageMask = &wait_stage, .commandBufferCount = 1u,
        .pCommandBuffers = &cmd, .signalSemaphoreCount = 1u, .pSignalSemaphores = &finished};
    uint64_t start = gpu_phase_now();
    const bool submitted = r->vkQueueSubmit(r->queue, 1u, &submit, r->fence) == VK_SUCCESS;
    gpu_phase_add(GPU_PHASE_PRESENT_SUBMIT, start);
    if (!submitted) return false;
    VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1u,
        .pWaitSemaphores = &finished, .swapchainCount = 1u, .pSwapchains = &r->swapchain, .pImageIndices = &image};
    start = gpu_phase_now();
    const VkResult result = r->vkQueuePresentKHR(r->queue, &present);
    gpu_phase_add(GPU_PHASE_PRESENT_QUEUE, start);
    r->stats.frames++;
    r->stats.presented++;
    if (blit) r->stats.blit_presented++;
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        /* T1339: this frame may be dropped, the next one draws into a swapchain of the surface's current size */
        if (!recreate_swapchain(r)) { r->stats.present_failures++; return false; }
        return true;
    }
    if (result != VK_SUCCESS) {
        if (r->stats.present_failures++ == 0u) {
            fprintf(stderr, "gpu window: vkQueuePresentKHR failed (VkResult %d), the window is not showing frames\n", (int)result);
        }
        return false;
    }
    if (s_present_sync) {
        start = gpu_phase_now();
        const bool idle = r->vkQueueWaitIdle(r->queue) == VK_SUCCESS;
        gpu_phase_add(GPU_PHASE_PRESENT_IDLE, start);
        if (!idle) { r->stats.present_failures++; return false; }
    }
    return true;
}

/* The completion hook retires resources of commands recorded into the window pass (the frame_hook route), so it runs after the fence
 * says they finished. The pixels and blit routes record nothing the hook retires and call it at once. */
static bool run_complete_hook(gpu_window *r, bool window_commands)
{
    if (r->complete_hook == NULL) return true;
    if (window_commands && !s_present_sync && !wait_previous_present(r)) return false;
    return r->complete_hook(r->complete_hook_context);
}

/* Host staging buffer for the readback route, kept between presents. Only called after the previous submit's fence was waited. */
static bool staging_ensure(gpu_window *r, size_t bytes)
{
    if (r->staging != VK_NULL_HANDLE && r->staging_bytes >= bytes) return true;
    if (r->staging_mapped != NULL) { r->vkUnmapMemory(r->device, r->staging_memory); r->staging_mapped = NULL; }
    if (r->staging != VK_NULL_HANDLE) { r->vkDestroyBuffer(r->device, r->staging, NULL); r->staging = VK_NULL_HANDLE; }
    if (r->staging_memory != VK_NULL_HANDLE) { r->vkFreeMemory(r->device, r->staging_memory, NULL); r->staging_memory = VK_NULL_HANDLE; }
    r->staging_bytes = 0u;
    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (r->vkCreateBuffer(r->device, &buffer_info, NULL, &r->staging) != VK_SUCCESS) { r->staging = VK_NULL_HANDLE; return false; }
    VkMemoryRequirements requirements;
    r->vkGetBufferMemoryRequirements(r->device, r->staging, &requirements);
    const uint32_t memory_type = host_memory_type(&r->memory_properties, requirements.memoryTypeBits);
    VkMemoryAllocateInfo memory_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = memory_type};
    if (memory_type == UINT32_MAX || r->vkAllocateMemory(r->device, &memory_info, NULL, &r->staging_memory) != VK_SUCCESS ||
        r->vkBindBufferMemory(r->device, r->staging, r->staging_memory, 0u) != VK_SUCCESS ||
        r->vkMapMemory(r->device, r->staging_memory, 0u, bytes, 0u, &r->staging_mapped) != VK_SUCCESS || r->staging_mapped == NULL) {
        if (r->staging_memory != VK_NULL_HANDLE) { r->vkFreeMemory(r->device, r->staging_memory, NULL); r->staging_memory = VK_NULL_HANDLE; }
        r->vkDestroyBuffer(r->device, r->staging, NULL);
        r->staging = VK_NULL_HANDLE;
        r->staging_mapped = NULL;
        return false;
    }
    r->staging_bytes = bytes;
    return true;
}

static bool present_frame(gpu_window *r, const uint8_t *rgba, uint32_t width, uint32_t height,
                          uint32_t stride_bytes, const float clear_rgba[4])
{
    if (r == NULL || clear_rgba == NULL || r->swapchain == VK_NULL_HANDLE) return false;
    if (!wait_previous_present(r)) return false;
    uint32_t image = 0u;
    if (!acquire_image(r, &image)) return false;
    (void)r->vkResetFences(r->device, 1u, &r->fence);
    uint64_t record_start = gpu_phase_now();
    VkCommandBuffer cmd = r->commands[image];
    r->current_command_buffer = cmd;
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (r->vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) return false;
    VkClearValue clear = {.color = {.float32 = {clear_rgba[0], clear_rgba[1], clear_rgba[2], clear_rgba[3]}}};
    VkRenderPassBeginInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = r->render_pass,
        .framebuffer = r->framebuffers[image], .renderArea = {{0, 0}, r->extent}, .clearValueCount = 1u,
        .pClearValues = &clear};
    r->vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
    r->recording_frame = true;
    if (rgba == NULL && r->frame_hook != NULL) {
        gpu_window_native native;
        if (!gpu_window_get_native(r, &native) || !r->frame_hook(&native, r->frame_hook_context))
            r->stats.draw_refusals++;
    }
    /* The per-frame draw feed is intentionally at this point in the command buffer. T791
     * attaches the live NV2A pipeline; T790 consumes and accounts the title methods here. */
    r->stats.draw_packets += r->pending_packets;
    r->stats.draw_methods += r->pending_methods;
    r->pending_packets = 0u; r->pending_methods = 0u;
    r->vkCmdEndRenderPass(cmd);
    r->recording_frame = false;
    r->current_command_buffer = VK_NULL_HANDLE;
    if (rgba != NULL) {
        if (!r->can_upload || width == 0u || height == 0u || stride_bytes < width * 4u ||
            (size_t)r->extent.width > SIZE_MAX / 4u / r->extent.height) return false;
        const size_t bytes = (size_t)r->extent.width * r->extent.height * 4u;
        const uint64_t upload_start = gpu_phase_now();
        if (!staging_ensure(r, bytes)) return false;
        uint8_t *destination = r->staging_mapped;
        const bool swap_rb = r->format == VK_FORMAT_B8G8R8A8_UNORM || r->format == VK_FORMAT_B8G8R8A8_SRGB;
        /* T1262: the same nearest neighbour mapping as before, with the per column source index computed once per size (it was a 64 bit
         * division per output pixel, about 2 million of them at 1080p, on the thread the guest waits for), a repeated source row copied
         * instead of converted again, and 32 bit pixel moves. */
        if (r->scale_x_width != width || r->scale_x_extent != r->extent.width) {
            uint32_t *grown = realloc(r->scale_x, (size_t)r->extent.width * sizeof *grown);
            if (grown == NULL) return false;
            r->scale_x = grown;
            for (uint32_t x = 0u; x < r->extent.width; x++) r->scale_x[x] = (uint32_t)(((uint64_t)x * width) / r->extent.width);
            r->scale_x_width = width;
            r->scale_x_extent = r->extent.width;
        }
        uint32_t previous_source_y = UINT32_MAX;
        const uint8_t *previous_row = NULL;
        for (uint32_t y = 0u; y < r->extent.height; y++) {
            const uint32_t source_y = (uint32_t)(((uint64_t)y * height) / r->extent.height);
            uint8_t *destination_row = destination + (size_t)y * r->extent.width * 4u;
            if (source_y == previous_source_y && previous_row != NULL) {
                memcpy(destination_row, previous_row, (size_t)r->extent.width * 4u);
                continue;
            }
            const uint8_t *source_row = rgba + (size_t)source_y * stride_bytes;
            for (uint32_t x = 0u; x < r->extent.width; x++) {
                uint32_t texel;
                memcpy(&texel, source_row + (size_t)r->scale_x[x] * 4u, sizeof texel);
                if (swap_rb) texel = (texel & 0xFF00FF00u) | ((texel & 0xFFu) << 16) | ((texel >> 16) & 0xFFu); /* little endian bytes 0 and 2 swap */
                memcpy(destination_row + (size_t)x * 4u, &texel, sizeof texel);
            }
            previous_source_y = source_y;
            previous_row = destination_row;
        }
        gpu_phase_add(GPU_PHASE_PRESENT_UPLOAD, upload_start);
        VkImageMemoryBarrier to_transfer = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = r->images[image], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u}};
        r->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                0u, 0u, NULL, 0u, NULL, 1u, &to_transfer);
        VkBufferImageCopy copy = {.bufferOffset = 0u, .bufferRowLength = r->extent.width,
            .bufferImageHeight = r->extent.height, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
            .imageOffset = {0, 0, 0}, .imageExtent = {r->extent.width, r->extent.height, 1u}};
        r->vkCmdCopyBufferToImage(cmd, r->staging, r->images[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &copy);
        finish_to_present(r, cmd, r->images[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT);
    } else {
        finish_to_present(r, cmd, r->images[image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    }
    if (r->vkEndCommandBuffer(cmd) != VK_SUCCESS) return false;
    gpu_phase_add(GPU_PHASE_PRESENT_RECORD, record_start);
    if (!submit_and_present(r, cmd, image, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, false)) return false;
    if (!run_complete_hook(r, rgba == NULL)) {
        r->stats.present_failures++;
        return false;
    }
    return true;
}

bool gpu_window_present(gpu_window *r, const float clear_rgba[4])
{
    return present_frame(r, NULL, 0u, 0u, 0u, clear_rgba);
}

bool gpu_window_present_pixels(gpu_window *r, const uint8_t *rgba, uint32_t width,
                               uint32_t height, uint32_t stride_bytes, const float clear_rgba[4])
{
    if (rgba == NULL) return false;
    return present_frame(r, rgba, width, height, stride_bytes, clear_rgba);
}

bool gpu_window_can_blit(const gpu_window *r)
{
    return r != NULL && r->swapchain != VK_NULL_HANDLE && r->can_upload && r->blit_exact;
}

/* T849: the pre-pass route. No render pass: the acquired image goes UNDEFINED -> TRANSFER_DST, the hook records its copies and
 * blits into it, then it goes to PRESENT_SRC. A refusing hook leaves a black frame (the acquired image must still be
 * submitted so the acquire semaphore is consumed) and the call returns false. */
bool gpu_window_present_blit(gpu_window *r, gpu_window_blit_hook hook, void *context)
{
    if (r == NULL || hook == NULL || r->swapchain == VK_NULL_HANDLE || !r->can_upload) return false;
    if (!wait_previous_present(r)) return false;
    uint32_t image = 0u;
    if (!acquire_image(r, &image)) return false;
    (void)r->vkResetFences(r->device, 1u, &r->fence);
    const uint64_t record_start = gpu_phase_now();
    VkCommandBuffer cmd = r->commands[image];
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (r->vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) return false;
    image_barrier(r, cmd, r->images[image], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0u,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    gpu_window_native native;
    bool recorded = gpu_window_get_native(r, &native);
    native.command_buffer = cmd;
    recorded = recorded && hook(&native, cmd, r->images[image], r->extent, r->format, context);
    if (!recorded) {
        const VkClearColorValue black = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}};
        const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
        r->vkCmdClearColorImage(cmd, r->images[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1u, &range);
        r->stats.blit_refusals++;
    }
    finish_to_present(r, cmd, r->images[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (r->vkEndCommandBuffer(cmd) != VK_SUCCESS) return false;
    gpu_phase_add(GPU_PHASE_PRESENT_RECORD, record_start);
    if (!submit_and_present(r, cmd, image, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, true)) return false;
    if (!run_complete_hook(r, false)) {
        r->stats.present_failures++;
        return false;
    }
    return recorded;
}

bool gpu_window_set_capture(gpu_window *r, bool enable)
{
    if (r == NULL || r->device == VK_NULL_HANDLE) return false;
    if (!enable) { r->capturing = false; return true; }
    if (!r->can_capture) return false;
    if (r->capture_buffer != VK_NULL_HANDLE) { r->capturing = true; return true; }
    const size_t bytes = (size_t)r->extent.width * r->extent.height * 4u;
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (r->vkCreateBuffer(r->device, &info, NULL, &r->capture_buffer) != VK_SUCCESS) { r->capture_buffer = VK_NULL_HANDLE; return false; }
    VkMemoryRequirements requirements;
    r->vkGetBufferMemoryRequirements(r->device, r->capture_buffer, &requirements);
    const uint32_t type = host_memory_type(&r->memory_properties, requirements.memoryTypeBits);
    VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = type};
    if (type == UINT32_MAX || r->vkAllocateMemory(r->device, &allocate, NULL, &r->capture_memory) != VK_SUCCESS ||
        r->vkBindBufferMemory(r->device, r->capture_buffer, r->capture_memory, 0u) != VK_SUCCESS) {
        if (r->capture_memory != VK_NULL_HANDLE) r->vkFreeMemory(r->device, r->capture_memory, NULL);
        r->vkDestroyBuffer(r->device, r->capture_buffer, NULL);
        r->capture_memory = VK_NULL_HANDLE; r->capture_buffer = VK_NULL_HANDLE;
        return false;
    }
    r->capturing = true;
    return true;
}

void gpu_window_extent(const gpu_window *r, uint32_t *width, uint32_t *height)
{
    if (width != NULL) *width = r != NULL ? r->extent.width : 0u;
    if (height != NULL) *height = r != NULL ? r->extent.height : 0u;
}

bool gpu_window_read_capture(const gpu_window *r, uint8_t *rgba, size_t capacity, uint32_t *width, uint32_t *height)
{
    if (r == NULL || rgba == NULL || r->capture_buffer == VK_NULL_HANDLE || r->stats.presented == 0u) return false;
    if (!s_present_sync && r->vkWaitForFences(r->device, 1u, &r->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) return false; /* T1262: the copy may still be queued */
    const size_t bytes = (size_t)r->extent.width * r->extent.height * 4u;
    if (capacity < bytes) return false;
    void *mapped = NULL;
    if (r->vkMapMemory(r->device, r->capture_memory, 0u, bytes, 0u, &mapped) != VK_SUCCESS || mapped == NULL) return false;
    const uint8_t *source = mapped;
    const bool swap_rb = r->format == VK_FORMAT_B8G8R8A8_UNORM || r->format == VK_FORMAT_B8G8R8A8_SRGB;
    for (size_t i = 0u; i < bytes; i += 4u) {
        rgba[i] = source[i + (swap_rb ? 2u : 0u)]; rgba[i + 1u] = source[i + 1u];
        rgba[i + 2u] = source[i + (swap_rb ? 0u : 2u)]; rgba[i + 3u] = source[i + 3u];
    }
    r->vkUnmapMemory(r->device, r->capture_memory);
    if (width != NULL) *width = r->extent.width;
    if (height != NULL) *height = r->extent.height;
    return true;
}

void gpu_window_feed_method(gpu_window *r, uint32_t method, uint32_t data)
{
    if (r == NULL) return;
    r->pending_packets++;
    if (method == 0x17FCu || method == 0x1810u || method == 0x1808u || method == 0x1800u) {
        (void)data;
        r->pending_methods++;
    }
}

bool gpu_window_get_native(const gpu_window *r, gpu_window_native *out_native)
{
    if (r == NULL || out_native == NULL || r->device == VK_NULL_HANDLE) return false;
    *out_native = (gpu_window_native){
        .instance = r->instance,
        .physical_device = r->physical,
        .device = r->device,
        .queue = r->queue,
        .queue_family = r->family,
        .command_pool = r->command_pool,
        .render_pass = r->render_pass,
        .command_buffer = r->recording_frame ? r->current_command_buffer : VK_NULL_HANDLE,
        .get_device_proc_addr = r->vkGetDeviceProcAddr,
        .extent = r->extent,
        .format = r->format,
        .memory_properties = r->memory_properties,
        .negative_viewport = r->negative_viewport,
        .sampler_anisotropy = r->sampler_anisotropy,
        .large_points = r->large_points,
        .fill_mode_non_solid = r->fill_mode_non_solid,
        .occlusion_query_precise = r->occlusion_query_precise,
        .swapchain_image_count = r->image_count,
        .swapchain_images = r->images,
    };
    return true;
}

void gpu_window_set_complete_hook(gpu_window *r, gpu_window_complete_hook hook, void *context)
{
    if (r == NULL) return;
    r->complete_hook = hook;
    r->complete_hook_context = context;
}

void gpu_window_set_frame_hook(gpu_window *r, gpu_window_frame_hook hook, void *context)
{
    if (r == NULL) return;
    r->frame_hook = hook;
    r->frame_hook_context = context;
}

gpu_window_stats gpu_window_get_stats(const gpu_window *r) { return r != NULL ? r->stats : (gpu_window_stats){0}; }
const char *gpu_window_device_name(const gpu_window *r) { return r != NULL ? r->device_name : ""; }

void gpu_window_destroy(gpu_window *r)
{
    if (r == NULL) return;
    if (r->device != VK_NULL_HANDLE) {
        if (r->vkDeviceWaitIdle != NULL) (void)r->vkDeviceWaitIdle(r->device);
        if (r->acquired != VK_NULL_HANDLE && r->vkDestroySemaphore) r->vkDestroySemaphore(r->device, r->acquired, NULL);
        if (r->finished != VK_NULL_HANDLE && r->vkDestroySemaphore) r->vkDestroySemaphore(r->device, r->finished, NULL);
        for (uint32_t i = 0u; r->finished_images != NULL && r->vkDestroySemaphore && i < r->finished_count; i++) {
            if (r->finished_images[i] != VK_NULL_HANDLE) r->vkDestroySemaphore(r->device, r->finished_images[i], NULL);
        }
        if (r->staging_mapped != NULL) r->vkUnmapMemory(r->device, r->staging_memory);
        if (r->staging != VK_NULL_HANDLE) r->vkDestroyBuffer(r->device, r->staging, NULL);
        if (r->staging_memory != VK_NULL_HANDLE) r->vkFreeMemory(r->device, r->staging_memory, NULL);
        if (r->fence != VK_NULL_HANDLE && r->vkDestroyFence) r->vkDestroyFence(r->device, r->fence, NULL);
        if (r->capture_buffer != VK_NULL_HANDLE) r->vkDestroyBuffer(r->device, r->capture_buffer, NULL);
        if (r->capture_memory != VK_NULL_HANDLE) r->vkFreeMemory(r->device, r->capture_memory, NULL);
        release_swapchain(r, false);
        if (r->command_pool != VK_NULL_HANDLE && r->vkDestroyCommandPool) r->vkDestroyCommandPool(r->device, r->command_pool, NULL);
        if (r->vkDestroyDevice) r->vkDestroyDevice(r->device, NULL);
    }
    if (r->surface != VK_NULL_HANDLE && r->vkDestroySurfaceKHR != NULL)
        r->vkDestroySurfaceKHR(r->instance, r->surface, NULL);
    if (r->instance != VK_NULL_HANDLE && r->vkDestroyInstance != NULL) r->vkDestroyInstance(r->instance, NULL);
    free(r->finished_images);
    free(r->scale_x);
    free(r);
}
