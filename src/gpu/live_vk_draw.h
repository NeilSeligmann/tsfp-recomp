/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T829 (M9): live draws INTO T793 target images. It is the per draw target provider (`live_vk_target_hook`, live_vk_pipeline.h)
 * that sends a draw to the registered T793 target the NV2A surface state names, in the BGRA8 + D32S8 pass kind
 * (LIVE_VK_PASS_TARGET) whose colour layout is GENERAL, the layout of every target image, so a finished draw leaves its target
 * sampleable (T792 sampling in place, `live_vk_target_texture_view`) and blittable (T793 CopyRects) with no transition.
 *
 * WHICH TARGET. From the draw's snapshot: SURFACE_FORMAT 0x0208 (colour nibble 8 A8R8G8B8 only: the replay's output state decode
 * refuses every other colour format, so an X8R8G8B8 state could not be planned either, the zeta nibble 0 or 2 and the pitch layout
 * type 1 only), SURFACE_PITCH 0x020C (low 16 bits, must be
 * the registered target's pitch) and SURFACE_COLOR_OFFSET 0x0210 masked to 28 bits as the target's Data word (INFERRED: the offset
 * the SetRenderTarget packet writes is the surface header's Data word, the same name T578's blit offsets use, so a CopyRects and a
 * draw name one target alike). A target the set does not hold, a word the stream never wrote, a pitch or a format that disagrees
 * with the registration is a NAMED refusal (census stage "device"), never a draw into some other image.
 *
 * ORDER AND SUBMISSION. A target draw records into its own command buffer inside the pass, so draws of one target batch into one
 * RUN: a run opens at the first draw of a target and CLOSES (end pass, submit, wait, bump the target generation) when the next
 * draw names another target, when `live_vk_draw_flush` is called (the frame calls it before every CopyRects event and after the
 * last draw, which is how the T793 blit order stays interleaved with the draws) or at the end of the frame. Every submit waits, so
 * an image a later run samples or blits is complete. A frame's per target depth and stencil image (D32S8, created on first use,
 * the size of the colour target) is cleared to 1.0 and 0 at the first draw into the target of each frame, as the replay's pass
 * starts, before the draws of the frame (a CLEAR_SURFACE event applied through `live_vk_draw_apply_clear` follows that reset, in order). The zeta offset is not used: one depth image per colour target.
 *
 * Nothing here is installed by default (live_vk_frame_config.draw_into_targets).
 */
#ifndef TSFP_LIVE_VK_DRAW_H
#define TSFP_LIVE_VK_DRAW_H

#include "live_vk_pipeline.h"
#include "live_vk_target.h"

typedef struct {
    uint64_t draws;          /* draws handed a target */
    uint64_t runs;           /* command buffers submitted */
    uint64_t depth_clears;   /* per frame depth and stencil resets */
    uint64_t refused;        /* draws whose surface state named no usable target */
    uint64_t clears;         /* CLEAR_SURFACE events applied to a target */
    uint64_t clears_refused; /* events with no usable target or refused by live_vk_renderer_clear (its census names it) */
    uint64_t device_failures;
    uint64_t snapshots;      /* T1206: feedback snapshot copies made */
} live_vk_draw_stats;

typedef struct live_vk_draw live_vk_draw;

/* `renderer`'s target pass (live_vk_renderer_target_pass) is what the framebuffers are built against; `targets` supplies the
 * images. Both stay owned by the caller and outlive the provider. NULL with `error` filled on a missing function or argument. */
live_vk_draw *live_vk_draw_create(const live_vk_device *device, live_vk_renderer *renderer, live_vk_target_set *targets,
                                  char *error, size_t error_bytes);
void live_vk_draw_destroy(live_vk_draw *draw);
/* For live_vk_renderer_set_target_hook. The pointer's `context` is the provider. */
live_vk_target_hook live_vk_draw_hook(live_vk_draw *draw);
/* Apply clear event `clear_index` of `model` to the target of the draw it precedes (the draw `before_draw` names, the last draw for
 * an event after it): the events carry no surface words, so the next draw's snapshot is the surface the clear is for. The clear runs
 * inside that target's run (live_vk_renderer_clear, T828), after the per frame depth reset and in order with the draws. False with
 * `error` filled when no target is named (a frame with no draw, the refusals above) or the renderer refuses the clear. */
bool live_vk_draw_apply_clear(live_vk_draw *draw, const gpu_pgraph *model, size_t clear_index, char *error, size_t error_bytes);
/* A new frame: the next draw into each target resets its depth and stencil. Closes a run still open. */
void live_vk_draw_begin_frame(live_vk_draw *draw);
/* Close the open run (submit and wait). True when nothing was open or the submit succeeded. */
bool live_vk_draw_flush(live_vk_draw *draw);
/* T1264: waits for the runs submitted without a wait and frees their command buffers (no-op when none is in flight). Call before
 * anything the GPU may still read is rewritten (the frame start does, before the arena and descriptor pool reset). */
bool live_vk_draw_sync(live_vk_draw *draw);
/* T1264: true restores the old submit + vkQueueWaitIdle after every run (--live-present-sync, for bisecting). */
void live_vk_draw_set_sync_each_run(bool enable);
/* The texture id (live_vk_target_texture_id) of the target the last accepted draw rendered into, 0 before any. */
uint32_t live_vk_draw_current_texture_id(const live_vk_draw *draw);
/* T1206: a draw whose texture stage samples the very target it renders into (xemu copies the surface into a separate texture
 * image at bind time, so the stage reads the target as it stood BEFORE the draw, hw/xbox/nv2a/pgraph/vk/texture.c
 * copy_surface_to_texture). Closes the open run, copies the target of `texture_id` into its snapshot image, reopens the run, and
 * returns the snapshot's view (layout GENERAL). Several stages of one draw share one copy. False with `error` filled when the
 * target is not the open run's or a Vulkan call failed. */
bool live_vk_draw_snapshot(live_vk_draw *draw, size_t draw_index, uint32_t texture_id, VkImageView *view, char *error,
                           size_t error_bytes);
/* The command buffer of the open run (VK_NULL_HANDLE when none): it changes when live_vk_draw_snapshot reopens the run. */
VkCommandBuffer live_vk_draw_current_command(const live_vk_draw *draw);
live_vk_draw_stats live_vk_draw_stats_get(const live_vk_draw *draw);

#endif
