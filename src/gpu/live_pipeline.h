/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T791 (M9): live NV2A state to a Vulkan pipeline per draw.
 *
 * The decisions are NOT made here. Every piece is the M2 replay's own resolver, called once per draw from the live
 * PGRAPH snapshot: gpu_pgraph_resolve_program (T95 selector table, vertex module), gpu_pgraph_resolve_fragment (the
 * register combiner plan and its module, when the backend enables the combiner), gpu_pgraph_resolve_output (blend, depth,
 * stencil, cull, scissor, alpha test, polygon offset, colour mask) and gpu_pgraph_resolve_primitive (point and line gates).
 * This file adds only what a live consumer needs on top: the pipeline description struct, the cache key, a bounded cache
 * over an opaque device seam, and the refusal census.
 *
 * STATIC versus DYNAMIC. A Vulkan pipeline bakes in the fields of `live_pipeline_key`: vertex and fragment module, the
 * topology, cull and front face, blend enable and factors and equation, colour write mask, alpha test function AND
 * reference (the replay's fragment modules take both as specialisation constants), depth
 * test and write and function, the stencil test, function, masks and ops, and whether a depth bias is on. The rest
 * (viewport, scissor, blend constant, stencil reference, depth bias factors) change per draw without a
 * new pipeline and travel in `live_pipeline_dynamic`. A mutation of a dynamic field never changes the key.
 *
 * INFERRED pieces stay opt-in. Nothing here enables an inference: the caller's gpu_pgraph_backend.allowed_inferences and
 * output_groups decide (`--window-to-clip` is backend.window_clip_modules, HQ57 and the viewport rules are the
 * INFER_VIEWPORT_* and INFER_OUTPUT_* bits), and the bits a selected draw depends on are returned in `used_inferences` and
 * accumulated in the census. Until the M4 xemu measurement lands the live path default is the replay's default: refuse.
 *
 * THE DEVICE SEAM. `live_pipeline_ops` creates and destroys a pipeline from a description and key. The Vulkan
 * implementation takes the device handle T790 (src/gpu/gpu_window.*) owns, and never touches its swapchain: a pipeline
 * targets RGBA8 colour (and D32S8 when depth or stencil is on) as the replay's does. This file has no Vulkan include.
 * NOT verified at runtime in this container: Vulkan on lavapipe fails here, so only the headless half is exercised.
 *
 * REFUSALS. A draw the resolvers refuse is recorded in the census with its draw index, the stage that refused and the
 * resolver's own message, and the draw is NOT drawn (no stand-in, no nearest match). Every refused draw is kept, in
 * order, so the exit gate (T794, zero refused draws) can print them all.
 */
#ifndef TSFP_LIVE_PIPELINE_H
#define TSFP_LIVE_PIPELINE_H

#include "gpu_pgraph_replay.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define LIVE_PIPELINE_CACHE_CAPACITY 256u
#define LIVE_PIPELINE_KEY_WORDS 40u
/* Key words 24..32 hold the texel coordinate rewrite (stage mask, then width and height of each stage), 39 the depth bias. */
#define LIVE_PIPELINE_KEY_TEXEL_WORD 24u
#define LIVE_PIPELINE_REASON_BYTES 320u

typedef enum {
    LIVE_STAGE_NONE = 0,
    LIVE_STAGE_PROGRAM,   /* vertex program not resolved */
    LIVE_STAGE_FRAGMENT,  /* register combiner not resolved */
    LIVE_STAGE_OUTPUT,    /* fixed-function output state not resolved */
    LIVE_STAGE_PRIMITIVE, /* point or line gate */
    LIVE_STAGE_VERTEX,    /* the draw's vertex data could not be assembled (gpu_pgraph_assemble_draw) */
    LIVE_STAGE_DEVICE,    /* the device could not build the pipeline */
    LIVE_STAGE_COUNT
} live_pipeline_stage;

/* Everything one draw's pipeline and per-draw dynamic state are built from. */
typedef struct {
    gpu_pgraph_program program;
    bool has_fragment;
    gpu_pgraph_fragment fragment;
    /* The sampled stages whose texture coordinate is in TEXELS (a linear texture, T510): the combiner module is rewritten for
     * them (gpu_spirv_texel_coordinates) with the texture size baked in, so the stage mask and sizes are part of the key. */
    uint32_t texel_stages;
    uint32_t texel_size[4][2];
    gpu_pgraph_output output;
    uint32_t topology; /* GPU_VSH_TOPOLOGY_* */
    uint32_t used_inferences; /* GPU_PGRAPH_INFER_* bits the draw depends on */
} live_pipeline_description;

/* The static (pipeline baked) part. Fixed array of words so equality is a memcmp and no padding is ever compared. */
typedef struct {
    uint32_t words[LIVE_PIPELINE_KEY_WORDS];
} live_pipeline_key;

/* The part that changes per draw without a new pipeline. Scissor is in target pixels as the output resolver gave it. */
typedef struct {
    uint32_t viewport_width;
    uint32_t viewport_height;
    bool scissor;
    uint32_t scissor_x, scissor_y, scissor_width, scissor_height;
    float blend_constant[4];
    uint32_t stencil_ref;
    uint32_t alpha_ref; /* informational: the pipeline bakes it, see live_pipeline_key */
    float depth_bias_constant;
    float depth_bias_slope;
} live_pipeline_dynamic;

typedef struct {
    /* Build the device pipeline. false fills `error`. `*handle` is opaque to the cache. */
    bool (*create)(void *context, const live_pipeline_description *description, const live_pipeline_key *key,
                   void **handle, char *error, size_t error_bytes);
    void (*destroy)(void *context, void *handle);
    void *context;
} live_pipeline_ops;

typedef struct {
    size_t draw;
    live_pipeline_stage stage;
    gpu_pgraph_result result;
    char reason[LIVE_PIPELINE_REASON_BYTES];
} live_pipeline_refusal;

typedef struct {
    live_pipeline_refusal *refusals; /* every refused draw, in order */
    size_t count;
    size_t capacity;
    uint32_t by_stage[LIVE_STAGE_COUNT];
    uint64_t selected;       /* draws that got a pipeline */
    uint32_t used_inferences; /* OR over selected draws */
} live_pipeline_census;

typedef struct {
    uint64_t hash;
    live_pipeline_key key;
    void *handle;
    uint64_t stamp;
    bool used;
} live_pipeline_slot;

typedef struct {
    live_pipeline_ops ops;
    live_pipeline_slot slots[LIVE_PIPELINE_CACHE_CAPACITY];
    uint64_t (*hash)(const live_pipeline_key *key); /* live_pipeline_key_hash, replaceable so a test can force collisions */
    uint64_t clock;
    uint64_t hits, misses, evictions, creates_failed;
} live_pipeline_cache;

typedef struct {
    live_pipeline_description description;
    live_pipeline_key key;
    live_pipeline_dynamic dynamic;
    void *handle; /* the cache's pipeline, valid until the cache evicts or is destroyed */
    bool cache_hit;
} live_pipeline_selection;

const char *live_pipeline_stage_name(live_pipeline_stage stage);

/* One census entry for a draw refused outside live_pipeline_select (a target hook, the renderer). NULL census: no-op. */
void live_pipeline_census_refuse(live_pipeline_census *census, size_t draw, live_pipeline_stage stage,
                                 gpu_pgraph_result result, const char *reason);

/* A draw that live_pipeline_select accepted but a later step (vertex assembly, a texture, the arena) refused: one census
 * entry at `stage`, and the draw no longer counts as selected. `census` NULL is a no-op. */
void live_pipeline_census_refuse_selected(live_pipeline_census *census, size_t draw, live_pipeline_stage stage,
                                          gpu_pgraph_result result, const char *reason);

/* Resolve one draw's state to a description, device free. `primitive` is the BEGIN_END operation (gpu_pgraph_draw).
 * `draw` only labels the refusal. Returns the replay resolver's result, `*stage` names which one refused and report->error
 * holds its message. ERR_ARGUMENT for NULLs. */
gpu_pgraph_result live_pipeline_resolve(const gpu_pgraph_state *state, uint32_t primitive, size_t draw,
                                        const gpu_pgraph_backend *backend, uint32_t width, uint32_t height,
                                        live_pipeline_description *out, live_pipeline_stage *stage,
                                        gpu_pgraph_report *report);

/* The backend one draw is resolved with: `backend` plus the per draw target extent (viewport inference). Use it for every live draw. */
gpu_pgraph_backend live_pipeline_draw_backend(const gpu_pgraph_backend *backend, const gpu_pgraph_draw *draw,
                                              uint32_t target_width, uint32_t target_height);

/* The static key and the dynamic state of a description. Device free, deterministic. */
void live_pipeline_key_build(const live_pipeline_description *description, live_pipeline_key *key);
void live_pipeline_dynamic_build(const live_pipeline_description *description, const gpu_pgraph_state *state,
                                 uint32_t width, uint32_t height, live_pipeline_dynamic *out);
uint64_t live_pipeline_key_hash(const live_pipeline_key *key);

void live_pipeline_census_init(live_pipeline_census *census);
void live_pipeline_census_free(live_pipeline_census *census);
/* One line per refused draw (index, stage, result, message), then the per-stage totals and selected count. */
void live_pipeline_census_print(const live_pipeline_census *census, FILE *out);

void live_pipeline_cache_init(live_pipeline_cache *cache, const live_pipeline_ops *ops);
/* Destroy every cached pipeline through ops.destroy. */
void live_pipeline_cache_destroy(live_pipeline_cache *cache);

/* The per-draw entry point: resolve, build key, find or create the pipeline, record in the census. A refused draw (any
 * stage, including the device refusing to create) returns the error, is added to the census and leaves `out` without a
 * handle. `census` may be NULL only for tests. */
gpu_pgraph_result live_pipeline_select(live_pipeline_cache *cache, live_pipeline_census *census,
                                       const gpu_pgraph_state *state, uint32_t primitive, size_t draw,
                                       const gpu_pgraph_backend *backend, uint32_t width, uint32_t height,
                                       live_pipeline_selection *out);

#endif
