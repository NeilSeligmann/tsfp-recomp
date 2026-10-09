/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T791, the texture bridge (src/gpu/live_vk_bind.c) end to end on a real (software) device: a combiner draw that samples a guest
 * texture draws the SAME PIXELS as gpu_pgraph_replay given the same texture (decoded by T792's cache, uploaded as a VkImage,
 * sampled with the decoded sampler state), a guest rewrite (live_texture_note_write) re-uploads and changes the pixels, and a
 * stage that cannot be sampled is refused BY NAME with no stand-in. Skips with 77 without a Vulkan device.
 */
#define VK_NO_PROTOTYPES

#include "gpu_png.h"
#include "live_texture.h"
#include "live_texture_watch.h"
#include "live_vk_bind.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;
static const char *png_directory;

#define CHECK(condition)                                                                      \
    do {                                                                                      \
        checks++;                                                                             \
        if (!(condition)) {                                                                   \
            failures++;                                                                       \
            printf("  FAIL line %d: %s\n", __LINE__, #condition);                             \
        }                                                                                     \
    } while (0)

#include "live_vk_scenes.h"

#include "live_vk_rig.h"

#include "live_vk_texture_scene.h"

static void write_png(const char *name, const gpu_image *image)
{
    if (png_directory != NULL && image->pixels != NULL) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s.png", png_directory, name);
        (void)gpu_png_write_rgba(path, image->pixels, image->width, image->height, image->stride_bytes);
    }
}

static bool mip_binding_enabled;
static bool reviewed_binding(void *context, size_t draw, uint32_t stage,
                             live_texture_binding *out, const char **refusal)
{
    if (!binding_of(context, draw, stage, out, refusal)) return false;
    if (mip_binding_enabled) out->format = (out->format & ~0xF0000u) | 0x20000u;
    return true;
}

static size_t resolved_reads;
static bool synthetic_backing(void *context, const live_texture_binding *binding, uint32_t bytes,
                              uint32_t *address, uint64_t *identity, const char **refusal)
{
    (void)context; (void)bytes; (void)refusal;
    *address=binding->data+0x100000u;
    *identity=932u;
    return true;
}
static bool synthetic_virtual_read(void *context, uint32_t address, void *out, size_t bytes)
{
    CHECK(address>=0x100000u);
    resolved_reads++;
    return fake_guest_read(context,address-0x100000u,out,bytes);
}

int main(int argc, char **argv)
{
    for (int index = 1; index + 1 < argc; index++) {
        if (strcmp(argv[index], "--png") == 0) {
            png_directory = argv[index + 1];
        }
    }
    if (!gpu_vulkan_available()) {
        printf("SKIP: no Vulkan loader\n");
        return 77;
    }
    gpu_device *device = NULL;
    if (gpu_device_create_selected("software", &device) != GPU_OK) {
        printf("SKIP: no software Vulkan device\n");
        return 77;
    }
    fill_memory();
    static const uint8_t first[16] = {30u, 20u, 10u, 255u, 100u, 200u, 255u, 255u, 3u, 2u, 1u, 255u, 6u, 5u, 4u, 255u};
    static const uint8_t second[16] = {30u, 20u, 10u, 255u, 80u, 60u, 40u, 255u, 3u, 2u, 1u, 255u, 6u, 5u, 4u, 255u};
    write_texels(first);
    rig r;
    CHECK(rig_init(&r, device));
    gpu_pgraph *model = textured_model(true, CLAMP_BOTH, BILINEAR);
    char error[256] = "";
    prepare_probe_names(model);
    live_texture_cache cache;
    live_texture_cache_init(&cache, true);
    gpu_window_native native = {0};
    native.instance = r.native.instance;
    native.physical_device = r.native.physical_device;
    native.device = r.native.device;
    native.queue = r.native.queue;
    native.queue_family = r.native.queue_family;
    native.command_pool = r.native.command_pool;
    native.get_device_proc_addr = r.native.get_device_proc_addr;
    native.memory_properties = r.native.memory_properties;
    const char *texture_error = NULL;
    live_vk_texture_set *texture_set = live_vk_texture_create(&native, r.native.get_instance_proc_addr, &texture_error);
    CHECK(texture_set != NULL);
    if (texture_set == NULL) {
        printf("  %s\n", texture_error != NULL ? texture_error : "live_vk_texture_create failed");
        return 1;
    }
    fake_guest guest = {MEMORY_BASE, memory, sizeof memory};
    const live_vk_bind_source source = {reviewed_binding, NULL, fake_guest_read, &guest};
    live_vk_bind *bind = live_vk_bind_create(&cache, texture_set, &source);
    CHECK(bind != NULL);
    live_vk_bind_set_resolver(bind,synthetic_backing,NULL,synthetic_virtual_read,&guest);
    printf("textured draw parity (resolved virtual backing)\n");
    const float clear[4] = {0.2f, 0.4f, 0.6f, 1.0f};
    gpu_image kept[4] = {{0}};
    for (int case_index = 0; case_index < 4; case_index++) {
        /* 0 swizzled, 1 swizzled after a guest rewrite, 2 linear (texel coordinates, rewritten module), 3 linear rewritten */
        linear_texture = case_index >= 2;
        const bool rewritten = case_index == 1 || case_index == 3;
        const uint64_t uploads_before = live_vk_texture_get_stats(texture_set).uploads;
        write_texels(rewritten ? second : first);
        /* Direct backing rewrite: resolver path must detect changes without a LockRect note. */
        live_vk_bind_begin_frame(bind);
        gpu_pgraph_backend replay_backend = probe_backend(&guest);
        live_vk_bind_backend(bind, &replay_backend);
        gpu_image expected = {0};
        gpu_pgraph_report report;
        memset(&report, 0, sizeof report);
        CHECK(gpu_pgraph_replay(model, device, &replay_backend, WIDTH, HEIGHT, clear, &expected, &report) == GPU_PGRAPH_OK);
        if (report.error[0] != '\0') {
            printf("  replay: %s\n", report.error);
        }
        gpu_pgraph_backend live_backend = probe_backend(&guest);
        live_vk_bind_backend(bind, &live_backend);
        live_vk_renderer *renderer = live_vk_renderer_create(&r.device, r.colour_pass, &live_backend, error, sizeof error);
        CHECK(renderer != NULL);
        if (renderer == NULL) {
            continue;
        }
        const live_vk_texture_hook hook = live_vk_bind_hook(bind);
        live_vk_renderer_set_texture_hook(renderer, &hook);
        live_vk_bind_begin_frame(bind);
        frame_result live;
        render_live(&r, renderer, LIVE_VK_PASS_WINDOW, model, clear, &live);
        live_vk_bind_end_frame(bind);
        if (live.refused != 0u) {
            printf("  refused: %s\n", live.first_refusal);
        }
        CHECK(live.drawn == 1u && live.refused == 0u);
        CHECK(live_vk_texture_get_stats(texture_set).uploads == uploads_before + 1u); /* new content, one upload */
        CHECK(expected.pixels != NULL && live.image.pixels != NULL);
        if (expected.pixels != NULL && live.image.pixels != NULL) {
            const size_t different = count_differing(&live.image, &expected);
            printf("  case %d (%s%s): covered %zu differing vs replay %zu\n", case_index, linear_texture ? "linear" : "swizzled",
                   rewritten ? ", rewritten" : "", count_covered(&live.image, clear), different);
            CHECK(different == 0u);
            CHECK(count_covered(&live.image, clear) > 0u);
        }
        static const char *const names[4] = {"texture-swizzled", "texture-swizzled-rewritten", "texture-linear", "texture-linear-rewritten"};
        write_png(names[case_index], &live.image);
        kept[case_index] = live.image;
        live.image.pixels = NULL;
        gpu_image_free(&expected);
        live_vk_renderer_destroy(renderer);
    }
    /* the texture content reaches the pixels: a rewritten texel changes the frame, in both layouts */
    CHECK(kept[0].pixels != NULL && kept[1].pixels != NULL && count_differing(&kept[0], &kept[1]) > 0u);
    CHECK(kept[2].pixels != NULL && kept[3].pixels != NULL && count_differing(&kept[2], &kept[3]) > 0u);
    /* the same texel through a texel coordinate (linear, rewritten module) and a clamped normalised one (swizzled) differ in
     * the module's coordinate (1.5, 0.5): the texel rewrite is what makes the linear frame read texel (1, 0) */
    CHECK(kept[0].pixels != NULL && kept[2].pixels != NULL && count_differing(&kept[0], &kept[2]) > 0u);
    for (int index = 0; index < 4; index++) {
        gpu_image_free(&kept[index]);
    }
    linear_texture = false;
    write_texels(first);
    live_texture_watch_note(TEXTURE_DATA, 0x100u);
    live_vk_bind_begin_frame(bind);
    /* a second frame on the same texture uploads nothing */
    {
        gpu_pgraph_backend live_backend = probe_backend(&guest);
        live_vk_bind_backend(bind, &live_backend);
        live_vk_renderer *renderer = live_vk_renderer_create(&r.device, r.colour_pass, &live_backend, error, sizeof error);
        const live_vk_texture_hook hook = live_vk_bind_hook(bind);
        live_vk_renderer_set_texture_hook(renderer, &hook);
        frame_result again;
        render_live(&r, renderer, LIVE_VK_PASS_WINDOW, model, clear, &again);
        const uint64_t before = live_vk_texture_get_stats(texture_set).uploads;
        gpu_image_free(&again.image);
        render_live(&r, renderer, LIVE_VK_PASS_WINDOW, model, clear, &again);
        CHECK(again.drawn == 1u && live_vk_texture_get_stats(texture_set).uploads == before);
        gpu_image_free(&again.image);
        live_vk_renderer_destroy(renderer);
    }
    CHECK(resolved_reads>0u);
    printf("render target texture\n");
    {
        /* a 16 x 8 linear texture: the same 512 bytes as guest memory and as the render target the T793 set holds */
        wide_texture = true;
        uint8_t wide[64u * 8u];
        for (size_t i = 0u; i < sizeof wide; i++) {
            wide[i] = (uint8_t)(i * 29u + (i / 7u) * 3u + 11u);
        }
        memset(memory + TEXTURE_OFFSET, 0, sizeof memory - TEXTURE_OFFSET);
        memcpy(memory + TEXTURE_OFFSET, wide, sizeof wide);
        live_vk_target_device description = {0};
        description.device = r.native.device;
        description.queue = r.native.queue;
        description.queue_family = r.native.queue_family;
        description.command_pool = r.native.command_pool;
        description.memory = r.native.memory_properties;
        description.get_device_proc_addr = r.native.get_device_proc_addr;
        live_vk_target_set *targets = live_vk_target_create(&description, 0u, &cache);
        CHECK(targets != NULL);
        if (targets != NULL) {
            CHECK(live_vk_target_register(targets, WIDE_TARGET_DATA, LINEAR_A8R8G8B8, WIDE_SIZE_WORD, NULL) == LIVE_VK_OK);
            CHECK(live_vk_target_upload(targets, WIDE_TARGET_DATA, wide, sizeof wide) == LIVE_VK_OK);
            live_vk_bind_use_targets(bind, targets);
            gpu_image frames[2] = {{0}};
            for (int which = 0; which < 2; which++) {
                target_binding = which == 1;
                live_texture_watch_note(TEXTURE_DATA, 0x200u);
                live_vk_bind_begin_frame(bind);
                gpu_pgraph_backend live_backend = probe_backend(&guest);
                live_vk_bind_backend(bind, &live_backend);
                live_vk_renderer *renderer = live_vk_renderer_create(&r.device, r.colour_pass, &live_backend, error, sizeof error);
                const live_vk_texture_hook hook = live_vk_bind_hook(bind);
                live_vk_renderer_set_texture_hook(renderer, &hook);
                frame_result frame;
                render_live(&r, renderer, LIVE_VK_PASS_WINDOW, model, clear, &frame);
                if (frame.refused != 0u) {
                    printf("  refused: %s\n", frame.first_refusal);
                }
                CHECK(frame.drawn == 1u && frame.refused == 0u);
                frames[which] = frame.image;
                live_vk_renderer_destroy(renderer);
            }
            const uint64_t sampled_in_place = live_vk_texture_get_stats(texture_set).target_binds;
            CHECK(sampled_in_place == 1u); /* only the second frame sampled a target, nothing was copied or uploaded for it */
            CHECK(frames[0].pixels != NULL && frames[1].pixels != NULL);
            if (frames[0].pixels != NULL && frames[1].pixels != NULL) {
                printf("  guest vs target texture: covered %zu differing %zu\n", count_covered(&frames[0], clear),
                       count_differing(&frames[0], &frames[1]));
                CHECK(count_covered(&frames[1], clear) > 0u);
                CHECK(count_differing(&frames[0], &frames[1]) == 0u);
            }
            /* the guest copy against the replay: the wide linear texture is a parity case of its own */
            target_binding = false;
            live_texture_watch_note(TEXTURE_DATA, 0x200u);
            live_vk_bind_begin_frame(bind);
            gpu_pgraph_backend replay_backend = probe_backend(&guest);
            live_vk_bind_backend(bind, &replay_backend);
            gpu_image expected = {0};
            gpu_pgraph_report report;
            memset(&report, 0, sizeof report);
            CHECK(gpu_pgraph_replay(model, device, &replay_backend, WIDTH, HEIGHT, clear, &expected, &report) == GPU_PGRAPH_OK);
            CHECK(expected.pixels != NULL && frames[0].pixels != NULL && count_differing(&expected, &frames[0]) == 0u);
            write_png("texture-target", &frames[1]);
            gpu_image_free(&expected);
            gpu_image_free(&frames[0]);
            gpu_image_free(&frames[1]);
            /* a target without a view provider is refused by name, never stood in for */
            live_vk_bind_use_targets(bind, NULL);
            target_binding = true;
            live_vk_bind_begin_frame(bind);
            gpu_pgraph_backend live_backend = probe_backend(&guest);
            live_vk_bind_backend(bind, &live_backend);
            live_vk_renderer *renderer = live_vk_renderer_create(&r.device, r.colour_pass, &live_backend, error, sizeof error);
            const live_vk_texture_hook hook = live_vk_bind_hook(bind);
            live_vk_renderer_set_texture_hook(renderer, &hook);
            frame_result none;
            render_live(&r, renderer, LIVE_VK_PASS_WINDOW, model, clear, &none);
            CHECK(none.drawn == 0u && none.refused == 1u && strstr(none.first_refusal, "no image view") != NULL);
            CHECK(count_covered(&none.image, clear) == 0u);
            gpu_image_free(&none.image);
            live_vk_renderer_destroy(renderer);
            live_vk_target_destroy(targets);
        }
        wide_texture = false;
        target_binding = false;
    }
    printf("refusals\n");
    {
        /* no texture hook: the sampled stage has no image and the draw is refused, not stood in for */
        gpu_pgraph_backend live_backend = probe_backend(&guest);
        live_vk_bind_backend(bind, &live_backend);
        live_vk_renderer *renderer = live_vk_renderer_create(&r.device, r.colour_pass, &live_backend, error, sizeof error);
        frame_result none;
        render_live(&r, renderer, LIVE_VK_PASS_WINDOW, model, clear, &none);
        CHECK(none.drawn == 0u && none.refused == 1u && strstr(none.first_refusal, "no texture hook") != NULL);
        CHECK(count_covered(&none.image, clear) == 0u);
        gpu_image_free(&none.image);
        live_vk_renderer_destroy(renderer);
        /* the sampler words never written by the stream: refused at planning with the named reason */
        gpu_pgraph *unsampled = textured_model(false, 0u, 0u);
        renderer = live_vk_renderer_create(&r.device, r.colour_pass, &live_backend, error, sizeof error);
        const live_vk_texture_hook hook = live_vk_bind_hook(bind);
        live_vk_renderer_set_texture_hook(renderer, &hook);
        render_live(&r, renderer, LIVE_VK_PASS_WINDOW, unsampled, clear, &none);
        CHECK(none.drawn == 0u && none.refused == 1u);
        CHECK(strstr(none.first_refusal, "was never written") != NULL || strstr(none.first_refusal, "never written") != NULL);
        CHECK(count_covered(&none.image, clear) == 0u);
        gpu_image_free(&none.image);
        live_vk_renderer_destroy(renderer);
        gpu_pgraph_destroy(unsampled);
        /* A real multi-level header requires the draw's Control0 snapshot;
         * zero-initialised host state cannot invent the active LOD range. */
        mip_binding_enabled = true;
        renderer = live_vk_renderer_create(&r.device, r.colour_pass, &live_backend, error, sizeof error);
        live_vk_renderer_set_texture_hook(renderer, &hook);
        render_live(&r, renderer, LIVE_VK_PASS_WINDOW, model, clear, &none);
        CHECK(none.drawn == 0u && none.refused == 1u && strstr(none.first_refusal, "mip Control0 was never written") != NULL);
        gpu_image_free(&none.image);
        live_vk_renderer_destroy(renderer);
        mip_binding_enabled = false;
        /* U and V wrap (0x101, measured) against the replay: the repeat sampler matches the replay's. */
        gpu_pgraph *wrapped = textured_model(true, WRAP_BOTH, BILINEAR);
        gpu_pgraph_backend wrap_backend = probe_backend(&guest);
        live_vk_bind_backend(bind, &wrap_backend);
        gpu_image wrap_expected = {0};
        gpu_pgraph_report wrap_report;
        memset(&wrap_report, 0, sizeof wrap_report);
        CHECK(gpu_pgraph_replay(wrapped, device, &wrap_backend, WIDTH, HEIGHT, clear, &wrap_expected, &wrap_report) == GPU_PGRAPH_OK);
        renderer = live_vk_renderer_create(&r.device, r.colour_pass, &wrap_backend, error, sizeof error);
        live_vk_renderer_set_texture_hook(renderer, &hook);
        render_live(&r, renderer, LIVE_VK_PASS_WINDOW, wrapped, clear, &none);
        CHECK(none.drawn == 1u && none.refused == 0u);
        CHECK(wrap_expected.pixels != NULL && none.image.pixels != NULL && count_differing(&wrap_expected, &none.image) == 0u);
        write_png("texture-wrap", &none.image);
        /* T919: the retail LOD0-linear filter reaches the full combiner/texture bridge only with
         * explicit inference. This one-level image must match filter6 pixels, not an empty draw. */
        gpu_pgraph *lod0_model = textured_model(true, WRAP_BOTH, 0x02022000u);
        live_vk_renderer *lod0_renderer = live_vk_renderer_create(&r.device, r.colour_pass, &wrap_backend, error, sizeof error);
        live_vk_renderer_set_texture_hook(lod0_renderer, &hook);
        frame_result lod0_frame;
        render_live(&r, lod0_renderer, LIVE_VK_PASS_WINDOW, lod0_model, clear, &lod0_frame);
        CHECK(lod0_frame.drawn == 1u && lod0_frame.refused == 0u);
        CHECK(count_covered(&lod0_frame.image, clear) > 100u);
        CHECK(count_differing(&none.image, &lod0_frame.image) == 0u);
        gpu_image_free(&lod0_frame.image);
        live_vk_renderer_destroy(lod0_renderer);
        gpu_pgraph_backend strict_filter = wrap_backend;
        strict_filter.allowed_inferences &= ~GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING;
        lod0_renderer = live_vk_renderer_create(&r.device, r.colour_pass, &strict_filter, error, sizeof error);
        live_vk_renderer_set_texture_hook(lod0_renderer, &hook);
        render_live(&r, lod0_renderer, LIVE_VK_PASS_WINDOW, lod0_model, clear, &lod0_frame);
        CHECK(lod0_frame.drawn == 0u && lod0_frame.refused == 1u);
        printf("  strict LOD0 filter refused: %s\n", lod0_frame.first_refusal);
        CHECK(strstr(lod0_frame.first_refusal, "INFERRED") != NULL);
        CHECK(count_covered(&lod0_frame.image, clear) == 0u);
        gpu_image_free(&lod0_frame.image);
        live_vk_renderer_destroy(lod0_renderer);
        gpu_pgraph_destroy(lod0_model);
        gpu_image_free(&none.image);
        gpu_image_free(&wrap_expected);
        live_vk_renderer_destroy(renderer);
        gpu_pgraph_destroy(wrapped);
    }
    live_vk_bind_destroy(bind);
    live_vk_texture_destroy(texture_set);
    live_texture_cache_free(&cache);
    gpu_pgraph_destroy(model);
    r.fn.vkDestroyRenderPass(r.native.device, r.colour_pass, NULL);
    gpu_device_destroy(device);
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
