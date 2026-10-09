/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "guest_frame_trace.h"
#include "d3d8_swap_replay.h"
#include "live_texture.h"

#include "d3d8_guest.h"
#include "d3d8_gpu.h"
#include "d3d8_gpu_pgraph.h"
#include "d3d8_hle.h"
#include "d3d8_overlay.h"
#include "d3d8_overlay_key.h"
#include "d3d8_pushbuffer.h"
#include "d3d8_surface.h"
#include "d3d8_surface_model.h"
#include "gpu_png.h"
#include "gpu_pgraph.h"
#include "gpu_vsh_select.h"
#include "guest_mem.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SWAP_ADDRESS 0x003D8E50u
#define DEV_RENDER_TARGET 0x1A04u
#define SPV_MAGIC 0x07230203u
#define COMBINER_MODULE_PREFIX "combiner_" /* gpu_combiner.h: combiner_<sha256> */
#define MAX_SPV_BYTES (16u * 1024u * 1024u)
#define MAX_SWITCHES 64u
#define MAX_PASSES (MAX_SWITCHES + 1u)

typedef struct {
    bool measured;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;  /* T510: the header's pitch in bytes, 0 when unmeasured */
    uint32_t format; /* T510: the header's Format word, 0 when unmeasured */
} target_size;

/* A SetRenderTarget that changed the bound target: where in the recording it happened and what was
 * bound on each side, with each side's size measured THEN (the header is live then, and may be
 * released or reused by the present). */
typedef struct {
    size_t stream_index;
    size_t draws;        /* draws the model held at the switch: the next pass starts here (T262) */
    size_t clears;       /* clear events (T267) the model held at the switch: the next pass's clears start here */
    bool bracket_open;   /* a BEGIN_END bracket was open at the switch */
    uint32_t from;
    uint32_t to;
    uint32_t from_data; /* T578: the Data word of each header at the switch, the name a 2D blit gives a surface */
    uint32_t to_data;
    target_size from_size;
    target_size to_size;
} target_switch;

/* One replayed pass: the draws between two switches, in an image of that target's size. */
typedef struct {
    uint32_t target;
    uint32_t data;       /* T578: the target header's Data word (28 bit physical address), 0 when unknown */
    size_t first_draw;   /* T578: the draws [first_draw, end_draw) of the frame's list this pass replayed */
    size_t end_draw;
    uint32_t width;
    uint32_t height;
    uint32_t drawn;
    uint32_t used_inferences;
    uint32_t clears_applied;
    uint32_t offset_applied;    /* T511 */
    uint32_t offset_unobserved; /* T511 */
    uint32_t textured_draws;
    target_size size;           /* T633: the header's measured size, pitch and Format when the pass was planned */
    gpu_image image;
} pass_result;

/* T578: the last replayed image of a surface, kept across frames so a blit can read what an earlier frame drew. */
typedef struct {
    bool valid;
    bool touched; /* a blit wrote it this frame (dumped) */
    bool guest_built; /* T596: built from the guest memory rows a blit touched (partial: only those rows), not from a replayed pass */
    uint32_t data;
    uint32_t pitch;  /* T633: the header's pitch and Format of the pass that left it, 0 when guest built (unknown) */
    uint32_t format;
    gpu_image image;
} stored_surface;

/* T510: a SetTexture binding as the header was when the title bound it. */
typedef struct {
    bool bound;      /* false: the stage was unbound (SetTexture 0) */
    uint32_t header;
    uint32_t data;   /* the Data word, 28 bit physical address */
    uint32_t format; /* the Format word */
    uint32_t size_word;
    bool measured;   /* the Size word names a linear width and height */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;  /* from the Size word, bytes */
} texture_binding;

/* A binding noted at `draw`: it applies to the draws from that index of the frame's list on. */
typedef struct {
    size_t draw;
    uint32_t stage;
    texture_binding binding;
} texture_event;

/* One pass of the frame as it will be replayed (only the segments that have draws), known before the first one runs so a
 * texture can tell an earlier producer from a later one. */
typedef struct {
    uint32_t target;
    uint32_t data;
    target_size size;
    size_t first;
    size_t end;
} segment_plan;

typedef struct {
    char text[260];
    uint64_t draws;
} texture_census_line;

/* The `.spv` files of one kind in the module directory: sorted stem names (the selector table's names), and
 * the words of each, read the first time the module is drawn. */
typedef struct {
    char **names;
    uint32_t count;
    struct gpu_vsh_table table;
    uint32_t **words;
    size_t *word_counts;
} module_set;

typedef struct {
    bool active;
    d3d8_swap_replay_config config;
    char *spv_directory;
    char *dump_directory;
    char *device_selector;
    uint8_t *standin_pixels; /* T497: the stand-in texture, standin_width x standin_height of one colour, RGBA8 */
    module_set vertex;   /* static_<sha256> and generated_<sha256>, the vertex programs */
    module_set fragment; /* combiner_<sha256> (T478), the translated register combiner stages */
    gpu_pgraph *pgraph;
    uint64_t gpu_resets_seen; /* d3d8_gpu_reset_count when the model was built */
    size_t next;
    bool decode_failed;          /* a kick's decode was refused: the frame is refused at the present */
    char decode_error[sizeof ((d3d8_swap_replay_stats *)0)->error];
    uint64_t vertex_bytes_folded; /* of the model's counter, already in stats.vertex_bytes_captured */
    uint64_t vertex_draws_folded;
    uint64_t pairs_ignored_folded; /* T511: of the model's counter, already in stats.pairs_ignored */
    size_t unhandled_logged;
    gpu_device *device;
    gpu_image last;
    uint64_t last_frame;           /* T484: the present number of `last`, and whether it was written */
    bool last_dumped;
    uint64_t offscreen_frame;      /* the present number of the offscreen images, and whether they were written */
    bool offscreen_dumped;
    target_switch switches[MAX_SWITCHES];
    size_t switch_count;
    bool switch_overflow;
    pass_result offscreen[MAX_PASSES];
    size_t offscreen_count;
    texture_event texture_events[D3D8_SWAP_REPLAY_TEXTURE_EVENTS]; /* T510: this frame's bindings, in call order */
    size_t texture_event_count;
    bool texture_overflow;
    texture_binding texture_carry[4]; /* the binding each stage had when the last frame ended */
    bool texture_carry_known[4];
    texture_census_line texture_census[D3D8_SWAP_REPLAY_TEXTURE_CENSUS];
    size_t texture_census_count;
    uint64_t texture_census_overflow;
    stored_surface surfaces[D3D8_SWAP_REPLAY_SURFACES]; /* T578, only with the BLIT group */
    size_t surfaces_next;                              /* the slot the next eviction takes */
    uint8_t *surface_buffer[4];                        /* T633 (1): the RGBA image of the surface each texture stage samples, from guest memory */
    size_t surface_buffer_bytes[4];
    d3d8_swap_replay_stats stats;
} replay_state;

static replay_state state;
static d3d8_swap_replay_frame_hook live_frame_hook;
static void *live_frame_hook_context;
static d3d8_swap_replay_live_hook live_model_hook; /* T838 */
static void *live_model_hook_context;
static d3d8_swap_replay_live_target_hook live_target_hook;
static void *live_target_hook_context;
static uint64_t live_model_frames;
static gpu_pgraph_module_make_fn live_module_maker; /* T847 */
static void *live_module_maker_context;

static int compare_names(const void *left, const void *right)
{
    return strcmp(*(const char *const *)left, *(const char *const *)right);
}

static char *copy_string(const char *text)
{
    if (text == NULL) {
        return NULL;
    }
    char *copy = malloc(strlen(text) + 1u);
    if (copy != NULL) {
        memcpy(copy, text, strlen(text) + 1u);
    }
    return copy;
}

static void fail(char *error, size_t error_size, const char *format, const char *detail)
{
    if (error != NULL && error_size != 0u) {
        snprintf(error, error_size, format, detail);
    }
}

/* --- the module directory --------------------------------------------------------------- */

/* The stem of a `<stem>.spv` entry, or NULL. */
static char *spv_stem(const char *file_name)
{
    const size_t length = strlen(file_name);
    if (length <= 4u || strcmp(file_name + length - 4u, ".spv") != 0) {
        return NULL;
    }
    char *stem = malloc(length - 3u);
    if (stem != NULL) {
        memcpy(stem, file_name, length - 4u);
        stem[length - 4u] = '\0';
    }
    return stem;
}

static bool is_fragment_name(const char *stem)
{
    return strncmp(stem, COMBINER_MODULE_PREFIX, strlen(COMBINER_MODULE_PREFIX)) == 0;
}

static void free_module_set(module_set *set)
{
    for (uint32_t i = 0u; i < set->count; i++) {
        free(set->names[i]);
        if (set->words != NULL) {
            free(set->words[i]);
        }
    }
    free(set->names);
    free(set->words);
    free(set->word_counts);
    memset(set, 0, sizeof *set);
}

/* Sort, build the selector table and the lazy word cache. Takes the ownership of `names`. */
static bool adopt_module_set(module_set *set, char **names, uint32_t count)
{
    if (count != 0u) {
        qsort(names, count, sizeof *names, compare_names);
    }
    set->names = names;
    set->count = count;
    set->table = (struct gpu_vsh_table){0u, 0u, NULL, 0u, NULL, count, (const char *const *)names};
    set->words = calloc(count != 0u ? count : 1u, sizeof *set->words);
    set->word_counts = calloc(count != 0u ? count : 1u, sizeof *set->word_counts);
    return set->words != NULL && set->word_counts != NULL;
}

/* The directory's `.spv` files: the vertex programs, and (T478) the `combiner_<sha256>` fragment stages the
 * combiner option selects from. A combiner module is never a candidate for a vertex program. */
static bool list_modules(const char *directory, bool combiner, char *error, size_t error_size)
{
    DIR *handle = opendir(directory);
    if (handle == NULL) {
        fail(error, error_size, "cannot open the module directory %s", directory);
        return false;
    }
    size_t capacity = 64u;
    char **vertex = malloc(capacity * sizeof *vertex);
    char **fragment = malloc(capacity * sizeof *fragment);
    uint32_t vertex_count = 0u;
    uint32_t fragment_count = 0u;
    bool ok = vertex != NULL && fragment != NULL;
    for (struct dirent *entry = ok ? readdir(handle) : NULL; entry != NULL; entry = readdir(handle)) {
        char *stem = spv_stem(entry->d_name);
        if (stem == NULL) {
            continue;
        }
        if (vertex_count == capacity || fragment_count == capacity) {
            capacity *= 2u;
            char **grown_vertex = realloc(vertex, capacity * sizeof *vertex);
            vertex = grown_vertex != NULL ? grown_vertex : vertex;
            char **grown_fragment = realloc(fragment, capacity * sizeof *fragment);
            fragment = grown_fragment != NULL ? grown_fragment : fragment;
            if (grown_vertex == NULL || grown_fragment == NULL) {
                free(stem);
                ok = false;
                break;
            }
        }
        if (is_fragment_name(stem)) {
            fragment[fragment_count++] = stem;
        } else {
            vertex[vertex_count++] = stem;
        }
    }
    closedir(handle);
    if (!ok || vertex_count + fragment_count == 0u) {
        for (uint32_t i = 0u; i < vertex_count; i++) {
            free(vertex[i]);
        }
        for (uint32_t i = 0u; i < fragment_count; i++) {
            free(fragment[i]);
        }
        free(vertex);
        free(fragment);
        fail(error, error_size, ok ? "no .spv module in %s" : "out of memory listing %s", directory);
        return false;
    }
    if (combiner && fragment_count == 0u) {
        for (uint32_t i = 0u; i < vertex_count; i++) {
            free(vertex[i]);
        }
        free(vertex);
        free(fragment);
        fail(error, error_size, "the combiner is on and there is no combiner_<sha256>.spv module in %s", directory);
        return false;
    }
    const bool vertex_ok = adopt_module_set(&state.vertex, vertex, vertex_count);
    const bool fragment_ok = adopt_module_set(&state.fragment, fragment, fragment_count);
    return vertex_ok && fragment_ok;
}

static bool load_module_words(module_set *set, uint32_t module, const uint32_t **words, size_t *word_count)
{
    if (module >= set->count) {
        return false;
    }
    if (set->words[module] == NULL) {
        const size_t length = strlen(state.spv_directory) + strlen(set->names[module]) + 6u;
        char *path = malloc(length);
        if (path == NULL) {
            return false;
        }
        snprintf(path, length, "%s/%s.spv", state.spv_directory, set->names[module]);
        FILE *file = fopen(path, "rb");
        free(path);
        if (file == NULL) {
            return false;
        }
        uint32_t *buffer = NULL;
        size_t bytes = 0u;
        if (fseek(file, 0, SEEK_END) == 0) {
            const long end = ftell(file);
            if (end > 0 && (size_t)end <= MAX_SPV_BYTES && end % 4 == 0 &&
                fseek(file, 0, SEEK_SET) == 0) {
                bytes = (size_t)end;
                buffer = malloc(bytes);
            }
        }
        const bool read = buffer != NULL && fread(buffer, 1u, bytes, file) == bytes;
        fclose(file);
        if (!read || buffer[0] != SPV_MAGIC) {
            free(buffer);
            return false;
        }
        set->words[module] = buffer;
        set->word_counts[module] = bytes / 4u;
    }
    *words = set->words[module];
    *word_count = set->word_counts[module];
    return true;
}

static bool load_module(void *context, uint32_t module, const uint32_t **words, size_t *word_count)
{
    (void)context;
    return load_module_words(&state.vertex, module, words, word_count);
}

/* Each module's words stay cached for the run, so the vertex module's words remain valid while a fragment
 * module is loaded (gpu_pgraph_backend.load_fragment_module needs that). */
static bool load_fragment_module(void *context, uint32_t module, const uint32_t **words, size_t *word_count)
{
    (void)context;
    return load_module_words(&state.fragment, module, words, word_count);
}

/* --- lifecycle -------------------------------------------------------------------------- */

d3d8_swap_replay_config d3d8_swap_replay_default_config(void)
{
    d3d8_swap_replay_config config;
    memset(&config, 0, sizeof config);
    config.strict = true;
    config.vertex_budget_bytes = D3D8_SWAP_REPLAY_DEFAULT_VERTEX_BUDGET;
    config.clear[3] = 1.0f;
    return config;
}

uint32_t d3d8_swap_replay_host_inferences(bool assume_program_mode, bool output_state, bool combiner,
                                          bool viewport_from_target, bool standin_texture)
{
    uint32_t allowed = GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_CMP_PACKED; /* T1204 */
    if (assume_program_mode) {
        allowed |= GPU_PGRAPH_INFER_EXECUTION_MODE_UNWRITTEN;
    }
    if (output_state) {
        allowed |= GPU_PGRAPH_INFER_OUTPUT_ALL;
        allowed |= GPU_PGRAPH_INFER_OUTPUT_BLIT_PLANNER_RULES;
    }
    if (combiner) {
        allowed |= GPU_PGRAPH_INFER_COMBINER_REPLAY;
    }
    if (viewport_from_target) {
        allowed |= GPU_PGRAPH_INFER_VIEWPORT_FROM_TARGET;
    }
    if (standin_texture) {
        allowed |= D3D8_SWAP_REPLAY_INFER_STANDIN_TEXTURE;
    }
    return allowed;
}

static void free_surfaces(void)
{
    for (size_t i = 0u; i < D3D8_SWAP_REPLAY_SURFACES; i++) {
        gpu_image_free(&state.surfaces[i].image);
        memset(&state.surfaces[i], 0, sizeof state.surfaces[i]);
    }
    state.surfaces_next = 0u;
    d3d8_surface_model_reset(); /* T596: the byte surfaces name the old surfaces too */
}

void d3d8_swap_replay_disable(void)
{
    free_surfaces();
    for (size_t i = 0u; i < 4u; i++) {
        free(state.surface_buffer[i]);
    }
    gpu_image_free(&state.last);
    for (size_t i = 0u; i < state.offscreen_count; i++) {
        gpu_image_free(&state.offscreen[i].image);
    }
    if (state.device != NULL) {
        gpu_device_destroy(state.device);
    }
    d3d8_gpu_set_recorded_observer(NULL);
    gpu_pgraph_destroy(state.pgraph);
    free_module_set(&state.vertex);
    free_module_set(&state.fragment);
    free(state.spv_directory);
    free(state.dump_directory);
    free(state.device_selector);
    free(state.standin_pixels);
    memset(&state, 0, sizeof state);
}

/* Capture allocation identity at decoding; the report cannot follow subsequent physical reuse. */
static void capture_report_identity(void *context, gpu_pgraph_query *event)
{
    (void)context;
    kernel_guest_ptr address;
    if (guest_virtual_from_physical(event->data & 0xFFFFFFu,&address)) {
        event->report_address = address;
        event->report_generation = guest_allocation_generation(address);
    }
}

/* Strictness and the vertex snapshot of a fresh model. */
static void configure_model(void)
{
    gpu_pgraph_set_strict(state.pgraph, state.config.strict);
    gpu_pgraph_set_visibility(state.pgraph,state.config.visibility);
    gpu_pgraph_set_report_identity(state.pgraph,capture_report_identity,NULL);
    gpu_pgraph_set_vertex_capture(state.pgraph, d3d8_gpu_read_guest, NULL,
                                  state.config.vertex_budget_bytes);
    gpu_pgraph_set_output_groups(state.pgraph, state.config.output_groups);
    gpu_pgraph_set_combiner(state.pgraph, state.config.combiner);
}

static void on_recorded(void);

bool d3d8_swap_replay_enable(const d3d8_swap_replay_config *config, char *error, size_t error_size)
{
    d3d8_swap_replay_disable();
    if (error != NULL && error_size != 0u) {
        error[0] = '\0';
    }
    if (config == NULL || config->spv_directory == NULL || config->spv_directory[0] == '\0') {
        fail(error, error_size, "%s", "spv_directory is required");
        return false;
    }
    if ((config->width == 0u) != (config->height == 0u)) {
        fail(error, error_size, "%s", "width and height must both be set or both be 0");
        return false;
    }
    if (!(config->line_width == 0.0f || config->line_width == 1.0f)) {
        fail(error, error_size, "%s", "line_width must be 0 (unstated) or 1.0: the device has no wideLines");
        return false;
    }
    if ((config->output_groups & ~GPU_PGRAPH_OUTPUT_ALL_MEASURED) != 0u) {
        fail(error, error_size, "%s", "output_groups names a state group that is not measured");
        return false;
    }
    if (config->standin_texture && !config->combiner) {
        fail(error, error_size, "%s", "the stand-in texture only feeds the register combiner: it needs the combiner option");
        return false;
    }
    if (config->standin_texture &&
        (config->standin_width < 1u || config->standin_width > D3D8_SWAP_REPLAY_STANDIN_MAX_EDGE ||
         config->standin_height < 1u || config->standin_height > D3D8_SWAP_REPLAY_STANDIN_MAX_EDGE)) {
        fail(error, error_size, "%s", "the stand-in texture's width and height must each be in 1..4096");
        return false;
    }
    if (config->standin_texel_units && !config->standin_texture) {
        fail(error, error_size, "%s", "the stand-in texel units mode needs the stand-in texture option");
        return false;
    }
    if (config->standin_unit_rule_count != 0u &&
        (!config->standin_texture || config->standin_texel_units || config->standin_unit_rule_count > GPU_STANDIN_UNIT_RULES)) {
        fail(error, error_size, "%s", "the per-draw stand-in units need the stand-in texture, at most 16 rules, and exclude the per-boot texel units");
        return false;
    }
    if (config->standin_texture && config->standin_pattern != GPU_STANDIN_SOLID &&
        config->standin_pattern != GPU_STANDIN_CHECKER) {
        fail(error, error_size, "%s", "unknown stand-in texture pattern");
        return false;
    }
    if (config->render_target_texture) {
        const char *reason = NULL;
        if (!config->combiner) {
            reason = "the render target texture only feeds the register combiner: it needs the combiner option";
        } else if (config->standin_texture) {
            reason = "the render target texture and the stand-in texture are exclusive: a stand-in would stand in for a refused binding";
        } else if (config->flip_y) {
            reason = "the render target texture cannot be combined with flip_y: the replayed image's orientation is undecided (T100f)";
        } else if ((config->output_groups & GPU_PGRAPH_OUTPUT_TEXTURE) == 0u) {
            reason = "the render target texture needs the TEXTURE output group: the address and filter words are what it checks";
        } else if ((config->allowed_inferences & D3D8_SWAP_REPLAY_INFER_RT_TEXTURE) != D3D8_SWAP_REPLAY_INFER_RT_TEXTURE) {
            reason = "the render target texture needs the announced inference D3D8_SWAP_REPLAY_INFER_RT_TEXTURE";
        }
        if (reason != NULL) {
            fail(error, error_size, "%s", reason);
            return false;
        }
    }
    if (config->surface_source) {
        const char *reason = NULL;
        if (!config->render_target_texture) {
            reason = "the surface source feeds the render target texture bridge and the blits over it: it needs the render target texture option";
        } else if ((config->output_groups & GPU_PGRAPH_OUTPUT_BLIT) == 0u) {
            reason = "the surface source needs the BLIT output group: a blit over a surface the replay holds no image of is what it models";
        } else if ((config->allowed_inferences & D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE) != D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE) {
            reason = "the surface source needs the announced inference D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE";
        }
        if (reason) {
            fail(error, error_size, "%s", reason);
            return false;
        }
    }
    if (config->target_persist) {
        const char *reason = NULL;
        if (config->flip_y) {
            reason = "target persistence cannot be combined with flip_y: a kept image would be mirrored a second time (T100f)";
        } else if ((config->allowed_inferences & D3D8_SWAP_REPLAY_INFER_TARGET_PERSIST) != D3D8_SWAP_REPLAY_INFER_TARGET_PERSIST) {
            reason = "target persistence needs the announced inference D3D8_SWAP_REPLAY_INFER_TARGET_PERSIST";
        }
        if (reason) {
            fail(error, error_size, "%s", reason);
            return false;
        }
    }
    if (config->render_target_texture_census && !config->render_target_texture) {
        fail(error, error_size, "%s", "the render target texture census needs the render target texture option");
        return false;
    }
    state.config = *config;
    state.spv_directory = copy_string(config->spv_directory);
    state.dump_directory = copy_string(config->dump_directory);
    state.device_selector = copy_string(config->device_selector);
    bool ok = state.spv_directory != NULL && list_modules(state.spv_directory, config->combiner, error, error_size);
    if (ok && state.dump_directory != NULL) {
        struct stat info;
        if (mkdir(state.dump_directory, 0777) != 0 && errno != EEXIST) {
            fail(error, error_size, "cannot create the dump directory %s", state.dump_directory);
            ok = false;
        } else if (stat(state.dump_directory, &info) != 0 || !S_ISDIR(info.st_mode)) {
            fail(error, error_size, "%s is not a directory", state.dump_directory);
            ok = false;
        }
    }
    if (ok && config->standin_texture) {
        const size_t texels = (size_t)config->standin_width * config->standin_height;
        state.standin_pixels = malloc(texels * 4u);
        if (state.standin_pixels == NULL) {
            fail(error, error_size, "%s", "out of memory for the stand-in texture");
            ok = false;
        } else {
            ok = gpu_standin_pattern_fill(state.standin_pixels, texels * 4u, config->standin_width,
                                           config->standin_height, config->standin_pattern,
                                           config->standin_rgba);
        }
    }
    state.pgraph = ok ? gpu_pgraph_create() : NULL;
    if (ok && state.pgraph == NULL) {
        fail(error, error_size, "%s", "out of memory creating the command model");
        ok = false;
    }
    if (!ok) {
        d3d8_swap_replay_disable();
        return false;
    }
    configure_model();
    state.gpu_resets_seen = d3d8_gpu_reset_count();
    state.active = true;
    d3d8_gpu_set_recorded_observer(on_recorded);
    return true;
}

d3d8_swap_replay_config d3d8_swap_replay_get_config(void)
{
    d3d8_swap_replay_config config;
    memset(&config, 0, sizeof config);
    if (state.active) {
        config = state.config;
        config.spv_directory = state.spv_directory;
        config.dump_directory = state.dump_directory;
        config.device_selector = state.device_selector;
    }
    return config;
}

bool d3d8_swap_replay_enabled(void)
{
    return state.active;
}

d3d8_swap_replay_stats d3d8_swap_replay_get_stats(void)
{
    d3d8_swap_replay_stats stats = state.stats;
    if (state.pgraph != NULL) {
        stats.unhandled_methods = gpu_pgraph_unhandled_count(state.pgraph);
        /* what the model snapshotted since the last fold, so a kick shows before the present */
        const gpu_pgraph_stats model = gpu_pgraph_get_stats(state.pgraph);
        stats.vertex_bytes_captured += model.vertex_bytes_captured - state.vertex_bytes_folded;
        stats.draws_snapshotted += model.vertex_draws_captured - state.vertex_draws_folded;
        stats.pairs_ignored += model.pairs_ignored - state.pairs_ignored_folded;
        stats.copies_pending = gpu_pgraph_copy_count(state.pgraph);
        stats.snapshots_peak = stats.snapshots_peak > model.snapshots_peak ? stats.snapshots_peak : model.snapshots_peak;
        stats.draws_peak = stats.draws_peak > model.draws_peak ? stats.draws_peak : model.draws_peak;
        stats.indices_peak = stats.indices_peak > model.indices_peak ? stats.indices_peak : model.indices_peak;
    }
    return stats;
}

size_t d3d8_swap_replay_standin_summary(char *text, size_t size)
{
    if (text == NULL || size == 0u) {
        return 0u;
    }
    text[0] = '\0';
    if (!state.active || !state.config.standin_texture) {
        return 0u;
    }
    const uint8_t *colour = state.config.standin_rgba;
    char pattern[64];
    if (state.config.standin_pattern == GPU_STANDIN_CHECKER) {
        (void)snprintf(pattern, sizeof pattern, "checker red/yellow/blue/cyan");
    } else {
        (void)snprintf(pattern, sizeof pattern, "RGBA %02X%02X%02X%02X", colour[0], colour[1], colour[2], colour[3]);
    }
    const int written = snprintf(
        text, size,
        "STAND-IN TEXTURE (T497): %ux%u %s at combiner texture stage 0, sampled by %llu of %llu "
        "replayed draw(s). It is NOT the title's texture (the title's textures are not decoded, T480): a frame "
        "that sampled it is NOT a rendering of the title. INFERRED: it is sampled nearest, clamped, row 0 on top "
        "(TEXTURE_SAMPLING). The stage program is the stream's own (word 54) and the coordinate is what each vertex "
        "program wrote to oT0%s, a program that writes none is refused",
        (unsigned)state.config.standin_width, (unsigned)state.config.standin_height, pattern, (unsigned long long)state.stats.standin_draws, (unsigned long long)state.stats.draws,
        state.config.standin_texel_units ? " (T713, INFERRED: oT0 in TEXELS divided by the stand-in's size, the T510 unnormalised path)"
        : state.config.standin_unit_rule_count != 0u ? " (T719, INFERRED: the unit, texel or normalised, is chosen per draw by the vertex program digest, an unlisted program is refused)" : "");
    if (written < 0) {
        text[0] = '\0';
        return 0u;
    }
    return strlen(text);
}

const char *d3d8_swap_replay_device_name(void)
{
    return state.device != NULL ? gpu_device_name(state.device) : NULL;
}

const gpu_image *d3d8_swap_replay_last_frame(void)
{
    return state.last.pixels != NULL ? &state.last : NULL;
}

size_t d3d8_swap_replay_offscreen_count(void)
{
    return state.offscreen_count;
}

const gpu_image *d3d8_swap_replay_offscreen_frame(size_t index, uint32_t *target_header)
{
    if (index >= state.offscreen_count) {
        return NULL;
    }
    if (target_header != NULL) {
        *target_header = state.offscreen[index].target;
    }
    return &state.offscreen[index].image;
}

/* The model's snapshot counters are per model: add what it did since the last fold to the totals. */
static void fold_vertex_stats(void)
{
    if (state.pgraph == NULL) {
        return;
    }
    const gpu_pgraph_stats model = gpu_pgraph_get_stats(state.pgraph);
    state.stats.vertex_bytes_captured += model.vertex_bytes_captured - state.vertex_bytes_folded;
    state.stats.draws_snapshotted += model.vertex_draws_captured - state.vertex_draws_folded;
    state.vertex_bytes_folded = model.vertex_bytes_captured;
    state.vertex_draws_folded = model.vertex_draws_captured;
    state.stats.pairs_ignored += model.pairs_ignored - state.pairs_ignored_folded;
    state.pairs_ignored_folded = model.pairs_ignored;
    state.stats.snapshots_peak = state.stats.snapshots_peak > model.snapshots_peak ? state.stats.snapshots_peak : model.snapshots_peak;
    state.stats.draws_peak = state.stats.draws_peak > model.draws_peak ? state.stats.draws_peak : model.draws_peak;
    state.stats.indices_peak = state.stats.indices_peak > model.indices_peak ? state.stats.indices_peak : model.indices_peak;
}

/* --- d3d8_gpu_reset --------------------------------------------------------------------- */

/* The recording was cleared since the model was built (CreateDevice), so the model, the decode
 * position and the pending switches describe a stream that no longer exists: start over, and clear
 * the latch, which was about that model. When no model can be built it latches instead. */
static void follow_gpu_reset(void)
{
    if (d3d8_gpu_reset_count() == state.gpu_resets_seen) {
        return;
    }
    state.gpu_resets_seen = d3d8_gpu_reset_count();
    fold_vertex_stats();
    free_surfaces(); /* CreateDevice made new surfaces: the stored images name old ones */
    gpu_pgraph_destroy(state.pgraph);
    state.pgraph = gpu_pgraph_create();
    state.vertex_bytes_folded = 0u;
    state.vertex_draws_folded = 0u;
    state.pairs_ignored_folded = 0u;
    state.decode_failed = false;
    state.decode_error[0] = '\0';
    state.next = 0u;
    state.unhandled_logged = 0u;
    state.switch_count = 0u;
    state.switch_overflow = false;
    state.texture_event_count = 0u;
    state.texture_overflow = false;
    memset(state.texture_carry_known, 0, sizeof state.texture_carry_known); /* CreateDevice unbinds every stage */
    state.stats.model_resets++;
    state.stats.latched = false;
    state.stats.error[0] = '\0';
    if (state.pgraph == NULL) {
        state.stats.latched = true;
        snprintf(state.stats.error, sizeof state.stats.error,
                 "out of memory rebuilding the command model after d3d8_gpu_reset");
        return;
    }
    configure_model();
    (void)d3d8_hle_log()("d3d8 swap replay: the GPU was reset (CreateDevice): the command model and "
                         "the pending render target switches were rebuilt\n");
}

const gpu_image *d3d8_swap_replay_surface_image(uint32_t data)
{
    for (size_t i = 0u; i < D3D8_SWAP_REPLAY_SURFACES; i++) {
        if (state.surfaces[i].valid && state.surfaces[i].data == (data & 0x0FFFFFFFu)) {
            return &state.surfaces[i].image;
        }
    }
    return NULL;
}

/* --- decoding at the kick (T262) -------------------------------------------------------- */

/* Decode what the recording holds that the model has not seen. WHEN THIS RUNS IS WHEN GUEST MEMORY IS
 * READ for the vertex snapshot: the consumer calls on_recorded at every kick (d3d8_gpu.h), so a draw's
 * vertex bytes are copied when the GPU is handed the commands that end its bracket, which on this
 * instantaneous GPU is when the draw executes. A refusal is kept and refuses the frame at the present,
 * where every other refusal is reported, so the log and the latch are unchanged. */
static void decode_pending(void)
{
    const uint64_t pairs_before = gpu_pgraph_get_stats(state.pgraph).pairs;
    const gpu_pgraph_result decoded = d3d8_gpu_decode_recording(state.pgraph, &state.next);
    state.stats.commands_decoded += gpu_pgraph_get_stats(state.pgraph).pairs - pairs_before;
    if (decoded != GPU_PGRAPH_OK) {
        state.decode_failed = true;
        snprintf(state.decode_error, sizeof state.decode_error, "decoder (%s) at pair %llu: %s",
                 gpu_pgraph_result_string(decoded),
                 (unsigned long long)gpu_pgraph_get_stats(state.pgraph).pairs,
                 gpu_pgraph_error(state.pgraph));
    }
}

static bool can_decode(void)
{
    return state.pgraph != NULL && !state.stats.latched && !state.decode_failed;
}

static void on_recorded(void)
{
    if (!state.active) {
        return;
    }
    follow_gpu_reset();
    if (can_decode()) {
        decode_pending();
    }
}

/* --- render target switches ------------------------------------------------------------- */

/* T578: the Data word of a surface header (the 28 bit physical address a blit's offsets carry), 0 for no header. */
static uint32_t surface_data(uint32_t header)
{
    return header == 0u ? 0u : d3d8_guest_load32(header + D3D8_SURFACE_DATA) & 0x0FFFFFFFu;
}

static target_size measure_target(uint32_t header)
{
    target_size size = {false, 0u, 0u, 0u, 0u};
    size.measured = d3d8_surface_dimensions(header, &size.width, &size.height);
    if (header != 0u) {
        /* the pitch only of a linear header (a Size word): the swizzled path of the helper needs a format table */
        size.pitch = d3d8_guest_load32(header + D3D8_SURFACE_SIZE) != 0u ? d3d8_surface_header_pitch(header) : 0u;
        size.format = d3d8_guest_load32(header + D3D8_SURFACE_FORMAT);
    }
    return size;
}

void d3d8_swap_replay_on_render_target(uint32_t previous, uint32_t target)
{
    if (!state.active) {
        return;
    }
    follow_gpu_reset();
    if (previous == target) {
        return;
    }
    if (live_target_hook != NULL) {
        live_target_hook(target, live_target_hook_context);
        if (previous != 0u) {
            live_target_hook(previous, live_target_hook_context);
        }
    }
    /* The consumer records at a kick, so hand it what the ring holds now: the recording up to here
     * is exactly what ran against the previous target. A drain, not a kick: no fence completes. */
    d3d8_pushbuffer_drain();
    /* The drain went through the consumer, so on_recorded has decoded everything up to here. */
    if (state.switch_count == MAX_SWITCHES) {
        state.switch_overflow = true;
        return;
    }
    target_switch *entry = &state.switches[state.switch_count++];
    entry->stream_index = d3d8_gpu_stream_count();
    entry->draws = gpu_pgraph_draw_count(state.pgraph);
    entry->clears = gpu_pgraph_clear_count(state.pgraph);
    entry->bracket_open = gpu_pgraph_in_bracket(state.pgraph);
    entry->from = previous;
    entry->to = target;
    entry->from_data = surface_data(previous);
    entry->to_data = surface_data(target);
    entry->from_size = measure_target(previous);
    entry->to_size = measure_target(target);
}

/* --- render target textures (T510) ----------------------------------------------------- */

#define TEXTURE_STAGES 4u
#define RT_ADDRESS_CLAMP 0x00000303u /* U and V clamp to edge (nxdk names, INFERRED) */
#define RT_ADDRESS_WRAP 0x00000101u  /* U and V wrap, i.e. repeat (nxdk names, INFERRED), taken only for the DXT1 texture (T735) */
#define RT_FILTER_LINEAR 0x02062000u /* min 6 (tent, tent), mag 2 (tent), by the nxdk names: bilinear (INFERRED names) */
#define RT_REFUSAL_BYTES 440u
#define RT_SOURCE_BYTES 260u

static texture_binding read_texture_binding(uint32_t header)
{
    texture_binding binding;
    memset(&binding, 0, sizeof binding);
    if (header == 0u) {
        return binding;
    }
    binding.bound = true;
    binding.header = header;
    binding.data = d3d8_guest_load32(header + D3D8_SURFACE_DATA) & 0x0FFFFFFFu;
    binding.format = d3d8_guest_load32(header + D3D8_SURFACE_FORMAT);
    binding.size_word = d3d8_guest_load32(header + D3D8_SURFACE_SIZE);
    if (binding.size_word != 0u) {
        binding.measured = true;
        binding.width = (binding.size_word & 0xFFFu) + 1u;
        binding.height = ((binding.size_word >> 12) & 0xFFFu) + 1u;
        binding.pitch = (((binding.size_word >> 24) & 0xFFu) + 1u) << 6;
    }
    return binding;
}

static bool same_texture_binding(const texture_binding *left, const texture_binding *right)
{
    return left->bound == right->bound && left->header == right->header && left->data == right->data &&
           left->format == right->format && left->size_word == right->size_word;
}

/* The binding stage `stage` had at draw `draw` of the frame: the last note at or before it, else what the last frame left. */
static const texture_binding *binding_at(uint32_t stage, size_t draw)
{
    const texture_binding *found = state.texture_carry_known[stage] ? &state.texture_carry[stage] : NULL;
    for (size_t i = 0u; i < state.texture_event_count; i++) {
        if (state.texture_events[i].stage == stage && state.texture_events[i].draw <= draw) {
            found = &state.texture_events[i].binding;
        }
    }
    return found;
}

void d3d8_swap_replay_on_texture(uint32_t stage, uint32_t texture)
{
    if (!state.active || !(state.config.render_target_texture || live_model_hook != NULL) || stage >= TEXTURE_STAGES) {
        return;
    }
    follow_gpu_reset();
    if (state.pgraph == NULL || state.stats.latched) {
        return;
    }
    const texture_binding binding = read_texture_binding(texture);
    const texture_binding *current = binding_at(stage, SIZE_MAX);
    if (current != NULL && same_texture_binding(current, &binding)) {
        return;
    }
    /* a drain, not a kick: the recording up to here is what ran against the previous binding */
    d3d8_pushbuffer_drain();
    if (state.texture_event_count == D3D8_SWAP_REPLAY_TEXTURE_EVENTS) {
        state.texture_overflow = true;
        return;
    }
    texture_event *entry = &state.texture_events[state.texture_event_count++];
    entry->draw = gpu_pgraph_draw_count(state.pgraph);
    entry->stage = stage;
    entry->binding = binding;
    state.stats.texture_bindings++;
}

/* What the stages of the draw being replayed resolved to. File static: gpu_pgraph_backend carries only a callback and a context. */
typedef struct {
    const pass_result *passes;     /* the passes replayed so far in this frame, in plan order */
    const segment_plan *plan;      /* every pass the frame will replay */
    size_t plan_count;
    size_t current;                /* the plan index of the pass being replayed (== the number of finished passes) */
    char refusal[TEXTURE_STAGES][RT_REFUSAL_BYTES];
    char source[TEXTURE_STAGES][RT_SOURCE_BYTES];
    const char *category[TEXTURE_STAGES]; /* the short name of the refusal in `refusal`, NULL when the stage resolved */
    char detail[TEXTURE_STAGES][64];      /* a census detail of the refusal (the header's Format and Size words), "" when none */
    uint8_t kind[TEXTURE_STAGES];  /* T633: 0 a pass image of the frame, 1 a kept image of an earlier frame, 2 guest memory all zero, 3 guest memory */
    bool dry;                      /* census: no image is read, a resolved stage gets a one texel placeholder */
    bool sampled;                  /* some draw of the pass sampled a bridged image */
    bool surface_sampled;          /* T633: some draw of the pass sampled a kept or guest memory surface (the SURFACE_SOURCE inference) */
} texture_provider;

#define SURFACE_KIND_PASS 0u
#define SURFACE_KIND_KEPT 1u
#define SURFACE_KIND_GUEST_ZERO 2u
#define SURFACE_KIND_GUEST 3u

static texture_provider provider;

static void stage_refusal(uint32_t stage, gpu_combiner_texture *out, const char *category, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(provider.refusal[stage], RT_REFUSAL_BYTES, format, arguments);
    va_end(arguments);
    provider.category[stage] = category;
    out->rgba = NULL;
    out->refusal = provider.refusal[stage];
}

/* Do the byte ranges [a, a + a_bytes) and [b, b + b_bytes) share a byte? */
static bool ranges_overlap(uint64_t a, uint64_t a_bytes, uint64_t b, uint64_t b_bytes)
{
    return a < b + b_bytes && b < a + a_bytes;
}

/* T632: the title's own swizzled DXT1 texture, Format 0x09910C29 with no Size word (colour byte 0x0C, one level, width and
 * height exponents 9 so 512x512). Other formats and levels are refused by name. */
#define DXT1_COLOR_BYTE 0x0Cu
#define DXT1_MAX_EXPONENT 9u
#define DXT1_MAX_TEXELS (1u << DXT1_MAX_EXPONENT)
#define DXT1_MAX_BLOCKS (DXT1_MAX_TEXELS / 4u)
#define DXT1_BLOCK_BYTES 8u

static uint8_t dxt1_blocks[DXT1_MAX_BLOCKS * DXT1_MAX_BLOCKS * DXT1_BLOCK_BYTES];
static uint8_t dxt1_pixels[TEXTURE_STAGES][DXT1_MAX_TEXELS * DXT1_MAX_TEXELS * 4u];

static bool format_is_dxt1(uint32_t format)
{
    return ((format >> 8) & 0xFFu) == DXT1_COLOR_BYTE;
}

/* T792: the swizzled DXT1 decode (Morton block order, 565 palette, 3 colour transparent mode) lives in live_texture.c, shared with the live path. */
static void decode_dxt1(uint32_t size, uint8_t *rgba)
{
    live_texture_decode_dxt1_square(dxt1_blocks, size, rgba);
}

/* --- T633 (1): a surface no pass of the frame drew ---------------------------------------------------------------------- */

static stored_surface *find_stored(uint32_t data);
static bool persist_initial(uint32_t data, uint32_t width, uint32_t height, target_size size, const uint8_t **initial, char *what, size_t what_size);
static void note_overlay(uint64_t frame);

/* The newest blit of this frame that sits before draw `draw` and writes a byte of [data, data + bytes), or NULL. A blit's result is applied to the
 * images after the frame's passes ran, so a draw that sampled the surface would have read it without the blit: refused rather than sampled stale. */
static const gpu_pgraph_copy *blit_written_before(uint32_t data, uint64_t bytes, size_t draw)
{
    const gpu_pgraph_copy *found = NULL;
    for (size_t i = 0u; state.pgraph != NULL && i < gpu_pgraph_copy_count(state.pgraph); i++) {
        const gpu_pgraph_copy *copy = gpu_pgraph_copy_at(state.pgraph, i);
        const size_t extent = gpu_pgraph_blit_extent_bytes(copy->color_format, copy->destination_pitch, copy->out_x, copy->out_y,
                                                           gpu_pgraph_blit_row_pixels(copy), copy->height);
        if (extent != 0u && copy->before_draw <= draw && ranges_overlap(data, bytes, copy->destination_offset, extent)) {
            found = copy;
        }
    }
    return found;
}

static void provide_surface(uint32_t stage, const texture_binding *binding, size_t draw, gpu_combiner_texture *out)
{
    static const uint8_t placeholder[4] = {0u, 0u, 0u, 0u};
    const uint64_t bytes = (uint64_t)binding->pitch * binding->height;
    if ((state.config.allowed_inferences & D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE) == 0u) {
        stage_refusal(stage, out, "surface source not allowed",
                      "render target texture: the surface source of Data 0x%07X is INFERRED and its inference is not allowed: surface source not allowed",
                      (unsigned)binding->data);
        return;
    }
    const gpu_pgraph_copy *blit = blit_written_before(binding->data, bytes, draw);
    if (blit != NULL) {
        stage_refusal(stage, out, "blit before the draw",
                      "render target texture: a 2D engine blit of this frame writes Data 0x%07X (to 0x%07X) before draw %zu samples it, and a blit's result is not fed "
                      "to the draws of its own frame: blit before the draw", (unsigned)binding->data, (unsigned)blit->destination_offset, draw);
        return;
    }
    for (size_t i = 0u; i < D3D8_SWAP_REPLAY_SURFACES; i++) {
        const stored_surface *other = &state.surfaces[i];
        if (other->valid && other->data != binding->data &&
            ranges_overlap(binding->data, bytes, other->data, (uint64_t)other->image.width * 4u * other->image.height)) {
            stage_refusal(stage, out,
                          "alias",
                          "render target texture: header 0x%08X (Data 0x%07X, %u bytes) aliases the kept image of Data 0x%07X at a different offset: alias",
                          (unsigned)binding->header, (unsigned)binding->data, (unsigned)bytes, (unsigned)other->data);
            return;
        }
    }
    if (d3d8_surface_model_overlaps_bytes(binding->data, bytes)) {
        stage_refusal(stage, out, "byte surface",
                      "render target texture: Data 0x%07X overlaps a guest byte surface a CopyRects byte blit wrote, which holds bytes and no image: byte surface",
                      (unsigned)binding->data);
        return;
    }
    const d3d8_surface_probe probe = d3d8_surface_model_probe(binding->data, bytes);
    if (probe == D3D8_SURFACE_UNREADABLE) {
        stage_refusal(stage, out, "unreadable bytes",
                      "render target texture: the %llu bytes of header 0x%08X (Data 0x%07X) cannot be read from guest memory: unreadable bytes",
                      (unsigned long long)bytes, (unsigned)binding->header, (unsigned)binding->data);
        return;
    }
    stored_surface *kept = find_stored(binding->data);
    if (kept != NULL) {
        if (kept->image.width != binding->width || kept->image.height != binding->height || (kept->pitch != 0u && kept->pitch != binding->pitch) ||
            (kept->format != 0u && kept->format != binding->format)) {
            stage_refusal(stage, out,
                          "inexact relationship",
                          "render target texture: header 0x%08X (%ux%u, pitch %u, Format 0x%08X) is not exactly the kept image of Data 0x%07X "
                          "(%ux%u, pitch %u, Format 0x%08X): the header, data and format relationship is not exact",
                          (unsigned)binding->header, (unsigned)binding->width, (unsigned)binding->height, (unsigned)binding->pitch,
                          (unsigned)binding->format, (unsigned)kept->data, (unsigned)kept->image.width, (unsigned)kept->image.height,
                          (unsigned)kept->pitch, (unsigned)kept->format);
            return;
        }
        if (probe != D3D8_SURFACE_ZERO) {
            stage_refusal(stage, out, "cpu written surface",
                          "render target texture: Data 0x%07X holds a kept image of an earlier frame but its guest memory is not zero: the CPU wrote it "
                          "(xemu T736: a later CPU write wins byte by byte) and its order against the pass is not observed: cpu written surface", (unsigned)binding->data);
            return;
        }
        if (!provider.dry && (kept->image.pixels == NULL || kept->image.stride_bytes != binding->width * 4u)) {
            stage_refusal(stage, out, "image not packed", "render target texture: the kept image of Data 0x%07X is not a tightly packed %ux%u RGBA image",
                          (unsigned)binding->data, (unsigned)binding->width, (unsigned)binding->height);
            return;
        }
        out->rgba = provider.dry ? placeholder : kept->image.pixels;
        out->width = provider.dry ? 1u : kept->image.width;
        out->height = provider.dry ? 1u : kept->image.height;
        provider.kind[stage] = SURFACE_KIND_KEPT;
        (void)snprintf(provider.source[stage], RT_SOURCE_BYTES,
                       "kept render target image (Data 0x%07X, %ux%u, Format 0x%08X) of an earlier frame, MEASURED in xemu (T736) the surface still holds it",
                       (unsigned)binding->data, (unsigned)binding->width, (unsigned)binding->height, (unsigned)binding->format);
    } else {
        const size_t image_bytes = (size_t)binding->width * binding->height * 4u;
        if (!provider.dry) {
            if (state.surface_buffer_bytes[stage] < image_bytes) {
                uint8_t *grown = realloc(state.surface_buffer[stage], image_bytes);
                if (grown == NULL) {
                    stage_refusal(stage, out, "out of memory", "render target texture: out of memory for the %zu byte guest surface image: out of memory",
                                  image_bytes);
                    return;
                }
                state.surface_buffer[stage] = grown;
                state.surface_buffer_bytes[stage] = image_bytes;
            }
            if (d3d8_surface_model_read_argb(binding->data, binding->width, binding->height, binding->pitch, state.surface_buffer[stage]) !=
                D3D8_SURFACE_MODEL_OK) {
                stage_refusal(stage, out, "unreadable bytes",
                              "render target texture: the %llu bytes of header 0x%08X (Data 0x%07X) cannot be read from guest memory: unreadable bytes",
                              (unsigned long long)bytes, (unsigned)binding->header, (unsigned)binding->data);
                return;
            }
        }
        out->rgba = provider.dry ? placeholder : state.surface_buffer[stage];
        out->width = provider.dry ? 1u : binding->width;
        out->height = provider.dry ? 1u : binding->height;
        provider.kind[stage] = probe == D3D8_SURFACE_ZERO ? SURFACE_KIND_GUEST_ZERO : SURFACE_KIND_GUEST;
        (void)snprintf(provider.source[stage], RT_SOURCE_BYTES,
                       probe == D3D8_SURFACE_ZERO
                           ? "guest memory surface (Data 0x%07X, %ux%u, Format 0x%08X) no replayed pass drew: all zero, transparent black (MEASURED bytes, INFERRED unchanged at the draw)"
                           : "guest memory surface (Data 0x%07X, %ux%u, Format 0x%08X) no replayed pass drew: read at the present (INFERRED the bytes at the draw)",
                       (unsigned)binding->data, (unsigned)binding->width, (unsigned)binding->height, (unsigned)binding->format);
    }
    out->linear = true;
    out->unnormalised = true;
}

/* The refusal list of docs/d3d8-copy-composition.md "T510": each is a distinct, named cause. */
static void provide_texture(void *context, size_t draw, const gpu_pgraph_state *snapshot, uint32_t stage,
                            gpu_combiner_texture *out)
{
    (void)context;
    if (stage >= TEXTURE_STAGES) {
        return;
    }
    provider.kind[stage] = SURFACE_KIND_PASS;
    provider.source[stage][0] = '\0';
    provider.category[stage] = NULL;
    provider.detail[stage][0] = '\0';
    const texture_binding *binding = binding_at(stage, draw);
    if (binding == NULL) {
        stage_refusal(stage, out, "no binding observed", "render target texture: no SetTexture of stage %u was observed since the replay was enabled or the "
                      "device was created, so its binding at draw %zu is unknown", (unsigned)stage, draw);
        return;
    }
    if (!binding->bound) {
        stage_refusal(stage, out, "unbound", "render target texture: stage %u is unbound (SetTexture 0) at draw %zu", (unsigned)stage, draw);
        return;
    }
    (void)snprintf(provider.detail[stage], sizeof provider.detail[stage], " Format 0x%08X Size 0x%08X", (unsigned)binding->format,
                   (unsigned)binding->size_word);
    const bool dxt1 = !binding->measured && format_is_dxt1(binding->format);
    const uint32_t dxt1_exponent_u = (binding->format >> 20) & 0xFu;
    const uint32_t dxt1_exponent_v = (binding->format >> 24) & 0xFu;
    if (dxt1) {
        const uint32_t levels = (binding->format >> 16) & 0xFu;
        const uint32_t dimensions = (binding->format >> 4) & 0xFu;
        if (levels != 1u) {
            stage_refusal(stage, out, "second level",
                          "render target texture: header 0x%08X Format 0x%08X has %u mipmap levels, the DXT1 path samples exactly one: second level",
                          (unsigned)binding->header, (unsigned)binding->format, (unsigned)levels);
            return;
        }
        if (dimensions != 2u || dxt1_exponent_u != dxt1_exponent_v || dxt1_exponent_u < 2u || dxt1_exponent_u > DXT1_MAX_EXPONENT ||
            ((binding->format >> 28) & 0xFu) != 0u || (binding->format & 0xFu) != 9u || ((binding->format >> 12) & 0xFu) != 0u) {
            stage_refusal(stage, out,
                          "unsupported format",
                          "render target texture: header 0x%08X DXT1 Format 0x%08X is not a square 2D swizzled texture of the measured "
                          "kind (0x09910C29 is 512x512): unsupported format", (unsigned)binding->header, (unsigned)binding->format);
            return;
        }
    }
    if (!dxt1 && (!binding->measured || binding->format != D3D8_SWAP_REPLAY_RT_TEXTURE_FORMAT)) {
        stage_refusal(stage, out, "unsupported format",
                      "render target texture: header 0x%08X has Format 0x%08X and Size 0x%08X, the bridge takes only a linear "
                      "A8R8G8B8 2D texture of one level (Format 0x%08X with a Size word): unsupported format",
                      (unsigned)binding->header, (unsigned)binding->format, (unsigned)binding->size_word,
                      (unsigned)D3D8_SWAP_REPLAY_RT_TEXTURE_FORMAT);
        return;
    }
    if (!dxt1 && binding->pitch != binding->width * 4u) {
        stage_refusal(stage, out, "unsupported layout",
                      "render target texture: header 0x%08X pitch %u is not the tight %u bytes of its %u texels: unsupported layout",
                      (unsigned)binding->header, (unsigned)binding->pitch, (unsigned)(binding->width * 4u), (unsigned)binding->width);
        return;
    }
    const uint32_t address_index = GPU_PGRAPH_OUT_TEXTURE_ADDRESS + stage;
    const uint32_t filter_index = GPU_PGRAPH_OUT_TEXTURE_FILTER + stage;
    const bool wrap = snapshot->output_written[address_index] && snapshot->output[address_index] == RT_ADDRESS_WRAP;
    if (!snapshot->output_written[address_index] ||
        (snapshot->output[address_index] != RT_ADDRESS_CLAMP && !(dxt1 && wrap))) {
        stage_refusal(stage, out, "address mode",
                      "render target texture: stage %u address mode %s, the replay's sampler implements clamp 0x%08X (and wrap 0x%08X for the DXT1 texture) (0x%04X)",
                      (unsigned)stage,
                      snapshot->output_written[address_index] ? "is not clamp" : "was never written by the stream",
                      (unsigned)RT_ADDRESS_CLAMP, (unsigned)RT_ADDRESS_WRAP, (unsigned)(0x1B08u + 0x40u * stage));
        return;
    }
    if (!snapshot->output_written[filter_index] || snapshot->output[filter_index] != RT_FILTER_LINEAR) {
        stage_refusal(stage, out, "filter",
                      "render target texture: stage %u filter %s, only the measured 0x%08X (bilinear) is sampled (0x%04X)",
                      (unsigned)stage, snapshot->output_written[filter_index] ? "is not the measured word" : "was never written by the stream",
                      (unsigned)RT_FILTER_LINEAR, (unsigned)(0x1B14u + 0x40u * stage));
        return;
    }
    if (dxt1) {
        const uint32_t size = 1u << dxt1_exponent_u;
        const uint32_t blocks = size / 4u;
        const size_t bytes = (size_t)blocks * blocks * DXT1_BLOCK_BYTES;
        if (!d3d8_gpu_read_guest(NULL, binding->data, dxt1_blocks, bytes)) {
            stage_refusal(stage, out, "unreadable bytes",
                          "render target texture: the %zu bytes of the DXT1 texture of header 0x%08X (Data 0x%07X) cannot be read from "
                          "guest memory: unreadable bytes", bytes, (unsigned)binding->header, (unsigned)binding->data);
            return;
        }
        static const uint8_t placeholder_texel[4] = {0u, 0u, 0u, 0u};
        if (provider.dry) {
            out->rgba = placeholder_texel;
            out->width = 1u;
            out->height = 1u;
        } else {
            decode_dxt1(size, dxt1_pixels[stage]);
            out->rgba = dxt1_pixels[stage];
            out->width = size;
            out->height = size;
        }
        out->linear = true;
        out->unnormalised = false;
        out->repeat = wrap;
        (void)snprintf(provider.source[stage], RT_SOURCE_BYTES,
                       "guest memory DXT1 texture (header 0x%08X, Data 0x%07X, %ux%u, Format 0x%08X) swizzled, decoded at the draw",
                       (unsigned)binding->header, (unsigned)binding->data, (unsigned)size, (unsigned)size, (unsigned)binding->format);
        return;
    }
    const uint64_t texture_bytes = (uint64_t)binding->pitch * binding->height;
    size_t match = SIZE_MAX;
    size_t matches = 0u;
    size_t alias = SIZE_MAX;
    for (size_t index = 0u; index < provider.plan_count; index++) {
        const segment_plan *segment = &provider.plan[index];
        if (segment->data == binding->data) {
            matches++; /* a second pass or a second header on the same bytes makes it ambiguous */
            if (match == SIZE_MAX) {
                match = index;
            }
        } else if (ranges_overlap(binding->data, texture_bytes, segment->data, (uint64_t)segment->size.pitch * segment->size.height)) {
            alias = index;
        }
    }
    if (matches > 1u) {
        stage_refusal(stage, out, "ambiguous mapping",
                      "render target texture: ambiguous mapping, Data 0x%07X is drawn into by more than one pass or header of this frame",
                      (unsigned)binding->data);
        return;
    }
    if (matches == 0u) {
        if (alias != SIZE_MAX) {
            stage_refusal(stage, out, "alias",
                          "render target texture: header 0x%08X (Data 0x%07X, %u bytes) aliases render target 0x%08X (Data 0x%07X) at a "
                          "different offset: only an exact Data match is taken",
                          (unsigned)binding->header, (unsigned)binding->data, (unsigned)texture_bytes,
                          (unsigned)provider.plan[alias].target, (unsigned)provider.plan[alias].data);
        } else if (state.config.surface_source) {
            provide_surface(stage, binding, draw, out); /* T633 (1): the kept image of an earlier frame, else the guest memory under it */
        } else {
            stage_refusal(stage, out, "no earlier image",
                          "render target texture: no earlier image, no pass of this frame draws into Data 0x%07X (header 0x%08X) "
                          "so its pixels are unknown (a surface written by the CPU or an earlier frame is not modelled)",
                          (unsigned)binding->data, (unsigned)binding->header);
        }
        return;
    }
    if (alias != SIZE_MAX) {
        stage_refusal(stage, out, "ambiguous mapping",
                      "render target texture: ambiguous mapping, header 0x%08X (Data 0x%07X) also overlaps render target 0x%08X (Data 0x%07X) "
                      "at a different offset", (unsigned)binding->header, (unsigned)binding->data,
                      (unsigned)provider.plan[alias].target, (unsigned)provider.plan[alias].data);
        return;
    }
    const segment_plan *producer = &provider.plan[match];
    if (match == provider.current) {
        stage_refusal(stage, out, "same-target feedback",
                      "render target texture: same-target feedback, stage %u samples render target 0x%08X (Data 0x%07X) that draw %zu "
                      "draws into", (unsigned)stage, (unsigned)producer->target, (unsigned)producer->data, draw);
        return;
    }
    if (match > provider.current && state.config.surface_source) {
        provide_surface(stage, binding, draw, out); /* T633 (1): when this draw ran the surface still held what an earlier frame left */
        return;
    }
    if (match > provider.current) {
        stage_refusal(stage, out, "out of order",
                      "render target texture: out of order, the only pass that draws into Data 0x%07X (render target 0x%08X) starts at "
                      "draw %zu, after draw %zu that samples it", (unsigned)producer->data, (unsigned)producer->target, producer->first,
                      draw);
        return;
    }
    if (!producer->size.measured || producer->size.width != binding->width || producer->size.height != binding->height ||
        producer->size.pitch != binding->pitch || producer->size.format != binding->format) {
        stage_refusal(stage, out, "inexact relationship",
                      "render target texture: header 0x%08X (%ux%u, pitch %u, Format 0x%08X) is not exactly render target 0x%08X "
                      "(%ux%u, pitch %u, Format 0x%08X): the header, data and format relationship is not exact",
                      (unsigned)binding->header, (unsigned)binding->width, (unsigned)binding->height, (unsigned)binding->pitch,
                      (unsigned)binding->format, (unsigned)producer->target, (unsigned)producer->size.width,
                      (unsigned)producer->size.height, (unsigned)producer->size.pitch, (unsigned)producer->size.format);
        return;
    }
    if (state.config.surface_source && blit_written_before(binding->data, texture_bytes, draw) != NULL) {
        stage_refusal(stage, out, "blit before the draw",
                      "render target texture: a 2D engine blit of this frame writes Data 0x%07X before draw %zu samples the pass image of render target 0x%08X, "
                      "and a blit's result is not fed to the draws of its own frame: blit before the draw", (unsigned)binding->data, draw,
                      (unsigned)producer->target);
        return;
    }
    static const uint8_t placeholder[4] = {0u, 0u, 0u, 0u};
    const gpu_image *image = provider.dry ? NULL : &provider.passes[match].image;
    if (!provider.dry && (image->pixels == NULL || image->width != binding->width || image->height != binding->height ||
        image->stride_bytes != binding->width * 4u)) {
        stage_refusal(stage, out, "image not packed", "render target texture: the replayed image of render target 0x%08X is not a tightly packed %ux%u RGBA image",
                      (unsigned)producer->target, (unsigned)binding->width, (unsigned)binding->height);
        return;
    }
    out->rgba = provider.dry ? placeholder : image->pixels;
    out->width = provider.dry ? 1u : image->width;
    out->height = provider.dry ? 1u : image->height;
    out->linear = true;
    out->unnormalised = true;
    out->repeat = false;
    (void)snprintf(provider.source[stage], RT_SOURCE_BYTES,
                   "render target 0x%08X (Data 0x%07X, %ux%u, Format 0x%08X) drawn by an earlier pass of the same frame",
                   (unsigned)producer->target, (unsigned)producer->data, (unsigned)binding->width, (unsigned)binding->height,
                   (unsigned)binding->format);
}

static void tally_texture_source(uint32_t stage, const char *source)
{
    char line[RT_SOURCE_BYTES + 24];
    (void)snprintf(line, sizeof line, "stage %u from %s", (unsigned)stage, source);
    for (size_t i = 0u; i < state.texture_census_count; i++) {
        if (strcmp(state.texture_census[i].text, line) == 0) {
            state.texture_census[i].draws++;
            return;
        }
    }
    if (state.texture_census_count == D3D8_SWAP_REPLAY_TEXTURE_CENSUS) {
        state.texture_census_overflow++;
        return;
    }
    texture_census_line *entry = &state.texture_census[state.texture_census_count++];
    (void)snprintf(entry->text, sizeof entry->text, "%s", line);
    entry->draws = 1u;
}

static void texture_sampled(void *context, size_t draw, uint32_t stage)
{
    (void)context;
    (void)draw;
    if (stage < TEXTURE_STAGES && provider.source[stage][0] != '\0') {
        provider.sampled = true;
        state.stats.rt_texture_draws++;
        tally_texture_source(stage, provider.source[stage]);
        if (provider.kind[stage] != SURFACE_KIND_PASS) {
            provider.surface_sampled = true;
        }
        if (provider.kind[stage] == SURFACE_KIND_KEPT) {
            state.stats.surface_kept_samples++;
        } else if (provider.kind[stage] != SURFACE_KIND_PASS) {
            state.stats.surface_guest_samples++;
            state.stats.surface_guest_zero_samples += provider.kind[stage] == SURFACE_KIND_GUEST_ZERO ? 1u : 0u;
        }
    }
}

size_t d3d8_swap_replay_texture_census_count(void)
{
    return state.texture_census_count;
}

bool d3d8_swap_replay_texture_census_at(size_t index, char *text, size_t size, uint64_t *draws)
{
    if (index >= state.texture_census_count) {
        return false;
    }
    if (text != NULL && size != 0u) {
        (void)snprintf(text, size, "%s", state.texture_census[index].text);
    }
    if (draws != NULL) {
        *draws = state.texture_census[index].draws;
    }
    return true;
}

size_t d3d8_swap_replay_rt_texture_summary(char *text, size_t size)
{
    if (text == NULL || size == 0u) {
        return 0u;
    }
    text[0] = '\0';
    if (!state.active || !state.config.render_target_texture) {
        return 0u;
    }
    if (state.config.render_target_texture_census) {
        const int census = snprintf(
            text, size,
            "RENDER TARGET TEXTURE CENSUS (T510): CENSUS ONLY, no pixel was replayed. %llu frame(s), %llu draw(s) classified by what their "
            "texture stage would resolve to or why it is refused, %llu SetTexture binding(s) noted. Nothing is latched: every draw is "
            "counted past the first refusal",
            (unsigned long long)state.stats.census_frames, (unsigned long long)state.stats.census_draws,
            (unsigned long long)state.stats.texture_bindings);
        if (census < 0) {
            text[0] = '\0';
            return 0u;
        }
        return strlen(text);
    }
    const int written = snprintf(
        text, size,
        "RENDER TARGET TEXTURE (T510): %llu SetTexture binding(s) noted, %llu (draw, stage) sample(s) read an image an earlier pass of the same "
        "frame drew, of %llu replayed draw(s). Every other bound texture is REFUSED by name, never a white or default texture. "
        "INFERRED: the replayed image is the surface the hardware samples, a linear A8R8G8B8 texture takes its coordinate in texels "
        "(unnormalised, xemu), bilinear and clamped by the stream's filter and address words (nxdk names), "
        "the texel centre convention is the host's Vulkan one",
        (unsigned long long)state.stats.texture_bindings, (unsigned long long)state.stats.rt_texture_draws,
        (unsigned long long)state.stats.draws);
    if (written < 0) {
        text[0] = '\0';
        return 0u;
    }
    return strlen(text);
}

/* T510 census mode: classify every draw of the frame the way the replay would resolve it, without replaying a pixel, so one run
 * lists the resolved source of each supported draw AND every refusal, past the first one (which stops a replay). Each draw plans its
 * combiner against the provider's per stage answer, exactly as gpu_pgraph_resolve_fragment does, with a one texel placeholder where a
 * stage resolved. A draw is counted once: by the sources of the stages its combiner samples, or by the reason it is refused. */
static void census_line(const char *text)
{
    char line[RT_SOURCE_BYTES + 160];
    (void)snprintf(line, sizeof line, "%s", text);
    for (size_t i = 0u; i < state.texture_census_count; i++) {
        if (strcmp(state.texture_census[i].text, line) == 0) {
            state.texture_census[i].draws++;
            return;
        }
    }
    if (state.texture_census_count == D3D8_SWAP_REPLAY_TEXTURE_CENSUS) {
        state.texture_census_overflow++;
        return;
    }
    texture_census_line *entry = &state.texture_census[state.texture_census_count++];
    (void)snprintf(entry->text, sizeof entry->text, "%s", line);
    entry->draws = 1u;
}

static void census_frame(const segment_plan *plan, size_t plan_count)
{
    provider.dry = true;
    provider.passes = NULL;
    for (size_t segment = 0u; segment < plan_count; segment++) {
        provider.current = segment;
        for (size_t index = plan[segment].first; index < plan[segment].end; index++) {
            const gpu_pgraph_draw *draw = gpu_pgraph_draw_at(state.pgraph, index);
            const gpu_pgraph_state *snapshot = gpu_pgraph_snapshot(state.pgraph, draw->snapshot);
            gpu_combiner_texture textures[TEXTURE_STAGES];
            memset(textures, 0, sizeof textures);
            for (uint32_t stage = 0u; stage < TEXTURE_STAGES; stage++) {
                provide_texture(NULL, index, snapshot, stage, &textures[stage]);
            }
            gpu_combiner_plan combiner;
            char error[320];
            const gpu_pgraph_result planned = gpu_combiner_plan_build(snapshot, state.config.allowed_inferences & GPU_COMBINER_INFER_ALL,
                                                                      textures, &combiner, error, sizeof error);
            state.stats.census_draws++;
            char line[RT_SOURCE_BYTES + 160];
            if (planned != GPU_PGRAPH_OK) {
                const char *category = NULL;
                uint32_t refused_stage = 0u;
                for (uint32_t stage = 0u; stage < TEXTURE_STAGES; stage++) {
                    if (provider.category[stage] != NULL && strstr(error, provider.refusal[stage]) != NULL) {
                        category = provider.category[stage];
                        refused_stage = stage;
                        break;
                    }
                }
                if (category != NULL) {
                    (void)snprintf(line, sizeof line, "REFUSED stage %u texture: %s,%s (drawing into target 0x%08X)", (unsigned)refused_stage,
                                   category, provider.detail[refused_stage], (unsigned)plan[segment].target);
                } else {
                    (void)snprintf(line, sizeof line, "REFUSED by the combiner plan, not a texture: %.100s", error);
                }
                census_line(line);
                continue;
            }
            if (combiner.texture_stages == 0u) {
                census_line("no texture read by the combiner");
                continue;
            }
            for (uint32_t stage = 0u; stage < TEXTURE_STAGES; stage++) {
                if (((combiner.texture_stages >> stage) & 1u) != 0u) {
                    (void)snprintf(line, sizeof line, "stage %u from %s", (unsigned)stage, provider.source[stage]);
                    census_line(line);
                    state.stats.rt_texture_draws++;
                }
            }
        }
    }
    provider.dry = false;
}

/* The frame is over (replayed, empty or refused): what each stage held last is what the next frame starts with. */
static void roll_texture_events(void)
{
    for (size_t i = 0u; i < state.texture_event_count; i++) {
        state.texture_carry[state.texture_events[i].stage] = state.texture_events[i].binding;
        state.texture_carry_known[state.texture_events[i].stage] = true;
    }
    state.texture_event_count = 0u;
    state.texture_overflow = false;
}

/* --- one frame -------------------------------------------------------------------------- */

static void log_new_unhandled(uint64_t frame)
{
    const size_t total = gpu_pgraph_unhandled_count(state.pgraph);
    for (; state.unhandled_logged < total; state.unhandled_logged++) {
        uint32_t method = 0u;
        uint64_t pairs = 0u;
        gpu_pgraph_unhandled_at(state.pgraph, state.unhandled_logged, &method, &pairs);
        (void)d3d8_hle_log()("d3d8 swap replay: frame %llu: UNHANDLED method 0x%04X (counted, "
                             "its state is NOT applied to the replay)\n",
                             (unsigned long long)frame, (unsigned)method);
    }
    if (gpu_pgraph_unhandled_overflow(state.pgraph) != 0u) {
        (void)d3d8_hle_log()("d3d8 swap replay: frame %llu: more distinct unhandled methods than "
                             "the table holds (%llu pairs not listed)\n",
                             (unsigned long long)frame,
                             (unsigned long long)gpu_pgraph_unhandled_overflow(state.pgraph));
    }
}

static void refuse(uint64_t frame, const char *what)
{
    state.stats.frames_refused++;
    state.stats.latched = true;
    snprintf(state.stats.error, sizeof state.stats.error, "frame %llu refused: %s",
             (unsigned long long)frame, what);
    const gpu_pgraph_stats peaks = gpu_pgraph_get_stats(state.pgraph);
    (void)d3d8_hle_log()("d3d8 swap replay: %s. The replay is STOPPED: later frames are skipped, "
                         "not replayed from a broken model. Per-frame peak usage (T1268): snapshots %llu of %u, "
                         "draws %llu of %u, indices %llu of %u\n",
                         state.stats.error, (unsigned long long)peaks.snapshots_peak, GPU_PGRAPH_MAX_SNAPSHOTS,
                         (unsigned long long)peaks.draws_peak, GPU_PGRAPH_MAX_DRAWS,
                         (unsigned long long)peaks.indices_peak, GPU_PGRAPH_MAX_INDICES);
    if (state.config.fatal_on_refusal) {
        d3d8_hle_fatal(SWAP_ADDRESS, "swap replay: %s", state.stats.error);
    }
}

static void dump_image(uint64_t frame, const char *suffix, const gpu_image *image)
{
    if (state.dump_directory == NULL) {
        return;
    }
    const size_t length = strlen(state.dump_directory) + strlen(suffix) + 32u;
    char *path = malloc(length);
    if (path == NULL) {
        state.stats.dump_failures++;
        return;
    }
    snprintf(path, length, "%s/frame_%06llu%s.png", state.dump_directory,
             (unsigned long long)frame, suffix);
    if (gpu_png_write_rgba(path, image->pixels, image->width, image->height, image->stride_bytes)) {
        state.stats.dumps_written++;
    } else {
        state.stats.dump_failures++;
        (void)d3d8_hle_log()("d3d8 swap replay: frame %llu: cannot write %s\n",
                             (unsigned long long)frame, path);
    }
    free(path);
}

/* T484: which replayed frames are written as they are replayed. With neither option, all of them (the
 * behaviour before T484). `dump_every` N writes those whose present number is a multiple of N. `dump_last`
 * alone writes none here (d3d8_swap_replay_dump_last writes the last one at the end). */
static bool dump_due(uint64_t frame)
{
    if (state.dump_directory == NULL) {
        return false;
    }
    if (state.config.dump_every != 0u) {
        return frame % state.config.dump_every == 0u;
    }
    return !state.config.dump_last;
}

static void dump_offscreen(uint64_t frame)
{
    for (size_t i = 0u; i < state.offscreen_count; i++) {
        char suffix[32];
        snprintf(suffix, sizeof suffix, "_target_%08X", (unsigned)state.offscreen[i].target);
        dump_image(frame, suffix, &state.offscreen[i].image);
    }
}

static void free_passes(pass_result *passes, size_t count)
{
    for (size_t i = 0u; i < count; i++) {
        gpu_image_free(&passes[i].image);
    }
}

/* Every refusal leaves nothing behind: the frame's switches are spent and its passes are freed. */
static void refuse_frame(uint64_t frame, const char *what, pass_result *passes, size_t pass_count)
{
    free_passes(passes, pass_count);
    state.switch_count = 0u;
    state.switch_overflow = false;
    refuse(frame, what);
}

static bool open_device(char *what, size_t what_size)
{
    if (state.device != NULL) {
        return true;
    }
    const char *selector = state.device_selector != NULL ? state.device_selector
                                                         : getenv("VKRUN_DEVICE");
    const gpu_result created = gpu_device_create_selected(selector, &state.device);
    if (created != GPU_OK) {
        snprintf(what, what_size, "no Vulkan device (selector \"%s\"): %s",
                 selector != NULL ? selector : "", gpu_result_string(created));
        return false;
    }
    return true;
}

/* The draw list now in the model, replayed into an image of the target's size. False with `what`
 * filled for every refusal: an unmeasured size, a target already drawn this frame, an explicit size
 * asked of more than one target, no device, or the replay's own refusal. */
static bool replay_pass(uint32_t target, uint32_t target_data, target_size size, const pass_result *passes,
                        size_t pass_count, size_t first_draw, size_t draw_count, size_t first_clear,
                        size_t clear_count, pass_result *pass, char *what, size_t what_size)
{
    for (size_t i = 0u; i < pass_count; i++) {
        if (passes[i].target == target) {
            snprintf(what, what_size,
                     "render target 0x%08X is drawn in two separate passes (another target was bound "
                     "in between): drawing into an image that already holds draws is not modelled",
                     (unsigned)target);
            return false;
        }
    }
    uint32_t width = size.width;
    uint32_t height = size.height;
    if (state.config.width != 0u) {
        if (pass_count != 0u) {
            snprintf(what, what_size,
                     "an explicit %ux%u size cannot be applied to a second render target (0x%08X): "
                     "each target's own size is needed",
                     (unsigned)state.config.width, (unsigned)state.config.height, (unsigned)target);
            return false;
        }
        width = state.config.width;
        height = state.config.height;
    } else if (!size.measured) {
        snprintf(what, what_size,
                 "the size of render target 0x%08X cannot be measured (no header bound, or a swizzled "
                 "one with no size exponents): pass an explicit width and height", (unsigned)target);
        return false;
    }
    const uint8_t *initial = NULL;
    if (!persist_initial(target_data, width, height, size, &initial, what, what_size)) {
        return false;
    }
    if (!open_device(what, what_size)) {
        return false;
    }
    gpu_pgraph_backend backend;
    memset(&backend, 0, sizeof backend);
    backend.initial_pixels = initial;
    backend.table = &state.vertex.table;
    backend.read_guest = d3d8_gpu_read_guest;
    backend.load_module = load_module;
    backend.allowed_inferences = state.config.allowed_inferences;
    backend.viewport_inverse_modules = state.config.viewport_inverse_modules;
    backend.window_clip_modules = state.config.window_clip_modules;
    backend.viewport_from_target = state.config.viewport_from_target;
    backend.viewport_width = width;
    backend.viewport_height = height;
    backend.flip_y = state.config.flip_y;
    backend.line_width = state.config.line_width;
    backend.output_groups = state.config.output_groups;
    backend.combiner = state.config.combiner;
    backend.fragment_table = &state.fragment.table;
    backend.load_fragment_module = load_fragment_module;
    backend.draw_dump_path = state.config.draw_dump_path;
    if (state.config.standin_texture) {
        backend.test_textures[0].rgba = state.standin_pixels;
        backend.test_textures[0].width = state.config.standin_width;
        backend.test_textures[0].height = state.config.standin_height;
        backend.test_textures[0].unnormalised = state.config.standin_texel_units;
        memcpy(backend.standin_unit_rules, state.config.standin_unit_rules, sizeof backend.standin_unit_rules);
        backend.standin_unit_rule_count = state.config.standin_unit_rule_count;
    }
    if (state.config.render_target_texture) {
        backend.resolve_texture = provide_texture;
        backend.texture_sampled = texture_sampled;
        provider.passes = passes;
        provider.current = pass_count;
        provider.sampled = false;
        provider.surface_sampled = false;
    }
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    memset(pass, 0, sizeof *pass);
    const gpu_pgraph_result replayed =
        gpu_pgraph_replay_pass(state.pgraph, state.device, &backend, width, height, state.config.clear,
                               first_draw, draw_count, first_clear, clear_count, &pass->image, &report);
    if (replayed != GPU_PGRAPH_OK) {
        gpu_image_free(&pass->image);
        snprintf(what, what_size, "replay (%s) of draw %zu of %zu into render target 0x%08X: %s",
                 gpu_pgraph_result_string(replayed), report.failed_draw,
                 gpu_pgraph_draw_count(state.pgraph), (unsigned)target, report.error);
        return false;
    }
    pass->target = target;
    pass->width = width;
    pass->height = height;
    if (initial != NULL) {
        state.stats.targets_persisted++;
    }
    pass->drawn = report.drawn;
    pass->used_inferences = report.used_inferences | (provider.sampled ? GPU_PGRAPH_INFER_OUTPUT_TEXTURE_BRIDGE : 0u);
    pass->used_inferences |= (provider.surface_sampled ? GPU_PGRAPH_INFER_OUTPUT_SURFACE_SOURCE : 0u) |
                             (initial != NULL ? GPU_PGRAPH_INFER_OUTPUT_TARGET_PERSIST : 0u);
    pass->clears_applied = report.clears_applied;
    pass->offset_applied = report.offset_applied;
    pass->offset_unobserved = report.offset_unobserved;
    pass->textured_draws = report.textured_draws;
    return true;
}

/* The switches must describe one unbroken chain that ends at the target bound now. */
static bool check_switches(size_t recorded, uint32_t bound, char *what, size_t what_size)
{
    for (size_t i = 0u; i < state.switch_count; i++) {
        const target_switch *entry = &state.switches[i];
        const size_t earlier = i == 0u ? 0u : state.switches[i - 1u].stream_index;
        if (entry->stream_index < earlier || entry->stream_index > recorded) {
            snprintf(what, what_size,
                     "render target switch %zu is at recorded pair %zu, outside [%zu, %zu]: the "
                     "recording was released or reset between the switch and the present",
                     i, entry->stream_index, earlier, recorded);
            return false;
        }
        if (i != 0u && entry->from != state.switches[i - 1u].to) {
            snprintf(what, what_size,
                     "render target switch %zu starts from 0x%08X but the one before it bound "
                     "0x%08X: a target changed without SetRenderTarget", i, (unsigned)entry->from,
                     (unsigned)state.switches[i - 1u].to);
            return false;
        }
    }
    if (state.switch_count != 0u && state.switches[state.switch_count - 1u].to != bound) {
        snprintf(what, what_size,
                 "the last switch bound render target 0x%08X but 0x%08X is bound at the present: a "
                 "target changed without SetRenderTarget", (unsigned)state.switches[state.switch_count - 1u].to,
                 (unsigned)bound);
        return false;
    }
    return true;
}

/* --- the 2D engine blit over surface images (T578) -------------------------------------------------- */

static stored_surface *find_stored(uint32_t data)
{
    for (size_t i = 0u; i < D3D8_SWAP_REPLAY_SURFACES; i++) {
        if (state.surfaces[i].valid && state.surfaces[i].data == data) {
            return &state.surfaces[i];
        }
    }
    return NULL;
}

/* Surface images are kept only when something reads them: a blit (the BLIT group), a texture (T633 surface source) or a persistent target (T633). */
static bool surfaces_are_kept(void)
{
    return (state.config.output_groups & GPU_PGRAPH_OUTPUT_BLIT) != 0u || state.config.surface_source || state.config.target_persist;
}

/* The slot `data` is kept in: its own, a free one, or the oldest (evicted and counted), never `protect`. Its old image is released. */
static stored_surface *claim_slot(uint32_t data, const stored_surface *protect)
{
    stored_surface *slot = find_stored(data);
    for (size_t i = 0u; slot == NULL && i < D3D8_SWAP_REPLAY_SURFACES; i++) {
        if (!state.surfaces[i].valid) {
            slot = &state.surfaces[i];
        }
    }
    if (slot == NULL) {
        slot = &state.surfaces[state.surfaces_next++ % D3D8_SWAP_REPLAY_SURFACES];
        if (slot == protect) {
            slot = &state.surfaces[(state.surfaces_next++) % D3D8_SWAP_REPLAY_SURFACES];
        }
        state.stats.surfaces_evicted++;
    }
    gpu_image_free(&slot->image);
    memset(slot, 0, sizeof *slot);
    return slot;
}

/* Keep a copy of a replayed pass image under its Data word, replacing the older image of that surface, and never for a target whose header has no
 * Data word. Only when something reads the images (surfaces_are_kept). */
static void store_pass_image(const pass_result *pass)
{
    if (!surfaces_are_kept() || pass->data == 0u || pass->image.pixels == NULL) {
        return;
    }
    const size_t bytes = (size_t)pass->image.stride_bytes * pass->image.height;
    uint8_t *pixels = malloc(bytes);
    if (pixels == NULL) {
        stored_surface *lost = find_stored(pass->data);
        if (lost != NULL) {
            gpu_image_free(&lost->image);
            memset(lost, 0, sizeof *lost);
        }
        return;
    }
    memcpy(pixels, pass->image.pixels, bytes);
    stored_surface *slot = claim_slot(pass->data, NULL);
    slot->image = pass->image;
    slot->image.pixels = pixels;
    slot->valid = true;
    slot->touched = false;
    slot->guest_built = false;
    slot->data = pass->data;
    slot->pitch = pass->size.pitch;
    slot->format = pass->size.format;
}

/* T633 (2): the pixels a pass over `data` starts from, the kept image an earlier frame (and the blits since) left under it. `*initial` NULL is a
 * fresh image, the pass as it always was. False with `what` filled for a refusal by name. */
static bool persist_initial(uint32_t data, uint32_t width, uint32_t height, target_size size, const uint8_t **initial, char *what, size_t what_size)
{
    *initial = NULL;
    if (!state.config.target_persist || data == 0u) {
        return true;
    }
    const stored_surface *held = find_stored(data);
    if (held == NULL) {
        return true; /* nothing drew it before: a fresh image, as ever */
    }
    if (held->image.width != width || held->image.height != height || held->image.pixels == NULL ||
        held->image.stride_bytes != width * 4u || (held->pitch != 0u && size.pitch != 0u && held->pitch != size.pitch) ||
        (held->format != 0u && size.format != 0u && held->format != size.format)) {
        snprintf(what, what_size,
                 "target persistence: the kept image of Data 0x%07X is %ux%u (pitch %u, Format 0x%08X) but the target drawn into it is %ux%u (pitch %u, "
                 "Format 0x%08X): the surface was reused with another layout and its old pixels cannot be placed (target persistence is MEASURED in xemu, T736, for a surface of one layout)",
                 (unsigned)data, (unsigned)held->image.width, (unsigned)held->image.height, (unsigned)held->pitch, (unsigned)held->format,
                 (unsigned)width, (unsigned)height, (unsigned)size.pitch, (unsigned)size.format);
        return false;
    }
    const d3d8_surface_probe probe = d3d8_surface_model_probe(data, (uint64_t)(size.pitch != 0u ? size.pitch : width * 4u) * height);
    if (probe != D3D8_SURFACE_ZERO) {
        snprintf(what, what_size,
                 "target persistence: the guest memory under the kept image of Data 0x%07X is %s: the CPU wrote the surface and the order of that "
                 "write against the pass is not observed", (unsigned)data, probe == D3D8_SURFACE_NONZERO ? "not zero" : "unreadable");
        return false;
    }
    *initial = held->image.pixels;
    return true;
}

typedef enum { BLIT_SURFACE_OK, BLIT_SURFACE_REFUSED, BLIT_SURFACE_NO_IMAGE } blit_surface_result;

/* The image a blit reads or writes at draw position `position` of the frame (the blit followed that many draws): the newest pass
 * of the surface that finished by then, else the surface's image from an earlier frame. A pass that is mid-draw at the position
 * has no image yet and is refused, and so is a write into a surface a LATER pass draws on (that pass starts from a fresh image,
 * so the blit would be lost or would have to be composited, neither is modelled). NO_IMAGE (with `what` filled) says the replay holds no image
 * at all: a caller with the surface source builds one from guest memory, any other caller refuses with the text. */
static blit_surface_result resolve_blit_surface(const char *role, uint32_t data, size_t position, bool writing, pass_result *passes,
                                                size_t pass_count, gpu_image **image, stored_surface **stored, char *what,
                                                size_t what_size)
{
    pass_result *finished = NULL;
    bool later = false;
    for (size_t i = 0u; i < pass_count; i++) {
        if (passes[i].data != data) {
            continue;
        }
        if (passes[i].end_draw <= position) {
            finished = &passes[i];
        } else if (passes[i].first_draw < position) {
            snprintf(what, what_size,
                     "the %s surface (Data 0x%07X) is mid-draw at the blit (draws %zu to %zu of the frame, the blit follows %zu): "
                     "its image does not exist yet", role, (unsigned)data, passes[i].first_draw, passes[i].end_draw,
                     position);
            return BLIT_SURFACE_REFUSED;
        } else {
            later = true;
        }
    }
    *stored = NULL;
    if (finished != NULL) {
        *image = &finished->image;
        return BLIT_SURFACE_OK;
    }
    if (later && writing) {
        snprintf(what, what_size,
                 "the %s surface (Data 0x%07X) is drawn into by a later pass of the frame, which starts from a fresh image: "
                 "a blit before it is not modelled", role, (unsigned)data);
        return BLIT_SURFACE_REFUSED;
    }
    stored_surface *held = find_stored(data);
    if (held == NULL) {
        snprintf(what, what_size,
                 "the replay holds no image of the %s surface (Data 0x%07X): no pass of this frame finished drawing into it "
                 "and no earlier frame's pass did, so its pixels and size are unknown", role, (unsigned)data);
        return BLIT_SURFACE_NO_IMAGE;
    }
    *image = &held->image;
    *stored = held;
    return BLIT_SURFACE_OK;
}

/* T596: the rows [old height, rows) of a guest built surface, read from guest memory, so a later blit may reach further down than the first one did. */
static bool grow_guest_surface(stored_surface *surface, uint32_t rows, char *what, size_t what_size)
{
    if (!surface->guest_built || surface->image.height >= rows) {
        return true;
    }
    if (rows > 8192u) {
        snprintf(what, what_size, "the guest memory image of Data 0x%07X would grow to %u rows: more than the 8192 a guest built surface may hold", (unsigned)surface->data,
                 (unsigned)rows);
        return false;
    }
    const uint32_t width = surface->image.width;
    uint8_t *pixels = realloc(surface->image.pixels, (size_t)width * 4u * rows);
    if (pixels == NULL) {
        snprintf(what, what_size, "out of memory growing the guest memory image of Data 0x%07X to %u rows", (unsigned)surface->data, (unsigned)rows);
        return false;
    }
    surface->image.pixels = pixels;
    const uint32_t old_rows = surface->image.height;
    if (d3d8_surface_model_read_argb(surface->data + old_rows * surface->pitch, width, rows - old_rows, surface->pitch,
                                     pixels + (size_t)width * 4u * old_rows) != D3D8_SURFACE_MODEL_OK) {
        snprintf(what, what_size, "the guest memory of Data 0x%07X rows %u to %u cannot be read", (unsigned)surface->data, (unsigned)old_rows, (unsigned)rows);
        return false;
    }
    surface->image.height = rows;
    return true;
}

/* T596, INFERRED (SURFACE_SOURCE): a blit surface the replay holds no image of is the guest memory under it, `rows` rows of `pitch` bytes read as
 * A8R8G8B8 (a partial surface: only the rows a blit reaches, its height is what was read), kept under its Data word. */
static blit_surface_result guest_blit_surface(const char *role, uint32_t data, uint32_t pitch, uint32_t rows, const stored_surface *protect,
                                              gpu_image **image, stored_surface **stored, char *what, size_t what_size)
{
    const uint64_t bytes = (uint64_t)pitch * rows;
    if (pitch == 0u || pitch % 4u != 0u || pitch > 4u * 8192u || rows == 0u || rows > 8192u) {
        snprintf(what, what_size, "the %s surface (Data 0x%07X) has a pitch of %u bytes over %u rows: not an A8R8G8B8 surface the guest model can read",
                 role, (unsigned)data, (unsigned)pitch, (unsigned)rows);
        return BLIT_SURFACE_REFUSED;
    }
    if (d3d8_surface_model_has_bytes(data) || d3d8_surface_model_overlaps_bytes(data, bytes)) {
        snprintf(what, what_size, "the %s surface (Data 0x%07X) is held as bytes by an earlier byte path blit: an image blit over it is not modelled",
                 role, (unsigned)data);
        return BLIT_SURFACE_REFUSED;
    }
    for (size_t i = 0u; i < D3D8_SWAP_REPLAY_SURFACES; i++) {
        const stored_surface *other = &state.surfaces[i];
        if (other->valid && other->data != data && ranges_overlap(data, bytes, other->data, (uint64_t)other->image.width * 4u * other->image.height)) {
            snprintf(what, what_size, "the %s surface (Data 0x%07X) aliases the kept image of Data 0x%07X at a different offset", role, (unsigned)data,
                     (unsigned)other->data);
            return BLIT_SURFACE_REFUSED;
        }
    }
    const uint32_t width = pitch / 4u; /* T769: a pitch that is not a multiple of 4 is a stride as given, its last bytes are never touched by a pixel */
    uint8_t *pixels = malloc((size_t)width * 4u * rows);
    if (pixels == NULL) {
        snprintf(what, what_size, "out of memory for the %llu byte guest memory image of Data 0x%07X", (unsigned long long)width * 4u * rows, (unsigned)data);
        return BLIT_SURFACE_REFUSED;
    }
    if (d3d8_surface_model_read_argb(data, width, rows, pitch, pixels) != D3D8_SURFACE_MODEL_OK) {
        free(pixels);
        snprintf(what, what_size, "the %s surface (Data 0x%07X): the %llu bytes cannot be read from guest memory", role, (unsigned)data,
                 (unsigned long long)bytes);
        return BLIT_SURFACE_REFUSED;
    }
    stored_surface *slot = claim_slot(data, protect);
    slot->image.pixels = pixels;
    slot->image.width = width;
    slot->image.height = rows;
    slot->image.stride_bytes = width * 4u;
    slot->valid = true;
    slot->guest_built = true;
    slot->data = data;
    slot->pitch = pitch;
    state.stats.blit_guest_surfaces++;
    *image = &slot->image;
    *stored = slot;
    return BLIT_SURFACE_OK;
}

/* T596, INFERRED (SURFACE_SOURCE): a Y8 or R5G6B5 blit (the byte path of CopyRects), over guest BYTE surfaces. Neither surface may be one the replay holds an
 * image of (a pass of the frame, a kept image): bytes and pixels of one surface are not mixed. */
static bool apply_byte_copy(const gpu_pgraph_copy *copy, const pass_result *passes, size_t pass_count, const gpu_pgraph_backend *backend, char *what,
                            size_t what_size)
{
    const uint32_t datas[2] = {copy->source_offset, copy->destination_offset};
    const char *const roles[2] = {"source", "destination"};
    for (size_t side = 0u; side < 2u; side++) {
        bool image = find_stored(datas[side]) != NULL;
        for (size_t i = 0u; i < pass_count; i++) {
            image = image || passes[i].data == datas[side];
        }
        if (image) {
            snprintf(what, what_size, "a byte path blit (colour format 0x%X) %s surface Data 0x%07X is held by the replay as an A8R8G8B8 image: bytes and "
                     "pixels of one surface are not mixed", (unsigned)copy->color_format, roles[side], (unsigned)datas[side]);
            return false;
        }
    }
    const size_t source_need = gpu_pgraph_blit_extent_bytes(copy->color_format, copy->source_pitch, copy->in_x, copy->in_y,
                                                            gpu_pgraph_blit_row_pixels(copy), copy->height);
    const size_t destination_need = gpu_pgraph_blit_extent_bytes(copy->color_format, copy->destination_pitch, copy->out_x, copy->out_y,
                                                                 gpu_pgraph_blit_row_pixels(copy), copy->height);
    if (source_need == 0u || destination_need == 0u) {
        snprintf(what, what_size, "a byte path blit of colour format 0x%X has no extent the guest model can size: byte path", (unsigned)copy->color_format);
        return false;
    }
    uint8_t *source = NULL;
    uint8_t *destination = NULL;
    size_t source_length = source_need;
    size_t destination_length = destination_need;
    d3d8_surface_model_status status = D3D8_SURFACE_MODEL_OK;
    if (copy->source_offset == copy->destination_offset) {
        const size_t need = source_need > destination_need ? source_need : destination_need;
        status = d3d8_surface_model_acquire(copy->source_offset, need, &source);
        destination = source;
        source_length = destination_length = need;
    } else {
        status = d3d8_surface_model_acquire(copy->source_offset, source_need, &source);
        if (status == D3D8_SURFACE_MODEL_OK) {
            status = d3d8_surface_model_acquire(copy->destination_offset, destination_need, &destination);
        }
    }
    if (status != D3D8_SURFACE_MODEL_OK) {
        snprintf(what, what_size, "a byte path blit (colour format 0x%X, Data 0x%07X to 0x%07X): %s: byte path", (unsigned)copy->color_format,
                 (unsigned)copy->source_offset, (unsigned)copy->destination_offset, d3d8_surface_model_status_string(status));
        return false;
    }
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    uint32_t used = 0u;
    const gpu_pgraph_result applied =
        gpu_pgraph_replay_byte_copy(copy, backend, source, source_length, destination, destination_length, &used, &report);
    if (applied != GPU_PGRAPH_OK) {
        snprintf(what, what_size, "a byte path blit (colour format 0x%X, Data 0x%07X to 0x%07X): %s", (unsigned)copy->color_format,
                 (unsigned)copy->source_offset, (unsigned)copy->destination_offset,
                 report.error[0] != '\0' ? report.error : gpu_pgraph_result_string(applied));
        return false;
    }
    state.stats.used_inferences |= used | GPU_PGRAPH_INFER_OUTPUT_SURFACE_SOURCE;
    state.stats.byte_copies_applied++;
    return true;
}

/* Apply the frame's blits, in stream order, over the pass images and the stored surface images. False with `what` filled for a
 * refusal (the caller refuses the frame). An empty blit (zero width or height) is counted and moves nothing. */
static bool apply_copies(uint64_t frame, pass_result *passes, size_t pass_count, char *what, size_t what_size)
{
    const size_t total = gpu_pgraph_copy_count(state.pgraph);
    const gpu_pgraph_backend backend = {.output_groups = state.config.output_groups,
                                        .allowed_inferences = state.config.allowed_inferences,
                                        .flip_y = state.config.flip_y};
    for (size_t i = 0u; i < total; i++) {
        const gpu_pgraph_copy *copy = gpu_pgraph_copy_at(state.pgraph, i);
        gpu_image *source = NULL;
        gpu_image *destination = NULL;
        stored_surface *stored_source = NULL;
        stored_surface *stored_destination = NULL;
        const bool byte_path = copy->width != 0u && copy->height != 0u && copy->color_format != GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8 &&
                               state.config.surface_source;
        if (byte_path) {
            char reason[300];
            if (!apply_byte_copy(copy, passes, pass_count, &backend, reason, sizeof reason)) {
                snprintf(what, what_size, "blit %zu of %zu (pair %u): %s", i + 1u, total, (unsigned)copy->command, reason);
                return false;
            }
            state.stats.copies_applied += 1u;
            (void)d3d8_hle_log()("d3d8 swap replay: frame %llu: blit %zu of %zu: byte path, Data 0x%07X (%u, %u) -> 0x%07X (%u, %u), %ux%u, colour "
                                 "format 0x%X, pitches %u and %u, over guest byte surfaces (INFERRED)\n", (unsigned long long)frame, i + 1u, total,
                                 (unsigned)copy->source_offset, (unsigned)copy->in_x, (unsigned)copy->in_y, (unsigned)copy->destination_offset,
                                 (unsigned)copy->out_x, (unsigned)copy->out_y, (unsigned)copy->width, (unsigned)copy->height,
                                 (unsigned)copy->color_format, (unsigned)copy->source_pitch, (unsigned)copy->destination_pitch);
            continue;
        }
        if (copy->width != 0u && copy->height != 0u) {
            char reason[300];
            const bool guest = state.config.surface_source && copy->color_format == GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8;
            blit_surface_result found = resolve_blit_surface("source", copy->source_offset, copy->before_draw, false, passes, pass_count, &source,
                                                             &stored_source, reason, sizeof reason);
            if (found == BLIT_SURFACE_NO_IMAGE && guest) {
                found = guest_blit_surface("source", copy->source_offset, copy->source_pitch, copy->in_y + copy->height, NULL, &source,
                                           &stored_source, reason, sizeof reason);
            }
            if (found == BLIT_SURFACE_OK) {
                found = resolve_blit_surface("destination", copy->destination_offset, copy->before_draw, true, passes, pass_count, &destination,
                                             &stored_destination, reason, sizeof reason);
                if (found == BLIT_SURFACE_NO_IMAGE && guest) {
                    found = guest_blit_surface("destination", copy->destination_offset, copy->destination_pitch, copy->out_y + copy->height,
                                               stored_source, &destination, &stored_destination, reason, sizeof reason);
                }
            }
            if (found == BLIT_SURFACE_OK && guest && stored_source != NULL && !grow_guest_surface(stored_source, copy->in_y + copy->height, reason, sizeof reason)) {
                found = BLIT_SURFACE_REFUSED;
            }
            if (found == BLIT_SURFACE_OK && guest && stored_destination != NULL &&
                !grow_guest_surface(stored_destination, copy->out_y + copy->height, reason, sizeof reason)) {
                found = BLIT_SURFACE_REFUSED;
            }
            if (found != BLIT_SURFACE_OK) {
                snprintf(what, what_size, "blit %zu of %zu (pair %u): %s", i + 1u, total, (unsigned)copy->command, reason);
                return false;
            }
        }
        gpu_pgraph_report report;
        memset(&report, 0, sizeof report);
        uint32_t used = 0u;
        const gpu_pgraph_result applied =
            gpu_pgraph_replay_copy(copy, &backend, source, destination, &used, &report);
        if (applied != GPU_PGRAPH_OK) {
            snprintf(what, what_size, "blit %zu of %zu (pair %u, Data 0x%07X to 0x%07X): %s", i + 1u, total,
                     (unsigned)copy->command, (unsigned)copy->source_offset, (unsigned)copy->destination_offset,
                     report.error[0] != '\0' ? report.error : gpu_pgraph_result_string(applied));
            return false;
        }
        if (copy->width == 0u || copy->height == 0u) {
            state.stats.copies_empty++;
            continue;
        }
        state.stats.copies_applied++;
        state.stats.used_inferences |= used;
        if ((stored_source != NULL && stored_source->guest_built) || (stored_destination != NULL && stored_destination->guest_built)) {
            state.stats.used_inferences |= GPU_PGRAPH_INFER_OUTPUT_SURFACE_SOURCE; /* a surface built from guest memory (T596) depends on the surface source */
        }
        (void)d3d8_hle_log()("d3d8 swap replay: frame %llu: blit %zu of %zu: Data 0x%07X (%u, %u) -> 0x%07X (%u, %u), %ux%u, colour "
                             "format 0x%X, pitches %u and %u\n", (unsigned long long)frame, i + 1u, total,
                             (unsigned)copy->source_offset, (unsigned)copy->in_x, (unsigned)copy->in_y,
                             (unsigned)copy->destination_offset, (unsigned)copy->out_x, (unsigned)copy->out_y,
                             (unsigned)copy->width, (unsigned)copy->height, (unsigned)copy->color_format,
                             (unsigned)copy->source_pitch, (unsigned)copy->destination_pitch);
        if (stored_destination != NULL) {
            stored_destination->touched = true;
        }
    }
    if (dump_due(frame)) {
        for (size_t i = 0u; i < D3D8_SWAP_REPLAY_SURFACES; i++) {
            if (state.surfaces[i].valid && state.surfaces[i].touched) {
                char suffix[40];
                snprintf(suffix, sizeof suffix, "_blit_surface_%07X", (unsigned)state.surfaces[i].data);
                dump_image(frame, suffix, &state.surfaces[i].image);
            }
        }
    }
    for (size_t i = 0u; i < D3D8_SWAP_REPLAY_SURFACES; i++) {
        state.surfaces[i].touched = false;
    }
    (void)d3d8_hle_log()("d3d8 swap replay: frame %llu: %zu 2D engine blit(s) (CopyRects) applied over surface images "
                         "(INFERRED, xemu's image blit)\n", (unsigned long long)frame, total);
    return true;
}

/* The target a segment of the frame's draw list drew into: before the first switch, what that switch left, after switch k what
 * it bound, with no switch at all what is bound at the present. */
static void segment_target(size_t segment, uint32_t bound, uint32_t *target, uint32_t *data, target_size *size)
{
    *target = bound;
    *data = surface_data(bound);
    *size = measure_target(bound);
    if (state.switch_count != 0u) {
        const target_switch *entry = segment == 0u ? &state.switches[0] : &state.switches[segment - 1u];
        *target = segment == 0u ? entry->from : entry->to;
        *data = segment == 0u ? entry->from_data : entry->to_data;
        *size = segment == 0u ? entry->from_size : entry->to_size;
    }
}

static void present_frame(uint64_t frame_number)
{
    if (!state.active) {
        return;
    }
    follow_gpu_reset();
    state.stats.presents++;
    if (state.stats.latched) {
        state.stats.frames_skipped++;
        d3d8_gpu_stream_discard(d3d8_gpu_stream_count());
        return;
    }
    pass_result passes[MAX_PASSES];
    size_t pass_count = 0u;
    char what[460];
    if (d3d8_gpu_get_stats().commands_dropped != 0u) {
        refuse_frame(frame_number, "the recording filled before it was decoded, so commands were "
                                   "dropped and the model state is incomplete", passes, 0u);
        return;
    }
    const size_t recorded = d3d8_gpu_stream_count();
    const uint32_t bound = d3d8_device_load32(DEV_RENDER_TARGET);
    if (state.switch_overflow) {
        snprintf(what, sizeof what, "more than %u render target switches in one frame", MAX_SWITCHES);
        refuse_frame(frame_number, what, passes, 0u);
        return;
    }
    if (state.texture_overflow) {
        snprintf(what, sizeof what, "more than %u SetTexture bindings in one frame (render target texture)",
                 D3D8_SWAP_REPLAY_TEXTURE_EVENTS);
        refuse_frame(frame_number, what, passes, 0u);
        return;
    }
    if (!check_switches(recorded, bound, what, sizeof what)) {
        refuse_frame(frame_number, what, passes, 0u);
        return;
    }
    /* Catch up: what was recorded before the replay was enabled and has had no kick since is decoded
     * here, so its vertex bytes are read now (the only case where the present reads guest memory). Every
     * other command was decoded, and its vertices copied, at its kick. */
    if (can_decode()) {
        decode_pending();
    }
    log_new_unhandled(frame_number);
    if (state.decode_failed) {
        refuse_frame(frame_number, state.decode_error, passes, 0u);
        return;
    }
    for (size_t i = 0u; i < state.switch_count; i++) {
        if (state.switches[i].bracket_open) {
            refuse_frame(frame_number, "a BEGIN_END bracket was still open at a render target switch",
                         passes, 0u);
            return;
        }
    }
    if (gpu_pgraph_in_bracket(state.pgraph)) {
        refuse_frame(frame_number, "a BEGIN_END bracket was still open at the present", passes, 0u);
        return;
    }
    const size_t segments = state.switch_count + 1u;
    const size_t frame_draws = gpu_pgraph_draw_count(state.pgraph);
    segment_plan plan[MAX_PASSES];
    size_t plan_count = 0u;
    if (state.config.render_target_texture) {
        /* every pass the frame will replay, so a texture can tell an earlier producer from a later one */
        for (size_t segment = 0u; segment < segments; segment++) {
            const size_t first = segment == 0u ? 0u : state.switches[segment - 1u].draws;
            const size_t end = segment < state.switch_count ? state.switches[segment].draws : frame_draws;
            if (end > frame_draws || end < first || end == first) {
                continue; /* a malformed segment is refused by the replay loop below, an empty one is not a pass */
            }
            segment_plan *entry = &plan[plan_count++];
            entry->first = first;
            entry->end = end;
            segment_target(segment, bound, &entry->target, &entry->data, &entry->size);
        }
        provider.plan = plan;
        provider.plan_count = plan_count;
        memset(provider.source, 0, sizeof provider.source);
    }
    if (live_model_hook != NULL) {
        live_model_frames++;
        live_model_hook(state.pgraph, frame_number, live_model_hook_context);
    }
    if (state.config.live_only) {
        d3d8_gpu_stream_discard(state.next);
        state.next = 0u;
        state.switch_count = 0u;
        state.switch_overflow = false;
        (void)gpu_pgraph_begin_frame(state.pgraph);
        state.stats.frames_empty++;
        return;
    }
    if (state.config.render_target_texture_census) {
        census_frame(plan, plan_count);
        state.stats.census_frames++;
        d3d8_gpu_stream_discard(state.next);
        state.next = 0u;
        state.switch_count = 0u;
        state.switch_overflow = false;
        (void)gpu_pgraph_begin_frame(state.pgraph);
        state.stats.frames_empty++;
        return;
    }
    for (size_t segment = 0u; segment < segments; segment++) {
        /* The draws of this segment: from where the previous switch left the list to where this one
         * did, and the rest of the frame's list for the last. */
        const size_t first = segment == 0u ? 0u : state.switches[segment - 1u].draws;
        const size_t end = segment < state.switch_count ? state.switches[segment].draws : frame_draws;
        if (end > frame_draws || end < first) {
            refuse_frame(frame_number, "a render target switch lies outside the frame's draw list",
                         passes, pass_count);
            return;
        }
        if (end != first) {
            /* The target this segment drew into: before the first switch, what that switch left;
             * after switch k, what it bound; with no switch at all, what is bound now. */
            uint32_t target_data = 0u;
            uint32_t target = 0u;
            target_size size;
            segment_target(segment, bound, &target, &target_data, &size);
            /* The clear events of this segment: those the model held between the switch before it and the switch
             * after it (all the rest for the last segment), so a clear in a pass with no draw is never given to the
             * pass before it, which draws into another target. A note that disagrees with the list (end before first, or
             * past it) makes the count wrap or overrun, which gpu_pgraph_replay_pass refuses as a range outside the list. */
            const size_t first_clear = segment == 0u ? 0u : state.switches[segment - 1u].clears;
            const size_t end_clear =
                segment < state.switch_count ? state.switches[segment].clears : gpu_pgraph_clear_count(state.pgraph);
            if (!replay_pass(target, target_data, size, passes, pass_count, first, end - first, first_clear,
                             end_clear - first_clear, &passes[pass_count], what, sizeof what)) {
                refuse_frame(frame_number, what, passes, pass_count);
                return;
            }
            passes[pass_count].data = target_data;
            passes[pass_count].size = size;
            passes[pass_count].first_draw = first;
            passes[pass_count].end_draw = end;
            pass_count++;
        }
    }
    if (gpu_pgraph_copy_count(state.pgraph) != 0u &&
        !apply_copies(frame_number, passes, pass_count, what, sizeof what)) {
        refuse_frame(frame_number, what, passes, pass_count);
        return;
    }
    fold_vertex_stats();
    {
        size_t applied = 0u;
        for (size_t i = 0u; i < pass_count; i++) {
            applied += passes[i].clears_applied;
        }
        const size_t recorded_clears = gpu_pgraph_clear_count(state.pgraph);
        if (recorded_clears > applied) {
            state.stats.clears_not_replayed += recorded_clears - applied;
            (void)d3d8_hle_log()("d3d8 swap replay: frame %llu: %zu clear(s) (0x1D94) were not replayed: they "
                                 "sit in a render target pass with no draw, which has no image\n",
                                 (unsigned long long)frame_number, recorded_clears - applied);
        }
    }
    d3d8_gpu_stream_discard(state.next);
    state.next = 0u;
    state.switch_count = 0u;
    state.switch_overflow = false;
    (void)gpu_pgraph_begin_frame(state.pgraph); /* cannot refuse: the bracket was checked closed above */
    if (pass_count == 0u) {
        state.stats.frames_empty++;
        return;
    }

    /* Commit: the pass of the target bound at the present is the presented frame, the others are
     * the frame's offscreen targets. */
    for (size_t i = 0u; i < state.offscreen_count; i++) {
        gpu_image_free(&state.offscreen[i].image);
    }
    state.offscreen_count = 0u;
    bool presented = false;
    for (size_t i = 0u; i < pass_count; i++) {
        store_pass_image(&passes[i]);
        state.stats.passes_replayed++;
        state.stats.draws += passes[i].drawn;
        state.stats.standin_draws += passes[i].textured_draws;
        state.stats.offset_applied += passes[i].offset_applied;
        state.stats.offset_unobserved += passes[i].offset_unobserved;
        if (passes[i].offset_applied != 0u || passes[i].offset_unobserved != 0u) {
            /* never silent (T502), and silent when there is nothing to say: the title's loop writes a zero offset */
            (void)d3d8_hle_log()("d3d8 swap replay: frame %llu: render target 0x%08X: %u draw(s) ran with a polygon "
                                 "offset (Vulkan depth bias), %u draw(s) had an enabled non-zero offset that cannot "
                                 "show (no depth test, or a line or point list) (unobserved)\n",
                                 (unsigned long long)frame_number, (unsigned)passes[i].target,
                                 (unsigned)passes[i].offset_applied, (unsigned)passes[i].offset_unobserved);
        }
        state.stats.used_inferences |= passes[i].used_inferences;
        if (passes[i].target == bound && !presented) {
            presented = true;
            gpu_image_free(&state.last);
            state.last = passes[i].image;
            state.stats.last_width = passes[i].width;
            state.stats.last_height = passes[i].height;
            continue;
        }
        state.offscreen[state.offscreen_count++] = passes[i];
    }
    const bool due = dump_due(frame_number);
    note_overlay(frame_number); /* any replayed frame: the composition's target is usually not the bound one (frames_offscreen_only) */
    if (presented) {
        state.stats.frames_replayed++;
        state.last_frame = frame_number;
        state.last_dumped = due;
        if (due) {
            dump_image(frame_number, "", &state.last);
        }
    } else {
        state.stats.frames_offscreen_only++;
    }
    state.offscreen_frame = frame_number;
    state.offscreen_dumped = due;
    if (due) {
        dump_offscreen(frame_number);
    }
}

/* T633 (3): is the overlay on while a frame is replayed. MEASURED: the title's UpdateOverlay calls pass the colour key DISABLED (key enable 0, the format register
 * `control 0x10A00` has no key bit 0x100000, all 169 updates of the retail movie boot), so the key changes nothing and the overlay hardware shows its YUY2 picture over
 * its whole destination rectangle (MEASURED in xemu by T770: a clear key bit shows the overlay on every pixel of its rectangle), whatever the framebuffer holds. This replay
 * does NOT compose the overlay picture over its frames (the opt-in --overlay-xemu-key does, in the frame hook, T831), so a frame replayed under the overlay is the
 * framebuffer UNDER it, not the displayed picture. An update with the key ENABLED (T770 measured it: the overlay shows where the framebuffer RGB equals the key RGB, alpha
 * ignored, but no title passes it) is REFUSED by name here, as a frame this replay cannot make. Counted, and logged at the first frame of each kind, never silent, never a latch. */
static void note_overlay(uint64_t frame)
{
    const d3d8_overlay_descriptor overlay = d3d8_overlay_state();
    if (overlay.updates == 0u || (overlay.enables & 1u) == 0u) {
        return; /* no UpdateOverlay yet, or EnableOverlay was last called with its closing 0 */
    }
    state.stats.frames_under_overlay++;
    if (overlay.color_key_enable == 0u) {
        if (state.stats.frames_under_overlay == 1u) {
            (void)d3d8_hle_log()("d3d8 swap replay: frame %llu: the overlay is on with its colour key DISABLED (MEASURED, control 0x%X): the key changes nothing and the overlay "
                                 "covers its destination rectangle %u,%u to %u,%u over the framebuffer (INFERRED). The replay does not compose the overlay picture: a replayed frame is "
                                 "the framebuffer UNDER the overlay, not the displayed picture (HQ60)\n",
                                 (unsigned long long)frame, (unsigned)overlay.control, (unsigned)overlay.destination[0], (unsigned)overlay.destination[1],
                                 (unsigned)overlay.destination[2], (unsigned)overlay.destination[3]);
        }
        return;
    }
    state.stats.frames_under_color_key++;
    if (state.stats.frames_under_color_key == 1u) {
        (void)d3d8_hle_log()("d3d8 swap replay: frame %llu: REFUSED BY NAME (T633 (3), not a latch): the overlay is on with its colour key ENABLED (key 0x%08X, destination "
                             "%u,%u to %u,%u): xemu shows the overlay where the framebuffer RGB equals the key RGB (T770, HQ60) and this replay does not compose the overlay\n",
                             (unsigned long long)frame, (unsigned)overlay.color_key, (unsigned)overlay.destination[0], (unsigned)overlay.destination[1],
                             (unsigned)overlay.destination[2], (unsigned)overlay.destination[3]);
    }
}

size_t d3d8_swap_replay_surface_summary(char *text, size_t size)
{
    if (text == NULL || size == 0u) {
        return 0u;
    }
    text[0] = '\0';
    if (!state.active || (!state.config.surface_source && !state.config.target_persist && state.stats.frames_under_overlay == 0u)) {
        return 0u;
    }
    const d3d8_overlay_descriptor overlay = d3d8_overlay_state();
    const int written = snprintf(
        text, size,
        "SURFACE MODEL (T633, T596), opt-in, the rules MEASURED in xemu (T736, xemu-level) and the INFERRED parts named: (1) surface source %s: a surface no pass of the frame drew is its current memory image, the kept image from an earlier frame "
        "(%llu sample(s)), else the guest memory under it read as A8R8G8B8 (%llu sample(s), %llu all zero = transparent black, MEASURED bytes), %llu blit surface(s) built from "
        "guest memory, %llu byte path blit(s) (Y8, R5G6B5) over guest byte surfaces; a size, pitch or Format that differs, a CPU written surface under a kept image, an alias "
        "and a blit before the draw are REFUSED by name. (2) target persistence %s: %llu pass(es) started from the kept image an earlier frame left instead of a fresh image (MEASURED in xemu: a pass without a clear starts from the previous image). "
        "(3) the overlay colour key (T537): the title's UpdateOverlay calls pass the key DISABLED (MEASURED), so it changes nothing and the overlay covers its destination "
        "rectangle (MEASURED in xemu, T770); an update with the key ENABLED is REFUSED by name (measured by T770, not composed here, last key 0x%08X); the overlay picture is NOT composed over the replayed "
        "frames: %llu replayed frame(s) were under the overlay, %llu with the key enabled, each is the framebuffer UNDER it and not the displayed picture; the xemu measurement "
        "that decided it is HQ60 (T770), the opt-in --overlay-xemu-key composes it in the frame hook (T831)",
        state.config.surface_source ? "ON" : "off", (unsigned long long)state.stats.surface_kept_samples,
        (unsigned long long)state.stats.surface_guest_samples, (unsigned long long)state.stats.surface_guest_zero_samples,
        (unsigned long long)state.stats.blit_guest_surfaces, (unsigned long long)state.stats.byte_copies_applied,
        state.config.target_persist ? "ON" : "off", (unsigned long long)state.stats.targets_persisted,
        (unsigned)overlay.color_key, (unsigned long long)state.stats.frames_under_overlay, (unsigned long long)state.stats.frames_under_color_key);
    if (written < 0) {
        text[0] = '\0';
        return 0u;
    }
    return strlen(text);
}

void d3d8_swap_replay_on_present(uint64_t frame_number)
{
    guest_frame_trace_claim(); /* T1289: the thread that Swaps is the one the per interval trace follows */
    guest_frame_trace_swap();
    const uint64_t before = state.stats.frames_replayed;
    present_frame(frame_number);
    if (state.stats.frames_replayed != before && live_frame_hook != NULL) {
        live_frame_hook(d3d8_overlay_key_displayed(&state.last), frame_number, live_frame_hook_context);
    }
    provider.passes = NULL;
    provider.plan = NULL;
    provider.plan_count = 0u;
    roll_texture_events();
}

void d3d8_swap_replay_set_live_hook(d3d8_swap_replay_live_hook hook, void *context)
{
    live_model_hook = hook;
    live_model_hook_context = context;
}

void d3d8_swap_replay_set_live_target_hook(d3d8_swap_replay_live_target_hook hook, void *context)
{
    live_target_hook = hook;
    live_target_hook_context = context;
}

uint64_t d3d8_swap_replay_live_frames(void)
{
    return live_model_frames;
}

/* T1246: the SetTexture history of the frame as it stood at the Swap, so the presenter thread can answer the binding of a draw
 * after the guest moved on (the live history is rolled at the end of the present and rewritten by the next frame). */
struct d3d8_swap_replay_live_bindings {
    texture_event *events;
    size_t count;
    texture_binding carry[TEXTURE_STAGES];
    bool carry_known[TEXTURE_STAGES];
};

d3d8_swap_replay_live_bindings *d3d8_swap_replay_live_bindings_create(void)
{
    d3d8_swap_replay_live_bindings *copy = calloc(1u, sizeof *copy);
    if (copy != NULL) {
        copy->events = calloc(D3D8_SWAP_REPLAY_TEXTURE_EVENTS, sizeof *copy->events);
        if (copy->events == NULL) {
            free(copy);
            return NULL;
        }
    }
    return copy;
}

void d3d8_swap_replay_live_bindings_destroy(d3d8_swap_replay_live_bindings *copy)
{
    if (copy != NULL) {
        free(copy->events);
        free(copy);
    }
}

void d3d8_swap_replay_live_bindings_capture(d3d8_swap_replay_live_bindings *copy)
{
    copy->count = state.texture_event_count;
    memcpy(copy->events, state.texture_events, copy->count * sizeof *copy->events);
    memcpy(copy->carry, state.texture_carry, sizeof copy->carry);
    memcpy(copy->carry_known, state.texture_carry_known, sizeof copy->carry_known);
}

static bool live_binding_fill(const texture_binding *binding, uint32_t *header, uint32_t *format, uint32_t *size_word, uint32_t *data,
                              const char **reason)
{
    if (binding == NULL) {
        *reason = "no binding observed: no SetTexture of the stage was seen since the live renderer was enabled";
        return false;
    }
    if (!binding->bound) {
        *reason = "unbound: the stage's last SetTexture was 0";
        return false;
    }
    *header = binding->header;
    *format = binding->format;
    *size_word = binding->size_word;
    *data = binding->data;
    return true;
}

bool d3d8_swap_replay_live_bindings_lookup(const d3d8_swap_replay_live_bindings *copy, size_t draw, uint32_t stage, uint32_t *header,
                                           uint32_t *format, uint32_t *size_word, uint32_t *data, const char **reason)
{
    const texture_binding *found = NULL;
    if (stage < TEXTURE_STAGES) {
        found = copy->carry_known[stage] ? &copy->carry[stage] : NULL;
        for (size_t i = 0u; i < copy->count; i++) {
            if (copy->events[i].stage == stage && copy->events[i].draw <= draw) {
                found = &copy->events[i].binding;
            }
        }
    }
    return live_binding_fill(found, header, format, size_word, data, reason);
}

bool d3d8_swap_replay_live_binding(size_t draw, uint32_t stage, uint32_t *header, uint32_t *format, uint32_t *size_word, uint32_t *data,
                                   const char **reason)
{
    const texture_binding *binding = stage < TEXTURE_STAGES ? binding_at(stage, draw) : NULL;
    if (binding == NULL) {
        *reason = "no binding observed: no SetTexture of the stage was seen since the live renderer was enabled";
        return false;
    }
    if (!binding->bound) {
        *reason = "unbound: the stage's last SetTexture was 0";
        return false;
    }
    *header = binding->header;
    *format = binding->format;
    *size_word = binding->size_word;
    *data = binding->data;
    return true;
}

/* T847: append a module that appeared in the directory to a set. The sorted part keeps its indices (a lookup by name is linear), so a
 * pipeline cache keyed by module index stays valid. The words of the existing modules are separate allocations and stay put. */
static bool append_module(module_set *set, const char *name)
{
    char *copy = copy_string(name);
    char **names = realloc(set->names, (set->count + 1u) * sizeof *names);
    if (names != NULL) {
        set->names = names;
    }
    uint32_t **words = realloc(set->words, (set->count + 1u) * sizeof *words);
    if (words != NULL) {
        set->words = words;
    }
    size_t *counts = realloc(set->word_counts, (set->count + 1u) * sizeof *counts);
    if (counts != NULL) {
        set->word_counts = counts;
    }
    if (copy == NULL || names == NULL || words == NULL || counts == NULL) {
        free(copy);
        return false;
    }
    set->names[set->count] = copy;
    set->words[set->count] = NULL;
    set->word_counts[set->count] = 0u;
    set->count++;
    set->table.module_count = set->count;
    set->table.module_names = (const char *const *)set->names;
    return true;
}

static bool swap_make_module(void *context, bool fragment, const char *name, const uint8_t *bytes, size_t byte_count, char *error,
                             size_t error_bytes)
{
    (void)context;
    if (live_module_maker == NULL || !state.active) {
        return false;
    }
    if (!live_module_maker(live_module_maker_context, fragment, name, bytes, byte_count, error, error_bytes)) {
        return false;
    }
    module_set *set = fragment ? &state.fragment : &state.vertex;
    uint32_t existing = 0u;
    if (gpu_vsh_lookup_name(&set->table, name, &existing) || append_module(set, name)) {
        return true;
    }
    snprintf(error, error_bytes, "out of memory adding the module %s", name);
    return false;
}

void d3d8_swap_replay_set_live_module_maker(gpu_pgraph_module_make_fn maker, void *context)
{
    live_module_maker = maker;
    live_module_maker_context = context;
}

bool d3d8_swap_replay_live_backend(gpu_pgraph_backend *out)
{
    if (!state.active) {
        return false;
    }
    memset(out, 0, sizeof *out);
    out->table = &state.vertex.table;
    out->read_guest = d3d8_gpu_read_guest;
    out->load_module = load_module;
    out->allowed_inferences = state.config.allowed_inferences;
    out->viewport_inverse_modules = state.config.viewport_inverse_modules;
    out->window_clip_modules = state.config.window_clip_modules;
    out->viewport_from_target = false; /* needs the replay's per pass size, the live draw reads its own target */
    out->flip_y = state.config.flip_y;
    out->line_width = state.config.line_width;
    out->output_groups = state.config.output_groups;
    out->combiner = state.config.combiner;
    out->fragment_table = &state.fragment.table;
    out->load_fragment_module = load_fragment_module;
    if (live_module_maker != NULL) {
        out->make_module = swap_make_module;
    }
    return true;
}

void d3d8_swap_replay_set_frame_hook(d3d8_swap_replay_frame_hook hook, void *context)
{
    live_frame_hook = hook;
    live_frame_hook_context = context;
}

void d3d8_swap_replay_dump_last(void)
{
    if (!state.active || state.dump_directory == NULL || !state.config.dump_last) {
        return;
    }
    if (state.last.pixels != NULL && !state.last_dumped) {
        state.last_dumped = true;
        dump_image(state.last_frame, "", &state.last);
    }
    if (state.offscreen_count != 0u && !state.offscreen_dumped) {
        state.offscreen_dumped = true;
        dump_offscreen(state.offscreen_frame);
    }
}
