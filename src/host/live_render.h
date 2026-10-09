/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * T838 (M9 host hook-up): the live Vulkan renderer inside the host. OPT-IN (--gpu-live), INFERRED until validated, SDL3 builds only.
 *
 * WHO OWNS WHAT.
 *   The --present window sink (present_sink.c) keeps the window and ALL its pacing and queue logic. With live_render_ops installed it
 *   creates an SDL_WINDOW_VULKAN window on its presenter thread and hands that thread's output stage to this module: the presenter
 *   thread creates the gpu_window (instance, device, swapchain), the texture set, the T793 target set and every other Vulkan object, and
 *   is the only thread that uses them (T815). The guest thread never touches Vulkan: each hook marshals one synchronous job to the
 *   presenter thread (present_video_sink_run), so the model, the SetTexture history and guest memory are read while the guest is
 *   blocked, exactly the consistency the swap replay has.
 *
 * DATA FLOW.
 *   Swap (guest thread) -> d3d8_swap_replay decodes the frame -> live model hook -> [presenter thread] live_vk_frame_run_targets: every
 *   draw into its registered T793 target image, CopyRects applied in order, clears, refusals named in the census.
 *   SetRenderTarget -> live target hook -> target registered (header words) on the presenter thread.
 *   Swap -> d3d8_present SECOND observer -> live_vk_target_present_observer (normal schedule), or a timestamped immutable front
 *   snapshot in playback mode. Modelled vblank / movie picture / expose -> presenter thread -> live_vk_target_vblank: in playback
 *   mode it releases only a front whose modelled timestamp the audio media clock has reached, composes the movie overlay on top
 *   (T793 compositor), then calls gpu_window_present_pixels. Movie pictures use the sink's audio-master queue.
 *
 * INFERRED: the overlay is dropped once three title frames with draws arrived with no new picture and no picture waits in the sink's
 * queue or timestamped fronts; the hardware overlay enable is not modelled. Playback fronts are snapshotted by CPU readback so guest
 * writes cannot replace a frame before its media timestamp; this also means playback currently uses the readback route even with
 * --gpu-live-blit enabled.
 */
#ifndef TSFP_HOST_LIVE_RENDER_H
#define TSFP_HOST_LIVE_RENDER_H

#include "present_sink.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    bool inferred; /* --gpu-live-inferred: INFERRED texture formats, sampler states and byte format blits */
    uint32_t pipeline; /* --live-pipeline N (T1246): frames the presenter may run behind the guest, 0 = serial */
    bool blit; /* T849/T1267: present with the swapchain pre-pass blit (the default, --live-readback turns it off), the readback route stays the fallback */
    const char *pipeline_cache_dir; /* --live-pipeline-cache DIR (T1247): the VkPipelineCache file lives here, NULL off */
    const char *frame_hash_path; /* --live-frame-hash FILE (T1246): a sha256 line per frame handed to the window, NULL off */
    uint32_t blit_verify; /* --live-blit-verify N (T1267): every Nth vblank also present the readback route and compare it with the blit, 0 off */
    bool present_sync; /* --live-present-sync (T1262): wait for the queue to go idle after every present, the pre T1262 behaviour, for bisecting */
} live_render_config;

/* The ops for present_video_set_live_ops (call before present_video_sink_open). */
present_live_ops live_render_ops(const live_render_config *config);
/* After the sink is open AND d3d8_swap_replay_enable succeeded with live_only: attach the frame loop, the swap replay hooks and the
 * present observer. False with `error` filled. */
bool live_render_attach(present_video_sink *sink, char *error, size_t error_bytes);
/* T1247: the module maker the swap replay calls on a missing module (install it with d3d8_swap_replay_set_live_module_maker, context the
 * live_module_maker). It translates the other modules the frame in hand is missing in parallel with the one asked for, then makes it. */
bool live_render_make_module(void *maker, bool fragment, const char *name, const uint8_t *bytes, size_t byte_count, char *error,
                             size_t error_bytes);
/* The guest side of a movie picture (counts it for the overlay expiry). Any thread. */
void live_render_note_picture(void);
/* Remove the hooks, destroy the frame loop (on the presenter thread). Call before d3d8_swap_replay_disable and before the sink closes. */
void live_render_detach(void);

typedef struct {
    bool attached;
    char device[128];
    unsigned long long title_frames, title_frames_with_draws, frames_failed, frames_refused;
    unsigned long long draws_offered, draws_drawn, draws_refused;
    unsigned long long clears_offered, clears_refused, copies_applied, copies_refused;
    unsigned long long target_runs, target_draws_refused;
    unsigned long long window_acquire_failures, window_present_failures, swapchain_recreations; /* T1339 */
    unsigned long long arena_peak, arena_capacity, arena_blocks, frames_with_refusals, first_refused_frame, most_refused_in_frame; /* T1339 */
    unsigned long long presents, black_frames, overlay_composed, present_failures, overlay_cleared;
    unsigned long long window_frames; /* gpu_window_present_pixels calls that reached the swapchain */
    unsigned long long targets_registered, targets_refused, observer_calls;
    unsigned long long front_events_submitted, front_events_refused, front_events_released;
    unsigned long long media_snapshots, media_snapshot_drops, media_frames_released, media_pending, media_direct_fallbacks;
    /* T849 */
    bool blit_requested, blit_active; /* --gpu-live-blit asked for / the window could do the pre-pass blit */
    unsigned long long blit_frames, readback_frames, blit_fallbacks, blit_failures, vblanks;
    /* T1267 */
    unsigned long long verify_samples, verify_mismatch_frames, verify_pixels_compared, verify_pixels_differing, verify_alpha_differing, verify_skipped, verify_nonblack_pixels;
    unsigned verify_max_delta;
    long long verify_first_mismatch_vblank;
    bool verify_on;
    unsigned long long query_reports, query_reports_refused, query_nonzero_reports; /* T998 */
    unsigned long long texture_input_misses; /* T1246 */
    /* T1247 */
    unsigned long long pipelines_created, pipeline_create_calls, pipeline_cache_loaded_bytes, pipeline_cache_saved_bytes, pipeline_cache_saves;
    double pipeline_create_ms, pipeline_create_worst_ms;
    char pipeline_cache_file[512];
    double wall_seconds;  /* first to last presented frame, monotonic clock, presenter thread */
    double vblank_ms_mean, vblank_ms_max; /* cost inside live_vk_target_vblank per vblank */
} live_render_report_data;
live_render_report_data live_render_report_get(void);
/* The stop report lines: counters, then the refusal census by named reason. */
void live_render_report_print(FILE *out);

#endif
