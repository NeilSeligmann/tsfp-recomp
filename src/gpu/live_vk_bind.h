/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T791 (M9): the texture bridge between the live draw renderer (live_vk_pipeline.h) and the T792 texture path
 * (live_texture.h decode and cache, live_vk_texture.h VkImages and samplers). Two seams come out of it:
 *
 *   PLANNING   gpu_pgraph_backend.resolve_texture (`live_vk_bind_backend`): per draw and stage, the stage's texture binding
 *              becomes a gpu_combiner_texture for gpu_combiner_plan_build. A stage that cannot be sampled exactly carries a NAMED
 *              refusal that the plan prints only when the combiner reads the stage (no stand-in image is ever made).
 *   BINDING    live_vk_texture_hook (`live_vk_bind_hook`): at draw time the same binding is looked up again, uploaded when the
 *              guest rewrote it (live_vk_texture_bind, a synchronous one time submit on its own command buffer, legal inside the
 *              swapchain pass) and handed to the renderer as a view and sampler it writes into its own descriptor set 0,
 *              binding 2 + stage (the combiner modules' interface, gpu_vsh_draw.h).
 *
 * THE BINDING SOURCE. Which texture header a stage holds at a draw is the title's SetTexture history (the swap replay's
 * binding_at), not PGRAPH state. The host supplies it through `live_vk_bind_source.binding`. The sampler words ARE PGRAPH state
 * (0x1B08 + 0x40 stage address, 0x1B14 + 0x40 stage filter) and are read from the draw's snapshot.
 *
 * GATING. A sampler state live_vk_texture.h marks INFERRED (not xemu measured) is refused unless the backend allows
 * GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING, so the default stays the replay's: refuse.
 */
#ifndef TSFP_LIVE_VK_BIND_H
#define TSFP_LIVE_VK_BIND_H

#include "live_texture.h"
#include "live_texture_inputs.h"
#include "live_vk_pipeline.h"
#include "live_vk_target.h"
#include "live_vk_texture.h"

typedef struct {
    /* The binding of `stage` at `draw`. False with `*refusal` a stable string for a stage the title never bound or left
     * unbound (the stage then has no texture, named). */
    bool (*binding)(void *context, size_t draw, uint32_t stage, live_texture_binding *out, const char **refusal);
    void *binding_context;
    live_texture_reader reader; /* guest memory */
    void *reader_context;

} live_vk_bind_source;

typedef struct live_vk_bind live_vk_bind;

/* `cache` and `set` stay owned by the caller and must outlive the bridge. */
live_vk_bind *live_vk_bind_create(live_texture_cache *cache, live_vk_texture_set *set, const live_vk_bind_source *source);
void live_vk_bind_destroy(live_vk_bind *bind);
/* Optional checked binding resolver; reader must read resolved guest VA directly. */
void live_vk_bind_set_resolver(live_vk_bind *bind, live_texture_resolver resolver, void *context,
                               live_texture_reader reader, void *reader_context);
/* A texture lookup that resolves to a render target (T793's images, registered with the cache by live_vk_target_register) is
 * sampled in place: the bridge fetches the target's view from `targets` (created on first use) and registers it with the texture
 * set. Without this a render target texture stage is refused, named. `targets` must outlive the bridge. */
void live_vk_bind_use_targets(live_vk_bind *bind, live_vk_target_set *targets);
/* T829: the texture id (live_vk_target_texture_id) of the target the next draw renders into, 0 for none. A stage that samples that
 * same target is refused by name (a feedback loop). live_vk_frame sets it from live_vk_draw. */
void live_vk_bind_set_draw_target(live_vk_bind *bind, uint32_t texture_id);
/* T1206: with a snapshot provider a stage that samples the draw's own target reads a copy of it as it stood before the draw
 * (what xemu does: it copies the surface into a texture image at bind time) instead of the feedback refusal. The provider
 * returns the copy's view (VK_IMAGE_LAYOUT_GENERAL) for target `texture_id` of draw `draw`, false with `error` filled to refuse. */
typedef bool (*live_vk_bind_snapshot_fn)(void *context, size_t draw, uint32_t texture_id, VkImageView *view, char *error,
                                         size_t error_bytes);
void live_vk_bind_set_snapshot(live_vk_bind *bind, live_vk_bind_snapshot_fn snapshot, void *context);
/* Install the planning callback into `backend` (before live_vk_renderer_create copies it). */
void live_vk_bind_backend(live_vk_bind *bind, gpu_pgraph_backend *backend);
/* The binding hook for live_vk_renderer_set_texture_hook. The pointer stays valid as long as `bind`. */
live_vk_texture_hook live_vk_bind_hook(live_vk_bind *bind);
/* Per frame: drain the guest write watch into the cache before the first lookup, and drop the images of entries no longer in
 * use after the last draw. */
bool live_vk_bind_begin_frame(live_vk_bind *bind);
bool live_vk_bind_end_frame(live_vk_bind *bind);

/* T1246: see live_texture_inputs.h. Captures the guest memory the lookups of every draw of `model` will read into `inputs`, with the
 * given binding source and the guest side resolver and reader. False when the store is full (do not pipeline the frame). Reads the
 * cache's registered targets, which only change in synchronous presenter jobs. */
bool live_vk_bind_capture_inputs(const live_vk_bind *bind, const gpu_pgraph *model, bool (*binding)(void *context, size_t draw, uint32_t stage,
                                 live_texture_binding *out, const char **refusal), void *binding_context,
                                 live_texture_inputs *inputs, live_texture_resolver resolver, void *resolver_context,
                                 live_texture_reader reader, void *reader_context);

#endif
