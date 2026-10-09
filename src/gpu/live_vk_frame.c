/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See live_vk_frame.h.
 */
#include "live_vk_frame.h"
#include "live_vk_query.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct live_vk_frame {
    gpu_window *window;
    live_vk_renderer *renderer;
    live_vk_draw *draw; /* T829, NULL until live_vk_frame_enable_target_draws */
    live_vk_frame_source source;
    live_vk_frame_config config;
    live_vk_frame_stats stats;
    live_vk_query *query;
    live_vk_query_report_fn query_report;
    void *query_context;
    bool texture_batch_active;
    bool query_enabled;
    bool query_valid;
    uint32_t query_samples;
    uint32_t pending_queries[LIVE_VK_MAX_DRAWS_PER_FRAME];
    size_t pending_query_count;
    live_vk_frame_clear_reason clear_reasons[LIVE_VK_FRAME_CLEAR_REASONS]; /* T860: refused clears by reason */
    size_t clear_reason_count;
    uint64_t clear_reason_other; /* refused clears whose reason did not fit the table */
};

/* T860: count one refused clear under its reason text. */
static void note_clear_refusal(live_vk_frame *frame, const gpu_pgraph_clear *clear, const char *plain_reason)
{
    char reason[LIVE_PIPELINE_REASON_BYTES];
    snprintf(reason, sizeof reason, "%s [clear flags 0x%02X: %s%s%s]", plain_reason, (unsigned)clear->flags,
             (clear->flags & 0x01u) != 0u ? "depth " : "", (clear->flags & 0x02u) != 0u ? "stencil " : "",
             (clear->flags & 0xF0u) != 0u ? "colour" : "no colour");
    for (size_t index = 0u; index < frame->clear_reason_count; index++) {
        if (strcmp(frame->clear_reasons[index].text, reason) == 0) {
            frame->clear_reasons[index].count++;
            return;
        }
    }
    if (frame->clear_reason_count == LIVE_VK_FRAME_CLEAR_REASONS) {
        frame->clear_reason_other++;
        return;
    }
    live_vk_frame_clear_reason *entry = &frame->clear_reasons[frame->clear_reason_count++];
    snprintf(entry->text, sizeof entry->text, "%s", reason);
    entry->count = 1u;
}

size_t live_vk_frame_clear_reason_count(const live_vk_frame *frame, uint64_t *other)
{
    if (other != NULL) {
        *other = frame->clear_reason_other;
    }
    return frame->clear_reason_count;
}

const live_vk_frame_clear_reason *live_vk_frame_clear_reason_at(const live_vk_frame *frame, size_t index)
{
    return index < frame->clear_reason_count ? &frame->clear_reasons[index] : NULL;
}

/* T829: the target hook of the renderer, the draw provider plus the texture bridge's feedback guard. */
static bool target_begin(void *context, size_t draw_index, const gpu_pgraph_state *state, live_vk_draw_target *out, char *error,
                         size_t error_bytes)
{
    live_vk_frame *frame = context;
    const live_vk_target_hook hook = live_vk_draw_hook(frame->draw);
    const bool accepted = hook.begin(hook.context, draw_index, state, out, error, error_bytes);
    if (frame->config.textures != NULL) {
        live_vk_bind_set_draw_target(frame->config.textures, accepted ? live_vk_draw_current_texture_id(frame->draw) : 0u);
    }
    return accepted;
}

static VkCommandBuffer target_command(void *context)
{
    live_vk_frame *frame = context;
    return live_vk_draw_current_command(frame->draw);
}

static bool target_snapshot(void *context, size_t draw_index, uint32_t texture_id, VkImageView *view, char *error, size_t error_bytes)
{
    live_vk_frame *frame = context;
    return live_vk_draw_snapshot(frame->draw, draw_index, texture_id, view, error, error_bytes);
}

static void apply_copy(live_vk_frame *frame, const gpu_pgraph_copy *copy)
{
    if (frame->draw != NULL && !live_vk_draw_flush(frame->draw)) { /* the draws before the copy must be in the images */
        frame->stats.copies_refused++;
        if (frame->query_enabled) frame->query_valid = false;
        return;
    }
    if (frame->config.targets == NULL) {
        frame->stats.copies_ignored++;
        if (frame->query_enabled) frame->query_valid = false;
        return;
    }
    size_t failed = 0u;
    live_target_refusal refusal;
    frame->stats.copies_applied++;
    if (live_vk_target_apply_copies(frame->config.targets, copy, 1u, &failed, &refusal) != 1u) {
        frame->stats.copies_refused++;
        if (frame->query_enabled) frame->query_valid = false;
    }
}

static bool apply_clear(live_vk_frame *frame, const gpu_pgraph *model, size_t index, const gpu_window_native *native)
{
    char error[LIVE_PIPELINE_REASON_BYTES];
    const live_vk_draw_target target = {native->command_buffer, LIVE_VK_PASS_WINDOW, native->extent.width,
                                        native->extent.height};
    frame->stats.clears_offered++;
    if (frame->draw != NULL) { /* T829: the clear goes to the target of the draw it precedes, not to the swapchain */
        if (live_vk_draw_apply_clear(frame->draw, model, index, error, sizeof error)) {
            return true;
        }
        frame->stats.clears_refused++;
    if (frame->query_enabled) frame->query_valid = false;
        note_clear_refusal(frame, gpu_pgraph_clear_at(model, index), error);
        return false;
    }
    if (live_vk_renderer_clear(frame->renderer, model, index, &target, error, sizeof error)) {
        return true;
    }
    frame->stats.clears_refused++;
    if (frame->query_enabled) frame->query_valid = false;
    note_clear_refusal(frame, gpu_pgraph_clear_at(model, index), error);
    return false;
}

static bool collect_queries(live_vk_frame *frame)
{
    if (frame->query == NULL) return true;
    if (!live_vk_draw_flush(frame->draw)) { frame->query_valid = false; frame->pending_query_count = 0u; return false; }
    for (size_t i = 0u; i < frame->pending_query_count; i++) {
        uint64_t samples;
        if (!live_vk_query_result(frame->query,frame->pending_queries[i],&samples)) {
            frame->query_valid = false;
            frame->pending_query_count = 0u;
            return false;
        }
        frame->query_samples += (uint32_t)samples;
    }
    frame->pending_query_count = 0u;
    return true;
}

static bool apply_query(live_vk_frame *frame, const gpu_pgraph_query *event)
{
    if (frame->query == NULL) return false;
    if (event->method == 0x17CCu) {
        frame->query_enabled = event->data != 0u;
        live_vk_renderer_set_query(frame->renderer,frame->query,frame->query_enabled);
        return true;
    }
    const bool complete = collect_queries(frame);
    if (event->method == 0x17C8u) {
        frame->query_samples = 0u;
        frame->query_valid = complete;
        return complete;
    }
    return complete && frame->query_valid && frame->query_report(frame->query_context,event,
                                                                 frame->query_samples);
}

static bool run_model(live_vk_frame *frame, const gpu_pgraph *model, const gpu_window_native *native, bool *device_ok);

static bool frame_complete(void *context)
{
    live_vk_frame *frame = context;
    if (!frame->texture_batch_active) return true;
    if (!live_vk_bind_end_frame(frame->config.textures)) return false;
    frame->texture_batch_active = false;
    return true;
}

static bool frame_hook(const gpu_window_native *native, void *context)
{
    live_vk_frame *frame = context;
    frame->stats.frames++;
    const gpu_pgraph *model = frame->source.pgraph != NULL ? frame->source.pgraph(frame->source.context) : NULL;
    if (model == NULL || native == NULL || native->command_buffer == VK_NULL_HANDLE) {
        return true; /* nothing to draw is not a refusal */
    }
    return run_model(frame, model, native, NULL);
}

/* T838: the same loop with no swapchain pass open (the host hook-up runs a frame at the Swap, the swapchain is presented later at
 * the modelled vblank). Only meaningful with target draws enabled, every draw then records its own command buffer. */
bool live_vk_frame_run_targets(live_vk_frame *frame)
{
    if (frame == NULL || frame->draw == NULL) {
        return false;
    }
    frame->stats.frames++;
    const gpu_pgraph *model = frame->source.pgraph != NULL ? frame->source.pgraph(frame->source.context) : NULL;
    if (model == NULL) {
        return true;
    }
    gpu_window_native native;
    if (!gpu_window_get_native(frame->window, &native)) {
        return false;
    }
    native.command_buffer = VK_NULL_HANDLE;
    /* T847: a refused draw is not a loop failure (the census names it, stats.frames_refused counts the frames), only a device that
     * could not record or submit is. The first T838 counter reported every frame with one refused draw, 1062 of 1062 on the retail
     * intro, which looked like a broken loop. */
    bool device_ok = true;
    (void)run_model(frame, model, &native, &device_ok);
    return device_ok;
}

static bool run_model(live_vk_frame *frame, const gpu_pgraph *model, const gpu_window_native *native, bool *device_ok)
{
    frame->stats.frames_drawn++;
    if (frame->query != NULL && !live_vk_query_reset(frame->query)) {
        if (device_ok != NULL) *device_ok = false;
        return false;
    }
    if (frame->draw != NULL) {
        (void)live_vk_draw_sync(frame->draw); /* T1264: runs of the last frame may still read the arena the next call resets */
    }
    live_vk_renderer_begin_frame(frame->renderer);
    if (frame->draw != NULL) {
        live_vk_draw_begin_frame(frame->draw);
    }
    if (frame->config.textures != NULL) {
        if (frame->texture_batch_active || !live_vk_bind_begin_frame(frame->config.textures)) {
            if (device_ok != NULL) *device_ok = false;
            frame->stats.frames_refused++;
            return false;
        }
        frame->texture_batch_active = true;
    }
    bool all_drawn = true;
    const size_t draws = gpu_pgraph_draw_count(model);
    const size_t copies = gpu_pgraph_copy_count(model);
    const size_t clears = gpu_pgraph_clear_count(model);
    size_t next_copy = 0u;
    size_t next_clear = 0u;
    size_t next_query = 0u;
    const size_t queries = gpu_pgraph_query_count(model);
    for (size_t index = 0u; index <= draws; index++) {
        for (;;) {
            const gpu_pgraph_copy *copy = next_copy < copies ? gpu_pgraph_copy_at(model,next_copy) : NULL;
            const gpu_pgraph_clear *clear = next_clear < clears ? gpu_pgraph_clear_at(model,next_clear) : NULL;
            const gpu_pgraph_query *query = next_query < queries ? gpu_pgraph_query_at(model,next_query) : NULL;
            uint64_t first = UINT64_MAX;
            unsigned kind = 0u;
            if (copy != NULL && copy->before_draw <= index) { first = copy->command; kind = 1u; }
            if (clear != NULL && clear->before_draw <= index && clear->command < first) { first = clear->command; kind = 2u; }
            if (query != NULL && query->before_draw <= index && query->command < first) { first = query->command; kind = 3u; }
            if (kind == 0u) break;
            if (kind == 1u) { apply_copy(frame,copy); next_copy++; }
            else if (kind == 2u) { all_drawn = apply_clear(frame,model,next_clear++,native) && all_drawn; }
            else { all_drawn = apply_query(frame,query) && all_drawn; next_query++; }
        }
        if (index == draws) break;
        char error[LIVE_PIPELINE_REASON_BYTES];
        frame->stats.draws_offered++;
        if (!live_vk_renderer_draw(frame->renderer, model, index, native->command_buffer, native->extent.width,
                                   native->extent.height, error, sizeof error)) {
            frame->stats.draws_refused++;
            all_drawn = false;
            if (frame->query_enabled) frame->query_valid = false;
        } else if (frame->query_enabled) {
            uint32_t slot = live_vk_renderer_query_slot(frame->renderer);
            if (slot != UINT32_MAX && frame->pending_query_count < LIVE_VK_MAX_DRAWS_PER_FRAME)
                frame->pending_queries[frame->pending_query_count++] = slot;
            else { frame->query_valid = false; all_drawn = false; }
        }
    }
    if (!collect_queries(frame)) all_drawn = false;
    while (next_clear < clears) {
        all_drawn = apply_clear(frame, model, next_clear++, native) && all_drawn;
    }
    while (next_copy < copies) {
        apply_copy(frame, gpu_pgraph_copy_at(model, next_copy++));
    }
    if (frame->draw != NULL && !live_vk_draw_flush(frame->draw)) {
        all_drawn = false;
        if (device_ok != NULL) {
            *device_ok = false;
        }
    }
    if (frame->config.textures != NULL) {
        live_vk_bind_set_draw_target(frame->config.textures, 0u);
        /* Target command buffers were submitted and waited above. Window commands are
         * still recording: retire their resources only in the window completion hook. */
        if (frame->draw != NULL && !frame_complete(frame)) {
            all_drawn = false;
            if (device_ok != NULL) *device_ok = false;
        }
    }
    if (!all_drawn) {
        frame->stats.frames_refused++;
    }
    return all_drawn;
}

live_vk_frame *live_vk_frame_attach(gpu_window *window, const gpu_pgraph_backend *backend,
                                    const live_vk_frame_source *source, const live_vk_frame_config *config, char *error,
                                    size_t error_bytes)
{
    gpu_window_native native;
    live_vk_device device;
    if (window == NULL || backend == NULL || source == NULL || !gpu_window_get_native(window, &native) ||
        !live_vk_device_from_window(&native, &device)) {
        if (error != NULL && error_bytes != 0u) {
            snprintf(error, error_bytes, "live_vk_frame_attach: no window device or missing argument");
        }
        return NULL;
    }
    live_vk_frame *frame = calloc(1u, sizeof *frame);
    if (frame == NULL) {
        return NULL;
    }
    frame->window = window;
    frame->source = *source;
    if (config != NULL) {
        frame->config = *config;
    }
    gpu_pgraph_backend planned = *backend;
    if (frame->config.textures != NULL) {
        live_vk_bind_backend(frame->config.textures, &planned);
        if (frame->config.targets != NULL) {
            live_vk_bind_use_targets(frame->config.textures, frame->config.targets);
        }
    }
    frame->renderer = live_vk_renderer_create(&device, native.render_pass, &planned, error, error_bytes);
    if (frame->renderer == NULL) {
        free(frame);
        return NULL;
    }
    if (frame->config.textures != NULL) {
        const live_vk_texture_hook hook = live_vk_bind_hook(frame->config.textures);
        live_vk_renderer_set_texture_hook(frame->renderer, &hook);
    }
    gpu_window_set_frame_hook(window, frame_hook, frame);
    gpu_window_set_complete_hook(window,frame_complete,frame);
    return frame;
}

void live_vk_frame_detach(live_vk_frame *frame)
{
    if (frame == NULL) {
        return;
    }
    gpu_window_set_frame_hook(frame->window, NULL, NULL);
    gpu_window_set_complete_hook(frame->window,NULL,NULL);
    live_vk_draw_destroy(frame->draw);
    live_vk_renderer_destroy(frame->renderer);
    live_vk_query_destroy(frame->query);
    free(frame);
}

bool live_vk_frame_overlay_submit(live_vk_frame *frame, uint32_t width, uint32_t height, const uint8_t *rgb)
{
    if (frame == NULL || frame->config.targets == NULL) {
        return false;
    }
    live_vk_target_overlay_submit(frame->config.targets, width, height, rgb);
    return true;
}

live_vk_renderer *live_vk_frame_renderer(live_vk_frame *frame)
{
    return frame->renderer;
}

bool live_vk_frame_enable_target_draws(live_vk_frame *frame, char *error, size_t error_bytes)
{
    if (frame == NULL || frame->config.targets == NULL) {
        if (error != NULL && error_bytes != 0u) {
            snprintf(error, error_bytes, "live_vk_frame_enable_target_draws: no frame or no target set in the config");
        }
        return false;
    }
    if (frame->draw != NULL) {
        return true;
    }
    gpu_window_native native;
    live_vk_device device;
    if (!gpu_window_get_native(frame->window, &native) || !live_vk_device_from_window(&native, &device)) {
        if (error != NULL && error_bytes != 0u) {
            snprintf(error, error_bytes, "live_vk_frame_enable_target_draws: no window device");
        }
        return false;
    }
    frame->draw = live_vk_draw_create(&device, frame->renderer, frame->config.targets, error, error_bytes);
    if (frame->draw == NULL) {
        return false;
    }
    const live_vk_target_hook hook = {target_begin, NULL, frame};
    live_vk_renderer_set_target_hook(frame->renderer, &hook);
    live_vk_renderer_set_target_command(frame->renderer, target_command, frame);
    if (frame->config.textures != NULL) {
        live_vk_bind_set_snapshot(frame->config.textures, target_snapshot, frame); /* T1206 */
    }
    return true;
}

live_vk_draw *live_vk_frame_draw(live_vk_frame *frame)
{
    return frame->draw;
}

live_vk_frame_stats live_vk_frame_get_stats(const live_vk_frame *frame)
{
    return frame->stats;
}

bool live_vk_frame_enable_visibility(live_vk_frame *frame, live_vk_query_report_fn report, void *context,
                                     char *error, size_t error_bytes)
{
    gpu_window_native native;
    live_vk_device device;
    if (frame == NULL || frame->draw == NULL || report == NULL ||
        !gpu_window_get_native(frame->window,&native) || !live_vk_device_from_window(&native,&device)) return false;
    if (frame->query == NULL) frame->query = live_vk_query_create(&device);
    if (frame->query == NULL) {
        if (error != NULL && error_bytes != 0u) snprintf(error,error_bytes,"precise occlusion queries unsupported");
        return false;
    }
    frame->query_report = report;
    frame->query_context = context;
    frame->query_valid = true;
    return true;
}
