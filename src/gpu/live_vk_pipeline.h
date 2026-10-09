/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T791 (M9), the Vulkan half: the live draw renderer. It turns each live NV2A draw into a Vulkan pipeline (through
 * live_pipeline.h, which only DECIDES) and records the draw into a command buffer. docs/live-pipeline.md has the design.
 *
 * THE INTERFACE SIBLING WORKERS PLUG INTO (T792 textures: src/gpu/live_vk_texture.*, T793 targets: src/gpu/live_vk_target.*).
 * Everything they need is in this header. They never touch the pipeline cache.
 *
 *   live_vk_device         the Vulkan handles, built from a gpu_window_native or a gpu_device_native (the two adapters below).
 *   live_vk_texture_hook   T792 fills it: for a draw whose combiner samples stage n, give the VkImageView and VkSampler.
 *   live_vk_target_hook    T793 fills it: for a draw, give the command buffer, render pass and extent to draw into.
 *
 * RENDER PASSES. A pipeline is compatible with a render pass by its attachment formats, so there are two kinds of pass
 * and the renderer keeps one pipeline cache per kind:
 *   LIVE_VK_PASS_WINDOW     the swapchain pass T790 owns (colour only, no depth, format of the swapchain).
 *   LIVE_VK_PASS_OFFSCREEN  RGBA8 colour plus D32_SFLOAT_S8_UINT depth and stencil, created by the renderer
 *                           (`live_vk_renderer_offscreen_pass`), the same formats as gpu_vsh_draw's pass. T793 builds its
 *                           framebuffers against this pass (colour attachment 0, depth stencil attachment 1) and begins it
 *                           itself with LOAD ops, its own command buffer, its own submit.
 * A draw that needs depth or stencil and runs in the window pass is REFUSED (census stage "device", named): the swapchain
 * has no depth attachment. A draw never silently loses state.
 *
 * COMMAND BUFFER RULES. The window frame hook records INSIDE the swapchain render pass, so a draw there may bind, set state
 * and draw but never copy or begin a pass. T792 therefore uploads with its OWN one time command buffer, vkQueueSubmit and
 * wait (legal while another command buffer records), before `bind` returns. A T793 offscreen draw is recorded into the
 * target's own command buffer from `begin` (already inside its pass), and `end` is where it may close the pass and submit.
 *
 * REFUSALS. Every hook failure is a named reason, the draw is NOT drawn (no stand-in), and it is recorded in the census of
 * live_pipeline.h with stage LIVE_STAGE_DEVICE.
 */
#ifndef TSFP_LIVE_VK_PIPELINE_H
#define TSFP_LIVE_VK_PIPELINE_H

#include "gpu_window.h" /* defines VK_NO_PROTOTYPES, includes vulkan_core.h */
#include "gpu_device_native.h"
#include "gpu_pgraph.h"
#include "gpu_pgraph_replay.h"
#include "live_pipeline.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LIVE_VK_ARENA_BYTES (48u * 1024u * 1024u) /* T1339: the size of one arena block, more blocks are made on demand */
#define LIVE_VK_ARENA_MAX_BLOCKS 32u              /* 1.5 GiB of host visible memory at most */

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t *map;
    size_t bytes, used;
} live_vk_arena_block;
#define LIVE_VK_MAX_DRAWS_PER_FRAME 2048u
#define LIVE_VK_TEXTURE_STAGES 4u

typedef struct {
    PFN_vkGetDeviceProcAddr get_device_proc_addr;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool command_pool;
    VkPhysicalDeviceMemoryProperties memory_properties;
    bool negative_viewport; /* VK_KHR_maintenance1 enabled: a viewport may have a negative height (flip_y, T828) */
    bool occlusion_query_precise; /* T998: precise sample counts supported and enabled. */
    bool fill_mode_non_solid; /* T860: fillModeNonSolid enabled: a LINE or POINT polygon mode pipeline can be made */
    bool large_points; /* T1039: largePoints actually supported and enabled. */
} live_vk_device;

typedef enum {
    LIVE_VK_PASS_WINDOW = 0,
    LIVE_VK_PASS_OFFSCREEN,
    LIVE_VK_PASS_TARGET, /* T829: BGRA8 plus D32S8, the guest A8R8G8B8 byte order of a T793 target, colour layout GENERAL */
    LIVE_VK_PASS_COUNT
} live_vk_pass_kind;

/* One texture stage's image, sampled in `layout` (0 is VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL). Whether the stage's
 * coordinate is in texels is decided at planning (live_pipeline_description.texel_stages), the fragment module is rewritten
 * for it when the pipeline is built. */
typedef struct {
    VkImageView view;
    VkSampler sampler;
    VkImageLayout layout;
} live_vk_texture_binding;

typedef struct {
    /* `draw` is the index in the model, `state` the draw's snapshot, `stage` 0..3 a stage the combiner samples. Return false
     * with `error` filled to refuse the draw. NULL: a draw whose combiner samples any stage is refused. */
    bool (*bind)(void *context, size_t draw, const gpu_pgraph_state *state, uint32_t stage, live_vk_texture_binding *out,
                 char *error, size_t error_bytes);
    void *context;
} live_vk_texture_hook;

/* Where one draw goes. A NULL hook, or `begin` returning with out->command_buffer NULL, is the window frame. */
typedef struct {
    VkCommandBuffer command_buffer; /* already recording, inside `render_pass` */
    live_vk_pass_kind pass_kind;    /* which pipeline cache: must match the pass the buffer is inside */
    uint32_t width, height;         /* target pixels, the viewport and the scissor bound */
} live_vk_draw_target;

typedef struct {
    bool (*begin)(void *context, size_t draw, const gpu_pgraph_state *state, live_vk_draw_target *out, char *error,
                  size_t error_bytes);
    void (*end)(void *context, size_t draw, const live_vk_draw_target *target);
    void *context;
} live_vk_target_hook;

typedef struct {
    uint64_t drawn;        /* draws recorded */
    uint64_t degenerate;   /* draws that expanded to no primitive */
    uint64_t refused;      /* draws refused at any stage (census holds the names) */
    uint64_t vertices;
    uint64_t arena_peak;   /* most arena bytes one frame used, over all blocks */
    uint64_t arena_capacity; /* T1339: bytes of every arena block now allocated */
    uint32_t arena_blocks;   /* T1339: blocks allocated (1 until a frame outgrew one) */
    uint64_t frames;         /* T1339: frames begun */
    uint64_t frames_with_refusals; /* T1339: frames with at least one refused draw (any stage) */
    uint64_t first_refused_frame;  /* T1339: 1 based index of the first of those, 0 when none */
    uint32_t most_refused_in_frame;
    uint64_t pipelines_created;
    uint64_t pipeline_create_calls; /* T1247: every vkCreateGraphicsPipelines that succeeded, the clear pipelines included */
    uint64_t pipeline_create_ns, pipeline_create_worst_ns; /* T1247: wall time inside vkCreateGraphicsPipelines, summed and the slowest one */
    uint64_t pipeline_cache_loaded_bytes, pipeline_cache_saved_bytes, pipeline_cache_saves; /* T1247: the on disk VkPipelineCache */
    uint32_t used_inferences; /* OR over drawn draws, includes the rewrites the device stage applied */
    uint64_t clears_applied;  /* CLEAR_SURFACE events recorded (T828), including a flags 0 event that clears nothing */
    uint64_t clears_masked;   /* of those, colour clears with a partial channel mask, recorded as a masked triangle */
    uint64_t clears_refused;  /* events refused or only partly applied, live_vk_renderer_clear_refusal names the last */
} live_vk_stats;

typedef struct live_vk_renderer live_vk_renderer;

/* Adapters. false when a handle is missing. */
bool live_vk_device_from_window(const gpu_window_native *native, live_vk_device *out);
bool live_vk_device_from_gpu_device(const gpu_device_native *native, live_vk_device *out);

/* `window_pass` is the swapchain pass (VK_NULL_HANDLE: the window kind refuses every draw), `window_depth` is always false
 * today. `backend` is copied; its pointers (table, module loaders, read_guest, contexts) must outlive the renderer. The
 * renderer owns the offscreen pass, the arena, the descriptor pool and layouts, and a pipeline cache per pass kind. Refuses
 * (NULL, `error` filled) a backend with flip_y on a device without `negative_viewport`. flip_y (T828) is applied by a negative
 * viewport height: the renderer draws straight into the finished (reversed) image, so the scissor, the clear rectangle and the
 * winding are the ones of the finished image, which is what gpu_pgraph_replay's mirrored rectangle and swapped winding mean. */
live_vk_renderer *live_vk_renderer_create(const live_vk_device *device, VkRenderPass window_pass,
                                          const gpu_pgraph_backend *backend, char *error, size_t error_bytes);
void live_vk_renderer_destroy(live_vk_renderer *renderer);
/* T1247: a VkPipelineCache persisted in `path` (the file is read now, an unreadable, absent or foreign one, as the driver validates its
 * header against the device, starts an empty cache). Every pipeline the renderer makes after this goes through it. The data is written back
 * by live_vk_renderer_pipeline_cache_save and at destroy, to a temporary file renamed over `path`. False when the device has no
 * pipeline cache entry points or the cache could not be created (pipelines are then made without a cache, as before). */
bool live_vk_renderer_pipeline_cache_open(live_vk_renderer *renderer, const char *path);
/* Write the cache when pipelines were created since the last save. True when written. */
bool live_vk_renderer_pipeline_cache_save(live_vk_renderer *renderer);

void live_vk_renderer_set_texture_hook(live_vk_renderer *renderer, const live_vk_texture_hook *hook);
void live_vk_renderer_set_target_hook(live_vk_renderer *renderer, const live_vk_target_hook *hook);
/* T1206: the draw's command buffer AFTER the texture hook ran (a feedback snapshot closes and reopens the run, so the buffer the
 * target hook's `begin` gave can be stale). Used for LIVE_VK_PASS_TARGET draws, a NULL or VK_NULL_HANDLE result keeps the buffer
 * `begin` gave. */
void live_vk_renderer_set_target_command(live_vk_renderer *renderer, VkCommandBuffer (*command)(void *context), void *context);

/* The pass T793 builds its framebuffers against, valid until the renderer is destroyed. */
VkRenderPass live_vk_renderer_offscreen_pass(const live_vk_renderer *renderer);
/* The colour format of the pass kind: RGBA8 for OFFSCREEN, B8G8R8A8 for TARGET (the swapchain's is T790's). */
VkFormat live_vk_pass_colour_format(live_vk_pass_kind kind);
/* T829: the BGRA8 + D32S8 pass live_vk_draw (live_vk_draw.h) builds its framebuffers against (colour attachment 0, depth 1). */
VkRenderPass live_vk_renderer_target_pass(const live_vk_renderer *renderer);

/* Start a frame: resets the draw arena and the descriptor pool. The previous frame's work MUST have finished (the window
 * waits its fence first). */
void live_vk_renderer_begin_frame(live_vk_renderer *renderer);

/* T1339: the usual arena block size (default LIVE_VK_ARENA_BYTES) and the most blocks (default and cap LIVE_VK_ARENA_MAX_BLOCKS), for
 * tests. A draw bigger than a block gets a block of its own size. Only before the first frame; remakes block 0. */
bool live_vk_renderer_set_arena_limits(live_vk_renderer *renderer, size_t block_bytes, uint32_t max_blocks);

/* Draw draw `draw_index` of `pgraph`. `window_command_buffer`, `width` and `height` are the window frame's (used when the
 * target hook names no buffer). Returns true when the draw was recorded or was degenerate (nothing to draw), false when it
 * was refused: `error` then names why and the census holds it. */
bool live_vk_renderer_draw(live_vk_renderer *renderer, const gpu_pgraph *pgraph, size_t draw_index,
                           VkCommandBuffer window_command_buffer, uint32_t width, uint32_t height, char *error,
                           size_t error_bytes);

/* T828: apply CLEAR_SURFACE event `clear_index` of `pgraph` to the target, inside its render pass (the window frame hook or a T793
 * target's pass). The rules are gpu_pgraph_replay's (gpu_pgraph_resolve_clear: flags, rectangle, colour channels, D24 depth value
 * as a float, stencil byte, the CLEAR output group and the INFERRED clear model from `allowed_inferences`); the write masks of
 * the state do not matter. Colour with all four channels, depth and stencil are one vkCmdClearAttachments with the rectangle,
 * a partial channel mask is a target triangle with that colour write mask scissored to the rectangle. A depth or stencil clear
 * on a target kind without a depth attachment (LIVE_VK_PASS_WINDOW) applies its colour part, is refused by name for the rest and
 * returns false. Returns true when the event was applied. `target` carries the command buffer, pass kind and pixel size. */
bool live_vk_renderer_clear(live_vk_renderer *renderer, const gpu_pgraph *pgraph, size_t clear_index,
                            const live_vk_draw_target *target, char *error, size_t error_bytes);
/* The reason of the last refused clear, "" when none was. */
const char *live_vk_renderer_clear_refusal(const live_vk_renderer *renderer);

live_vk_stats live_vk_renderer_stats(const live_vk_renderer *renderer);
/* The census over every draw of every frame since create (live_pipeline_census_print prints it). */
const live_pipeline_census *live_vk_renderer_census(const live_vk_renderer *renderer);
const live_pipeline_cache *live_vk_renderer_cache(const live_vk_renderer *renderer, live_vk_pass_kind kind);

/* live_pipeline_ops of one pass kind, the device seam live_pipeline_cache_init takes. `context` of the returned ops is
 * the renderer's; exposed so a test can build a cache on a real device without the draw loop. */
live_pipeline_ops live_vk_renderer_pipeline_ops(live_vk_renderer *renderer, live_vk_pass_kind kind);

/* T998: optional real per-draw sample counter. Lifetime belongs to the frame. */
typedef struct live_vk_query live_vk_query;
void live_vk_renderer_set_query(live_vk_renderer *renderer, live_vk_query *query, bool enabled);
uint32_t live_vk_renderer_query_slot(const live_vk_renderer *renderer);

#endif
