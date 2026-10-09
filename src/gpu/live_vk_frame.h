/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T791 (M9), the per-frame draw loop of the SDL window path. live_vk_frame_attach installs a gpu_window frame hook
 * (gpu_window.h, T790) that, inside the swapchain clear pass, resets the live renderer's frame arena and draws every draw of
 * the model the source names: live_pipeline_select, pipeline bind, dynamic state, draw (live_vk_pipeline.h).
 *
 * WHO OWNS WHAT. The caller (the swap replay seam, T790) owns TIMING: the source callback must return the model while its draw
 * list is still the frame's (before gpu_pgraph_begin_frame clears it) and NULL otherwise, and the backend it passes (tables,
 * module loaders, inferences, output groups, combiner) must be the one the swap replay builds. This module owns the loop.
 *
 * ORDER. Within a frame the model's CopyRects events are applied to the target set interleaved with the draws: an event runs
 * before the draw whose index is its `before_draw`, the rest after the last draw. The window pass has no intermediate
 * results of its own, so the order matters only for what a copy writes into and a later draw samples (a render target texture).
 *
 * CLEARS (T828). The model's CLEAR_SURFACE events are applied inside the pass in order with the draws: an event runs before the
 * draw whose index is its `before_draw`, the rest after the last draw (live_vk_renderer_clear, the replay's rules). A colour
 * clear lands on the swapchain image; a clear of depth or stencil is refused by name (the swapchain pass has no depth
 * attachment, a T793 target will carry it) and makes the hook return false. flip_y is a negative viewport (live_vk_pipeline.h).
 *
 * NOT done here, on purpose: draws that test depth or stencil are refused (same reason).
 */
#ifndef TSFP_LIVE_VK_FRAME_H
#define TSFP_LIVE_VK_FRAME_H

#include "live_vk_bind.h"
#include "live_vk_draw.h"
#include "live_vk_pipeline.h"
#include "live_vk_target.h"

typedef struct {
    /* The model whose draws this frame shows, or NULL for none (nothing is drawn, the frame still presents). */
    const gpu_pgraph *(*pgraph)(void *context);
    void *context;
} live_vk_frame_source;

/* Optional collaborators, all owned by the caller and outliving the frame object. */
typedef struct {
    live_vk_bind *textures;      /* NULL: a combiner draw that samples a texture is refused by name */
    live_vk_target_set *targets; /* NULL: the CopyRects events of the model are not applied (counted in `copies_ignored`) */
} live_vk_frame_config;

typedef struct {
    uint64_t frames;         /* hook calls */
    uint64_t frames_drawn;   /* of those, with a model */
    uint64_t frames_refused; /* T847: of the frames with a model, those with at least one refused draw or clear, or a failed submit */
    uint64_t draws_offered;
    uint64_t draws_refused;  /* named in the renderer's census */
    uint64_t clears_offered; /* CLEAR_SURFACE events of the offered models (T828), applied in order with the draws */
    uint64_t clears_refused; /* of those, refused or only partly applied (live_vk_renderer_clear_refusal says why) */
    uint64_t copies_applied; /* CopyRects events handed to live_vk_target_apply_copies */
    uint64_t copies_refused; /* of those, refused by the planner or the device (live_vk_target_stats says why) */
    uint64_t copies_ignored; /* CopyRects events seen with no target set configured */
} live_vk_frame_stats;

typedef struct live_vk_frame live_vk_frame;

/* T860: refused clears grouped by the reason text the clear path gave (the renderer's own or the target provider's). */
#define LIVE_VK_FRAME_CLEAR_REASONS 16u
typedef struct {
    char text[LIVE_PIPELINE_REASON_BYTES];
    uint64_t count;
} live_vk_frame_clear_reason;
size_t live_vk_frame_clear_reason_count(const live_vk_frame *frame, uint64_t *other);
const live_vk_frame_clear_reason *live_vk_frame_clear_reason_at(const live_vk_frame *frame, size_t index);

/* Build the renderer on the window's device and install the hook. NULL with `error` filled when the window has no device,
 * the renderer cannot be built (flip_y without VK_KHR_maintenance1, no Vulkan function) or `window` is NULL. `backend` is copied, its
 * pointers must outlive the returned object. */
live_vk_frame *live_vk_frame_attach(gpu_window *window, const gpu_pgraph_backend *backend,
                                    const live_vk_frame_source *source, const live_vk_frame_config *config, char *error,
                                    size_t error_bytes);
/* Remove the hook (when still ours) and destroy the renderer. NULL is a no-op. */
void live_vk_frame_detach(live_vk_frame *frame);

/* The movie overlay picture of the T760 sink (RGB24), forwarded to the target set's compositor. False without a target set. */
bool live_vk_frame_overlay_submit(live_vk_frame *frame, uint32_t width, uint32_t height, const uint8_t *rgb);

live_vk_renderer *live_vk_frame_renderer(live_vk_frame *frame);
/* T829: from now on draw into the registered T793 target the surface state of each draw names (live_vk_draw.h) instead of the
 * swapchain pass. A separate call, not a config field, so the config's positional initialisers stay valid. Needs `targets` in the
 * config. The CopyRects events then run after the draws before them have been submitted (live_vk_draw_flush). Without this call
 * the draws go to the window pass and a draw that tests depth or stencil is refused. False with `error` filled when there is no
 * target set or the provider cannot be built; true again when already enabled. */
bool live_vk_frame_enable_target_draws(live_vk_frame *frame, char *error, size_t error_bytes);
/* T829: the target draw provider, NULL until live_vk_frame_enable_target_draws. */
live_vk_draw *live_vk_frame_draw(live_vk_frame *frame);
/* T838: run the frame loop now, outside a swapchain pass (the source names the model). Needs live_vk_frame_enable_target_draws; false
 * otherwise or when a draw or clear was refused (named in the renderer's census). The host hook-up calls this at each Swap on the
 * thread that owns the window, the swapchain is presented separately at the modelled vblank. */
bool live_vk_frame_run_targets(live_vk_frame *frame);
live_vk_frame_stats live_vk_frame_get_stats(const live_vk_frame *frame);

/* T998: real report callback after all counted commands complete. Failure leaves guest pending. */
typedef bool (*live_vk_query_report_fn)(void *context, const gpu_pgraph_query *event, uint32_t samples);
bool live_vk_frame_enable_visibility(live_vk_frame *frame, live_vk_query_report_fn report, void *context,
                                     char *error, size_t error_bytes);

#endif
