/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T791, the window frame loop (src/gpu/live_vk_frame.c) on a real SDL3 + Vulkan window: live_vk_frame_attach installs the
 * gpu_window frame hook, each present draws the model the source names into the swapchain pass, a refused draw is named in the
 * census and makes the hook report a refusal, and no model draws nothing without refusing. Skips (77) without a window or a
 * Vulkan device (the dummy video driver the ctest environment sets cannot create a Vulkan window), FAILs instead when
 * TSFP_TEST_GPU_WINDOW_REQUIRED is set. `--hold MS` keeps the window up after the last present and `--scene NAME` picks the
 * scene, for looking at it under Xvfb.
 */
#include "gpu_window.h"
#include "live_texture_watch.h"
#include "live_vk_frame.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

static int failures;
static int checks;

#define CHECK(condition)                                                                      \
    do {                                                                                      \
        checks++;                                                                             \
        if (!(condition)) {                                                                   \
            failures++;                                                                       \
            printf("  FAIL line %d: %s\n", __LINE__, #condition);                             \
        }                                                                                     \
    } while (0)

#include "live_vk_scenes.h"
#include "live_vk_texture_scene.h"

static int unavailable(const char *why)
{
    const char *required = getenv("TSFP_TEST_GPU_WINDOW_REQUIRED");
    fprintf(stderr, "%s: %s\n", required != NULL ? "FAIL" : "SKIP", why != NULL ? why : "no detail");
    return required != NULL ? 1 : 77;
}

typedef struct {
    const gpu_pgraph *model;
} source_state;

static const gpu_pgraph *source_model(void *context)
{
    return ((source_state *)context)->model;
}

/* A combiner draw that samples a guest texture through a window frame: the attach installs the planning callback and the
 * texture hook, so the draw is drawn (not refused for want of a hook), and the texture's pixels reach the swapchain pass. */
static void test_textured(gpu_window *renderer, fake_guest *guest)
{
    printf("textured frame\n");
    static const uint8_t texels[16] = {30u, 20u, 10u, 255u, 100u, 200u, 255u, 255u, 3u, 2u, 1u, 255u, 6u, 5u, 4u, 255u};
    linear_texture = false;
    write_texels(texels);
    gpu_pgraph *model = textured_model(true, CLAMP_BOTH, BILINEAR);
    prepare_probe_names(model);
    gpu_window_native native;
    CHECK(gpu_window_get_native(renderer, &native));
    SDL_FunctionPointer sdl_gipa = SDL_Vulkan_GetVkGetInstanceProcAddr();
    PFN_vkGetInstanceProcAddr gipa = NULL;
    memcpy(&gipa, &sdl_gipa, sizeof gipa);
    live_texture_cache cache;
    live_texture_cache_init(&cache, true);
    const char *texture_error = NULL;
    live_vk_texture_set *set = live_vk_texture_create(&native, gipa, &texture_error);
    CHECK(set != NULL);
    if (set != NULL) {
        const live_vk_bind_source bind_source = {binding_of, NULL, fake_guest_read, guest};
        live_vk_bind *bind = live_vk_bind_create(&cache, set, &bind_source);
        CHECK(bind != NULL);
        gpu_pgraph_backend backend = probe_backend(guest);
        source_state state = {model};
        const live_vk_frame_source source = {source_model, &state};
        const live_vk_frame_config config = {bind, NULL};
        char error[256] = "";
        const float clear[4] = {0.2f, 0.4f, 0.6f, 1.0f};
        const uint64_t refusals_before = gpu_window_get_stats(renderer).draw_refusals;
        live_vk_frame *frame = live_vk_frame_attach(renderer, &backend, &source, &config, error, sizeof error);
        CHECK(frame != NULL);
        if (frame != NULL) {
            CHECK(gpu_window_present(renderer, clear));
            const live_vk_frame_stats stats = live_vk_frame_get_stats(frame);
            CHECK(stats.draws_offered == 1u && stats.draws_refused == 0u && gpu_window_get_stats(renderer).draw_refusals == refusals_before);
            CHECK(live_vk_texture_get_stats(set).uploads == 1u);
            /* a second present on the unchanged texture uploads nothing, a LockRect style note makes it upload again */
            CHECK(gpu_window_present(renderer, clear));
            CHECK(live_vk_texture_get_stats(set).uploads == 1u);
            live_texture_watch_note(TEXTURE_DATA, 0x100u);
            CHECK(gpu_window_present(renderer, clear));
            CHECK(live_vk_texture_get_stats(set).uploads == 2u);
            live_vk_frame_detach(frame);
        }
        /* without the bridge the same draw is refused by name, no stand-in */
        live_vk_frame *bare = live_vk_frame_attach(renderer, &backend, &source, NULL, error, sizeof error);
        CHECK(bare != NULL);
        if (bare != NULL) {
            CHECK(gpu_window_present(renderer, clear));
            CHECK(live_vk_frame_get_stats(bare).draws_refused == 1u && gpu_window_get_stats(renderer).draw_refusals == refusals_before + 1u);
            live_vk_frame_detach(bare);
        }
        live_vk_bind_destroy(bind);
        live_vk_texture_destroy(set);
    }
    live_texture_cache_free(&cache);
    gpu_pgraph_destroy(model);
}

#define COPY_SOURCE 0x0100000u
#define COPY_DESTINATION 0x0200000u
#define COPY_FORMAT 0x00011229u /* linear A8R8G8B8 */
#define COPY_SIZE (15u | (7u << 12)) /* 16 x 8, pitch 64 */

/* draw, CopyRects of the whole 16 x 8 target A to B, draw: the copy sits between the two draws (before_draw 1). */
static gpu_pgraph *copy_model(void)
{
    gpu_pgraph *model = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(model, ALL_GROUPS | GPU_PGRAPH_OUTPUT_BLIT);
    stream_builder first = {0};
    stream_setup(&first);
    stream_draw(&first, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
    CHECK(gpu_pgraph_decode(model, first.pairs, first.count) == GPU_PGRAPH_OK);
    stream_free(&first);
    static const struct {
        uint32_t subchannel, method, data;
    } blit[] = {
        {3u, GPU_PGRAPH_S2D_OFFSET_SOURCE, COPY_SOURCE},
        {3u, GPU_PGRAPH_S2D_OFFSET_DESTINATION, COPY_DESTINATION},
        {3u, GPU_PGRAPH_S2D_PITCH, 64u | (64u << 16)},
        {3u, GPU_PGRAPH_S2D_COLOR_FORMAT, GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8},
        {2u, GPU_PGRAPH_BLIT_OPERATION, GPU_PGRAPH_BLIT_OPERATION_SRCCOPY},
        {2u, GPU_PGRAPH_BLIT_POINT_IN, 0u},
        {2u, GPU_PGRAPH_BLIT_POINT_OUT, 0u},
        {2u, GPU_PGRAPH_BLIT_SIZE, 16u | (8u << 16)},
    };
    for (size_t i = 0u; i < sizeof blit / sizeof blit[0]; i++) {
        CHECK(gpu_pgraph_decode_other_subchannel(model, blit[i].subchannel, blit[i].method, blit[i].data) == GPU_PGRAPH_OK);
    }
    stream_builder second = {0};
    stream_draw(&second, 1u, GPU_PGRAPH_OP_TRIANGLES, 3u);
    CHECK(gpu_pgraph_decode(model, second.pairs, second.count) == GPU_PGRAPH_OK);
    stream_free(&second);
    CHECK(gpu_pgraph_draw_count(model) == 2u && gpu_pgraph_copy_count(model) == 1u);
    CHECK(gpu_pgraph_copy_count(model) == 1u && gpu_pgraph_copy_at(model, 0u)->before_draw == 1u);
    return model;
}

static void test_copies(gpu_window *renderer, const gpu_pgraph_backend *base_backend, fake_guest *guest)
{
    printf("copies\n");
    const float clear[4] = {0.2f, 0.4f, 0.6f, 1.0f};
    gpu_pgraph *model = copy_model();
    gpu_pgraph_backend backend = *base_backend;
    backend.context = guest;
    source_state state = {model};
    const live_vk_frame_source source = {source_model, &state};
    char error[256] = "";
    /* without a target set the events are seen and counted, not applied */
    live_vk_frame *ignoring = live_vk_frame_attach(renderer, &backend, &source, NULL, error, sizeof error);
    CHECK(ignoring != NULL);
    if (ignoring != NULL) {
        CHECK(gpu_window_present(renderer, clear));
        const live_vk_frame_stats stats = live_vk_frame_get_stats(ignoring);
        CHECK(stats.copies_ignored == 1u && stats.copies_applied == 0u && stats.draws_refused == 0u);
        live_vk_frame_detach(ignoring);
    }
    /* with one the blit runs on the device: B becomes A */
    gpu_window_native native;
    CHECK(gpu_window_get_native(renderer, &native));
    live_vk_target_device description;
    SDL_FunctionPointer sdl_gipa = SDL_Vulkan_GetVkGetInstanceProcAddr();
    PFN_vkGetInstanceProcAddr gipa = NULL;
    memcpy(&gipa, &sdl_gipa, sizeof gipa);
    CHECK(live_vk_target_device_from_native(&native, gipa, &description));
    live_vk_target_set *targets = live_vk_target_create(&description, 0u, NULL);
    CHECK(targets != NULL);
    if (targets != NULL) {
        uint8_t source_bytes[64u * 8u], destination_bytes[64u * 8u], readback[64u * 8u];
        for (size_t i = 0u; i < sizeof source_bytes; i++) {
            source_bytes[i] = (uint8_t)(i * 7u + 3u);
            destination_bytes[i] = 0xA3u;
        }
        CHECK(live_vk_target_register(targets, COPY_SOURCE, COPY_FORMAT, COPY_SIZE, NULL) == LIVE_VK_OK);
        CHECK(live_vk_target_register(targets, COPY_DESTINATION, COPY_FORMAT, COPY_SIZE, NULL) == LIVE_VK_OK);
        CHECK(live_vk_target_upload(targets, COPY_SOURCE, source_bytes, sizeof source_bytes) == LIVE_VK_OK);
        CHECK(live_vk_target_upload(targets, COPY_DESTINATION, destination_bytes, sizeof destination_bytes) == LIVE_VK_OK);
        const live_vk_frame_config config = {NULL, targets};
        live_vk_frame *applying = live_vk_frame_attach(renderer, &backend, &source, &config, error, sizeof error);
        CHECK(applying != NULL);
        if (applying != NULL) {
            CHECK(gpu_window_present(renderer, clear));
            const live_vk_frame_stats stats = live_vk_frame_get_stats(applying);
            CHECK(stats.copies_applied == 1u && stats.copies_refused == 0u && stats.copies_ignored == 0u);
            CHECK(live_vk_target_readback(targets, COPY_DESTINATION, readback, sizeof readback) == LIVE_VK_OK);
            CHECK(memcmp(readback, source_bytes, sizeof readback) == 0);
            CHECK(live_vk_target_stats_get(targets).copy_image_blits + live_vk_target_stats_get(targets).staged_blits == 1u);
            CHECK(live_vk_frame_overlay_submit(applying, 1u, 1u, (const uint8_t[3]){1u, 2u, 3u}));
            live_vk_frame_detach(applying);
        }
        live_vk_target_destroy(targets);
    }
    gpu_pgraph_destroy(model);
}

/* T828: the clear events of a model are applied inside the swapchain pass in order with the draws (counted, none refused for a
 * colour clear), a depth clear is refused by name and makes the hook report the refusal, and flip_y attaches on this window's
 * device (VK_KHR_maintenance1) and draws. */
static void test_clears(gpu_window *renderer)
{
    printf("clears and flip_y\n");
    const float clear[4] = {0.2f, 0.4f, 0.6f, 1.0f};
    static const struct {
        scene_kind kind;
        uint64_t clears, clears_refused, draws, draws_refused;
    } cases[] = {
        {SCENE_CLEAR_COLOUR, 1u, 0u, 2u, 0u}, {SCENE_CLEAR_RECT, 2u, 0u, 2u, 0u}, {SCENE_CLEAR_MASK, 2u, 0u, 2u, 0u},
        {SCENE_CLEAR_DEPTH, 2u, 2u, 2u, 2u},  {SCENE_FLIP_Y, 1u, 0u, 3u, 0u},
    };
    for (size_t index = 0u; index < sizeof cases / sizeof cases[0]; index++) {
        scene s = make_scene(cases[index].kind);
        gpu_pgraph_backend backend = make_backend_for(cases[index].kind, &s.guest, EVERYTHING);
        source_state state = {s.model};
        const live_vk_frame_source source = {source_model, &state};
        char error[256] = "";
        live_vk_frame *frame = live_vk_frame_attach(renderer, &backend, &source, NULL, error, sizeof error);
        CHECK(frame != NULL);
        if (frame != NULL) {
            const uint64_t refusals_before = gpu_window_get_stats(renderer).draw_refusals;
            CHECK(gpu_window_present(renderer, clear));
            const live_vk_frame_stats stats = live_vk_frame_get_stats(frame);
            printf("  %s: clears %llu refused %llu draws %llu refused %llu\n", scene_names[cases[index].kind],
                   (unsigned long long)stats.clears_offered, (unsigned long long)stats.clears_refused,
                   (unsigned long long)stats.draws_offered, (unsigned long long)stats.draws_refused);
            CHECK(stats.clears_offered == cases[index].clears && stats.clears_refused == cases[index].clears_refused);
            CHECK(stats.draws_offered == cases[index].draws && stats.draws_refused == cases[index].draws_refused);
            const bool refused = cases[index].clears_refused != 0u || cases[index].draws_refused != 0u;
            CHECK(gpu_window_get_stats(renderer).draw_refusals == refusals_before + (refused ? 1u : 0u));
            const live_vk_stats drawn = live_vk_renderer_stats(live_vk_frame_renderer(frame));
            CHECK(drawn.clears_applied == cases[index].clears && drawn.clears_refused == cases[index].clears_refused);
            CHECK((drawn.clears_refused == 0u) == (strlen(live_vk_renderer_clear_refusal(live_vk_frame_renderer(frame))) == 0u));
            if (cases[index].kind == SCENE_CLEAR_MASK) {
                CHECK(drawn.clears_masked == 2u); /* both events have a partial channel mask */
            }
            live_vk_frame_detach(frame);
        }
        gpu_pgraph_destroy(s.model);
    }
}

#define TARGET_A 0x0200000u
#define TARGET_B 0x0300000u
#define TARGET_SIZE_64 (63u | (63u << 12) | (3u << 24)) /* 64 x 64, pitch 256 */

static void target_surface(stream_builder *stream, uint32_t data, uint32_t pitch)
{
    stream_pair(stream, 0x0208u, 0x128u);
    stream_pair(stream, 0x020Cu, pitch);
    stream_pair(stream, 0x0210u, data);
}

/* surface A, draw 0, CopyRects A to B (the whole 64 x 64), draw 1 into A: B must hold A as it was at the copy. */
static gpu_pgraph *target_copy_model(bool with_copy, bool second_draw)
{
    gpu_pgraph *model = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(model, ALL_GROUPS | GPU_PGRAPH_OUTPUT_BLIT | GPU_PGRAPH_OUTPUT_SURFACE);
    stream_builder first = {0};
    stream_setup(&first);
    target_surface(&first, TARGET_A, 256u);
    stream_draw(&first, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
    CHECK(gpu_pgraph_decode(model, first.pairs, first.count) == GPU_PGRAPH_OK);
    stream_free(&first);
    if (with_copy) {
        const struct {
            uint32_t subchannel, method, data;
        } blit[] = {
            {3u, GPU_PGRAPH_S2D_OFFSET_SOURCE, TARGET_A},
            {3u, GPU_PGRAPH_S2D_OFFSET_DESTINATION, TARGET_B},
            {3u, GPU_PGRAPH_S2D_PITCH, 256u | (256u << 16)},
            {3u, GPU_PGRAPH_S2D_COLOR_FORMAT, GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8},
            {2u, GPU_PGRAPH_BLIT_OPERATION, GPU_PGRAPH_BLIT_OPERATION_SRCCOPY},
            {2u, GPU_PGRAPH_BLIT_POINT_IN, 0u},
            {2u, GPU_PGRAPH_BLIT_POINT_OUT, 0u},
            {2u, GPU_PGRAPH_BLIT_SIZE, 64u | (64u << 16)},
        };
        for (size_t i = 0u; i < sizeof blit / sizeof blit[0]; i++) {
            CHECK(gpu_pgraph_decode_other_subchannel(model, blit[i].subchannel, blit[i].method, blit[i].data) == GPU_PGRAPH_OK);
        }
    }
    if (second_draw) {
        stream_builder second = {0};
        stream_draw(&second, 1u, GPU_PGRAPH_OP_TRIANGLES, 3u);
        CHECK(gpu_pgraph_decode(model, second.pairs, second.count) == GPU_PGRAPH_OK);
        stream_free(&second);
    }
    return model;
}

typedef struct {
    unsigned calls;
    uint8_t rgba[64u * 64u * 4u];
    uint32_t width, height;
} capture;

static bool capture_present(void *context, const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t stride)
{
    capture *shot = context;
    shot->calls++;
    shot->width = width;
    shot->height = height;
    if (width == 64u && height == 64u && stride == 256u) {
        memcpy(shot->rgba, rgba, sizeof shot->rgba);
    }
    return true;
}

typedef struct { unsigned calls; uint32_t offsets[4]; uint32_t counts[4]; } query_capture;
static bool capture_query(void *context, const gpu_pgraph_query *event, uint32_t samples)
{
    query_capture *capture = context;
    if (capture->calls == 4u) return false;
    capture->offsets[capture->calls] = event->data & 0xFFFFFFu;
    capture->counts[capture->calls++] = samples;
    return true;
}

static size_t target_pixels_set(const uint8_t *bgra)
{
    size_t count = 0u;
    for (size_t pixel = 0u; pixel < 64u * 64u; pixel++) {
        count += (bgra[pixel * 4u] | bgra[pixel * 4u + 1u] | bgra[pixel * 4u + 2u] | bgra[pixel * 4u + 3u]) != 0u;
    }
    return count;
}

/* T829: live draws into T793 targets through the window frame hook: the draws are submitted to the target images before a
 * CopyRects event runs, so the copy carries exactly the draws before it, the later draw lands in A only, and the Swap's front buffer
 * (B) is then presented at the modelled vblank. A draw sampling the target it renders into is refused by name. */
static void test_target_draws(gpu_window *renderer, const gpu_pgraph_backend *base_backend, fake_guest *guest)
{
    printf("draws into target images\n");
    const float clear[4] = {0.2f, 0.4f, 0.6f, 1.0f};
    gpu_window_native native;
    CHECK(gpu_window_get_native(renderer, &native));
    live_vk_target_device description;
    SDL_FunctionPointer sdl_gipa = SDL_Vulkan_GetVkGetInstanceProcAddr();
    PFN_vkGetInstanceProcAddr gipa = NULL;
    memcpy(&gipa, &sdl_gipa, sizeof gipa);
    CHECK(live_vk_target_device_from_native(&native, gipa, &description));
    live_texture_cache cache;
    live_texture_cache_init(&cache, true);
    live_vk_target_set *targets = live_vk_target_create(&description, 0u, &cache);
    const char *texture_error = NULL;
    live_vk_texture_set *texture_set = live_vk_texture_create(&native, gipa, &texture_error);
    CHECK(targets != NULL && texture_set != NULL);
    if (targets == NULL || texture_set == NULL) {
        return;
    }
    CHECK(live_vk_target_register(targets, TARGET_A, COPY_FORMAT, TARGET_SIZE_64, NULL) == LIVE_VK_OK);
    CHECK(live_vk_target_register(targets, TARGET_B, COPY_FORMAT, TARGET_SIZE_64, NULL) == LIVE_VK_OK);
    CHECK(live_vk_target_register(targets, WIDE_TARGET_DATA, COPY_FORMAT, WIDE_SIZE_WORD, NULL) == LIVE_VK_OK);
    const live_vk_bind_source bind_source = {binding_of, NULL, fake_guest_read, guest};
    live_vk_bind *bind = live_vk_bind_create(&cache, texture_set, &bind_source);
    live_vk_bind_use_targets(bind, targets);
    gpu_pgraph_backend backend = *base_backend;
    backend.context = guest;
    backend.output_groups |= GPU_PGRAPH_OUTPUT_SURFACE;
    gpu_pgraph *only_first = target_copy_model(false, false);
    gpu_pgraph *full = target_copy_model(true, true);
    CHECK(gpu_pgraph_draw_count(full) == 2u && gpu_pgraph_copy_count(full) == 1u && gpu_pgraph_copy_at(full, 0u)->before_draw == 1u);
    source_state state = {only_first};
    const live_vk_frame_source source = {source_model, &state};
    const live_vk_frame_config config = {bind, targets};
    char error[256] = "";
    live_vk_frame *frame = live_vk_frame_attach(renderer, &backend, &source, &config, error, sizeof error);
    CHECK(frame != NULL);
    uint8_t zero[256u * 64u] = {0}, a_first[256u * 64u], a_final[256u * 64u], b_final[256u * 64u];
    if (frame != NULL) {
        CHECK(live_vk_frame_draw(frame) == NULL);
        CHECK(live_vk_frame_enable_target_draws(frame, error, sizeof error) && live_vk_frame_draw(frame) != NULL);
        CHECK(live_vk_frame_enable_target_draws(frame, error, sizeof error)); /* idempotent */
        CHECK(live_vk_target_upload(targets, TARGET_A, zero, sizeof zero) == LIVE_VK_OK);
        CHECK(live_vk_target_upload(targets, TARGET_B, zero, sizeof zero) == LIVE_VK_OK);
        CHECK(gpu_window_present(renderer, clear));
        live_vk_frame_stats stats = live_vk_frame_get_stats(frame);
        CHECK(stats.draws_offered == 1u && stats.draws_refused == 0u && live_vk_draw_stats_get(live_vk_frame_draw(frame)).runs == 1u);
        CHECK(live_vk_target_readback(targets, TARGET_A, a_first, sizeof a_first) == LIVE_VK_OK);
        CHECK(target_pixels_set(a_first) > 0u);
        /* the whole model: draw 0, the copy, draw 1 */
        CHECK(live_vk_target_upload(targets, TARGET_A, zero, sizeof zero) == LIVE_VK_OK);
        state.model = full;
        CHECK(gpu_window_present(renderer, clear));
        stats = live_vk_frame_get_stats(frame);
        CHECK(stats.draws_offered == 3u && stats.draws_refused == 0u && stats.copies_applied == 1u && stats.copies_refused == 0u);
        CHECK(live_vk_target_readback(targets, TARGET_A, a_final, sizeof a_final) == LIVE_VK_OK);
        CHECK(live_vk_target_readback(targets, TARGET_B, b_final, sizeof b_final) == LIVE_VK_OK);
        printf("  A first %zu set, A final %zu set, B %zu set\n", target_pixels_set(a_first), target_pixels_set(a_final),
               target_pixels_set(b_final));
        CHECK(memcmp(b_final, a_first, sizeof b_final) == 0);  /* the copy carried draw 0 only */
        CHECK(memcmp(a_final, a_first, sizeof a_final) != 0);  /* draw 1 came after it and landed in A */
        CHECK(live_vk_draw_stats_get(live_vk_frame_draw(frame)).runs == 3u); /* one run per frame before, two now: a flush before the copy */
        CHECK(live_vk_draw_stats_get(live_vk_frame_draw(frame)).depth_clears == 2u); /* the target A of each of the two frames */
        /* the present: B as the Swap's front buffer at the modelled vblank, equal to its bytes with alpha opaque */
        capture shot;
        memset(&shot, 0, sizeof shot);
        live_vk_target_set_present(targets, capture_present, &shot);
        d3d8_frame_record record;
        memset(&record, 0, sizeof record);
        record.number = 1u;
        record.interval = 1u;
        record.vblank = 1u;
        record.data = TARGET_B;
        record.format_word = COPY_FORMAT;
        record.size_word = TARGET_SIZE_64;
        CHECK(live_vk_target_present_submit(targets, &record) == LIVE_TARGET_OK);
        CHECK(live_vk_target_vblank(targets, 2u, NULL));
        bool same = shot.calls == 1u && shot.width == 64u && shot.height == 64u;
        for (size_t pixel = 0u; same && pixel < 64u * 64u; pixel++) {
            same = shot.rgba[pixel * 4u] == b_final[pixel * 4u + 2u] && shot.rgba[pixel * 4u + 1u] == b_final[pixel * 4u + 1u] &&
                   shot.rgba[pixel * 4u + 2u] == b_final[pixel * 4u] && shot.rgba[pixel * 4u + 3u] == 0xFFu;
        }
        CHECK(same);
        live_vk_frame_detach(frame);
    }
    /* Real query events: reset/enable/draw/disable/report, then reset/report without a draw. */
    gpu_pgraph *queried = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(queried, ALL_GROUPS | GPU_PGRAPH_OUTPUT_SURFACE);
    gpu_pgraph_set_visibility(queried,true);
    stream_builder query_stream = {0};
    target_surface(&query_stream,TARGET_A,256u);
    stream_setup(&query_stream);
    stream_pair(&query_stream,0x17C8u,1u);
    stream_pair(&query_stream,0x17CCu,1u);
    stream_draw(&query_stream,0u,GPU_PGRAPH_OP_TRIANGLES,3u);
    stream_pair(&query_stream,0x17CCu,0u);
    stream_pair(&query_stream,0x17D0u,0x01001000u);
    stream_pair(&query_stream,0x17C8u,1u);
    stream_pair(&query_stream,0x17D0u,0x01001010u);
    /* Same before_draw: earlier reports, reset/enable, then clear, then draw. */
    stream_pair(&query_stream,0x17C8u,1u);
    stream_pair(&query_stream,0x17CCu,1u);
    scene_clear(&query_stream,0xF0u,0u,0u,0u,63u,0u,63u);
    stream_draw(&query_stream,0u,GPU_PGRAPH_OP_TRIANGLES,3u);
    stream_pair(&query_stream,0x17CCu,0u);
    stream_pair(&query_stream,0x17D0u,0x01001020u);
    /* A pitch mismatch is an actual refused clear, so its query report stays pending. */
    stream_pair(&query_stream,0x17C8u,1u);
    stream_pair(&query_stream,0x17CCu,1u);
    stream_pair(&query_stream,0x020Cu,257u);
    scene_clear(&query_stream,0xF0u,0u,0u,0u,63u,0u,63u);
    stream_pair(&query_stream,0x17CCu,0u);
    stream_pair(&query_stream,0x17D0u,0x01001030u);
    stream_pair(&query_stream,0x020Cu,256u);
    stream_pair(&query_stream,0x17C8u,1u);
    stream_pair(&query_stream,0x17D0u,0x01001040u);
    CHECK(gpu_pgraph_decode(queried,query_stream.pairs,query_stream.count) == GPU_PGRAPH_OK);
    state.model = queried;
    frame = live_vk_frame_attach(renderer,&backend,&source,&config,error,sizeof error);
    CHECK(frame != NULL);
    if (frame != NULL) {
        query_capture counts = {0};
        CHECK(live_vk_frame_enable_target_draws(frame,error,sizeof error));
        CHECK(live_vk_frame_enable_visibility(frame,capture_query,&counts,error,sizeof error));
        CHECK(live_vk_target_upload(targets,TARGET_A,zero,sizeof zero) == LIVE_VK_OK);
        CHECK(live_vk_frame_run_targets(frame));
        CHECK(live_vk_frame_get_stats(frame).clears_refused == 1u);
        CHECK(live_vk_target_readback(targets,TARGET_A,a_final,sizeof a_final) == LIVE_VK_OK);
        CHECK(counts.calls == 4u && counts.offsets[0] == 0x1000u && counts.offsets[1] == 0x1010u && counts.offsets[2] == 0x1020u && counts.offsets[3] == 0x1040u);
        CHECK(counts.counts[0] == target_pixels_set(a_final) && counts.counts[0] > 0u);
        CHECK(counts.counts[1] == 0u);
        CHECK(counts.counts[2] == counts.counts[0]);
        CHECK(counts.counts[3] == 0u);
        live_vk_frame_detach(frame);
    }
    gpu_pgraph_destroy(queried);
    stream_free(&query_stream);
    /* a clear event goes to the target of the draw it precedes (T828 clears applied to T829 targets), not to the swapchain */
    gpu_pgraph *cleared = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(cleared, ALL_GROUPS | GPU_PGRAPH_OUTPUT_SURFACE);
    stream_builder clear_stream = {0};
    target_surface(&clear_stream, TARGET_B, 256u);
    build_stream(&clear_stream, SCENE_CLEAR_COLOUR);
    CHECK(gpu_pgraph_decode(cleared, clear_stream.pairs, clear_stream.count) == GPU_PGRAPH_OK);
    stream_free(&clear_stream);
    state.model = cleared;
    frame = live_vk_frame_attach(renderer, &backend, &source, &config, error, sizeof error);
    CHECK(frame != NULL && live_vk_frame_enable_target_draws(frame, error, sizeof error));
    if (frame != NULL) {
        CHECK(live_vk_target_upload(targets, TARGET_B, zero, sizeof zero) == LIVE_VK_OK);
        CHECK(gpu_window_present(renderer, clear));
        const live_vk_frame_stats stats = live_vk_frame_get_stats(frame);
        CHECK(stats.clears_offered == 1u && stats.clears_refused == 0u && stats.draws_refused == 0u);
        CHECK(live_vk_draw_stats_get(live_vk_frame_draw(frame)).clears == 1u);
        CHECK(live_vk_target_readback(targets, TARGET_B, b_final, sizeof b_final) == LIVE_VK_OK);
        CHECK(target_pixels_set(b_final) == 64u * 64u); /* the clear covers the whole target, the draws on top */
        live_vk_frame_detach(frame);
    }
    gpu_pgraph_destroy(cleared);
    /* feedback: the draw samples render target WIDE_TARGET_DATA and renders into it, refused by name, nothing drawn */
    gpu_pgraph *feedback = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(feedback, ALL_GROUPS | GPU_PGRAPH_OUTPUT_TEXTURE | GPU_PGRAPH_OUTPUT_SURFACE);
    gpu_pgraph_set_combiner(feedback, true);
    stream_builder stream = {0};
    stream_setup(&stream);
    target_surface(&stream, WIDE_TARGET_DATA, 64u);
    uint32_t words[COMBINER_WORDS];
    texture_words(words);
    stream_pixel_shader(&stream, words);
    stream_pair(&stream, 0x1B08u, CLAMP_BOTH);
    stream_pair(&stream, 0x1B14u, BILINEAR);
    stream_draw(&stream, 0u, GPU_PGRAPH_OP_TRIANGLES, 3u);
    CHECK(gpu_pgraph_decode(feedback, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    stream_free(&stream);
    prepare_probe_names(feedback);
    wide_texture = true;
    target_binding = true;
    gpu_pgraph_backend textured = probe_backend(guest);
    textured.output_groups |= GPU_PGRAPH_OUTPUT_SURFACE;
    state.model = feedback;
    frame = live_vk_frame_attach(renderer, &textured, &source, &config, error, sizeof error);
    CHECK(frame != NULL && live_vk_frame_enable_target_draws(frame, error, sizeof error));
    if (frame != NULL) {
        /* T1206: the draw samples a copy of the target as it stood before the draw (xemu's copy_surface_to_texture) and is
         * drawn: nothing refused, one snapshot copy per draw */
        CHECK(gpu_window_present(renderer, clear));
        CHECK(live_vk_frame_get_stats(frame).draws_refused == 0u);
        CHECK(live_vk_draw_stats_get(live_vk_frame_draw(frame)).snapshots == 1u);
        CHECK(live_vk_frame_run_targets(frame));
        CHECK(live_vk_frame_get_stats(frame).draws_refused == 0u);
        CHECK(live_vk_draw_stats_get(live_vk_frame_draw(frame)).snapshots == 2u);
        /* without a snapshot provider the draw is still refused by name (the pre-T1206 behaviour stays available) */
        live_vk_bind_set_snapshot(bind, NULL, NULL);
        CHECK(live_vk_frame_run_targets(frame));
        const live_pipeline_census *census = live_vk_renderer_census(live_vk_frame_renderer(frame));
        CHECK(live_vk_frame_get_stats(frame).draws_refused == 1u && census->count == 1u);
        CHECK(census->count == 1u && strstr(census->refusals[0].reason, "renders into it") != NULL);
        CHECK(live_vk_draw_stats_get(live_vk_frame_draw(frame)).snapshots == 2u);
        /* T847: a frame with a refused draw is counted (frames_refused) but is NOT a loop failure of live_vk_frame_run_targets (the
         * T838 host counter reported 1062 failures in 1062 title frames for exactly this) */
        CHECK(live_vk_frame_get_stats(frame).frames_refused == 1u);
        CHECK(live_vk_frame_run_targets(frame));
        CHECK(live_vk_frame_get_stats(frame).frames_refused == 2u && live_vk_frame_get_stats(frame).draws_refused == 2u);
        live_vk_frame_detach(frame);
    }
    wide_texture = false;
    target_binding = false;
    /* without a target set there is nothing to draw into */
    const live_vk_frame_config bare_config = {bind, NULL};
    frame = live_vk_frame_attach(renderer, &textured, &source, &bare_config, error, sizeof error);
    CHECK(frame != NULL && !live_vk_frame_enable_target_draws(frame, error, sizeof error) && strstr(error, "no target set") != NULL);
    live_vk_frame_detach(frame);
    gpu_pgraph_destroy(feedback);
    gpu_pgraph_destroy(only_first);
    gpu_pgraph_destroy(full);
    live_vk_bind_destroy(bind);
    live_vk_texture_destroy(texture_set);
    live_vk_target_destroy(targets);
    live_texture_cache_free(&cache);
}

int main(int argc, char **argv)
{
    unsigned hold_ms = 0u;
    scene_kind chosen = SCENE_BLEND;
    for (int index = 1; index + 1 < argc; index++) {
        if (strcmp(argv[index], "--hold") == 0) {
            hold_ms = (unsigned)atoi(argv[index + 1]);
        } else if (strcmp(argv[index], "--scene") == 0) {
            for (int kind = 0; kind < SCENE_COUNT; kind++) {
                if (strcmp(argv[index + 1], scene_names[kind]) == 0) {
                    chosen = (scene_kind)kind;
                }
            }
        }
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return unavailable(SDL_GetError());
    }
    SDL_Window *window = SDL_CreateWindow("live-vk-frame", (int)WIDTH, (int)HEIGHT,
                                          SDL_WINDOW_VULKAN | (hold_ms != 0u ? 0u : SDL_WINDOW_HIDDEN));
    if (window == NULL) {
        const int result = unavailable(SDL_GetError());
        SDL_Quit();
        return result;
    }
    const char *window_error = NULL;
    gpu_window *renderer = gpu_window_create(window, &window_error);
    if (renderer == NULL) {
        const int result = unavailable(window_error);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return result;
    }
    fill_memory();
    scene opaque = make_scene(chosen);
    scene depth = make_scene(SCENE_DEPTH);
    gpu_pgraph_backend backend = make_backend_for(chosen, &opaque.guest, EVERYTHING);
    source_state state = {opaque.model};
    const live_vk_frame_source source = {source_model, &state};
    char error[256] = "";
    live_vk_frame *frame = live_vk_frame_attach(renderer, &backend, &source, NULL, error, sizeof error);
    CHECK(frame != NULL);
    const float clear[4] = {0.2f, 0.4f, 0.6f, 1.0f};
    if (frame != NULL) {
        const size_t draws = gpu_pgraph_draw_count(opaque.model);
        CHECK(gpu_window_present(renderer, clear));
        CHECK(gpu_window_present(renderer, clear));
        live_vk_frame_stats stats = live_vk_frame_get_stats(frame);
        CHECK(stats.frames == 2u && stats.frames_drawn == 2u && stats.draws_offered == 2u * draws);
        CHECK(stats.draws_refused == 0u && gpu_window_get_stats(renderer).draw_refusals == 0u && stats.frames_refused == 0u);
        const live_vk_stats drawn = live_vk_renderer_stats(live_vk_frame_renderer(frame));
        CHECK(drawn.drawn == 2u * draws && drawn.refused == 0u);
        /* the second present reused the pipelines of the first */
        CHECK(drawn.pipelines_created <= draws);
        CHECK(live_vk_renderer_cache(live_vk_frame_renderer(frame), LIVE_VK_PASS_WINDOW)->hits >= draws);
        /* no model: nothing drawn, not a refusal */
        state.model = NULL;
        CHECK(gpu_window_present(renderer, clear));
        stats = live_vk_frame_get_stats(frame);
        CHECK(stats.frames == 3u && stats.frames_drawn == 2u && gpu_window_get_stats(renderer).draw_refusals == 0u);
        /* a depth tested model: the swapchain pass has no depth, both draws are refused by name, the frame still presents */
        state.model = depth.model;
        CHECK(gpu_window_present(renderer, clear));
        stats = live_vk_frame_get_stats(frame);
        CHECK(stats.draws_refused == 2u && gpu_window_get_stats(renderer).draw_refusals == 1u && stats.frames_refused == 1u);
        const live_pipeline_census *census = live_vk_renderer_census(live_vk_frame_renderer(frame));
        CHECK(census->count == 2u && census->by_stage[LIVE_STAGE_DEVICE] == 2u);
        CHECK(census->count == 2u && strstr(census->refusals[0].reason, "no depth attachment") != NULL);
        state.model = opaque.model;
        for (unsigned waited = 0u; waited < hold_ms; waited += 50u) {
            (void)gpu_window_present(renderer, clear);
            SDL_Delay(50u);
        }
        printf("live vk frame device=%s frames=%llu drawn=%llu refused=%llu\n", gpu_window_device_name(renderer),
               (unsigned long long)stats.frames, (unsigned long long)drawn.drawn, (unsigned long long)stats.draws_refused);
        live_vk_frame_detach(frame);
        test_copies(renderer, &backend, &opaque.guest);
        test_clears(renderer);
        test_textured(renderer, &opaque.guest);
        test_target_draws(renderer, &backend, &opaque.guest);
    }
    gpu_pgraph_destroy(opaque.model);
    gpu_pgraph_destroy(depth.model);
    gpu_window_destroy(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
