/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T793 on a real swapchain: a real CopyRects (BACK to FRONT, a plain vkCmdCopyImage, then a staged alpha-forcing blit) and a
 * real Swap present at modelled vblanks through gpu_window_present_pixels, then a movie overlay letterboxed above the front.
 * The frames handed to the swapchain are compared byte for byte with the CPU reference executor of live_target, the front
 * and the overlay frame are written as PNG (`--png PREFIX`, PREFIX-front.png and PREFIX-overlay.png), and `--hold-ms N`
 * keeps re-presenting the overlay frame so an external screenshot of the display can see the window.
 * Exits 77 when SDL has no Vulkan capable window (ctest runs it under SDL's dummy video driver, which never has one).
 */
#include "gpu_png.h"
#include "gpu_window.h"
#include "live_vk_target.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t *picture_rgb;
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

#define WIDTH 320u
#define HEIGHT 240u
#define BACK_DATA 0x1000000u
#define FRONT_DATA 0x2000000u
#define FORMAT_A8R8G8B8 0x00011229u
/* 320x240 linear, pitch 1280: width - 1 | (height - 1) << 12 | (pitch / 64 - 1) << 24 */
#define SIZE_WORD (319u | (239u << 12) | (19u << 24))

typedef struct {
    gpu_window *window;
    uint32_t calls;
    uint8_t *last;
    size_t capacity;
    uint32_t width, height;
} display;

static bool present_to_window(void *context, const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t stride)
{
    display *d = context;
    const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const size_t bytes = (size_t)stride * height;
    if (bytes > d->capacity) {
        d->last = realloc(d->last, bytes);
        d->capacity = bytes;
    }
    memcpy(d->last, rgba, bytes);
    d->width = width;
    d->height = height;
    d->calls++;
    return gpu_window_present_pixels(d->window, rgba, width, height, stride, clear);
}

/* BGRA bytes of the test picture: a gradient, a white marker top left (orientation), a red square, a diagonal. */
static void paint(uint8_t *bgra)
{
    for (uint32_t y = 0; y < HEIGHT; y++) {
        for (uint32_t x = 0; x < WIDTH; x++) {
            uint8_t *p = bgra + ((size_t)y * WIDTH + x) * 4u;
            p[0] = (uint8_t)(x * 255u / (WIDTH - 1u));
            p[1] = (uint8_t)(y * 255u / (HEIGHT - 1u));
            p[2] = (uint8_t)(255u - (x * 255u / (WIDTH - 1u)));
            p[3] = 0x20u;   /* alpha is not shown */
            if (x < 24u && y < 24u) { p[0] = p[1] = p[2] = 0xFFu; }
            if (x >= 200u && x < 280u && y >= 120u && y < 200u) { p[0] = 0x10u; p[1] = 0x10u; p[2] = 0xE0u; }
            if (x == y) { p[0] = p[1] = p[2] = 0xFFu; }
        }
    }
}

/* RGB bytes equal, alpha not compared: the swapchain is opaque, the blit copies the front's alpha, the compositor forces 0xFF. */
static size_t rgb_mismatches(const uint8_t *a, const uint8_t *b, size_t pixels)
{
    size_t wrong = 0u;
    for (size_t i = 0u; i < pixels; i++) {
        wrong += (a[i * 4u] != b[i * 4u] || a[i * 4u + 1u] != b[i * 4u + 1u] || a[i * 4u + 2u] != b[i * 4u + 2u]) ? 1u : 0u;
    }
    return wrong;
}

static bool direct_present(void *context, gpu_window_blit_hook hook, void *hook_context)
{
    return gpu_window_present_blit(context, hook, hook_context);
}

static bool refusing_hook(const gpu_window_native *native, VkCommandBuffer command, VkImage destination, VkExtent2D extent,
                          VkFormat format, void *context)
{
    (void)native; (void)command; (void)destination; (void)extent; (void)format; (void)context;
    return false;
}

static bool capture_frame(gpu_window *window, uint8_t *rgba, size_t capacity, uint32_t *width, uint32_t *height)
{
    return gpu_window_read_capture(window, rgba, capacity, width, height);
}

int main(int argc, char **argv)
{
    const char *png_prefix = NULL;
    uint32_t hold_ms = 0u;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--png") == 0 && i + 1 < argc) png_prefix = argv[++i];
        else if (strcmp(argv[i], "--hold-ms") == 0 && i + 1 < argc) hold_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
        else { fprintf(stderr, "usage: %s [--png PREFIX] [--hold-ms N]\n", argv[0]); return 2; }
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) { printf("SKIP: %s\n", SDL_GetError()); return 77; }
    SDL_Window *window = SDL_CreateWindow("live-vk-present", 640, 480, SDL_WINDOW_VULKAN);
    if (window == NULL) { printf("SKIP: %s\n", SDL_GetError()); SDL_Quit(); return 77; }
    const char *error = NULL;
    gpu_window *renderer = gpu_window_create(window, &error);
    if (renderer == NULL) { printf("SKIP: %s\n", error != NULL ? error : "no gpu window"); SDL_DestroyWindow(window); SDL_Quit(); return 77; }
    printf("live_vk_present device: %s\n", gpu_window_device_name(renderer));
    gpu_window_native native;
    live_vk_target_device description;
    PFN_vkGetInstanceProcAddr gipa = NULL;
    SDL_FunctionPointer sdl_gipa = SDL_Vulkan_GetVkGetInstanceProcAddr();
    memcpy(&gipa, &sdl_gipa, sizeof gipa);
    CHECK(gpu_window_get_native(renderer, &native));
    CHECK(live_vk_target_device_from_native(&native, gipa, &description));
    live_vk_target_set *set = live_vk_target_create(&description, 0u, NULL);
    CHECK(set != NULL);
    live_target_registry *reference = live_target_registry_create();
    CHECK(live_vk_target_register(set, BACK_DATA, FORMAT_A8R8G8B8, SIZE_WORD, NULL) == LIVE_VK_OK);
    CHECK(live_vk_target_register(set, FRONT_DATA, FORMAT_A8R8G8B8, SIZE_WORD, NULL) == LIVE_VK_OK);
    CHECK(live_target_register(reference, BACK_DATA, FORMAT_A8R8G8B8, SIZE_WORD, true) == LIVE_TARGET_OK);
    CHECK(live_target_register(reference, FRONT_DATA, FORMAT_A8R8G8B8, SIZE_WORD, true) == LIVE_TARGET_OK);
    const size_t bytes = (size_t)WIDTH * HEIGHT * 4u;
    paint(live_target_pixels(reference, BACK_DATA));
    CHECK(live_vk_target_upload(set, BACK_DATA, live_target_pixels(reference, BACK_DATA), bytes) == LIVE_VK_OK);
    display shown = {.window = renderer};
    live_vk_target_set_present(set, present_to_window, &shown);

    /* the real CopyRects: the whole back buffer to the front (vkCmdCopyImage), then a 64x64 stamp that forces alpha (staged) */
    const live_target_blit whole = {BACK_DATA, FRONT_DATA, 0xA, 1280, 1280, 0, 0, 0, 0, WIDTH, HEIGHT, LIVE_BLIT_OPERATION_SRCCOPY};
    const live_target_blit stamp = {BACK_DATA, FRONT_DATA, 0x7, 1280, 1280, 0, 0, 240, 150, 64, 64, LIVE_BLIT_OPERATION_SRCCOPY};
    live_target_blit_plan plan;
    CHECK(live_vk_target_blit(set, &whole, &plan, NULL) == LIVE_VK_OK && plan.copy_image_ok);
    CHECK(live_vk_target_blit(set, &stamp, &plan, NULL) == LIVE_VK_OK && !plan.copy_image_ok);
    CHECK(live_target_apply_blit(reference, &whole, 0u, NULL, NULL) == LIVE_TARGET_OK);
    CHECK(live_target_apply_blit(reference, &stamp, 0u, NULL, NULL) == LIVE_TARGET_OK);
    uint8_t *readback = malloc(bytes);
    CHECK(live_vk_target_readback(set, FRONT_DATA, readback, bytes) == LIVE_VK_OK);
    CHECK(memcmp(readback, live_target_pixels(reference, FRONT_DATA), bytes) == 0);
    free(readback);

    /* the real Swap: submitted at vblank 10, nothing before it, shown at vblank 11 */
    d3d8_frame_record record;
    memset(&record, 0, sizeof record);
    record.number = 1u; record.interval = 1u; record.vblank = 10u; record.data = FRONT_DATA;
    record.format_word = FORMAT_A8R8G8B8; record.size_word = SIZE_WORD;
    live_present_frame frame;
    CHECK(live_vk_target_vblank(set, 9u, &frame) && frame.layer_count == 0u);
    CHECK(live_vk_target_present_submit(set, &record) == LIVE_TARGET_OK);
    CHECK(live_vk_target_vblank(set, 10u, &frame) && frame.layer_count == 0u);
    CHECK(live_vk_target_vblank(set, 11u, &frame) && frame.front_new && frame.layer_count == 1u);
    CHECK(shown.width == WIDTH && shown.height == HEIGHT);
    {
        const uint8_t *front = live_target_pixels(reference, FRONT_DATA);
        bool equal = true;
        for (size_t i = 0; i < (size_t)WIDTH * HEIGHT; i++) {
            equal = equal && shown.last[i * 4] == front[i * 4 + 2] && shown.last[i * 4 + 1] == front[i * 4 + 1] &&
                    shown.last[i * 4 + 2] == front[i * 4] && shown.last[i * 4 + 3] == 0xFFu;
        }
        CHECK(equal);   /* byte for byte the CPU reference, alpha not shown */
        CHECK(shown.last[0] == 0xFFu && shown.last[(150 * WIDTH + 240) * 4 + 3] == 0xFFu);
    }
    if (png_prefix != NULL) {
        char path[512];
        snprintf(path, sizeof path, "%s-front.png", png_prefix);
        CHECK(gpu_png_write_rgba(path, shown.last, WIDTH, HEIGHT, WIDTH * 4u));
    }
    /* the movie overlay above the front (the T760 sink's RGB24 picture, 16:9, letterboxed) */
    {
        enum { PW = 160, PH = 90 };
        uint8_t *picture = malloc((size_t)PW * PH * 3u);
        picture_rgb = picture;
        for (uint32_t y = 0; y < PH; y++) {
            for (uint32_t x = 0; x < PW; x++) {
                uint8_t *p = picture + ((size_t)y * PW + x) * 3u;
                p[0] = (uint8_t)(255u * x / (PW - 1u));
                p[1] = (uint8_t)(((x / 20u + y / 15u) & 1u) != 0u ? 230u : 40u);
                p[2] = (uint8_t)(255u * y / (PH - 1u));
            }
        }
        live_vk_target_overlay_submit(set, PW, PH, picture);
        CHECK(live_vk_target_vblank(set, 12u, &frame));
        CHECK(frame.layer_count == 2u && frame.layers[1] == LIVE_LAYER_OVERLAY && frame.front_held);
        uint8_t *expected = malloc(bytes);
        live_vk_target_compose(live_target_pixels(reference, FRONT_DATA), WIDTH * 4u, picture, PW, PH, WIDTH, HEIGHT, expected);
        CHECK(memcmp(expected, shown.last, bytes) == 0);
        /* the picture covers rows 30..209 (320x180 fit), the front shows in the bars above and below */
        CHECK(shown.last[(10 * WIDTH + 100) * 4] == live_target_pixels(reference, FRONT_DATA)[(10 * WIDTH + 100) * 4 + 2]);
        CHECK(shown.last[(40 * WIDTH + 0) * 4 + 1] == 230u || shown.last[(40 * WIDTH + 0) * 4 + 1] == 40u);
        free(expected);
        /* picture stays allocated for the T849 block below */
    }
    if (png_prefix != NULL) {
        char path[512];
        snprintf(path, sizeof path, "%s-overlay.png", png_prefix);
        CHECK(gpu_png_write_rgba(path, shown.last, WIDTH, HEIGHT, WIDTH * 4u));
    }
    for (uint32_t waited = 0u; waited < hold_ms; waited += 50u) {
        SDL_PumpEvents();
        CHECK(live_vk_target_vblank(set, 13u + waited / 50u, &frame));
        SDL_Delay(50u);
    }

    /* T849: the swapchain pre-pass blit against the readback route, both through the real swapchain, compared on the captured
     * final image (640x480 window, 320x240 front: the front scales by 2, the 160x90 overlay by 4, integers, where nearest
     * blit and nearest CPU scaling agree exactly). Overlay frame first, then front only. */
    {
        CHECK(gpu_window_can_blit(renderer));
        const bool capturing = gpu_window_set_capture(renderer, true);
        CHECK(capturing);
        if (capturing) {
            const size_t capacity = 640u * 480u * 4u;
            uint8_t *readback_ov = malloc(capacity), *blit_ov = malloc(capacity), *readback_front = malloc(capacity),
                    *blit_front = malloc(capacity);
            uint32_t width = 0u, height = 0u;
            live_vk_target_overlay_submit(set, 160u, 90u, picture_rgb);   /* the picture of the block above, re-latched */
            CHECK(live_vk_target_vblank(set, 100u, &frame) && frame.layer_count == 2u);
            CHECK(capture_frame(renderer, readback_ov, capacity, &width, &height) && width == 640u && height == 480u);
            live_vk_target_set_direct(set, direct_present, renderer);
            const uint64_t window_before = gpu_window_get_stats(renderer).blit_presented;
            CHECK(live_vk_target_vblank(set, 101u, &frame) && frame.layer_count == 2u);
            CHECK(gpu_window_get_stats(renderer).blit_presented == window_before + 1u);
            CHECK(capture_frame(renderer, blit_ov, capacity, &width, &height));
            CHECK(rgb_mismatches(readback_ov, blit_ov, 640u * 480u) == 0u);
            /* a NEW picture is uploaded again (other size and aspect), blit and readback agree and differ from the first picture */
            {
                /* a portrait 40x60 picture (pillarboxed: left 80 of the 320 canvas, scale 4), a different texture size */
                uint8_t *inverted = malloc(40u * 60u * 3u);
                for (size_t i = 0u; i < 40u * 60u * 3u; i++) inverted[i] = (uint8_t)(255u - picture_rgb[i * 5u]);
                live_vk_target_overlay_submit(set, 40u, 60u, inverted);
                CHECK(live_vk_target_vblank(set, 104u, &frame) && frame.layer_count == 2u);
                uint8_t *second_blit = malloc(capacity), *second_readback = malloc(capacity);
                CHECK(capture_frame(renderer, second_blit, capacity, &width, &height));
                live_vk_target_set_direct(set, NULL, NULL);
                CHECK(live_vk_target_vblank(set, 105u, &frame) && frame.layer_count == 2u);
                CHECK(capture_frame(renderer, second_readback, capacity, &width, &height));
                CHECK(rgb_mismatches(second_blit, second_readback, 640u * 480u) == 0u);
                CHECK(rgb_mismatches(second_blit, blit_ov, 640u * 480u) > 10000u);
                live_vk_target_set_direct(set, direct_present, renderer);
                free(second_blit); free(second_readback); free(inverted);
            }
            live_vk_target_overlay_clear(set);
            CHECK(live_vk_target_vblank(set, 106u, &frame) && frame.layer_count == 1u);
            CHECK(capture_frame(renderer, blit_front, capacity, &width, &height));
            live_vk_target_set_direct(set, NULL, NULL);
            CHECK(live_vk_target_vblank(set, 107u, &frame) && frame.layer_count == 1u);
            CHECK(capture_frame(renderer, readback_front, capacity, &width, &height));
            CHECK(rgb_mismatches(readback_front, blit_front, 640u * 480u) == 0u);
            CHECK(rgb_mismatches(blit_front, blit_ov, 640u * 480u) > 10000u);   /* the overlay frame differs from the front only */
            /* the blit frame is not a copy of the readback frame by accident: the front corner marker is white, the overlay rows are not */
            CHECK(blit_front[0] == 0xFFu && blit_front[1] == 0xFFu && blit_front[2] == 0xFFu);
            /* the channel order of the captured swapchain image: the red square of the front is (0xE0, 0x10, 0x10) in RGB */
            CHECK(blit_front[(300 * 640 + 440) * 4] == 0xE0u && blit_front[(300 * 640 + 440) * 4 + 2] == 0x10u);
            /* read_pixel / capture of the blit route compose the last vblank's layers on demand: the front, 320x240 */
            {
                const uint8_t *last = NULL;
                uint32_t last_width = 0u, last_height = 0u;
                CHECK(live_vk_target_compose_last(set, &last, &last_width, &last_height) && last_width == WIDTH && last_height == HEIGHT);
                uint8_t *expected = malloc(bytes);
                live_vk_target_compose(live_target_pixels(reference, FRONT_DATA), WIDTH * 4u, NULL, 0u, 0u, WIDTH, HEIGHT, expected);
                CHECK(last != NULL && memcmp(expected, last, bytes) == 0);
                free(expected);
            }
            /* a hook that refuses: the acquired image is still presented (black), counted, the call reports false */
            {
                const uint64_t refusals = gpu_window_get_stats(renderer).blit_refusals;
                CHECK(!gpu_window_present_blit(renderer, refusing_hook, NULL));
                CHECK(gpu_window_get_stats(renderer).blit_refusals == refusals + 1u);
                CHECK(capture_frame(renderer, blit_ov, capacity, &width, &height) && blit_ov[0] == 0u && blit_ov[(240 * 640 + 320) * 4 + 1] == 0u);
            }
            if (png_prefix != NULL) {
                char path[512];
                snprintf(path, sizeof path, "%s-blit-overlay.png", png_prefix);
                CHECK(gpu_png_write_rgba(path, blit_ov, 640u, 480u, 640u * 4u));
                snprintf(path, sizeof path, "%s-blit-front.png", png_prefix);
                CHECK(gpu_png_write_rgba(path, blit_front, 640u, 480u, 640u * 4u));
            }
            /* a front the blit cannot read (16 bit) falls back to the readback route while the direct route is set */
            live_vk_target_set_direct(set, direct_present, renderer);
            const live_vk_target_stats before = live_vk_target_stats_get(set);
            d3d8_frame_record small;
            memset(&small, 0, sizeof small);
            small.number = 2u; small.interval = 1u; small.vblank = 110u; small.data = 0x3000000u;
            small.format_word = 0x00011129u; small.size_word = (31u | (7u << 12));
            CHECK(live_vk_target_present_submit(set, &small) == LIVE_TARGET_OK);
            CHECK(live_vk_target_vblank(set, 111u, &frame));
            const live_vk_target_stats after = live_vk_target_stats_get(set);
            CHECK(after.direct_fallbacks == before.direct_fallbacks + 1u);
            CHECK(after.readback_frames == before.readback_frames + 1u && after.blit_frames == before.blit_frames);
            const live_vk_target_stats now = live_vk_target_stats_get(set);
            printf("blit frames=%llu readback frames=%llu fallbacks=%llu failures=%llu overlay uploads=%llu\n",
                   (unsigned long long)now.blit_frames, (unsigned long long)now.readback_frames,
                   (unsigned long long)now.direct_fallbacks, (unsigned long long)now.direct_failures,
                   (unsigned long long)now.overlay_uploads);
            CHECK(now.direct_failures == 0u && now.blit_frames == 3u && now.overlay_uploads >= 1u);
            CHECK(gpu_window_get_stats(renderer).blit_refusals == 1u);   /* only the deliberate refusal */
            free(readback_ov); free(blit_ov); free(readback_front); free(blit_front);
        }
    }
    const live_vk_target_stats stats = live_vk_target_stats_get(set);
    const gpu_window_stats window_stats = gpu_window_get_stats(renderer);
    printf("copy_image=%llu staged=%llu presented=%llu readbacks=%llu window presented=%llu failures=%llu\n",
           (unsigned long long)stats.copy_image_blits, (unsigned long long)stats.staged_blits,
           (unsigned long long)stats.frames_presented, (unsigned long long)stats.front_readbacks,
           (unsigned long long)window_stats.presented, (unsigned long long)window_stats.present_failures);
    CHECK(stats.copy_image_blits == 1u && stats.staged_blits == 1u && stats.device_failures == 0u);
    CHECK(window_stats.present_failures == 0u && window_stats.presented == stats.frames_presented + 1u);   /* +1: the refusing hook frame */
    CHECK(stats.frames_presented == stats.blit_frames + stats.readback_frames && stats.first_frame_ns != 0u && stats.last_frame_ns >= stats.first_frame_ns);
    free((void *)picture_rgb);
    free(shown.last);
    live_target_registry_destroy(reference);
    live_vk_target_destroy(set);
    gpu_window_destroy(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    printf("live_vk_present: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
