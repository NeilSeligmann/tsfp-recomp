/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "guest_frame_trace.h"
#include "gpu_phase_timing.h"
#include "live_render.h"

#include <string.h>

#include "live_module_maker.h"

#ifdef TSFP_HAVE_SDL3

#include "d3d8_gpu.h"
#include "d3d8_gpu_pgraph.h"
#include "d3d8_guest.h"
#include "d3d8_present.h"
#include "d3d8_visibility.h"
#include "d3d8_surface.h"
#include "d3d8_swap_replay.h"
#include "kernel_clock.h"
#include "live_vk_draw.h"
#include "live_vk_frame.h"
#include "live_texture_inputs.h"
#include "live_report_pending.h"
#include "live_module_maker.h"
#include "gpu_pgraph_replay.h"
#include "gpu_sha256.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

#define EXPIRY_FRAMES 3u
#define KNOWN_TARGETS 32u
#define REASON_GROUPS 48u

typedef struct {
    uint32_t data, format, size;
} known_target;

/* T1246: everything one pipelined frame needs that the guest thread would otherwise change while the presenter draws it. Two of them
 * alternate: the presenter draws one while the guest thread fills the other. */
#define INPUT_BUDGET_BYTES (64u << 20)

typedef struct {
    gpu_pgraph *model; /* a clone of the decoder's frame */
    d3d8_swap_replay_live_bindings *bindings;
    live_texture_inputs *inputs;
    uint64_t clock;            /* kernel_clock_peek at the Swap, the timestamp of the frame's query reports */
    size_t pictures_pending;   /* the sink's playback queue depth at the Swap */
} live_package;

typedef struct {
    bool open_ok, attached;
    present_video_sink *sink;
    bool playback;
    live_render_config config;
    gpu_window *window;
    live_texture_cache cache;
    live_vk_texture_set *textures;
    live_vk_target_set *targets;
    live_vk_bind *bind;
    live_vk_frame *frame;
    gpu_pgraph_backend backend;
    const gpu_pgraph *model;
    FILE *frame_hash;                 /* T1246: --live-frame-hash FILE, one sha256 line per frame handed to the window */
    unsigned long long frames_hashed, jobs_digested;
    uint64_t pipeline_cache_saved_ticks;
    uint32_t pipeline;                /* T1246: frames in flight, 0 = serial */
    live_package packages[2];
    unsigned next_package;
    bool scouting;                    /* T1247: presenter thread, inside live_render_make_module */
    unsigned long long scout_calls, scout_batches, scout_modules; /* T1247 */
    live_report_pending *pending;
    const live_package *job;          /* presenter thread: the package being drawn, NULL in a serial job */
    unsigned long long pipelined_frames, serial_frames, serial_not_self_contained, serial_clone_failed, serial_capture_full, serial_post_failed;
    unsigned long long max_inputs_entries, max_inputs_bytes, max_frame_draws;
    unsigned long long slot_checks, slot_waits, slot_wait_ns, slot_wait_max_ns; /* guest thread, report slot waits */
    uint8_t *last_rgba; /* the last frame handed to the swapchain, for read_pixel and capture */
    size_t last_capacity;
    uint32_t last_width, last_height;
    live_render_report_data counters;
    d3d8_frame_record record;
    uint64_t record_media_time_ns;
    bool record_media_timed;
    uint32_t header;
    known_target known[KNOWN_TARGETS]; /* guest thread only: targets already marshalled */
    size_t known_count;
    _Atomic uint32_t frames_since_picture;
    _Atomic uint64_t pictures;
    _Atomic bool overlay_expiry_applied;
    uint64_t pictures_seen;
    char attach_error[256];
    bool attach_ok;
} live_state;

static live_state g;

/* ---- presenter thread ------------------------------------------------------------------------------------------------------- */

static bool present_pixels(void *context, const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t stride)
{
    (void)context;
    const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const size_t bytes = (size_t)width * height * 4u;
    if (bytes > g.last_capacity) {
        uint8_t *grown = realloc(g.last_rgba, bytes);
        if (grown != NULL) {
            g.last_rgba = grown;
            g.last_capacity = bytes;
        }
    }
    if (bytes <= g.last_capacity) {
        for (uint32_t row = 0u; row < height; row++) {
            memcpy(g.last_rgba + (size_t)row * width * 4u, rgba + (size_t)row * stride, (size_t)width * 4u);
        }
        g.last_width = width;
        g.last_height = height;
        if (g.frame_hash != NULL) {
            uint8_t digest[32];
            char hex[65];
            gpu_sha256(g.last_rgba, bytes, digest);
            gpu_sha256_hex(digest, hex);
            fprintf(g.frame_hash, "F %llu %ux%u %s\n", g.frames_hashed++, (unsigned)width, (unsigned)height, hex);
            fflush(g.frame_hash);
        }
    }
    return gpu_window_present_pixels(g.window, rgba, width, height, stride, clear);
}

/* T1267: --live-blit-verify. After a vblank the blit presented, present the same frame by the readback route (the reference
 * compositor, front read back then scaled on the CPU) too, read both swapchain images back through the capture buffer and compare. */
static uint8_t *s_verify_a, *s_verify_b;
static size_t s_verify_capacity;

static void verify_sample(uint64_t vblank)
{
    uint32_t width = 0u, height = 0u;
    gpu_window_extent(g.window, &width, &height);
    const size_t bytes = (size_t)width * height * 4u;
    if (bytes == 0u) {
        g.counters.verify_skipped++;
        return;
    }
    if (bytes > s_verify_capacity) {
        free(s_verify_a);
        free(s_verify_b);
        s_verify_a = malloc(bytes);
        s_verify_b = malloc(bytes);
        s_verify_capacity = s_verify_a != NULL && s_verify_b != NULL ? bytes : 0u;
    }
    const uint8_t *rgba = NULL;
    uint32_t source_width = 0u, source_height = 0u;
    const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (bytes > s_verify_capacity || !gpu_window_read_capture(g.window, s_verify_a, s_verify_capacity, NULL, NULL) ||
        !live_vk_target_compose_last(g.targets, &rgba, &source_width, &source_height) ||
        !gpu_window_present_pixels(g.window, rgba, source_width, source_height, source_width * 4u, clear) ||
        !gpu_window_read_capture(g.window, s_verify_b, s_verify_capacity, NULL, NULL)) {
        g.counters.verify_skipped++;
        return;
    }
    unsigned long long differing = 0u;
    for (size_t i = 0u; i < bytes; i += 4u) {
        unsigned worst = 0u;
        for (size_t c = 0u; c < 3u; c++) {
            const unsigned a = s_verify_a[i + c], b = s_verify_b[i + c];
            const unsigned delta = a > b ? a - b : b - a;
            worst = delta > worst ? delta : worst;
        }
        if (worst != 0u) {
            differing++;
            if (worst > g.counters.verify_max_delta) {
                g.counters.verify_max_delta = worst;
            }
        }
        if ((s_verify_a[i] | s_verify_a[i + 1u] | s_verify_a[i + 2u]) != 0u) {
            g.counters.verify_nonblack_pixels++;
        }
        if (s_verify_a[i + 3u] != s_verify_b[i + 3u]) {
            g.counters.verify_alpha_differing++;
        }
    }
    g.counters.verify_samples++;
    g.counters.verify_pixels_compared += bytes / 4u;
    g.counters.verify_pixels_differing += differing;
    if (differing != 0u) {
        if (g.counters.verify_mismatch_frames++ == 0u) {
            g.counters.verify_first_mismatch_vblank = (long long)vblank;
        }
    }
}

static bool direct_present(void *context, gpu_window_blit_hook hook, void *hook_context)
{
    (void)context;
    return gpu_window_present_blit(g.window, hook, hook_context);
}

/* The CPU copy of the last frame for read_pixel and capture. The readback route keeps it as it presents; the blit route has none,
 * so it is composed on demand from the current target contents (the reference compositor). */
static bool last_frame_refresh(void)
{
    if (g.targets == NULL || !g.counters.blit_active) {
        return g.last_rgba != NULL && g.last_width != 0u;
    }
    const uint8_t *rgba = NULL;
    uint32_t width = 0u, height = 0u;
    if (!live_vk_target_compose_last(g.targets, &rgba, &width, &height)) {
        return false;
    }
    const size_t bytes = (size_t)width * height * 4u;
    if (bytes > g.last_capacity) {
        uint8_t *grown = realloc(g.last_rgba, bytes);
        if (grown == NULL) {
            return false;
        }
        g.last_rgba = grown;
        g.last_capacity = bytes;
    }
    memcpy(g.last_rgba, rgba, bytes);
    g.last_width = width;
    g.last_height = height;
    return true;
}

static bool op_open(void *context, void *sdl_window, char *error, size_t error_bytes)
{
    (void)context;
    const char *window_error = NULL;
    g.window = gpu_window_create((SDL_Window *)sdl_window, &window_error);
    if (g.window == NULL) {
        snprintf(error, error_bytes, "%s", window_error != NULL ? window_error : "gpu_window_create failed");
        return false;
    }
    gpu_window_native native;
    live_vk_target_device description;
    SDL_FunctionPointer sdl_gipa = SDL_Vulkan_GetVkGetInstanceProcAddr();
    PFN_vkGetInstanceProcAddr gipa = NULL;
    memcpy(&gipa, &sdl_gipa, sizeof gipa);
    const char *texture_error = NULL;
    live_texture_cache_init(&g.cache, g.config.inferred);
    if (!gpu_window_get_native(g.window, &native) || !live_vk_target_device_from_native(&native, gipa, &description)) {
        snprintf(error, error_bytes, "the window has no usable Vulkan device");
        gpu_window_destroy(g.window);
        g.window = NULL;
        return false;
    }
    g.targets = live_vk_target_create(&description, g.config.inferred ? LIVE_TARGET_INFER_BYTE_FORMATS : 0u, &g.cache);
    g.textures = live_vk_texture_create(&native, gipa, &texture_error);
    if (g.targets == NULL || g.textures == NULL) {
        snprintf(error, error_bytes, "targets or textures: %s", texture_error != NULL ? texture_error : "out of memory");
        live_vk_target_destroy(g.targets);
        live_vk_texture_destroy(g.textures);
        g.targets = NULL;
        g.textures = NULL;
        live_texture_cache_free(&g.cache);
        gpu_window_destroy(g.window);
        g.window = NULL;
        return false;
    }
    live_vk_target_set_present(g.targets, present_pixels, NULL);
    live_vk_target_bind_observer(g.targets);
    gpu_window_set_present_sync(g.config.present_sync);
    live_vk_draw_set_sync_each_run(g.config.present_sync); /* T1264 */
    g.counters.blit_requested = g.config.blit;
    g.counters.verify_first_mismatch_vblank = -1;
    if (g.config.blit && gpu_window_can_blit(g.window)) {
        if (g.config.blit_verify != 0u) {
            g.counters.verify_on = gpu_window_set_capture(g.window, true);
        }
        live_vk_target_set_direct(g.targets, direct_present, NULL);
        g.counters.blit_active = true;
    }
    snprintf(g.counters.device, sizeof g.counters.device, "%s", gpu_window_device_name(g.window));
    g.open_ok = true;
    return true;
}

static void op_picture(void *context, uint32_t width, uint32_t height, const uint8_t *rgb)
{
    (void)context;
    live_vk_target_overlay_submit(g.targets, width, height, rgb);
    /* Reset on actual scheduled picture release, not only guest runahead submission. */
    atomic_store(&g.frames_since_picture, 0u);
    atomic_store(&g.overlay_expiry_applied, false);
    g.pictures_seen++;
}

static void op_present_body(bool pictures_pending);
static void op_present(void *context, bool pictures_pending)
{
    (void)context;
    const uint64_t phase_start = gpu_phase_now();
    op_present_body(pictures_pending);
    gpu_phase_add(GPU_PHASE_VBLANK_PRESENT, phase_start);
}

static void op_present_body(bool pictures_pending)
{
    if (g.targets == NULL) {
        return;
    }
    (void)pictures_pending; /* the overlay owns the picture, the front shows in its bars */
    g.counters.presents++;
    uint64_t media_time_ns = 0u;
    const bool media_clock_valid = present_video_sink_job_media_time(g.sink, &media_time_ns);
    if (g.playback) {
        (void)live_vk_target_media_vblank(g.targets, d3d8_gpu_vblank_count(), media_clock_valid, media_time_ns, NULL);
        const bool fronts_waiting = live_vk_target_schedule(g.targets).count != 0u;
        if (atomic_load(&g.frames_since_picture) >= EXPIRY_FRAMES &&
            (g.job != NULL ? g.job->pictures_pending : present_video_sink_job_pictures_pending(g.sink)) == 0u && !fronts_waiting &&
            !atomic_exchange(&g.overlay_expiry_applied, true)) {
            live_vk_target_overlay_clear(g.targets);
            g.counters.overlay_cleared++;
        }
    } else {
        const unsigned long long blits_before = live_vk_target_stats_get(g.targets).blit_frames;
        (void)live_vk_target_vblank(g.targets, d3d8_gpu_vblank_count(), NULL);
        if (g.counters.verify_on && live_vk_target_stats_get(g.targets).blit_frames != blits_before &&
            g.counters.presents % g.config.blit_verify == 0u) {
            verify_sample(d3d8_gpu_vblank_count());
        }
    }
}

static bool op_read_pixel(void *context, uint32_t x, uint32_t y, uint8_t rgb[3])
{
    (void)context;
    if (!last_frame_refresh() || g.last_rgba == NULL || x >= g.last_width || y >= g.last_height) {
        return false;
    }
    const uint8_t *pixel = g.last_rgba + ((size_t)y * g.last_width + x) * 4u;
    rgb[0] = pixel[0];
    rgb[1] = pixel[1];
    rgb[2] = pixel[2];
    return true;
}

static void put_le(uint8_t *out, uint32_t value, unsigned bytes)
{
    for (unsigned i = 0u; i < bytes; i++) {
        out[i] = (uint8_t)(value >> (8u * i));
    }
}

/* The last frame handed to the swapchain as a 24 bit BMP. */
static bool op_capture(void *context, const char *path)
{
    (void)context;
    if (!last_frame_refresh() || g.last_rgba == NULL || g.last_width == 0u) {
        return false;
    }
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    const uint32_t row_bytes = (g.last_width * 3u + 3u) & ~3u;
    uint8_t header[54] = {'B', 'M'};
    put_le(header + 2, 54u + row_bytes * g.last_height, 4u);
    put_le(header + 10, 54u, 4u);
    put_le(header + 14, 40u, 4u);
    put_le(header + 18, g.last_width, 4u);
    put_le(header + 22, g.last_height, 4u);
    put_le(header + 26, 1u, 2u);
    put_le(header + 28, 24u, 2u);
    put_le(header + 34, row_bytes * g.last_height, 4u);
    bool good = fwrite(header, 1u, sizeof header, file) == sizeof header;
    uint8_t *row = calloc(1u, row_bytes);
    good = good && row != NULL;
    for (uint32_t y = g.last_height; good && y > 0u; y--) {
        const uint8_t *source = g.last_rgba + (size_t)(y - 1u) * g.last_width * 4u;
        for (uint32_t x = 0u; x < g.last_width; x++) {
            row[x * 3u] = source[x * 4u + 2u];
            row[x * 3u + 1u] = source[x * 4u + 1u];
            row[x * 3u + 2u] = source[x * 4u];
        }
        good = fwrite(row, 1u, row_bytes, file) == row_bytes;
    }
    free(row);
    return fclose(file) == 0 && good;
}

static void destroy_frame(void)
{
    live_vk_frame_detach(g.frame);
    g.frame = NULL;
    live_vk_bind_destroy(g.bind);
    g.bind = NULL;
}

static void op_close(void *context)
{
    (void)context;
    destroy_frame();
    live_vk_target_bind_observer(NULL);
    live_vk_target_destroy(g.targets);
    live_vk_texture_destroy(g.textures);
    live_texture_cache_free(&g.cache);
    g.targets = NULL;
    g.textures = NULL;
    if (g.window != NULL) {
        gpu_window_destroy(g.window);
        g.window = NULL;
    }
}

present_live_ops live_render_ops(const live_render_config *config)
{
    memset(&g, 0, sizeof g);
    if (config != NULL) {
        g.config = *config;
    }
    const present_live_ops ops = {op_open, op_picture, op_present, op_read_pixel, op_capture, op_close, NULL};
    return ops;
}

static const gpu_pgraph *source_model(void *context)
{
    (void)context;
    return g.job != NULL ? g.job->model : g.model;
}

/* The Swap's SetTexture history in a pipelined job, the live one (guest blocked) otherwise. */
static bool binding_from(const live_package *package, size_t draw, uint32_t stage, live_texture_binding *out, const char **refusal)
{
    uint32_t header = 0u, format = 0u, size_word = 0u, data = 0u;
    const bool found = package != NULL
                           ? d3d8_swap_replay_live_bindings_lookup(package->bindings, draw, stage, &header, &format, &size_word, &data, refusal)
                           : d3d8_swap_replay_live_binding(draw, stage, &header, &format, &size_word, &data, refusal);
    if (!found) {
        return false;
    }
    out->header = header;
    out->format = format;
    out->size_word = size_word;
    out->data = data;
    return true;
}

static bool binding_of(void *context, size_t draw, uint32_t stage, live_texture_binding *out, const char **refusal)
{
    (void)context;
    return binding_from(g.job, draw, stage, out, refusal);
}

static bool binding_of_guest_thread(void *context, size_t draw, uint32_t stage, live_texture_binding *out, const char **refusal)
{
    return binding_from(context, draw, stage, out, refusal);
}

/* Guest memory for the texture lookups: the Swap's captured bytes in a pipelined job, guest memory itself otherwise. */
static bool texture_resolver(void *context, const live_texture_binding *binding, uint32_t bytes, uint32_t *address, uint64_t *identity,
                             const char **refusal)
{
    return g.job != NULL ? live_texture_inputs_resolver(g.job->inputs, binding, bytes, address, identity, refusal)
                         : d3d8_gpu_resolve_texture(context, binding, bytes, address, identity, refusal);
}

static bool texture_reader(void *context, uint32_t address, void *out, size_t bytes)
{
    return g.job != NULL ? live_texture_inputs_reader(g.job->inputs, address, out, bytes) : d3d8_gpu_read_virtual(context, address, out, bytes);
}

/* ---- T1247: translate a frame's missing modules together --------------------------------------------------------------------- */
/* A cold module directory made the Story run translate 37 modules one at a time on the thread that draws (about 107 ms each, frame gaps
 * of 1.2 s and more). The first module a frame misses is the cue: the presenter thread resolves the vertex program and the combiner of
 * every state snapshot of the frame with a backend whose maker only records what is missing, translates all of those at once
 * (live_module_maker_make_batch), and the draws then find their modules on disk. Nothing else changes: the same translator makes the
 * same files, and a module the scout did not predict is made on demand as before. */
#define SCOUT_MAX 64u
#define SCOUT_BYTES 2304u
#define SCOUT_PARALLEL 16u

typedef struct {
    live_module_request requests[SCOUT_MAX];
    uint8_t bytes[SCOUT_MAX][SCOUT_BYTES];
    size_t count;
} scout_list;

static scout_list s_scout;

static bool scout_record(void *context, bool fragment, const char *name, const uint8_t *bytes, size_t byte_count, char *error,
                         size_t error_bytes)
{
    scout_list *list = context;
    if (error != NULL && error_bytes != 0u) {
        snprintf(error, error_bytes, "scouting only");
    }
    if (list->count == SCOUT_MAX || byte_count > SCOUT_BYTES || strlen(name) >= sizeof list->requests[0].name) {
        return false;
    }
    for (size_t i = 0u; i < list->count; i++) {
        if (strcmp(list->requests[i].name, name) == 0) {
            return false;
        }
    }
    live_module_request *request = &list->requests[list->count];
    request->fragment = fragment;
    snprintf(request->name, sizeof request->name, "%s", name);
    memcpy(list->bytes[list->count], bytes, byte_count);
    request->bytes = list->bytes[list->count];
    request->byte_count = byte_count;
    list->count++;
    return false;
}

bool live_render_make_module(void *maker, bool fragment, const char *name, const uint8_t *bytes, size_t byte_count, char *error,
                             size_t error_bytes)
{
    const gpu_pgraph *model = source_model(NULL);
    if (g.attached && !g.scouting && model != NULL) {
        g.scouting = true;
        g.scout_calls++;
        s_scout.count = 0u;
        (void)scout_record(&s_scout, fragment, name, bytes, byte_count, NULL, 0u);
        gpu_pgraph_backend scout_backend = g.backend;
        scout_backend.make_module = scout_record;
        scout_backend.make_module_context = &s_scout;
        for (size_t i = 0u; i < gpu_pgraph_snapshot_count(model); i++) {
            const gpu_pgraph_state *state = gpu_pgraph_snapshot(model, i);
            gpu_pgraph_program program;
            gpu_pgraph_fragment fragment_module;
            gpu_pgraph_report report;
            (void)gpu_pgraph_resolve_program(state, &scout_backend, &program, &report);
            (void)gpu_pgraph_resolve_fragment(state, &scout_backend, &fragment_module, &report);
        }
        if (s_scout.count > 1u) {
            g.scout_batches++;
            g.scout_modules += s_scout.count;
            (void)live_module_maker_make_batch(maker, s_scout.requests, s_scout.count, SCOUT_PARALLEL);
        }
        g.scouting = false;
    }
    return live_module_maker_make(maker, fragment, name, bytes, byte_count, error, error_bytes);
}

/* T1246: the visibility reports the queued frames still owe the guest, see live_report_pending.h. */
#define PENDING_REPORTS 4096u

static bool is_report_event(const gpu_pgraph_query *event)
{
    return event->report_address != 0u && event->method != 0x17CCu && event->method != 0x17C8u;
}

/* Guest thread, before the frame is posted. False when the table is full (the frame is drawn serially instead). */
static bool pending_add(const gpu_pgraph *model, unsigned package)
{
    for (size_t i = 0u; i < gpu_pgraph_query_count(model); i++) {
        const gpu_pgraph_query *event = gpu_pgraph_query_at(model, i);
        if (is_report_event(event) && !live_report_pending_add(g.pending, event->report_address, package)) {
            live_report_pending_done(g.pending, 0u, package);
            return false;
        }
    }
    return true;
}

static bool query_report_write(const gpu_pgraph_query *event, uint32_t samples);

static bool query_report(void *context, const gpu_pgraph_query *event, uint32_t samples)
{
    (void)context;
    const unsigned package = g.job != NULL ? (unsigned)(g.job - g.packages) : 0u;
    const bool written = query_report_write(event, samples);
    if (g.job != NULL) {
        live_report_pending_done(g.pending, event->report_address, package);
    }
    return written;
}

static bool query_report_write(const gpu_pgraph_query *event, uint32_t samples)
{
    /* FABRICATED timestamp: unified modeled CPU clock, not xemu's FIXME constant or GPU measurement. */
    if (!d3d8_visibility_complete_identity(event->data & 0xFFFFFFu,event->report_address,event->report_generation,samples,g.job != NULL ? g.job->clock : kernel_clock_peek())) {
        g.counters.query_reports_refused++;
        return false;
    }
    g.counters.query_reports++;
    if (g.frame_hash != NULL) {
        fprintf(g.frame_hash, "Q %u %u %u\n", (unsigned)event->data, (unsigned)samples, (unsigned)(g.job != NULL ? g.job->clock : kernel_clock_peek()));
    }
    if (samples != 0u) g.counters.query_nonzero_reports++;
    return true;
}

/* T1247: the VkPipelineCache file, one per Vulkan device name (the driver also validates the blob against its own uuid and version). */
static void open_pipeline_cache(void)
{
    char key[128];
    size_t used = 0u;
    for (const char *c = g.counters.device; *c != '\0' && used + 1u < sizeof key; c++) {
        const bool plain = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '.' || *c == '-';
        key[used++] = plain ? *c : '_';
    }
    key[used] = '\0';
    char directory[400];
    snprintf(directory, sizeof directory, "%s/pipeline-cache", g.config.pipeline_cache_dir);
    (void)mkdir(directory, 0700); /* a module directory scan never sees the blob */
    snprintf(g.counters.pipeline_cache_file, sizeof g.counters.pipeline_cache_file, "%s/%s.bin", directory, key);
    if (!live_vk_renderer_pipeline_cache_open(live_vk_frame_renderer(g.frame), g.counters.pipeline_cache_file)) {
        g.counters.pipeline_cache_file[0] = '\0';
    }
    g.pipeline_cache_saved_ticks = SDL_GetTicks();
}

static void job_attach(void *context)
{
    (void)context;
    g.attach_ok = false;
    if (!g.open_ok) {
        snprintf(g.attach_error, sizeof g.attach_error, "the Vulkan window was not opened");
        return;
    }
    if (!d3d8_swap_replay_live_backend(&g.backend)) {
        snprintf(g.attach_error, sizeof g.attach_error, "the swap replay is not enabled");
        return;
    }
    g.backend.output_groups |= GPU_PGRAPH_OUTPUT_SURFACE;
    if (g.config.inferred) {
      g.backend.live_raster_modules = true;
      g.backend.live_fog_modules = true;
      g.backend.live_swizzled_targets = true;
      g.backend.live_texture_modes = true; /* T1490 */
      g.backend.allowed_inferences |=
          GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING;
    }
    const live_vk_bind_source source = {binding_of, NULL, d3d8_gpu_read_guest, NULL};
    g.bind = live_vk_bind_create(&g.cache, g.textures, &source);
    if (g.bind == NULL) {
        snprintf(g.attach_error, sizeof g.attach_error, "out of memory for the texture bridge");
        return;
    }
    live_vk_bind_set_resolver(g.bind, texture_resolver, NULL, texture_reader, NULL);
    live_vk_bind_use_targets(g.bind, g.targets);
    const live_vk_frame_source frame_source = {source_model, NULL};
    const live_vk_frame_config frame_config = {g.bind, g.targets};
    g.frame = live_vk_frame_attach(g.window, &g.backend, &frame_source, &frame_config, g.attach_error, sizeof g.attach_error);
    if (g.frame == NULL || !live_vk_frame_enable_target_draws(g.frame, g.attach_error, sizeof g.attach_error)) {
        destroy_frame();
        return;
    }
    if (g.config.inferred && !live_vk_frame_enable_visibility(g.frame,query_report,NULL,g.attach_error,sizeof g.attach_error)) {
        destroy_frame();
        return;
    }
    if (g.config.pipeline_cache_dir != NULL) {
        open_pipeline_cache();
    }
    g.attach_ok = true;
}

/* T1246 equivalence evidence (--live-frame-hash): a digest of everything a frame job reads, taken at the start of the job from the
 * same accessors the job uses, so a serial job digests the live model, SetTexture history and guest memory while a pipelined job
 * digests its clone and captured inputs. Two runs whose jobs have equal digests must present equal frames: a divergence that
 * starts at a job whose digest differs is the guest's, one that starts earlier is the renderer's. FNV-1a 64 over named fields. */
typedef struct {
    uint64_t value;
} digest;

static void digest_bytes(digest *state, const void *data, size_t bytes)
{
    const uint8_t *cursor = data;
    for (size_t i = 0u; i < bytes; i++) {
        state->value = (state->value ^ cursor[i]) * 0x100000001B3ull;
    }
}

static void digest_u64(digest *state, uint64_t value)
{
    digest_bytes(state, &value, sizeof value);
}

static void digest_texture(digest *bindings, digest *identities, digest *texels, size_t draw, uint32_t stage, const gpu_pgraph_state *draw_state)
{
    live_texture_binding binding;
    const char *why = NULL;
    memset(&binding, 0, sizeof binding);
    if (!binding_of(NULL, draw, stage, &binding, &why)) {
        digest_u64(bindings, 0xB0u + stage);
        return;
    }
    if (((binding.format >> 16) & 15u) > 1u) {
        const uint32_t control_index = GPU_PGRAPH_OUT_TEXTURE_CONTROL0 + stage;
        binding.mip_limit = ((draw_state->output[control_index] & 0x0003FFC0u) >> 6) + 1u;
    }
    /* guest addresses (header, Data, the resolved address) vary run to run with the guest heap layout, the texels they hold do not */
    digest_u64(bindings, binding.format);
    digest_u64(bindings, binding.size_word);
    digest_u64(bindings, binding.mip_limit);
    live_texture_plan plan;
    if (!live_texture_plan_binding(&binding, g.cache.allow_inferred, &plan) || plan.source_bytes == 0u ||
        live_texture_inputs_names_target(&g.cache, &binding, plan.source_bytes)) {
        return;
    }
    uint32_t address = binding.data;
    uint64_t identity = 0u;
    const char *refusal = NULL;
    if (!texture_resolver(NULL, &binding, plan.source_bytes, &address, &identity, &refusal)) {
        digest_u64(identities, 0xDEADu);
        return;
    }
    digest_u64(identities, identity != 0u);
    uint8_t *bytes = malloc(plan.source_bytes);
    if (bytes != NULL && texture_reader(NULL, address, bytes, plan.source_bytes)) {
        digest_bytes(texels, bytes, plan.source_bytes);
    } else {
        digest_u64(texels, 0xBADu);
    }
    free(bytes);
}

#define DIGEST_PARTS 8u
static const char *const digest_names[DIGEST_PARTS] = {"states", "clears", "copies", "queries", "draws", "bindings", "identities", "texels"};

/* The digest of one frame job as DIGEST_PARTS component digests (so a divergence names the part that differs). */
static void frame_digest(const gpu_pgraph *model, uint64_t out[DIGEST_PARTS])
{
    digest parts[DIGEST_PARTS];
    for (size_t i = 0u; i < DIGEST_PARTS; i++) parts[i].value = 0xCBF29CE484222325ull;
    digest *states = &parts[0], *clears = &parts[1], *copies = &parts[2], *queries = &parts[3], *draws_digest = &parts[4];
    const size_t draws = gpu_pgraph_draw_count(model);
    digest_u64(states, gpu_pgraph_snapshot_count(model));
    for (size_t i = 0u; i < gpu_pgraph_snapshot_count(model); i++) {
        digest_bytes(states, gpu_pgraph_snapshot(model, i), sizeof(gpu_pgraph_state));
    }
    digest_u64(clears, gpu_pgraph_clear_count(model));
    for (size_t i = 0u; i < gpu_pgraph_clear_count(model); i++) {
        const gpu_pgraph_clear *clear = gpu_pgraph_clear_at(model, i);
        const uint32_t words[] = {clear->before_draw, clear->command, clear->flags, clear->zstencil, clear->color, clear->rect_horizontal,
                                  clear->rect_vertical, clear->surface_format, clear->surface_pitch, clear->surface_color_offset,
                                  (uint32_t)clear->zstencil_written | (uint32_t)clear->color_written << 1 | (uint32_t)clear->surface_written << 2};
        digest_bytes(clears, words, sizeof words);
    }
    digest_u64(copies, gpu_pgraph_copy_count(model));
    for (size_t i = 0u; i < gpu_pgraph_copy_count(model); i++) {
        const gpu_pgraph_copy *copy = gpu_pgraph_copy_at(model, i);
        const uint32_t words[] = {copy->before_draw, copy->before_clear, copy->command, copy->source_offset, copy->destination_offset,
                                  copy->color_format, copy->source_pitch, copy->destination_pitch, copy->in_x, copy->in_y, copy->out_x,
                                  copy->out_y, copy->width, copy->height, copy->operation};
        digest_bytes(copies, words, sizeof words);
    }
    digest_u64(queries, gpu_pgraph_query_count(model));
    for (size_t i = 0u; i < gpu_pgraph_query_count(model); i++) {
        const gpu_pgraph_query *query = gpu_pgraph_query_at(model, i);
        digest_u64(queries, query->method);
        digest_u64(queries, query->data);
        digest_u64(queries, query->before_draw);
        digest_u64(queries, query->command);
        digest_u64(queries, query->report_address != 0u); /* the address varies with the guest heap */
    }
    digest_u64(draws_digest, draws);
    const uint32_t *indices = gpu_pgraph_indices(model);
    for (size_t i = 0u; i < draws; i++) {
        const gpu_pgraph_draw *draw = gpu_pgraph_draw_at(model, i);
        digest_u64(draws_digest, draw->primitive);
        digest_u64(draws_digest, draw->first_command);
        digest_u64(draws_digest, draw->first_index);
        digest_u64(draws_digest, draw->index_count);
        digest_u64(draws_digest, draw->snapshot);
        digest_u64(draws_digest, draw->vertices_captured);
        digest_u64(draws_digest, draw->inline_vertices);
        if (indices != NULL && draw->index_count != 0u) {
            digest_bytes(draws_digest, indices + draw->first_index, (size_t)draw->index_count * sizeof *indices);
        }
        for (uint32_t slot = 0u; slot < GPU_PGRAPH_ATTRIBUTES; slot++) {
            uint32_t address = 0u, length = 0u;
            const uint8_t *vertex = gpu_pgraph_draw_vertex_bytes(model, i, slot, &address, &length);
            if (vertex != NULL) {
                digest_u64(draws_digest, length);
                digest_bytes(draws_digest, vertex, length);
            }
        }
        const gpu_pgraph_state *draw_state = gpu_pgraph_snapshot(model, draw->snapshot);
        for (uint32_t stage = 0u; stage < GPU_COMBINER_TEXTURE_STAGES && draw_state != NULL; stage++) {
            digest_texture(&parts[5], &parts[6], &parts[7], i, stage, draw_state);
        }
    }
    for (size_t i = 0u; i < DIGEST_PARTS; i++) out[i] = parts[i].value;
}

/* Overlay expiry and one frame of the loop, presenter thread, the guest is blocked. */
static void job_frame_body(void *context);
/* T1255: the frame job time per window of FRAME_SERIES_FRAMES frames with the draws of those frames, so a section of a run (a level
 * intro movie, a heavy room) is found in the stop report by its draws per frame and its job time. Presenter thread only. */
#define FRAME_SERIES_FRAMES 128u
#define FRAME_SERIES_WINDOWS 160u
static struct {
    uint32_t frames;
    uint64_t draws, ns, worst_ns, most_draws;
} g_frame_series[FRAME_SERIES_WINDOWS];
static uint64_t g_frame_series_index, g_input_serial;

static void job_frame(void *context)
{
    const gpu_pgraph *model = source_model(NULL);
    const uint64_t draws = model != NULL ? gpu_pgraph_draw_count(model) : 0u;
    const uint64_t phase_start = gpu_phase_now();
    g.cache.input_serial = ++g_input_serial; /* the frame's captured bytes do not change while the job runs (T1255) */
    job_frame_body(context);
    g.cache.input_serial = 0u;
    gpu_phase_add(GPU_PHASE_FRAME_JOB, phase_start);
    const uint64_t spent = gpu_phase_now() - phase_start;
    uint64_t window = g_frame_series_index++ / FRAME_SERIES_FRAMES;
    if (window >= FRAME_SERIES_WINDOWS) {
        window = FRAME_SERIES_WINDOWS - 1u;
    }
    g_frame_series[window].frames++;
    g_frame_series[window].draws += draws;
    g_frame_series[window].ns += spent;
    if (spent > g_frame_series[window].worst_ns) g_frame_series[window].worst_ns = spent;
    if (draws > g_frame_series[window].most_draws) g_frame_series[window].most_draws = draws;
}

static void job_frame_body(void *context)
{
    (void)context;
    if (g.frame_hash != NULL) {
        uint64_t parts[DIGEST_PARTS];
        frame_digest(source_model(NULL), parts);
        fprintf(g.frame_hash, "J %llu", g.jobs_digested++);
        for (size_t i = 0u; i < DIGEST_PARTS; i++) {
            fprintf(g.frame_hash, " %s=%016llx", digest_names[i], (unsigned long long)parts[i]);
        }
        fputc('\n', g.frame_hash);
    }
    const size_t draws = gpu_pgraph_draw_count(source_model(NULL));
    g.counters.title_frames++;
    if (draws != 0u) {
        g.counters.title_frames_with_draws++;
        const uint32_t since = atomic_fetch_add(&g.frames_since_picture, 1u) + 1u;
        if (since >= EXPIRY_FRAMES && !g.playback && present_video_sink_job_pictures_pending(g.sink) == 0u &&
            !atomic_exchange(&g.overlay_expiry_applied, true)) {
            live_vk_target_overlay_clear(g.targets);
            g.counters.overlay_cleared++;
        }
    }
    if (!live_vk_frame_run_targets(g.frame)) {
        /* T1339: a device that could not record or submit must be loud, it used to be a counter read at the stop only */
        if (g.counters.frames_failed++ % 600u == 0u) {
            fprintf(stderr, "live renderer  LOOP FAILURE (T1339): title frame %llu could not be recorded or submitted (failure %llu), the window may be "
                            "showing a stale or empty picture while the guest runs on\n",
                    g.counters.title_frames, g.counters.frames_failed);
        }
    }
    if (g.counters.pipeline_cache_file[0] != '\0' && SDL_GetTicks() - g.pipeline_cache_saved_ticks > 15000u) {
        (void)live_vk_renderer_pipeline_cache_save(live_vk_frame_renderer(g.frame)); /* new pipelines since the last write, at most every 15 s */
        g.pipeline_cache_saved_ticks = SDL_GetTicks();
    }
}

/* T1246: the presenter thread draws a package while the guest thread runs on. */
static void job_frame_pipelined(void *context)
{
    g.job = context;
    job_frame(NULL);
    live_report_pending_done(g.pending, 0u, (unsigned)(g.job - g.packages));
    g.job = NULL;
}

/* Guest thread, at the Swap: copy what the frame reads from the decoder and the guest, then queue it. False: draw it serially. */
static bool pipeline_frame_capture(const gpu_pgraph *model);

static bool pipeline_frame(const gpu_pgraph *model)
{
    gft_enter(GFT_SWAP_CAPTURE, 0u);
    const bool done = pipeline_frame_capture(model);
    gft_leave();
    return done;
}

static bool pipeline_frame_capture(const gpu_pgraph *model)
{
    live_package *package = &g.packages[g.next_package];
    if (!gpu_pgraph_frame_self_contained(model)) {
        g.serial_not_self_contained++;
        return false;
    }
    if (!gpu_pgraph_clone_frame(package->model, model)) {
        g.serial_clone_failed++;
        return false;
    }
    d3d8_swap_replay_live_bindings_capture(package->bindings);
    live_texture_inputs_reset(package->inputs);
    if (!live_vk_bind_capture_inputs(g.bind, model, binding_of_guest_thread, package, package->inputs, d3d8_gpu_resolve_texture, NULL,
                                     d3d8_gpu_read_virtual, NULL)) {
        g.serial_capture_full++;
        return false;
    }
    if (!pending_add(model, (unsigned)(package - g.packages))) {
        g.serial_capture_full++;
        return false;
    }
    package->clock = kernel_clock_peek();
    package->pictures_pending = present_video_sink_pictures_pending(g.sink);
    const size_t draws = gpu_pgraph_draw_count(model);
    if (draws > g.max_frame_draws) g.max_frame_draws = draws;
    if (live_texture_inputs_entries(package->inputs) > g.max_inputs_entries) g.max_inputs_entries = live_texture_inputs_entries(package->inputs);
    if (live_texture_inputs_bytes(package->inputs) > g.max_inputs_bytes) g.max_inputs_bytes = live_texture_inputs_bytes(package->inputs);
    if (!present_video_sink_post(g.sink, job_frame_pipelined, package)) {
        live_report_pending_done(g.pending, 0u, (unsigned)(package - g.packages));
        g.serial_post_failed++;
        return false;
    }
    g.next_package ^= 1u;
    g.pipelined_frames++;
    return true;
}

static void model_hook(const gpu_pgraph *model, uint64_t frame, void *context)
{
    (void)frame;
    (void)context;
    guest_frame_trace_draws(gpu_pgraph_draw_count(model));
    if (g.pipeline != 0u && pipeline_frame(model)) {
        return;
    }
    g.model = model;
    (void)present_video_sink_run_kind(g.sink, PRESENT_JOB_SERIAL_FRAME, job_frame, NULL);
    g.model = NULL;
    g.serial_frames++;
}

static void job_register(void *context)
{
    (void)context;
    const uint32_t header = g.header;
    const uint32_t data = d3d8_guest_load32(header + D3D8_SURFACE_DATA) & 0x0FFFFFFFu;
    live_target_refusal refusal;
    const live_vk_status status = live_vk_target_register(g.targets, data, d3d8_guest_load32(header + D3D8_SURFACE_FORMAT),
                                                          d3d8_guest_load32(header + D3D8_SURFACE_SIZE), &refusal);
    if (status == LIVE_VK_OK) {
        g.counters.targets_registered++;
    } else {
        g.counters.targets_refused++;
    }
}

static void target_hook(uint32_t header, void *context)
{
    (void)context;
    if (header == 0u) {
        return;
    }
    const known_target now = {d3d8_guest_load32(header + D3D8_SURFACE_DATA) & 0x0FFFFFFFu, d3d8_guest_load32(header + D3D8_SURFACE_FORMAT),
                              d3d8_guest_load32(header + D3D8_SURFACE_SIZE)};
    for (size_t i = 0u; i < g.known_count; i++) {
        if (memcmp(&g.known[i], &now, sizeof now) == 0) {
            return;
        }
    }
    if (g.known_count < KNOWN_TARGETS) {
        g.known[g.known_count++] = now;
    }
    g.header = header;
    (void)present_video_sink_run_kind(g.sink, PRESENT_JOB_REGISTER, job_register, NULL);
}

static void job_observer(void *context)
{
    (void)context;
    g.counters.observer_calls++;
    if (g.record_media_timed) {
        (void)live_vk_target_present_submit_at(g.targets, &g.record, g.record_media_time_ns);
    } else {
        (void)live_vk_target_present_submit(g.targets, &g.record);
    }
}

static void present_observer(const d3d8_frame_record *record)
{
    g.record = *record;
    g.record_media_timed = g.playback;
    if (g.record_media_timed) {
        const uint64_t ticks = kernel_clock_peek();
        g.record_media_time_ns = (ticks / KERNEL_CLOCK_FREQUENCY_HZ) * 1000000000ull +
                                 (ticks % KERNEL_CLOCK_FREQUENCY_HZ) * 1000000000ull / KERNEL_CLOCK_FREQUENCY_HZ;
    }
    (void)present_video_sink_run_kind(g.sink, PRESENT_JOB_OBSERVER, job_observer, NULL);
}

/* A report slot is about to be read or rewritten by the guest: the frames queued behind the guest must have completed their reports. */
static void drain_pipeline(uint32_t slot_address)
{
    const uint64_t spent = live_report_pending_wait(g.pending, slot_address);
    g.slot_checks++;
    if (spent != 0u) {
        g.slot_waits++;
        g.slot_wait_ns += spent;
        if (spent > g.slot_wait_max_ns) g.slot_wait_max_ns = spent;
    }
}

static void free_packages(void)
{
    live_report_pending_destroy(g.pending);
    g.pending = NULL;
    for (size_t i = 0u; i < 2u; i++) {
        gpu_pgraph_destroy(g.packages[i].model);
        d3d8_swap_replay_live_bindings_destroy(g.packages[i].bindings);
        live_texture_inputs_destroy(g.packages[i].inputs);
        memset(&g.packages[i], 0, sizeof g.packages[i]);
    }
}

bool live_render_attach(present_video_sink *sink, char *error, size_t error_bytes)
{
    g.sink = sink;
    g.playback = present_video_sink_playback_enabled(sink) && !present_video_sink_interactive(sink);
    if (sink == NULL || !present_video_sink_run(sink, job_attach, NULL) || !g.attach_ok) {
        snprintf(error, error_bytes, "%s", g.attach_error[0] != '\0' ? g.attach_error : "the presenter thread is not running");
        return false;
    }
    if (g.config.frame_hash_path != NULL && g.frame_hash == NULL) {
        g.frame_hash = fopen(g.config.frame_hash_path, "w");
        if (g.frame_hash == NULL) {
            snprintf(error, error_bytes, "--live-frame-hash: cannot write %s", g.config.frame_hash_path);
            return false;
        }
    }
    g.pipeline = g.config.pipeline;
    if (g.pipeline != 0u) {
        for (size_t i = 0u; i < 2u; i++) {
            g.packages[i].model = gpu_pgraph_create();
            g.packages[i].bindings = d3d8_swap_replay_live_bindings_create();
            g.packages[i].inputs = live_texture_inputs_create(INPUT_BUDGET_BYTES);
            if (g.packages[i].model == NULL || g.packages[i].bindings == NULL || g.packages[i].inputs == NULL) {
                snprintf(error, error_bytes, "out of memory for the pipelined frame packages");
                free_packages();
                g.pipeline = 0u;
                return false;
            }
        }
        g.pending = live_report_pending_create(PENDING_REPORTS);
        if (g.pending == NULL) {
            snprintf(error, error_bytes, "out of memory for the pipelined report table");
            free_packages();
            g.pipeline = 0u;
            return false;
        }
        d3d8_visibility_set_drain_hook(drain_pipeline);
    }
    g.attached = true;
    d3d8_swap_replay_set_live_target_hook(target_hook, NULL);
    d3d8_swap_replay_set_live_hook(model_hook, NULL);
    d3d8_present_set_second_observer(present_observer);
    return true;
}

void live_render_note_picture(void)
{
    atomic_store(&g.frames_since_picture, 0u);
    atomic_store(&g.overlay_expiry_applied, false);
    atomic_fetch_add(&g.pictures, 1u);
}

static live_vk_frame_clear_reason s_clear_reasons[LIVE_VK_FRAME_CLEAR_REASONS];
static size_t s_clear_reason_count;
static uint64_t s_clear_reason_other;

static void job_counters(void *context)
{
    (void)context;
    if (g.frame != NULL) {
        s_clear_reason_count = live_vk_frame_clear_reason_count(g.frame, &s_clear_reason_other);
        for (size_t i = 0u; i < s_clear_reason_count; i++) {
            s_clear_reasons[i] = *live_vk_frame_clear_reason_at(g.frame, i);
        }
        const live_vk_frame_stats stats = live_vk_frame_get_stats(g.frame);
        g.counters.frames_refused = stats.frames_refused;
        g.counters.draws_offered = stats.draws_offered;
        g.counters.draws_refused = stats.draws_refused;
        g.counters.clears_offered = stats.clears_offered;
        g.counters.clears_refused = stats.clears_refused;
        g.counters.copies_applied = stats.copies_applied;
        g.counters.copies_refused = stats.copies_refused;
        g.counters.draws_drawn = live_vk_renderer_stats(live_vk_frame_renderer(g.frame)).drawn;
        const live_vk_stats renderer_stats = live_vk_renderer_stats(live_vk_frame_renderer(g.frame));
        g.counters.arena_peak = renderer_stats.arena_peak;
        g.counters.arena_capacity = renderer_stats.arena_capacity;
        g.counters.arena_blocks = renderer_stats.arena_blocks;
        g.counters.frames_with_refusals = renderer_stats.frames_with_refusals;
        g.counters.first_refused_frame = renderer_stats.first_refused_frame;
        g.counters.most_refused_in_frame = renderer_stats.most_refused_in_frame;
        g.counters.pipelines_created = renderer_stats.pipelines_created;
        g.counters.pipeline_create_calls = renderer_stats.pipeline_create_calls;
        g.counters.pipeline_create_ms = (double)renderer_stats.pipeline_create_ns / 1e6;
        g.counters.pipeline_create_worst_ms = (double)renderer_stats.pipeline_create_worst_ns / 1e6;
        g.counters.pipeline_cache_loaded_bytes = renderer_stats.pipeline_cache_loaded_bytes;
        g.counters.pipeline_cache_saved_bytes = renderer_stats.pipeline_cache_saved_bytes;
        g.counters.pipeline_cache_saves = renderer_stats.pipeline_cache_saves;
        const live_vk_draw *draw = live_vk_frame_draw(g.frame);
        if (draw != NULL) {
            g.counters.target_runs = live_vk_draw_stats_get(draw).runs;
            g.counters.target_draws_refused = live_vk_draw_stats_get(draw).refused;
        }
    }
}

static struct {
    char text[LIVE_PIPELINE_REASON_BYTES];
    const char *stage;
    unsigned long long count;
} s_groups[REASON_GROUPS];
static size_t s_group_count;
static unsigned long long s_group_other;
static unsigned long long s_census_total, s_census_by_stage[LIVE_STAGE_COUNT];

static void census_snapshot(void)
{
    s_group_count = 0u;
    s_group_other = 0u;
    s_census_total = 0u;
    memset(s_census_by_stage, 0, sizeof s_census_by_stage);
    if (g.frame == NULL) {
        return;
    }
    const live_pipeline_census *census = live_vk_renderer_census(live_vk_frame_renderer(g.frame));
    s_census_total = census->count;
    for (size_t i = 0u; i < LIVE_STAGE_COUNT; i++) {
        s_census_by_stage[i] = census->by_stage[i];
    }
    for (size_t i = 0u; i < census->count; i++) {
        const live_pipeline_refusal *entry = &census->refusals[i];
        const char *stage = live_pipeline_stage_name(entry->stage);
        size_t group = 0u;
        for (; group < s_group_count; group++) {
            if (strcmp(s_groups[group].text, entry->reason) == 0 && strcmp(s_groups[group].stage, stage) == 0) {
                break;
            }
        }
        if (group == s_group_count) {
            if (s_group_count == REASON_GROUPS) {
                s_group_other++;
                continue;
            }
            snprintf(s_groups[group].text, sizeof s_groups[group].text, "%s", entry->reason);
            s_groups[group].stage = stage;
            s_groups[group].count = 0u;
            s_group_count++;
        }
        s_groups[group].count++;
    }
}

static void job_snapshot(void *context)
{
    (void)context;
    job_counters(NULL);
    g.counters.texture_input_misses = 0u;
    for (size_t i = 0u; i < 2u && g.packages[i].inputs != NULL; i++) {
        g.counters.texture_input_misses += live_texture_inputs_misses(g.packages[i].inputs);
    }
    if (g.targets != NULL) {
        const live_vk_target_stats stats = live_vk_target_stats_get(g.targets);
        const live_present_schedule schedule = live_vk_target_schedule(g.targets);
        g.counters.window_frames = stats.frames_presented;
        g.counters.black_frames = stats.black_frames;
        g.counters.overlay_composed = stats.overlay_composed;
        g.counters.present_failures = stats.present_callback_failures;
        const gpu_window_stats window_stats = gpu_window_get_stats(g.window);
        g.counters.window_acquire_failures = window_stats.acquire_failures;
        g.counters.window_present_failures = window_stats.present_failures;
        g.counters.swapchain_recreations = window_stats.swapchain_recreations;
        g.counters.blit_frames = stats.blit_frames;
        g.counters.readback_frames = stats.readback_frames;
        g.counters.blit_fallbacks = stats.direct_fallbacks;
        g.counters.blit_failures = stats.direct_failures;
        g.counters.vblanks = stats.vblanks;
        g.counters.front_events_submitted = stats.presents_submitted;
        g.counters.front_events_refused = stats.present_refused;
        g.counters.front_events_released = schedule.shown;
        g.counters.media_snapshots = stats.media_snapshots;
        g.counters.media_snapshot_drops = stats.media_snapshot_drops;
        g.counters.media_frames_released = stats.media_frames_released;
        g.counters.media_direct_fallbacks = stats.media_direct_fallbacks;
        g.counters.media_pending = live_vk_target_schedule(g.targets).count;
        g.counters.wall_seconds =
            stats.frames_presented > 1u ? (double)(stats.last_frame_ns - stats.first_frame_ns) / 1e9 : 0.0;
        g.counters.vblank_ms_mean = stats.vblanks != 0u ? (double)stats.vblank_ns / 1e6 / (double)stats.vblanks : 0.0;
        g.counters.vblank_ms_max = (double)stats.vblank_ns_max / 1e6;
    }
    census_snapshot();
}

static void job_destroy_frame(void *context)
{
    (void)context;
    destroy_frame();
}

void live_render_detach(void)
{
    if (!g.attached) {
        return;
    }
    d3d8_swap_replay_set_live_hook(NULL, NULL);
    d3d8_swap_replay_set_live_target_hook(NULL, NULL);
    d3d8_present_set_second_observer(NULL);
    d3d8_visibility_set_drain_hook(NULL);
    (void)present_video_sink_run(g.sink, job_snapshot, NULL); /* a synchronous job waits for the queued frame first */
    (void)present_video_sink_run(g.sink, job_destroy_frame, NULL);
    free_packages();
    g.attached = false;
}

live_render_report_data live_render_report_get(void)
{
    live_render_report_data data = g.counters;
    data.attached = g.attached;
    return data;
}

void live_render_report_print(FILE *out)
{
    if (g.sink == NULL) {
        return;
    }
    if (g.attached) {
        (void)present_video_sink_run(g.sink, job_snapshot, NULL);
    }
    const live_render_report_data d = g.counters;
    fprintf(out,
            "live renderer  OPT-IN, INFERRED until validated (T838): Vulkan device \"%s\", title frames %llu (with draws %llu, "
            "with refused draws or clears %llu, loop failures %llu (a device that could not record or submit)), frames presented to the swapchain %llu (black %llu, with the movie overlay %llu, overlay dropped %llu "
            "times, present failures %llu)\n",
            d.device, d.title_frames, d.title_frames_with_draws, d.frames_refused, d.frames_failed, d.window_frames, d.black_frames, d.overlay_composed,
            d.overlay_cleared, d.present_failures);
    fprintf(out,
            "live renderer  frame pipeline (T1246): depth %u (0 = serial), frames pipelined %llu, drawn serially %llu (frame not self contained "
            "%llu, clone failed %llu, input capture full %llu, post failed %llu), texture input misses %llu (a miss is a frame that differs from the "
            "serial one), most draws in a frame %llu, most captured texture inputs %llu entries %llu bytes\n",
            g.pipeline, g.pipelined_frames, g.serial_frames, g.serial_not_self_contained, g.serial_clone_failed, g.serial_capture_full,
            g.serial_post_failed, d.texture_input_misses, g.max_frame_draws, g.max_inputs_entries, g.max_inputs_bytes);
    {
        static const char *const kinds[3] = {"synchronous jobs (vblank, observer, serial frames)", "posts (a frame waiting for the previous one)",
                                             "drains (a guest report slot read or rewritten)"};
        for (unsigned kind = 0u; kind < 3u; kind++) {
            uint64_t calls = 0u;
            double total_ms = 0.0, max_ms = 0.0;
            present_video_sink_blocked(g.sink, kind, &calls, &total_ms, &max_ms);
            fprintf(out, "live renderer  guest thread blocked on the presenter (T1246), %s: %llu calls, %.1f ms total, %.3f ms worst\n", kinds[kind],
                    (unsigned long long)calls, total_ms, max_ms);
        }
    }
    {
        static const char *const job_names[PRESENT_JOB_KINDS] = {"other", "vblank", "observer", "serial frame", "target register"};
        for (unsigned kind = 0u; kind < PRESENT_JOB_KINDS; kind++) {
            present_job_kind_stats stats;
            present_video_sink_job_kind_stats(g.sink, (present_job_kind)kind, &stats);
            if (stats.calls == 0u) {
                continue;
            }
            fprintf(out,
                    "live renderer  synchronous job phases (T1262), %s: %llu calls, waited for the job slot (the frame in flight) %.1f ms total %.3f ms worst, "
                    "then for the job %.1f ms total %.3f ms worst\n",
                    job_names[kind], (unsigned long long)stats.calls, stats.wait_ms, stats.wait_max_ms, stats.exec_ms, stats.exec_max_ms);
        }
        fprintf(out,
                "live renderer  texture cache (T1255): %u entries, %llu hits, %llu decodes (a miss: the texture bytes converted to RGBA), %llu invalidations (the guest changed the bytes), %llu refusals, %llu byte compares skipped (the binding was already compared with this frame capture)\n",
                (unsigned)LIVE_TEXTURE_CACHE_ENTRIES, (unsigned long long)g.cache.hits, (unsigned long long)g.cache.decodes,
                (unsigned long long)g.cache.invalidations, (unsigned long long)g.cache.refusals,
                (unsigned long long)g.cache.compares_skipped);
        /* T1491: what the refusals above are, by category, Format, Size and Data (up to LIVE_TEXTURE_CENSUS_ENTRIES distinct, the rest counted) */
        for (size_t entry = 0u; g.cache.refusals != 0u; entry++) {
            char census[512];
            if (!live_texture_census_line(&g.cache, entry, census, sizeof census)) {
                break;
            }
            fprintf(out, "live renderer  texture cache census (T1491): %s\n", census);
        }
        if (g.cache.census_overflow != 0u) {
            fprintf(out, "live renderer  texture cache census (T1491): %llu more refusals of other bindings (the census holds %u distinct)\n",
                    (unsigned long long)g.cache.census_overflow, (unsigned)LIVE_TEXTURE_CENSUS_ENTRIES);
        }
        for (unsigned window = 0u; window < FRAME_SERIES_WINDOWS; window++) {
            if (g_frame_series[window].frames == 0u) {
                continue;
            }
            fprintf(out,
                    "live renderer  frame job series (T1255), frames %u to %u: %u frames, %.1f draws per frame (most %llu), frame job %.3f ms mean, %.3f ms worst\n",
                    window * FRAME_SERIES_FRAMES, window * FRAME_SERIES_FRAMES + g_frame_series[window].frames - 1u,
                    g_frame_series[window].frames, (double)g_frame_series[window].draws / g_frame_series[window].frames,
                    (unsigned long long)g_frame_series[window].most_draws,
                    (double)g_frame_series[window].ns / 1e6 / g_frame_series[window].frames,
                    (double)g_frame_series[window].worst_ns / 1e6);
        }
        for (unsigned phase = 0u; phase < GPU_PHASE_COUNT; phase++) {
            const uint64_t calls = atomic_load(&gpu_phase_global.calls[phase]);
            if (calls == 0u) {
                continue;
            }
            fprintf(out, "live renderer  presenter phase (T1262), %s: %llu calls, %.1f ms total, %.3f ms mean, %.3f ms worst\n",
                    gpu_phase_name((gpu_phase)phase), (unsigned long long)calls, (double)atomic_load(&gpu_phase_global.total_ns[phase]) / 1e6,
                    (double)atomic_load(&gpu_phase_global.total_ns[phase]) / 1e6 / (double)calls,
                    (double)atomic_load(&gpu_phase_global.max_ns[phase]) / 1e6);
        }
        fprintf(out, "live renderer  present sync (T1262): %s\n",
                g.config.present_sync ? "ON (--live-present-sync: vkQueueWaitIdle after every present, the old behaviour)"
                                      : "off (the present does not wait for the queue, the submit fence retires it)");
    }
    fprintf(out,
            "live renderer  report slot waits (T1246): the guest thread checked %llu slots, %llu still had a report owed by a queued frame, "
            "waited %.1f ms total, %.3f ms worst\n",
            g.slot_checks, g.slot_waits, (double)g.slot_wait_ns / 1e6, (double)g.slot_wait_max_ns / 1e6);
    fprintf(out,
            "live renderer  pipeline creation (T1247): %llu pipelines (%llu vkCreateGraphicsPipelines calls) took %.1f ms in total, the slowest %.1f ms; "
            "VkPipelineCache %s: loaded %llu bytes at start, written %llu times (last %llu bytes)\n",
            d.pipelines_created, d.pipeline_create_calls, d.pipeline_create_ms, d.pipeline_create_worst_ms,
            d.pipeline_cache_file[0] != '\0' ? d.pipeline_cache_file : "off", d.pipeline_cache_loaded_bytes, d.pipeline_cache_saves,
            d.pipeline_cache_saved_bytes);
    fprintf(out,
            "live renderer  module scouting (T1247): %llu misses scouted, %llu of them found other modules missing in the same frame "
            "(%llu modules translated together, in parallel)\n",
            g.scout_calls, g.scout_batches, g.scout_modules);
    fprintf(out,"live renderer  visibility (T998, INFERRED mapping, FABRICATED modeled-clock timestamp): reports %llu, nonzero %llu, refused guest writes %llu\n",
            d.query_reports,d.query_nonzero_reports,d.query_reports_refused);
    fprintf(out, "live renderer  present route (T849): %s, frames by the swapchain blit %llu, by the readback route %llu, blit "
                 "fallbacks to readback %llu (front the blit cannot read), blit failures %llu\n",
            d.blit_active ? "swapchain pre-pass blit (default for window play since T1267, --live-readback forces the readback route)"
                          : (d.blit_requested ? "readback (the blit route is wanted but the swapchain has no transfer usage: auto fallback)"
                                              : "readback (--live-readback, or --live-frame-hash which hashes the readback frames)"),
            d.blit_frames, d.readback_frames, d.blit_fallbacks, d.blit_failures);
    if (d.verify_on) {
        fprintf(out, "live renderer  blit verify (T1267): %llu sampled frames compared, %llu pixels, %llu differing (RGB), %llu frames with a difference, "
                     "max channel delta %u, non black pixels in the blit images %llu, alpha differing %llu (not compared: the swapchain composite alpha is OPAQUE), first mismatch vblank %lld, samples skipped %llu\n",
                d.verify_samples, d.verify_pixels_compared, d.verify_pixels_differing, d.verify_mismatch_frames, d.verify_max_delta,
                d.verify_nonblack_pixels, d.verify_alpha_differing, d.verify_first_mismatch_vblank, d.verify_skipped);
    }
    if (d.wall_seconds > 0.0) {
        fprintf(out,
                "live renderer  frame rate MEASURED ON THIS MACHINE (T849), Vulkan device \"%s\": %llu frames presented in %.3f s of "
                "wall time between the first and the last = %.2f frames per second (includes the sink's pacing and the swapchain "
                "wait), cost inside the vblank call %.3f ms mean, %.3f ms worst over %llu vblanks. A real GPU frame rate is NOT "
                "measured by this build: the owner's RX 9070 XT run is still to do\n",
                d.device, d.window_frames, d.wall_seconds, (double)(d.window_frames - 1u) / d.wall_seconds, d.vblank_ms_mean,
                d.vblank_ms_max, d.vblanks);
    } else {
        fprintf(out, "live renderer  frame rate (T849): fewer than two frames presented, nothing measured; no real GPU number\n");
    }
    fprintf(out,
            "live renderer  draws offered %llu, drawn %llu, refused %llu (target runs %llu, refused by the target provider %llu), clears "
            "offered %llu refused %llu, CopyRects applied %llu refused %llu, targets registered %llu refused %llu\n",
            d.draws_offered, d.draws_drawn, d.draws_refused, d.target_runs, d.target_draws_refused, d.clears_offered, d.clears_refused,
            d.copies_applied, d.copies_refused, d.targets_registered, d.targets_refused);
    fprintf(out,
            "live renderer  swapchain (T1339): acquire failures %llu, present failures %llu, swapchains recreated after out of date or "
            "suboptimal %llu (a nonzero failure count means the window stopped showing frames while the guest ran on)\n",
            d.window_acquire_failures, d.window_present_failures, d.swapchain_recreations);
    fprintf(out,
            "live renderer  draw arena (T1339): peak %llu bytes in one frame of %llu bytes allocated in %llu block(s) (grown on demand, a draw "
            "is never refused for capacity unless the device refuses a block); frames with a refused draw %llu (first refused frame %llu, 1 "
            "based, 0 = none), most refused draws in one frame %llu\n",
            d.arena_peak, d.arena_capacity, d.arena_blocks, d.frames_with_refusals, d.first_refused_frame, d.most_refused_in_frame);
    fprintf(out,
            "live renderer  Swap present origin: second-observer calls %llu, front events queued %llu, refused %llu, released %llu\n",
            d.observer_calls, d.front_events_submitted, d.front_events_refused, d.front_events_released);
    fprintf(out,
            "live renderer  playback fronts (T850, INFERRED): immutable snapshots %llu, released at audio media time %llu, "
            "pending %llu, queue drops %llu, direct-route fallback frames %llu\n",
            d.media_snapshots, d.media_frames_released, d.media_pending, d.media_snapshot_drops, d.media_direct_fallbacks);
    fprintf(out, "live renderer  refusal census: %llu refused draws (program %llu fragment %llu output %llu primitive %llu vertex %llu device %llu)\n",
            s_census_total, s_census_by_stage[LIVE_STAGE_PROGRAM], s_census_by_stage[LIVE_STAGE_FRAGMENT],
            s_census_by_stage[LIVE_STAGE_OUTPUT], s_census_by_stage[LIVE_STAGE_PRIMITIVE], s_census_by_stage[LIVE_STAGE_VERTEX],
            s_census_by_stage[LIVE_STAGE_DEVICE]);
    for (size_t i = 0u; i < s_group_count; i++) {
        fprintf(out, "live renderer  refused x%llu [%s] %s\n", s_groups[i].count, s_groups[i].stage, s_groups[i].text);
    }
    if (s_group_other != 0u) {
        fprintf(out, "live renderer  refused x%llu other distinct reasons\n", s_group_other);
    }
    /* T860: why each refused clear was refused */
    for (size_t i = 0u; i < s_clear_reason_count; i++) {
        fprintf(out, "live renderer  refused clear x%llu %s\n", (unsigned long long)s_clear_reasons[i].count, s_clear_reasons[i].text);
    }
    if (s_clear_reason_other != 0u) {
        fprintf(out, "live renderer  refused clear x%llu other distinct reasons\n", (unsigned long long)s_clear_reason_other);
    }
}

#else /* !TSFP_HAVE_SDL3 */

present_live_ops live_render_ops(const live_render_config *config)
{
    (void)config;
    present_live_ops none;
    memset(&none, 0, sizeof none);
    return none;
}
bool live_render_attach(present_video_sink *sink, char *error, size_t error_bytes)
{
    (void)sink;
    snprintf(error, error_bytes, "--gpu-live needs a build with SDL3");
    return false;
}
bool live_render_make_module(void *maker, bool fragment, const char *name, const uint8_t *bytes, size_t byte_count, char *error,
                             size_t error_bytes)
{
    return live_module_maker_make(maker, fragment, name, bytes, byte_count, error, error_bytes);
}
void live_render_note_picture(void) {}
void live_render_detach(void) {}
live_render_report_data live_render_report_get(void)
{
    live_render_report_data none;
    memset(&none, 0, sizeof none);
    return none;
}
void live_render_report_print(FILE *out)
{
    (void)out;
}

#endif
