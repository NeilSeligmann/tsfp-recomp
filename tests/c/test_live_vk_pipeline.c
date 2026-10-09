/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T791, the Vulkan half (src/gpu/live_vk_pipeline.c): a pipeline built from a real PGRAPH snapshot draws the SAME PIXELS as
 * the M2 replay (gpu_pgraph_replay) on a real (software) device, for opaque, blended, depth tested, stencilled, culled,
 * scissored, alpha tested and line draws, in the window kind (colour only pass) and the offscreen kind (colour plus depth).
 *
 * The test owns a minimal render target (image, framebuffer, command buffer, readback) because the production target is
 * T793's. Skips with exit code 77 and a stated note when no Vulkan device exists. `--png DIR` writes the live and replay
 * images of every scene to look at.
 */
#define _POSIX_C_SOURCE 200809L
#define VK_NO_PROTOTYPES

#include "gpu_device.h"
#include "gpu_device_native.h"
#include "gpu_png.h"
#include "live_vk_pipeline.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;
static const char *png_directory;

#define CHECK(condition)                                                                      \
    do {                                                                                      \
        checks++;                                                                             \
        if (!(condition)) {                                                                   \
            failures++;                                                                       \
            printf("  FAIL line %d: %s\n", __LINE__, #condition);                             \
        }                                                                                     \
    } while (0)

#include "live_vk_scenes.h"

#include "live_vk_rig.h"

static void write_png(const char *scene_name, const char *which, const gpu_image *image)
{
    if (png_directory == NULL || image->pixels == NULL) {
        return;
    }
    char path[512];
    snprintf(path, sizeof path, "%s/%s-%s.png", png_directory, scene_name, which);
    (void)gpu_png_write_rgba(path, image->pixels, image->width, image->height, image->stride_bytes);
}

/* --- tests -------------------------------------------------------------------------------- */

static const float clear_colour[4] = {0.2f, 0.4f, 0.6f, 1.0f};

static void test_parity(rig *r, gpu_device *device)
{
    printf("pixel parity with gpu_pgraph_replay\n");
    char error[256] = "";
    gpu_pgraph_backend probe = make_backend(NULL, EVERYTHING);
    live_vk_renderer *renderer = live_vk_renderer_create(&r->device, r->colour_pass, &probe, error, sizeof error);
    CHECK(renderer != NULL);
    if (renderer == NULL) {
        printf("  %s\n", error);
        return;
    }
    live_vk_renderer_destroy(renderer);
    size_t covered_offscreen[SCENE_COUNT] = {0};
    for (int scene_index = 0; scene_index < SCENE_COUNT; scene_index++) {
        const scene_kind kind = (scene_kind)scene_index;
        printf("scene %s\n", scene_names[kind]);
        scene s = make_scene(kind);
        gpu_pgraph_backend backend = make_backend_for(kind, &s.guest, EVERYTHING);
        /* The live frames are rendered BEFORE the replay: undefined dynamic state (a draw that never set its blend constants)
         * would otherwise inherit the replay's pipeline constants through the driver's queue state and hide the mutant. */
        frame_result lives[LIVE_VK_PASS_COUNT];
        memset(lives, 0, sizeof lives);
        bool twovary_reported[LIVE_VK_PASS_COUNT] = {false, false};
        bool device_census_ok[LIVE_VK_PASS_COUNT] = {true, true};
        live_vk_stats pass_stats[LIVE_VK_PASS_COUNT];
        uint32_t clear_inferences[LIVE_VK_PASS_COUNT] = {0u, 0u};
        memset(pass_stats, 0, sizeof pass_stats);
        for (int pass_index = 0; pass_index <= LIVE_VK_PASS_OFFSCREEN; pass_index++) {
            const live_vk_pass_kind pass = (live_vk_pass_kind)pass_index;
            live_vk_renderer *pass_renderer = live_vk_renderer_create(&r->device, r->colour_pass, &backend, error, sizeof error);
            CHECK(pass_renderer != NULL);
            if (pass_renderer == NULL) {
                continue;
            }
            render_live(r, pass_renderer, pass, s.model, clear_colour, &lives[pass_index]);
            twovary_reported[pass_index] =
                (live_vk_renderer_stats(pass_renderer).used_inferences & GPU_PGRAPH_INFER_OUTPUT_UNWRITTEN_VARYING) != 0u;
            device_census_ok[pass_index] =
                live_vk_renderer_census(pass_renderer)->by_stage[LIVE_STAGE_DEVICE] == lives[pass_index].refused;
            pass_stats[pass_index] = live_vk_renderer_stats(pass_renderer);
            clear_inferences[pass_index] = live_vk_renderer_census(pass_renderer)->used_inferences;
            live_vk_renderer_destroy(pass_renderer);
        }
        gpu_image expected = {0};
        gpu_pgraph_report report;
        memset(&report, 0, sizeof report);
        const gpu_pgraph_result replayed =
            gpu_pgraph_replay(s.model, device, &backend, WIDTH, HEIGHT, clear_colour, &expected, &report);
        const bool polygon_zero_refused = kind == SCENE_POLYGON_ZERO;
        CHECK(replayed == (polygon_zero_refused ? GPU_PGRAPH_ERR_UNMEASURED : GPU_PGRAPH_OK));
        if (replayed != GPU_PGRAPH_OK) {
            printf("  replay refused: %s\n", report.error);
        }
        write_png(scene_names[kind], "replay", &expected);
        for (int pass_index = 0; pass_index <= LIVE_VK_PASS_OFFSCREEN; pass_index++) {
            const live_vk_pass_kind pass = (live_vk_pass_kind)pass_index;
            frame_result *live = &lives[pass_index];
            const bool refused_expected = polygon_zero_refused || (pass == LIVE_VK_PASS_WINDOW && scene_needs_depth(kind));
            printf("  %s pass: drawn %u refused %u covered %zu\n", pass == LIVE_VK_PASS_WINDOW ? "window" : "offscreen",
                   live->drawn, live->refused, count_covered(&live->image, clear_colour));
            const size_t clear_events = gpu_pgraph_clear_count(s.model);
            if (pass == LIVE_VK_PASS_WINDOW && scene_clears_depth(kind)) {
                /* the clear's depth or stencil part is refused by name in the colour only pass, its colour part is applied */
                CHECK(live->clears_refused == clear_events && strstr(live->first_clear_refusal, "no depth attachment") != NULL);
            } else {
                CHECK(live->clears_refused == 0u && live->clears_applied == clear_events);
                CHECK(clear_events == (size_t)report.clears_applied);
            }
            /* the renderer counts what it applied, a partial channel mask is the masked triangle, the INFERRED clear model the
             * scene allowed is reported like the replay reports it */
            CHECK(pass_stats[pass_index].clears_applied == clear_events);
            CHECK(pass_stats[pass_index].clears_refused == live->clears_refused);
            CHECK(pass_stats[pass_index].clears_masked == (kind == SCENE_CLEAR_MASK ? 2u : 0u));
            if (clear_events != 0u) {
                CHECK((report.used_inferences & GPU_PGRAPH_INFER_OUTPUT_CLEAR_MODEL) != 0u);
                CHECK((pass_stats[pass_index].used_inferences & GPU_PGRAPH_INFER_OUTPUT_CLEAR_MODEL) != 0u);
                CHECK((clear_inferences[pass_index] & GPU_PGRAPH_INFER_OUTPUT_CLEAR_MODEL) != 0u);
            }
            if (refused_expected) {
                /* Invalid polygon modes are refused in both live passes; the swapchain also has no depth. */
                CHECK(live->refused > 0u);
                CHECK(strstr(live->first_refusal,
                             polygon_zero_refused ? "none of POINT 0x1B00" : "no depth attachment") != NULL);
                if (!polygon_zero_refused) {
                    CHECK(device_census_ok[pass_index]);
                }
            } else {
                if (live->refused != 0u) {
                    printf("    refused: %s\n", live->first_refusal);
                }
                CHECK(live->refused == 0u);
                CHECK(replayed == GPU_PGRAPH_OK && live->image.pixels != NULL && expected.pixels != NULL);
                if (live->image.pixels != NULL && expected.pixels != NULL) {
                    const size_t different = count_differing(&live->image, &expected);
                    printf("    differing pixels vs replay: %zu\n", different);
                    CHECK(different == 0u);
                }
                /* something was drawn, so equality is not two empty frames */
                CHECK(count_covered(&live->image, clear_colour) > 0u);
                if (pass == LIVE_VK_PASS_OFFSCREEN) {
                    covered_offscreen[kind] = count_covered(&live->image, clear_colour);
                }
                write_png(scene_names[kind], pass == LIVE_VK_PASS_WINDOW ? "live-window" : "live-offscreen", &live->image);
            }
            if (kind == SCENE_COMBINER_TWOVARY && !refused_expected) {
                /* the combiner reads varyings the program never wrote: defaulted, an INFERRED rewrite that is reported */
                CHECK(twovary_reported[pass_index]);
                CHECK((report.used_inferences & GPU_PGRAPH_INFER_OUTPUT_UNWRITTEN_VARYING) != 0u);
            }
            gpu_image_free(&live->image);
        }
        gpu_image_free(&expected);
        gpu_pgraph_destroy(s.model);
    }
    /* T860/T885, expectations that do not come from the replay: a polygon mode of 0 is refused (it is not a valid encoding),
     * LINE draws only edges (fewer pixels than the fill, more than none) and POINT only the 3 vertices of each
     * of the two triangles (at most 6 pixels), and the alpha test in the combiner module drops the same pixels the fixed stage's
     * does: the combiner alpha scene shows the passing triangle only, so fewer pixels than the same two draws untested. */
    CHECK(covered_offscreen[SCENE_POLYGON_ZERO] == 0u);
    CHECK(covered_offscreen[SCENE_POLYGON_LINE] > 100u && covered_offscreen[SCENE_POLYGON_LINE] < covered_offscreen[SCENE_OPAQUE] / 2u);
    CHECK(covered_offscreen[SCENE_POLYGON_POINT] > 0u && covered_offscreen[SCENE_POLYGON_POINT] <= 6u);
    CHECK(covered_offscreen[SCENE_COMBINER_ALPHA] > 0u && covered_offscreen[SCENE_COMBINER_ALPHA] == covered_offscreen[SCENE_ALPHA]);
}

static void test_cache_and_census(rig *r)
{
    printf("cache and census\n");
    char error[256] = "";
    scene s = make_scene(SCENE_BLEND);
    gpu_pgraph_backend backend = make_backend(&s.guest, EVERYTHING);
    live_vk_renderer *renderer = live_vk_renderer_create(&r->device, r->colour_pass, &backend, error, sizeof error);
    CHECK(renderer != NULL);
    if (renderer == NULL) {
        return;
    }
    frame_result first, second;
    render_live(r, renderer, LIVE_VK_PASS_WINDOW, s.model, clear_colour, &first);
    live_vk_stats after_first = live_vk_renderer_stats(renderer);
    render_live(r, renderer, LIVE_VK_PASS_WINDOW, s.model, clear_colour, &second);
    live_vk_stats after_second = live_vk_renderer_stats(renderer);
    /* two draws, two keys (one blends), built once, then reused by the second frame */
    CHECK(after_first.pipelines_created == 2u);
    CHECK(after_second.pipelines_created == 2u);
    CHECK(after_second.drawn == 4u);
    CHECK(live_vk_renderer_cache(renderer, LIVE_VK_PASS_WINDOW)->hits == 2u);
    CHECK(live_vk_renderer_cache(renderer, LIVE_VK_PASS_WINDOW)->misses == 2u);
    CHECK(live_vk_renderer_census(renderer)->selected == 4u && live_vk_renderer_census(renderer)->count == 0u);
    CHECK(after_first.arena_peak > 0u);
    CHECK(after_second.arena_peak == after_first.arena_peak); /* the arena starts over every frame */
    CHECK(count_differing(&first.image, &second.image) == 0u);
    gpu_image_free(&first.image);
    gpu_image_free(&second.image);
    live_vk_renderer_destroy(renderer);
    /* the closed default: no inference allowed, the replay's own refusal reaches the census, nothing is drawn */
    gpu_pgraph_backend closed = make_backend(&s.guest, 0u);
    renderer = live_vk_renderer_create(&r->device, r->colour_pass, &closed, error, sizeof error);
    CHECK(renderer != NULL);
    if (renderer != NULL) {
        frame_result refused;
        render_live(r, renderer, LIVE_VK_PASS_WINDOW, s.model, clear_colour, &refused);
        CHECK(refused.drawn == 0u && refused.refused == 2u);
        CHECK(live_vk_renderer_census(renderer)->count == 2u && live_vk_renderer_census(renderer)->selected == 0u);
        CHECK(live_vk_renderer_stats(renderer).pipelines_created == 0u);
        CHECK(count_covered(&refused.image, clear_colour) == 0u);
        CHECK(live_vk_renderer_census(renderer)->used_inferences == 0u);
        gpu_image_free(&refused.image);
        live_vk_renderer_destroy(renderer);
    }
    gpu_pgraph_destroy(s.model);
}

/* A texture hook is required by a combiner draw: without one the draw is refused by name, never given a stand-in. The
 * decision lives in record(); exercised end to end by the scenes of the replay's combiner when T792 plugs in. Here the
 * hooks' plumbing: a target hook that refuses names its reason in the census, a hook that names no buffer is the window. */
typedef struct {
    int begins, ends;
    bool refuse;
} hook_counts;

/* T1339: the draw arena grows by blocks instead of refusing draws, rewinds every frame, and counts refused frames. */
static void test_arena_growth(rig *r)
{
    printf("arena growth\n");
    char error[256] = "";
    scene s = make_scene(SCENE_BLEND);
    gpu_pgraph_backend backend = make_backend(&s.guest, EVERYTHING);
    live_vk_renderer *renderer = live_vk_renderer_create(&r->device, r->colour_pass, &backend, error, sizeof error);
    CHECK(renderer != NULL);
    if (renderer == NULL) {
        return;
    }
    frame_result whole;
    render_live(r, renderer, LIVE_VK_PASS_WINDOW, s.model, clear_colour, &whole);
    const live_vk_stats one_block = live_vk_renderer_stats(renderer);
    CHECK(one_block.arena_blocks == 1u && one_block.refused == 0u && one_block.frames_with_refusals == 0u);
    live_vk_renderer_destroy(renderer);
    /* a block that holds one draw but not two: the second draw takes a second block, the picture is the same */
    renderer = live_vk_renderer_create(&r->device, r->colour_pass, &backend, error, sizeof error);
    CHECK(renderer != NULL && live_vk_renderer_set_arena_limits(renderer, one_block.arena_peak / 2u + 512u, LIVE_VK_ARENA_MAX_BLOCKS));
    for (unsigned frame = 0u; frame < 3u && renderer != NULL; frame++) {
        frame_result grown;
        render_live(r, renderer, LIVE_VK_PASS_WINDOW, s.model, clear_colour, &grown);
        const live_vk_stats stats = live_vk_renderer_stats(renderer);
        CHECK(grown.drawn == 2u && grown.refused == 0u && stats.refused == 0u);
        CHECK(stats.arena_blocks == 2u);                   /* made once, kept, never grown again by a later frame (the rewind) */
        CHECK(stats.arena_peak == one_block.arena_peak);   /* the same bytes as the single block run */
        CHECK(stats.arena_capacity == 2u * (one_block.arena_peak / 2u + 512u));
        CHECK(count_differing(&whole.image, &grown.image) == 0u);
        gpu_image_free(&grown.image);
    }
    live_vk_renderer_destroy(renderer);
    /* a block smaller than one draw: the draw gets a block of its own size, nothing is refused and the picture is the same */
    renderer = live_vk_renderer_create(&r->device, r->colour_pass, &backend, error, sizeof error);
    CHECK(renderer != NULL && live_vk_renderer_set_arena_limits(renderer, 1024u, LIVE_VK_ARENA_MAX_BLOCKS));
    if (renderer != NULL) {
        frame_result tiny;
        render_live(r, renderer, LIVE_VK_PASS_WINDOW, s.model, clear_colour, &tiny);
        const live_vk_stats stats = live_vk_renderer_stats(renderer);
        CHECK(tiny.drawn == 2u && tiny.refused == 0u && stats.arena_blocks == 3u); /* the 1 KiB first block stays empty */
        CHECK(count_differing(&whole.image, &tiny.image) == 0u);
        gpu_image_free(&tiny.image);
        live_vk_renderer_destroy(renderer);
    }
    /* a block limit of one: the second draw is refused, loudly counted per frame, and the next frame is counted again */
    renderer = live_vk_renderer_create(&r->device, r->colour_pass, &backend, error, sizeof error);
    CHECK(renderer != NULL && live_vk_renderer_set_arena_limits(renderer, one_block.arena_peak / 2u + 512u, 1u));
    for (unsigned frame = 1u; frame <= 2u && renderer != NULL; frame++) {
        frame_result limited;
        render_live(r, renderer, LIVE_VK_PASS_WINDOW, s.model, clear_colour, &limited);
        const live_vk_stats stats = live_vk_renderer_stats(renderer);
        CHECK(limited.drawn == 1u && limited.refused == 1u);
        CHECK(stats.frames_with_refusals == frame && stats.first_refused_frame == 1u && stats.most_refused_in_frame == 1u);
        gpu_image_free(&limited.image);
    }
    live_vk_renderer_destroy(renderer);
    gpu_image_free(&whole.image);
    gpu_pgraph_destroy(s.model);
}

static bool hook_begin(void *context, size_t draw, const gpu_pgraph_state *state, live_vk_draw_target *out, char *error,
                       size_t error_bytes)
{
    (void)draw;
    (void)state;
    (void)out;
    hook_counts *counts = context;
    counts->begins++;
    if (counts->refuse) {
        snprintf(error, error_bytes, "target hook: no render target bound");
        return false;
    }
    return true; /* out->command_buffer stays NULL: the window frame */
}

static void hook_end(void *context, size_t draw, const live_vk_draw_target *target)
{
    (void)draw;
    (void)target;
    ((hook_counts *)context)->ends++;
}

/* T1247: the on disk VkPipelineCache. Pipelines go through it, it is written once new pipelines exist, a later renderer starts from the file,
 * a garbage file is no obstacle, and none of it changes a pixel. */
static void test_pipeline_cache(rig *r)
{
    printf("pipeline cache\n");
    char error[256] = "";
    char directory[] = "/tmp/tsfp_pcache_XXXXXX";
    if (mkdtemp(directory) == NULL) {
        printf("  FAIL no scratch directory\n");
        failures++;
        return;
    }
    char path[300];
    snprintf(path, sizeof path, "%s/cache.bin", directory);
    scene s = make_scene(SCENE_BLEND);
    gpu_pgraph_backend backend = make_backend(&s.guest, EVERYTHING);
    live_vk_renderer *renderer = live_vk_renderer_create(&r->device, r->colour_pass, &backend, error, sizeof error);
    CHECK(renderer != NULL);
    if (renderer == NULL) {
        return;
    }
    CHECK(!live_vk_renderer_pipeline_cache_save(renderer)); /* no cache open: nothing to write */
    CHECK(live_vk_renderer_pipeline_cache_open(renderer, path));
    CHECK(!live_vk_renderer_pipeline_cache_open(renderer, path)); /* once */
    CHECK(live_vk_renderer_stats(renderer).pipeline_cache_loaded_bytes == 0u); /* the file did not exist */
    frame_result first;
    render_live(r, renderer, LIVE_VK_PASS_WINDOW, s.model, clear_colour, &first);
    const live_vk_stats built = live_vk_renderer_stats(renderer);
    CHECK(built.pipeline_create_calls >= 2u && built.pipeline_create_ns > 0u && built.pipeline_create_worst_ns <= built.pipeline_create_ns);
    CHECK(live_vk_renderer_pipeline_cache_save(renderer));
    CHECK(!live_vk_renderer_pipeline_cache_save(renderer)); /* nothing new since */
    FILE *file = fopen(path, "rb");
    long size = 0;
    if (file != NULL) {
        fseek(file, 0, SEEK_END);
        size = ftell(file);
        fclose(file);
    }
    CHECK(size > 0 && live_vk_renderer_stats(renderer).pipeline_cache_saved_bytes == (uint64_t)size);
    live_vk_renderer_destroy(renderer);
    /* a second renderer starts from the file and draws the same pixels */
    renderer = live_vk_renderer_create(&r->device, r->colour_pass, &backend, error, sizeof error);
    CHECK(renderer != NULL && live_vk_renderer_pipeline_cache_open(renderer, path));
    if (renderer != NULL) {
        frame_result second;
        CHECK(live_vk_renderer_stats(renderer).pipeline_cache_loaded_bytes == (uint64_t)size);
        render_live(r, renderer, LIVE_VK_PASS_WINDOW, s.model, clear_colour, &second);
        CHECK(count_differing(&first.image, &second.image) == 0u);
        gpu_image_free(&second.image);
        live_vk_renderer_destroy(renderer);
    }
    /* a garbage file is read but the driver refuses it as a header: the renderer still draws */
    file = fopen(path, "wb");
    const unsigned char junk[64] = {0xDE, 0xAD, 0xBE, 0xEF};
    CHECK(file != NULL && fwrite(junk, 1u, sizeof junk, file) == sizeof junk && fclose(file) == 0);
    renderer = live_vk_renderer_create(&r->device, r->colour_pass, &backend, error, sizeof error);
    CHECK(renderer != NULL && live_vk_renderer_pipeline_cache_open(renderer, path));
    if (renderer != NULL) {
        frame_result third;
        render_live(r, renderer, LIVE_VK_PASS_WINDOW, s.model, clear_colour, &third);
        CHECK(count_differing(&first.image, &third.image) == 0u);
        gpu_image_free(&third.image);
        live_vk_renderer_destroy(renderer);
    }
    gpu_image_free(&first.image);
    unlink(path);
    rmdir(directory);
}

static void test_hooks(rig *r)
{
    printf("target hook\n");
    char error[256] = "";
    scene s = make_scene(SCENE_OPAQUE);
    gpu_pgraph_backend backend = make_backend(&s.guest, EVERYTHING);
    live_vk_renderer *renderer = live_vk_renderer_create(&r->device, r->colour_pass, &backend, error, sizeof error);
    CHECK(renderer != NULL);
    if (renderer == NULL) {
        return;
    }
    hook_counts counts = {0, 0, false};
    const live_vk_target_hook hook = {hook_begin, hook_end, &counts};
    live_vk_renderer_set_target_hook(renderer, &hook);
    frame_result frame;
    render_live(r, renderer, LIVE_VK_PASS_WINDOW, s.model, clear_colour, &frame);
    CHECK(counts.begins == 2 && counts.ends == 0 && frame.drawn == 2u); /* no buffer named: the window, `end` not called */
    gpu_image_free(&frame.image);
    counts.refuse = true;
    render_live(r, renderer, LIVE_VK_PASS_WINDOW, s.model, clear_colour, &frame);
    CHECK(frame.drawn == 0u && frame.refused == 2u && strcmp(frame.first_refusal, "target hook: no render target bound") == 0);
    CHECK(live_vk_renderer_census(renderer)->by_stage[LIVE_STAGE_DEVICE] == 2u);
    CHECK(live_vk_renderer_census(renderer)->selected == 2u); /* only the first frame's two draws */
    CHECK(count_covered(&frame.image, clear_colour) == 0u);
    gpu_image_free(&frame.image);
    live_vk_renderer_destroy(renderer);
    gpu_pgraph_destroy(s.model);
    printf("flip_y and bad arguments\n");
    gpu_pgraph_backend flipped = make_backend(NULL, 0u);
    flipped.flip_y = true;
    /* the device needs VK_KHR_maintenance1 for the negative viewport: llvmpipe has it, a device without it is refused by name */
    CHECK(r->device.negative_viewport);
    live_vk_device plain = r->device;
    plain.negative_viewport = false;
    CHECK(live_vk_renderer_create(&plain, r->colour_pass, &flipped, error, sizeof error) == NULL &&
          strstr(error, "flip_y") != NULL);
    live_vk_renderer *flip_renderer = live_vk_renderer_create(&r->device, r->colour_pass, &flipped, error, sizeof error);
    CHECK(flip_renderer != NULL);
    live_vk_renderer_destroy(flip_renderer);
    CHECK(live_vk_renderer_create(NULL, r->colour_pass, &flipped, error, sizeof error) == NULL);
}

/* The refusals of a clear are the replay's own (gpu_pgraph_resolve_clear), named, nothing is cleared, the census of draws is
 * untouched, and a flags 0 event is applied as nothing. */
static void test_clear_refusals(rig *r, gpu_device *device)
{
    printf("clear refusals\n");
    char error[256] = "";
    struct {
        const char *name;
        const char *needle;
        uint32_t flags, colour, zstencil, x_max, y_max;
        uint32_t groups, inferences;
        bool rectangle;
        bool colour_written;
        bool applied;
    } const cases[] = {
        {"closed model", "INFERRED", 0xF0u, 0xFF112233u, 0u, 63u, 63u, ALL_GROUPS, 0u, true, true, false},
        {"no clear group", "output_groups", 0xF0u, 0xFF112233u, 0u, 63u, 63u, ALL_GROUPS & ~GPU_PGRAPH_OUTPUT_CLEAR, EVERYTHING, true, true, false},
        {"past the target", "reaches past", 0xF0u, 0xFF112233u, 0u, 64u, 63u, ALL_GROUPS, EVERYTHING, true, true, false},
        {"unknown flag bit", "flags 0x4", 0x04u, 0xFF112233u, 0u, 63u, 63u, ALL_GROUPS, EVERYTHING, true, true, false},
        {"nothing flagged", NULL, 0x00u, 0xFF112233u, 0u, 63u, 63u, ALL_GROUPS, EVERYTHING, true, true, true},
    };
    for (size_t index = 0u; index < sizeof cases / sizeof cases[0]; index++) {
        const float background[4] = {0.2f, 0.4f, 0.6f, 1.0f};
        gpu_pgraph *model = gpu_pgraph_create();
        gpu_pgraph_set_output_groups(model, ALL_GROUPS); /* the model records the event, the backend decides */
        stream_builder stream = {0};
        stream_setup(&stream);
        scene_clear(&stream, cases[index].flags, cases[index].colour, cases[index].zstencil, 0u, cases[index].x_max, 0u,
                    cases[index].y_max);
        CHECK(gpu_pgraph_decode(model, stream.pairs, stream.count) == GPU_PGRAPH_OK);
        stream_free(&stream);
        gpu_pgraph_backend backend = make_backend(NULL, cases[index].inferences);
        backend.output_groups = cases[index].groups;
        live_vk_renderer *renderer = live_vk_renderer_create(&r->device, r->colour_pass, &backend, error, sizeof error);
        CHECK(renderer != NULL && gpu_pgraph_clear_count(model) == 1u);
        if (renderer == NULL || gpu_pgraph_clear_count(model) != 1u) {
            gpu_pgraph_destroy(model);
            continue;
        }
        frame_result frame;
        render_live(r, renderer, LIVE_VK_PASS_WINDOW, model, background, &frame);
        gpu_pgraph_report report;
        memset(&report, 0, sizeof report);
        gpu_image expected = {0};
        const gpu_pgraph_result replayed = gpu_pgraph_replay(model, device, &backend, WIDTH, HEIGHT, background, &expected, &report);
        printf("  %s: applied %u refused %u, replay %s\n", cases[index].name, frame.clears_applied, frame.clears_refused,
               replayed == GPU_PGRAPH_OK ? "applied" : report.error);
        CHECK(cases[index].applied == (frame.clears_applied == 1u));
        CHECK(cases[index].applied == (replayed == GPU_PGRAPH_OK));
        CHECK(live_vk_renderer_stats(renderer).clears_refused == (cases[index].applied ? 0u : 1u));
        if (!cases[index].applied) {
            /* the replay's own message reaches the caller, and the target is left as it was */
            CHECK(strstr(frame.first_clear_refusal, cases[index].needle) != NULL);
            CHECK(strcmp(frame.first_clear_refusal, report.error) == 0);
            CHECK(strcmp(live_vk_renderer_clear_refusal(renderer), report.error) == 0);
        }
        CHECK(count_covered(&frame.image, background) == 0u);
        CHECK(live_vk_renderer_census(renderer)->count == 0u);
        gpu_image_free(&frame.image);
        gpu_image_free(&expected);
        live_vk_renderer_destroy(renderer);
        gpu_pgraph_destroy(model);
    }
}

int main(int argc, char **argv)
{
    for (int index = 1; index + 1 < argc; index++) {
        if (strcmp(argv[index], "--png") == 0) {
            png_directory = argv[index + 1];
        }
    }
    if (!gpu_vulkan_available()) {
        printf("SKIP: no Vulkan loader\n");
        return 77;
    }
    gpu_device *device = NULL;
    if (gpu_device_create_selected("software", &device) != GPU_OK) {
        printf("SKIP: no software Vulkan device\n");
        return 77;
    }
    fill_memory();
    rig r;
    if (!rig_init(&r, device)) {
        printf("FAIL: the Vulkan functions the test needs are missing\n");
        gpu_device_destroy(device);
        return 1;
    }
    printf("device %s\n", gpu_device_name(device));
    test_parity(&r, device);
    test_cache_and_census(&r);
    test_arena_growth(&r);
    test_pipeline_cache(&r);
    test_hooks(&r);
    test_clear_refusals(&r, device);
    r.fn.vkDestroyRenderPass(r.native.device, r.colour_pass, NULL);
    gpu_device_destroy(device);
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
