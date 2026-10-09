/* Regression for a later descriptor-pool reset failure after an earlier pool reset. */
#define main t792_probe_reference_main
#include "test_live_vk_texture.c"
#undef main

static PFN_vkGetDeviceProcAddr real_get_device_proc;
static PFN_vkResetDescriptorPool real_reset_pool;
static bool fail_reset_armed;
static unsigned reset_calls;

static VkResult VKAPI_CALL fail_second_pool_reset(VkDevice device, VkDescriptorPool pool,
                                                  VkDescriptorPoolResetFlags flags)
{
    if (fail_reset_armed && ++reset_calls == 2u) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return real_reset_pool(device, pool, flags);
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
    VkImageView views[129] = {image.view};
    for (unsigned index = 1u; index < 129u; index++) {
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

    CHECK(live_vk_texture_begin_batch(set));
    const live_vk_sampler_desc sampler = live_vk_sampler_decode(0x00000303u, NEAREST_FILTER);
    const live_texture_result result = {
        .source = LIVE_TEXTURE_SOURCE_TARGET,
        .width = 4u,
        .height = 4u,
        .target_id = 0x1041u,
    };
    for (unsigned index = 0u; index < 129u; index++) {
        CHECK(live_vk_texture_register_target(set, result.target_id, views[index], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
        live_vk_texture_bound bound;
        CHECK(live_vk_texture_bind(set, NULL, &result, &sampler, &bound, &error));
        CHECK(bound.descriptor != VK_NULL_HANDLE);
    }
    CHECK(live_vk_texture_get_stats(set).descriptor_versions_peak == 129u);

    /* The first reset succeeds; the second pool reset fails. */
    reset_calls = 0u;
    fail_reset_armed = true;
    CHECK(!live_vk_texture_end_batch(set));
    fail_reset_armed = false;
    CHECK(reset_calls == 2u);
    CHECK(live_vk_texture_batch_active(set));
    CHECK(live_vk_texture_get_stats(set).completion_failures == 1u);

    /* A successful earlier reset invalidated its sets. Refuse binds until every pool reset
     * succeeds instead of returning any possibly stale descriptor from the incomplete batch. */
    CHECK(live_vk_texture_register_target(set, result.target_id, views[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    const uint64_t writes_before_lookup = live_vk_texture_get_stats(set).descriptor_writes;
    live_vk_texture_bound rebound = {0};
    CHECK(!live_vk_texture_bind(set, NULL, &result, &sampler, &rebound, &error));
    CHECK(error != NULL && strcmp(error, "descriptor batch completion retry required") == 0);
    CHECK(live_vk_texture_get_stats(set).descriptor_writes == writes_before_lookup);

    /* A retry resets the remaining allocated pool, retires the batch and permits another batch. */
    CHECK(live_vk_texture_end_batch(set));
    CHECK(!live_vk_texture_batch_active(set));
    CHECK(live_vk_texture_get_stats(set).batch_completions == 1u);
    CHECK(live_vk_texture_begin_batch(set));
    CHECK(live_vk_texture_register_target(set, result.target_id, views[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    const uint64_t writes_after_recovery = live_vk_texture_get_stats(set).descriptor_writes;
    live_vk_texture_bound after_recovery = {0};
    CHECK(live_vk_texture_bind(set, NULL, &result, &sampler, &after_recovery, &error));
    CHECK(after_recovery.descriptor != VK_NULL_HANDLE);
    CHECK(live_vk_texture_get_stats(set).descriptor_writes == writes_after_recovery + 1u);
    CHECK(live_vk_texture_end_batch(set));
    CHECK(live_vk_texture_get_stats(set).batch_completions == 2u);
    live_vk_texture_destroy(set);
    for (unsigned index = 1u; index < 129u; index++) {
        p.vkDestroyImageView(native.device, views[index], NULL);
    }
    probe_image_destroy(&p, &image);
    p.vkDestroyPipeline(native.device, p.pipeline, NULL);
    p.vkDestroyPipelineLayout(native.device, p.layout, NULL);
    p.vkDestroyRenderPass(native.device, p.render_pass, NULL);
    gpu_window_destroy(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    printf("T1041 partial pool reset recovery: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
