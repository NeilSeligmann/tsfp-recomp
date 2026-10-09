/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See live_vk_bind.h.
 */
#include "live_vk_bind.h"

#include "live_texture_watch.h"
#include "live_draw_dump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REFUSAL_BYTES 320u

struct live_vk_bind {
    live_texture_cache *cache;
    live_vk_texture_set *set;
    live_vk_bind_source source;
    live_texture_resolver resolver;
    void *resolver_context;
    live_vk_target_set *targets;
    live_vk_bind_snapshot_fn snapshot; /* T1206: feedback snapshot provider, NULL refuses a feedback draw */
    void *snapshot_context;
    uint32_t draw_target_id; /* T829: the target the current draw renders into (texture id), 0 for none */
    uint32_t allowed_inferences;
    /* gpu_combiner_texture.refusal is a borrowed const char *: one buffer per stage, valid until the next draw's planning */
    char refusal[GPU_COMBINER_TEXTURE_STAGES][REFUSAL_BYTES];
    uint8_t placeholder[4]; /* a target texture has no host pixels: the plan only needs the stage to be present */
};

live_vk_bind *live_vk_bind_create(live_texture_cache *cache, live_vk_texture_set *set, const live_vk_bind_source *source)
{
    if (cache == NULL || set == NULL || source == NULL || source->binding == NULL) {
        return NULL;
    }
    live_vk_bind *bind = calloc(1u, sizeof *bind);
    if (bind != NULL) {
        bind->cache = cache;
        bind->set = set;
        bind->source = *source;
    }
    return bind;
}

void live_vk_bind_destroy(live_vk_bind *bind)
{
    free(bind);
}

void live_vk_bind_set_resolver(live_vk_bind *bind, live_texture_resolver resolver, void *context,
                               live_texture_reader reader, void *reader_context)
{
    bind->resolver = resolver;
    bind->resolver_context = context;
    bind->source.reader = reader;
    bind->source.reader_context = reader_context;
}

/* The sampler of `stage` from the draw's snapshot, or false with a named reason in `reason`. */
static bool stage_sampler(const live_vk_bind *bind, const gpu_pgraph_state *state, uint32_t stage, uint32_t levels, bool mip_chain, bool cube, live_vk_sampler_desc *sampler,
                          char *reason, size_t reason_bytes)
{
    const uint32_t address_index = GPU_PGRAPH_OUT_TEXTURE_ADDRESS + stage;
    const uint32_t filter_index = GPU_PGRAPH_OUT_TEXTURE_FILTER + stage;
    if (!state->output_written[address_index] || !state->output_written[filter_index]) {
        snprintf(reason, reason_bytes, "texture stage %u: the %s word (0x%04X) was never written by the stream: its initial value is not measured",
                 (unsigned)stage, !state->output_written[address_index] ? "address" : "filter",
                 (unsigned)((!state->output_written[address_index] ? 0x1B08u : 0x1B14u) + 0x40u * stage));
        return false;
    }
    uint32_t address_word = state->output[address_index];
    if (cube && ((address_word >> 16) & 0xFu) <= 5u) {
        /* T1490: a cube map samples seamlessly (xemu GL cube maps ignore the wrap modes), only the P field and the cylinder bits
         * xemu never reads (it masks ADDRU, ADDRV and ADDRP only) are dropped, U and V still decode as for a 2D texture. INFERRED. */
        address_word &= 0x00000F0Fu;
    }
    *sampler = mip_chain ? live_vk_sampler_decode_mips(address_word, state->output[filter_index],
                                                       state->output[GPU_PGRAPH_OUT_TEXTURE_CONTROL0 + stage], levels)
                          : live_vk_sampler_decode(address_word, state->output[filter_index]);
    if (!mip_chain) live_vk_sampler_apply_control(sampler, state->output[GPU_PGRAPH_OUT_TEXTURE_CONTROL0 + stage]);
    if (!sampler->ok) {
        snprintf(reason, reason_bytes, "texture stage %u: sampler state address 0x%08X filter 0x%08X is not decodable: %s", (unsigned)stage,
                 (unsigned)state->output[address_index], (unsigned)state->output[filter_index], sampler->refusal);
        return false;
    }
    if (!sampler->measured && (bind->allowed_inferences & GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING) == 0u) {
        snprintf(reason, reason_bytes,
                 "texture stage %u: sampler state address 0x%08X filter 0x%08X is INFERRED (not xemu measured) and "
                 "GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING is not allowed",
                 (unsigned)stage, (unsigned)state->output[address_index], (unsigned)state->output[filter_index]);
        return false;
    }
    return true;
}

/* A mipmapped binding carries the maxLOD of the draw's Control0 word (the plan and the cache key depend on it). `levels` is the
 * chain length the plan gives (1 for a binding with no mip chain). False with a named reason. */
static bool mip_limit_of(const live_vk_bind *bind, const gpu_pgraph_state *state, uint32_t stage, live_texture_binding *binding,
                         uint32_t *levels, char *reason, size_t reason_bytes)
{
    *levels = 1u;
    if (((binding->format >> 16) & 15u) <= 1u) {
        return true;
    }
    const uint32_t control_index = GPU_PGRAPH_OUT_TEXTURE_CONTROL0 + stage;
    if (!state->output_written[control_index]) {
        snprintf(reason, reason_bytes, "texture stage %u: mip Control0 was never written", (unsigned)stage);
        return false;
    }
    binding->mip_limit = ((state->output[control_index] & 0x0003FFC0u) >> 6) + 1u;
    live_texture_plan plan;
    if (!live_texture_plan_binding(binding, bind->cache->allow_inferred, &plan)) {
        snprintf(reason, reason_bytes, "texture stage %u: %s: %s", (unsigned)stage, plan.refusal, plan.detail);
        return false;
    }
    *levels = plan.levels;
    return true;
}

/* Binding, lookup and sampler of one stage at one draw. False with `reason`; `result` and `sampler` are valid on success. */
static bool resolve_stage(live_vk_bind *bind, size_t draw, const gpu_pgraph_state *state, uint32_t stage,
                          live_texture_result *result, live_vk_sampler_desc *sampler, char *reason, size_t reason_bytes)
{
    live_texture_binding binding;
    const char *why = NULL;
    memset(&binding, 0, sizeof binding);
    if (!bind->source.binding(bind->source.binding_context, draw, stage, &binding, &why)) {
        snprintf(reason, reason_bytes, "texture stage %u: %s", (unsigned)stage, why != NULL ? why : "no binding");
        return false;
    }
    uint32_t levels = 1u;
    if (!mip_limit_of(bind, state, stage, &binding, &levels, reason, reason_bytes)) {
        return false;
    }
    if (!stage_sampler(bind, state, stage, levels, ((binding.format >> 16) & 15u) > 1u, ((binding.format >> 2) & 1u) != 0u, sampler, reason, reason_bytes)) {
        return false;
    }
    live_texture_lookup_resolved(bind->cache, &binding, bind->source.reader, bind->source.reader_context,
                                 bind->resolver, bind->resolver_context, result);
    if (live_draw_dump_file() != NULL) {
        live_draw_dump_line("T draw=%zu stage=%u fmt=%08x size=%08x data=%07x src=%d addr=%x filt=%x ctl0=%x", draw,
                            (unsigned)stage, (unsigned)binding.format, (unsigned)binding.size_word, (unsigned)binding.data,
                            (int)result->source, (unsigned)state->output[GPU_PGRAPH_OUT_TEXTURE_ADDRESS + stage],
                            (unsigned)state->output[GPU_PGRAPH_OUT_TEXTURE_FILTER + stage],
                            (unsigned)state->output[GPU_PGRAPH_OUT_TEXTURE_CONTROL0 + stage]);
    }
    if (result->source == LIVE_TEXTURE_SOURCE_REFUSED) {
        snprintf(reason, reason_bytes, "texture stage %u: %s%s%s", (unsigned)stage, result->refusal != NULL ? result->refusal : "refused",
                 result->detail[0] != '\0' ? ": " : "", result->detail);
        return false;
    }
    return true;
}

static void plan_texture(void *context, size_t draw, const gpu_pgraph_state *state, uint32_t stage, gpu_combiner_texture *out)
{
    live_vk_bind *bind = context;
    if (stage >= GPU_COMBINER_TEXTURE_STAGES) {
        return;
    }
    /* T1491: a stage whose written program is NONE (0) or DOTPRODUCT (17) samples no texture, whatever the title still has bound there, so the
     * lookup, its refusal and its census entry are skipped (the combiner plan treats a stage of those programs as defined without a texture). */
    if (state->combiner_written[54]) {
        const uint32_t mode = (state->combiner[54] >> (5u * stage)) & 0x1Fu;
        if (mode == 0u || mode == 17u) {
            out->rgba = NULL;
            out->refusal = NULL;
            return;
        }
    }
    live_texture_result result;
    live_vk_sampler_desc sampler;
    char *reason = bind->refusal[stage];
    reason[0] = '\0';
    if (!resolve_stage(bind, draw, state, stage, &result, &sampler, reason, REFUSAL_BYTES)) {
        out->rgba = NULL;
        out->refusal = reason;
        return;
    }
    out->rgba = result.source == LIVE_TEXTURE_SOURCE_GUEST ? result.rgba : bind->placeholder;
    out->width = result.width;
    out->height = result.height;
    out->linear = sampler.mag_filter == VK_FILTER_LINEAR;
    out->repeat = sampler.address_u == VK_SAMPLER_ADDRESS_MODE_REPEAT;
    out->cube = result.source == LIVE_TEXTURE_SOURCE_GUEST && result.cube; /* T1490 */
    /* a linear texture or a linear render target takes its coordinate in texels (T510), a swizzled one (a swizzled target, T1489) normalised */
    out->unnormalised = result.source == LIVE_TEXTURE_SOURCE_TARGET ? !result.target_swizzled
                                                                    : !bind->cache->entries[result.entry].plan.swizzled;
    out->refusal = NULL;
}

void live_vk_bind_use_targets(live_vk_bind *bind, live_vk_target_set *targets)
{
    bind->targets = targets;
}

void live_vk_bind_set_snapshot(live_vk_bind *bind, live_vk_bind_snapshot_fn snapshot, void *context)
{
    bind->snapshot = snapshot;
    bind->snapshot_context = context;
}

void live_vk_bind_set_draw_target(live_vk_bind *bind, uint32_t texture_id)
{
    bind->draw_target_id = texture_id;
}

void live_vk_bind_backend(live_vk_bind *bind, gpu_pgraph_backend *backend)
{
    bind->allowed_inferences = backend->allowed_inferences;
    backend->resolve_texture = plan_texture;
    backend->texture_context = bind;
}

static bool bind_texture(void *context, size_t draw, const gpu_pgraph_state *state, uint32_t stage, live_vk_texture_binding *out,
                         char *error, size_t error_bytes)
{
    live_vk_bind *bind = context;
    live_texture_result result;
    live_vk_sampler_desc sampler;
    if (!resolve_stage(bind, draw, state, stage, &result, &sampler, error, error_bytes)) {
        return false;
    }
    const bool feedback =
        result.source == LIVE_TEXTURE_SOURCE_TARGET && bind->draw_target_id != 0u && result.target_id == bind->draw_target_id;
    if (feedback && bind->snapshot == NULL) {
        snprintf(error, error_bytes, "texture stage %u samples render target %u while the draw renders into it (a feedback loop, refused)",
                 (unsigned)stage, (unsigned)result.target_id);
        return false;
    }
    VkImageView real_view = VK_NULL_HANDLE;
    if (result.source == LIVE_TEXTURE_SOURCE_TARGET) {
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
        if (bind->targets != NULL) {
            real_view = live_vk_target_texture_view(bind->targets, result.target_id);
            view = real_view;
        }
        if (feedback && view != VK_NULL_HANDLE) {
            /* T1206: sample the target as it stood before this draw (xemu copies the surface into a texture image at bind time) */
            if (!bind->snapshot(bind->snapshot_context, draw, result.target_id, &view, error, error_bytes)) {
                return false;
            }
        }
        if (view == VK_NULL_HANDLE || !live_vk_texture_register_target(bind->set, result.target_id, view, layout)) {
            snprintf(error, error_bytes, "texture stage %u: the render target %u has no image view to sample", (unsigned)stage,
                     (unsigned)result.target_id);
            return false;
        }
    }
    live_vk_texture_bound bound;
    const char *failure = NULL;
    const bool bound_ok = live_vk_texture_bind(bind->set, bind->cache, &result, &sampler, &bound, &failure);
    if (feedback && real_view != VK_NULL_HANDLE) {
        /* the snapshot view was registered under the target's id only for this bind, the next draw samples the target itself */
        (void)live_vk_texture_register_target(bind->set, result.target_id, real_view, VK_IMAGE_LAYOUT_GENERAL);
    }
    if (!bound_ok) {
        snprintf(error, error_bytes, "texture stage %u: %s", (unsigned)stage, failure != NULL ? failure : "bind failed");
        return false;
    }
    out->view = bound.view;
    out->sampler = bound.sampler;
    out->layout = bound.layout;
    return true;
}

live_vk_texture_hook live_vk_bind_hook(live_vk_bind *bind)
{
    return (live_vk_texture_hook){bind_texture, bind};
}

bool live_vk_bind_begin_frame(live_vk_bind *bind)
{
    if (!live_vk_texture_begin_batch(bind->set)) return false;
    (void)live_texture_watch_drain(bind->cache);
    return true;
}

bool live_vk_bind_end_frame(live_vk_bind *bind)
{
    if (!live_vk_texture_end_batch(bind->set)) return false;
    (void)live_vk_texture_trim(bind->set, bind->cache);
    return true;
}

/* T1246: capture, on the guest thread at the Swap, every guest memory input the texture lookups of the frame's draws will make. The
 * binding of each stage comes from `binding` (the Swap's SetTexture history), the key is built exactly like resolve_stage's. */
bool live_vk_bind_capture_inputs(const live_vk_bind *bind, const gpu_pgraph *model, bool (*binding)(void *context, size_t draw, uint32_t stage,
                                 live_texture_binding *out, const char **refusal), void *binding_context,
                                 live_texture_inputs *inputs, live_texture_resolver resolver, void *resolver_context,
                                 live_texture_reader reader, void *reader_context)
{
    const size_t draws = gpu_pgraph_draw_count(model);
    for (size_t draw = 0u; draw < draws; draw++) {
        const gpu_pgraph_draw *info = gpu_pgraph_draw_at(model, draw);
        const gpu_pgraph_state *state = info != NULL ? gpu_pgraph_snapshot(model, info->snapshot) : NULL;
        if (state == NULL) {
            return false;
        }
        for (uint32_t stage = 0u; stage < GPU_COMBINER_TEXTURE_STAGES; stage++) {
            live_texture_binding key;
            const char *why = NULL;
            memset(&key, 0, sizeof key);
            if (!binding(binding_context, draw, stage, &key, &why)) {
                continue;
            }
            uint32_t levels = 1u;
            char reason[REFUSAL_BYTES];
            if (!mip_limit_of(bind, state, stage, &key, &levels, reason, sizeof reason)) {
                continue;
            }
            if (!live_texture_inputs_capture(inputs, bind->cache, &key, resolver, resolver_context, reader, reader_context)) {
                return false;
            }
        }
    }
    return true;
}
