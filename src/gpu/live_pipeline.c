/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See live_pipeline.h. The resolvers are gpu_pgraph_replay.c's, this file adds the key, the cache and the census.
 */
#include "live_pipeline.h"

#include <stdlib.h>
#include <string.h>

const char *live_pipeline_stage_name(live_pipeline_stage stage)
{
    switch (stage) {
    case LIVE_STAGE_PROGRAM:
        return "program";
    case LIVE_STAGE_FRAGMENT:
        return "fragment";
    case LIVE_STAGE_OUTPUT:
        return "output";
    case LIVE_STAGE_PRIMITIVE:
        return "primitive";
    case LIVE_STAGE_VERTEX:
        return "vertex";
    case LIVE_STAGE_DEVICE:
        return "device";
    default:
        return "none";
    }
}

gpu_pgraph_backend live_pipeline_draw_backend(const gpu_pgraph_backend *backend, const gpu_pgraph_draw *draw,
                                              uint32_t target_width, uint32_t target_height)
{
    (void)draw; /* kept for the callers' signature, the draw no longer contributes (T870) */
    gpu_pgraph_backend result = *backend;
    result.viewport_width = target_width;
    result.viewport_height = target_height;
    result.viewport_from_target = backend->viewport_from_target ||
                                  (result.allowed_inferences & GPU_PGRAPH_INFER_VIEWPORT_FROM_TARGET) != 0u;
    return result;
}

gpu_pgraph_result live_pipeline_resolve(const gpu_pgraph_state *state, uint32_t primitive, size_t draw,
                                        const gpu_pgraph_backend *backend, uint32_t width, uint32_t height,
                                        live_pipeline_description *out, live_pipeline_stage *stage,
                                        gpu_pgraph_report *report)
{
    (void)draw;
    *stage = LIVE_STAGE_NONE;
    if (state == NULL || backend == NULL || out == NULL || report == NULL) {
        snprintf(report != NULL ? report->error : (char[1]){0}, report != NULL ? sizeof report->error : 1u,
                 "live_pipeline_resolve: missing argument");
        return GPU_PGRAPH_ERR_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    report->error[0] = '\0';
    gpu_pgraph_result result = gpu_pgraph_resolve_program(state, backend, &out->program, report);
    if (result != GPU_PGRAPH_OK) {
        *stage = LIVE_STAGE_PROGRAM;
        return result;
    }
    out->used_inferences |= GPU_PGRAPH_INFER_PROGRAM_HEADER |
                            (out->program.mode_inferred ? GPU_PGRAPH_INFER_EXECUTION_MODE_UNWRITTEN : 0u);
    gpu_pgraph_backend draw_backend = *backend;
    if (backend->resolve_texture != NULL) {
        for (uint32_t texture = 0u; texture < GPU_COMBINER_TEXTURE_STAGES; texture++) {
            memset(&draw_backend.test_textures[texture], 0, sizeof draw_backend.test_textures[texture]);
            backend->resolve_texture(backend->texture_context, draw, state, texture,
                                     &draw_backend.test_textures[texture]);
        }
    }
    if (backend->combiner) {
        result = gpu_pgraph_resolve_fragment(state, &draw_backend, &out->fragment, report);
        if (result != GPU_PGRAPH_OK) {
            *stage = LIVE_STAGE_FRAGMENT;
            return result;
        }
        out->has_fragment = true;
        for (uint32_t texture = 0u; texture < GPU_COMBINER_TEXTURE_STAGES; texture++) {
            if (((out->fragment.plan.texture_stages >> texture) & 1u) != 0u &&
                draw_backend.test_textures[texture].unnormalised) {
                out->texel_stages |= 1u << texture;
                out->texel_size[texture][0] = draw_backend.test_textures[texture].width;
                out->texel_size[texture][1] = draw_backend.test_textures[texture].height;
            }
        }
    }
    result = gpu_pgraph_resolve_output(state, backend, width, height, &out->output, report);
    if (result != GPU_PGRAPH_OK) {
        *stage = LIVE_STAGE_OUTPUT;
        return result;
    }
    out->used_inferences |= out->output.used_inferences;
    out->topology = gpu_pgraph_topology_of(primitive);
    result = gpu_pgraph_resolve_primitive(backend, out->topology, &out->used_inferences, report);
    if (result != GPU_PGRAPH_OK) {
        *stage = LIVE_STAGE_PRIMITIVE;
    }
    return result;
}

void live_pipeline_key_build(const live_pipeline_description *description, live_pipeline_key *key)
{
    memset(key, 0, sizeof *key);
    const gpu_vsh_output *output = &description->output.output;
    const bool active = description->output.active;
    uint32_t *word = key->words;
    *word++ = description->program.module;
    *word++ = description->has_fragment ? description->fragment.module + 1u : 0u;
    *word++ = description->has_fragment ? description->fragment.plan.texture_stages : 0u;
    *word++ = description->topology;
    *word++ = active ? 1u : 0u;
    key->words[LIVE_PIPELINE_KEY_TEXEL_WORD] = description->has_fragment ? description->texel_stages : 0u;
    for (uint32_t texture = 0u; texture < 4u && description->has_fragment; texture++) {
        if (((description->texel_stages >> texture) & 1u) != 0u) {
            key->words[LIVE_PIPELINE_KEY_TEXEL_WORD + 1u + 2u * texture] = description->texel_size[texture][0];
            key->words[LIVE_PIPELINE_KEY_TEXEL_WORD + 2u + 2u * texture] = description->texel_size[texture][1];
        }
    }
    if (!active) {
        return; /* nothing the output resolver set: every output word stays the zero default */
    }
    *word++ = output->cull_mode;
    *word++ = output->front_clockwise ? 1u : 0u;
    *word++ = output->blend ? 1u : 0u;
    /* blend factors are only baked while blending is on */
    *word++ = output->blend ? output->blend_source : 0u;
    *word++ = output->blend ? output->blend_destination : 0u;
    *word++ = output->blend ? output->blend_equation : 0u;
    *word++ = output->color_write_disable;
    *word++ = output->alpha_test ? 1u + output->alpha_func : 0u;
    *word++ = output->alpha_test ? output->alpha_ref : 0u;
    *word++ = output->depth_test ? 1u : 0u;
    *word++ = output->depth_test ? (output->depth_write ? 1u : 0u) | (output->depth_func << 1) : 0u;
    *word++ = output->stencil_test ? 1u : 0u;
    if (output->stencil_test) {
        *word++ = output->stencil_func;
        *word++ = output->stencil_compare_mask;
        *word++ = output->stencil_write_mask;
        *word++ = output->stencil_fail_op | (output->stencil_zfail_op << 8) | (output->stencil_zpass_op << 16);
    }
    key->words[LIVE_PIPELINE_KEY_WORDS - 1u] = (output->depth_bias ? 1u : 0u) | (output->polygon_mode << 1); /* T860 */
}

void live_pipeline_dynamic_build(const live_pipeline_description *description, const gpu_pgraph_state *state,
                                 uint32_t width, uint32_t height, live_pipeline_dynamic *out)
{
    (void)state;
    memset(out, 0, sizeof *out);
    out->viewport_width = width;
    out->viewport_height = height;
    if (!description->output.active) {
        return;
    }
    const gpu_vsh_output *output = &description->output.output;
    out->scissor = output->scissor;
    out->scissor_x = output->scissor_x;
    out->scissor_y = output->scissor_y;
    out->scissor_width = output->scissor_width;
    out->scissor_height = output->scissor_height;
    memcpy(out->blend_constant, output->blend_constant, sizeof out->blend_constant);
    out->stencil_ref = output->stencil_ref;
    out->alpha_ref = output->alpha_ref;
    out->depth_bias_constant = output->depth_bias_constant;
    out->depth_bias_slope = output->depth_bias_slope;
}

uint64_t live_pipeline_key_hash(const live_pipeline_key *key)
{
    uint64_t hash = 0xCBF29CE484222325ull;
    for (size_t index = 0u; index < LIVE_PIPELINE_KEY_WORDS; index++) {
        for (uint32_t shift = 0u; shift < 32u; shift += 8u) {
            hash = (hash ^ ((key->words[index] >> shift) & 0xFFu)) * 0x100000001B3ull;
        }
    }
    return hash;
}

/* --- census ------------------------------------------------------------------------------ */

void live_pipeline_census_init(live_pipeline_census *census)
{
    memset(census, 0, sizeof *census);
}

void live_pipeline_census_free(live_pipeline_census *census)
{
    free(census->refusals);
    memset(census, 0, sizeof *census);
}

static void census_refuse(live_pipeline_census *census, size_t draw, live_pipeline_stage stage,
                          gpu_pgraph_result result, const char *reason)
{
    if (census == NULL) {
        return;
    }
    if (census->count == census->capacity) {
        const size_t capacity = census->capacity == 0u ? 64u : census->capacity * 2u;
        live_pipeline_refusal *grown = realloc(census->refusals, capacity * sizeof *grown);
        if (grown == NULL) {
            census->by_stage[stage]++; /* counted even when the entry cannot be stored */
            return;
        }
        census->refusals = grown;
        census->capacity = capacity;
    }
    live_pipeline_refusal *entry = &census->refusals[census->count++];
    entry->draw = draw;
    entry->stage = stage;
    entry->result = result;
    snprintf(entry->reason, sizeof entry->reason, "%s", reason);
    census->by_stage[stage]++;
}

void live_pipeline_census_refuse(live_pipeline_census *census, size_t draw, live_pipeline_stage stage,
                                 gpu_pgraph_result result, const char *reason)
{
    census_refuse(census, draw, stage, result, reason);
}

void live_pipeline_census_refuse_selected(live_pipeline_census *census, size_t draw, live_pipeline_stage stage,
                                          gpu_pgraph_result result, const char *reason)
{
    if (census == NULL) {
        return;
    }
    if (census->selected != 0u) {
        census->selected--;
    }
    census_refuse(census, draw, stage, result, reason);
}

void live_pipeline_census_print(const live_pipeline_census *census, FILE *out)
{
    for (size_t index = 0u; index < census->count; index++) {
        const live_pipeline_refusal *entry = &census->refusals[index];
        fprintf(out, "live-pipeline refused draw %zu [%s] %s\n", entry->draw,
                live_pipeline_stage_name(entry->stage), entry->reason);
    }
    fprintf(out,
            "live-pipeline census: selected %llu refused %zu (program %u fragment %u output %u primitive %u vertex %u device %u)\n",
            (unsigned long long)census->selected, census->count, census->by_stage[LIVE_STAGE_PROGRAM],
            census->by_stage[LIVE_STAGE_FRAGMENT], census->by_stage[LIVE_STAGE_OUTPUT],
            census->by_stage[LIVE_STAGE_PRIMITIVE], census->by_stage[LIVE_STAGE_VERTEX],
            census->by_stage[LIVE_STAGE_DEVICE]);
}

/* --- cache ------------------------------------------------------------------------------- */

void live_pipeline_cache_init(live_pipeline_cache *cache, const live_pipeline_ops *ops)
{
    memset(cache, 0, sizeof *cache);
    cache->ops = *ops;
    cache->hash = live_pipeline_key_hash;
}

void live_pipeline_cache_destroy(live_pipeline_cache *cache)
{
    for (size_t index = 0u; index < LIVE_PIPELINE_CACHE_CAPACITY; index++) {
        if (cache->slots[index].used && cache->ops.destroy != NULL) {
            cache->ops.destroy(cache->ops.context, cache->slots[index].handle);
        }
        cache->slots[index].used = false;
    }
}

static live_pipeline_slot *cache_find(live_pipeline_cache *cache, uint64_t hash, const live_pipeline_key *key)
{
    for (size_t index = 0u; index < LIVE_PIPELINE_CACHE_CAPACITY; index++) {
        live_pipeline_slot *slot = &cache->slots[index];
        if (slot->used && slot->hash == hash && memcmp(&slot->key, key, sizeof *key) == 0) {
            return slot;
        }
    }
    return NULL;
}

/* A free slot, else the least recently used one (destroyed first). */
static live_pipeline_slot *cache_victim(live_pipeline_cache *cache)
{
    live_pipeline_slot *victim = &cache->slots[0];
    for (size_t index = 0u; index < LIVE_PIPELINE_CACHE_CAPACITY; index++) {
        live_pipeline_slot *slot = &cache->slots[index];
        if (!slot->used) {
            return slot;
        }
        if (slot->stamp < victim->stamp) {
            victim = slot;
        }
    }
    if (cache->ops.destroy != NULL) {
        cache->ops.destroy(cache->ops.context, victim->handle);
    }
    victim->used = false;
    cache->evictions++;
    return victim;
}

gpu_pgraph_result live_pipeline_select(live_pipeline_cache *cache, live_pipeline_census *census,
                                       const gpu_pgraph_state *state, uint32_t primitive, size_t draw,
                                       const gpu_pgraph_backend *backend, uint32_t width, uint32_t height,
                                       live_pipeline_selection *out)
{
    if (cache == NULL || out == NULL) {
        return GPU_PGRAPH_ERR_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    live_pipeline_stage stage;
    const gpu_pgraph_result result = live_pipeline_resolve(state, primitive, draw, backend, width, height,
                                                           &out->description, &stage, &report);
    if (result != GPU_PGRAPH_OK) {
        census_refuse(census, draw, stage == LIVE_STAGE_NONE ? LIVE_STAGE_PROGRAM : stage, result, report.error);
        return result;
    }
    live_pipeline_key_build(&out->description, &out->key);
    live_pipeline_dynamic_build(&out->description, state, width, height, &out->dynamic);
    const uint64_t hash = cache->hash(&out->key);
    live_pipeline_slot *slot = cache_find(cache, hash, &out->key);
    if (slot != NULL) {
        cache->hits++;
        out->cache_hit = true;
    } else {
        cache->misses++;
        char error[LIVE_PIPELINE_REASON_BYTES] = "device refused the pipeline";
        void *handle = NULL;
        if (cache->ops.create == NULL ||
            !cache->ops.create(cache->ops.context, &out->description, &out->key, &handle, error, sizeof error)) {
            cache->creates_failed++;
            census_refuse(census, draw, LIVE_STAGE_DEVICE, GPU_PGRAPH_ERR_DEVICE, error);
            return GPU_PGRAPH_ERR_DEVICE;
        }
        slot = cache_victim(cache);
        slot->used = true;
        slot->hash = hash;
        slot->key = out->key;
        slot->handle = handle;
    }
    slot->stamp = ++cache->clock;
    out->handle = slot->handle;
    if (census != NULL) {
        census->selected++;
        census->used_inferences |= out->description.used_inferences;
    }
    return GPU_PGRAPH_OK;
}
