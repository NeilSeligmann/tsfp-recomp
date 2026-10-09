/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T793 live_vk_target on a real Vulkan device (no surface, no SDL): every CopyRects blit shape runs on the device and its
 * result is compared byte for byte with the CPU reference executor of live_target (live_target_apply_blit on CPU images),
 * the copy events hook-up, the present schedule driven at modelled vblanks with a fake present callback, the movie overlay
 * letterboxed over the front, and the texture cache registration. Exits 77 when no Vulkan loader or device exists.
 */
#include "live_vk_target.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(condition)                                                          \
    do {                                                                          \
        checks++;                                                                 \
        if (!(condition)) {                                                       \
            failures++;                                                           \
            printf("  FAIL line %d: %s\n", __LINE__, #condition);                 \
        }                                                                         \
    } while (0)

#define FORMAT_A8R8G8B8 0x00011229u
#define FORMAT_X8R8G8B8 0x00011E29u
#define FORMAT_R5G6B5 0x00011129u
#define A_DATA 0x0100000u
#define B_DATA 0x0200000u
#define C_DATA 0x0300000u
#define D_DATA 0x0400000u
#define E_DATA 0x0500000u
#define F_DATA 0x0600000u
#define SIZE_16X8 (15u | (7u << 12))               /* pitch 64 */
#define SIZE_16X8_P128 (15u | (7u << 12) | (1u << 24))
#define SIZE_32X8_R5 (31u | (7u << 12))            /* 16 bit: 32 texels, pitch 64 */

typedef struct {
    VkInstance instance;
    VkDevice device;
    VkQueue queue;
    VkCommandPool pool;
    live_vk_target_device description;
    char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
} vulkan;

static bool make_vulkan(vulkan *vk)
{
    void *library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (library == NULL) {
        return false;
    }
    PFN_vkGetInstanceProcAddr gipa = NULL;
    void *symbol = dlsym(library, "vkGetInstanceProcAddr");
    memcpy(&gipa, &symbol, sizeof gipa);
    if (gipa == NULL) {
        return false;
    }
#define GLOBAL(name) PFN_##name name = NULL; { PFN_vkVoidFunction raw = gipa(VK_NULL_HANDLE, #name); memcpy(&name, &raw, sizeof name); }
    GLOBAL(vkCreateInstance)
    if (vkCreateInstance == NULL) {
        return false;
    }
    const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0};
    const VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    if (vkCreateInstance(&info, NULL, &vk->instance) != VK_SUCCESS) {
        return false;
    }
#define INSTANCE(name) PFN_##name name = NULL; { PFN_vkVoidFunction raw = gipa(vk->instance, #name); memcpy(&name, &raw, sizeof name); }
    INSTANCE(vkEnumeratePhysicalDevices)
    INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties)
    INSTANCE(vkGetPhysicalDeviceMemoryProperties)
    INSTANCE(vkGetPhysicalDeviceProperties)
    INSTANCE(vkCreateDevice)
    INSTANCE(vkGetDeviceProcAddr)
    uint32_t count = 0u;
    VkPhysicalDevice devices[8];
    if (vkEnumeratePhysicalDevices(vk->instance, &count, NULL) != VK_SUCCESS || count == 0u) {
        return false;
    }
    if (count > 8u) {
        count = 8u;
    }
    vkEnumeratePhysicalDevices(vk->instance, &count, devices);
    for (uint32_t d = 0u; d < count; d++) {
        uint32_t queues = 0u;
        VkQueueFamilyProperties props[16];
        vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &queues, NULL);
        if (queues > 16u) {
            queues = 16u;
        }
        vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &queues, props);
        for (uint32_t q = 0u; q < queues; q++) {
            if ((props[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0u) {
                continue;
            }
            const float priority = 1.0f;
            const VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                .queueFamilyIndex = q, .queueCount = 1u, .pQueuePriorities = &priority};
            const VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                .queueCreateInfoCount = 1u, .pQueueCreateInfos = &queue_info};
            if (vkCreateDevice(devices[d], &device_info, NULL, &vk->device) != VK_SUCCESS) {
                continue;
            }
            PFN_vkGetDeviceProcAddr device_proc = vkGetDeviceProcAddr;
            PFN_vkGetDeviceQueue get_queue = NULL;
            PFN_vkCreateCommandPool create_pool = NULL;
            PFN_vkVoidFunction raw = device_proc(vk->device, "vkGetDeviceQueue");
            memcpy(&get_queue, &raw, sizeof get_queue);
            raw = device_proc(vk->device, "vkCreateCommandPool");
            memcpy(&create_pool, &raw, sizeof create_pool);
            get_queue(vk->device, q, 0u, &vk->queue);
            const VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = q};
            if (create_pool(vk->device, &pool_info, NULL, &vk->pool) != VK_SUCCESS) {
                return false;
            }
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(devices[d], &properties);
            snprintf(vk->name, sizeof vk->name, "%s", properties.deviceName);
            vk->description = (live_vk_target_device){.device = vk->device, .queue = vk->queue, .queue_family = q,
                .command_pool = vk->pool, .get_device_proc_addr = device_proc};
            vkGetPhysicalDeviceMemoryProperties(devices[d], &vk->description.memory);
            return true;
        }
    }
    return false;
}

static void fill(uint8_t *pixels, size_t bytes, uint8_t seed)
{
    for (size_t i = 0; i < bytes; i++) {
        pixels[i] = (uint8_t)(seed + i * 7u + (i >> 5));
    }
}

typedef struct {
    uint32_t data, format_word, size_word;
} target_spec;

static const target_spec SPECS[] = {
    {A_DATA, FORMAT_A8R8G8B8, SIZE_16X8},      {B_DATA, FORMAT_A8R8G8B8, SIZE_16X8},
    {C_DATA, FORMAT_X8R8G8B8, SIZE_16X8},      {D_DATA, FORMAT_A8R8G8B8, SIZE_16X8_P128},
    {E_DATA, FORMAT_R5G6B5, SIZE_32X8_R5},     {F_DATA, FORMAT_R5G6B5, SIZE_32X8_R5},
};
#define SPEC_COUNT (sizeof SPECS / sizeof SPECS[0])

typedef struct {
    live_vk_target_set *set;
    live_target_registry *reference;
} rig;

static bool make_rig(const vulkan *vk, uint32_t allowed, live_texture_cache *cache, rig *out)
{
    out->set = live_vk_target_create(&vk->description, allowed, cache);
    out->reference = live_target_registry_create();
    if (out->set == NULL || out->reference == NULL) {
        return false;
    }
    for (size_t i = 0; i < SPEC_COUNT; i++) {
        live_target_refusal refusal;
        if (live_vk_target_register(out->set, SPECS[i].data, SPECS[i].format_word, SPECS[i].size_word, &refusal) != LIVE_VK_OK ||
            live_target_register(out->reference, SPECS[i].data, SPECS[i].format_word, SPECS[i].size_word, true) != LIVE_TARGET_OK) {
            return false;
        }
        const live_target_desc *desc = live_target_find(out->reference, SPECS[i].data);
        const size_t bytes = (size_t)desc->pitch * desc->height;
        fill(live_target_pixels(out->reference, SPECS[i].data), bytes, (uint8_t)(i * 37u + 5u));
        live_target_note_written(out->reference, SPECS[i].data);   /* the upload below bumps the device side too */
        if (live_vk_target_upload(out->set, SPECS[i].data, live_target_pixels(out->reference, SPECS[i].data), bytes) != LIVE_VK_OK) {
            return false;
        }
    }
    return true;
}

static void free_rig(rig *r)
{
    live_vk_target_destroy(r->set);
    live_target_registry_destroy(r->reference);
}

/* Every target of the device equals the CPU reference image, and the generations agree. */
static bool all_equal(rig *r)
{
    bool equal = true;
    for (size_t i = 0; i < SPEC_COUNT; i++) {
        const live_target_desc *desc = live_target_find(r->reference, SPECS[i].data);
        const size_t bytes = (size_t)desc->pitch * desc->height;
        uint8_t *device_bytes = malloc(bytes);
        if (live_vk_target_readback(r->set, SPECS[i].data, device_bytes, bytes) != LIVE_VK_OK ||
            memcmp(device_bytes, live_target_pixels(r->reference, SPECS[i].data), bytes) != 0 ||
            live_target_generation(live_vk_target_registry(r->set), SPECS[i].data) !=
                live_target_generation(r->reference, SPECS[i].data)) {
            equal = false;
        }
        free(device_bytes);
    }
    return equal;
}

typedef struct {
    const char *name;
    live_target_blit blit;
    bool copy_image;   /* expected route */
    bool empty;
} shape;

static live_target_blit blit_of(uint32_t source, uint32_t destination, uint32_t format, uint32_t source_pitch,
                                uint32_t destination_pitch, uint32_t in_x, uint32_t in_y, uint32_t out_x, uint32_t out_y,
                                uint32_t w, uint32_t h)
{
    return (live_target_blit){source, destination, format, source_pitch, destination_pitch, in_x, in_y, out_x, out_y, w, h,
                              LIVE_BLIT_OPERATION_SRCCOPY};
}

static void test_blit_shapes(const vulkan *vk)
{
    const shape shapes[] = {
        {"distinct 4x4", blit_of(A_DATA, B_DATA, 0xA, 64, 64, 2, 1, 5, 3, 4, 4), true, false},
        {"whole image", blit_of(A_DATA, B_DATA, 0xA, 64, 64, 0, 0, 0, 0, 16, 8), true, false},
        {"same surface disjoint", blit_of(A_DATA, A_DATA, 0xA, 64, 64, 0, 0, 8, 4, 4, 4), true, false},
        {"same surface 3 px right", blit_of(B_DATA, B_DATA, 0xA, 64, 64, 0, 2, 3, 2, 10, 2), false, false},
        {"same surface 3 rows down smear", blit_of(A_DATA, A_DATA, 0xA, 64, 64, 1, 0, 1, 3, 8, 5), false, false},
        {"alpha forced FF", blit_of(A_DATA, B_DATA, 0x7, 64, 64, 0, 0, 0, 0, 16, 8), false, false},
        {"alpha forced 00", blit_of(B_DATA, A_DATA, 0x6, 64, 64, 4, 4, 0, 0, 8, 4), false, false},
        {"x8r8g8b8 target alpha ff", blit_of(C_DATA, B_DATA, 0x7, 64, 64, 0, 0, 0, 0, 16, 8), false, false},
        {"wide source clamped to narrow destination", blit_of(D_DATA, A_DATA, 0xA, 128, 64, 0, 0, 0, 0, 32, 8), true, false},
        {"empty width", blit_of(A_DATA, B_DATA, 0xA, 64, 64, 0, 0, 0, 0, 0, 4), false, true},
        {"empty height", blit_of(A_DATA, B_DATA, 0xA, 64, 64, 0, 0, 0, 0, 4, 0), false, true},
        {"r5g6b5 byte copy", blit_of(E_DATA, F_DATA, 0x4, 64, 64, 1, 1, 2, 2, 8, 4), false, false},
    };
    for (size_t i = 0; i < sizeof shapes / sizeof shapes[0]; i++) {
        rig r;
        CHECK(make_rig(vk, LIVE_TARGET_INFER_BYTE_FORMATS, NULL, &r));
        CHECK(all_equal(&r));
        live_target_blit_plan device_plan, reference_plan;
        live_target_refusal refusal = LIVE_TARGET_OK;
        bool executed = false;
        const live_target_refusal reference =
            live_target_apply_blit(r.reference, &shapes[i].blit, LIVE_TARGET_INFER_BYTE_FORMATS, &reference_plan, &executed);
        const live_vk_status status = live_vk_target_blit(r.set, &shapes[i].blit, &device_plan, &refusal);
        if (!(status == LIVE_VK_OK && reference == LIVE_TARGET_OK)) {
            printf("  shape '%s': status %d refusal %d reference %d\n", shapes[i].name, (int)status, (int)refusal, (int)reference);
        }
        CHECK(status == LIVE_VK_OK && reference == LIVE_TARGET_OK);
        CHECK(device_plan.empty == shapes[i].empty);
        if (!shapes[i].empty) {
            CHECK(device_plan.copy_image_ok == shapes[i].copy_image);
        }
        const live_vk_target_stats stats = live_vk_target_stats_get(r.set);
        CHECK(stats.copy_image_blits == (!shapes[i].empty && shapes[i].copy_image ? 1u : 0u));
        CHECK(stats.staged_blits == (!shapes[i].empty && !shapes[i].copy_image ? 1u : 0u));
        CHECK(stats.empty_blits == (shapes[i].empty ? 1u : 0u));
        if (!all_equal(&r)) {
            printf("  shape '%s': device bytes differ from the CPU reference\n", shapes[i].name);
        }
        CHECK(all_equal(&r));
        free_rig(&r);
    }
}

static void test_refusals(const vulkan *vk)
{
    const shape refused[] = {
        {"unknown source", blit_of(0x0F00000u, B_DATA, 0xA, 64, 64, 0, 0, 0, 0, 4, 4), false, false},
        {"unknown destination", blit_of(A_DATA, 0x0F00000u, 0xA, 64, 64, 0, 0, 0, 0, 4, 4), false, false},
        {"out of bounds", blit_of(A_DATA, B_DATA, 0xA, 64, 64, 14, 0, 0, 0, 4, 4), false, false},
        {"format mismatch", blit_of(A_DATA, E_DATA, 0xA, 64, 64, 0, 0, 0, 0, 4, 4), false, false},
        {"wrong pitch", blit_of(A_DATA, B_DATA, 0xA, 128, 64, 0, 0, 0, 0, 4, 4), false, false},
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        rig r;
        CHECK(make_rig(vk, 0u, NULL, &r));
        live_target_refusal refusal = LIVE_TARGET_OK;
        live_target_refusal reference = LIVE_TARGET_OK;
        live_target_blit_plan plan;
        CHECK(live_target_plan_blit(r.reference, &refused[i].blit, 0u, &plan) != LIVE_TARGET_OK);
        reference = live_target_plan_blit(r.reference, &refused[i].blit, 0u, &plan);
        CHECK(live_vk_target_blit(r.set, &refused[i].blit, NULL, &refusal) == LIVE_VK_PLAN_REFUSED);
        CHECK(refusal == reference && refusal != LIVE_TARGET_OK);
        CHECK(live_vk_target_stats_get(r.set).refused_blits == 1u);
        CHECK(live_target_registry_stats(live_vk_target_registry(r.set)).refusals[refusal] == 1u);
        CHECK(all_equal(&r));   /* a refused blit moves nothing and bumps no generation */
        free_rig(&r);
    }
    /* INFERRED byte formats stay opt-in: without LIVE_TARGET_INFER_BYTE_FORMATS the blit is refused and nothing moves */
    rig r;
    CHECK(make_rig(vk, 0u, NULL, &r));
    const live_target_blit inferred = blit_of(E_DATA, F_DATA, 0x4, 64, 64, 0, 0, 0, 0, 8, 4);
    live_target_refusal refusal = LIVE_TARGET_OK;
    CHECK(live_vk_target_blit(r.set, &inferred, NULL, &refusal) == LIVE_VK_PLAN_REFUSED);
    CHECK(refusal == LIVE_TARGET_REFUSE_INFERRED_BYTE_FORMAT);
    CHECK(all_equal(&r));
    free_rig(&r);
}

static void test_copy_events(const vulkan *vk)
{
    rig r;
    CHECK(make_rig(vk, 0u, NULL, &r));
    gpu_pgraph_copy copies[3];
    memset(copies, 0, sizeof copies);
    for (size_t i = 0; i < 3; i++) {
        copies[i] = (gpu_pgraph_copy){.command = (uint32_t)(10 + i), .source_offset = A_DATA, .destination_offset = B_DATA,
                                      .color_format = 0xA, .source_pitch = 64, .destination_pitch = 64, .in_x = (uint32_t)i,
                                      .out_x = (uint32_t)(i * 2), .width = 4, .height = 4, .operation = LIVE_BLIT_OPERATION_SRCCOPY};
    }
    copies[1].source_offset = 0x0F00000u;   /* no such target: the stream stops here, the third event never runs */
    const live_target_blit mapped = live_vk_target_blit_from_copy(&copies[2]);
    CHECK(mapped.source_data == A_DATA && mapped.destination_data == B_DATA && mapped.in_x == 2u && mapped.out_x == 4u &&
          mapped.width == 4u && mapped.height == 4u && mapped.color_format == 0xAu && mapped.source_pitch == 64u &&
          mapped.destination_pitch == 64u && mapped.operation == LIVE_BLIT_OPERATION_SRCCOPY);
    size_t failure = 99u;
    live_target_refusal refusal = LIVE_TARGET_OK;
    CHECK(live_vk_target_apply_copies(r.set, copies, 3u, &failure, &refusal) == 1u);
    CHECK(failure == 1u && refusal == LIVE_TARGET_REFUSE_NO_SOURCE);
    const live_target_blit first = live_vk_target_blit_from_copy(&copies[0]);
    CHECK(live_target_apply_blit(r.reference, &first, 0u, NULL, NULL) == LIVE_TARGET_OK);
    CHECK(all_equal(&r));
    CHECK(live_vk_target_stats_get(r.set).copy_image_blits == 1u);
    size_t none = 99u;
    const gpu_pgraph_copy valid[2] = {copies[0], copies[2]};
    CHECK(live_vk_target_apply_copies(r.set, valid, 2u, &none, NULL) == 2u && none == 2u);
    for (size_t i = 0; i < 2; i++) {
        const live_target_blit blit = live_vk_target_blit_from_copy(&valid[i]);
        CHECK(live_target_apply_blit(r.reference, &blit, 0u, NULL, NULL) == LIVE_TARGET_OK);
    }
    CHECK(all_equal(&r));
    free_rig(&r);
}

static PFN_vkVoidFunction fake_gipa_result;
static uint32_t fake_gipa_calls;

static void fake_memory_properties(VkPhysicalDevice device, VkPhysicalDeviceMemoryProperties *out)
{
    memset(out, 0, sizeof *out);
    out->memoryTypeCount = (uint32_t)(uintptr_t)device;
    out->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
}

static PFN_vkVoidFunction fake_gipa(VkInstance instance, const char *name)
{
    fake_gipa_calls++;
    if (instance == (VkInstance)(uintptr_t)0x11u && strcmp(name, "vkGetPhysicalDeviceMemoryProperties") == 0) {
        return fake_gipa_result;
    }
    return NULL;
}

static PFN_vkVoidFunction fake_gdpa(VkDevice device, const char *name)
{
    (void)device;
    (void)name;
    return NULL;
}

static void test_device_from_native(void)
{
    PFN_vkGetPhysicalDeviceMemoryProperties properties = fake_memory_properties;
    memcpy(&fake_gipa_result, &properties, sizeof properties);
    gpu_window_native native;
    memset(&native, 0, sizeof native);
    native.instance = (VkInstance)(uintptr_t)0x11u;
    native.physical_device = (VkPhysicalDevice)(uintptr_t)3u;
    native.device = (VkDevice)(uintptr_t)0x22u;
    native.queue = (VkQueue)(uintptr_t)0x33u;
    native.queue_family = 5u;
    native.command_pool = (VkCommandPool)(uintptr_t)0x44u;
    native.get_device_proc_addr = fake_gdpa;
    live_vk_target_device out;
    CHECK(live_vk_target_device_from_native(&native, fake_gipa, &out));
    CHECK(out.device == native.device && out.queue == native.queue && out.queue_family == 5u &&
          out.command_pool == native.command_pool && out.get_device_proc_addr == fake_gdpa);
    CHECK(out.memory.memoryTypeCount == 3u && out.memory.memoryTypes[0].propertyFlags == VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    CHECK(!live_vk_target_device_from_native(NULL, fake_gipa, &out));
    CHECK(!live_vk_target_device_from_native(&native, NULL, &out));
    CHECK(!live_vk_target_device_from_native(&native, fake_gipa, NULL));
    gpu_window_native incomplete = native;
    incomplete.device = VK_NULL_HANDLE;
    CHECK(!live_vk_target_device_from_native(&incomplete, fake_gipa, &out));
    incomplete = native;
    incomplete.get_device_proc_addr = NULL;
    CHECK(!live_vk_target_device_from_native(&incomplete, fake_gipa, &out));
    incomplete = native;
    incomplete.instance = (VkInstance)(uintptr_t)0x99u;   /* the loader knows no such function for it */
    CHECK(!live_vk_target_device_from_native(&incomplete, fake_gipa, &out));
    /* no device, no queue, no pool, no loader entry: no set */
    live_vk_target_device none = out;
    none.queue = VK_NULL_HANDLE;
    CHECK(live_vk_target_create(&none, 0u, NULL) == NULL);
    none = out;
    none.command_pool = VK_NULL_HANDLE;
    CHECK(live_vk_target_create(&none, 0u, NULL) == NULL);
    CHECK(live_vk_target_create(&out, 0u, NULL) == NULL);   /* the fake loader resolves no device function */
    CHECK(live_vk_target_create(NULL, 0u, NULL) == NULL);
}

static void test_registration(const vulkan *vk)
{
    live_texture_cache cache;
    live_texture_cache_init(&cache, false);
    rig r;
    CHECK(make_rig(vk, 0u, &cache, &r));
    /* linear A8R8G8B8 targets only: A, B and D (not X8R8G8B8 or 16 bit; Y8 has no registrable surface format) */
    CHECK(live_vk_target_stats_get(r.set).texture_registrations == 3u);
    CHECK(live_vk_target_stats_get(r.set).images_created == SPEC_COUNT);
    live_texture_binding binding = {0x300u, FORMAT_A8R8G8B8, SIZE_16X8_P128, D_DATA, 0u};
    live_texture_result result;
    live_texture_lookup(&cache, &binding, NULL, NULL, &result);
    CHECK(result.source == LIVE_TEXTURE_SOURCE_TARGET && result.target_id == 4u);
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {0u, 0u};
    uint64_t generation = 99u;
    CHECK(live_vk_target_image(r.set, D_DATA, &format, &extent, &generation) != VK_NULL_HANDLE);
    CHECK(format == VK_FORMAT_B8G8R8A8_UNORM && extent.width == 32u && extent.height == 8u);   /* pitch 128 / 4 */
    CHECK(live_vk_target_image(r.set, E_DATA, &format, &extent, NULL) != VK_NULL_HANDLE && format == VK_FORMAT_R5G6B5_UNORM_PACK16);
    CHECK(live_vk_target_image(r.set, 0x0F00000u, NULL, NULL, NULL) == VK_NULL_HANDLE);
    /* the generation is shared: an upload (x1), a draw note and a blit each bump it */
    const uint64_t before = live_target_generation(live_vk_target_registry(r.set), A_DATA);
    live_vk_target_note_written(r.set, A_DATA);
    CHECK(live_target_generation(live_vk_target_registry(r.set), A_DATA) == before + 1u);
    uint8_t small[16] = {0};
    CHECK(live_vk_target_upload(r.set, A_DATA, small, sizeof small) == LIVE_VK_ARGUMENT);
    CHECK(live_vk_target_readback(r.set, A_DATA, small, sizeof small) == LIVE_VK_ARGUMENT);
    CHECK(live_vk_target_upload(r.set, 0x0F00000u, small, sizeof small) == LIVE_VK_UNKNOWN_TARGET);
    CHECK(live_target_generation(live_vk_target_registry(r.set), A_DATA) == before + 1u);   /* a refused upload bumps nothing */
    /* a re-declaration with other geometry is refused, no second image */
    live_target_refusal refusal = LIVE_TARGET_OK;
    CHECK(live_vk_target_register(r.set, A_DATA, FORMAT_A8R8G8B8, SIZE_16X8_P128, &refusal) == LIVE_VK_PLAN_REFUSED &&
          refusal == LIVE_TARGET_REFUSE_REDECLARED);
    CHECK(live_vk_target_stats_get(r.set).images_created == SPEC_COUNT);
    /* T1489: a swizzled A8R8G8B8 target gets an image and is offered to the sampler, flagged swizzled (only a swizzled header of its size takes it) */
    CHECK(live_vk_target_register(r.set, 0x0800000u, 0x00000600u | (4u << 20) | (3u << 24), 0u, NULL) == LIVE_VK_OK);
    CHECK(live_vk_target_stats_get(r.set).texture_registrations == 4u);
    live_texture_binding swizzled_binding = {0u, 0x00000629u | (1u << 16) | (4u << 20) | (3u << 24), 0u, 0x0800000u, 0u};
    live_texture_lookup(&cache, &swizzled_binding, NULL, NULL, &result);
    CHECK(result.source == LIVE_TEXTURE_SOURCE_REFUSED); /* the swizzled A8R8G8B8 format is INFERRED, opt-in only (the host's --live-inferred) */
    cache.allow_inferred = true;
    live_texture_lookup(&cache, &swizzled_binding, NULL, NULL, &result);
    CHECK(result.source == LIVE_TEXTURE_SOURCE_TARGET && result.target_swizzled && result.width == 16u && result.height == 8u);
    swizzled_binding.data = D_DATA;
    live_texture_lookup(&cache, &swizzled_binding, NULL, NULL, &result);
    CHECK(result.source == LIVE_TEXTURE_SOURCE_REFUSED); /* a swizzled header over a linear target stays refused */
    cache.allow_inferred = false;
    free_rig(&r);
    live_texture_cache_free(&cache);
}

/* --- present --- */

typedef struct {
    unsigned calls;
    uint32_t width, height, stride;
    uint8_t *last;
    size_t capacity;
    bool fail_next;
} sink;

static bool sink_present(void *context, const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t stride)
{
    sink *s = context;
    s->calls++;
    if (s->fail_next) {
        s->fail_next = false;
        return false;
    }
    s->width = width;
    s->height = height;
    s->stride = stride;
    const size_t bytes = (size_t)stride * height;
    if (bytes > s->capacity) {
        s->last = realloc(s->last, bytes);
        s->capacity = bytes;
    }
    memcpy(s->last, rgba, bytes);
    return true;
}

static d3d8_frame_record swap_record(uint64_t number, uint64_t vblank, uint32_t data, uint32_t interval)
{
    d3d8_frame_record record;
    memset(&record, 0, sizeof record);
    record.number = number;
    record.interval = interval;
    record.vblank = vblank;
    record.data = data;
    record.format_word = FORMAT_A8R8G8B8;
    record.size_word = SIZE_16X8;
    return record;
}

static bool rgba_of_bgra_matches(const uint8_t *rgba, const uint8_t *bgra, uint32_t pixels)
{
    for (uint32_t i = 0; i < pixels; i++) {
        if (rgba[i * 4u] != bgra[i * 4u + 2u] || rgba[i * 4u + 1u] != bgra[i * 4u + 1u] ||
            rgba[i * 4u + 2u] != bgra[i * 4u] || rgba[i * 4u + 3u] != 0xFFu) {
            return false;
        }
    }
    return true;
}

static void test_present(const vulkan *vk)
{
    rig r;
    CHECK(make_rig(vk, 0u, NULL, &r));
    sink s;
    memset(&s, 0, sizeof s);
    live_vk_target_set_present(r.set, sink_present, &s);
    live_present_frame frame;
    /* nothing queued: a black 640x480 frame, no layer */
    CHECK(live_vk_target_vblank(r.set, 9u, &frame));
    CHECK(frame.layer_count == 0u && s.width == 640u && s.height == 480u && s.stride == 2560u);
    CHECK(s.last[0] == 0u && s.last[3] == 0xFFu && s.last[2559] == 0xFFu);
    CHECK(live_vk_target_stats_get(r.set).black_frames == 1u);
    /* the Swap at vblank 10 shows at 11 (one vblank late, INFERRED) */
    const d3d8_frame_record swap = swap_record(1u, 10u, A_DATA, 1u);
    CHECK(live_vk_target_present_submit(r.set, &swap) == LIVE_TARGET_OK);
    CHECK(live_vk_target_vblank(r.set, 10u, &frame) && frame.layer_count == 0u);
    CHECK(live_vk_target_vblank(r.set, 11u, &frame));
    CHECK(frame.front_new && frame.layer_count == 1u && frame.layers[0] == LIVE_LAYER_FRONT && frame.front_data == A_DATA);
    CHECK(s.width == 16u && s.height == 8u && s.stride == 64u);
    uint8_t *front = live_target_pixels(r.reference, A_DATA);
    CHECK(rgba_of_bgra_matches(s.last, front, 16u * 8u));   /* the CPU image A holds what the device shows */
    CHECK(live_vk_target_stats_get(r.set).front_readbacks == 1u);
    /* held: same generation, no new readback */
    CHECK(live_vk_target_vblank(r.set, 12u, &frame) && frame.front_held && !frame.front_new);
    CHECK(live_vk_target_stats_get(r.set).front_readbacks == 1u && live_vk_target_stats_get(r.set).front_cache_hits == 1u);
    CHECK(rgba_of_bgra_matches(s.last, front, 16u * 8u));
    /* a CopyRects into the front changes what the next vblank shows */
    const live_target_blit blit = blit_of(B_DATA, A_DATA, 0xA, 64, 64, 0, 0, 0, 0, 16, 8);
    CHECK(live_vk_target_blit(r.set, &blit, NULL, NULL) == LIVE_VK_OK);
    CHECK(live_target_apply_blit(r.reference, &blit, 0u, NULL, NULL) == LIVE_TARGET_OK);
    CHECK(live_vk_target_vblank(r.set, 13u, &frame));
    CHECK(live_vk_target_stats_get(r.set).front_readbacks == 2u);
    CHECK(rgba_of_bgra_matches(s.last, live_target_pixels(r.reference, A_DATA), 16u * 8u));
    /* a Swap of another target whose generation equals the cached front's must show ITS pixels, not the cached ones */
    fill(live_target_pixels(r.reference, B_DATA), 64u * 8u, 0xC3u);   /* B now differs from A, which the blit made equal to it */
    CHECK(live_vk_target_upload(r.set, B_DATA, live_target_pixels(r.reference, B_DATA), 64u * 8u) == LIVE_VK_OK);
    live_target_note_written(r.reference, B_DATA);
    CHECK(live_target_generation(live_vk_target_registry(r.set), B_DATA) == live_target_generation(live_vk_target_registry(r.set), A_DATA));
    const d3d8_frame_record other = swap_record(2u, 14u, B_DATA, 1u);
    CHECK(live_vk_target_present_submit(r.set, &other) == LIVE_TARGET_OK);
    CHECK(live_vk_target_vblank(r.set, 15u, &frame) && frame.front_new && frame.front_data == B_DATA);
    CHECK(rgba_of_bgra_matches(s.last, live_target_pixels(r.reference, B_DATA), 16u * 8u));
    /* two Swaps replaced before they showed: the newest eligible wins, the older one is superseded */
    const d3d8_frame_record third = swap_record(3u, 16u, B_DATA, 1u);
    const d3d8_frame_record fourth = swap_record(4u, 16u, A_DATA, 1u);
    CHECK(live_vk_target_present_submit(r.set, &third) == LIVE_TARGET_OK);
    CHECK(live_vk_target_present_submit(r.set, &fourth) == LIVE_TARGET_OK);
    CHECK(live_vk_target_vblank(r.set, 17u, &frame) && frame.front_new && frame.front_data == A_DATA && frame.front_number == 4u);
    CHECK(live_vk_target_schedule(r.set).superseded == 1u);
    /* the movie overlay: a 4x4 picture letterboxed above the 16x8 front, the front shows in the bars */
    uint8_t picture[4 * 4 * 3];
    for (size_t i = 0; i < 16u; i++) {
        picture[i * 3u] = 200u;
        picture[i * 3u + 1u] = (uint8_t)(i * 10u);
        picture[i * 3u + 2u] = 7u;
    }
    live_vk_target_overlay_submit(r.set, 4u, 4u, picture);
    CHECK(live_vk_target_vblank(r.set, 18u, &frame));
    CHECK(frame.layer_count == 2u && frame.layers[0] == LIVE_LAYER_FRONT && frame.layers[1] == LIVE_LAYER_OVERLAY);
    CHECK(s.width == 16u && s.height == 8u);
    const uint8_t *shown = s.last;
    uint8_t *current = live_target_pixels(r.reference, A_DATA);
    CHECK(shown[0] == current[2] && shown[1] == current[1] && shown[2] == current[0]);   /* bar, left of x = 4 */
    CHECK(shown[(3 * 16 + 15) * 4] == current[(3 * 16 + 15) * 4 + 2]);                   /* bar, right of x = 11 */
    CHECK(shown[(0 * 16 + 4) * 4] == 200u && shown[(0 * 16 + 4) * 4 + 1] == 0u && shown[(0 * 16 + 4) * 4 + 2] == 7u);
    CHECK(shown[(7 * 16 + 11) * 4 + 1] == 150u);   /* picture pixel (3, 3) */
    CHECK(live_vk_target_stats_get(r.set).overlay_composed == 1u);
    /* clearing the overlay leaves the front alone */
    live_vk_target_overlay_clear(r.set);
    CHECK(live_vk_target_vblank(r.set, 19u, &frame) && frame.layer_count == 1u);
    CHECK(rgba_of_bgra_matches(s.last, current, 16u * 8u));
    /* a present callback that fails is counted, never hidden */
    s.fail_next = true;
    CHECK(!live_vk_target_vblank(r.set, 20u, &frame));
    CHECK(live_vk_target_stats_get(r.set).present_callback_failures == 1u);
    /* an undecodable front header is refused and counted, not queued */
    d3d8_frame_record bad = swap_record(9u, 21u, A_DATA, 1u);
    bad.format_word = 0u;
    bad.size_word = 0u;
    CHECK(live_vk_target_present_submit(r.set, &bad) != LIVE_TARGET_OK);
    CHECK(live_vk_target_stats_get(r.set).present_refused == 1u);
    /* the observer entry point reaches the bound set, and is a no-op once unbound */
    live_vk_target_bind_observer(r.set);
    const d3d8_frame_record observed = swap_record(10u, 22u, B_DATA, 1u);
    live_vk_target_present_observer(&observed);
    CHECK(live_vk_target_stats_get(r.set).presents_submitted == 5u);
    live_vk_target_bind_observer(NULL);
    live_vk_target_present_observer(&observed);
    CHECK(live_vk_target_stats_get(r.set).presents_submitted == 5u);
    /* every vblank reached the callback and exactly the failed one was not counted as presented */
    CHECK(s.calls == live_vk_target_stats_get(r.set).vblanks);
    CHECK(live_vk_target_stats_get(r.set).frames_presented + 1u == s.calls);
    free(s.last);
    free_rig(&r);
}

/* the overlay alone, with no front yet, takes the picture's own size */
static void test_overlay_only(const vulkan *vk)
{
    rig r;
    CHECK(make_rig(vk, 0u, NULL, &r));
    sink s;
    memset(&s, 0, sizeof s);
    live_vk_target_set_present(r.set, sink_present, &s);
    uint8_t picture[2 * 2 * 3] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    live_vk_target_overlay_submit(r.set, 2u, 2u, picture);
    live_present_frame frame;
    CHECK(live_vk_target_vblank(r.set, 1u, &frame) && frame.layer_count == 1u && frame.layers[0] == LIVE_LAYER_OVERLAY);
    CHECK(s.width == 2u && s.height == 2u && s.last[0] == 1u && s.last[1] == 2u && s.last[2] == 3u && s.last[3] == 0xFFu &&
          s.last[12] == 10u);
    free(s.last);
    free_rig(&r);
}

static void test_media_present_snapshots_front(const vulkan *vk)
{
    rig r;
    CHECK(make_rig(vk, 0u, NULL, &r));
    sink s;
    memset(&s, 0, sizeof s);
    live_vk_target_set_present(r.set, sink_present, &s);
    const size_t bytes = 16u * 8u * 4u;
    uint8_t original[16u * 8u * 4u];
    memcpy(original, live_target_pixels(r.reference, A_DATA), bytes);
    d3d8_frame_record swap = swap_record(70u, 10u, A_DATA, 1u);
    CHECK(live_vk_target_present_submit_at(r.set, &swap, 1000000000u) == LIVE_TARGET_OK);
    CHECK(live_vk_target_stats_get(r.set).media_snapshots == 1u);
    const uint8_t movie_pixel[3] = {20u, 30u, 40u};
    live_vk_target_overlay_submit(r.set, 1u, 1u, movie_pixel);

    /* The guest may reuse this front target before audio reaches the queued present. */
    fill(live_target_pixels(r.reference, A_DATA), bytes, 0x11u);
    CHECK(live_vk_target_upload(r.set, A_DATA, live_target_pixels(r.reference, A_DATA), bytes) == LIVE_VK_OK);
    live_present_frame frame;
    CHECK(live_vk_target_media_vblank(r.set, 11u, false, 999999999u, &frame));
    CHECK(!frame.front_new && frame.layer_count == 1u && frame.layers[0] == LIVE_LAYER_OVERLAY &&
          live_vk_target_stats_get(r.set).media_frames_released == 0u);
    CHECK(s.width == 1u && s.height == 1u && s.last[0] == movie_pixel[0] && s.last[1] == movie_pixel[1] &&
          s.last[2] == movie_pixel[2]);
    CHECK(live_vk_target_media_vblank(r.set, 12u, true, 1000000000u, &frame));
    CHECK(frame.front_new && frame.front_number == 70u && frame.front_data == A_DATA && frame.layer_count == 2u &&
          frame.layers[0] == LIVE_LAYER_FRONT && frame.layers[1] == LIVE_LAYER_OVERLAY);
    CHECK(s.width == 16u && s.height == 8u);
    CHECK(s.last[0] == original[2] && s.last[1] == original[1] && s.last[2] == original[0]); /* front remains in the bars */
    CHECK(s.last[(4u * 4u)] == movie_pixel[0] && s.last[(4u * 4u) + 1u] == movie_pixel[1] &&
          s.last[(4u * 4u) + 2u] == movie_pixel[2]); /* overlay is over the due front */
    const uint8_t *composed = NULL;
    uint32_t width = 0u, height = 0u;
    CHECK(live_vk_target_compose_last(r.set, &composed, &width, &height));
    CHECK(width == 16u && height == 8u && composed[0] == original[2] && composed[1] == original[1] &&
          composed[2] == original[0] && composed[4u * 4u] == movie_pixel[0]);
    CHECK(live_vk_target_stats_get(r.set).media_frames_released == 1u);
    for (uint64_t i = 0u; i < LIVE_PRESENT_QUEUE + 1u; i++) {
        swap = swap_record(80u + i, 40u, A_DATA, 1u);
        CHECK(live_vk_target_present_submit_at(r.set, &swap, 2000000000u + i) == LIVE_TARGET_OK);
    }
    CHECK(live_vk_target_schedule(r.set).count == LIVE_PRESENT_QUEUE &&
          live_vk_target_stats_get(r.set).media_snapshot_drops == 1u);
    CHECK(live_vk_target_media_vblank(r.set, 42u, true, UINT64_MAX, &frame));
    CHECK(frame.front_new && frame.front_number == 80u + LIVE_PRESENT_QUEUE &&
          live_vk_target_stats_get(r.set).media_frames_released == 2u);
    free(s.last);
    free_rig(&r);
}

static void test_compose_pure(void)
{
    /* letterbox geometry: a 4x2 picture in an 8x8 canvas is 8x4 wide, centred at row 2 */
    uint8_t picture[4 * 2 * 3];
    memset(picture, 0x80, sizeof picture);
    uint8_t out[8 * 8 * 4];
    live_vk_target_compose(NULL, 0u, picture, 4u, 2u, 8u, 8u, out);
    CHECK(out[(1 * 8 + 0) * 4] == 0u && out[(2 * 8 + 0) * 4] == 0x80u && out[(5 * 8 + 7) * 4] == 0x80u && out[(6 * 8 + 0) * 4] == 0u);
    CHECK(out[3] == 0xFFu);
    /* a taller picture is pillarboxed: 2x4 in 8x8 is 4x8, columns 2..5 */
    uint8_t tall[2 * 4 * 3];
    memset(tall, 0x40, sizeof tall);
    live_vk_target_compose(NULL, 0u, tall, 2u, 4u, 8u, 8u, out);
    CHECK(out[(0 * 8 + 1) * 4] == 0u && out[(0 * 8 + 2) * 4] == 0x40u && out[(7 * 8 + 5) * 4] == 0x40u && out[(0 * 8 + 6) * 4] == 0u);
    /* no layers: black opaque */
    live_vk_target_compose(NULL, 0u, NULL, 0u, 0u, 2u, 2u, out);
    CHECK(out[0] == 0u && out[3] == 0xFFu && out[15] == 0xFFu);
    /* front alone: BGRA to RGBA, alpha forced */
    const uint8_t bgra[8] = {10, 20, 30, 0, 40, 50, 60, 1};
    live_vk_target_compose(bgra, 8u, NULL, 0u, 0u, 2u, 1u, out);
    CHECK(out[0] == 30u && out[1] == 20u && out[2] == 10u && out[3] == 0xFFu && out[4] == 60u && out[6] == 40u && out[7] == 0xFFu);
}

int main(void)
{
    test_compose_pure();
    test_device_from_native();
    vulkan vk;
    memset(&vk, 0, sizeof vk);
    if (!make_vulkan(&vk)) {
        const char *required = getenv("TSFP_TEST_VK_REQUIRED");
        printf("%s: no Vulkan loader or graphics device (pure compositor checks %d, failures %d)\n",
               required != NULL ? "FAIL" : "SKIP", checks, failures);
        return failures != 0 ? 1 : (required != NULL ? 1 : 77);
    }
    printf("live_vk_target device: %s\n", vk.name);
    test_blit_shapes(&vk);
    test_refusals(&vk);
    test_copy_events(&vk);
    test_registration(&vk);
    test_present(&vk);
    test_media_present_snapshots_front(&vk);
    test_overlay_only(&vk);
    printf("live_vk_target: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
