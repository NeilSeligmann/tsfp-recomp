/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gpu_phase_timing.h"
#include "gpu_window.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int unavailable(const char *why)
{
    const char *required = getenv("TSFP_TEST_GPU_WINDOW_REQUIRED");
    fprintf(stderr, "%s: %s\n", required != NULL ? "FAIL" : "SKIP", why != NULL ? why : "no detail");
    return required != NULL ? 1 : 77;
}

typedef struct { gpu_window *renderer; unsigned calls; bool refuse; } completion_state;
static bool frame_completed(void *context)
{
    completion_state *state = context;
    state->calls++;
    return gpu_window_get_stats(state->renderer).presented == state->calls && !state->refuse;
}

static bool record_frame(const gpu_window_native *native, void *context)
{
    unsigned *calls = context;
    if (native->device == VK_NULL_HANDLE || native->command_buffer == VK_NULL_HANDLE ||
        native->render_pass == VK_NULL_HANDLE || native->queue == VK_NULL_HANDLE) return false;
    (*calls)++;
    return true;
}

int main(void)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) return unavailable(SDL_GetError());
    SDL_Window *window = SDL_CreateWindow("gpu-window-test", 96, 64, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (window == NULL) {
        const int result = unavailable(SDL_GetError());
        SDL_Quit();
        return result;
    }
    const char *error = NULL;
    gpu_window *renderer = gpu_window_create(window, &error);
    if (renderer == NULL) {
        const int result = unavailable(error);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return result;
    }
    const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const uint8_t pixels[16] = {255u, 0u, 0u, 255u, 0u, 255u, 0u, 255u,
                                0u, 0u, 255u, 255u, 255u, 255u, 255u, 255u};
    gpu_window_feed_method(renderer, 0x17FCu, 1u);
    unsigned hook_calls = 0u;
    gpu_window_set_frame_hook(renderer, record_frame, &hook_calls);
    completion_state completed = {.renderer=renderer};
    gpu_window_set_complete_hook(renderer,frame_completed,&completed);
    if (!gpu_window_present(renderer, clear) ||
        !gpu_window_present_pixels(renderer, pixels, 2u, 2u, 8u, clear)) {
        gpu_window_destroy(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    const gpu_window_stats stats = gpu_window_get_stats(renderer);
    bool passed = completed.calls == 2u && stats.frames == 2u && stats.presented == 2u &&
                        stats.draw_packets == 1u && stats.draw_methods == 1u && hook_calls == 1u &&
                        stats.draw_refusals == 0u;
    completed.refuse = true;
    passed = !gpu_window_present(renderer,clear) && completed.calls == 3u &&
             gpu_window_get_stats(renderer).present_failures == 1u && passed;
    gpu_window_set_complete_hook(renderer,NULL,NULL);
    /* T1262: the present does not end in vkQueueWaitIdle (async, the default), and --live-present-sync restores it. Both route the same
     * pixels: six frames of a flat colour each are presented and read back from the capture buffer, which waits for the submit fence. */
    for (unsigned mode = 0u; mode < 2u; mode++) {
        gpu_window_set_present_sync(mode == 1u);
        memset(&gpu_phase_global, 0, sizeof gpu_phase_global);
        const bool capturing = gpu_window_set_capture(renderer, true);
        for (unsigned frame = 0u; frame < 6u; frame++) {
            uint8_t flat[16];
            for (unsigned texel = 0u; texel < 4u; texel++) {
                flat[texel * 4u] = (uint8_t)(frame * 40u + 10u);
                flat[texel * 4u + 1u] = (uint8_t)(250u - frame * 40u);
                flat[texel * 4u + 2u] = (uint8_t)(frame * 7u + 1u);
                flat[texel * 4u + 3u] = 255u;
            }
            passed = gpu_window_present_pixels(renderer, flat, 2u, 2u, 8u, clear) && passed;
            uint8_t read[96u * 64u * 4u];
            uint32_t width = 0u, height = 0u;
            if (capturing) {
                const bool read_ok = gpu_window_read_capture(renderer, read, sizeof read, &width, &height);
                passed = read_ok && width == 96u && height == 64u && read[0] == flat[0] && read[1] == flat[1] && read[2] == flat[2] && passed;
            }
        }
        const uint64_t idle = atomic_load(&gpu_phase_global.calls[GPU_PHASE_PRESENT_IDLE]);
        passed = atomic_load(&gpu_phase_global.calls[GPU_PHASE_PRESENT_QUEUE]) == 6u &&
                 atomic_load(&gpu_phase_global.calls[GPU_PHASE_PRESENT_FENCE_WAIT]) >= 6u && idle == (mode == 1u ? 6u : 0u) && passed;
        if (idle != (mode == 1u ? 6u : 0u)) fprintf(stderr, "mode %u: %llu wait idles\n", mode, (unsigned long long)idle);
    }
    gpu_window_set_present_sync(false);
    /* The nearest neighbour mapping of a 2x2 picture onto the 96x64 window: columns below 48 and rows below 32 take texel 0 and 0. */
    if (gpu_window_set_capture(renderer, true)) {
        static uint8_t read[96u * 64u * 4u];
        uint32_t width = 0u, height = 0u;
        passed = gpu_window_present_pixels(renderer, pixels, 2u, 2u, 8u, clear) &&
                 gpu_window_read_capture(renderer, read, sizeof read, &width, &height) && passed;
        const struct { uint32_t x, y, texel; } probes[] = {{0u, 0u, 0u}, {47u, 31u, 0u}, {48u, 0u, 1u}, {95u, 31u, 1u},
                                                            {0u, 32u, 2u}, {47u, 63u, 2u}, {48u, 32u, 3u}, {95u, 63u, 3u}};
        for (size_t probe = 0u; probe < sizeof probes / sizeof probes[0]; probe++) {
            const uint8_t *got = read + ((size_t)probes[probe].y * 96u + probes[probe].x) * 4u;
            const uint8_t *want = pixels + probes[probe].texel * 4u;
            if (got[0] != want[0] || got[1] != want[1] || got[2] != want[2]) {
                fprintf(stderr, "scale probe %zu (%u,%u): got %u,%u,%u\n", probe, probes[probe].x, probes[probe].y, got[0], got[1], got[2]);
                passed = false;
            }
        }
    }
    printf("gpu window device=%s frames=%llu presents=%llu methods=%llu upload=%s\n",
           gpu_window_device_name(renderer), (unsigned long long)stats.frames,
           (unsigned long long)stats.presented, (unsigned long long)stats.draw_methods,
           passed ? "passed" : "failed");
    gpu_window_destroy(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return passed ? 0 : 1;
}
