/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T829, live draws INTO T793 target images (src/gpu/live_vk_draw.c, the BGRA8 + D32S8 pass kind of live_vk_pipeline.c) on a real
 * (software) device: every scene of the T791 harness drawn into a registered BGRA8 target through the per draw provider equals
 * gpu_pgraph_replay pixel for pixel; a draw whose target a later draw samples in place (T792, no copy) equals the replay of the
 * sampling draw over the replayed image given as the texture (the T510 render target texture semantics: the replayed image is the
 * surface the hardware samples, texel coordinates, bilinear and clamped); two targets drawn in one frame, in alternation; the
 * per frame depth reset; and every unusable surface state refused BY NAME with nothing drawn. Skips with 77 without a device.
 * `--png DIR` writes the live and replay images to look at.
 */
#define VK_NO_PROTOTYPES

#include "gpu_phase_timing.h"
#include "gpu_png.h"
#include "live_texture.h"
#include "live_texture_watch.h"
#include "live_vk_bind.h"
#include "live_vk_draw.h"
#include "live_vk_query.h"

#include <stdatomic.h>
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

#define BIG_FORMAT 0x00011229u /* linear A8R8G8B8 */
#define BIG_SIZE (63u | (63u << 12) | (3u << 24)) /* 64 x 64, pitch 256 */
#define BIG_A 0x0200000u
#define BIG_B 0x0300000u
#define BIG_C 0x0400000u
#define SWZ_A 0x0500000u /* T1489: swizzled A8R8G8B8 64 x 64 */
#define SWZ_FORMAT 0x06610629u
#define SWZ_SURFACE 0x06060228u
#define SMALL_DATA WIDE_TARGET_DATA /* 16 x 8, pitch 64 */
#define SMALL_W 16u
#define SMALL_H 8u
#define SURFACE_ALL (ALL_GROUPS | GPU_PGRAPH_OUTPUT_POLYGON_OFFSET | GPU_PGRAPH_OUTPUT_SURFACE)

static void write_png(const char *name, const gpu_image *image)
{
    if (png_directory != NULL && image->pixels != NULL) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s.png", png_directory, name);
        /* alpha forced opaque on a copy: a pixel whose alpha is 0 would show as white in a viewer */
        uint8_t *opaque = malloc((size_t)image->height * image->stride_bytes);
        if (opaque != NULL) {
            memcpy(opaque, image->pixels, (size_t)image->height * image->stride_bytes);
            for (size_t offset = 3u; offset < (size_t)image->height * image->stride_bytes; offset += 4u) {
                opaque[offset] = 0xFFu;
            }
            (void)gpu_png_write_rgba(path, opaque, image->width, image->height, image->stride_bytes);
            free(opaque);
        }
    }
}

static void surface_words(stream_builder *stream, uint32_t data, uint32_t pitch)
{
    stream_pair(stream, 0x0208u, 0x128u); /* A8R8G8B8, Z24S8, pitch layout: the one measured combination */
    stream_pair(stream, 0x020Cu, pitch);
    stream_pair(stream, 0x0210u, data);
}

/* T1489: the 0x0208 word the title writes for a swizzled target (measured 0x07070228 for 128 x 128): colour 8, zeta 2, type 2, log2 w, log2 h. */
static void swizzled_surface_words(stream_builder *stream, uint32_t data, uint32_t format)
{
    stream_pair(stream, 0x0208u, format);
    stream_pair(stream, 0x020Cu, 0u);
    stream_pair(stream, 0x0210u, data);
}

static gpu_pgraph *scene_model_in(scene_kind kind, uint32_t data, uint32_t pitch, uint32_t swizzled_format)
{
    gpu_pgraph *model = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(model, SURFACE_ALL);
    gpu_pgraph_set_combiner(model, scene_uses_combiner(kind));
    stream_builder stream = {0};
    if (swizzled_format != 0u) {
        swizzled_surface_words(&stream, data, swizzled_format);
    } else {
        surface_words(&stream, data, pitch);
    }
    build_stream(&stream, kind);
    CHECK(gpu_pgraph_decode(model, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    stream_free(&stream);
    return model;
}

static gpu_pgraph *scene_model(scene_kind kind, uint32_t data, uint32_t pitch)
{
    return scene_model_in(kind, data, pitch, 0u);
}

typedef struct {
    rig *r;
    gpu_device *device;
    live_texture_cache cache;
    live_vk_target_set *targets;
    fake_guest guest;
} harness;

typedef struct {
    uint32_t drawn, refused, clears_applied;
    char first_refusal[LIVE_PIPELINE_REASON_BYTES];
    live_vk_draw_stats stats;
    uint64_t waits;                                 /* T1264: the deferred vkQueueWaitIdle calls (GPU_PHASE_DRAW_SYNC) of the frame */
    uint64_t target_pipelines, offscreen_pipelines; /* misses of the two pass kinds' caches: which cache the draws used */
} live_result;

/* A fresh renderer and provider, one frame of every draw of `model`, flushed. `bind` (may be NULL) is the texture bridge. */
static void run_frame(harness *h, gpu_pgraph_backend backend, live_vk_bind *bind, const gpu_pgraph *model, int frames,
                      live_result *out)
{
    memset(out, 0, sizeof *out);
    const uint64_t waits_before = atomic_load(&gpu_phase_global.calls[GPU_PHASE_DRAW_SYNC]);
    char error[256] = "";
    if (bind != NULL) {
        live_vk_bind_backend(bind, &backend);
    }
    live_vk_renderer *renderer = live_vk_renderer_create(&h->r->device, h->r->colour_pass, &backend, error, sizeof error);
    CHECK(renderer != NULL);
    if (renderer == NULL) {
        return;
    }
    live_vk_draw *draw = live_vk_draw_create(&h->r->device, renderer, h->targets, error, sizeof error);
    CHECK(draw != NULL);
    if (draw != NULL) {
        const live_vk_target_hook target_hook = live_vk_draw_hook(draw);
        live_vk_renderer_set_target_hook(renderer, &target_hook);
        if (bind != NULL) {
            const live_vk_texture_hook texture_hook = live_vk_bind_hook(bind);
            live_vk_renderer_set_texture_hook(renderer, &texture_hook);
        }
        for (int frame = 0; frame < frames; frame++) {
            (void)live_vk_draw_sync(draw); /* the frame hook's order: the last frame's runs are done before the arena resets */
            live_vk_renderer_begin_frame(renderer);
            live_vk_draw_begin_frame(draw);
            if (bind != NULL) {
                live_vk_bind_begin_frame(bind);
            }
            const size_t clears = gpu_pgraph_clear_count(model);
            size_t next_clear = 0u;
            for (size_t index = 0u; index < gpu_pgraph_draw_count(model); index++) {
                /* the frame hook's order: the clear events before a draw run before it */
                while (next_clear < clears && gpu_pgraph_clear_at(model, next_clear)->before_draw <= index) {
                    char clear_reason[LIVE_PIPELINE_REASON_BYTES] = "";
                    out->clears_applied += live_vk_draw_apply_clear(draw, model, next_clear++, clear_reason, sizeof clear_reason);
                }
                char reason[LIVE_PIPELINE_REASON_BYTES] = "";
                if (live_vk_renderer_draw(renderer, model, index, VK_NULL_HANDLE, 0u, 0u, reason, sizeof reason)) {
                    out->drawn++;
                } else if (out->refused++ == 0u) {
                    snprintf(out->first_refusal, sizeof out->first_refusal, "%s", reason);
                }
            }
            while (next_clear < clears) {
                char clear_reason[LIVE_PIPELINE_REASON_BYTES] = "";
                out->clears_applied += live_vk_draw_apply_clear(draw, model, next_clear++, clear_reason, sizeof clear_reason);
            }
            CHECK(live_vk_draw_flush(draw));
            if (bind != NULL) {
                live_vk_bind_end_frame(bind);
            }
        }
        out->stats = live_vk_draw_stats_get(draw);
        out->target_pipelines = live_vk_renderer_cache(renderer, LIVE_VK_PASS_TARGET)->misses;
        out->offscreen_pipelines = live_vk_renderer_cache(renderer, LIVE_VK_PASS_OFFSCREEN)->misses;
        live_vk_draw_destroy(draw);
        out->waits = atomic_load(&gpu_phase_global.calls[GPU_PHASE_DRAW_SYNC]) - waits_before;
    }
    live_vk_renderer_destroy(renderer);
}

static gpu_pgraph_backend scene_backend(harness *h, scene_kind kind)
{
    gpu_pgraph_backend backend = make_backend_for(kind, &h->guest, EVERYTHING);
    backend.output_groups |= GPU_PGRAPH_OUTPUT_SURFACE;
    return backend;
}

/* The target's bytes as a tight RGBA image. */
static gpu_image target_image(harness *h, uint32_t data, uint32_t width, uint32_t height)
{
    gpu_image image = {0};
    uint8_t *bytes = malloc((size_t)width * height * 4u);
    image.pixels = malloc((size_t)width * height * 4u);
    image.width = width;
    image.height = height;
    image.stride_bytes = width * 4u;
    CHECK(bytes != NULL && image.pixels != NULL);
    if (bytes != NULL && image.pixels != NULL) {
        CHECK(live_vk_target_readback(h->targets, data, bytes, (size_t)width * height * 4u) == LIVE_VK_OK);
        for (size_t pixel = 0u; pixel < (size_t)width * height; pixel++) {
            image.pixels[pixel * 4u] = bytes[pixel * 4u + 2u];
            image.pixels[pixel * 4u + 1u] = bytes[pixel * 4u + 1u];
            image.pixels[pixel * 4u + 2u] = bytes[pixel * 4u];
            image.pixels[pixel * 4u + 3u] = bytes[pixel * 4u + 3u];
        }
    }
    free(bytes);
    return image;
}

static size_t differing(const gpu_image *a, const gpu_image *b)
{
    if (a->pixels == NULL || b->pixels == NULL || a->width != b->width || a->height != b->height) {
        return (size_t)-1;
    }
    size_t different = 0u;
    for (size_t pixel = 0u; pixel < (size_t)a->width * a->height; pixel++) {
        different += memcmp(a->pixels + pixel * 4u, b->pixels + pixel * 4u, 4u) != 0;
    }
    return different;
}

static size_t non_zero(const gpu_image *image)
{
    size_t count = 0u;
    for (size_t pixel = 0u; image->pixels != NULL && pixel < (size_t)image->width * image->height; pixel++) {
        count += (image->pixels[pixel * 4u] | image->pixels[pixel * 4u + 1u] | image->pixels[pixel * 4u + 2u] | image->pixels[pixel * 4u + 3u]) != 0u;
    }
    return count;
}

static void clear_target(harness *h, uint32_t data, size_t bytes)
{
    uint8_t *zero = calloc(1u, bytes);
    CHECK(zero != NULL && live_vk_target_upload(h->targets, data, zero, bytes) == LIVE_VK_OK);
    free(zero);
}

static gpu_image replay_image(harness *h, const gpu_pgraph *model, gpu_pgraph_backend backend, uint32_t width, uint32_t height)
{
    static const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f}; /* the target starts zeroed */
    gpu_image expected = {0};
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    CHECK(gpu_pgraph_replay(model, h->device, &backend, width, height, clear, &expected, &report) == GPU_PGRAPH_OK);
    if (report.error[0] != '\0') {
        printf("  replay: %s\n", report.error);
    }
    return expected;
}

static void test_parity(harness *h)
{
    CHECK(live_vk_pass_colour_format(LIVE_VK_PASS_TARGET) == VK_FORMAT_B8G8R8A8_UNORM); /* the guest A8R8G8B8 byte order */
    CHECK(live_vk_pass_colour_format(LIVE_VK_PASS_OFFSCREEN) == VK_FORMAT_R8G8B8A8_UNORM);
    printf("scenes drawn into a BGRA8 target against gpu_pgraph_replay\n");
    for (int index = 0; index < SCENE_COUNT; index++) {
        const scene_kind kind = (scene_kind)index;
        gpu_pgraph *model = scene_model(kind, BIG_A, 256u);
        clear_target(h, BIG_A, 256u * 64u);
        /* the live frame first: see test_live_vk_pipeline.c, undefined dynamic state must not inherit the replay's */
        live_result live;
        VkFormat format;
        VkExtent2D extent;
        uint64_t generation_before = 0u, generation_after = 0u;
        (void)live_vk_target_image(h->targets, BIG_A, &format, &extent, &generation_before);
        run_frame(h, scene_backend(h, kind), NULL, model, 1, &live);
        (void)live_vk_target_image(h->targets, BIG_A, &format, &extent, &generation_after);
        CHECK(generation_after > generation_before); /* the draw bumped the target generation (the present cache and T792 key on it) */
        gpu_image drawn = target_image(h, BIG_A, 64u, 64u);
        if (kind == SCENE_POLYGON_ZERO) {
            /* T885 retired the zero-as-fill inference. Both paths must retain the refusal. */
            const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            gpu_image refused_image = {0};
            gpu_pgraph_report refused_report = {0};
            gpu_pgraph_backend backend = scene_backend(h, kind);
            CHECK(gpu_pgraph_replay(model, h->device, &backend, 64u, 64u, clear,
                                   &refused_image, &refused_report) == GPU_PGRAPH_ERR_UNMEASURED);
            CHECK(strstr(refused_report.error, "front polygon mode") != NULL);
            CHECK(live.refused == gpu_pgraph_draw_count(model) && live.drawn == 0u);
            CHECK(live.target_pipelines == 0u && non_zero(&drawn) == 0u);
            gpu_image_free(&refused_image);
            gpu_image_free(&drawn);
            gpu_pgraph_destroy(model);
            continue;
        }
        gpu_image expected = replay_image(h, model, scene_backend(h, kind), 64u, 64u);
        const size_t different = differing(&drawn, &expected);
        printf("  %-18s drawn %u refused %u covered %zu differing %zu runs %llu\n", scene_names[kind], live.drawn, live.refused,
               non_zero(&drawn), different, (unsigned long long)live.stats.runs);
        CHECK(live.refused == 0u && live.drawn == gpu_pgraph_draw_count(model));
        CHECK(live.clears_applied == gpu_pgraph_clear_count(model)); /* every clear event reached its target */
        CHECK(live.stats.runs == 1u && live.stats.draws == live.drawn); /* one target, one run */
        CHECK(live.target_pipelines > 0u && live.offscreen_pipelines == 0u); /* the BGRA8 pass kind's pipelines */
        CHECK(non_zero(&drawn) > 0u);
        CHECK(different == 0u);
        char name[96];
        snprintf(name, sizeof name, "target-%s-live", scene_names[kind]);
        write_png(name, &drawn);
        snprintf(name, sizeof name, "target-%s-replay", scene_names[kind]);
        write_png(name, &expected);
        gpu_image_free(&drawn);
        gpu_image_free(&expected);
        gpu_pgraph_destroy(model);
    }
}

/* Pass 1 draws a covering triangle into the 16 x 8 target, pass 2 samples that target (texel (1, 0), the probe module) into the
 * 64 x 64 one. `first` and `second` pick the passes, `target` the surface the sampling draw renders into. */
static gpu_pgraph *sampling_model(bool first, bool second, uint32_t second_data, uint32_t second_pitch)
{
    gpu_pgraph *model = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(model, ALL_GROUPS | GPU_PGRAPH_OUTPUT_TEXTURE | GPU_PGRAPH_OUTPUT_SURFACE);
    gpu_pgraph_set_combiner(model, true);
    stream_builder stream = {0};
    stream_setup(&stream);
    if (first) {
        static const float offset[4] = {8.0f, 4.0f, 0.0f, 0.0f};
        static const float scale[4] = {8.0f, -4.0f, 1.0f, 0.0f};
        stream_viewport(&stream, offset, scale);
        surface_words(&stream, SMALL_DATA, 64u);
        uint32_t words[COMBINER_WORDS];
        combiner_words_of(SCENE_COMBINER_FINAL, words);
        stream_pixel_shader(&stream, words);
        stream_draw(&stream, 6u, GPU_PGRAPH_OP_TRIANGLES, 3u);
    }
    if (second) {
        static const float offset[4] = {32.0f, 32.0f, 0.0f, 0.0f};
        static const float scale[4] = {32.0f, -32.0f, 1.0f, 0.0f};
        stream_viewport(&stream, offset, scale);
        surface_words(&stream, second_data, second_pitch);
        uint32_t words[COMBINER_WORDS];
        texture_words(words);
        stream_pixel_shader(&stream, words);
        stream_pair(&stream, 0x1B08u, CLAMP_BOTH);
        stream_pair(&stream, 0x1B14u, BILINEAR);
        stream_draw(&stream, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
    }
    CHECK(gpu_pgraph_decode(model, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    stream_free(&stream);
    return model;
}

static gpu_pgraph_backend sampling_backend(harness *h)
{
    gpu_pgraph_backend backend = probe_backend(&h->guest);
    backend.output_groups |= GPU_PGRAPH_OUTPUT_SURFACE;
    return backend;
}

static void test_sampling(harness *h, live_vk_texture_set *texture_set, live_vk_bind *bind)
{
    printf("a draw into a target, then a draw sampling it in place\n");
    static const float big[3][3] = {{-1.5f, -1.5f, 0.3f}, {3.0f, -1.5f, 0.3f}, {-1.5f, 3.0f, 0.3f}};
    static const float orange[4] = {0.9f, 0.5f, 0.1f, 1.0f};
    put_triangle(6u, big, orange);
    wide_texture = true;
    gpu_pgraph *whole = sampling_model(true, true, BIG_B, 256u);
    gpu_pgraph *producer = sampling_model(true, false, 0u, 0u);
    gpu_pgraph *consumer = sampling_model(false, true, BIG_B, 256u);
    prepare_probe_names(whole);
    clear_target(h, SMALL_DATA, 64u * SMALL_H);
    clear_target(h, BIG_B, 256u * 64u);
    target_binding = true; /* the title's SetTexture names the render target of the first pass */
    live_result live;
    const uint64_t binds_before = live_vk_texture_get_stats(texture_set).target_binds;
    const uint64_t uploads_before = live_vk_texture_get_stats(texture_set).uploads;
    run_frame(h, sampling_backend(h), bind, whole, 1, &live);
    if (live.refused != 0u) {
        printf("  refused: %s\n", live.first_refusal);
    }
    CHECK(live.drawn == 2u && live.refused == 0u);
    CHECK(live.stats.runs == 2u && live.stats.draws == 2u); /* the two targets are two runs, in draw order */
    CHECK(live_vk_texture_get_stats(texture_set).target_binds == binds_before + 1u);
    CHECK(live_vk_texture_get_stats(texture_set).uploads == uploads_before); /* sampled in place: nothing uploaded or copied */
    gpu_image first_live = target_image(h, SMALL_DATA, SMALL_W, SMALL_H);
    gpu_image second_live = target_image(h, BIG_B, 64u, 64u);
    /* the oracle: replay the producer, then the consumer reading the replayed image as a linear A8R8G8B8 texture */
    target_binding = false;
    gpu_image first_replay = replay_image(h, producer, sampling_backend(h), SMALL_W, SMALL_H);
    uint8_t *texels = memory + TEXTURE_OFFSET;
    memset(texels, 0, 64u * SMALL_H);
    for (size_t pixel = 0u; first_replay.pixels != NULL && pixel < (size_t)SMALL_W * SMALL_H; pixel++) {
        texels[pixel * 4u] = first_replay.pixels[pixel * 4u + 2u];
        texels[pixel * 4u + 1u] = first_replay.pixels[pixel * 4u + 1u];
        texels[pixel * 4u + 2u] = first_replay.pixels[pixel * 4u];
        texels[pixel * 4u + 3u] = first_replay.pixels[pixel * 4u + 3u];
    }
    live_texture_watch_note(TEXTURE_DATA, 0x200u);
    live_vk_bind_begin_frame(bind);
    gpu_pgraph_backend replay_backend = sampling_backend(h);
    live_vk_bind_backend(bind, &replay_backend);
    gpu_image second_replay = replay_image(h, consumer, replay_backend, 64u, 64u);
    printf("  producer differing %zu covered %zu, consumer differing %zu covered %zu\n", differing(&first_live, &first_replay),
           non_zero(&first_live), differing(&second_live, &second_replay), non_zero(&second_live));
    CHECK(non_zero(&first_live) > 0u && differing(&first_live, &first_replay) == 0u);
    CHECK(non_zero(&second_live) > 0u && differing(&second_live, &second_replay) == 0u);
    /* the sampled texel is the producer's: (1, 0) of the live target equals the pixel the consumer's triangle shows */
    CHECK(first_live.pixels != NULL && (first_live.pixels[4] | first_live.pixels[5] | first_live.pixels[6]) != 0u);
    write_png("target-sample-producer-live", &first_live);
    write_png("target-sample-producer-replay", &first_replay);
    write_png("target-sample-consumer-live", &second_live);
    write_png("target-sample-consumer-replay", &second_replay);
    gpu_image_free(&first_live);
    gpu_image_free(&second_live);
    gpu_image_free(&first_replay);
    gpu_image_free(&second_replay);
    gpu_pgraph_destroy(whole);
    gpu_pgraph_destroy(producer);
    gpu_pgraph_destroy(consumer);
    wide_texture = false;
    target_binding = false;
}

/* T1489: a swizzled target named by a swizzled header takes normalised coordinates, a linear target texels, and both are target sources. */
static void test_swizzled_target_sampling(harness *h, live_vk_bind *bind)
{
    printf("a swizzled target sampled by its swizzled header: normalised coordinates\n");
    gpu_pgraph *model = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(model, ALL_GROUPS | GPU_PGRAPH_OUTPUT_TEXTURE | GPU_PGRAPH_OUTPUT_SURFACE);
    gpu_pgraph_set_combiner(model, true);
    stream_builder stream = {0};
    stream_setup(&stream);
    surface_words(&stream, BIG_A, 256u);
    uint32_t words[COMBINER_WORDS];
    texture_words(words);
    stream_pixel_shader(&stream, words);
    stream_pair(&stream, 0x1B08u, CLAMP_BOTH);
    stream_pair(&stream, 0x1B14u, BILINEAR);
    stream_draw(&stream, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
    CHECK(gpu_pgraph_decode(model, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    stream_free(&stream);
    gpu_pgraph_backend backend = sampling_backend(h);
    live_vk_bind_backend(bind, &backend);
    live_vk_bind_begin_frame(bind);
    gpu_combiner_texture plan = {0};
    swizzled_target_binding = true;
    backend.resolve_texture(backend.texture_context, 0u, gpu_pgraph_state_now(model), 0u, &plan);
    swizzled_target_binding = false;
    printf("  swizzled: refusal %s, %ux%u unnormalised %d\n", plan.refusal != NULL ? plan.refusal : "none", (unsigned)plan.width,
           (unsigned)plan.height, (int)plan.unnormalised);
    CHECK(plan.refusal == NULL && plan.width == 64u && plan.height == 64u && !plan.unnormalised);
    gpu_combiner_texture linear_plan = {0};
    wide_texture = true;
    target_binding = true;
    backend.resolve_texture(backend.texture_context, 0u, gpu_pgraph_state_now(model), 0u, &linear_plan);
    wide_texture = false;
    target_binding = false;
    CHECK(linear_plan.refusal == NULL && linear_plan.unnormalised);
    gpu_pgraph_destroy(model);
}

/* A, B, A in one frame: three runs, each target gets exactly its own draws, and every draw lands in the image its surface names. */
static void test_two_targets(harness *h)
{
    printf("two targets drawn in alternation in one frame\n");
    gpu_pgraph *model = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(model, SURFACE_ALL);
    stream_builder stream = {0};
    stream_setup(&stream);
    surface_words(&stream, BIG_A, 256u);
    stream_draw(&stream, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
    surface_words(&stream, BIG_B, 256u);
    stream_draw(&stream, 1u, GPU_PGRAPH_OP_TRIANGLES, 3u);
    surface_words(&stream, BIG_A, 256u);
    stream_draw(&stream, 3u, GPU_PGRAPH_OP_TRIANGLES, 3u);
    CHECK(gpu_pgraph_decode(model, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    stream_free(&stream);
    gpu_pgraph *only_a = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(only_a, SURFACE_ALL);
    stream_builder first = {0};
    stream_setup(&first);
    surface_words(&first, BIG_A, 256u);
    stream_draw(&first, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
    stream_draw(&first, 3u, GPU_PGRAPH_OP_TRIANGLES, 3u);
    CHECK(gpu_pgraph_decode(only_a, first.pairs, first.count) == GPU_PGRAPH_OK);
    stream_free(&first);
    gpu_pgraph *only_b = scene_model(SCENE_OPAQUE, BIG_B, 256u); /* draws 0 and 1: the second is B's */
    gpu_pgraph *just_b = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(just_b, SURFACE_ALL);
    stream_builder second = {0};
    stream_setup(&second);
    surface_words(&second, BIG_B, 256u);
    stream_draw(&second, 1u, GPU_PGRAPH_OP_TRIANGLES, 3u);
    CHECK(gpu_pgraph_decode(just_b, second.pairs, second.count) == GPU_PGRAPH_OK);
    stream_free(&second);
    gpu_pgraph_destroy(only_b);
    clear_target(h, BIG_A, 256u * 64u);
    clear_target(h, BIG_B, 256u * 64u);
    live_result live;
    run_frame(h, scene_backend(h, SCENE_OPAQUE), NULL, model, 1, &live);
    CHECK(live.drawn == 3u && live.refused == 0u && live.stats.runs == 3u);
    /* T1264: three runs, one wait (the retire at destroy), not one per run */
    CHECK(live.waits == 1u);
    gpu_image a_live = target_image(h, BIG_A, 64u, 64u);
    gpu_image b_live = target_image(h, BIG_B, 64u, 64u);
    gpu_image a_replay = replay_image(h, only_a, scene_backend(h, SCENE_OPAQUE), 64u, 64u);
    gpu_image b_replay = replay_image(h, just_b, scene_backend(h, SCENE_OPAQUE), 64u, 64u);
    printf("  A differing %zu (covered %zu), B differing %zu (covered %zu)\n", differing(&a_live, &a_replay), non_zero(&a_live),
           differing(&b_live, &b_replay), non_zero(&b_live));
    CHECK(non_zero(&a_live) > 0u && differing(&a_live, &a_replay) == 0u);
    CHECK(non_zero(&b_live) > 0u && differing(&b_live, &b_replay) == 0u);
    CHECK(differing(&a_live, &b_live) > 0u);
    /* the old behaviour (--live-present-sync): a wait per run, the same pixels */
    live_vk_draw_set_sync_each_run(true);
    clear_target(h, BIG_A, 256u * 64u);
    clear_target(h, BIG_B, 256u * 64u);
    live_result synced;
    run_frame(h, scene_backend(h, SCENE_OPAQUE), NULL, model, 1, &synced);
    live_vk_draw_set_sync_each_run(false);
    CHECK(synced.drawn == 3u && synced.waits == 3u);
    gpu_image a_synced = target_image(h, BIG_A, 64u, 64u);
    gpu_image b_synced = target_image(h, BIG_B, 64u, 64u);
    CHECK(non_zero(&a_synced) > 0u && differing(&a_synced, &a_live) == 0u && differing(&b_synced, &b_live) == 0u);
    gpu_image_free(&a_synced);
    gpu_image_free(&b_synced);
    write_png("target-two-a", &a_live);
    write_png("target-two-b", &b_live);
    gpu_image_free(&a_live);
    gpu_image_free(&b_live);
    gpu_image_free(&a_replay);
    gpu_image_free(&b_replay);
    gpu_pgraph_destroy(model);
    gpu_pgraph_destroy(only_a);
    gpu_pgraph_destroy(just_b);
}

/* A clear belongs to the target of the draw it precedes: A gets a draw, then B gets a clear and a draw, so the clear must not land in A. */
static void test_clear_names_next_draw(harness *h)
{
    printf("a clear event is applied to the target of the draw it precedes\n");
    gpu_pgraph *model = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(model, SURFACE_ALL);
    stream_builder stream = {0};
    stream_setup(&stream);
    surface_words(&stream, BIG_A, 256u);
    stream_draw(&stream, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
    surface_words(&stream, BIG_B, 256u);
    scene_clear(&stream, 0xF0u, 0xFF3060C0u, 0u, 0u, WIDTH - 1u, 0u, HEIGHT - 1u);
    stream_draw(&stream, 1u, GPU_PGRAPH_OP_TRIANGLES, 3u);
    CHECK(gpu_pgraph_decode(model, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    stream_free(&stream);
    gpu_pgraph *a_only = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(a_only, SURFACE_ALL);
    stream_builder first = {0};
    stream_setup(&first);
    surface_words(&first, BIG_A, 256u);
    stream_draw(&first, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
    CHECK(gpu_pgraph_decode(a_only, first.pairs, first.count) == GPU_PGRAPH_OK);
    stream_free(&first);
    gpu_pgraph *b_only = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(b_only, SURFACE_ALL);
    stream_builder second = {0};
    stream_setup(&second);
    surface_words(&second, BIG_B, 256u);
    scene_clear(&second, 0xF0u, 0xFF3060C0u, 0u, 0u, WIDTH - 1u, 0u, HEIGHT - 1u);
    stream_draw(&second, 1u, GPU_PGRAPH_OP_TRIANGLES, 3u);
    CHECK(gpu_pgraph_decode(b_only, second.pairs, second.count) == GPU_PGRAPH_OK);
    stream_free(&second);
    clear_target(h, BIG_A, 256u * 64u);
    clear_target(h, BIG_B, 256u * 64u);
    live_result live;
    run_frame(h, scene_backend(h, SCENE_CLEAR_COLOUR), NULL, model, 1, &live);
    CHECK(live.drawn == 2u && live.refused == 0u && live.clears_applied == 1u && live.stats.runs == 2u);
    gpu_image a_live = target_image(h, BIG_A, 64u, 64u);
    gpu_image b_live = target_image(h, BIG_B, 64u, 64u);
    gpu_image a_replay = replay_image(h, a_only, scene_backend(h, SCENE_CLEAR_COLOUR), 64u, 64u);
    gpu_image b_replay = replay_image(h, b_only, scene_backend(h, SCENE_CLEAR_COLOUR), 64u, 64u);
    printf("  A differing %zu (covered %zu), B differing %zu (covered %zu)\n", differing(&a_live, &a_replay), non_zero(&a_live),
           differing(&b_live, &b_replay), non_zero(&b_live));
    CHECK(differing(&a_live, &a_replay) == 0u && non_zero(&a_live) < 4096u);
    CHECK(differing(&b_live, &b_replay) == 0u && non_zero(&b_live) == 4096u);
    gpu_image_free(&a_live);
    gpu_image_free(&b_live);
    gpu_image_free(&a_replay);
    gpu_image_free(&b_replay);
    gpu_pgraph_destroy(model);
    gpu_pgraph_destroy(a_only);
    gpu_pgraph_destroy(b_only);
}

/* T848: a clear names its own render target (the surface words at the CLEAR_SURFACE), not a draw's snapshot. */
static void test_clear_own_surface(harness *h)
{
    printf("a clear event names its own render target: no draw in the frame, a retarget before the next draw, refusals\n");
    live_result live;
    /* 1. a frame with no draw: the clear lands in A (the old rule refused it, "names no render target") */
    gpu_pgraph *only_clear = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(only_clear, SURFACE_ALL);
    stream_builder stream = {0};
    stream_setup(&stream);
    surface_words(&stream, BIG_A, 256u);
    scene_clear(&stream, 0xF0u, 0xFF3060C0u, 0u, 0u, WIDTH - 1u, 0u, HEIGHT - 1u);
    CHECK(gpu_pgraph_decode(only_clear, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    stream_free(&stream);
    CHECK(gpu_pgraph_draw_count(only_clear) == 0u && gpu_pgraph_clear_count(only_clear) == 1u);
    clear_target(h, BIG_A, 256u * 64u);
    run_frame(h, scene_backend(h, SCENE_CLEAR_COLOUR), NULL, only_clear, 1, &live);
    CHECK(live.clears_applied == 1u && live.stats.clears == 1u && live.stats.clears_refused == 0u);
    gpu_image a_cleared = target_image(h, BIG_A, 64u, 64u);
    CHECK(non_zero(&a_cleared) == 4096u);
    gpu_image_free(&a_cleared);
    gpu_pgraph_destroy(only_clear);
    /* 2. the title retargets between the clear and the next draw: the clear belongs to A, the draw to B */
    gpu_pgraph *retarget = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(retarget, SURFACE_ALL);
    stream_setup(&stream);
    surface_words(&stream, BIG_A, 256u);
    scene_clear(&stream, 0xF0u, 0xFF3060C0u, 0u, 0u, WIDTH - 1u, 0u, HEIGHT - 1u);
    surface_words(&stream, BIG_B, 256u);
    stream_draw(&stream, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
    CHECK(gpu_pgraph_decode(retarget, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    stream_free(&stream);
    clear_target(h, BIG_A, 256u * 64u);
    clear_target(h, BIG_B, 256u * 64u);
    run_frame(h, scene_backend(h, SCENE_CLEAR_COLOUR), NULL, retarget, 1, &live);
    CHECK(live.clears_applied == 1u && live.drawn == 1u);
    gpu_image a_retarget = target_image(h, BIG_A, 64u, 64u);
    gpu_image b_retarget = target_image(h, BIG_B, 64u, 64u);
    CHECK(non_zero(&a_retarget) == 4096u && non_zero(&b_retarget) < 4096u);
    gpu_image_free(&a_retarget);
    gpu_image_free(&b_retarget);
    gpu_pgraph_destroy(retarget);
    /* 3. negative controls: a clear with no surface words, and one naming an unregistered surface, are refused and change nothing */
    const uint32_t unregistered = 0x0500000u;
    for (int variant = 0; variant < 2; variant++) {
        gpu_pgraph *refused = gpu_pgraph_create();
        gpu_pgraph_set_output_groups(refused, SURFACE_ALL);
        stream_setup(&stream);
        if (variant == 1) {
            surface_words(&stream, unregistered, 256u);
        }
        scene_clear(&stream, 0xF0u, 0xFF3060C0u, 0u, 0u, WIDTH - 1u, 0u, HEIGHT - 1u);
        CHECK(gpu_pgraph_decode(refused, stream.pairs, stream.count) == GPU_PGRAPH_OK);
        stream_free(&stream);
        clear_target(h, BIG_A, 256u * 64u);
        run_frame(h, scene_backend(h, SCENE_CLEAR_COLOUR), NULL, refused, 1, &live);
        CHECK(live.clears_applied == 0u && live.stats.clears == 0u && live.stats.clears_refused == 1u);
        gpu_image untouched = target_image(h, BIG_A, 64u, 64u);
        CHECK(non_zero(&untouched) == 0u);
        gpu_image_free(&untouched);
        gpu_pgraph_destroy(refused);
    }
}

/* Frame 1 leaves depth in the target's zeta, frame 2 draws behind it with LESS: it shows only when the depth image was reset. */
static void test_depth_reset(harness *h)
{
    printf("depth and stencil are reset at the first draw into a target of each frame\n");
    gpu_pgraph *first = scene_model(SCENE_DEPTH, BIG_A, 256u);
    gpu_pgraph *later = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(later, SURFACE_ALL);
    stream_builder stream = {0};
    stream_setup(&stream);
    surface_words(&stream, BIG_A, 256u);
    stream_pair(&stream, 0x030Cu, 1u);
    stream_pair(&stream, 0x0354u, 0x201u); /* LESS */
    stream_pair(&stream, 0x035Cu, 1u);
    stream_draw(&stream, 2u, GPU_PGRAPH_OP_TRIANGLES, 3u);
    CHECK(gpu_pgraph_decode(later, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    stream_free(&stream);
    char error[256] = "";
    gpu_pgraph_backend backend = scene_backend(h, SCENE_DEPTH);
    live_vk_renderer *renderer = live_vk_renderer_create(&h->r->device, h->r->colour_pass, &backend, error, sizeof error);
    live_vk_draw *draw = renderer != NULL ? live_vk_draw_create(&h->r->device, renderer, h->targets, error, sizeof error) : NULL;
    CHECK(draw != NULL);
    if (draw != NULL) {
        const live_vk_target_hook hook = live_vk_draw_hook(draw);
        live_vk_renderer_set_target_hook(renderer, &hook);
        const gpu_pgraph *models[2] = {first, later};
        for (int frame = 0; frame < 2; frame++) {
            clear_target(h, BIG_A, 256u * 64u);
            live_vk_renderer_begin_frame(renderer);
            live_vk_draw_begin_frame(draw);
            for (size_t index = 0u; index < gpu_pgraph_draw_count(models[frame]); index++) {
                char reason[LIVE_PIPELINE_REASON_BYTES] = "";
                CHECK(live_vk_renderer_draw(renderer, models[frame], index, VK_NULL_HANDLE, 0u, 0u, reason, sizeof reason));
            }
            CHECK(live_vk_draw_flush(draw));
        }
        CHECK(live_vk_draw_stats_get(draw).depth_clears == 2u);
        gpu_image drawn = target_image(h, BIG_A, 64u, 64u);
        gpu_image expected = replay_image(h, later, scene_backend(h, SCENE_DEPTH), 64u, 64u);
        printf("  second frame differing %zu covered %zu\n", differing(&drawn, &expected), non_zero(&drawn));
        CHECK(non_zero(&drawn) > 0u && differing(&drawn, &expected) == 0u);
        gpu_image_free(&drawn);
        gpu_image_free(&expected);
        live_vk_draw_destroy(draw);
    }
    live_vk_renderer_destroy(renderer);
    gpu_pgraph_destroy(first);
    gpu_pgraph_destroy(later);
}

/* `written` picks which of the three words the stream writes (1 format, 2 pitch, 4 colour offset). */
static gpu_pgraph *surface_model(unsigned written, uint32_t format, uint32_t pitch, uint32_t data)
{
    gpu_pgraph *model = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(model, ALL_GROUPS | GPU_PGRAPH_OUTPUT_SURFACE);
    stream_builder stream = {0};
    stream_setup(&stream);
    if ((written & 1u) != 0u) {
        stream_pair(&stream, 0x0208u, format);
    }
    if ((written & 2u) != 0u) {
        stream_pair(&stream, 0x020Cu, pitch);
    }
    if ((written & 4u) != 0u) {
        stream_pair(&stream, 0x0210u, data);
    }
    stream_draw(&stream, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
    CHECK(gpu_pgraph_decode(model, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    stream_free(&stream);
    return model;
}

static void expect_refusal(harness *h, const char *what, gpu_pgraph *model, const char *needle)
{
    live_result live;
    clear_target(h, BIG_A, 256u * 64u);
    run_frame(h, scene_backend(h, SCENE_OPAQUE), NULL, model, 1, &live);
    gpu_image after = target_image(h, BIG_A, 64u, 64u);
    printf("  %s: %s\n", what, live.first_refusal);
    CHECK(live.drawn == 0u && live.refused == 1u && strstr(live.first_refusal, needle) != NULL);
    CHECK(live.stats.draws == 0u && live.stats.runs == 0u && non_zero(&after) == 0u); /* nothing drawn anywhere, no stand-in */
    gpu_image_free(&after);
    gpu_pgraph_destroy(model);
}

static void test_refusals(harness *h)
{
    printf("unusable surface state is refused by name\n");
    expect_refusal(h, "never written", surface_model(0u, 0u, 0u, 0u), "never written");
    expect_refusal(h, "no format word", surface_model(6u, 0x128u, 256u, BIG_A), "never written");
    expect_refusal(h, "no pitch word", surface_model(5u, 0x128u, 256u, BIG_A), "never written");
    expect_refusal(h, "no offset word", surface_model(3u, 0x128u, 256u, BIG_A), "never written");
    expect_refusal(h, "unregistered target", surface_model(7u, 0x128u, 256u, 0x0900000u), "not a registered target");
    expect_refusal(h, "pitch", surface_model(7u, 0x128u, 512u, BIG_A), "pitch 512");
    expect_refusal(h, "colour format R5G6B5", surface_model(7u, 0x123u, 256u, BIG_A), "not A8R8G8B8");
    expect_refusal(h, "colour format X8R8G8B8", surface_model(7u, 0x124u, 256u, BIG_A), "not A8R8G8B8");
    expect_refusal(h, "swizzled type on a pitch target", surface_model(7u, 0x06060228u, 0u, BIG_A), "registered with a pitch");
    expect_refusal(h, "pitch type on a swizzled target", surface_model(7u, 0x128u, 256u, SWZ_A), "registered swizzled");
    expect_refusal(h, "swizzled size mismatch", surface_model(7u, 0x07070228u, 0u, SWZ_A), "log2 size 7 x 7");
    expect_refusal(h, "unsupported type 3", surface_model(7u, 0x328u, 256u, BIG_A), "pitch (type 1) or swizzled");
    CHECK(live_vk_target_register(h->targets, BIG_C, 0x00011129u, BIG_SIZE, NULL) == LIVE_VK_OK); /* R5G6B5 64 x 64, pitch 128 */
    expect_refusal(h, "2 byte target", surface_model(7u, 0x128u, 128u, BIG_C), "2 bytes per pixel");
}

/* T1489: every scene drawn into a swizzled target is the picture the pitch target gets (the image is in picture order), not refused. */
static void test_swizzled_target(harness *h)
{
    printf("scenes drawn into a swizzled target (surface 0x%08X) against gpu_pgraph_replay\n", (unsigned)SWZ_SURFACE);
    for (int index = 0; index < SCENE_COUNT; index++) {
        const scene_kind kind = (scene_kind)index;
        if (kind == SCENE_POLYGON_ZERO) {
            continue; /* refused by design (T885) */
        }
        gpu_pgraph *model = scene_model_in(kind, SWZ_A, 0u, SWZ_SURFACE);
        gpu_pgraph *linear = scene_model(kind, BIG_A, 256u);
        clear_target(h, SWZ_A, 64u * 64u * 4u);
        live_result live;
        gpu_pgraph_backend swizzled_backend = scene_backend(h, kind);
        swizzled_backend.live_swizzled_targets = true; /* the host sets it with --live-inferred */
        run_frame(h, swizzled_backend, NULL, model, 1, &live);
        if (live.refused != 0u) {
            printf("  scene %d refused: %s\n", index, live.first_refusal);
        }
        gpu_image drawn = target_image(h, SWZ_A, 64u, 64u);
        gpu_image expected = replay_image(h, linear, scene_backend(h, kind), 64u, 64u);
        CHECK(live.refused == 0u && live.drawn == gpu_pgraph_draw_count(model) && live.stats.runs >= 1u);
        CHECK(non_zero(&drawn) > 0u && differing(&drawn, &expected) == 0u);
        gpu_image_free(&drawn);
        gpu_image_free(&expected);
        gpu_pgraph_destroy(model);
        gpu_pgraph_destroy(linear);
    }
    /* without the opt-in (the default) the same draw is refused by the surface gate and nothing is drawn */
    gpu_pgraph *model = scene_model_in(SCENE_OPAQUE, SWZ_A, 0u, SWZ_SURFACE);
    live_result off;
    clear_target(h, SWZ_A, 64u * 64u * 4u);
    run_frame(h, scene_backend(h, SCENE_OPAQUE), NULL, model, 1, &off);
    gpu_image none = target_image(h, SWZ_A, 64u, 64u);
    printf("  opt-in off: drawn %u refused %u: %s\n", off.drawn, off.refused, off.first_refusal);
    CHECK(off.drawn == 0u && off.refused == gpu_pgraph_draw_count(model) && strstr(off.first_refusal, "pitch layout") != NULL && non_zero(&none) == 0u);
    gpu_image_free(&none);
    gpu_pgraph_destroy(model);
}

static void test_precise_query(harness *h)
{
    live_vk_device unsupported = h->r->device;
    unsupported.occlusion_query_precise = false;
    CHECK(live_vk_query_create(&unsupported) == NULL);
    live_vk_query *query = live_vk_query_create(&h->r->device);
    CHECK(query != NULL);
    if (query == NULL) return;
    CHECK(live_vk_query_reset(query));
    char error[256] = "";
    gpu_pgraph_backend backend = scene_backend(h, SCENE_OPAQUE);
    live_vk_renderer *renderer = live_vk_renderer_create(&h->r->device,h->r->colour_pass,&backend,error,sizeof error);
    CHECK(renderer != NULL);
    if (renderer == NULL) { live_vk_query_destroy(query); return; }
    live_vk_draw *draw = live_vk_draw_create(&h->r->device,renderer,h->targets,error,sizeof error);
    CHECK(draw != NULL);
    if (draw != NULL) {
        live_vk_target_hook hook = live_vk_draw_hook(draw);
        live_vk_renderer_set_target_hook(renderer,&hook);
        live_vk_renderer_set_query(renderer,query,true);
        gpu_pgraph *model = scene_model(SCENE_OPAQUE,BIG_A,256u);
        clear_target(h,BIG_A,256u*64u);
        live_vk_renderer_begin_frame(renderer);
        live_vk_draw_begin_frame(draw);
        CHECK(live_vk_renderer_draw(renderer,model,0u,VK_NULL_HANDLE,0u,0u,error,sizeof error));
        uint32_t slot = live_vk_renderer_query_slot(renderer);
        CHECK(slot == 0u);
        uint64_t samples = UINT64_MAX;
        CHECK(!live_vk_query_result(query,slot,&samples)); /* unsubmitted query remains pending */
        CHECK(samples == UINT64_MAX);
        CHECK(live_vk_draw_flush(draw));
        CHECK(live_vk_query_result(query,slot,&samples));
        gpu_image pixels = target_image(h,BIG_A,64u,64u);
        CHECK(samples > 0u && samples == non_zero(&pixels)); /* independent attachment coverage */
        gpu_image_free(&pixels);
        live_vk_renderer_set_query(renderer,query,false);
        CHECK(live_vk_renderer_draw(renderer,model,1u,VK_NULL_HANDLE,0u,0u,error,sizeof error));
        CHECK(live_vk_renderer_query_slot(renderer) == UINT32_MAX);
        CHECK(live_vk_draw_flush(draw));
        CHECK(live_vk_query_reset(query));
        CHECK(!live_vk_query_result(query,0u,&samples));
        gpu_pgraph_destroy(model);
        /* Independently measured xemu no-colour/no-depth/no-stencil control:
         * a submitted empty GPU query completes with zero, not triangle coverage. */
        model = gpu_pgraph_create();
        gpu_pgraph_set_output_groups(model,SURFACE_ALL);
        stream_builder nop = {0};
        surface_words(&nop,BIG_A,256u);
        stream_setup(&nop);
        stream_pair(&nop,0x0358u,0u);
        stream_draw(&nop,0u,GPU_PGRAPH_OP_TRIANGLES,3u);
        CHECK(gpu_pgraph_decode(model,nop.pairs,nop.count) == GPU_PGRAPH_OK);
        stream_free(&nop);
        live_vk_renderer_set_query(renderer,query,true);
        CHECK(live_vk_renderer_draw(renderer,model,0u,VK_NULL_HANDLE,0u,0u,error,sizeof error));
        slot = live_vk_renderer_query_slot(renderer);
        CHECK(slot == 0u);
        CHECK(live_vk_draw_flush(draw));
        samples = UINT64_MAX;
        CHECK(live_vk_query_result(query,slot,&samples));
        CHECK(samples == 0u);
        gpu_pgraph_destroy(model);
        live_vk_draw_destroy(draw);
    }
    live_vk_renderer_destroy(renderer);
    live_vk_query_destroy(query);
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
    rig r;
    CHECK(rig_init(&r, device));
    harness h = {&r, device, {0}, NULL, {MEMORY_BASE, memory, sizeof memory}};
    live_texture_cache_init(&h.cache, true);
    live_vk_target_device description = {0};
    description.device = r.native.device;
    description.queue = r.native.queue;
    description.queue_family = r.native.queue_family;
    description.command_pool = r.native.command_pool;
    description.memory = r.native.memory_properties;
    description.get_device_proc_addr = r.native.get_device_proc_addr;
    h.targets = live_vk_target_create(&description, 0u, &h.cache);
    CHECK(h.targets != NULL);
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
    if (h.targets == NULL || texture_set == NULL) {
        return 1;
    }
    CHECK(live_vk_target_register(h.targets, BIG_A, BIG_FORMAT, BIG_SIZE, NULL) == LIVE_VK_OK);
    CHECK(live_vk_target_register(h.targets, BIG_B, BIG_FORMAT, BIG_SIZE, NULL) == LIVE_VK_OK);
    CHECK(live_vk_target_register(h.targets, SWZ_A, SWZ_FORMAT, 0u, NULL) == LIVE_VK_OK);
    CHECK(live_vk_target_register(h.targets, SMALL_DATA, BIG_FORMAT, WIDE_SIZE_WORD, NULL) == LIVE_VK_OK);
    const live_vk_bind_source source = {binding_of, NULL, fake_guest_read, &h.guest};
    live_vk_bind *bind = live_vk_bind_create(&h.cache, texture_set, &source);
    CHECK(bind != NULL);
    live_vk_bind_use_targets(bind, h.targets);
    test_precise_query(&h);
    test_parity(&h);
    test_sampling(&h, texture_set, bind);
    test_swizzled_target_sampling(&h, bind);
    test_two_targets(&h);
    test_clear_names_next_draw(&h);
    test_clear_own_surface(&h);
    test_depth_reset(&h);
    test_refusals(&h);
    test_swizzled_target(&h);
    live_vk_bind_destroy(bind);
    live_vk_texture_destroy(texture_set);
    live_vk_target_destroy(h.targets);
    live_texture_cache_free(&h.cache);
    r.fn.vkDestroyRenderPass(r.native.device, r.colour_pass, NULL);
    gpu_device_destroy(device);
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
