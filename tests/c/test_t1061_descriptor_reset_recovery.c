/* Independent three-page failure/retry and batch-identity review control for T1060. */
#define main t792_probe_reference_main
#include "test_live_vk_texture.c"
#undef main

static PFN_vkGetDeviceProcAddr real_get_device_proc;
static PFN_vkResetDescriptorPool real_reset_pool;
static PFN_vkQueueWaitIdle real_wait_idle;
static bool fail_reset_armed;
static bool fail_idle_armed;
static unsigned reset_calls;
static unsigned fail_reset_call;

static VkResult VKAPI_CALL fail_second_pool_reset(VkDevice device, VkDescriptorPool pool,
                                                  VkDescriptorPoolResetFlags flags)
{
    if (fail_reset_armed && ++reset_calls == fail_reset_call) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return real_reset_pool(device, pool, flags);
}

static VkResult VKAPI_CALL fail_queue_wait_idle(VkQueue queue)
{
    if (fail_idle_armed) return VK_ERROR_DEVICE_LOST;
    return real_wait_idle(queue);
}

static PFN_vkVoidFunction reset_intercept_proc(VkDevice device, const char *name)
{
    PFN_vkVoidFunction function = real_get_device_proc(device, name);
    if (strcmp(name, "vkResetDescriptorPool") == 0) {
        memcpy(&real_reset_pool, &function, sizeof real_reset_pool);
        union {
            PFN_vkVoidFunction generic;
            PFN_vkResetDescriptorPool typed;
        } replacement = {.typed = fail_second_pool_reset};
        return replacement.generic;
    }
    if (strcmp(name, "vkQueueWaitIdle") == 0) {
        memcpy(&real_wait_idle, &function, sizeof real_wait_idle);
        union {
            PFN_vkVoidFunction generic;
            PFN_vkQueueWaitIdle typed;
        } replacement = {.typed = fail_queue_wait_idle};
        return replacement.generic;
    }
    return function;
}

static VkComponentSwizzle swizzle(unsigned *value)
{
    const VkComponentSwizzle result = (VkComponentSwizzle)(*value % 7u);
    *value /= 7u;
    return result;
}

int main(void)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) return unavailable(SDL_GetError());
    SDL_Window *window = SDL_CreateWindow("t1041-partial-reset", 32, 32, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (window == NULL) return unavailable(SDL_GetError());
    const char *error = NULL;
    gpu_window *renderer = gpu_window_create(window, &error);
    if (renderer == NULL) return unavailable(error);

    gpu_window_native native;
    CHECK(gpu_window_get_native(renderer, &native));
    const PFN_vkGetDeviceProcAddr get_device_proc = native.get_device_proc_addr;
    real_get_device_proc = get_device_proc;
    gpu_window_native texture_native = native;
    texture_native.get_device_proc_addr = reset_intercept_proc;
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    live_vk_texture_set *set = live_vk_texture_create(&texture_native, gipa, &error);
    CHECK(set != NULL);
    if (set == NULL) {
        gpu_window_destroy(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    probe p;
    CHECK(probe_init(&p, &native, gipa, live_vk_texture_set_layout(set)));
    probe_image image;
    CHECK(probe_image_create(&p, &image, 4u, 4u));
    VkImageView views[257] = {image.view};
    for (unsigned index = 1u; index < 257u; index++) {
        unsigned components = index;
        const VkImageViewCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image.image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .components = {swizzle(&components), swizzle(&components), swizzle(&components), swizzle(&components)},
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u},
        };
        const VkResult created = p.vkCreateImageView(native.device, &info, NULL, &views[index]);
        CHECK(created == VK_SUCCESS);
        for (unsigned prior = 0u; prior < index; prior++) CHECK(views[index] != views[prior]);
    }

    live_texture_cache *cache = calloc(1u, sizeof(*cache));
    CHECK(cache != NULL);
    if (cache == NULL) return 1;
    live_texture_cache_init(cache, true);
    const live_texture_binding guest_binding = {0u, FORMAT(0x12, 0, 0), LINEAR_SIZE(2u, 2u, 64u), 0u, 0u};
    CHECK(live_texture_plan_binding(&guest_binding, true, &cache->entries[0].plan));
    CHECK(cache->entries[0].plan.ok);
    cache->entries[0].in_use = true;
    uint8_t pixels[64];
    for (size_t offset = 0u; offset < sizeof pixels; offset += 4u) {
        pixels[offset] = 0x20u; pixels[offset + 1u] = 0x40u; pixels[offset + 2u] = 0x60u; pixels[offset + 3u] = 0xFFu;
    }
    const live_vk_sampler_desc sampler = live_vk_sampler_decode(0x00000303u, NEAREST_FILTER);
    live_texture_result guest_result = {.source = LIVE_TEXTURE_SOURCE_GUEST, .entry = 0u, .rgba = pixels,
        .width = 2u, .height = 2u, .levels = 1u, .generation = 1u, .needs_upload = true};
    const live_texture_result result = {
        .source = LIVE_TEXTURE_SOURCE_TARGET,
        .width = 4u,
        .height = 4u,
        .target_id = 0x1061u,
    };
    live_vk_texture_bound rebound = {0};
    CHECK(live_vk_texture_begin_batch(set));
    CHECK(live_vk_texture_bind(set, cache, &guest_result, &sampler, &rebound, &error));
    for (uint64_t generation = 2u; generation <= 3u; generation++) {
        pixels[0] = (uint8_t)(generation * 20u);
        guest_result.generation = (uint32_t)generation;
        CHECK(live_vk_texture_bind(set, cache, &guest_result, &sampler, &rebound, &error));
    }
    CHECK(live_vk_texture_get_stats(set).image_destroys == 0u);
    CHECK(live_vk_texture_get_stats(set).image_creates == 3u);
    for (unsigned index = 0u; index < 257u; index++) {
        CHECK(live_vk_texture_register_target(set, result.target_id, views[index], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
        live_vk_texture_bound bound;
        CHECK(live_vk_texture_bind(set, cache, &result, &sampler, &bound, &error));
        CHECK(bound.descriptor != VK_NULL_HANDLE);
    }
    CHECK(live_vk_texture_get_stats(set).descriptor_versions_peak >= 257u);

    /* Two pool resets succeed, then the third fails; no bind may use the damaged batch. */
    reset_calls = 0u;
    fail_reset_call = 3u;
    fail_reset_armed = true;
    CHECK(!live_vk_texture_end_batch(set));
    fail_reset_armed = false;
    CHECK(reset_calls == 3u);
    CHECK(live_vk_texture_batch_active(set));
    CHECK(live_vk_texture_get_stats(set).completion_failures == 1u);
    CHECK(live_vk_texture_get_stats(set).image_destroys == 0u);

    uint64_t writes_before_lookup = live_vk_texture_get_stats(set).descriptor_writes;
    CHECK(live_vk_texture_register_target(set, result.target_id, views[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    CHECK(!live_vk_texture_bind(set, cache, &result, &sampler, &rebound, &error));
    CHECK(error != NULL && strstr(error, "completion retry required") != NULL);
    CHECK(live_vk_texture_get_stats(set).descriptor_writes == writes_before_lookup);
    CHECK(live_vk_texture_register_target(set, result.target_id, views[128], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    CHECK(!live_vk_texture_bind(set, cache, &result, &sampler, &rebound, &error));
    CHECK(error != NULL && strstr(error, "completion retry required") != NULL);
    CHECK(live_vk_texture_get_stats(set).descriptor_writes == writes_before_lookup);
    CHECK(live_vk_texture_get_stats(set).image_destroys == 0u);

    CHECK(live_vk_texture_end_batch(set));
    CHECK(!live_vk_texture_batch_active(set));
    CHECK(live_vk_texture_get_stats(set).batch_completions == 1u);
    CHECK(live_vk_texture_get_stats(set).image_destroys == 2u);
    CHECK(live_vk_texture_begin_batch(set));
    writes_before_lookup = live_vk_texture_get_stats(set).descriptor_writes;
    CHECK(live_vk_texture_register_target(set, result.target_id, views[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    CHECK(live_vk_texture_bind(set, cache, &result, &sampler, &rebound, &error));
    CHECK(live_vk_texture_get_stats(set).descriptor_writes == writes_before_lookup + 1u);
    writes_before_lookup = live_vk_texture_get_stats(set).descriptor_writes;
    CHECK(live_vk_texture_register_target(set, result.target_id, views[128], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    CHECK(live_vk_texture_bind(set, cache, &result, &sampler, &rebound, &error));
    CHECK(live_vk_texture_get_stats(set).descriptor_writes == writes_before_lookup + 1u);
    CHECK(live_vk_texture_end_batch(set));

    /* Queue-idle failure happens before pool reset and also blocks batch binds. */
    CHECK(live_vk_texture_begin_batch(set));
    CHECK(live_vk_texture_register_target(set, result.target_id, views[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    CHECK(live_vk_texture_bind(set, cache, &result, &sampler, &rebound, &error));
    writes_before_lookup = live_vk_texture_get_stats(set).descriptor_writes;
    fail_idle_armed = true;
    CHECK(!live_vk_texture_end_batch(set));
    fail_idle_armed = false;
    CHECK(live_vk_texture_batch_active(set));
    CHECK(!live_vk_texture_bind(set, cache, &result, &sampler, &rebound, &error));
    CHECK(error != NULL && strstr(error, "completion retry required") != NULL);
    CHECK(live_vk_texture_get_stats(set).descriptor_writes == writes_before_lookup);
    CHECK(live_vk_texture_get_stats(set).image_destroys == 2u);
    CHECK(live_vk_texture_end_batch(set));
    CHECK(!live_vk_texture_batch_active(set));

    CHECK(live_vk_texture_begin_batch(set));
    for (unsigned index = 0u; index < 257u; index++) {
        CHECK(live_vk_texture_register_target(set, result.target_id, views[index], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
        CHECK(live_vk_texture_bind(set, cache, &result, &sampler, &rebound, &error));
    }
    reset_calls = 0u;
    fail_reset_call = 1u;
    fail_reset_armed = true;
    CHECK(!live_vk_texture_end_batch(set));
    fail_reset_armed = false;
    CHECK(reset_calls == 1u);
    CHECK(live_vk_texture_batch_active(set));
    CHECK(live_vk_texture_register_target(set, result.target_id, views[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    writes_before_lookup = live_vk_texture_get_stats(set).descriptor_writes;
    CHECK(!live_vk_texture_bind(set, cache, &result, &sampler, &rebound, &error));
    CHECK(error != NULL && strstr(error, "completion retry required") != NULL);
    CHECK(live_vk_texture_get_stats(set).descriptor_writes == writes_before_lookup);
    CHECK(live_vk_texture_end_batch(set));
    CHECK(!live_vk_texture_batch_active(set));

    /* Destroying a no-submit batch after a partial reset must also release the page pools. */
    CHECK(live_vk_texture_begin_batch(set));
    for (unsigned index = 0u; index < 129u; index++) {
        CHECK(live_vk_texture_register_target(set, result.target_id, views[index], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
        CHECK(live_vk_texture_bind(set, cache, &result, &sampler, &rebound, &error));
    }
    reset_calls = 0u;
    fail_reset_call = 2u;
    fail_reset_armed = true;
    CHECK(!live_vk_texture_end_batch(set));
    fail_reset_armed = false;
    CHECK(reset_calls == 2u);
    CHECK(live_vk_texture_batch_active(set));
    live_vk_texture_destroy(set);
    free(cache);
    for (unsigned index = 1u; index < 257u; index++) {
        p.vkDestroyImageView(native.device, views[index], NULL);
    }
    probe_image_destroy(&p, &image);
    p.vkDestroyPipeline(native.device, p.pipeline, NULL);
    p.vkDestroyPipelineLayout(native.device, p.layout, NULL);
    p.vkDestroyRenderPass(native.device, p.render_pass, NULL);
    gpu_window_destroy(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    printf("T1061 descriptor reset recovery: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
