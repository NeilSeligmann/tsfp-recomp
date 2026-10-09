/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Swap, replayed (T84a): d3d8_swap's second call runs the recorded stream through the pushbuffer
 * decoder and gpu_vsh_draw when, and only when, d3d8_swap_replay_enable was called. The device is
 * the one CreateDevice made (640x480), the commands go through the real ring and the instantaneous
 * GPU's recording, and the module is a real `.spv` file read from a directory by name. Synthetic
 * guest memory and a hand-written vertex shader, not original-XBE equivalence evidence.
 *
 * Everything that needs a Vulkan device runs on "hardware" and "software" and prints a stated SKIP for
 * a device that is absent. The checks that need none (default off, configuration refusals, the strict
 * refusal and its latch, dropped commands, the size measurement) always run. Exit 77 (a ctest SKIP)
 * when the loader is absent or no device ran.
 *
 * The scene (clip-space triangles, the program reads v1 as the position and v2.zyxw + c[3] as oD0):
 *   frame 1  draw 1, upper left triangle, v2 = (0, 0, 1, 1)           -> red
 *   frame 2  c[3] = (0, 1, 0, 0), draw 2, lower right triangle        -> yellow, and ONLY draw 2 (the
 *            draw list of frame 1 is gone while the program, arrays and constants persist)
 *   frame 3  nothing drawn                                            -> counted empty, no new image
 *
 * T478 (combiner option): the same red triangle through `config.combiner`. The module directory also holds two
 * `combiner_<sha256>.spv` modules (pass and multiply, the fixtures of test_gpu_combiner), the third fixture
 * (final) is deliberately absent so a configuration with no translated module is a refusal that names it.
 * T484: `dump_every` and `dump_last` choose which of the replayed frames are written.
 *
 * T84a1 (per-target frames): the same triangles with SetRenderTarget calls between them, into
 * cloned surface headers of other sizes (128x64, 96x48), so one image of the wrong size, or one
 * image for two targets, cannot pass.
 */
#define _DEFAULT_SOURCE
#include "test_d3d8_support.h"

#include "d3d8_bind.h"
#include "d3d8_copy.h"
#include "d3d8_gpu.h"
#include "d3d8_gpu_pgraph.h"
#include "d3d8_overlay.h"
#include "d3d8_surface_model.h"
#include "d3d8_overlay_key.h"
#include "d3d8_present.h"
#include "d3d8_surface.h"
#include "d3d8_swap_replay.h"
#include "gpu_combiner.h"
#include "gpu_device.h"
#include "gpu_pgraph_test_support.h"
#include "gpu_combiner_words.h"
#include "gpu_standin_words.h"
#include "gpu_pgraph_vertex_words.h"
#include "gpu_png.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#define ARENA 0x00D00000u
#define MEMORY_BASE (ARENA + 0x1000u)
#define HEADER_SCRATCH (ARENA + 0x8000u)
#define UNHANDLED_METHOD 0x0304u
#define SET_RENDER_TARGET_COMMANDS 40u /* recorded commands of one d3d8_set_render_target (T449, MEASURED) */
#define TARGET_A (HEADER_SCRATCH + 0x100u)
#define TARGET_B (HEADER_SCRATCH + 0x140u)
#define TARGET_C (HEADER_SCRATCH + 0x180u)
#define TARGET_D (HEADER_SCRATCH + 0x1C0u)
#define DEV_RENDER_TARGET 0x1A04u
#define DEV_DEPTH_SURFACE 0x1A08u

typedef struct {
    float position[3];
    float v2[4];
} float_vertex; /* 28 bytes */

static char spv_directory[256];
static char dump_directory[300];

/* --- the guest side: vertex memory and the ring ------------------------------------------ */

static void write_vertices(void)
{
    static const float corners[6][2] = {
        {-0.9f, -0.9f}, {-0.1f, -0.9f}, {-0.5f, -0.1f}, /* draw 1 */
        {0.1f, 0.1f},   {0.9f, 0.1f},   {0.5f, 0.9f},   /* draw 2 */
    };
    for (uint32_t i = 0u; i < 6u; i++) {
        const float_vertex vertex = {{corners[i][0], corners[i][1], 0.5f}, {0.0f, 0.0f, 1.0f, 1.0f}};
        uint32_t words[7];
        memcpy(words, &vertex, sizeof words);
        for (uint32_t word = 0u; word < 7u; word++) {
            store(MEMORY_BASE + i * (uint32_t)sizeof vertex + word * 4u, words[word]);
        }
    }
}

/* One header and one value per pair, as the library's own single-method writes are. */
static void push(const stream_builder *stream)
{
    for (size_t done = 0u; done < stream->count; done += 16u) {
        const size_t chunk = stream->count - done < 16u ? stream->count - done : 16u;
        const uint32_t cursor = d3d8_pushbuffer_begin();
        for (size_t i = 0u; i < chunk; i++) {
            store(cursor + (uint32_t)i * 8u, 0x00040000u | stream->pairs[done + i].method);
            store(cursor + (uint32_t)i * 8u + 4u, stream->pairs[done + i].data);
        }
        d3d8_pushbuffer_end(cursor + (uint32_t)chunk * 8u);
    }
    d3d8_gpu_kick();
}

static void build_frame_one(stream_builder *stream)
{
    static const float offset[4] = {320.0f, 240.0f, 0.0f, 0.0f};
    static const float scale[4] = {320.0f, -240.0f, 1.0f, 0.0f};
    static const float c3_zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    stream_viewport(stream, offset, scale);
    stream_pair(stream, GPU_PGRAPH_EXECUTION_MODE, 6u);
    stream_program(stream, 0u, synthetic_program, 1u);
    stream_pair(stream, GPU_PGRAPH_PROGRAM_START, 0u);
    stream_constants(stream, 3u, c3_zero, 4u);
    stream_array(stream, 1u, MEMORY_BASE, array_format(28u, 3u, GPU_PGRAPH_TYPE_F));
    stream_array(stream, 2u, MEMORY_BASE + 12u, array_format(28u, 4u, GPU_PGRAPH_TYPE_F));
    stream_draw_arrays(stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
}

static void build_frame_two(stream_builder *stream)
{
    static const float c3_green[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    stream_constants(stream, 3u, c3_green, 4u);
    stream_draw_arrays(stream, GPU_PGRAPH_OP_TRIANGLES, 3u, 3u);
}

/* The real thing, library commands included. */
static void present_through_swap(void)
{
    (void)d3d8_swap(2u);
    (void)d3d8_swap(4u);
}

/* The frame number the replay logs: Swap counts it in its queue, and present() below does not call record_frame. */
static uint64_t presents_driven;

/* d3d8_set_render_target writes its measured stream (T449: SetRenderTarget 0x003D3800 with the 0x003D7B80 and
 * SetViewport 0x003D3E40 helpers, 40 recorded commands: NOP, WAIT_FOR_IDLE, surface pitch, colour and zeta offset,
 * clip, format, depth/stencil enable, scissor words, viewport, depth range, none of which the decoder interprets
 * in its default configuration), and Swap calls it twice, once to bind the front buffer for the copy and once to
 * restore the title's target. This suite tests the REPLAY of synthetic frames, so present() keeps what those calls
 * do to the model (the hook notes both switches) and rewinds the ring cursor over the library's own commands
 * before they are kicked. test_swap_own_emission_reaches_the_replay keeps the real Swap in the loop and pins what
 * the replay does with them. */
static void drop_unkicked_emission(void)
{
    d3d8_device_store32(D3D8_DEV_CURSOR, d3d8_pushbuffer_put());
}

/* Swap(2) then Swap(4) without the library's own render target commands: Swap(2) as is (its fence is kicked
 * BEFORE the bind), then Swap(4)'s restore, its finish (fence, kick), the vblank and the replay's present hook. */
static void present(void)
{
    (void)d3d8_swap(2u);
    drop_unkicked_emission();
    d3d8_present_restore_state();
    drop_unkicked_emission();
    d3d8_present_finish();
    d3d8_gpu_wait_vblank();
    d3d8_swap_replay_on_present(++presents_driven);
}

/* CreateDevice (640x480, copy swap), then forget its own recording and the frame queue. */
static void begin_device(bool discard_boot_commands)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    seed_recompute_constants();
    map_fixed(ARENA, 0x10000u);
    (void)d3d8_device_register();
    write_title_parameters(SCRATCH_DATA, 0x140u);
    const uint32_t args[6] = {0u, 1u, 0u, 0u, SCRATCH_DATA, SCRATCH_DATA + 0x200u};
    CHECK_EQ_U32(call_stdcall(0x003D9230u, args, 6u), 0u);
    d3d8_present_reset();
    presents_driven = 0u;
    d3d8_device_store32(0x1DE0u, 0u); /* the mode set is tested elsewhere */
    write_vertices();
    if (discard_boot_commands) {
        d3d8_gpu_kick(); /* the boot's last emission is still in the ring, unrecorded, until a kick drains it */
        d3d8_gpu_stream_discard(d3d8_gpu_stream_count());
    }
}

/* --- pixels ------------------------------------------------------------------------------ */

static bool pixel_is(const gpu_image *image, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b,
                     uint8_t a)
{
    const uint8_t *p = image->pixels + gpu_image_offset(image, x, y);
    return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}

static uint32_t count_not(const gpu_image *image, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    uint32_t count = 0u;
    for (uint32_t y = 0u; y < image->height; y++) {
        for (uint32_t x = 0u; x < image->width; x++) {
            count += !pixel_is(image, x, y, r, g, b, a);
        }
    }
    return count;
}

/* --- render targets (T84a1) -------------------------------------------------------------- */

/* A surface header cloned from the bound target, with its own size word (linear) and no parent. */
static void make_target(uint32_t address, uint32_t width, uint32_t height)
{
    const uint32_t source = d3d8_device_load32(DEV_RENDER_TARGET);
    for (uint32_t offset = 0u; offset < D3D8_SURFACE_HEADER_BYTES; offset += 4u) {
        store(address + offset, d3d8_guest_load32(source + offset));
    }
    store(address + D3D8_SURFACE_SIZE, ((height - 1u) << 12) | (width - 1u));
    store(address + D3D8_SURFACE_PARENT, 0u);
}

/* The same, with a size nothing can measure (no Size word, no exponents). */
static void make_unmeasurable_target(uint32_t address)
{
    make_target(address, 8u, 8u);
    store(address + D3D8_SURFACE_SIZE, 0u);
    store(address + D3D8_SURFACE_FORMAT, d3d8_guest_load32(address + D3D8_SURFACE_FORMAT) & 0x000FFFFFu);
}

/* SetRenderTarget as the title calls it, keeping the depth surface the device had. */
/* The title switching its target: the model sees the switch (the hook), the library's own commands for it are
 * rewound as in present() (the hook drained the ring first, so nothing of the title's is lost). */
static void set_target(uint32_t header, uint32_t depth)
{
    const uint32_t cursor = d3d8_device_load32(D3D8_DEV_CURSOR);
    d3d8_set_render_target(header, depth);
    d3d8_device_store32(D3D8_DEV_CURSOR, cursor);
}

static void bind_target(uint32_t header)
{
    set_target(header, d3d8_device_load32(DEV_DEPTH_SURFACE));
}

/* The ring only, with no kick: the consumer has not recorded these when the target changes. */
static void push_pending(const stream_builder *stream)
{
    const uint32_t cursor = d3d8_pushbuffer_begin();
    for (size_t i = 0u; i < stream->count; i++) {
        store(cursor + (uint32_t)i * 8u, 0x00040000u | stream->pairs[i].method);
        store(cursor + (uint32_t)i * 8u + 4u, stream->pairs[i].data);
    }
    d3d8_pushbuffer_end(cursor + (uint32_t)stream->count * 8u);
}

/* A viewport for a small target (clip +-1 spans the whole image) and the second triangle, green. */
static void build_small_target_draw(stream_builder *stream, uint32_t width, uint32_t height,
                                    uint32_t first_vertex)
{
    const float offset[4] = {(float)width / 2.0f, (float)height / 2.0f, 0.0f, 0.0f};
    const float scale[4] = {(float)width / 2.0f, -(float)height / 2.0f, 1.0f, 0.0f};
    static const float c3_green[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    stream_viewport(stream, offset, scale);
    stream_constants(stream, 3u, c3_green, 4u);
    stream_draw_arrays(stream, GPU_PGRAPH_OP_TRIANGLES, first_vertex, 3u);
}


/* --- temporary files --------------------------------------------------------------------- */

static void make_directories(void)
{
    const char *base = getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp";
    snprintf(spv_directory, sizeof spv_directory, "%s/tsfp_swap_replay_XXXXXX", base);
    if (mkdtemp(spv_directory) == NULL) {
        printf("FATAL mkdtemp failed\n");
        exit(EXIT_FAILURE);
    }
    snprintf(dump_directory, sizeof dump_directory, "%s/dump", spv_directory);
}

static void write_combiner_module(const char *name, const uint32_t *words, size_t bytes)
{
    char path[400];
    snprintf(path, sizeof path, "%s/%s.spv", spv_directory, name);
    FILE *file = fopen(path, "wb");
    if (file == NULL || fwrite(words, 1u, bytes, file) != bytes) {
        printf("FATAL cannot write %s\n", path);
        exit(EXIT_FAILURE);
    }
    fclose(file);
}

static void write_module(void)
{
    char path[400];
    snprintf(path, sizeof path, "%s/%s.spv", spv_directory, SYNTHETIC_PROGRAM_NAME);
    FILE *file = fopen(path, "wb");
    if (file == NULL ||
        fwrite(draw_vertex_words, sizeof(uint32_t), sizeof draw_vertex_words / sizeof(uint32_t),
               file) != sizeof draw_vertex_words / sizeof(uint32_t)) {
        printf("FATAL cannot write %s\n", path);
        exit(EXIT_FAILURE);
    }
    fclose(file);
    write_combiner_module(COMBINER_NAME_PASS, combiner_words_pass, sizeof combiner_words_pass);
    write_combiner_module(COMBINER_NAME_MULTIPLY, combiner_words_multiply, sizeof combiner_words_multiply);
    write_combiner_module(COMBINER_NAME_TEXTURED, combiner_words_textured, sizeof combiner_words_textured);
}

/* The vertex module the synthetic program resolves to, rewritten (T497): `draw_vertex_words` writes oD0 only, the
 * `draw_vertex_t0_words` one also writes oT0. The replay reads the file when a draw needs it. */
static void write_synthetic_vertex_module(const uint32_t *words, size_t bytes)
{
    char path[400];
    snprintf(path, sizeof path, "%s/%s.spv", spv_directory, SYNTHETIC_PROGRAM_NAME);
    FILE *file = fopen(path, "wb");
    if (file == NULL || fwrite(words, 1u, bytes, file) != bytes) {
        printf("FATAL cannot write %s\n", path);
        exit(EXIT_FAILURE);
    }
    fclose(file);
}

static void remove_tree(const char *directory)
{
    DIR *handle = opendir(directory);
    if (handle == NULL) {
        return;
    }
    for (struct dirent *entry = readdir(handle); entry != NULL; entry = readdir(handle)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char path[600];
        snprintf(path, sizeof path, "%s/%s", directory, entry->d_name);
        struct stat info;
        if (stat(path, &info) == 0 && S_ISDIR(info.st_mode)) {
            remove_tree(path);
        } else {
            (void)unlink(path);
        }
    }
    closedir(handle);
    (void)rmdir(directory);
}

static long file_size(const char *name)
{
    char path[600];
    snprintf(path, sizeof path, "%s/%s", dump_directory, name);
    struct stat info;
    return stat(path, &info) == 0 ? (long)info.st_size : -1;
}

static bool is_png(const char *name)
{
    char path[600];
    snprintf(path, sizeof path, "%s/%s", dump_directory, name);
    FILE *file = fopen(path, "rb");
    unsigned char magic[8] = {0};
    const bool read = file != NULL && fread(magic, 1u, 8u, file) == 8u;
    if (file != NULL) {
        fclose(file);
    }
    return read && memcmp(magic, "\x89PNG\r\n\x1a\n", 8u) == 0;
}

static d3d8_swap_replay_config base_config(const char *selector)
{
    d3d8_swap_replay_config config = d3d8_swap_replay_default_config();
    config.spv_directory = spv_directory;
    config.device_selector = selector;
    config.allowed_inferences = GPU_PGRAPH_INFER_ALL;
    return config;
}

/* --- T262: vertex bytes at the kick ------------------------------------------------------- */

static const float upper_left[3][2] = {{-0.9f, -0.9f}, {-0.1f, -0.9f}, {-0.5f, -0.1f}};
static const float lower_right[3][2] = {{0.1f, 0.1f}, {0.9f, 0.1f}, {0.5f, 0.9f}};

/* Rewrite vertices 0..2 of the buffer both draws read (the title reusing one dynamic buffer). */
static void rewrite_triangle(const float corners[3][2])
{
    for (uint32_t i = 0u; i < 3u; i++) {
        const float_vertex vertex = {{corners[i][0], corners[i][1], 0.5f}, {0.0f, 0.0f, 1.0f, 1.0f}};
        uint32_t words[7];
        memcpy(words, &vertex, sizeof words);
        for (uint32_t word = 0u; word < 7u; word++) {
            store(MEMORY_BASE + i * (uint32_t)sizeof vertex + word * 4u, words[word]);
        }
    }
}

/* Green on c3 and vertices 0..2, drawn again through the arrays that are still bound. */
static void build_redraw(stream_builder *stream)
{
    static const float c3_green[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    stream_constants(stream, 3u, c3_green, 4u);
    stream_draw_arrays(stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
}

/* The bug T262 fixes: ONE buffer, rewritten between two draws, each kicked. With the snapshot each draw
 * replays its own bytes. With vertex_budget_bytes 0 the replay reads at the present, so both draws land
 * where the LATER bytes put them (the control, so this test can fail). */
static void test_vertex_snapshot_at_the_kick(const char *selector)
{
    printf("test_vertex_snapshot_at_the_kick (%s)\n", selector);
    for (int snapshot = 1; snapshot >= 0; snapshot--) {
        begin_device(true);
        d3d8_swap_replay_config config = base_config(selector);
        CHECK(config.vertex_budget_bytes == D3D8_SWAP_REPLAY_DEFAULT_VERTEX_BUDGET);
        if (snapshot == 0) {
            config.vertex_budget_bytes = 0u;
        }
        CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
        stream_builder one = {0};
        build_frame_one(&one);      /* upper-left triangle, red, from vertices 0..2 */
        push(&one);                 /* the kick: the GPU runs draw 1 here */
        rewrite_triangle(lower_right);
        stream_builder two = {0};
        build_redraw(&two);         /* the same addresses, now holding the lower-right triangle */
        push(&two);
        d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
        CHECK(stats.commands_decoded == one.count + two.count); /* decoded at the kicks, not yet present */
        CHECK(stats.presents == 0u && stats.frames_replayed == 0u);
        present();
        stats = d3d8_swap_replay_get_stats();
        CHECK(!stats.latched && stats.frames_replayed == 1u && stats.draws == 2u);
        const gpu_image *frame = d3d8_swap_replay_last_frame();
        CHECK(frame != NULL && frame->pixels != NULL);
        if (snapshot != 0) {
            CHECK(stats.draws_snapshotted == 2u);
            CHECK(stats.vertex_bytes_captured == 2u * 84u); /* 3 vertices, stride 28, [0,84) each */
        } else {
            CHECK(stats.draws_snapshotted == 0u && stats.vertex_bytes_captured == 0u);
        }
        if (frame != NULL && frame->pixels != NULL) {
            CHECK(pixel_is(frame, 480u, 336u, 255u, 255u, 0u, 255u)); /* draw 2: its own bytes */
            if (snapshot != 0) {
                CHECK(pixel_is(frame, 160u, 120u, 255u, 0u, 0u, 255u)); /* draw 1: ITS bytes, red */
                CHECK(count_not(frame, 0u, 0u, 0u, 255u) > 20000u);     /* two triangles */
            } else {
                CHECK(pixel_is(frame, 160u, 120u, 0u, 0u, 0u, 255u));   /* the old bug: draw 1 moved */
            }
        }
        stream_free(&one);
        stream_free(&two);
        d3d8_swap_replay_disable();
        environment_end();
    }
}

/* The same across render target passes: the buffer a pass read is rewritten before the next pass. */
static void test_vertex_snapshot_across_passes(const char *selector)
{
    printf("test_vertex_snapshot_across_passes (%s)\n", selector);
    begin_device(true);
    d3d8_swap_replay_config config = base_config(selector);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    const uint32_t back = d3d8_device_load32(DEV_RENDER_TARGET);
    make_target(TARGET_A, 128u, 64u);
    stream_builder first = {0};
    build_frame_one(&first);        /* the back buffer: red, upper-left bytes */
    push(&first);                   /* kicked before the rewrite */
    rewrite_triangle(lower_right);
    bind_target(TARGET_A);
    stream_builder second = {0};
    build_small_target_draw(&second, 128u, 64u, 0u); /* vertices 0..2 again, now lower-right bytes */
    push(&second);
    bind_target(back);
    present();
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.passes_replayed == 2u);
    CHECK(stats.draws == 2u && stats.draws_snapshotted == 2u);
    const gpu_image *presented = d3d8_swap_replay_last_frame();
    CHECK(presented != NULL && presented->pixels != NULL);
    if (presented != NULL && presented->pixels != NULL) {
        CHECK(pixel_is(presented, 160u, 120u, 255u, 0u, 0u, 255u)); /* its own bytes, not the rewrite */
        CHECK(pixel_is(presented, 480u, 336u, 0u, 0u, 0u, 255u));
    }
    const gpu_image *offscreen = d3d8_swap_replay_offscreen_frame(0u, NULL);
    CHECK(d3d8_swap_replay_offscreen_count() == 1u && offscreen != NULL && offscreen->pixels != NULL);
    if (offscreen != NULL && offscreen->pixels != NULL) {
        CHECK(pixel_is(offscreen, 96u, 48u, 255u, 255u, 0u, 255u));
        CHECK(pixel_is(offscreen, 16u, 12u, 0u, 0u, 0u, 255u));
    }
    stream_free(&first);
    stream_free(&second);
    d3d8_swap_replay_disable();
    environment_end();
}

/* THE STATED ORDERING: the bytes are read at the kick, so two draws handed to the GPU in ONE kick with the
 * buffer rewritten between them (in program order) both see the later bytes. Asserted so the documented
 * behaviour is a measured one and a change to it is noticed. */
static void test_one_kick_sees_the_later_bytes(const char *selector)
{
    printf("test_one_kick_sees_the_later_bytes (%s)\n", selector);
    begin_device(true);
    d3d8_swap_replay_config config = base_config(selector);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    stream_builder one = {0};
    build_frame_one(&one);
    push_pending(&one);             /* in the ring, NOT kicked */
    rewrite_triangle(lower_right);
    stream_builder two = {0};
    build_redraw(&two);
    push(&two);                     /* one kick hands the GPU both draws */
    present();
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.draws == 2u);
    const gpu_image *frame = d3d8_swap_replay_last_frame();
    CHECK(frame != NULL && frame->pixels != NULL);
    if (frame != NULL && frame->pixels != NULL) {
        CHECK(pixel_is(frame, 480u, 336u, 255u, 255u, 0u, 255u));
        CHECK(pixel_is(frame, 160u, 120u, 0u, 0u, 0u, 255u)); /* draw 1 saw the rewrite: one kick */
    }
    stream_free(&one);
    stream_free(&two);
    d3d8_swap_replay_disable();
    environment_end();
}

/* The library's own commands in a real Swap (T449). Swap binds the front buffer and restores the title's target
 * through d3d8_set_render_target, 40 recorded commands each, and the decoder interprets none of the 17 distinct
 * methods they carry (NOP 0x0100, WAIT_FOR_IDLE 0x0110, the surface words 0x0208 0x020C 0x0210 0x0214 0x0290, the
 * clip 0x0200 0x0204 0x02B4 0x02C0 0x02E0 and depth/stencil enables 0x030C 0x032C that stay unhandled until their
 * output group is on, 0x1D7C, and the depth clip range 0x0394 0x0398). So a strict replay refuses the first real
 * Swap, and a non-strict one counts and names them while the draws still replay. The other tests present through
 * present(), which rewinds these commands, so THIS is the test that notices when the decoder learns them: then
 * the strict half flips and the unhandled count falls. Tracked as T541. */
static const uint32_t swap_own_methods[] = {0x0100u, 0x0110u, 0x0208u, 0x020Cu, 0x0210u, 0x0214u,
                                            0x0290u, 0x0200u, 0x0204u, 0x02B4u, 0x02C0u, 0x02E0u,
                                            0x030Cu, 0x032Cu, 0x1D7Cu, 0x0394u, 0x0398u};
#define SWAP_OWN_METHOD_COUNT (sizeof swap_own_methods / sizeof swap_own_methods[0])

static void test_swap_own_emission_reaches_the_replay(const char *selector)
{
    printf("test_swap_own_emission_reaches_the_replay (%s)\n", selector);
    CHECK(SWAP_OWN_METHOD_COUNT == 17u);
    /* strict: refused at the first method of the first emission, named */
    begin_device(true);
    d3d8_swap_replay_config strict_config = base_config(selector);
    CHECK(d3d8_swap_replay_enable(&strict_config, NULL, 0u));
    stream_builder stream = {0};
    build_frame_one(&stream);
    push(&stream);
    capture_clear();
    present_through_swap();
    d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && stats.frames_refused == 1u && stats.frames_replayed == 0u);
    CHECK(strstr(stats.error, "method 0x0100") != NULL);
    CHECK(strstr(stats.error, "not in the measured list") != NULL);
    stream_free(&stream);
    d3d8_swap_replay_disable();
    environment_end();
    /* non-strict: every one of the 17 is counted and logged, the draw still replays */
    begin_device(true);
    d3d8_swap_replay_config config = base_config(selector);
    config.strict = false;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    build_frame_one(&stream);
    push(&stream);
    capture_clear();
    present_through_swap();
    stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.draws == 1u);
    CHECK(stats.unhandled_methods == SWAP_OWN_METHOD_COUNT);
    for (size_t index = 0u; index < SWAP_OWN_METHOD_COUNT; index++) {
        char expected[48];
        snprintf(expected, sizeof expected, "UNHANDLED method 0x%04X", (unsigned)swap_own_methods[index]);
        CHECK(captured_has(expected));
    }
    const gpu_image *frame = d3d8_swap_replay_last_frame();
    CHECK(frame != NULL && frame->pixels != NULL);
    if (frame != NULL && frame->pixels != NULL) {
        CHECK(pixel_is(frame, 160u, 120u, 255u, 0u, 0u, 255u));
    }
    stream_free(&stream);
    d3d8_swap_replay_disable();
    environment_end();
    /* T541, T462: with every output group the bind packet's 17 methods are decoded, so STRICT no longer refuses at 0x0100:
     * the real Swap replays the synthetic frame, the same red pixel as the lenient run. */
    begin_device(true);
    d3d8_swap_replay_config groups_config = base_config(selector);
    groups_config.output_groups = GPU_PGRAPH_OUTPUT_ALL_MEASURED;
    groups_config.allowed_inferences = GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_OUTPUT_ALL;
    CHECK(d3d8_swap_replay_enable(&groups_config, NULL, 0u));
    build_frame_one(&stream);
    push(&stream);
    capture_clear();
    present_through_swap();
    stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_refused == 0u && stats.frames_replayed == 1u && stats.draws == 1u);
    CHECK(strstr(stats.error, "method 0x0100") == NULL);
    CHECK(strstr(stats.error, "not in the measured list") == NULL);
    CHECK(stats.unhandled_methods == 0u);
    frame = d3d8_swap_replay_last_frame();
    CHECK(frame != NULL && frame->pixels != NULL && pixel_is(frame, 160u, 120u, 255u, 0u, 0u, 255u));
    stream_free(&stream);
    d3d8_swap_replay_disable();
    environment_end();
}

/* The pool is per frame: a budget that holds two draws holds two draws in every frame. */
static void test_vertex_budget_is_per_frame(const char *selector)
{
    printf("test_vertex_budget_is_per_frame (%s)\n", selector);
    begin_device(true);
    d3d8_swap_replay_config config = base_config(selector);
    config.vertex_budget_bytes = 2u * 84u;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    for (uint64_t frame_index = 0u; frame_index < 3u; frame_index++) {
        rewrite_triangle(upper_left);
        stream_builder one = {0};
        if (frame_index == 0u) {
            build_frame_one(&one);
        } else {
            build_redraw(&one);
        }
        push(&one);
        rewrite_triangle(lower_right);
        stream_builder two = {0};
        build_redraw(&two);
        push(&two);
        present();
        const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
        CHECK(!stats.latched && stats.frames_replayed == frame_index + 1u);
        CHECK(stats.draws == 2u * (frame_index + 1u) && stats.vertex_bytes_captured == 168u * (frame_index + 1u));
        stream_free(&one);
        stream_free(&two);
    }
    d3d8_swap_replay_disable();
    environment_end();
}


/* --- the tests that need no device ------------------------------------------------------- */

static void test_default_is_off(void)
{
    printf("test_default_is_off\n");
    begin_device(true);
    stream_builder stream = {0};
    build_frame_one(&stream);
    push(&stream);
    const size_t recorded = d3d8_gpu_stream_count();
    CHECK(stream.count != 0u);
    CHECK_EQ_U32(recorded, stream.count); /* begin_device drained CreateDevice's own commands, only the frame is here */
    CHECK(!d3d8_swap_replay_enabled());
    const uint64_t fences_before = d3d8_gpu_get_stats().fences_inserted;
    present_through_swap();
    CHECK_EQ_U32(d3d8_frame_queue_total(), 1u);        /* the present itself still happened */
    /* Nothing was decoded or released. The present's own emissions grew the recording by exactly these, MEASURED
     * (stream dumped command by command, and bisected: the suite was green at 217f6d7 and red from b739b38):
     *  - T391: the fences. present() is Swap(2) then Swap(4), one fence each (d3d8_swap's flag 2 insert and
     *    d3d8_present_finish), each writing four recorded commands: the software method on subchannel 5,
     *    SEMAPHORE_RELEASE and two colour clear values.
     *  - T449 (b739b38): d3d8_set_render_target now emits the 75 dword stream of the original, 40 recorded commands
     *    (the target and depth offset blocks between NOP and WAIT_FOR_IDLE pairs, the surface format, the clip,
     *    the viewport and depth range). Swap(2) binds the front buffer through it in d3d8_present_prepare and
     *    Swap(4) restores the saved target through it in d3d8_present_restore_state, so two calls, 80 commands.
     * Before b739b38 the model elided both, and the pin was recorded + 8. */
    const uint64_t fences = d3d8_gpu_get_stats().fences_inserted - fences_before;
    CHECK_EQ_U32((uint32_t)fences, 2u);
    CHECK_EQ_U32(d3d8_gpu_stream_count(), recorded + 4u * fences + 2u * SET_RENDER_TARGET_COMMANDS);
    CHECK(d3d8_swap_replay_last_frame() == NULL);
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.presents == 0u && stats.frames_replayed == 0u && !stats.latched);
    stream_free(&stream);
    environment_end();
}

static void test_configuration_refusals(void)
{
    printf("test_configuration_refusals\n");
    char error[200];
    d3d8_swap_replay_config config = base_config(NULL);
    CHECK(!d3d8_swap_replay_enable(NULL, error, sizeof error));
    config.spv_directory = NULL;
    CHECK(!d3d8_swap_replay_enable(&config, error, sizeof error));
    CHECK(strstr(error, "spv_directory") != NULL);
    config.spv_directory = "/nonexistent/tsfp_swap_replay_dir";
    CHECK(!d3d8_swap_replay_enable(&config, error, sizeof error));
    CHECK(strstr(error, "cannot open") != NULL);
    char empty[300];
    snprintf(empty, sizeof empty, "%s/empty", spv_directory);
    CHECK(mkdir(empty, 0777) == 0);
    config.spv_directory = empty;
    CHECK(!d3d8_swap_replay_enable(&config, error, sizeof error));
    CHECK(strstr(error, "no .spv module") != NULL);
    config = base_config(NULL);
    config.width = 64u;
    CHECK(!d3d8_swap_replay_enable(&config, error, sizeof error));
    CHECK(strstr(error, "width and height") != NULL);
    CHECK(!d3d8_swap_replay_enabled());
    /* T84b/T84a3: line_width is 0 (unstated) or exactly 1.0, nothing else can be drawn */
    CHECK(base_config(NULL).line_width == 0.0f && d3d8_swap_replay_default_config().line_width == 0.0f);
    const float bad_widths[] = {2.0f, 0.5f, -1.0f, 1.0001f, __builtin_nanf(""), __builtin_inff()};
    for (size_t i = 0u; i < sizeof bad_widths / sizeof bad_widths[0]; i++) {
        config = base_config(NULL);
        config.line_width = bad_widths[i];
        error[0] = '\0';
        CHECK(!d3d8_swap_replay_enable(&config, error, sizeof error));
        CHECK(strstr(error, "line_width") != NULL);
        CHECK(!d3d8_swap_replay_enabled());
    }
    /* T267: output_groups is 0 by default, and only measured groups can be named (T502: 0x40 and 0x80 are groups now) */
    CHECK(base_config(NULL).output_groups == 0u && d3d8_swap_replay_default_config().output_groups == 0u);
    const uint32_t bad_groups[] = {0x8000u, 0x80000000u, GPU_PGRAPH_OUTPUT_SCISSOR | 0x4000u, 0x100u};
    for (size_t i = 0u; i < sizeof bad_groups / sizeof bad_groups[0]; i++) {
        config = base_config(NULL);
        config.output_groups = bad_groups[i];
        error[0] = '\0';
        CHECK(!d3d8_swap_replay_enable(&config, error, sizeof error));
        CHECK(strstr(error, "output_groups") != NULL);
        CHECK(!d3d8_swap_replay_enabled());
    }
    config = base_config(NULL);
    CHECK(d3d8_swap_replay_enable(&config, error, sizeof error));
    CHECK(d3d8_swap_replay_enabled() && error[0] == '\0');
    d3d8_swap_replay_disable();
    CHECK(!d3d8_swap_replay_enabled());
    CHECK(d3d8_swap_replay_get_stats().presents == 0u);
    (void)rmdir(empty);
}

/* T84a3: the options reach the config the replay holds, and the defaults are the old behaviour. */
static void test_config_options(void)
{
    printf("test_config_options\n");
    CHECK(d3d8_swap_replay_get_config().spv_directory == NULL); /* nothing when not enabled */
    d3d8_swap_replay_config config = base_config(NULL);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    d3d8_swap_replay_config held = d3d8_swap_replay_get_config();
    CHECK(held.spv_directory != NULL && strcmp(held.spv_directory, spv_directory) == 0);
    CHECK(!held.flip_y && !held.viewport_inverse_modules && held.line_width == 0.0f);
    CHECK(!held.window_clip_modules);
    CHECK(held.strict && held.allowed_inferences == GPU_PGRAPH_INFER_ALL);
    CHECK(held.output_groups == 0u);

    config = base_config(NULL);
    config.output_groups = GPU_PGRAPH_OUTPUT_SCISSOR;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    CHECK(d3d8_swap_replay_get_config().output_groups == GPU_PGRAPH_OUTPUT_SCISSOR);

    config = base_config(NULL);
    config.flip_y = true;
    config.viewport_inverse_modules = true;
    config.line_width = 1.0f;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    held = d3d8_swap_replay_get_config();
    CHECK(held.spv_directory != NULL && strcmp(held.spv_directory, spv_directory) == 0);
    CHECK(held.flip_y && held.viewport_inverse_modules && held.line_width == 1.0f);
    CHECK(!held.window_clip_modules);
    config = base_config(NULL);
    config.window_clip_modules = true;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    CHECK(d3d8_swap_replay_get_config().window_clip_modules && !d3d8_swap_replay_get_config().flip_y);
    d3d8_swap_replay_disable();
    CHECK(d3d8_swap_replay_get_config().spv_directory == NULL && !d3d8_swap_replay_get_config().flip_y);

    /* the strings are copies: the caller's buffer may go away after enable */
    char scratch[300];
    snprintf(scratch, sizeof scratch, "%s", spv_directory);
    config = base_config(NULL);
    config.spv_directory = scratch;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    memset(scratch, 'x', sizeof scratch - 1u);
    scratch[sizeof scratch - 1u] = '\0';
    held = d3d8_swap_replay_get_config();
    CHECK(held.spv_directory != NULL && strcmp(held.spv_directory, spv_directory) == 0);
    CHECK(held.spv_directory != scratch);
    d3d8_swap_replay_disable();
}

/* T84a3: d3d8_gpu_reset (CreateDevice) between enable and a present no longer needs disable + enable. */
static void test_reset_clears_latch(void)
{
    printf("test_reset_clears_latch\n");
    begin_device(true);
    d3d8_swap_replay_config config = base_config(NULL);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    stream_builder stream = {0};
    build_frame_one(&stream);
    stream_pair(&stream, UNHANDLED_METHOD, 0x12345678u);
    push(&stream);
    present();
    d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && stats.frames_refused == 1u && stats.model_resets == 0u);

    d3d8_gpu_reset();
    capture_clear();
    /* state-only commands of the new device (no draw), then the present hook called directly so
     * nothing else syncs first. The refused pair index of the old model must not skip the first ones. */
    stream_builder state_only = {0};
    static const float c3_blue[4] = {0.0f, 0.0f, 1.0f, 0.0f};
    stream_constants(&state_only, 3u, c3_blue, 4u);
    stream_array(&state_only, 1u, MEMORY_BASE, array_format(28u, 3u, GPU_PGRAPH_TYPE_F));
    const uint64_t decoded_before = d3d8_swap_replay_get_stats().commands_decoded;
    push(&state_only);
    const size_t pending = d3d8_gpu_stream_count();
    CHECK(pending >= state_only.count && state_only.count > 3u);
    /* T262: the commands are decoded at the kick that recorded them, so the model was rebuilt and the
     * new device's commands decoded from its first one before any present */
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.model_resets == 1u && !stats.latched && stats.error[0] == '\0');
    CHECK(stats.commands_decoded - decoded_before == pending);
    d3d8_swap_replay_on_present(2u);
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.model_resets == 1u && !stats.latched && stats.error[0] == '\0');
    CHECK(stats.presents == 2u && stats.frames_skipped == 0u && stats.frames_empty == 1u);
    CHECK(stats.commands_decoded - decoded_before == pending);
    stream_free(&state_only);
    CHECK(stats.frames_refused == 1u); /* the counters accumulate */
    CHECK(captured_has("the GPU was reset"));
    present(); /* no second rebuild without a second reset */
    CHECK(d3d8_swap_replay_get_stats().model_resets == 1u);
    stream_free(&stream);
    d3d8_swap_replay_disable();
    environment_end();
}

/* A switch noted before the reset points into a recording that no longer exists. */
static void test_reset_forgets_switches(void)
{
    printf("test_reset_forgets_switches\n");
    begin_device(true);
    d3d8_swap_replay_config config = base_config(NULL);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    make_target(TARGET_A, 128u, 64u);
    stream_builder stream = {0};
    build_frame_one(&stream);
    push(&stream);
    const size_t before = d3d8_gpu_stream_count();
    CHECK(before >= stream.count);
    bind_target(TARGET_A); /* noted at index `before` */
    d3d8_gpu_reset();      /* the recording is empty again */
    CHECK(d3d8_gpu_stream_count() == 0u);
    present();
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.model_resets == 1u && !stats.latched && stats.frames_refused == 0u);
    CHECK(stats.frames_empty == 1u);
    stream_free(&stream);
    d3d8_swap_replay_disable();
    environment_end();
}

/* The model is a new one: what the old one counted is gone, and what it logged is logged again. */
static void test_reset_rebuilds_the_model(void)
{
    printf("test_reset_rebuilds_the_model\n");
    begin_device(true);
    d3d8_swap_replay_config config = base_config(NULL);
    config.strict = false;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    stream_builder first = {0};
    stream_pair(&first, UNHANDLED_METHOD, 1u);
    push(&first);
    capture_clear();
    present();
    CHECK(captured_has("UNHANDLED method 0x0304"));
    CHECK(d3d8_swap_replay_get_stats().unhandled_methods == 1u);

    d3d8_gpu_reset();
    stream_builder second = {0};
    stream_pair(&second, UNHANDLED_METHOD, 2u);
    push(&second);
    capture_clear();
    present();
    CHECK(captured_has("UNHANDLED method 0x0304")); /* logged again: a new model, a new log position */
    CHECK(d3d8_swap_replay_get_stats().unhandled_methods == 1u);
    stream_builder third = {0};
    stream_pair(&third, UNHANDLED_METHOD + 4u, 3u);
    push(&third);
    present();
    CHECK(d3d8_swap_replay_get_stats().unhandled_methods == 2u); /* this model saw two, not three */
    CHECK(d3d8_swap_replay_get_stats().model_resets == 1u);
    stream_free(&first);
    stream_free(&second);
    stream_free(&third);
    d3d8_swap_replay_disable();
    environment_end();
}

static void test_dimensions(void)
{
    printf("test_dimensions\n");
    begin_device(true);
    uint32_t width = 0u;
    uint32_t height = 0u;
    /* the title's own target: linear 640x480, Size word 0x271DF27F (docs: measured under the oracle) */
    store(HEADER_SCRATCH + D3D8_SURFACE_SIZE, 0x271DF27Fu);
    CHECK(d3d8_surface_dimensions(HEADER_SCRATCH, &width, &height));
    CHECK(width == 640u && height == 480u);
    /* a non-square linear one so a swapped field cannot pass */
    store(HEADER_SCRATCH + D3D8_SURFACE_SIZE, (19u << 24) | ((200u - 1u) << 12) | (320u - 1u));
    CHECK(d3d8_surface_dimensions(HEADER_SCRATCH, &width, &height));
    CHECK(width == 320u && height == 200u);
    /* swizzled: no Size word, exponents in the Format word */
    store(HEADER_SCRATCH + D3D8_SURFACE_SIZE, 0u);
    store(HEADER_SCRATCH + D3D8_SURFACE_FORMAT, (7u << 24) | (8u << 20) | 0x1200u);
    CHECK(d3d8_surface_dimensions(HEADER_SCRATCH, &width, &height));
    CHECK(width == 256u && height == 128u);
    store(HEADER_SCRATCH + D3D8_SURFACE_FORMAT, 0x1200u);
    CHECK(!d3d8_surface_dimensions(HEADER_SCRATCH, &width, &height));
    CHECK(!d3d8_surface_dimensions(0u, &width, &height));
    CHECK(!d3d8_surface_dimensions(HEADER_SCRATCH, NULL, &height));
    environment_end();
}

/* A method outside the measured list in the frame: strict refuses it loudly and LATCHES. */
static void test_strict_refusal_and_latch(void)
{
    printf("test_strict_refusal_and_latch\n");
    begin_device(true);
    d3d8_swap_replay_config config = base_config(NULL);
    char error[200];
    CHECK(d3d8_swap_replay_enable(&config, error, sizeof error));
    stream_builder stream = {0};
    build_frame_one(&stream);
    stream_pair(&stream, UNHANDLED_METHOD, 0x12345678u);
    push(&stream);
    capture_clear();
    present();
    d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.presents == 1u && stats.frames_refused == 1u && stats.latched);
    CHECK(stats.frames_replayed == 0u && stats.dumps_written == 0u);
    CHECK(strstr(stats.error, "frame 1 refused") != NULL);
    CHECK(strstr(stats.error, "0x0304") != NULL);
    CHECK(captured_has("d3d8 swap replay: frame 1 refused"));
    CHECK(captured_has("0x0304"));
    CHECK(captured_has("STOPPED"));
    CHECK(d3d8_swap_replay_last_frame() == NULL);
    CHECK(stats.unhandled_methods == 0u); /* strict refuses it, it is not tallied (non-strict counts) */

    /* the next frame is skipped, says so, and the recording is released rather than left to fill */
    stream_builder more = {0};
    build_frame_two(&more);
    push(&more);
    capture_clear();
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.presents == 2u && stats.frames_skipped == 1u && stats.frames_refused == 1u);
    CHECK(d3d8_gpu_stream_count() == 0u);
    CHECK(d3d8_swap_replay_last_frame() == NULL);
    stream_free(&more);
    stream_free(&stream);
    d3d8_swap_replay_disable();
    environment_end();
}

static void test_fatal_on_refusal(void)
{
    printf("test_fatal_on_refusal\n");
    begin_device(true);
    d3d8_swap_replay_config config = base_config(NULL);
    config.fatal_on_refusal = true;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    stream_builder stream = {0};
    build_frame_one(&stream);
    stream_pair(&stream, UNHANDLED_METHOD, 1u);
    push(&stream);
    RUN_EXPECTING_FATAL(present());
    CHECK(fatal_seen);
    CHECK(fatal_address == 0x003D8E50u);
    CHECK(strstr(fatal_text, "0x0304") != NULL);
    stream_free(&stream);
    d3d8_swap_replay_disable();
    environment_end();
}

/* The recording fills before a present: commands are dropped, the model would be incomplete. */
static void test_dropped_commands_refuse(void)
{
    printf("test_dropped_commands_refuse\n");
    begin_device(true);
    stream_builder filler = {0};
    for (uint32_t i = 0u; i < 4096u; i++) {
        stream_pair(&filler, GPU_PGRAPH_EXECUTION_MODE, 6u);
    }
    for (uint32_t round = 0u; round < D3D8_GPU_STREAM_CAPACITY / 4096u + 2u; round++) {
        push(&filler);
    }
    CHECK(d3d8_gpu_get_stats().commands_dropped > 0u);
    CHECK(d3d8_swap_replay_enable(&(d3d8_swap_replay_config){
        .spv_directory = spv_directory, .strict = true, .allowed_inferences = GPU_PGRAPH_INFER_ALL},
        NULL, 0u));
    present();
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && stats.frames_refused == 1u);
    CHECK(strstr(stats.error, "dropped") != NULL);
    stream_free(&filler);
    d3d8_swap_replay_disable();
    environment_end();
}

static void test_unmeasurable_size_is_a_refusal(void)
{
    printf("test_unmeasurable_size_is_a_refusal\n");
    begin_device(true);
    CHECK(d3d8_swap_replay_enable(&(d3d8_swap_replay_config){
        .spv_directory = spv_directory, .strict = true, .allowed_inferences = GPU_PGRAPH_INFER_ALL},
        NULL, 0u));
    stream_builder stream = {0};
    build_frame_one(&stream);
    push(&stream);
    d3d8_device_store32(0x1A04u, 0u); /* no render target bound at the end of the swap */
    d3d8_device_store32(0x1A90u, 0u);
    (void)d3d8_gpu_stream_count();
    d3d8_swap_replay_on_present(1u);
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && strstr(stats.error, "cannot be measured") != NULL);
    stream_free(&stream);
    d3d8_swap_replay_disable();
    environment_end();
}

/* The recording released from the front, the rest kept in order. */
static void test_stream_discard(void)
{
    printf("test_stream_discard\n");
    begin_device(true);
    stream_builder stream = {0};
    for (uint32_t i = 0u; i < 6u; i++) {
        stream_pair(&stream, GPU_PGRAPH_EXECUTION_MODE, 100u + i);
    }
    push(&stream);
    const size_t total = d3d8_gpu_stream_count();
    CHECK(total == 6u);
    const uint64_t recorded = d3d8_gpu_get_stats().commands_recorded;
    /* T391: CreateDevice's own fence packet was recorded and released before this stream was pushed, so the
     * counters start above zero. */
    const uint64_t discarded = d3d8_gpu_get_stats().commands_discarded;
    d3d8_gpu_stream_discard(2u);
    CHECK(d3d8_gpu_stream_count() == 4u);
    CHECK(d3d8_gpu_stream_at(0u).data == 102u && d3d8_gpu_stream_at(3u).data == 105u);
    CHECK(d3d8_gpu_stream_at(4u).method == 0u);
    CHECK(d3d8_gpu_get_stats().commands_discarded == discarded + 2u && d3d8_gpu_get_stats().commands_recorded == recorded);
    d3d8_gpu_stream_discard(100u);
    CHECK(d3d8_gpu_stream_count() == 0u && d3d8_gpu_get_stats().commands_discarded == discarded + 6u);
    stream_free(&stream);
    environment_end();
}

/* A bracket still open at a present cannot be carried into the next frame's draw list. T262: it is
 * found at its own present (the model is decoded at the kick, so it is known then), not a frame later. */
static void test_open_bracket_at_present(void)
{
    printf("test_open_bracket_at_present\n");
    begin_device(true);
    CHECK(d3d8_swap_replay_enable(&(d3d8_swap_replay_config){
        .spv_directory = spv_directory, .strict = true, .allowed_inferences = GPU_PGRAPH_INFER_ALL},
        NULL, 0u));
    stream_builder stream = {0};
    stream_pair(&stream, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_TRIANGLES);
    push(&stream);
    d3d8_swap_replay_on_present(1u);
    d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.presents == 1u && stats.frames_refused == 1u && stats.frames_empty == 0u);
    CHECK(stats.latched && strstr(stats.error, "still open at the present") != NULL);
    stream_free(&stream);
    d3d8_swap_replay_disable();
    environment_end();
}


/* --- T84a1: per-target frames, the tests that need no device ------------------------------ */

/* A draw into a target nothing can measure is a loud refusal that names the target. */
static void test_unmeasured_target_refuses(void)
{
    printf("test_unmeasured_target_refuses\n");
    begin_device(true);
    CHECK(d3d8_swap_replay_enable(&(d3d8_swap_replay_config){
        .spv_directory = spv_directory, .strict = true, .allowed_inferences = GPU_PGRAPH_INFER_ALL},
        NULL, 0u));
    const uint32_t back = d3d8_device_load32(DEV_RENDER_TARGET);
    make_unmeasurable_target(TARGET_A);
    bind_target(TARGET_A);
    stream_builder stream = {0};
    build_frame_one(&stream);
    push(&stream);
    bind_target(back);
    capture_clear();
    present();
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && stats.frames_refused == 1u && stats.frames_replayed == 0u);
    CHECK(strstr(stats.error, "cannot be measured") != NULL);
    CHECK(strstr(stats.error, "0x00D08100") != NULL);
    CHECK(captured_has("render target 0x00D08100"));
    CHECK(d3d8_swap_replay_last_frame() == NULL && d3d8_swap_replay_offscreen_count() == 0u);
    stream_free(&stream);
    d3d8_swap_replay_disable();
    environment_end();
}

/* A bracket spanning a SetRenderTarget would put half a draw in each target. */
static void test_open_bracket_at_switch(void)
{
    printf("test_open_bracket_at_switch\n");
    begin_device(true);
    CHECK(d3d8_swap_replay_enable(&(d3d8_swap_replay_config){
        .spv_directory = spv_directory, .strict = true, .allowed_inferences = GPU_PGRAPH_INFER_ALL},
        NULL, 0u));
    const uint32_t back = d3d8_device_load32(DEV_RENDER_TARGET);
    make_target(TARGET_A, 128u, 64u);
    stream_builder stream = {0};
    stream_pair(&stream, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_TRIANGLES);
    push_pending(&stream);
    bind_target(TARGET_A);
    bind_target(back);
    present();
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    /* Latched either way. T391: the present's fence packet lands inside the open bracket, and the decoder refuses
     * that first (a software method in a bracket), before the switch check would name the bracket. */
    CHECK(stats.latched && (strstr(stats.error, "still open at a render target switch") != NULL ||
                            strstr(stats.error, "inside a BEGIN_END bracket") != NULL));
    stream_free(&stream);
    d3d8_swap_replay_disable();
    environment_end();
}

/* A target that changed without SetRenderTarget breaks the chain and is refused, not guessed. */
static void test_broken_switch_chain_refuses(void)
{
    printf("test_broken_switch_chain_refuses\n");
    begin_device(true);
    CHECK(d3d8_swap_replay_enable(&(d3d8_swap_replay_config){
        .spv_directory = spv_directory, .strict = true, .allowed_inferences = GPU_PGRAPH_INFER_ALL},
        NULL, 0u));
    const uint32_t back = d3d8_device_load32(DEV_RENDER_TARGET);
    make_target(TARGET_A, 128u, 64u);
    make_target(TARGET_B, 96u, 48u);
    bind_target(TARGET_A);
    d3d8_device_store32(DEV_RENDER_TARGET, TARGET_B); /* bound behind the hook's back */
    stream_builder stream = {0};
    build_frame_one(&stream);
    push(&stream);
    d3d8_swap_replay_on_present(1u);
    d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && strstr(stats.error, "changed without SetRenderTarget") != NULL);
    CHECK(strstr(stats.error, "0x00D08140") != NULL); /* names the target bound at the present */
    d3d8_swap_replay_disable();

    /* the same break in the MIDDLE of a frame: the target after the first switch is not the one the
     * second switch starts from, though the frame ends at the right target */
    CHECK(d3d8_swap_replay_enable(&(d3d8_swap_replay_config){
        .spv_directory = spv_directory, .strict = true, .allowed_inferences = GPU_PGRAPH_INFER_ALL},
        NULL, 0u));
    bind_target(back);
    bind_target(TARGET_A);
    d3d8_device_store32(DEV_RENDER_TARGET, TARGET_B);
    bind_target(back);
    d3d8_swap_replay_on_present(2u);
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && strstr(stats.error, "starts from 0x00D08140") != NULL);
    stream_free(&stream);
    d3d8_swap_replay_disable();
    environment_end();
}

/* More switches than the table holds: refused, and a frame with fewer is fine. */
static void test_too_many_switches_refuse(void)
{
    printf("test_too_many_switches_refuse\n");
    begin_device(true);
    CHECK(d3d8_swap_replay_enable(&(d3d8_swap_replay_config){
        .spv_directory = spv_directory, .strict = true, .allowed_inferences = GPU_PGRAPH_INFER_ALL},
        NULL, 0u));
    make_target(TARGET_A, 128u, 64u);
    const uint32_t back = d3d8_device_load32(DEV_RENDER_TARGET);
    for (uint32_t i = 0u; i < 33u; i++) {
        bind_target(TARGET_A);
        bind_target(back);
    }
    d3d8_swap_replay_on_present(1u);
    d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && strstr(stats.error, "more than 64 render target switches") != NULL);
    d3d8_swap_replay_disable();
    CHECK(d3d8_swap_replay_enable(&(d3d8_swap_replay_config){
        .spv_directory = spv_directory, .strict = true, .allowed_inferences = GPU_PGRAPH_INFER_ALL},
        NULL, 0u));
    for (uint32_t i = 0u; i < 32u; i++) {
        bind_target(TARGET_A);
        bind_target(back);
    }
    d3d8_swap_replay_on_present(2u);
    stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_empty == 1u);
    d3d8_swap_replay_disable();
    environment_end();
}

/* A recording released between a switch and the present leaves the switch pointing outside it. */
static void test_released_recording_refuses(void)
{
    printf("test_released_recording_refuses\n");
    for (int case_index = 0; case_index < 2; case_index++) {
        begin_device(true);
        CHECK(d3d8_swap_replay_enable(&(d3d8_swap_replay_config){
            .spv_directory = spv_directory, .strict = true,
            .allowed_inferences = GPU_PGRAPH_INFER_ALL}, NULL, 0u));
        const uint32_t back = d3d8_device_load32(DEV_RENDER_TARGET);
        make_target(TARGET_A, 128u, 64u);
        stream_builder stream = {0};
        build_frame_one(&stream);
        push_pending(&stream);
        bind_target(TARGET_A);                       /* switch 0 at the end of the frame's commands */
        if (case_index == 1) {
            d3d8_gpu_stream_discard(d3d8_gpu_stream_count());
        }
        bind_target(back);                           /* case 1: switch 1 is before switch 0 */
        if (case_index == 0) {
            d3d8_gpu_stream_discard(d3d8_gpu_stream_count());
        }
        d3d8_swap_replay_on_present(1u);
        const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
        CHECK(stats.latched && strstr(stats.error, "released or reset") != NULL);
        stream_free(&stream);
        d3d8_swap_replay_disable();
        environment_end();
    }
}

/* Disabled, the hook does nothing: no drain, no note. */
static void test_hook_is_inert_when_disabled(void)
{
    printf("test_hook_is_inert_when_disabled\n");
    begin_device(true);
    make_target(TARGET_A, 128u, 64u);
    stream_builder stream = {0};
    build_frame_one(&stream);
    push_pending(&stream);
    const size_t recorded = d3d8_gpu_stream_count();
    bind_target(TARGET_A);
    CHECK(d3d8_gpu_stream_count() == recorded); /* the ring was not drained */
    d3d8_swap_replay_on_render_target(1u, 2u);
    CHECK(d3d8_swap_replay_get_stats().presents == 0u);
    stream_free(&stream);
    environment_end();
}

/* --- the tests that need a device -------------------------------------------------------- */

static void test_frames(const char *selector)
{
    printf("test_frames (%s)\n", selector);
    begin_device(true);
    d3d8_swap_replay_config config = base_config(selector);
    config.dump_directory = dump_directory;
    char error[200];
    CHECK(d3d8_swap_replay_enable(&config, error, sizeof error));

    /* frame 1: measured size, one red triangle */
    stream_builder one = {0};
    build_frame_one(&one);
    push(&one);
    capture_clear();
    present();
    d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.presents == 1u && stats.frames_replayed == 1u && stats.draws == 1u);
    CHECK(!stats.latched && stats.frames_refused == 0u);
    CHECK(stats.last_width == 640u && stats.last_height == 480u);
    CHECK((stats.used_inferences & GPU_PGRAPH_INFER_PROGRAM_HEADER) != 0u);
    CHECK((stats.used_inferences & GPU_PGRAPH_INFER_COMPONENT_DEFAULTS) != 0u);
    CHECK(d3d8_gpu_stream_count() == 0u); /* decoded and released */
    const gpu_image *frame = d3d8_swap_replay_last_frame();
    CHECK(frame != NULL && frame->pixels != NULL);
    if (frame != NULL && frame->pixels != NULL) {
        CHECK(frame->width == 640u && frame->height == 480u);
        CHECK(pixel_is(frame, 160u, 120u, 255u, 0u, 0u, 255u));  /* inside draw 1 */
        CHECK(pixel_is(frame, 480u, 336u, 0u, 0u, 0u, 255u));    /* where draw 2 will be: clear */
        CHECK(pixel_is(frame, 639u, 0u, 0u, 0u, 0u, 255u));
        const uint32_t covered = count_not(frame, 0u, 0u, 0u, 255u);
        CHECK(covered > 10000u && covered < 60000u);              /* one triangle, not blank or full */
    }
    CHECK(file_size("frame_000001.png") > 100 && is_png("frame_000001.png"));
    CHECK(stats.dumps_written == 1u && stats.dump_failures == 0u);

    /* frame 2: state persisted (program, arrays), the draw list did not */
    stream_builder two = {0};
    build_frame_two(&two);
    push(&two);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.frames_replayed == 2u && stats.draws == 2u); /* one draw each, not 1 then 2 */
    frame = d3d8_swap_replay_last_frame();
    CHECK(frame != NULL && frame->pixels != NULL);
    if (frame != NULL && frame->pixels != NULL) {
        CHECK(pixel_is(frame, 480u, 336u, 255u, 255u, 0u, 255u)); /* draw 2, c[3] reached the device */
        CHECK(pixel_is(frame, 160u, 120u, 0u, 0u, 0u, 255u));     /* draw 1 is NOT in frame 2 */
    }
    CHECK(file_size("frame_000002.png") > 100 && is_png("frame_000002.png"));
    CHECK(stats.dumps_written == 2u);

    /* frame 3: nothing drawn is counted, not replayed, and the last image stays frame 2's */
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.presents == 3u && stats.frames_empty == 1u && stats.frames_replayed == 2u);
    CHECK(file_size("frame_000003.png") == -1);
    CHECK(d3d8_swap_replay_last_frame() == frame);
    CHECK(stats.commands_decoded > 20u);
    stream_free(&one);
    stream_free(&two);
    d3d8_swap_replay_disable();
    CHECK(d3d8_swap_replay_last_frame() == NULL);
    environment_end();
}

static void test_override_and_measured_sizes(const char *selector)
{
    printf("test_override_and_measured_sizes (%s)\n", selector);
    begin_device(true);
    d3d8_swap_replay_config config = base_config(selector);
    config.width = 64u;
    config.height = 64u;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    stream_builder one = {0};
    build_frame_one(&one);
    push(&one);
    present();
    d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.frames_replayed == 1u && stats.last_width == 64u && stats.last_height == 64u);
    const gpu_image *frame = d3d8_swap_replay_last_frame();
    CHECK(frame != NULL && frame->pixels != NULL);
    if (frame != NULL && frame->pixels != NULL) {
        CHECK(frame->width == 64u && frame->height == 64u);
        CHECK(pixel_is(frame, 16u, 12u, 255u, 0u, 0u, 255u));
    }

    /* without the override the bound target decides: a 320x200 header in place of the title's. The
     * hook is called directly because Swap(4) restores the saved target (the title's) first. */
    const d3d8_swap_replay_config measured = base_config(selector);
    CHECK(d3d8_swap_replay_enable(&measured, NULL, 0u));
    store(HEADER_SCRATCH + D3D8_SURFACE_SIZE, (19u << 24) | ((200u - 1u) << 12) | (320u - 1u));
    d3d8_device_store32(0x1A04u, HEADER_SCRATCH);
    stream_builder again = {0};
    build_frame_one(&again);
    push(&again);
    d3d8_swap_replay_on_present(9u);
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.frames_replayed == 1u && stats.last_width == 320u && stats.last_height == 200u);
    frame = d3d8_swap_replay_last_frame();
    CHECK(frame != NULL && frame->pixels != NULL && frame->width == 320u && frame->height == 200u);
    stream_free(&one);
    stream_free(&again);
    d3d8_swap_replay_disable();
    environment_end();
}

/* Non-strict: the unhandled method is logged by number, counted, its state not applied, the frame drawn. */
static void test_non_strict(const char *selector)
{
    printf("test_non_strict (%s)\n", selector);
    begin_device(true);
    d3d8_swap_replay_config config = base_config(selector);
    config.strict = false;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    stream_builder stream = {0};
    build_frame_one(&stream);
    stream_pair(&stream, UNHANDLED_METHOD, 0x12345678u);
    push(&stream);
    capture_clear();
    present();
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.frames_replayed == 1u && !stats.latched);
    CHECK(stats.unhandled_methods == 1u);
    CHECK(captured_has("UNHANDLED method 0x0304"));
    CHECK(d3d8_swap_replay_last_frame() != NULL);
    /* logged once per method, not once per frame */
    capture_clear();
    stream_builder again = {0};
    stream_pair(&again, UNHANDLED_METHOD, 7u);
    push(&again);
    present();
    CHECK(!captured_has("UNHANDLED method 0x0304"));
    stream_free(&again);
    stream_free(&stream);
    d3d8_swap_replay_disable();
    environment_end();
}

/* An inference the caller did not allow refuses the frame instead of being assumed. */
static void test_inference_refusal(const char *selector)
{
    printf("test_inference_refusal (%s)\n", selector);
    begin_device(true);
    d3d8_swap_replay_config config = base_config(selector);
    config.allowed_inferences = 0u;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    stream_builder stream = {0};
    build_frame_one(&stream);
    push(&stream);
    present();
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && stats.frames_replayed == 0u);
    CHECK(strstr(stats.error, "INFERRED") != NULL);
    CHECK(d3d8_swap_replay_last_frame() == NULL);
    stream_free(&stream);
    d3d8_swap_replay_disable();
    environment_end();
}

/* The config's selector wins over $VKRUN_DEVICE, and $VKRUN_DEVICE is used when the config has none. */
static void test_selector(const char *selector, const char *expected_name)
{
    printf("test_selector (%s)\n", selector);
    const char *inherited_text = getenv("VKRUN_DEVICE");
    char inherited[128] = "";
    snprintf(inherited, sizeof inherited, "%s", inherited_text != NULL ? inherited_text : "");
    for (int from_environment = 0; from_environment < 2; from_environment++) {
        begin_device(true);
        d3d8_swap_replay_config config = base_config(from_environment != 0 ? NULL : selector);
        CHECK(setenv("VKRUN_DEVICE", from_environment != 0 ? selector : "no-such-device-xyz", 1) == 0);
        CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
        CHECK(d3d8_swap_replay_device_name() == NULL); /* opened lazily, at the first replayed frame */
        stream_builder stream = {0};
        build_frame_one(&stream);
        push(&stream);
        present();
        CHECK(d3d8_swap_replay_get_stats().frames_replayed == 1u);
        const char *name = d3d8_swap_replay_device_name();
        CHECK(name != NULL && strcmp(name, expected_name) == 0);
        stream_free(&stream);
        d3d8_swap_replay_disable();
        environment_end();
    }
    /* an environment selector that matches nothing is a loud refusal, not another device */
    begin_device(true);
    CHECK(setenv("VKRUN_DEVICE", "no-such-device-xyz", 1) == 0);
    d3d8_swap_replay_config none = base_config(NULL);
    CHECK(d3d8_swap_replay_enable(&none, NULL, 0u));
    stream_builder stream = {0};
    build_frame_one(&stream);
    push(&stream);
    present();
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && strstr(stats.error, "no Vulkan device") != NULL);
    CHECK(d3d8_swap_replay_device_name() == NULL);
    stream_free(&stream);
    d3d8_swap_replay_disable();
    environment_end();
    if (inherited_text != NULL) {
        (void)setenv("VKRUN_DEVICE", inherited, 1);
    } else {
        (void)unsetenv("VKRUN_DEVICE");
    }
}


/* --- T84a1: per-target frames, the tests that need a device -------------------------------- */

static void dump_name(char *out, size_t size, uint64_t frame, uint32_t target)
{
    snprintf(out, size, "frame_%06llu_target_%08X.png", (unsigned long long)frame, (unsigned)target);
}

/* Red triangle into the back buffer (640x480), then a SetRenderTarget, then the yellow one into a
 * 128x64 target, then back. Two images of two sizes, each holding ONLY its own draw. The first part
 * is never kicked before the switch, so the hook's drain is what puts it before the boundary. */
static void test_two_targets(const char *selector)
{
    printf("test_two_targets (%s)\n", selector);
    begin_device(true);
    d3d8_swap_replay_config config = base_config(selector);
    config.dump_directory = dump_directory;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    const uint32_t back = d3d8_device_load32(DEV_RENDER_TARGET);
    make_target(TARGET_A, 128u, 64u);

    stream_builder first = {0};
    build_frame_one(&first);
    push_pending(&first);
    bind_target(TARGET_A);
    stream_builder second = {0};
    build_small_target_draw(&second, 128u, 64u, 3u);
    push_pending(&second);
    bind_target(back);
    CHECK(d3d8_gpu_stream_count() >= first.count + second.count);
    capture_clear();
    present();

    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.presents == 1u && !stats.latched && stats.frames_refused == 0u);
    CHECK(stats.frames_replayed == 1u && stats.passes_replayed == 2u && stats.draws == 2u);
    CHECK(stats.frames_offscreen_only == 0u && stats.frames_empty == 0u);
    CHECK(stats.last_width == 640u && stats.last_height == 480u);
    CHECK(d3d8_gpu_stream_count() == 0u);

    const gpu_image *presented = d3d8_swap_replay_last_frame();
    CHECK(presented != NULL && presented->pixels != NULL);
    if (presented != NULL && presented->pixels != NULL) {
        CHECK(presented->width == 640u && presented->height == 480u);
        CHECK(pixel_is(presented, 160u, 120u, 255u, 0u, 0u, 255u));  /* its own draw */
        CHECK(pixel_is(presented, 480u, 336u, 0u, 0u, 0u, 255u));    /* the texture's draw is NOT here */
        const uint32_t covered = count_not(presented, 0u, 0u, 0u, 255u);
        CHECK(covered > 10000u && covered < 60000u);
    }
    CHECK(d3d8_swap_replay_offscreen_count() == 1u);
    uint32_t target = 0u;
    const gpu_image *offscreen = d3d8_swap_replay_offscreen_frame(0u, &target);
    CHECK(offscreen != NULL && offscreen->pixels != NULL && target == TARGET_A);
    CHECK(d3d8_swap_replay_offscreen_frame(1u, NULL) == NULL);
    if (offscreen != NULL && offscreen->pixels != NULL) {
        CHECK(offscreen->width == 128u && offscreen->height == 64u);
        CHECK(pixel_is(offscreen, 96u, 48u, 255u, 255u, 0u, 255u));  /* c[3] reached the device */
        CHECK(pixel_is(offscreen, 16u, 12u, 0u, 0u, 0u, 255u));      /* the back buffer's draw is NOT here */
        const uint32_t covered = count_not(offscreen, 0u, 0u, 0u, 255u);
        CHECK(covered > 300u && covered < 2500u);
    }
    char name[64];
    CHECK(file_size("frame_000001.png") > 100 && is_png("frame_000001.png"));
    dump_name(name, sizeof name, 1u, TARGET_A);
    CHECK(file_size(name) > 100 && is_png(name));
    CHECK(stats.dumps_written == 2u && stats.dump_failures == 0u);
    stream_free(&first);
    stream_free(&second);

    /* the next frame replaces the offscreen set: a frame with no switch has none */
    stream_builder again = {0};
    build_frame_two(&again);
    push(&again);
    present();
    CHECK(d3d8_swap_replay_get_stats().frames_replayed == 2u);
    CHECK(d3d8_swap_replay_offscreen_count() == 0u);
    stream_free(&again);
    d3d8_swap_replay_disable();
    CHECK(d3d8_swap_replay_offscreen_count() == 0u);
    environment_end();
}

/* Draws into a texture only: the presented target has none, so the frame is offscreen-only. */
static void test_offscreen_only_frame(const char *selector)
{
    printf("test_offscreen_only_frame (%s)\n", selector);
    begin_device(true);
    CHECK(d3d8_swap_replay_enable(&(d3d8_swap_replay_config){
        .spv_directory = spv_directory, .device_selector = selector, .strict = true,
        .allowed_inferences = GPU_PGRAPH_INFER_ALL}, NULL, 0u));
    const uint32_t back = d3d8_device_load32(DEV_RENDER_TARGET);
    make_target(TARGET_A, 96u, 48u);
    bind_target(TARGET_A);
    stream_builder stream = {0};
    build_frame_one(&stream);
    build_small_target_draw(&stream, 96u, 48u, 0u);
    push(&stream);
    bind_target(back);
    present();
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_offscreen_only == 1u && stats.frames_replayed == 0u);
    CHECK(stats.passes_replayed == 1u && stats.draws == 2u && stats.last_width == 0u);
    CHECK(d3d8_swap_replay_last_frame() == NULL);
    CHECK(d3d8_swap_replay_offscreen_count() == 1u);
    const gpu_image *offscreen = d3d8_swap_replay_offscreen_frame(0u, NULL);
    CHECK(offscreen != NULL && offscreen->pixels != NULL && offscreen->width == 96u &&
          offscreen->height == 48u);
    if (offscreen != NULL && offscreen->pixels != NULL) {
        CHECK(pixel_is(offscreen, 24u, 12u, 255u, 255u, 0u, 255u)); /* both draws, one pass, one target */
    }
    stream_free(&stream);
    d3d8_swap_replay_disable();
    environment_end();
}

/* SetRenderTarget to the target already bound (a null argument, or the same header) is no switch:
 * the draws before and after it stay in one pass. */
static void test_same_target_is_one_pass(const char *selector)
{
    printf("test_same_target_is_one_pass (%s)\n", selector);
    begin_device(true);
    CHECK(d3d8_swap_replay_enable(&(d3d8_swap_replay_config){
        .spv_directory = spv_directory, .device_selector = selector, .strict = true,
        .allowed_inferences = GPU_PGRAPH_INFER_ALL}, NULL, 0u));
    const uint32_t back = d3d8_device_load32(DEV_RENDER_TARGET);
    stream_builder first = {0};
    build_frame_one(&first);
    push(&first);
    set_target(0u, d3d8_device_load32(DEV_DEPTH_SURFACE));
    bind_target(back);
    stream_builder second = {0};
    build_frame_two(&second);
    push(&second);
    present();
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.passes_replayed == 1u);
    CHECK(stats.draws == 2u && d3d8_swap_replay_offscreen_count() == 0u);
    const gpu_image *frame = d3d8_swap_replay_last_frame();
    CHECK(frame != NULL && frame->pixels != NULL);
    if (frame != NULL && frame->pixels != NULL) {
        CHECK(pixel_is(frame, 160u, 120u, 255u, 0u, 0u, 255u));
        CHECK(pixel_is(frame, 480u, 336u, 255u, 255u, 0u, 255u));
    }
    stream_free(&first);
    stream_free(&second);
    d3d8_swap_replay_disable();
    environment_end();
}

/* Back buffer, texture, back buffer again, with draws in both back buffer stretches: the replay
 * cannot draw onto an image that exists, so it refuses instead of replaying the second stretch alone. */
static void test_target_drawn_twice_refuses(const char *selector)
{
    printf("test_target_drawn_twice_refuses (%s)\n", selector);
    begin_device(true);
    CHECK(d3d8_swap_replay_enable(&(d3d8_swap_replay_config){
        .spv_directory = spv_directory, .device_selector = selector, .strict = true,
        .allowed_inferences = GPU_PGRAPH_INFER_ALL}, NULL, 0u));
    const uint32_t back = d3d8_device_load32(DEV_RENDER_TARGET);
    make_target(TARGET_A, 128u, 64u);
    stream_builder first = {0};
    build_frame_one(&first);
    push(&first);
    bind_target(TARGET_A);
    stream_builder second = {0};
    build_small_target_draw(&second, 128u, 64u, 3u);
    push(&second);
    bind_target(back);
    stream_builder third = {0};
    build_frame_two(&third);
    push(&third);
    capture_clear();
    present();
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && stats.frames_refused == 1u && stats.frames_replayed == 0u);
    CHECK(strstr(stats.error, "two separate passes") != NULL);
    CHECK(captured_has("two separate passes"));
    CHECK(d3d8_swap_replay_last_frame() == NULL && d3d8_swap_replay_offscreen_count() == 0u);
    stream_free(&first);
    stream_free(&second);
    stream_free(&third);
    d3d8_swap_replay_disable();
    environment_end();
}

/* An explicit size cannot describe two targets. */
static void test_override_with_two_targets_refuses(const char *selector)
{
    printf("test_override_with_two_targets_refuses (%s)\n", selector);
    begin_device(true);
    d3d8_swap_replay_config config = base_config(selector);
    config.width = 64u;
    config.height = 64u;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    const uint32_t back = d3d8_device_load32(DEV_RENDER_TARGET);
    make_target(TARGET_A, 128u, 64u);
    stream_builder first = {0};
    build_frame_one(&first);
    push(&first);
    bind_target(TARGET_A);
    stream_builder second = {0};
    build_small_target_draw(&second, 128u, 64u, 3u);
    push(&second);
    bind_target(back);
    present();
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && strstr(stats.error, "explicit 64x64 size cannot be applied") != NULL);
    stream_free(&first);
    stream_free(&second);
    d3d8_swap_replay_disable();
    environment_end();
}

/* Sizes are measured when the target is bound: a header reused after the switch does not change them. */
static void test_size_measured_at_the_switch(const char *selector)
{
    printf("test_size_measured_at_the_switch (%s)\n", selector);
    begin_device(true);
    CHECK(d3d8_swap_replay_enable(&(d3d8_swap_replay_config){
        .spv_directory = spv_directory, .device_selector = selector, .strict = true,
        .allowed_inferences = GPU_PGRAPH_INFER_ALL}, NULL, 0u));
    const uint32_t back = d3d8_device_load32(DEV_RENDER_TARGET);
    make_target(TARGET_A, 128u, 64u);
    bind_target(TARGET_A);
    stream_builder stream = {0};
    build_frame_one(&stream);
    build_small_target_draw(&stream, 128u, 64u, 3u);
    push(&stream);
    bind_target(back);
    store(TARGET_A + D3D8_SURFACE_SIZE, ((7u) << 12) | 7u); /* the header is now 8x8 */
    present();
    const gpu_image *offscreen = d3d8_swap_replay_offscreen_frame(0u, NULL);
    CHECK(offscreen != NULL && offscreen->pixels != NULL && offscreen->width == 128u &&
          offscreen->height == 64u);
    stream_free(&stream);
    d3d8_swap_replay_disable();
    environment_end();
}

/* T84a3, on the device: after a reset the program, arrays and constants of the old model are gone. */
static void test_reset_forgets_decoder_state(const char *selector)
{
    printf("test_reset_forgets_decoder_state (%s)\n", selector);
    begin_device(true);
    d3d8_swap_replay_config config = base_config(selector);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    stream_builder one = {0};
    build_frame_one(&one);
    push(&one);
    present();
    CHECK(d3d8_swap_replay_get_stats().frames_replayed == 1u);
    d3d8_gpu_reset();
    stream_builder two = {0};
    build_frame_two(&two); /* a draw that relies on the program the OLD model held */
    push(&two);
    present();
    d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.model_resets == 1u);
    CHECK(stats.frames_replayed == 1u && stats.draws == 1u); /* NOT drawn from the stale model */
    CHECK(stats.latched && stats.frames_refused == 1u);
    stream_free(&two);

    /* the same run again, but the new device sets its state up first: it replays with no re-enable */
    d3d8_gpu_reset();
    stream_builder again = {0};
    build_frame_one(&again);
    build_frame_two(&again);
    push(&again);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.model_resets == 2u && !stats.latched);
    CHECK(stats.frames_replayed == 2u && stats.draws == 3u && stats.frames_refused == 1u);
    const gpu_image *frame = d3d8_swap_replay_last_frame();
    CHECK(frame != NULL && frame->pixels != NULL);
    if (frame != NULL && frame->pixels != NULL) {
        CHECK(pixel_is(frame, 480u, 336u, 255u, 255u, 0u, 255u));
    }
    stream_free(&one);
    stream_free(&again);
    d3d8_swap_replay_disable();
    environment_end();
}

/* The frame of build_frame_one drawn with or without flip_y: the same rows, in reverse order. */
/* After a reset the FIRST event may be a render target switch: it belongs to the new model. */
static void test_reset_then_switch(const char *selector)
{
    printf("test_reset_then_switch (%s)\n", selector);
    begin_device(true);
    d3d8_swap_replay_config config = base_config(selector);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    d3d8_gpu_reset();
    const uint32_t back = d3d8_device_load32(DEV_RENDER_TARGET);
    make_target(TARGET_A, 128u, 64u);
    stream_builder first = {0};
    build_frame_one(&first);
    push_pending(&first);
    bind_target(TARGET_A);
    stream_builder second = {0};
    build_small_target_draw(&second, 128u, 64u, 3u);
    push_pending(&second);
    bind_target(back);
    present();
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.model_resets == 1u && !stats.latched);
    CHECK(stats.passes_replayed == 2u && stats.draws == 2u && stats.frames_replayed == 1u);
    CHECK(d3d8_swap_replay_offscreen_count() == 1u);
    stream_free(&first);
    stream_free(&second);
    d3d8_swap_replay_disable();
    environment_end();
}

/* A switch that is the FIRST thing after a d3d8_gpu_reset, with nothing in the ring to follow the reset
 * for it, must still be recorded in the rebuilt model's list: the next kick would otherwise discard it as
 * the old model's, and the size measured when it happened (the header changes afterwards) is lost.
 * T259b: gpu-swap-switch-follows-reset. */
static void test_reset_then_empty_ring_switch(const char *selector)
{
    printf("test_reset_then_empty_ring_switch (%s)\n", selector);
    begin_device(true);
    d3d8_swap_replay_config config = base_config(selector);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    d3d8_gpu_reset();
    const uint32_t back = d3d8_device_load32(DEV_RENDER_TARGET);
    make_target(TARGET_A, 128u, 64u);
    bind_target(TARGET_A);                                 /* empty ring: nothing else follows the reset */
    store(TARGET_A + D3D8_SURFACE_SIZE, ((7u) << 12) | 7u); /* the header is 8x8 from now on */
    stream_builder stream = {0};
    build_frame_one(&stream);
    build_small_target_draw(&stream, 128u, 64u, 3u);
    push(&stream);
    bind_target(back);
    present();
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.model_resets == 1u && !stats.latched && stats.passes_replayed == 1u);
    const gpu_image *offscreen = d3d8_swap_replay_offscreen_frame(0u, NULL);
    CHECK(offscreen != NULL && offscreen->pixels != NULL && offscreen->width == 128u &&
          offscreen->height == 64u);
    stream_free(&stream);
    d3d8_swap_replay_disable();
    environment_end();
}

static void test_flip_y_option(const char *selector)
{
    printf("test_flip_y_option (%s)\n", selector);
    uint8_t *plain = NULL;
    uint32_t width = 0u;
    uint32_t height = 0u;
    size_t stride = 0u;
    for (int flipped = 0; flipped < 2; flipped++) {
        begin_device(true);
        d3d8_swap_replay_config config = base_config(selector);
        config.flip_y = flipped != 0;
        CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
        stream_builder one = {0};
        build_frame_one(&one);
        push(&one);
        present();
        const gpu_image *frame = d3d8_swap_replay_last_frame();
        CHECK(frame != NULL && frame->pixels != NULL);
        if (frame != NULL && frame->pixels != NULL && flipped == 0) {
            width = frame->width;
            height = frame->height;
            stride = frame->stride_bytes;
            plain = malloc(stride * height);
            CHECK(plain != NULL);
            if (plain != NULL) {
                memcpy(plain, frame->pixels, stride * height);
            }
            CHECK(pixel_is(frame, 160u, 120u, 255u, 0u, 0u, 255u));
            CHECK(pixel_is(frame, 160u, 359u, 0u, 0u, 0u, 255u));
        } else if (frame != NULL && frame->pixels != NULL && plain != NULL) {
            CHECK(frame->width == width && frame->height == height && frame->stride_bytes == stride);
            CHECK(pixel_is(frame, 160u, 359u, 255u, 0u, 0u, 255u)); /* draw 1 moved to the bottom */
            CHECK(pixel_is(frame, 160u, 120u, 0u, 0u, 0u, 255u));
            uint32_t differing_rows = 0u;
            for (uint32_t y = 0u; y < height; y++) {
                CHECK(memcmp(frame->pixels + (size_t)y * stride, plain + (size_t)(height - 1u - y) * stride,
                             (size_t)width * 4u) == 0);
                differing_rows += memcmp(frame->pixels + (size_t)y * stride, plain + (size_t)y * stride,
                                         (size_t)width * 4u) != 0;
            }
            CHECK(differing_rows > 100u); /* the frame is not symmetric, the flip changed it */
        }
        stream_free(&one);
        d3d8_swap_replay_disable();
        environment_end();
    }
    free(plain);
}

/* T560: the config's window_clip_modules reaches the backend. A frame with no viewport scale anywhere replays
 * without the statement and is REFUSED by name with it (the modules would convert with a zero scale), while the
 * frame that carries its scale replays either way. */
static void test_window_clip_option(const char *selector)
{
    printf("test_window_clip_option (%s)\n", selector);
    for (int stated = 0; stated < 2; stated++) {
        for (int with_scale = 0; with_scale < 2; with_scale++) {
            begin_device(true);
            d3d8_swap_replay_config config = base_config(selector);
            config.window_clip_modules = stated != 0;
            CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
            stream_builder one = {0};
            if (with_scale != 0) {
                build_frame_one(&one);
            } else {
                static const float c3_zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                stream_pair(&one, GPU_PGRAPH_EXECUTION_MODE, 6u);
                stream_program(&one, 0u, synthetic_program, 1u);
                stream_pair(&one, GPU_PGRAPH_PROGRAM_START, 0u);
                stream_constants(&one, 3u, c3_zero, 4u);
                stream_array(&one, 1u, MEMORY_BASE, array_format(28u, 3u, GPU_PGRAPH_TYPE_F));
                stream_array(&one, 2u, MEMORY_BASE + 12u, array_format(28u, 4u, GPU_PGRAPH_TYPE_F));
                stream_draw_arrays(&one, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
            }
            push(&one);
            present();
            const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
            if (stated != 0 && with_scale == 0) {
                CHECK(stats.latched && stats.frames_refused == 1u && stats.frames_replayed == 0u);
                CHECK(strstr(stats.error, "window-to-clip") != NULL);
            } else {
                CHECK(!stats.latched && stats.frames_replayed == 1u);
            }
            stream_free(&one);
            d3d8_swap_replay_disable();
            environment_end();
        }
    }
}

/* T84b/T84a3: line_width 0 is the inferred behaviour, 1.0 is the same frame stated, no inference. */
static void test_line_width_option(const char *selector)
{
    printf("test_line_width_option (%s)\n", selector);
    uint32_t covered[2] = {0u, 0u};
    uint32_t used[2] = {0u, 0u};
    for (int stated = 0; stated < 2; stated++) {
        begin_device(true);
        d3d8_swap_replay_config config = base_config(selector);
        config.line_width = stated != 0 ? 1.0f : 0.0f;
        CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
        stream_builder lines = {0};
        build_frame_one(&lines);
        stream_draw_arrays(&lines, GPU_PGRAPH_OP_LINES, 0u, 2u);
        push(&lines);
        present();
        const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
        CHECK(stats.frames_replayed == 1u && !stats.latched && stats.draws == 2u);
        used[stated] = stats.used_inferences;
        const gpu_image *frame = d3d8_swap_replay_last_frame();
        CHECK(frame != NULL && frame->pixels != NULL);
        if (frame != NULL && frame->pixels != NULL) {
            covered[stated] = count_not(frame, 0u, 0u, 0u, 255u);
        }
        stream_free(&lines);
        d3d8_swap_replay_disable();
        environment_end();
    }
    CHECK((used[0] & GPU_PGRAPH_INFER_LINE_WIDTH) != 0u);   /* unstated: the inference is used */
    CHECK((used[1] & GPU_PGRAPH_INFER_LINE_WIDTH) == 0u);   /* stated 1.0: nothing inferred */
    CHECK(covered[0] > 10000u && covered[0] == covered[1]); /* the same frame (triangle plus the line) */

    /* with the inference not allowed the unstated width refuses and the stated one still draws */
    for (int stated = 0; stated < 2; stated++) {
        begin_device(true);
        d3d8_swap_replay_config config = base_config(selector);
        config.line_width = stated != 0 ? 1.0f : 0.0f;
        config.allowed_inferences = GPU_PGRAPH_INFER_ALL & ~GPU_PGRAPH_INFER_LINE_WIDTH;
        CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
        stream_builder lines = {0};
        build_frame_one(&lines);
        stream_draw_arrays(&lines, GPU_PGRAPH_OP_LINES, 0u, 2u);
        push(&lines);
        present();
        const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
        if (stated == 0) {
            CHECK(stats.latched && stats.frames_replayed == 0u && strstr(stats.error, "line_width") != NULL);
        } else {
            CHECK(!stats.latched && stats.frames_replayed == 1u);
        }
        stream_free(&lines);
        d3d8_swap_replay_disable();
        environment_end();
    }
}

/* --- T262, no device: the kick decodes, the budget refuses ------------------------------------ */

static void test_vertex_budget_refuses_the_frame(void)
{
    printf("test_vertex_budget_refuses_the_frame\n");
    begin_device(true);
    d3d8_swap_replay_config config = base_config(NULL);
    config.vertex_budget_bytes = 100u; /* a draw needs 84, two need 168 */
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    CHECK(d3d8_swap_replay_get_config().vertex_budget_bytes == 100u);
    stream_builder one = {0};
    build_frame_one(&one);
    push(&one);
    stream_builder two = {0};
    build_redraw(&two);
    push(&two); /* the second draw goes past the budget AT THE KICK, the refusal is reported at the present */
    d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.presents == 0u && !stats.latched && stats.frames_refused == 0u);
    capture_clear();
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.presents == 1u && stats.frames_refused == 1u && stats.latched);
    CHECK(stats.frames_replayed == 0u && stats.draws == 0u);
    CHECK(strstr(stats.error, "a bound was reached") != NULL);
    CHECK(strstr(stats.error, "vertex snapshot budget of 100 bytes") != NULL);
    CHECK(strstr(stats.error, "84 are held") != NULL);
    CHECK(captured_has("vertex snapshot budget of 100 bytes"));
    CHECK(captured_has("STOPPED"));
    CHECK(d3d8_swap_replay_last_frame() == NULL);
    /* the replay is latched, later frames are skipped and say so */
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.frames_skipped == 1u && stats.frames_refused == 1u);
    stream_free(&one);
    stream_free(&two);
    d3d8_swap_replay_disable();
    environment_end();
}

/* The default config snapshots, a hand-built config does not, and the decode happens at the kick. */
static void test_vertex_snapshot_config_and_kick_decode(void)
{
    printf("test_vertex_snapshot_config_and_kick_decode\n");
    CHECK(d3d8_swap_replay_default_config().vertex_budget_bytes ==
          D3D8_SWAP_REPLAY_DEFAULT_VERTEX_BUDGET);
    CHECK(D3D8_SWAP_REPLAY_DEFAULT_VERTEX_BUDGET == 64u * 1024u * 1024u);
    for (int snapshot = 0; snapshot < 2; snapshot++) {
        begin_device(true);
        d3d8_swap_replay_config config = base_config(NULL);
        config.vertex_budget_bytes = snapshot != 0 ? 1u << 20 : 0u;
        CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
        stream_builder one = {0};
        build_frame_one(&one);
        push_pending(&one);
        CHECK(d3d8_swap_replay_get_stats().commands_decoded == 0u); /* in the ring, not yet kicked */
        d3d8_gpu_kick();
        d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
        CHECK(stats.commands_decoded == one.count);                  /* decoded at the kick */
        CHECK(stats.vertex_bytes_captured == (snapshot != 0 ? 84u : 0u));
        CHECK(stats.draws_snapshotted == (snapshot != 0 ? 1u : 0u));
        CHECK(stats.presents == 0u);
        stream_free(&one);
        d3d8_swap_replay_disable();
        /* disabled: the observer is removed and a kick decodes nothing */
        stream_builder again = {0};
        build_frame_one(&again);
        push(&again);
        CHECK(d3d8_swap_replay_get_stats().commands_decoded == 0u);
        stream_free(&again);
        environment_end();
    }
}

/* The model rebuilt after a d3d8_gpu_reset (CreateDevice) is configured like the first one: the strict
 * flag and the vertex snapshot both come from the config, not from the model's own defaults (a fresh
 * model is lenient and does not snapshot). T259b: gpu-swap-reset-strict-flag. */
static void test_reset_reconfigures_the_model(void)
{
    printf("test_reset_reconfigures_the_model\n");
    begin_device(true);
    d3d8_swap_replay_config config = base_config(NULL);
    CHECK(config.strict && config.vertex_budget_bytes != 0u);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    d3d8_gpu_reset();
    stream_builder draw = {0};
    build_frame_one(&draw);
    push_pending(&draw);
    d3d8_gpu_kick(); /* the first call into the replay after the reset rebuilds the model */
    d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.model_resets == 1u);
    CHECK(stats.vertex_bytes_captured == 84u && stats.draws_snapshotted == 1u);
    stream_builder unhandled = {0};
    stream_pair(&unhandled, UNHANDLED_METHOD, 1u);
    push_pending(&unhandled);
    d3d8_gpu_kick();
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && stats.frames_refused == 1u); /* strict: an unmodelled method refuses the frame */
    CHECK(strstr(stats.error, "strict mode") != NULL);
    stream_free(&draw);
    stream_free(&unhandled);
    d3d8_swap_replay_disable();
    environment_end();
}

/* Commands recorded BEFORE the replay was enabled have had no kick since: they are decoded at the
 * present (the only case where the present is when guest memory is read), counted, and not lost. */
static void test_commands_before_enable_decode_at_the_present(void)
{
    printf("test_commands_before_enable_decode_at_the_present\n");
    begin_device(true);
    stream_builder state_only = {0};
    static const float c3_blue[4] = {0.0f, 0.0f, 1.0f, 0.0f};
    stream_constants(&state_only, 3u, c3_blue, 4u);
    push(&state_only);
    const size_t recorded = d3d8_gpu_stream_count();
    CHECK(recorded >= state_only.count && state_only.count > 3u);
    d3d8_swap_replay_config config = base_config(NULL);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    CHECK(d3d8_swap_replay_get_stats().commands_decoded == 0u); /* enabling decodes nothing */
    d3d8_swap_replay_on_present(1u);
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.commands_decoded == recorded && stats.frames_empty == 1u && !stats.latched);
    CHECK(d3d8_gpu_stream_count() == 0u);
    stream_free(&state_only);
    d3d8_swap_replay_disable();
    environment_end();
}

/* --- T259: what the first mutation sweep over d3d8_swap_replay.c found unpinned ------------------
 * tools/mutate/sets/d3d8_swap_replay.py. The tests without a selector need no device. */

static void make_file(const char *path, const void *bytes, size_t count)
{
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    if (file != NULL) {
        if (count != 0u) {
            CHECK(fwrite(bytes, 1u, count, file) == count);
        }
        fclose(file);
    }
}

/* enable() validates the directory argument, the dump directory and the module listing. */
static void test_enable_validation(void)
{
    printf("test_enable_validation\n");
    char error[300];
    d3d8_swap_replay_config config = base_config(NULL);
    config.spv_directory = "";
    CHECK(!d3d8_swap_replay_enable(&config, error, sizeof error));
    CHECK(strstr(error, "is required") != NULL); /* named for what it is, not a failed opendir("") */

    /* a dump path that already exists as a FILE is refused, as one that exists as a directory is not */
    char path[400];
    snprintf(path, sizeof path, "%s/dump_is_a_file", spv_directory);
    make_file(path, "x", 1u);
    config = base_config(NULL);
    config.dump_directory = path;
    CHECK(!d3d8_swap_replay_enable(&config, error, sizeof error));
    CHECK(strstr(error, "not a directory") != NULL && !d3d8_swap_replay_enabled());
    (void)unlink(path);
    snprintf(path, sizeof path, "%s/dump_twice", spv_directory);
    config.dump_directory = path;
    CHECK(d3d8_swap_replay_enable(&config, error, sizeof error));
    CHECK(d3d8_swap_replay_enable(&config, error, sizeof error)); /* the directory exists now */
    d3d8_swap_replay_disable();
    (void)rmdir(path);

    /* the module directory lists `*.spv` files with a non-empty stem and nothing else */
    char only_empty_stem[400];
    char only_other[400];
    char mixed[400];
    snprintf(only_empty_stem, sizeof only_empty_stem, "%s/only_empty_stem", spv_directory);
    snprintf(only_other, sizeof only_other, "%s/only_other", spv_directory);
    snprintf(mixed, sizeof mixed, "%s/mixed", spv_directory);
    CHECK(mkdir(only_empty_stem, 0777) == 0 && mkdir(only_other, 0777) == 0 && mkdir(mixed, 0777) == 0);
    snprintf(path, sizeof path, "%s/.spv", only_empty_stem);
    make_file(path, "abcd", 4u);
    snprintf(path, sizeof path, "%s/notes.txt", only_other);
    make_file(path, "abcdefgh", 8u);
    snprintf(path, sizeof path, "%s/notes.txt", mixed);
    make_file(path, "abcdefgh", 8u);
    snprintf(path, sizeof path, "%s/one.spv", mixed);
    make_file(path, "abcd", 4u);
    config = base_config(NULL);
    config.spv_directory = only_empty_stem;
    CHECK(!d3d8_swap_replay_enable(&config, error, sizeof error) && strstr(error, "no .spv module") != NULL);
    config.spv_directory = only_other;
    CHECK(!d3d8_swap_replay_enable(&config, error, sizeof error) && strstr(error, "no .spv module") != NULL);
    config.spv_directory = mixed;
    CHECK(d3d8_swap_replay_enable(&config, error, sizeof error));
    d3d8_swap_replay_disable();

    /* the device selector is held and reported, and absent when none was given */
    config = base_config("software");
    CHECK(d3d8_swap_replay_enable(&config, error, sizeof error));
    CHECK(d3d8_swap_replay_get_config().device_selector != NULL &&
          strcmp(d3d8_swap_replay_get_config().device_selector, "software") == 0);
    CHECK(d3d8_swap_replay_get_config().device_selector != config.device_selector);
    config = base_config(NULL);
    CHECK(d3d8_swap_replay_enable(&config, error, sizeof error));
    CHECK(d3d8_swap_replay_get_config().device_selector == NULL);
    d3d8_swap_replay_disable();
}

/* The switch log must be in recording order: a switch BEFORE its predecessor means the recording was
 * released or reset in between. Both switches are inside the recording at the present, so only the
 * order check can refuse it (test_released_recording_refuses trips the end-of-recording check first). */
static void test_switch_order_refuses(void)
{
    printf("test_switch_order_refuses\n");
    begin_device(true);
    CHECK(d3d8_swap_replay_enable(&(d3d8_swap_replay_config){
        .spv_directory = spv_directory, .strict = true, .allowed_inferences = GPU_PGRAPH_INFER_ALL},
        NULL, 0u));
    const uint32_t back = d3d8_device_load32(DEV_RENDER_TARGET);
    make_target(TARGET_A, 128u, 64u);
    stream_builder stream = {0};
    build_frame_one(&stream);
    push_pending(&stream);
    bind_target(TARGET_A);                              /* switch 0 at the end of the first commands */
    d3d8_gpu_stream_discard(d3d8_gpu_stream_count());   /* the recording is released ... */
    bind_target(back);                                  /* ... so switch 1 is at index 0, before switch 0 */
    push(&stream);                                      /* and the recording is as long as it was again */
    d3d8_swap_replay_on_present(1u);
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && strstr(stats.error, "released or reset") != NULL);
    CHECK(strstr(stats.error, "switch 1") != NULL);
    stream_free(&stream);
    d3d8_swap_replay_disable();
    environment_end();
}

/* A switch overflow belongs to the recording it was noted in: a reset forgets it. */
static void test_reset_forgets_switch_overflow(void)
{
    printf("test_reset_forgets_switch_overflow\n");
    begin_device(true);
    CHECK(d3d8_swap_replay_enable(&(d3d8_swap_replay_config){
        .spv_directory = spv_directory, .strict = true, .allowed_inferences = GPU_PGRAPH_INFER_ALL},
        NULL, 0u));
    make_target(TARGET_A, 128u, 64u);
    const uint32_t back = d3d8_device_load32(DEV_RENDER_TARGET);
    for (uint32_t i = 0u; i < 33u; i++) {
        bind_target(TARGET_A);
        bind_target(back);
    }
    d3d8_gpu_reset();
    d3d8_swap_replay_on_present(1u);
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.model_resets == 1u && !stats.latched && stats.frames_refused == 0u);
    CHECK(stats.frames_empty == 1u);
    d3d8_swap_replay_disable();
    environment_end();
}

/* A module file that is the right name but not a module: a size that is not whole words, and a
 * wrong magic. Both are refused as "could not be loaded", not handed to the device. */
static void test_corrupt_module_refuses(const char *selector)
{
    printf("test_corrupt_module_refuses (%s)\n", selector);
    char directory[400];
    snprintf(directory, sizeof directory, "%s/corrupt_%s", spv_directory, selector);
    CHECK(mkdir(directory, 0777) == 0);
    char path[500];
    snprintf(path, sizeof path, "%s/%s.spv", directory, SYNTHETIC_PROGRAM_NAME);
    for (int variant = 0; variant < 2; variant++) {
        uint32_t words[sizeof draw_vertex_words / sizeof(uint32_t) + 1u];
        memcpy(words, draw_vertex_words, sizeof draw_vertex_words);
        words[sizeof draw_vertex_words / sizeof(uint32_t)] = 0u;
        size_t bytes = sizeof draw_vertex_words;
        if (variant == 0) {
            bytes += 2u; /* valid magic, two bytes past a whole number of words */
        } else {
            words[0] ^= 1u; /* a whole number of words, no magic */
        }
        make_file(path, words, bytes);
        begin_device(true);
        d3d8_swap_replay_config config = base_config(selector);
        config.spv_directory = directory;
        CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
        stream_builder stream = {0};
        build_frame_one(&stream);
        push(&stream);
        capture_clear();
        present();
        const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
        CHECK(stats.latched && stats.frames_refused == 1u && stats.frames_replayed == 0u);
        CHECK(strstr(stats.error, "could not be loaded") != NULL);
        CHECK(d3d8_swap_replay_last_frame() == NULL);
        stream_free(&stream);
        d3d8_swap_replay_disable();
        environment_end();
    }
    (void)unlink(path);
    (void)rmdir(directory);
}

/* Three targets in one frame: the back buffer, then A, then B, then the back buffer again with no
 * draw. Each segment must be attributed to ITS target and size, the third one to B and not to A. */
static void test_three_targets(const char *selector)
{
    printf("test_three_targets (%s)\n", selector);
    begin_device(true);
    d3d8_swap_replay_config config = base_config(selector);
    config.dump_directory = dump_directory;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    const uint32_t back = d3d8_device_load32(DEV_RENDER_TARGET);
    make_target(TARGET_A, 128u, 64u);
    make_target(TARGET_B, 96u, 48u);
    stream_builder first = {0};
    build_frame_one(&first);
    push_pending(&first);
    bind_target(TARGET_A);
    stream_builder second = {0};
    build_small_target_draw(&second, 128u, 64u, 3u);
    push_pending(&second);
    bind_target(TARGET_B);
    stream_builder third = {0};
    build_small_target_draw(&third, 96u, 48u, 3u);
    push_pending(&third);
    bind_target(back);
    capture_clear();
    present();
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_refused == 0u);
    CHECK(stats.frames_replayed == 1u && stats.passes_replayed == 3u && stats.draws == 3u);
    CHECK(stats.last_width == 640u && stats.last_height == 480u);
    CHECK(d3d8_swap_replay_offscreen_count() == 2u);
    uint32_t target = 0u;
    const gpu_image *a = d3d8_swap_replay_offscreen_frame(0u, &target);
    CHECK(a != NULL && a->pixels != NULL && target == TARGET_A);
    if (a != NULL && a->pixels != NULL) {
        CHECK(a->width == 128u && a->height == 64u);
        CHECK(pixel_is(a, 96u, 48u, 255u, 255u, 0u, 255u));
    }
    const gpu_image *b = d3d8_swap_replay_offscreen_frame(1u, &target);
    CHECK(b != NULL && b->pixels != NULL && target == TARGET_B);
    if (b != NULL && b->pixels != NULL) {
        CHECK(b->width == 96u && b->height == 48u);
        CHECK(pixel_is(b, 72u, 36u, 255u, 255u, 0u, 255u));
    }
    char name[64];
    dump_name(name, sizeof name, 1u, TARGET_B);
    CHECK(file_size(name) > 100 && is_png(name));
    CHECK(stats.dumps_written == 3u);
    stream_free(&first);
    stream_free(&second);
    stream_free(&third);
    d3d8_swap_replay_disable();
    environment_end();
}

/* A dump that cannot be written is counted, logged, and does not lose the frame. */
static void test_dump_failure_is_counted(const char *selector)
{
    printf("test_dump_failure_is_counted (%s)\n", selector);
    begin_device(true);
    d3d8_swap_replay_config config = base_config(selector);
    config.dump_directory = dump_directory;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    remove_tree(dump_directory); /* enable created it, now it is gone */
    stream_builder stream = {0};
    build_frame_one(&stream);
    push(&stream);
    capture_clear();
    present();
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_replayed == 1u);
    CHECK(stats.dumps_written == 0u && stats.dump_failures == 1u);
    CHECK(captured_has("cannot write"));
    CHECK(d3d8_swap_replay_last_frame() != NULL);
    stream_free(&stream);
    d3d8_swap_replay_disable();
    environment_end();
}

/* T267: the scissor the library's SetScissors writes, through the whole swap replay. The window clip pair
 * covers the 640 x 480 target (fields[0] and fields[1] of the device), the surface clip is the rectangle. */
static void push_scissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    stream_builder stream = {0};
    stream_pair(&stream, 0x0200u, x | (width << 16));
    stream_pair(&stream, 0x0204u, y | (height << 16));
    stream_pair(&stream, 0x02B4u, 0u);
    stream_pair(&stream, 0x02C0u, 640u << 16);
    stream_pair(&stream, 0x02E0u, 480u << 16);
    push(&stream);
    stream_free(&stream);
}

static void test_output_state_scissor(const char *selector)
{
    printf("test_output_state_scissor (%s)\n", selector);
    /* OFF (the default): the scissor methods are unhandled, strict refuses the frame at the first one,
     * exactly as before T267 */
    begin_device(true);
    d3d8_swap_replay_config config = base_config(selector);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    push_scissor(100u, 60u, 120u, 100u);
    stream_builder one = {0};
    build_frame_one(&one);
    push(&one);
    present();
    d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && stats.frames_refused == 1u && stats.frames_replayed == 0u);
    CHECK(strstr(stats.error, "0x0200") != NULL && strstr(stats.error, "strict mode") != NULL);
    d3d8_swap_replay_disable();
    environment_end();

    /* ON: the same stream replays, clipped to the rectangle, and the report says what it rests on */
    begin_device(true);
    config = base_config(selector);
    config.output_groups = GPU_PGRAPH_OUTPUT_SCISSOR;
    config.allowed_inferences = GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_OUTPUT_ALL;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    push_scissor(100u, 60u, 120u, 100u);
    push(&one);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.draws == 1u);
    CHECK((stats.used_inferences & GPU_PGRAPH_INFER_OUTPUT_SCISSOR) != 0u);
    const gpu_image *frame = d3d8_swap_replay_last_frame();
    CHECK(frame != NULL && frame->pixels != NULL);
    if (frame != NULL && frame->pixels != NULL) {
        CHECK(pixel_is(frame, 160u, 120u, 255u, 0u, 0u, 255u)); /* inside the triangle and the rectangle */
        CHECK(pixel_is(frame, 100u, 60u, 255u, 0u, 0u, 255u));  /* the rectangle's first pixel */
        CHECK(pixel_is(frame, 219u, 100u, 255u, 0u, 0u, 255u)); /* and its last column */
        CHECK(pixel_is(frame, 60u, 40u, 0u, 0u, 0u, 255u));     /* in the triangle, above the rectangle */
        CHECK(pixel_is(frame, 99u, 120u, 0u, 0u, 0u, 255u));    /* left of it */
        CHECK(pixel_is(frame, 220u, 120u, 0u, 0u, 0u, 255u));   /* right of it */
        CHECK(pixel_is(frame, 160u, 160u, 0u, 0u, 0u, 255u));   /* below it */
        uint32_t outside = 0u;
        for (uint32_t y = 0u; y < frame->height; y++) {
            for (uint32_t x = 0u; x < frame->width; x++) {
                const bool inside = x >= 100u && x < 220u && y >= 60u && y < 160u;
                outside += !inside && !pixel_is(frame, x, y, 0u, 0u, 0u, 255u);
            }
        }
        CHECK(outside == 0u);
        CHECK(count_not(frame, 0u, 0u, 0u, 255u) > 5000u); /* not empty: the rectangle is mostly covered */
    }

    /* the rebuilt model keeps the opt-in: after CreateDevice's reset the next scissor still decodes */
    d3d8_gpu_reset();
    push_scissor(10u, 10u, 50u, 50u);
    push(&one);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.model_resets == 1u && !stats.latched);
    d3d8_swap_replay_disable();
    environment_end();

    /* the inference is a gate here too: a scissor with the bit not allowed latches, naming it */
    begin_device(true);
    config = base_config(selector);
    config.output_groups = GPU_PGRAPH_OUTPUT_SCISSOR;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    push_scissor(100u, 60u, 120u, 100u);
    push(&one);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && stats.frames_refused == 1u);
    CHECK(strstr(stats.error, "INFERRED and not allowed") != NULL);
    d3d8_swap_replay_disable();
    stream_free(&one);
    environment_end();
}

/* T267: blend and alpha test through the whole swap replay. Additive blending of the red triangle over the
 * black clear is the red triangle, and an alpha test the fragment fails (alpha 255 is not LESS than 200)
 * leaves the frame as the clear, so both the pass and the fail are on the path. */
static void push_blend_alpha(uint32_t blend_enable, uint32_t alpha_function)
{
    stream_builder stream = {0};
    stream_pair(&stream, 0x0304u, blend_enable);
    stream_pair(&stream, 0x0348u, 1u);
    stream_pair(&stream, 0x0344u, 1u);
    stream_pair(&stream, 0x0350u, 0x8006u);
    stream_pair(&stream, 0x033Cu, alpha_function);
    stream_pair(&stream, 0x0340u, 200u);
    stream_pair(&stream, 0x0300u, 1u);
    push(&stream);
    stream_free(&stream);
}

static void test_output_state_blend_alpha(const char *selector)
{
    printf("test_output_state_blend_alpha (%s)\n", selector);
    begin_device(true);
    d3d8_swap_replay_config config = base_config(selector);
    config.output_groups = GPU_PGRAPH_OUTPUT_BLEND | GPU_PGRAPH_OUTPUT_ALPHA_TEST;
    config.allowed_inferences = GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_OUTPUT_ALL;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    push_blend_alpha(1u, 0x0204u); /* GREATER 200: alpha 255 passes */
    stream_builder one = {0};
    build_frame_one(&one);
    push(&one);
    present();
    d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_replayed == 1u);
    CHECK((stats.used_inferences & GPU_PGRAPH_INFER_OUTPUT_BLEND_MODEL) != 0u);
    CHECK((stats.used_inferences & GPU_PGRAPH_INFER_OUTPUT_ALPHA_TEST_MODEL) != 0u);
    const gpu_image *frame = d3d8_swap_replay_last_frame();
    CHECK(frame != NULL && frame->pixels != NULL);
    if (frame != NULL && frame->pixels != NULL) {
        CHECK(pixel_is(frame, 160u, 120u, 255u, 0u, 0u, 255u));
        CHECK(pixel_is(frame, 480u, 336u, 0u, 0u, 0u, 255u));
        const uint32_t covered = count_not(frame, 0u, 0u, 0u, 255u);
        CHECK(covered > 10000u && covered < 60000u);
    }
    /* the next frame: LESS 200 fails the same fragment, and the frame is the clear */
    push_blend_alpha(1u, 0x0201u);
    stream_builder again = {0};
    stream_array(&again, 1u, MEMORY_BASE, array_format(28u, 3u, GPU_PGRAPH_TYPE_F));
    stream_draw_arrays(&again, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    push(&again);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_replayed == 2u);
    frame = d3d8_swap_replay_last_frame();
    CHECK(frame != NULL && frame->pixels != NULL);
    if (frame != NULL && frame->pixels != NULL) {
        CHECK(count_not(frame, 0u, 0u, 0u, 255u) == 0u);
    }
    stream_free(&one);
    stream_free(&again);
    d3d8_swap_replay_disable();
    environment_end();

    /* without the groups the same pairs are unhandled and strict refuses the frame, as before */
    begin_device(true);
    config = base_config(selector);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    push_blend_alpha(1u, 0x0204u);
    stream_builder refused = {0};
    build_frame_one(&refused);
    push(&refused);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && strstr(stats.error, "0x0304") != NULL);
    stream_free(&refused);
    d3d8_swap_replay_disable();
    environment_end();
}

/* T267: depth through the whole swap replay: the same triangle drawn twice at the same depth, the second in
 * green. Under LESS the second fails the depth test (0.5 is not less than 0.5) and the frame stays red, with the
 * depth test written off the second wins. */
static void test_output_state_depth(const char *selector)
{
    printf("test_output_state_depth (%s)\n", selector);
    for (uint32_t tested = 0u; tested < 2u; tested++) {
        begin_device(true);
        d3d8_swap_replay_config config = base_config(selector);
        config.output_groups = GPU_PGRAPH_OUTPUT_DEPTH_STENCIL;
        config.allowed_inferences = GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_OUTPUT_ALL;
        CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
        stream_builder depth = {0};
        stream_pair(&depth, 0x030Cu, tested);
        stream_pair(&depth, 0x0354u, 0x0201u);
        stream_pair(&depth, 0x035Cu, 1u);
        push(&depth);
        stream_builder one = {0};
        build_frame_one(&one);
        push(&one);
        stream_builder second = {0};
        static const float c3_green[4] = {-1.0f, 1.0f, 0.0f, 0.0f};
        stream_constants(&second, 3u, c3_green, 4u);
        stream_draw_arrays(&second, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
        push(&second);
        present();
        const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
        CHECK(!stats.latched && stats.frames_replayed == 1u && stats.draws == 2u);
        CHECK(((stats.used_inferences & GPU_PGRAPH_INFER_OUTPUT_DEPTH_MODEL) != 0u) == (tested == 1u));
        const gpu_image *frame = d3d8_swap_replay_last_frame();
        CHECK(frame != NULL && frame->pixels != NULL);
        if (frame != NULL && frame->pixels != NULL) {
            if (tested == 1u) {
                CHECK(pixel_is(frame, 160u, 120u, 255u, 0u, 0u, 255u)); /* the first stays: the second failed */
            } else {
                CHECK(pixel_is(frame, 160u, 120u, 0u, 255u, 0u, 255u)); /* no depth test: the second wins */
            }
            CHECK(pixel_is(frame, 480u, 336u, 0u, 0u, 0u, 255u));
        }
        stream_free(&depth);
        stream_free(&one);
        stream_free(&second);
        d3d8_swap_replay_disable();
        environment_end();
    }
}

/* T267: a clear through the whole swap replay: blue over the whole 640 x 480 target, then the red triangle, then a
 * green clear of a small rectangle after the last draw. */
static void test_output_state_clear(const char *selector)
{
    printf("test_output_state_clear (%s)\n", selector);
    begin_device(true);
    d3d8_swap_replay_config config = base_config(selector);
    config.output_groups = GPU_PGRAPH_OUTPUT_CLEAR;
    config.allowed_inferences = GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_OUTPUT_ALL;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    stream_builder clear = {0};
    stream_pair(&clear, 0x1D98u, 0u | (639u << 16));
    stream_pair(&clear, 0x1D9Cu, 0u | (479u << 16));
    stream_pair(&clear, 0x1D8Cu, 0u);
    stream_pair(&clear, 0x1D90u, 0xFF0000FFu);
    stream_pair(&clear, GPU_PGRAPH_CLEAR_SURFACE, 0xF0u);
    push(&clear);
    stream_builder one = {0};
    build_frame_one(&one);
    push(&one);
    stream_builder after = {0};
    stream_pair(&after, 0x1D98u, 100u | (109u << 16));
    stream_pair(&after, 0x1D9Cu, 100u | (109u << 16));
    stream_pair(&after, 0x1D8Cu, 0u);
    stream_pair(&after, 0x1D90u, 0xFF00FF00u);
    stream_pair(&after, GPU_PGRAPH_CLEAR_SURFACE, 0xF0u);
    push(&after);
    present();
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.draws == 1u);
    CHECK((stats.used_inferences & GPU_PGRAPH_INFER_OUTPUT_CLEAR_MODEL) != 0u);
    CHECK(stats.clears_not_replayed == 0u); /* both clears of the frame sit in a pass that has a draw */
    const gpu_image *frame = d3d8_swap_replay_last_frame();
    CHECK(frame != NULL && frame->pixels != NULL);
    if (frame != NULL && frame->pixels != NULL) {
        CHECK(pixel_is(frame, 480u, 336u, 0u, 0u, 255u, 255u)); /* the clear, where nothing was drawn */
        CHECK(pixel_is(frame, 160u, 120u, 255u, 0u, 0u, 255u)); /* the triangle over the clear */
        CHECK(pixel_is(frame, 100u, 100u, 0u, 255u, 0u, 255u)); /* the clear after the last draw, over the triangle */
        CHECK(pixel_is(frame, 109u, 109u, 0u, 255u, 0u, 255u) && pixel_is(frame, 110u, 110u, 255u, 0u, 0u, 255u));
        CHECK(pixel_is(frame, 639u, 0u, 0u, 0u, 255u, 255u));
    }
    stream_free(&clear);
    stream_free(&one);
    stream_free(&after);
    d3d8_swap_replay_disable();
    environment_end();
}

/* --- T511: the polygon offset and IGNORED counters ----------------------------------------- */

#define OFFSET_BIAS 1000000.0f /* 1e6 smallest depth steps: about 0.06 of depth near 0.5, far above any rounding */
#define OFFSET_METHOD_BIAS 0x0388u
#define OFFSET_METHOD_SCALE 0x0384u
#define OFFSET_METHOD_FILL 0x0338u
#define IGNORED_SPECULAR_METHOD 0x09F8u
#define IGNORED_DITHER_METHOD 0x0310u

static void stream_offset_words(stream_builder *stream, float scale, float bias, uint32_t fill)
{
    stream_pair(stream, OFFSET_METHOD_BIAS, float_bits_of(bias));
    stream_pair(stream, OFFSET_METHOD_SCALE, float_bits_of(scale));
    stream_pair(stream, OFFSET_METHOD_FILL, fill);
}

static void stream_depth_words(stream_builder *stream, uint32_t enable)
{
    stream_pair(stream, 0x030Cu, enable);
    stream_pair(stream, 0x0354u, 0x0201u); /* LESS: an equal depth fails unless the bias lifts it */
    stream_pair(stream, 0x035Cu, 1u);
}

/* The same triangle in green (the program adds c3 to v2.zyxw = red, so (-1, 1, 0, 0) gives green), drawn again. */
static void build_green_redraw(stream_builder *stream)
{
    static const float c3_green[4] = {-1.0f, 1.0f, 0.0f, 0.0f};
    stream_constants(stream, 3u, c3_green, 4u);
    stream_draw_arrays(stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
}

static d3d8_swap_replay_config offset_config(const char *selector, uint32_t groups)
{
    d3d8_swap_replay_config config = base_config(selector);
    config.output_groups = groups;
    config.allowed_inferences = GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_OUTPUT_ALL;
    return config;
}

#define OFFSET_GROUPS (GPU_PGRAPH_OUTPUT_DEPTH_STENCIL | GPU_PGRAPH_OUTPUT_POLYGON_OFFSET)

/* The IGNORED group's pairs (dither, 09F8) are decoded at the kick: counted with no device and no draw, kept over a
 * d3d8_gpu_reset, never counted with the group off. */
static void test_pairs_ignored_are_counted(void)
{
    printf("test_pairs_ignored_are_counted\n");
    begin_device(true);
    d3d8_swap_replay_config config = offset_config(NULL, GPU_PGRAPH_OUTPUT_IGNORED);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    CHECK(d3d8_swap_replay_get_stats().pairs_ignored == 0u);
    stream_builder both = {0};
    stream_pair(&both, IGNORED_SPECULAR_METHOD, 3u);
    stream_pair(&both, IGNORED_DITHER_METHOD, 1u);
    push(&both);
    d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.pairs_ignored == 2u); /* seen at the kick, before any present */
    CHECK(stats.offset_applied == 0u && stats.offset_unobserved == 0u && stats.unhandled_methods == 0u);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_empty == 1u && stats.pairs_ignored == 2u); /* the fold at the present adds nothing */
    stream_builder one = {0};
    stream_pair(&one, IGNORED_SPECULAR_METHOD, 0xFu);
    push(&one);
    CHECK(d3d8_swap_replay_get_stats().pairs_ignored == 3u);

    /* a reset builds a new model, whose own counter starts at 0: the total keeps what the old one counted */
    d3d8_gpu_reset();
    stream_builder after = {0};
    stream_pair(&after, IGNORED_DITHER_METHOD, 0u);
    push(&after);
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.model_resets == 1u && stats.pairs_ignored == 4u);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.pairs_ignored == 4u);
    d3d8_swap_replay_disable();
    environment_end();
    stream_free(&both);
    stream_free(&one);
    stream_free(&after);

    /* the group off (non-strict, so the words do not refuse the frame): nothing is ignored on purpose, both are
     * unhandled methods */
    begin_device(true);
    config = offset_config(NULL, 0u);
    config.strict = false;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    stream_builder off = {0};
    stream_pair(&off, IGNORED_SPECULAR_METHOD, 3u);
    stream_pair(&off, IGNORED_DITHER_METHOD, 1u);
    push(&off);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.pairs_ignored == 0u && stats.unhandled_methods == 2u);
    stream_free(&off);
    d3d8_swap_replay_disable();
    environment_end();
}

/* A bias towards the viewer lifts the second draw of an equal depth over LESS: the replay applied it. Pixels say the
 * bias reached the device, the counters say how many draws it touched. */
static void test_polygon_offset_counters(const char *selector)
{
    printf("test_polygon_offset_counters (%s)\n", selector);

    /* APPLIED: the first draw runs before the offset is written, the second with it (green over red) */
    begin_device(true);
    d3d8_swap_replay_config config = offset_config(selector, OFFSET_GROUPS);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    stream_builder first = {0};
    stream_depth_words(&first, 1u);
    build_frame_one(&first);
    push(&first);
    stream_builder second = {0};
    stream_offset_words(&second, 0.0f, -OFFSET_BIAS, 1u);
    build_green_redraw(&second);
    push(&second);
    capture_clear();
    present();
    d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.draws == 2u);
    CHECK(stats.offset_applied == 1u && stats.offset_unobserved == 0u && stats.pairs_ignored == 0u);
    CHECK((stats.used_inferences & GPU_PGRAPH_INFER_OUTPUT_POLYGON_OFFSET_MODEL) != 0u);
    CHECK(captured_has("1 draw(s) ran with a polygon offset") && captured_has("0 draw(s) had an enabled non-zero offset"));
    const gpu_image *frame = d3d8_swap_replay_last_frame();
    CHECK(frame != NULL && frame->pixels != NULL);
    if (frame != NULL && frame->pixels != NULL) {
        CHECK(pixel_is(frame, 160u, 120u, 0u, 255u, 0u, 255u)); /* the biased draw passed LESS at an equal depth */
    }
    /* a second frame keeps the state (the offset stays on), draws twice more: the totals add up, never overwrite */
    stream_builder again = {0};
    stream_draw_arrays(&again, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    stream_draw_arrays(&again, GPU_PGRAPH_OP_TRIANGLES, 3u, 3u);
    push(&again);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_replayed == 2u && stats.draws == 4u);
    CHECK(stats.offset_applied == 3u && stats.offset_unobserved == 0u);
    stream_free(&first);
    stream_free(&second);
    stream_free(&again);
    d3d8_swap_replay_disable();
    environment_end();

    /* UNOBSERVED: an enabled non-zero offset and nothing tests depth, both draws are counted and none applied */
    begin_device(true);
    config = offset_config(selector, OFFSET_GROUPS);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    stream_builder blind = {0};
    stream_depth_words(&blind, 0u);
    stream_offset_words(&blind, -0.25f, -OFFSET_BIAS, 1u);
    build_frame_one(&blind);
    build_green_redraw(&blind);
    push(&blind);
    capture_clear();
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.draws == 2u);
    CHECK(stats.offset_unobserved == 2u && stats.offset_applied == 0u);
    CHECK(captured_has("0 draw(s) ran with a polygon offset") && captured_has("2 draw(s) had an enabled non-zero offset"));
    frame = d3d8_swap_replay_last_frame();
    CHECK(frame != NULL && frame->pixels != NULL);
    if (frame != NULL && frame->pixels != NULL) {
        CHECK(pixel_is(frame, 160u, 120u, 0u, 255u, 0u, 255u)); /* no depth test: the second draw wins as it would anyway */
    }
    stream_free(&blind);
    d3d8_swap_replay_disable();
    environment_end();

    /* LINE (T552): the backend never biases a line list, so a line draw with the offset on is UNOBSERVED and never
     * applied, while the triangle drawn with the same offset in the same frame is applied. The log says so per pass */
    begin_device(true);
    config = offset_config(selector, OFFSET_GROUPS);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    stream_builder lines = {0};
    stream_depth_words(&lines, 1u);
    stream_offset_words(&lines, 0.0f, -OFFSET_BIAS, 1u);
    build_frame_one(&lines);
    stream_draw_arrays(&lines, GPU_PGRAPH_OP_LINES, 0u, 2u);
    push(&lines);
    capture_clear();
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.draws == 2u);
    CHECK(stats.offset_applied == 1u && stats.offset_unobserved == 1u);
    CHECK(captured_has("1 draw(s) ran with a polygon offset") && captured_has("1 draw(s) had an enabled non-zero offset"));
    stream_free(&lines);
    d3d8_swap_replay_disable();
    environment_end();

    /* ZERO: the title's own state (an enable of 0, or an enabled offset of -0.0 and 0.0) is no offset and says nothing */
    for (uint32_t zero_enabled = 0u; zero_enabled < 2u; zero_enabled++) {
        begin_device(true);
        config = offset_config(selector, OFFSET_GROUPS);
        CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
        stream_builder zero = {0};
        stream_depth_words(&zero, 1u);
        stream_offset_words(&zero, 0.0f, zero_enabled == 1u ? -0.0f : -OFFSET_BIAS, zero_enabled);
        build_frame_one(&zero);
        build_green_redraw(&zero);
        push(&zero);
        capture_clear();
        present();
        stats = d3d8_swap_replay_get_stats();
        CHECK(!stats.latched && stats.frames_replayed == 1u && stats.draws == 2u);
        CHECK(stats.offset_applied == 0u && stats.offset_unobserved == 0u);
        CHECK(!captured_has("polygon offset"));
        frame = d3d8_swap_replay_last_frame();
        CHECK(frame != NULL && frame->pixels != NULL);
        if (frame != NULL && frame->pixels != NULL) {
            CHECK(pixel_is(frame, 160u, 120u, 255u, 0u, 0u, 255u)); /* equal depth fails LESS: the first stays */
        }
        stream_free(&zero);
        d3d8_swap_replay_disable();
        environment_end();
    }

    /* GROUP OFF: the same stream, non-strict, applies and counts nothing and leaves the three words unhandled */
    begin_device(true);
    config = offset_config(selector, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL);
    config.strict = false;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    stream_builder off = {0};
    stream_depth_words(&off, 1u);
    stream_offset_words(&off, 0.0f, -OFFSET_BIAS, 1u);
    build_frame_one(&off);
    build_green_redraw(&off);
    push(&off);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.draws == 2u);
    CHECK(stats.offset_applied == 0u && stats.offset_unobserved == 0u && stats.unhandled_methods == 3u);
    stream_free(&off);
    d3d8_swap_replay_disable();
    environment_end();

    /* TWO PASSES: the counters add over the passes of a frame (one draw in each target), one log line per pass. With a
     * depth test they are applied, without one unobserved, and in both cases each pass has its own line */
    for (uint32_t no_depth = 0u; no_depth < 2u; no_depth++) {
        begin_device(true);
        config = offset_config(selector, OFFSET_GROUPS);
        CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
        const uint32_t back = d3d8_device_load32(DEV_RENDER_TARGET);
        make_target(TARGET_A, 128u, 64u);
        stream_builder on_back = {0};
        stream_depth_words(&on_back, no_depth == 0u ? 1u : 0u);
        stream_offset_words(&on_back, 0.0f, -OFFSET_BIAS, 1u);
        build_frame_one(&on_back);
        push_pending(&on_back);
        bind_target(TARGET_A);
        stream_builder on_target = {0};
        build_small_target_draw(&on_target, 128u, 64u, 3u);
        push_pending(&on_target);
        bind_target(back);
        capture_clear();
        present();
        stats = d3d8_swap_replay_get_stats();
        CHECK(!stats.latched && stats.passes_replayed == 2u && stats.draws == 2u);
        CHECK(stats.offset_applied == (no_depth == 0u ? 2u : 0u) && stats.offset_unobserved == (no_depth == 0u ? 0u : 2u));
        const uint32_t targets[2] = {back, TARGET_A};
        for (uint32_t i = 0u; i < 2u; i++) {
            char expected[200];
            snprintf(expected, sizeof expected, "render target 0x%08X: %u draw(s) ran with a polygon offset (Vulkan depth bias), %u draw(s) had",
                     (unsigned)targets[i], no_depth == 0u ? 1u : 0u, no_depth == 0u ? 0u : 1u);
            CHECK(captured_has(expected));
        }
        stream_free(&on_back);
        stream_free(&on_target);
        d3d8_swap_replay_disable();
        environment_end();
    }
}

static void push_clear_pairs(stream_builder *stream, uint32_t colour, uint32_t x_max, uint32_t y_max)
{
    stream_pair(stream, 0x1D98u, 0u | (x_max << 16));
    stream_pair(stream, 0x1D9Cu, 0u | (y_max << 16));
    stream_pair(stream, 0x1D8Cu, 0u);
    stream_pair(stream, 0x1D90u, colour);
    stream_pair(stream, GPU_PGRAPH_CLEAR_SURFACE, 0xF0u);
}

/* T267: a clear in a render target pass that has no draw has no image to land in. It must NOT be given to the pass
 * before it (another target), and it is counted and logged, not dropped silently. */
static void test_clear_in_a_pass_with_no_draw(const char *selector)
{
    printf("test_clear_in_a_pass_with_no_draw (%s)\n", selector);
    begin_device(true);
    d3d8_swap_replay_config config = base_config(selector);
    config.output_groups = GPU_PGRAPH_OUTPUT_CLEAR;
    config.allowed_inferences = GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_OUTPUT_ALL;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    const uint32_t back = d3d8_device_load32(DEV_RENDER_TARGET);
    make_target(TARGET_A, 128u, 64u);

    /* draws into the back buffer, then a switch to a 128 x 64 target, which is cleared and never drawn into */
    stream_builder first = {0};
    build_frame_one(&first);
    push_pending(&first);
    bind_target(TARGET_A);
    stream_builder clear = {0};
    push_clear_pairs(&clear, 0xFF0000FFu, 127u, 63u);
    push_pending(&clear);
    present();
    d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.passes_replayed == 1u && stats.draws == 1u);
    CHECK(stats.frames_offscreen_only == 1u && stats.clears_not_replayed == 1u);
    const gpu_image *offscreen = d3d8_swap_replay_offscreen_frame(0u, NULL);
    CHECK(offscreen != NULL && offscreen->pixels != NULL && offscreen->width == 640u);
    if (offscreen != NULL && offscreen->pixels != NULL) {
        CHECK(pixel_is(offscreen, 160u, 120u, 255u, 0u, 0u, 255u)); /* the triangle */
        CHECK(pixel_is(offscreen, 10u, 10u, 0u, 0u, 0u, 255u));     /* the other target's clear is NOT here */
        CHECK(count_not(offscreen, 0u, 0u, 0u, 255u) < 60000u);
    }
    stream_free(&first);
    stream_free(&clear);

    /* a frame with only a clear has no image at all: counted empty, the clear counted as not replayed */
    bind_target(back);
    stream_builder only = {0};
    push_clear_pairs(&only, 0xFF0000FFu, 639u, 479u);
    push(&only);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_empty == 1u && stats.clears_not_replayed == 2u);
    stream_free(&only);

    /* a clear in a pass that has a draw is applied, and counts as replayed */
    stream_builder drawn = {0};
    push_clear_pairs(&drawn, 0xFF0000FFu, 639u, 479u);
    build_frame_two(&drawn);
    push(&drawn);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.clears_not_replayed == 2u);
    const gpu_image *frame = d3d8_swap_replay_last_frame();
    CHECK(frame != NULL && frame->pixels != NULL);
    if (frame != NULL && frame->pixels != NULL) {
        CHECK(pixel_is(frame, 10u, 10u, 0u, 0u, 255u, 255u)); /* the clear, where nothing was drawn */
    }
    stream_free(&drawn);

    /* each clear lands on the target that was bound when it was written: A is cleared green and drawn into, then the
     * back buffer is cleared blue after A's pass and drawn into */
    bind_target(TARGET_A);
    stream_builder to_a = {0};
    push_clear_pairs(&to_a, 0xFF00FF00u, 127u, 63u);
    build_small_target_draw(&to_a, 128u, 64u, 3u);
    push_pending(&to_a);
    bind_target(back);
    stream_builder to_back = {0};
    stream_pair(&to_back, 0x1D98u, 200u | (639u << 16)); /* not the top left corner: A's clear (128 x 64) must not show */
    stream_pair(&to_back, 0x1D9Cu, 0u | (479u << 16));
    stream_pair(&to_back, 0x1D8Cu, 0u);
    stream_pair(&to_back, 0x1D90u, 0xFF0000FFu);
    stream_pair(&to_back, GPU_PGRAPH_CLEAR_SURFACE, 0xF0u);
    build_frame_two(&to_back);
    push_pending(&to_back);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_replayed == 2u && stats.passes_replayed == 4u);
    CHECK(stats.clears_not_replayed == 2u);
    frame = d3d8_swap_replay_last_frame();
    CHECK(frame != NULL && frame->pixels != NULL && frame->width == 640u);
    if (frame != NULL && frame->pixels != NULL) {
        CHECK(pixel_is(frame, 300u, 10u, 0u, 0u, 255u, 255u)); /* the back buffer's own clear, blue */
        CHECK(pixel_is(frame, 10u, 10u, 0u, 0u, 0u, 255u));    /* A's clear (green) is NOT here, nor the back buffer's */
        CHECK(pixel_is(frame, 480u, 336u, 255u, 255u, 0u, 255u)); /* and its draw */
    }
    offscreen = d3d8_swap_replay_offscreen_frame(0u, NULL);
    CHECK(d3d8_swap_replay_offscreen_count() == 1u);
    CHECK(offscreen != NULL && offscreen->pixels != NULL && offscreen->width == 128u && offscreen->height == 64u);
    if (offscreen != NULL && offscreen->pixels != NULL) {
        CHECK(pixel_is(offscreen, 4u, 4u, 0u, 255u, 0u, 255u)); /* A's clear, green, not the back buffer's blue */
        CHECK(pixel_is(offscreen, 96u, 48u, 255u, 255u, 0u, 255u)); /* A's draw over it */
    }
    stream_free(&to_a);
    stream_free(&to_back);
    d3d8_swap_replay_disable();
    environment_end();
}

/* --- T478: the combiner option ------------------------------------------------------------- */

typedef struct {
    uint32_t index;
    uint32_t value;
} combiner_entry;

/* The fixtures of test_gpu_combiner.c: the same pixel shader definitions, by index. */
static const combiner_entry pass_entries[] = {{34u, 0xC4200000u}, {0u, 0xD4300000u}, {45u, 0xC0u},
                                              {26u, 0xC0u},       {53u, 0x11101u},   {8u, 0x0Cu},
                                              {9u, 0x1C80u}};
static const combiner_entry multiply_entries[] = {{34u, 0x01020000u}, {0u, 0xD4300000u}, {45u, 0xC0u},
                                                  {26u, 0xC0u},       {53u, 0x11101u},   {8u, 0x0Cu},
                                                  {9u, 0x1C80u},      {10u, 0x00FF8040u}, {18u, 0x0080C0FFu}};
static const combiner_entry final_entries[] = {{53u, 0x11101u}, {8u, 0x0Fu}, {9u, 0x01021180u},
                                               {43u, 0x80FF4020u}, {44u, 0x00808080u}};

static void build_combiner_words(const combiner_entry *entries, size_t count,
                                 uint32_t words[GPU_PGRAPH_COMBINER_WORDS])
{
    memset(words, 0, GPU_PGRAPH_COMBINER_WORDS * sizeof words[0]);
    for (size_t i = 0u; i < count; i++) {
        words[entries[i].index] = entries[i].value;
    }
}

/* The three fixtures' module names are the ones the C key computes: a table that lists the files this
 * test wrote and the one it did not, so a name that drifted fails here and not as a mystery refusal. */
static void test_combiner_fixture_names(void)
{
    printf("test_combiner_fixture_names\n");
    static const struct {
        const char *name;
        const combiner_entry *entries;
        size_t count;
    } fixtures[] = {
        {COMBINER_NAME_PASS, pass_entries, sizeof pass_entries / sizeof pass_entries[0]},
        {COMBINER_NAME_MULTIPLY, multiply_entries, sizeof multiply_entries / sizeof multiply_entries[0]},
        {COMBINER_NAME_FINAL, final_entries, sizeof final_entries / sizeof final_entries[0]},
    };
    static const gpu_combiner_texture no_textures[GPU_COMBINER_TEXTURE_STAGES] = {{NULL, 0u, 0u, false, false, NULL, false, false}};
    CHECK(sizeof fixtures / sizeof fixtures[0] == 3u);
    CHECK(sizeof combiner_words_final > 0u && sizeof combiner_words_twovary > 0u);
    for (size_t i = 0u; i < sizeof fixtures / sizeof fixtures[0]; i++) {
        uint32_t words[GPU_PGRAPH_COMBINER_WORDS];
        build_combiner_words(fixtures[i].entries, fixtures[i].count, words);
        gpu_pgraph *pgraph = gpu_pgraph_create();
        gpu_pgraph_set_combiner(pgraph, true);
        stream_builder stream = {0};
        stream_pixel_shader(&stream, words);
        CHECK(stream.count > 0u);
        CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
        const gpu_pgraph_state *state = gpu_pgraph_state_now(pgraph);
        gpu_combiner_plan plan;
        char error[200] = "";
        const uint32_t everything = GPU_COMBINER_INFER_ALL;
        const gpu_pgraph_result planned =
            gpu_combiner_plan_build(state, everything, no_textures, &plan, error, sizeof error);
        if (planned != GPU_PGRAPH_OK) {
            printf("  plan refused: %s\n", error);
        }
        CHECK(planned == GPU_PGRAPH_OK);
        CHECK(strcmp(plan.name, fixtures[i].name) == 0);
        stream_free(&stream);
        gpu_pgraph_destroy(pgraph);
    }
}

/* One frame of the red triangle with the pixel shader definition `entries` written first. */
static void push_combiner_frame(const combiner_entry *entries, size_t count)
{
    uint32_t words[GPU_PGRAPH_COMBINER_WORDS];
    build_combiner_words(entries, count, words);
    stream_builder stream = {0};
    stream_pixel_shader(&stream, words);
    build_frame_one(&stream);
    push(&stream);
    stream_free(&stream);
}

#define MULTIPLY_COUNT (sizeof multiply_entries / sizeof multiply_entries[0])

static d3d8_swap_replay_config combiner_config(const char *selector)
{
    d3d8_swap_replay_config config = base_config(selector);
    config.combiner = true;
    config.allowed_inferences = GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_COMBINER_REPLAY;
    return config;
}

/* No device needed: what the model does with the 57 combiner words is decided at the kick. With the option the
 * words are decoded (strict mode, nothing unhandled), without it they are counted as unhandled, and the option
 * survives CreateDevice's reset. */
static void test_combiner_words_are_decoded_only_with_the_option(void)
{
    printf("test_combiner_words_are_decoded_only_with_the_option\n");
    begin_device(true);
    d3d8_swap_replay_config config = combiner_config(NULL);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    push_combiner_frame(multiply_entries, MULTIPLY_COUNT);
    d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.commands_decoded > 50u); /* the kick decoded the whole stream, the 57 words among it */
    CHECK(stats.unhandled_methods == 0u);
    const uint64_t decoded_before = stats.commands_decoded;
    d3d8_gpu_reset();
    present(); /* the reset is followed here: a rebuilt model that lost the option would count the words */
    push_combiner_frame(multiply_entries, MULTIPLY_COUNT);
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.model_resets == 1u && !stats.latched);
    CHECK(stats.commands_decoded - decoded_before > 50u); /* the second stream, into the rebuilt model */
    CHECK(stats.unhandled_methods == 0u);
    d3d8_swap_replay_disable();
    environment_end();

    begin_device(true);
    config = base_config(NULL);
    config.strict = false;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    push_combiner_frame(multiply_entries, MULTIPLY_COUNT);
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.commands_decoded > 50u);
    CHECK(stats.unhandled_methods >= 40u); /* the words, counted and not applied */
    d3d8_swap_replay_disable();
    environment_end();
}

/* main.c holds no inference mask of its own: each host flag adds exactly its bits to INFER_ALL (0x7F), and every host adds CMP_PACKED (T1204). */
static void test_host_inferences(void)
{
    printf("test_host_inferences\n");
    CHECK(d3d8_swap_replay_host_inferences(false, false, false, false, false) == ((GPU_PGRAPH_INFER_ALL) | GPU_PGRAPH_INFER_CMP_PACKED));
    CHECK(d3d8_swap_replay_host_inferences(false, false, false, false, false) == ((0x7Fu) | GPU_PGRAPH_INFER_CMP_PACKED));
    CHECK(d3d8_swap_replay_host_inferences(true, false, false, false, false) == (((0x7Fu | GPU_PGRAPH_INFER_EXECUTION_MODE_UNWRITTEN)) | GPU_PGRAPH_INFER_CMP_PACKED));
    CHECK(d3d8_swap_replay_host_inferences(true, false, false, false, false) == ((0xFFu) | GPU_PGRAPH_INFER_CMP_PACKED));
    CHECK(d3d8_swap_replay_host_inferences(false, true, false, false, false) == (((0x7Fu | GPU_PGRAPH_INFER_OUTPUT_ALL | GPU_PGRAPH_INFER_OUTPUT_BLIT_PLANNER_RULES)) | GPU_PGRAPH_INFER_CMP_PACKED));
    CHECK(d3d8_swap_replay_host_inferences(false, true, false, false, false) == ((0x2FFFE07Fu) | GPU_PGRAPH_INFER_CMP_PACKED)); /* T832 planner rules 0x20000000, */ /* T502 polygon offset inferences, T462 pinned state, immediate vertex, unwritten varying */
    CHECK(d3d8_swap_replay_host_inferences(false, false, true, false, false) == (((0x7Fu | 0xC00u)) | GPU_PGRAPH_INFER_CMP_PACKED));
    CHECK(d3d8_swap_replay_host_inferences(false, false, true, false, false) == ((0xC7Fu) | GPU_PGRAPH_INFER_CMP_PACKED));
    CHECK(d3d8_swap_replay_host_inferences(true, true, true, false, false) == (((0xFFu | 0xFFFE000u | 0xC00u | 0x20000000u)) | GPU_PGRAPH_INFER_CMP_PACKED));
    CHECK(d3d8_swap_replay_host_inferences(false, false, false, true, false) ==
          (((GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_VIEWPORT_FROM_TARGET)) | GPU_PGRAPH_INFER_CMP_PACKED));
    /* T497: the stand-in texture adds exactly TEXTURE_SAMPLING (0x1000), and none of the other combiner inferences, so
     * a stand-in never lets STAGE_PROGRAM (a stage program the stream never wrote), UNWRITTEN, INITIAL_STATE,
     * CONSTANT_BYTES or COLOUR_RANGE in */
    CHECK(D3D8_SWAP_REPLAY_INFER_STANDIN_TEXTURE == 0x1000u);
    CHECK((D3D8_SWAP_REPLAY_INFER_STANDIN_TEXTURE & GPU_PGRAPH_INFER_COMBINER_STAGE_PROGRAM) == 0u);
    CHECK((D3D8_SWAP_REPLAY_INFER_STANDIN_TEXTURE & GPU_PGRAPH_INFER_ALL) == 0u);
    CHECK((D3D8_SWAP_REPLAY_INFER_STANDIN_TEXTURE & GPU_PGRAPH_INFER_COMBINER_REPLAY) == 0u);
    CHECK(d3d8_swap_replay_host_inferences(false, false, false, false, true) == (((0x7Fu | 0x1000u)) | GPU_PGRAPH_INFER_CMP_PACKED));
    CHECK(d3d8_swap_replay_host_inferences(false, false, true, false, true) == (((0x7Fu | 0xC00u | 0x1000u)) | GPU_PGRAPH_INFER_CMP_PACKED));
    CHECK(d3d8_swap_replay_host_inferences(false, false, true, false, true) == ((0x1C7Fu) | GPU_PGRAPH_INFER_CMP_PACKED));
    CHECK(d3d8_swap_replay_host_inferences(true, true, true, true, true) ==
          (((0xFFu | 0xFFFE000u | 0xC00u | 0x20000000u | 0x1000u | GPU_PGRAPH_INFER_VIEWPORT_FROM_TARGET)) | GPU_PGRAPH_INFER_CMP_PACKED));
}

static void test_combiner_config(void)
{
    printf("test_combiner_config\n");
    char error[300];
    /* OFF by default, in the default config and in one a caller builds by hand */
    CHECK(!d3d8_swap_replay_default_config().combiner && !base_config(NULL).combiner);
    d3d8_swap_replay_config config = base_config(NULL);
    CHECK(d3d8_swap_replay_enable(&config, error, sizeof error));
    CHECK(!d3d8_swap_replay_get_config().combiner);
    config = combiner_config(NULL);
    CHECK(d3d8_swap_replay_enable(&config, error, sizeof error));
    CHECK(d3d8_swap_replay_get_config().combiner);
    CHECK(d3d8_swap_replay_get_config().allowed_inferences == config.allowed_inferences);
    d3d8_swap_replay_disable();
    CHECK(!d3d8_swap_replay_get_config().combiner);

    /* the opt-in names the inferences it rests on, and none of them is part of INFER_ALL */
    CHECK(GPU_PGRAPH_INFER_COMBINER_REPLAY == 0xC00u); /* CONSTANT_BYTES and COLOUR_RANGE, nothing else */
    CHECK((GPU_PGRAPH_INFER_COMBINER_REPLAY & GPU_PGRAPH_INFER_ALL) == 0u);
    CHECK((GPU_PGRAPH_INFER_COMBINER_REPLAY & ~GPU_PGRAPH_INFER_COMBINER_ALL) == 0u);
    CHECK(GPU_PGRAPH_INFER_ALL == 0x7Fu);

    /* a directory with no combiner module and the combiner on is refused, naming what is missing */
    char vertex_only[300];
    snprintf(vertex_only, sizeof vertex_only, "%s/vertex_only", spv_directory);
    CHECK(mkdir(vertex_only, 0777) == 0);
    char from[400], to[400];
    snprintf(from, sizeof from, "%s/%s.spv", spv_directory, SYNTHETIC_PROGRAM_NAME);
    snprintf(to, sizeof to, "%s/%s.spv", vertex_only, SYNTHETIC_PROGRAM_NAME);
    CHECK(link(from, to) == 0);
    config = combiner_config(NULL);
    config.spv_directory = vertex_only;
    error[0] = '\0';
    CHECK(!d3d8_swap_replay_enable(&config, error, sizeof error));
    CHECK(strstr(error, "combiner_") != NULL && strstr(error, vertex_only) != NULL);
    CHECK(!d3d8_swap_replay_enabled());
    config.combiner = false; /* the same directory is fine for the fixed stage */
    CHECK(d3d8_swap_replay_enable(&config, error, sizeof error));
    d3d8_swap_replay_disable();
    (void)unlink(to);
    (void)rmdir(vertex_only);
}

/* The default is unchanged: with the combiner off, a stream that carries a full pixel shader definition
 * and a directory that holds combiner modules replay byte for byte as the stream without them. */
static void test_combiner_off_is_byte_identical(const char *selector)
{
    printf("test_combiner_off_is_byte_identical (%s)\n", selector);
    begin_device(true);
    d3d8_swap_replay_config config = base_config(selector);
    config.strict = false;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    stream_builder plain = {0};
    build_frame_one(&plain);
    push(&plain);
    present();
    d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_replayed == 1u);
    const gpu_image *frame = d3d8_swap_replay_last_frame();
    CHECK(frame != NULL && frame->pixels != NULL);
    size_t bytes = 0u;
    uint8_t *reference = NULL;
    if (frame != NULL && frame->pixels != NULL) {
        bytes = (size_t)frame->stride_bytes * frame->height;
        reference = malloc(bytes);
        CHECK(reference != NULL);
        if (reference != NULL) {
            memcpy(reference, frame->pixels, bytes);
        }
        CHECK(pixel_is(frame, 160u, 120u, 255u, 0u, 0u, 255u));
        CHECK(count_not(frame, 0u, 0u, 0u, 255u) > 10000u); /* a triangle, so equality is not two blank frames */
    }
    d3d8_swap_replay_disable();
    environment_end();

    begin_device(true);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    push_combiner_frame(multiply_entries, MULTIPLY_COUNT);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_replayed == 1u);
    CHECK((stats.used_inferences & GPU_PGRAPH_INFER_COMBINER_ALL) == 0u);
    CHECK(d3d8_swap_replay_get_stats().unhandled_methods >= 40u); /* the 57 words are counted, not applied */
    frame = d3d8_swap_replay_last_frame();
    CHECK(frame != NULL && frame->pixels != NULL && reference != NULL);
    if (frame != NULL && frame->pixels != NULL && reference != NULL) {
        CHECK((size_t)frame->stride_bytes * frame->height == bytes);
        CHECK(memcmp(frame->pixels, reference, bytes) == 0);
    }
    free(reference);
    d3d8_swap_replay_disable();

    /* and strict mode refuses the first combiner word, as before */
    begin_device(true);
    config = base_config(selector);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    push_combiner_frame(multiply_entries, MULTIPLY_COUNT);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && stats.frames_replayed == 0u);
    CHECK(strstr(stats.error, "0x0260") != NULL);
    stream_free(&plain);
    d3d8_swap_replay_disable();
    environment_end();
}

static void test_combiner_on(const char *selector)
{
    printf("test_combiner_on (%s)\n", selector);
    /* the multiply configuration: r0 = c0 * c1, which is NOT the vertex colour (red) */
    begin_device(true);
    d3d8_swap_replay_config config = combiner_config(selector);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    push_combiner_frame(multiply_entries, MULTIPLY_COUNT);
    present();
    d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.draws == 1u);
    CHECK(stats.unhandled_methods == 0u); /* strict, and every combiner word was decoded */
    CHECK((stats.used_inferences & GPU_PGRAPH_INFER_COMBINER_CONSTANT_BYTES) != 0u);
    const gpu_image *frame = d3d8_swap_replay_last_frame();
    CHECK(frame != NULL && frame->pixels != NULL);
    if (frame != NULL && frame->pixels != NULL) {
        CHECK(pixel_is(frame, 160u, 120u, 128u, 96u, 64u, 255u)); /* factor words 0x00FF8040 * 0x0080C0FF */
        CHECK(pixel_is(frame, 480u, 336u, 0u, 0u, 0u, 255u));    /* outside the triangle: the clear */
        const uint32_t covered = count_not(frame, 0u, 0u, 0u, 255u);
        CHECK(covered > 10000u && covered < 60000u);
    }
    /* a later frame with another definition replaces it (the shadow persists, the draw list does not) */
    push_combiner_frame(pass_entries, sizeof pass_entries / sizeof pass_entries[0]);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_replayed == 2u);
    frame = d3d8_swap_replay_last_frame();
    CHECK(frame != NULL && frame->pixels != NULL);
    if (frame != NULL && frame->pixels != NULL) {
        CHECK(pixel_is(frame, 160u, 120u, 255u, 0u, 0u, 255u)); /* pass: oD0 unchanged, the red of the vertices */
    }
    CHECK((stats.used_inferences & GPU_PGRAPH_INFER_COMBINER_COLOUR_RANGE) != 0u);
    d3d8_swap_replay_disable();
    environment_end();

    /* the combiner inferences are a gate: allowed none, the draw is refused and names the combiner */
    begin_device(true);
    config = combiner_config(selector);
    config.allowed_inferences = GPU_PGRAPH_INFER_ALL;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    push_combiner_frame(multiply_entries, MULTIPLY_COUNT);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && stats.frames_replayed == 0u && stats.frames_refused == 1u);
    CHECK(strstr(stats.error, "combiner") != NULL && strstr(stats.error, "INFERRED") != NULL);
    d3d8_swap_replay_disable();
    environment_end();

    /* a configuration with no translated module is refused by name, never replaced by a near one */
    begin_device(true);
    config = combiner_config(selector);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    push_combiner_frame(final_entries, sizeof final_entries / sizeof final_entries[0]);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && stats.frames_replayed == 0u);
    CHECK(strstr(stats.error, "no translated module in the fragment table") != NULL);
    CHECK(strstr(stats.error, COMBINER_NAME_FINAL) != NULL);
    CHECK(d3d8_swap_replay_last_frame() == NULL);
    d3d8_swap_replay_disable();
    environment_end();

    /* a draw with no combiner word at all is not given a stage that was never written */
    begin_device(true);
    config = combiner_config(selector);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    stream_builder bare = {0};
    build_frame_one(&bare);
    push(&bare);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && stats.frames_replayed == 0u);
    stream_free(&bare);
    d3d8_swap_replay_disable();
    environment_end();

    /* CreateDevice's reset rebuilds the model: it keeps the opt-in, the next combiner words still decode */
    begin_device(true);
    config = combiner_config(selector);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    d3d8_gpu_reset();
    push_combiner_frame(multiply_entries, MULTIPLY_COUNT);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.model_resets == 1u && !stats.latched && stats.frames_replayed == 1u);
    frame = d3d8_swap_replay_last_frame();
    CHECK(frame != NULL && frame->pixels != NULL);
    if (frame != NULL && frame->pixels != NULL) {
        CHECK(pixel_is(frame, 160u, 120u, 128u, 96u, 64u, 255u));
    }
    d3d8_swap_replay_disable();
    environment_end();
}

/* --- T497: the stand-in texture ---------------------------------------------------------- */

/* PASS with the colour and alpha inputs reading t0. The stream also writes the stage program (word 54, method 0x1E70) as 1, stage 0
 * a 2D texture, as the title's does (MEASURED, push_textured_frame). The words of
 * tools/nv2a_combiner/standin_fixtures.py TEXTURED. */
static const combiner_entry textured_entries[] = {{34u, 0x08200000u}, {0u, 0x18300000u}, {45u, 0xC0u},
                                                  {26u, 0xC0u},       {53u, 0x11101u},   {8u, 0x0Cu},
                                                  {9u, 0x1C80u}};
#define TEXTURED_COUNT (sizeof textured_entries / sizeof textured_entries[0])
#define STANDIN_INFERENCES (GPU_PGRAPH_INFER_COMBINER_REPLAY | D3D8_SWAP_REPLAY_INFER_STANDIN_TEXTURE)
#define STAGE_PROGRAM_METHOD 0x1E70u /* combiner word 54: 5 bits per texture stage, mode 1 is a 2D texture */

/* The textured definition, the stage program word `program` when `write_program` (the title writes it), then the red
 * triangle. */
static void push_textured_frame(bool write_program, uint32_t program)
{
    uint32_t words[GPU_PGRAPH_COMBINER_WORDS];
    build_combiner_words(textured_entries, TEXTURED_COUNT, words);
    stream_builder stream = {0};
    stream_pixel_shader(&stream, words);
    if (write_program) {
        stream_pair(&stream, STAGE_PROGRAM_METHOD, program);
    }
    build_frame_one(&stream);
    push(&stream);
    stream_free(&stream);
}

static d3d8_swap_replay_config standin_config(const char *selector, uint32_t width, uint32_t height, uint32_t rgba)
{
    d3d8_swap_replay_config config = combiner_config(selector);
    config.standin_texture = true;
    config.standin_width = width;
    config.standin_height = height;
    config.standin_rgba[0] = (uint8_t)(rgba >> 24);
    config.standin_rgba[1] = (uint8_t)(rgba >> 16);
    config.standin_rgba[2] = (uint8_t)(rgba >> 8);
    config.standin_rgba[3] = (uint8_t)rgba;
    config.allowed_inferences = GPU_PGRAPH_INFER_ALL | STANDIN_INFERENCES;
    return config;
}

/* No device: the textured fixture is the module the planner names with a stand-in, and the only inferences it needs
 * is the one the option allows (TEXTURE_SAMPLING): the fixture reads t0 only, no oD0, no factor. */
static void test_standin_plan(void)
{
    printf("test_standin_plan\n");
    gpu_pgraph *pgraph = gpu_pgraph_create();
    gpu_pgraph_set_combiner(pgraph, true);
    uint32_t words[GPU_PGRAPH_COMBINER_WORDS];
    build_combiner_words(textured_entries, TEXTURED_COUNT, words);
    stream_builder stream = {0};
    stream_pixel_shader(&stream, words);
    stream_pair(&stream, STAGE_PROGRAM_METHOD, 1u);
    CHECK(stream.count > 0u);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    const gpu_pgraph_state *model = gpu_pgraph_state_now(pgraph);
    static const uint8_t texel[4] = {1u, 2u, 3u, 4u};
    gpu_combiner_texture standin[GPU_COMBINER_TEXTURE_STAGES] = {{texel, 1u, 1u, false, false, NULL, false, false}};
    static const gpu_combiner_texture none[GPU_COMBINER_TEXTURE_STAGES] = {{NULL, 0u, 0u, false, false, NULL, false, false}};
    gpu_combiner_plan plan;
    char error[300] = "";
    CHECK(gpu_combiner_plan_build(model, STANDIN_INFERENCES, standin, &plan, error, sizeof error) == GPU_PGRAPH_OK);
    CHECK(strcmp(plan.name, COMBINER_NAME_TEXTURED) == 0);
    CHECK(plan.texture_stages == 1u); /* stage 0 only */
    CHECK(plan.used_inferences != 0u);
    CHECK(plan.used_inferences == GPU_COMBINER_INFER_TEXTURE_SAMPLING);
    CHECK((plan.used_inferences & ~STANDIN_INFERENCES) == 0u); /* nothing the host flags do not announce */
    /* the one stand-in inference is a gate of its own */
    CHECK(gpu_combiner_plan_build(model, STANDIN_INFERENCES & ~GPU_COMBINER_INFER_TEXTURE_SAMPLING, standin, &plan,
                                  error, sizeof error) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(error, "sampling") != NULL && strstr(error, "INFERRED") != NULL);
    /* without the stand-in the draw is refused by name, whatever is allowed */
    CHECK(gpu_combiner_plan_build(model, GPU_COMBINER_INFER_ALL, none, &plan, error, sizeof error) ==
          GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(error, "no test texture was supplied for stage 0") != NULL);
    /* a stand-in for stage 1 does not feed a read of t0 */
    gpu_combiner_texture stage_one[GPU_COMBINER_TEXTURE_STAGES] = {{NULL, 0u, 0u, false, false, NULL, false, false}, {texel, 1u, 1u, false, false, NULL, false, false}};
    CHECK(gpu_combiner_plan_build(model, GPU_COMBINER_INFER_ALL, stage_one, &plan, error, sizeof error) ==
          GPU_PGRAPH_ERR_UNMEASURED);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);

    /* a stream that never wrote the stage program: the stand-in does NOT stand in for it, the draw is refused naming
     * the stage program (STAGE_PROGRAM is not allowed by the option), and with that inference lifted it plans */
    pgraph = gpu_pgraph_create();
    gpu_pgraph_set_combiner(pgraph, true);
    stream_builder unwritten = {0};
    stream_pixel_shader(&unwritten, words);
    CHECK(gpu_pgraph_decode(pgraph, unwritten.pairs, unwritten.count) == GPU_PGRAPH_OK);
    model = gpu_pgraph_state_now(pgraph);
    CHECK(gpu_combiner_plan_build(model, STANDIN_INFERENCES, standin, &plan, error, sizeof error) ==
          GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(error, "stage program") != NULL && strstr(error, "INFERRED") != NULL);
    CHECK(gpu_combiner_plan_build(model, STANDIN_INFERENCES | GPU_COMBINER_INFER_STAGE_PROGRAM, standin, &plan, error,
                                  sizeof error) == GPU_PGRAPH_OK);
    CHECK(strcmp(plan.name, COMBINER_NAME_TEXTURED) == 0);
    stream_free(&unwritten);
    gpu_pgraph_destroy(pgraph);

    /* a stage program written as "none" for stage 0 is the stream's own word and is not overridden by a stand-in:
     * T1207, the stage reads (0,0,0,1) like xemu's PS_TEXTUREMODES_NONE and nothing is sampled */
    pgraph = gpu_pgraph_create();
    gpu_pgraph_set_combiner(pgraph, true);
    stream_builder none_program = {0};
    stream_pixel_shader(&none_program, words);
    stream_pair(&none_program, STAGE_PROGRAM_METHOD, 0u);
    CHECK(gpu_pgraph_decode(pgraph, none_program.pairs, none_program.count) == GPU_PGRAPH_OK);
    CHECK(gpu_combiner_plan_build(gpu_pgraph_state_now(pgraph), GPU_COMBINER_INFER_ALL, standin, &plan, error,
                                  sizeof error) == GPU_PGRAPH_OK);
    CHECK(plan.texture_stages == 0u);
    stream_free(&none_program);
    gpu_pgraph_destroy(pgraph);
}

/* No device: the option's configuration, its refusals and the text that says what the picture is not. */
static void test_standin_config(void)
{
    printf("test_standin_config\n");
    char error[300];
    char text[700];
    /* OFF by default, in the default config and in one built by hand, and then there is nothing to announce */
    const d3d8_swap_replay_config defaults = d3d8_swap_replay_default_config();
    CHECK(!defaults.standin_texture && defaults.standin_width == 0u && defaults.standin_height == 0u);
    CHECK(defaults.standin_rgba[0] == 0u && defaults.standin_rgba[3] == 0u);
    CHECK(!base_config(NULL).standin_texture && !combiner_config(NULL).standin_texture);
    d3d8_swap_replay_config config = combiner_config(NULL);
    CHECK(d3d8_swap_replay_enable(&config, error, sizeof error));
    CHECK(!d3d8_swap_replay_get_config().standin_texture);
    text[0] = 'x';
    CHECK(d3d8_swap_replay_standin_summary(text, sizeof text) == 0u && text[0] == '\0');
    CHECK(d3d8_swap_replay_get_stats().standin_draws == 0u);
    d3d8_swap_replay_disable();

    /* on: the fields come back as enable took them, and the summary names size, colour and what it is NOT */
    config = standin_config(NULL, 2u, 3u, 0x336699FFu);
    CHECK(d3d8_swap_replay_enable(&config, error, sizeof error));
    const d3d8_swap_replay_config taken = d3d8_swap_replay_get_config();
    CHECK(taken.standin_texture && taken.standin_width == 2u && taken.standin_height == 3u);
    CHECK(taken.standin_rgba[0] == 0x33u && taken.standin_rgba[1] == 0x66u && taken.standin_rgba[2] == 0x99u &&
          taken.standin_rgba[3] == 0xFFu);
    const size_t length = d3d8_swap_replay_standin_summary(text, sizeof text);
    CHECK(length > 0u && length == strlen(text));
    CHECK(strstr(text, "NOT the title's texture") != NULL);
    CHECK(strstr(text, "2x3") != NULL && strstr(text, "336699FF") != NULL);
    CHECK(strstr(text, "sampled by 0 of 0 replayed draw(s)") != NULL);
    CHECK(strstr(text, "TEXTURE_SAMPLING") != NULL && strstr(text, "STAGE_PROGRAM") == NULL);
    CHECK(strstr(text, "stage 0") != NULL && strstr(text, "oT0") != NULL);
    char small[16];
    CHECK(d3d8_swap_replay_standin_summary(small, sizeof small) == strlen(small) && strlen(small) < sizeof small);
    d3d8_swap_replay_disable();
    CHECK(!d3d8_swap_replay_get_config().standin_texture);

    /* refused: no combiner (the stand-in only feeds the combiner), a zero or oversize edge, naming what is wrong */
    config = standin_config(NULL, 2u, 2u, 0x11223344u);
    config.combiner = false;
    error[0] = '\0';
    CHECK(!d3d8_swap_replay_enable(&config, error, sizeof error));
    CHECK(strstr(error, "combiner") != NULL && strstr(error, "stand-in") != NULL);
    CHECK(!d3d8_swap_replay_enabled());
    config = standin_config(NULL, 0u, 2u, 0x11223344u);
    error[0] = '\0';
    CHECK(!d3d8_swap_replay_enable(&config, error, sizeof error));
    CHECK(strstr(error, "stand-in") != NULL && strstr(error, "1..4096") != NULL);
    config = standin_config(NULL, 2u, 0u, 0x11223344u);
    error[0] = '\0';
    CHECK(!d3d8_swap_replay_enable(&config, error, sizeof error));
    CHECK(strstr(error, "1..4096") != NULL);
    config = standin_config(NULL, 4097u, 1u, 0x11223344u);
    error[0] = '\0';
    CHECK(!d3d8_swap_replay_enable(&config, error, sizeof error));
    CHECK(strstr(error, "1..4096") != NULL);
    config = standin_config(NULL, 1u, 4097u, 0x11223344u);
    error[0] = '\0';
    CHECK(!d3d8_swap_replay_enable(&config, error, sizeof error));
    CHECK(strstr(error, "1..4096") != NULL);
    CHECK(!d3d8_swap_replay_enabled());
    /* the edges themselves are accepted */
    config = standin_config(NULL, 4096u, 1u, 0x11223344u);
    CHECK(d3d8_swap_replay_enable(&config, error, sizeof error));
    d3d8_swap_replay_disable();
    config = standin_config(NULL, 1u, 4096u, 0x11223344u);
    CHECK(d3d8_swap_replay_enable(&config, error, sizeof error));
    d3d8_swap_replay_disable();
    /* with the option off the size and colour fields are not looked at (they are 0 in every default config) */
    config = combiner_config(NULL);
    config.standin_width = 99999u;
    CHECK(d3d8_swap_replay_enable(&config, error, sizeof error));
    d3d8_swap_replay_disable();
}

/* The triangle through the textured combiner with the stand-in `rgba`, in a fresh device. */
static d3d8_swap_replay_stats standin_frame(const char *selector, uint32_t rgba, uint8_t pixel[4], bool *have_pixel)
{
    begin_device(true);
    d3d8_swap_replay_config config = standin_config(selector, 2u, 2u, rgba);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    push_textured_frame(true, 1u);
    present();
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    const gpu_image *frame = d3d8_swap_replay_last_frame();
    *have_pixel = frame != NULL && frame->pixels != NULL;
    if (*have_pixel) {
        memcpy(pixel, frame->pixels + gpu_image_offset(frame, 160u, 120u), 4u);
        const uint32_t covered = count_not(frame, 0u, 0u, 0u, 255u);
        CHECK(covered > 10000u); /* a triangle, not a blank frame */
        /* the texture coordinate runs over the whole texture (oT0 follows the position), so EVERY covered pixel is the
         * stand-in colour: a texel the option left unset, or another colour, would show */
        CHECK(count_not(frame, (uint8_t)(rgba >> 24), (uint8_t)(rgba >> 16), (uint8_t)(rgba >> 8), (uint8_t)rgba) ==
              frame->width * frame->height - covered);
    }
    d3d8_swap_replay_disable();
    environment_end();
    return stats;
}

static void test_standin_texture(const char *selector)
{
    printf("test_standin_texture (%s)\n", selector);
    /* the vertex module that writes oT0, so the texture coordinate exists and is not invented */
    write_synthetic_vertex_module(draw_vertex_t0_words, sizeof draw_vertex_t0_words);

    uint8_t pixel[4] = {0};
    bool have = false;
    d3d8_swap_replay_stats stats = standin_frame(selector, 0x336699F0u, pixel, &have);
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.draws == 1u);
    CHECK(have);
    if (have) { /* the stand-in's own RGBA, all four channels: t0.rgb and t0.a */
        CHECK(pixel[0] == 0x33u && pixel[1] == 0x66u && pixel[2] == 0x99u && pixel[3] == 0xF0u);
    }
    CHECK(stats.standin_draws == 1u);
    CHECK((stats.used_inferences & GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING) != 0u);
    CHECK((stats.used_inferences & GPU_PGRAPH_INFER_COMBINER_COLOUR_RANGE) == 0u); /* it reads no oD0 */
    CHECK((stats.used_inferences & GPU_PGRAPH_INFER_COMBINER_STAGE_PROGRAM) == 0u); /* the stream wrote it */

    /* another colour, another pixel: the option's colour is what the draw samples */
    uint8_t other[4] = {0};
    bool have_other = false;
    stats = standin_frame(selector, 0xFF8000C0u, other, &have_other);
    CHECK(!stats.latched && stats.standin_draws == 1u && have_other);
    if (have && have_other) {
        CHECK(other[0] == 255u && other[1] == 128u && other[2] == 0u && other[3] == 0xC0u);
        CHECK(memcmp(pixel, other, 4u) != 0);
    }

    /* the stand-in persists over frames and the summary counts the draws that sampled it */
    begin_device(true);
    d3d8_swap_replay_config config = standin_config(selector, 1u, 1u, 0x336699FFu);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    push_textured_frame(true, 1u);
    present();
    stream_builder second = {0};
    build_frame_two(&second);
    push(&second);
    present();
    stream_free(&second);
    stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_replayed == 2u && stats.draws == 2u && stats.standin_draws == 2u);
    char text[700];
    CHECK(d3d8_swap_replay_standin_summary(text, sizeof text) > 0u);
    CHECK(strstr(text, "sampled by 2 of 2 replayed draw(s)") != NULL);
    CHECK(strstr(text, "NOT the title's texture") != NULL);
    d3d8_swap_replay_disable();
    environment_end();

    /* a draw whose combiner reads no texture does not sample it: stand-in on, the pass-through is the red of the
     * vertices and `standin_draws` stays 0 (the count is of draws that read it, not of draws made) */
    begin_device(true);
    config = standin_config(selector, 2u, 2u, 0x336699FFu);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    push_combiner_frame(pass_entries, sizeof pass_entries / sizeof pass_entries[0]);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.draws == 1u && stats.standin_draws == 0u);
    CHECK((stats.used_inferences & GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING) == 0u);
    const gpu_image *frame = d3d8_swap_replay_last_frame();
    CHECK(frame != NULL && frame->pixels != NULL && pixel_is(frame, 160u, 120u, 255u, 0u, 0u, 255u));
    CHECK(d3d8_swap_replay_standin_summary(text, sizeof text) > 0u);
    CHECK(strstr(text, "sampled by 0 of 1 replayed draw(s)") != NULL);
    d3d8_swap_replay_disable();
    environment_end();

    /* the stand-in fields without the option are ignored: the textured draw is refused, naming the missing texture */
    begin_device(true);
    config = standin_config(selector, 2u, 2u, 0x336699FFu);
    config.standin_texture = false;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    push_textured_frame(true, 1u);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && stats.frames_replayed == 0u && stats.standin_draws == 0u);
    CHECK(strstr(stats.error, "no test texture was supplied for stage 0") != NULL);
    CHECK(d3d8_swap_replay_last_frame() == NULL);
    d3d8_swap_replay_disable();
    environment_end();

    /* the option does not allow the inference by itself: the config's mask decides, and the refusal names it */
    begin_device(true);
    config = standin_config(selector, 2u, 2u, 0x336699FFu);
    config.allowed_inferences = GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_COMBINER_REPLAY;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    push_textured_frame(true, 1u);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && stats.frames_replayed == 0u && stats.standin_draws == 0u);
    CHECK(strstr(stats.error, "INFERRED and not allowed") != NULL && strstr(stats.error, "sampling") != NULL);
    d3d8_swap_replay_disable();
    environment_end();

    /* a stream that never wrote the stage program is refused naming it: the stand-in does not stand in for word 54 */
    begin_device(true);
    config = standin_config(selector, 2u, 2u, 0x336699FFu);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    push_textured_frame(false, 0u);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && stats.frames_replayed == 0u && stats.standin_draws == 0u);
    CHECK(strstr(stats.error, "stage program") != NULL && strstr(stats.error, "INFERRED and not allowed") != NULL);
    d3d8_swap_replay_disable();
    environment_end();

    /* a stage program that says "no texture" for stage 0 is the stream's own word, the combiner plan builds with
     * t0 = (0,0,0,1) (T1207, xemu PS_TEXTUREMODES_NONE) and is not given the stand-in */
    begin_device(true);
    config = standin_config(selector, 2u, 2u, 0x336699FFu);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    push_textured_frame(true, 0u);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && stats.standin_draws == 0u && strstr(stats.error, "program is none") == NULL);
    /* the plan now builds, the refusal is only that this table has no pre-translated module for the new key */
    CHECK(strstr(stats.error, "no translated module") != NULL);
    d3d8_swap_replay_disable();
    environment_end();

    /* a vertex program that writes no oT0 is refused BY NAME, never given a texture coordinate nobody wrote */
    write_synthetic_vertex_module(draw_vertex_words, sizeof draw_vertex_words);
    begin_device(true);
    config = standin_config(selector, 2u, 2u, 0x336699FFu);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    push_textured_frame(true, 1u);
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && stats.frames_replayed == 0u && stats.frames_refused == 1u && stats.draws == 0u);
    CHECK(strstr(stats.error, "combiner reads varyings 0x8") != NULL);
    CHECK(strstr(stats.error, "writes only 0x1") != NULL && strstr(stats.error, &SYNTHETIC_PROGRAM_NAME[7]) != NULL);
    CHECK(d3d8_swap_replay_last_frame() == NULL);
    d3d8_swap_replay_disable();
    environment_end();
}

/* --- T484: which frames are dumped --------------------------------------------------------- */

static void use_dump_leaf(const char *leaf)
{
    snprintf(dump_directory, sizeof dump_directory, "%s/%s", spv_directory, leaf);
    remove_tree(dump_directory); /* a run on the other device left its frames here */
}

/* `frames` presents, each drawing the red triangle (the program, arrays and viewport persist). */
static void present_drawn_frames(unsigned frames)
{
    for (unsigned frame = 0u; frame < frames; frame++) {
        stream_builder stream = {0};
        if (frame == 0u) {
            build_frame_one(&stream);
        } else {
            stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
        }
        push(&stream);
        present();
        stream_free(&stream);
    }
}

static unsigned dumped_frames(unsigned frames)
{
    unsigned found = 0u;
    for (unsigned frame = 1u; frame <= frames; frame++) {
        char name[64];
        snprintf(name, sizeof name, "frame_%06u.png", frame);
        found += file_size(name) > 100 && is_png(name);
    }
    return found;
}

static bool dumped(unsigned frame)
{
    char name[64];
    snprintf(name, sizeof name, "frame_%06u.png", frame);
    return file_size(name) > 100 && is_png(name);
}

static void test_dump_cadence(const char *selector)
{
    printf("test_dump_cadence (%s)\n", selector);
    char saved[sizeof dump_directory];
    snprintf(saved, sizeof saved, "%s", dump_directory);

    /* the default config dumps every replayed frame (and the cadence fields are 0 and false) */
    CHECK(d3d8_swap_replay_default_config().dump_every == 0u && !d3d8_swap_replay_default_config().dump_last);
    use_dump_leaf("cadence_all");
    begin_device(true);
    d3d8_swap_replay_config config = base_config(selector);
    config.dump_directory = dump_directory;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    present_drawn_frames(4u);
    d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.frames_replayed == 4u && stats.dumps_written == 4u);
    CHECK(dumped_frames(4u) == 4u);
    d3d8_swap_replay_dump_last(); /* not asked for: nothing more is written */
    CHECK(d3d8_swap_replay_get_stats().dumps_written == 4u);
    d3d8_swap_replay_disable();
    environment_end();

    /* every 2nd frame: the frames whose number is a multiple of 2, replayed all the same */
    use_dump_leaf("cadence_every");
    begin_device(true);
    config = base_config(selector);
    config.dump_directory = dump_directory;
    config.dump_every = 2u;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    CHECK(d3d8_swap_replay_get_config().dump_every == 2u);
    present_drawn_frames(5u);
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.frames_replayed == 5u && stats.dumps_written == 2u && stats.dump_failures == 0u);
    CHECK(!dumped(1u) && dumped(2u) && !dumped(3u) && dumped(4u) && !dumped(5u));
    d3d8_swap_replay_dump_last(); /* dump_last is off: still nothing */
    CHECK(d3d8_swap_replay_get_stats().dumps_written == 2u && !dumped(5u));
    d3d8_swap_replay_disable();
    environment_end();

    /* last only: nothing is written while it runs, the last replayed frame is written once at the end */
    use_dump_leaf("cadence_last");
    begin_device(true);
    config = base_config(selector);
    config.dump_directory = dump_directory;
    config.dump_last = true;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    present_drawn_frames(3u);
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.frames_replayed == 3u && stats.dumps_written == 0u);
    CHECK(dumped_frames(3u) == 0u);
    d3d8_swap_replay_dump_last();
    CHECK(d3d8_swap_replay_get_stats().dumps_written == 1u);
    CHECK(!dumped(1u) && !dumped(2u) && dumped(3u));
    d3d8_swap_replay_dump_last(); /* a second call writes nothing new */
    CHECK(d3d8_swap_replay_get_stats().dumps_written == 1u);
    d3d8_swap_replay_disable();
    environment_end();

    /* both: every 2nd and the last, and a last that was already written is not written twice */
    use_dump_leaf("cadence_both");
    begin_device(true);
    config = base_config(selector);
    config.dump_directory = dump_directory;
    config.dump_every = 2u;
    config.dump_last = true;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    present_drawn_frames(3u);
    d3d8_swap_replay_dump_last();
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.dumps_written == 2u && !dumped(1u) && dumped(2u) && dumped(3u));
    present_drawn_frames(1u); /* frame 4 is on the cadence: written at the present, then the flush adds nothing */
    d3d8_swap_replay_dump_last();
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.dumps_written == 3u && dumped(4u));
    d3d8_swap_replay_disable();
    environment_end();

    /* no dump directory: the cadence has nothing to choose, and the flush is a no-op */
    begin_device(true);
    config = base_config(selector);
    config.dump_last = true;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    present_drawn_frames(2u);
    d3d8_swap_replay_dump_last();
    CHECK(d3d8_swap_replay_get_stats().dumps_written == 0u && d3d8_swap_replay_get_stats().dump_failures == 0u);
    d3d8_swap_replay_disable();
    d3d8_swap_replay_dump_last(); /* not enabled: harmless */
    environment_end();

    /* the offscreen passes of a frame follow the frame's own decision */
    use_dump_leaf("cadence_offscreen");
    begin_device(true);
    config = base_config(selector);
    config.dump_directory = dump_directory;
    config.dump_every = 2u;
    config.dump_last = true;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    const uint32_t back = d3d8_device_load32(DEV_RENDER_TARGET);
    make_target(TARGET_A, 128u, 64u);
    for (unsigned frame = 1u; frame <= 3u; frame++) {
        stream_builder first = {0};
        if (frame == 1u) {
            build_frame_one(&first);
        } else {
            stream_draw_arrays(&first, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
        }
        push_pending(&first);
        bind_target(TARGET_A);
        stream_builder second = {0};
        build_small_target_draw(&second, 128u, 64u, 3u);
        push_pending(&second);
        bind_target(back);
        present();
        stream_free(&first);
        stream_free(&second);
    }
    char target_name[64];
    CHECK(d3d8_swap_replay_get_stats().frames_replayed == 3u && d3d8_swap_replay_get_stats().dumps_written == 2u);
    dump_name(target_name, sizeof target_name, 2u, TARGET_A);
    CHECK(dumped(2u) && file_size(target_name) > 100);
    dump_name(target_name, sizeof target_name, 1u, TARGET_A);
    CHECK(!dumped(1u) && file_size(target_name) == -1);
    d3d8_swap_replay_dump_last();
    CHECK(d3d8_swap_replay_get_stats().dumps_written == 4u && dumped(3u));
    d3d8_swap_replay_dump_last(); /* a second flush writes neither the frame nor its pass again */
    CHECK(d3d8_swap_replay_get_stats().dumps_written == 4u);
    dump_name(target_name, sizeof target_name, 3u, TARGET_A);
    CHECK(file_size(target_name) > 100);
    dump_name(target_name, sizeof target_name, 2u, TARGET_A);
    CHECK(file_size(target_name) > 100); /* the cadence's own frame 2 is still there, its pass named by frame 2 */
    /* frame 4 is on the cadence: the frame and its pass are written at the present, and the flush then
     * has nothing left of either */
    stream_builder fourth = {0}, fourth_target = {0};
    stream_draw_arrays(&fourth, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    push_pending(&fourth);
    bind_target(TARGET_A);
    build_small_target_draw(&fourth_target, 128u, 64u, 3u);
    push_pending(&fourth_target);
    bind_target(back);
    present();
    CHECK(d3d8_swap_replay_get_stats().dumps_written == 6u && dumped(4u));
    d3d8_swap_replay_dump_last();
    CHECK(d3d8_swap_replay_get_stats().dumps_written == 6u);
    stream_free(&fourth);
    stream_free(&fourth_target);
    d3d8_swap_replay_disable();
    environment_end();

    snprintf(dump_directory, sizeof dump_directory, "%s", saved);
}

/* --- T578: the 2D engine blit over the surface images ------------------------------------------------------------------ */

static uint32_t surface_data(uint32_t header)
{
    return d3d8_guest_load32(header + D3D8_SURFACE_DATA) & 0x0FFFFFFFu;
}

/* The ten dwords CopyRects writes (docs/d3d8-copy-composition.md): 0x86308 source and destination Data, 0x86300 colour format
 * and pitches, 0xC4300 source point, destination point and size. Pending (no kick), like a title's emission. */
static void push_blit_packet(uint32_t source, uint32_t destination, uint32_t format, uint32_t pitches, uint32_t from,
                             uint32_t to, uint32_t size)
{
    const uint32_t words[10] = {0x00086308u, source, destination, 0x00086300u, format, pitches, 0x000C4300u, from, to, size};
    const uint32_t cursor = d3d8_pushbuffer_begin();
    for (uint32_t i = 0u; i < 10u; i++) {
        store(cursor + i * 4u, words[i]);
    }
    d3d8_pushbuffer_end(cursor + 40u);
}

#define BLIT_PITCHES_640 0x0A000A00u
#define TARGET_A_DATA 0x00500000u

static d3d8_swap_replay_config blit_config(const char *selector, uint32_t inferences)
{
    d3d8_swap_replay_config config = base_config(selector);
    config.output_groups = GPU_PGRAPH_OUTPUT_BLIT;
    config.allowed_inferences = GPU_PGRAPH_INFER_ALL | inferences;
    return config;
}

static bool is_black(const gpu_image *image, uint32_t x, uint32_t y)
{
    return pixel_is(image, x, y, 0u, 0u, 0u, 255u);
}

/* back (the bound 640x480 target): red triangle 1. TARGET_A (640x480, its own Data word): green triangle 2. */
static void test_blit_over_surfaces(const char *selector)
{
    printf("test_blit_over_surfaces (%s)\n", selector);
    begin_device(true);
    d3d8_swap_replay_config config = blit_config(selector, GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL);
    config.dump_directory = dump_directory;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    const uint32_t back = d3d8_device_load32(DEV_RENDER_TARGET);
    const uint32_t back_data = surface_data(back);
    make_target(TARGET_A, 640u, 480u);
    store(TARGET_A + D3D8_SURFACE_DATA, TARGET_A_DATA);
    CHECK(back_data != 0u && back_data != TARGET_A_DATA);
    CHECK(d3d8_swap_replay_surface_image(back_data) == NULL); /* nothing is kept before a pass */

    /* Frame 1: draw 1 into back, draw 2 into A, then back (100, 100) 128 x 64 to A (0, 0), in one frame and two passes. */
    stream_builder first = {0};
    build_frame_one(&first);
    push_pending(&first);
    bind_target(TARGET_A);
    stream_builder second = {0};
    build_small_target_draw(&second, 640u, 480u, 3u);
    push_pending(&second);
    push_blit_packet(back_data, TARGET_A_DATA, 0xAu, BLIT_PITCHES_640, 100u | (100u << 16), 0u, 128u | (64u << 16));
    bind_target(back);
    present();
    d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_refused == 0u && stats.frames_replayed == 1u && stats.passes_replayed == 2u);
    CHECK(stats.copies_applied == 1u && stats.copies_empty == 0u && stats.draws == 2u);
    CHECK((stats.used_inferences & GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL) != 0u);
    CHECK(d3d8_swap_replay_offscreen_count() == 1u);
    uint32_t target = 0u;
    const gpu_image *offscreen = d3d8_swap_replay_offscreen_frame(0u, &target);
    CHECK(offscreen != NULL && offscreen->pixels != NULL && target == TARGET_A);
    if (offscreen != NULL && offscreen->pixels != NULL) {
        CHECK(offscreen->width == 640u && offscreen->height == 480u);
        CHECK(pixel_is(offscreen, 60u, 20u, 255u, 0u, 0u, 255u));   /* source (160, 120) of the back buffer's triangle */
        CHECK(is_black(offscreen, 160u, 120u));                    /* outside the destination rectangle: untouched */
        CHECK(!is_black(offscreen, 480u, 336u));                   /* the pass's own triangle survives */
        CHECK(pixel_is(offscreen, 0u, 0u, 255u, 0u, 0u, 255u));    /* source (100, 100) is inside the triangle */
        CHECK(is_black(offscreen, 127u, 63u));                     /* source (227, 163) is outside it */
        CHECK(is_black(offscreen, 128u, 64u));                     /* just outside the rectangle */
    }
    const gpu_image *presented = d3d8_swap_replay_last_frame();
    CHECK(presented != NULL && presented->pixels != NULL);
    if (presented != NULL && presented->pixels != NULL) {
        CHECK(pixel_is(presented, 160u, 120u, 255u, 0u, 0u, 255u)); /* a source is never changed by a blit */
    }
    /* both surfaces are kept under their Data words, with the blit in A's image */
    const gpu_image *kept_back = d3d8_swap_replay_surface_image(back_data);
    const gpu_image *kept_a = d3d8_swap_replay_surface_image(TARGET_A_DATA);
    CHECK(kept_back != NULL && kept_a != NULL && kept_back->pixels != NULL && kept_a->pixels != NULL);
    if (kept_back != NULL && kept_a != NULL && kept_back->pixels != NULL && kept_a->pixels != NULL) {
        CHECK(pixel_is(kept_back, 160u, 120u, 255u, 0u, 0u, 255u) && pixel_is(kept_a, 60u, 20u, 255u, 0u, 0u, 255u));
        CHECK(kept_a->pixels != offscreen->pixels && kept_back->pixels != presented->pixels); /* copies, not the frame's own */
    }
    CHECK(d3d8_swap_replay_surface_image(0x00700000u) == NULL);
    stream_free(&first);
    stream_free(&second);

    /* Frame 2 has no draw at all: A (kept from frame 1) onto back (kept), (0, 0) 128 x 64 to (300, 300). Both are stored images. */
    CHECK(is_black(kept_back, 360u, 320u));
    push_blit_packet(TARGET_A_DATA, back_data, 0xAu, BLIT_PITCHES_640, 0u, 300u | (300u << 16), 128u | (64u << 16));
    d3d8_gpu_kick();
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_empty == 1u && stats.copies_applied == 2u);
    kept_back = d3d8_swap_replay_surface_image(back_data);
    CHECK(kept_back != NULL && kept_back->pixels != NULL && pixel_is(kept_back, 360u, 320u, 255u, 0u, 0u, 255u));
    CHECK(kept_back != NULL && kept_back->pixels != NULL && pixel_is(kept_back, 160u, 120u, 255u, 0u, 0u, 255u));
    char name[96];
    snprintf(name, sizeof name, "frame_000002_blit_surface_%07X.png", (unsigned)back_data);
    CHECK(file_size(name) > 100 && is_png(name)); /* the blit's destination had no pass this frame: its image is dumped */

    /* Frame 3: an empty blit (zero height) moves nothing, counts, and needs no surface. */
    push_blit_packet(0x00710000u, 0x00720000u, 0xAu, BLIT_PITCHES_640, 0u, 0u, 128u);
    d3d8_gpu_kick();
    present();
    stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.copies_applied == 2u && stats.copies_empty == 1u);
    /* CreateDevice (d3d8_gpu_reset) made new surfaces: the kept images name old ones and are dropped */
    CHECK(d3d8_swap_replay_surface_image(back_data) != NULL);
    d3d8_gpu_reset();
    present();
    CHECK(d3d8_swap_replay_get_stats().model_resets == 1u);
    CHECK(d3d8_swap_replay_surface_image(back_data) == NULL && d3d8_swap_replay_surface_image(TARGET_A_DATA) == NULL);
    d3d8_swap_replay_disable();
    CHECK(d3d8_swap_replay_surface_image(back_data) == NULL); /* disable frees what was kept */
    environment_end();
}

/* T596: the measured blit semantics (docs/t736-xemu-surfaces.md) end to end over the kept surface images. A snapshot of the images
 * is taken, one frame of blits runs, and every pixel is compared with a plain model of "rows ascend, a row is buffered whole, the
 * row is clamped to the narrower pitch, the alpha rule" run over the snapshot. */
typedef struct {
    uint32_t width, height;
    uint8_t *pixels;
} snapshot_image;

typedef enum {
    CASE_UNKNOWN_DESTINATION,
    CASE_BYTE_FORMAT,
    CASE_PITCH,
    CASE_PAST_IMAGE,
    CASE_MID_DRAW,
    CASE_LATER_PASS,
    CASE_NO_INFERENCE,
    CASE_GROUP_OFF
} blit_case;

static void test_blit_refusal(const char *selector, blit_case which, const char *expected)
{
    begin_device(true);
    d3d8_swap_replay_config config = blit_config(selector, GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL);
    if (which == CASE_NO_INFERENCE) {
        config.allowed_inferences = GPU_PGRAPH_INFER_ALL;
    }
    if (which == CASE_GROUP_OFF) {
        config.output_groups = 0u;
    }
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    const uint32_t back = d3d8_device_load32(DEV_RENDER_TARGET);
    const uint32_t back_data = surface_data(back);
    make_target(TARGET_A, which == CASE_PITCH ? 128u : 640u, which == CASE_PITCH ? 64u : 480u);
    store(TARGET_A + D3D8_SURFACE_DATA, TARGET_A_DATA);
    stream_builder first = {0};
    build_frame_one(&first);
    stream_builder second = {0};
    build_small_target_draw(&second, 640u, 480u, 3u);
    stream_builder tail = {0};
    build_frame_two(&tail);
    switch (which) {
    case CASE_UNKNOWN_DESTINATION:
        push(&first);
        push_blit_packet(back_data, 0x00600000u, 0xAu, BLIT_PITCHES_640, 0u, 0u, 128u | (64u << 16));
        break;
    case CASE_BYTE_FORMAT:
        push(&first);
        push_blit_packet(back_data, back_data, 1u, 0x0A000A00u, 0u, 0u, 128u | (64u << 16));
        break;
    case CASE_PITCH:
        push_pending(&first);
        bind_target(TARGET_A);
        build_small_target_draw(&second, 128u, 64u, 3u);
        push_pending(&second);
        push_blit_packet(back_data, TARGET_A_DATA, 0xAu, BLIT_PITCHES_640, 0u, 0u, 16u | (16u << 16));
        bind_target(back);
        break;
    case CASE_PAST_IMAGE:
        push(&first);
        push_blit_packet(back_data, back_data, 0xAu, BLIT_PITCHES_640, 0u, 100u << 16, 128u | (480u << 16));
        break;
    case CASE_MID_DRAW:
        push(&first);
        push_blit_packet(back_data, TARGET_A_DATA, 0xAu, BLIT_PITCHES_640, 0u, 0u, 128u | (64u << 16));
        push(&tail);
        break;
    case CASE_LATER_PASS:
        push_pending(&first);
        push_blit_packet(back_data, TARGET_A_DATA, 0xAu, BLIT_PITCHES_640, 0u, 0u, 128u | (64u << 16));
        bind_target(TARGET_A);
        push_pending(&second);
        bind_target(back);
        break;
    case CASE_NO_INFERENCE:
    case CASE_GROUP_OFF:
        push(&first);
        push_blit_packet(back_data, back_data, 0xAu, BLIT_PITCHES_640, 0u, 300u | (300u << 16), 128u | (64u << 16));
        break;
    }
    d3d8_gpu_kick();
    present();
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && stats.frames_refused == 1u && stats.frames_replayed == 0u && stats.copies_applied == 0u);
    CHECK(strstr(stats.error, expected) != NULL);
    if (strstr(stats.error, expected) == NULL) {
        printf("  refusal was: %s\n", stats.error);
    }
    stream_free(&first);
    stream_free(&second);
    stream_free(&tail);
    d3d8_swap_replay_disable();
    environment_end();
}

static void test_blit_refusals_in_the_replay(const char *selector)
{
    printf("test_blit_refusals_in_the_replay (%s)\n", selector);
    test_blit_refusal(selector, CASE_UNKNOWN_DESTINATION, "holds no image of the destination surface (Data 0x0600000)");
    test_blit_refusal(selector, CASE_BYTE_FORMAT, "the replay's surfaces are A8R8G8B8 images");
    test_blit_refusal(selector, CASE_PITCH, "tightly packed");
    test_blit_refusal(selector, CASE_PAST_IMAGE, "reaches past an image");
    test_blit_refusal(selector, CASE_MID_DRAW, "is mid-draw at the blit");
    test_blit_refusal(selector, CASE_LATER_PASS, "is drawn into by a later pass of the frame");
    test_blit_refusal(selector, CASE_NO_INFERENCE, "INFERRED and not allowed");
    test_blit_refusal(selector, CASE_GROUP_OFF, "only the 3D subchannel 0 is decoded");
}

/* The real CopyRects of the retail boot, ported (T555, equal to the original dword for dword): the whole back buffer onto the
 * front buffer of CreateDevice's surfaces, replayed over the two passes. The recorded words are the boot's. */
static void test_blit_copy_rects_real(const char *selector)
{
    printf("test_blit_copy_rects_real (%s)\n", selector);
    begin_device(true);
    d3d8_swap_replay_config config = blit_config(selector, GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    const uint32_t back = d3d8_get_back_buffer(0);
    const uint32_t front = d3d8_get_back_buffer(-1);
    CHECK(back == d3d8_device_load32(DEV_RENDER_TARGET) && front != back);
    const uint32_t back_data = surface_data(back);
    const uint32_t front_data = surface_data(front);
    CHECK(back_data != 0u && front_data != 0u && back_data != front_data);
    printf("  back Data 0x%07X, front Data 0x%07X\n", (unsigned)back_data, (unsigned)front_data);
    CHECK(d3d8_swap_replay_get_stats().copies_pending == 0u);

    stream_builder first = {0};
    build_frame_one(&first);
    push_pending(&first);
    bind_target(front);
    stream_builder second = {0};
    build_small_target_draw(&second, 640u, 480u, 3u);
    push_pending(&second);
    bind_target(back);
    const size_t before_copy = d3d8_gpu_stream_count();
    (void)d3d8_copy_rects(back, 0u, 0u, front, 0u);
    d3d8_gpu_kick();
    /* the recording holds the boot's seven words on subchannels 3 and 2, in order, and nothing else off subchannel 0 but fences */
    static const struct {
        uint32_t subchannel;
        uint32_t method;
    } shape[7] = {{3u, 0x308u}, {3u, 0x30Cu}, {3u, 0x300u}, {3u, 0x304u}, {2u, 0x300u}, {2u, 0x304u}, {2u, 0x308u}};
    const uint32_t expected[7] = {back_data, front_data, 0xAu, BLIT_PITCHES_640, 0u, 0u, 0x01E00280u};
    size_t seen = 0u;
    for (size_t index = before_copy; index < d3d8_gpu_stream_count(); index++) {
        const d3d8_gpu_command command = d3d8_gpu_stream_at(index);
        if (command.subchannel == 2u || command.subchannel == 3u) {
            CHECK(seen < 7u);
            if (seen < 7u) {
                CHECK(command.subchannel == shape[seen].subchannel && command.method == shape[seen].method);
                CHECK(command.data == expected[seen]);
            }
            seen++;
        }
    }
    CHECK(seen == 7u);
    CHECK(d3d8_swap_replay_get_stats().copies_pending == 1u); /* decoded at the kick, no present has applied it yet */
    present();
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_refused == 0u && stats.copies_applied == 1u && stats.passes_replayed == 2u);
    CHECK(stats.copies_pending == 0u);
    const gpu_image *front_image = d3d8_swap_replay_surface_image(front_data);
    const gpu_image *back_image = d3d8_swap_replay_surface_image(back_data);
    CHECK(front_image != NULL && back_image != NULL && front_image->pixels != NULL && back_image->pixels != NULL);
    if (front_image != NULL && back_image != NULL && front_image->pixels != NULL && back_image->pixels != NULL) {
        CHECK(front_image->width == 640u && front_image->height == 480u);
        CHECK(pixel_is(front_image, 160u, 120u, 255u, 0u, 0u, 255u)); /* the back buffer's triangle is on the front buffer */
        CHECK(is_black(front_image, 480u, 336u));                    /* and the front's own triangle is gone: a whole copy */
        CHECK(memcmp(front_image->pixels, back_image->pixels, (size_t)back_image->stride_bytes * back_image->height) == 0);
        CHECK(count_not(front_image, 0u, 0u, 0u, 255u) > 10000u);     /* something to compare */
    }
    stream_free(&first);
    stream_free(&second);
    d3d8_swap_replay_disable();
    environment_end();
}

/* Without the BLIT group the replay keeps no image of any surface. */
static void test_blit_group_off_keeps_nothing(const char *selector)
{
    printf("test_blit_group_off_keeps_nothing (%s)\n", selector);
    begin_device(true);
    d3d8_swap_replay_config config = base_config(selector);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    const uint32_t back = d3d8_device_load32(DEV_RENDER_TARGET);
    stream_builder first = {0};
    build_frame_one(&first);
    push(&first);
    present();
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.copies_applied == 0u && stats.copies_pending == 0u);
    CHECK(d3d8_swap_replay_surface_image(surface_data(back)) == NULL);
    stream_free(&first);
    d3d8_swap_replay_disable();
    environment_end();
}

/* A target whose header has no Data word is not named: nothing is kept for it. */
static void test_blit_unnamed_surface_is_not_kept(const char *selector)
{
    printf("test_blit_unnamed_surface_is_not_kept (%s)\n", selector);
    begin_device(true);
    d3d8_swap_replay_config config = blit_config(selector, GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    const uint32_t back = d3d8_device_load32(DEV_RENDER_TARGET);
    make_target(TARGET_A, 32u, 16u);
    store(TARGET_A + D3D8_SURFACE_DATA, 0u);
    stream_builder first = {0};
    build_frame_one(&first);
    push_pending(&first);
    bind_target(TARGET_A);
    stream_builder second = {0};
    build_small_target_draw(&second, 32u, 16u, 3u);
    push_pending(&second);
    bind_target(back);
    present();
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.passes_replayed == 2u);
    CHECK(d3d8_swap_replay_surface_image(0u) == NULL);
    CHECK(d3d8_swap_replay_surface_image(surface_data(back)) != NULL); /* the named one is kept */
    stream_free(&first);
    stream_free(&second);
    d3d8_swap_replay_disable();
    environment_end();
}

/* A surface drawn again keeps its newest image, in the same slot. */
static void test_blit_kept_image_is_replaced(const char *selector)
{
    printf("test_blit_kept_image_is_replaced (%s)\n", selector);
    begin_device(true);
    d3d8_swap_replay_config config = blit_config(selector, GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    const uint32_t back_data = surface_data(d3d8_device_load32(DEV_RENDER_TARGET));
    stream_builder first = {0};
    build_frame_one(&first);
    push(&first);
    present();
    const gpu_image *image = d3d8_swap_replay_surface_image(back_data);
    CHECK(image != NULL && image->pixels != NULL && pixel_is(image, 160u, 120u, 255u, 0u, 0u, 255u));
    stream_builder again = {0};
    build_frame_two(&again);
    push(&again);
    present();
    image = d3d8_swap_replay_surface_image(back_data);
    CHECK(image != NULL && image->pixels != NULL);
    if (image != NULL && image->pixels != NULL) {
        CHECK(is_black(image, 160u, 120u));  /* the first frame's triangle is gone */
        CHECK(!is_black(image, 480u, 336u)); /* the second frame's is there */
    }
    CHECK(d3d8_swap_replay_get_stats().surfaces_evicted == 0u);
    stream_free(&first);
    stream_free(&again);
    d3d8_swap_replay_disable();
    environment_end();
}

/* More surfaces than the store holds: the oldest are evicted and counted, the newest are kept. */
static void test_blit_surface_eviction(const char *selector)
{
    printf("test_blit_surface_eviction (%s)\n", selector);
    begin_device(true);
    d3d8_swap_replay_config config = blit_config(selector, GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL);
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    const uint32_t back = d3d8_device_load32(DEV_RENDER_TARGET);
    const uint32_t extra = D3D8_SWAP_REPLAY_SURFACES + 1u; /* with the back buffer: two more surfaces than slots */
    CHECK(extra * 0x40u + 0x100u < 0x4000u);
    stream_builder first = {0};
    build_frame_one(&first);
    push_pending(&first);
    stream_builder draws[D3D8_SWAP_REPLAY_SURFACES + 1u];
    memset(draws, 0, sizeof draws);
    for (uint32_t i = 0u; i < extra; i++) {
        const uint32_t header = HEADER_SCRATCH + 0x200u + i * 0x40u;
        make_target(header, 32u, 16u);
        store(header + D3D8_SURFACE_DATA, 0x00600000u + i * 0x1000u);
        bind_target(header);
        build_small_target_draw(&draws[i], 32u, 16u, 3u);
        push_pending(&draws[i]);
    }
    bind_target(back);
    present();
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.passes_replayed == extra + 1u);
    CHECK(stats.surfaces_evicted == 2u);
    CHECK(d3d8_swap_replay_surface_image(surface_data(back)) == NULL);           /* the oldest two went */
    CHECK(d3d8_swap_replay_surface_image(0x00600000u) == NULL);
    CHECK(d3d8_swap_replay_surface_image(0x00600000u + 1u * 0x1000u) != NULL);
    CHECK(d3d8_swap_replay_surface_image(0x00600000u + (extra - 1u) * 0x1000u) != NULL);
    uint32_t kept = 0u;
    for (uint32_t i = 0u; i < extra; i++) {
        kept += d3d8_swap_replay_surface_image(0x00600000u + i * 0x1000u) != NULL ? 1u : 0u;
    }
    CHECK(kept == D3D8_SWAP_REPLAY_SURFACES);
    stream_free(&first);
    for (uint32_t i = 0u; i < extra; i++) {
        stream_free(&draws[i]);
    }
    d3d8_swap_replay_disable();
    environment_end();
}

/* --- T510: a render target bound as a texture --------------------------------------------------------------------------------- */

#define RT_PRODUCER TARGET_A
#define RT_CONSUMER TARGET_B
#define RT_PRODUCER_DATA 0x00500000u
#define RT_CONSUMER_DATA 0x00600000u
#define RT_EDGE_WIDTH 128u
#define RT_EDGE_HEIGHT 64u
#define RT_INFERENCES (GPU_PGRAPH_INFER_COMBINER_REPLAY | D3D8_SWAP_REPLAY_INFER_RT_TEXTURE | GPU_PGRAPH_INFER_OUTPUT_PINNED_STATE)
#define RT_ADDRESS_METHOD 0x1B08u
#define RT_CONTROL_METHOD 0x1B0Cu
#define RT_FILTER_METHOD 0x1B14u
#define RT_CLAMP 0x00000303u
#define RT_WRAP 0x00000101u
#define RT_LINEAR 0x02062000u
#define RT_FIRST_CONSUMER_VERTEX 6u

static d3d8_swap_replay_config rt_config(const char *selector)
{
    d3d8_swap_replay_config config = combiner_config(selector);
    /* every group: Swap's own SetRenderTarget commands are in the stream (the texture hook inside Swap drains the ring, so
     * present()'s rewind of them finds nothing to drop) and the SURFACE group is what decodes them */
    config.output_groups = GPU_PGRAPH_OUTPUT_ALL_MEASURED;
    config.render_target_texture = true;
    config.allowed_inferences = GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_OUTPUT_ALL | RT_INFERENCES;
    return config;
}

/* A linear A8R8G8B8 target header of its own Data and a Size word with the tight pitch (the cloned back buffer's format word). */
static void make_rt_target(uint32_t header, uint32_t data, uint32_t width, uint32_t height)
{
    make_target(header, width, height);
    store(header + D3D8_SURFACE_DATA, data);
    store(header + D3D8_SURFACE_SIZE, ((width * 4u / 64u - 1u) << 24) | ((height - 1u) << 12) | (width - 1u));
}

/* Three vertices covering the whole target past its edges (clip (-1, -1), (3, -1), (-1, 3)) after the six of write_vertices. */
static void write_consumer_vertices(void)
{
    static const float corners[3][2] = {{-1.0f, -1.0f}, {3.0f, -1.0f}, {-1.0f, 3.0f}};
    for (uint32_t i = 0u; i < 3u; i++) {
        const float_vertex vertex = {{corners[i][0], corners[i][1], 0.5f}, {0.0f, 0.0f, 1.0f, 1.0f}};
        uint32_t words[7];
        memcpy(words, &vertex, sizeof words);
        for (uint32_t word = 0u; word < 7u; word++) {
            store(MEMORY_BASE + (RT_FIRST_CONSUMER_VERTEX + i) * (uint32_t)sizeof vertex + word * 4u, words[word]);
        }
    }
}

/* The producer pass: bind `header`, pass-through combiner, the red upper left triangle then the yellow lower right one. */
static void rt_producer_pass(uint32_t header)
{
    bind_target(header);
    uint32_t words[GPU_PGRAPH_COMBINER_WORDS];
    build_combiner_words(pass_entries, sizeof pass_entries / sizeof pass_entries[0], words);
    stream_builder stream = {0};
    stream_pair(&stream, 0x030Cu, 0u); /* a SetRenderTarget in an earlier frame wrote the depth and stencil enables */
    stream_pair(&stream, 0x032Cu, 0u);
    stream_pixel_shader(&stream, words);
    build_frame_one(&stream);
    build_frame_two(&stream);
    push_pending(&stream);
    stream_free(&stream);
}

/* The textured combiner reading t1 instead of t0 (the register 9 inputs), for the stage 1 scenes. */
static const combiner_entry stage_one_entries[] = {{34u, 0x09200000u}, {0u, 0x19300000u}, {45u, 0xC0u},
                                                   {26u, 0xC0u},       {53u, 0x11101u},   {8u, 0x0Cu},
                                                   {9u, 0x1C80u}};
#define RT_STAGE_SPACING 0x40u /* the address, control and filter methods of stage n sit n * 0x40 above stage 0's */

/* The consumer pass: the textured combiner (stage program 1, the stream's own word), address and filter words for `stage`, one
 * triangle over the whole target. SetTexture is called BEFORE the draw is pushed, as the title does. */
static void rt_consumer_stage_pass(uint32_t header, uint32_t stage, uint32_t texture, uint32_t address, uint32_t filter)
{
    if (header != 0u) { /* 0: no SetRenderTarget, the draw goes to the target the device already has */
        bind_target(header);
    }
    if (texture != UINT32_MAX) {
        d3d8_set_texture(stage, texture);
    }
    uint32_t words[GPU_PGRAPH_COMBINER_WORDS];
    build_combiner_words(stage == 0u ? textured_entries : stage_one_entries, TEXTURED_COUNT, words);
    stream_builder stream = {0};
    /* the program and arrays again (the pass may be the first of the frame) */
    stream_pair(&stream, 0x030Cu, 0u);
    stream_pair(&stream, 0x032Cu, 0u);
    stream_pair(&stream, GPU_PGRAPH_EXECUTION_MODE, 6u);
    stream_program(&stream, 0u, synthetic_program, 1u);
    stream_pair(&stream, GPU_PGRAPH_PROGRAM_START, 0u);
    stream_array(&stream, 1u, MEMORY_BASE, array_format(28u, 3u, GPU_PGRAPH_TYPE_F));
    stream_array(&stream, 2u, MEMORY_BASE + 12u, array_format(28u, 4u, GPU_PGRAPH_TYPE_F));
    stream_pixel_shader(&stream, words);
    stream_pair(&stream, STAGE_PROGRAM_METHOD, 1u << (5u * stage));
    stream_pair(&stream, RT_ADDRESS_METHOD + stage * RT_STAGE_SPACING, address);
    stream_pair(&stream, RT_CONTROL_METHOD + stage * RT_STAGE_SPACING, 0x0003FFC0u);
    stream_pair(&stream, RT_FILTER_METHOD + stage * RT_STAGE_SPACING, filter);
    static const float c3_zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    stream_constants(&stream, 3u, c3_zero, 4u);
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, RT_FIRST_CONSUMER_VERTEX, 3u);
    push_pending(&stream);
    stream_free(&stream);
}

static void rt_consumer_pass(uint32_t header, uint32_t texture, uint32_t address, uint32_t filter)
{
    rt_consumer_stage_pass(header, 0u, texture, address, filter);
}

/* A producer that draws only the red triangle: a picture that is not the full producer's (the yellow one is missing). */
static void rt_red_pass(uint32_t header)
{
    bind_target(header);
    uint32_t words[GPU_PGRAPH_COMBINER_WORDS];
    build_combiner_words(pass_entries, sizeof pass_entries / sizeof pass_entries[0], words);
    stream_builder stream = {0};
    stream_pair(&stream, 0x030Cu, 0u);
    stream_pair(&stream, 0x032Cu, 0u);
    stream_pixel_shader(&stream, words);
    build_frame_one(&stream);
    push_pending(&stream);
    stream_free(&stream);
}

/* A fresh device with the two headers and the replay enabled. */
static bool rt_begin(const d3d8_swap_replay_config *config)
{
    begin_device(true);
    write_consumer_vertices();
    make_rt_target(RT_PRODUCER, RT_PRODUCER_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    make_rt_target(RT_CONSUMER, RT_CONSUMER_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    return d3d8_swap_replay_enable(config, NULL, 0u);
}

static void rt_end(void)
{
    d3d8_swap_replay_disable();
    environment_end();
}

/* Replay one frame and say whether it refused, with the error text. */
static d3d8_swap_replay_stats rt_present(void)
{
    present();
    return d3d8_swap_replay_get_stats();
}

static bool images_equal(const gpu_image *left, const gpu_image *right)
{
    if (left == NULL || right == NULL || left->pixels == NULL || right->pixels == NULL || left->width != right->width ||
        left->height != right->height) {
        return false;
    }
    for (uint32_t y = 0u; y < left->height; y++) {
        if (memcmp(left->pixels + gpu_image_offset(left, 0u, y), right->pixels + gpu_image_offset(right, 0u, y),
                   (size_t)left->width * 4u) != 0) {
            return false;
        }
    }
    return true;
}

/* TSFP_T510_LOOK=dir writes the replayed images of this scene as PNGs, for looking at them (nothing is asserted from the files). */
static void rt_look(const char *name, const gpu_image *image)
{
    const char *directory = getenv("TSFP_T510_LOOK");
    if (directory == NULL || image == NULL || image->pixels == NULL) {
        return;
    }
    char path[600];
    snprintf(path, sizeof path, "%s/%s.png", directory, name);
    (void)gpu_png_write_rgba(path, image->pixels, image->width, image->height, image->stride_bytes);
}

/* T632: a swizzled DXT1 512x512 texture read from guest memory. Every block is red except the one at swizzle index 0x3FFF
 * (block 127, 127), blue: the clamped sample of the whole consumer target lands on the last texel. */
static void test_rt_texture_dxt1_guest(const char *selector)
{
    printf("test_rt_texture_dxt1_guest (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    d3d8_swap_replay_config config = rt_config(selector);
    CHECK(rt_begin(&config));
    const uint32_t dxt1_data = 0x00E00000u;
    map_fixed(dxt1_data, 0x20000u);
    for (uint32_t block = 0u; block < 128u * 128u; block++) {
        const bool last = block == 0x3FFFu;
        store(dxt1_data + block * 8u, last ? 0x0000001Fu : 0x0000F800u); /* color0 (565 LE), color1 = 0 */
        store(dxt1_data + block * 8u + 4u, 0u);                           /* every texel index 0 */
    }
    store(TARGET_C + D3D8_SURFACE_DATA, dxt1_data);
    store(TARGET_C + D3D8_SURFACE_FORMAT, 0x09910C29u);
    store(TARGET_C + D3D8_SURFACE_SIZE, 0u);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    const d3d8_swap_replay_stats stats = rt_present();
    CHECK(!stats.latched && stats.frames_refused == 0u && stats.frames_replayed == 1u);
    CHECK(stats.rt_texture_draws == 1u);
    const gpu_image *image = d3d8_swap_replay_last_frame();
    CHECK(image != NULL && image->pixels != NULL);
    if (image != NULL && image->pixels != NULL) {
        const uint8_t *texel = image->pixels + gpu_image_offset(image, image->width - 1u, image->height - 1u);
        CHECK(texel[0] == 0u && texel[2] == 255u && texel[3] == 255u);
    }
    rt_end();
}

/* T735: the DXT1 texture is also taken with the wrap address word 0x101 (U and V repeat), the sampler gets REPEAT. */
static void test_rt_texture_dxt1_wrap(const char *selector)
{
    printf("test_rt_texture_dxt1_wrap (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    d3d8_swap_replay_config config = rt_config(selector);
    CHECK(rt_begin(&config));
    const uint32_t dxt1_data = 0x00E00000u;
    map_fixed(dxt1_data, 0x20000u);
    for (uint32_t block = 0u; block < 128u * 128u; block++) {
        store(dxt1_data + block * 8u, 0x0000F800u);
        store(dxt1_data + block * 8u + 4u, 0u);
    }
    store(TARGET_C + D3D8_SURFACE_DATA, dxt1_data);
    store(TARGET_C + D3D8_SURFACE_FORMAT, 0x09910C29u);
    store(TARGET_C + D3D8_SURFACE_SIZE, 0u);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_WRAP, RT_LINEAR);
    const d3d8_swap_replay_stats stats = rt_present();
    CHECK(!stats.latched && stats.frames_refused == 0u && stats.frames_replayed == 1u);
    CHECK(stats.rt_texture_draws == 1u);
    rt_end();
}

/* The two-target producer then consumer: the consumer image is the producer's, texel for texel. */
static void test_rt_texture_exact_pixels(const char *selector)
{
    printf("test_rt_texture_exact_pixels (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    d3d8_swap_replay_config config = rt_config(selector);
    CHECK(rt_begin(&config));
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, RT_PRODUCER, RT_CLAMP, RT_LINEAR);
    const d3d8_swap_replay_stats stats = rt_present();
    CHECK(!stats.latched && stats.frames_refused == 0u && stats.frames_replayed == 1u);
    CHECK(stats.passes_replayed == 2u && stats.draws == 3u);
    CHECK(stats.rt_texture_draws == 1u && stats.texture_bindings >= 1u);
    CHECK((stats.used_inferences & GPU_PGRAPH_INFER_OUTPUT_TEXTURE_BRIDGE) != 0u);
    CHECK((stats.used_inferences & GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING) != 0u);
    const gpu_image *consumer = d3d8_swap_replay_last_frame();
    uint32_t producer_target = 0u;
    const gpu_image *producer = d3d8_swap_replay_offscreen_frame(0u, &producer_target);
    CHECK(consumer != NULL && producer != NULL && producer_target == RT_PRODUCER);
    rt_look("rt_producer_pass", producer);
    rt_look("rt_consumer_pass_texel_centres", consumer);
    if (consumer != NULL && producer != NULL && consumer->pixels != NULL && producer->pixels != NULL) {
        CHECK(consumer->width == RT_EDGE_WIDTH && consumer->height == RT_EDGE_HEIGHT);
        /* the producer is a picture of three colours, the consumer is that picture and nothing else */
        CHECK(pixel_is(producer, 32u, 12u, 255u, 0u, 0u, 255u) && pixel_is(producer, 96u, 48u, 255u, 255u, 0u, 255u));
        CHECK(pixel_is(producer, 2u, 2u, 0u, 0u, 0u, 255u) && pixel_is(producer, 125u, 2u, 0u, 0u, 0u, 255u));
        CHECK(count_not(producer, 0u, 0u, 0u, 255u) > 300u);
        CHECK(images_equal(consumer, producer));
        CHECK(consumer->pixels != producer->pixels);
        CHECK(pixel_is(consumer, 32u, 12u, 255u, 0u, 0u, 255u) && pixel_is(consumer, 96u, 48u, 255u, 255u, 0u, 255u));
        CHECK(pixel_is(consumer, 2u, 2u, 0u, 0u, 0u, 255u));
    }
    char text[1100];
    CHECK(d3d8_swap_replay_rt_texture_summary(text, sizeof text) > 0u);
    CHECK(strstr(text, "RENDER TARGET TEXTURE (T510)") != NULL && strstr(text, "INFERRED") != NULL &&
          strstr(text, "never a white or default texture") != NULL);
    char counted[160];
    snprintf(counted, sizeof counted, "%llu SetTexture binding(s) noted, 1 (draw, stage) sample(s) read an image an earlier pass",
             (unsigned long long)stats.texture_bindings);
    CHECK(strstr(text, counted) != NULL && strstr(text, "of 3 replayed draw(s)") != NULL);
    CHECK(d3d8_swap_replay_texture_census_count() == 1u);
    char census[300];
    uint64_t draws = 0u;
    CHECK(d3d8_swap_replay_texture_census_at(0u, census, sizeof census, &draws) && draws == 1u);
    CHECK(strstr(census, "stage 0 from render target 0x") != NULL && strstr(census, "Data 0x0500000") != NULL &&
          strstr(census, "128x64") != NULL && strstr(census, "Format 0x00011229") != NULL &&
          strstr(census, "drawn by an earlier pass of the same frame") != NULL);
    CHECK(!d3d8_swap_replay_texture_census_at(1u, census, sizeof census, &draws));
    rt_end();

    /* A half texel to the right of the texel centres: bilinear reads the mean of two texels, a nearest sample a single one
     * (this pins the filter the stream's 0x02062000 names). Pixel (56, 28) sits at the right edge of the red triangle. */
    write_synthetic_vertex_module(draw_vertex_texel_half_words, sizeof draw_vertex_texel_half_words);
    config = rt_config(selector);
    CHECK(rt_begin(&config));
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, RT_PRODUCER, RT_CLAMP, RT_LINEAR);
    const d3d8_swap_replay_stats half = rt_present();
    CHECK(!half.latched && half.rt_texture_draws == 1u);
    consumer = d3d8_swap_replay_last_frame();
    producer = d3d8_swap_replay_offscreen_frame(0u, NULL);
    rt_look("rt_consumer_pass_half_texel", consumer);
    if (consumer != NULL && producer != NULL && consumer->pixels != NULL && producer->pixels != NULL) {
        uint32_t mixed = 0u;
        for (uint32_t y = 0u; y < consumer->height; y++) {
            for (uint32_t x = 0u; x + 1u < consumer->width; x++) {
                const uint8_t *here = consumer->pixels + gpu_image_offset(consumer, x, y);
                const uint8_t *left = producer->pixels + gpu_image_offset(producer, x, y);
                const uint8_t *right = producer->pixels + gpu_image_offset(producer, x + 1u, y);
                if (left[0] == 0u && right[0] == 255u && left[1] == 0u && right[1] == 0u) { /* black next to red */
                    CHECK(here[0] >= 127u && here[0] <= 128u && here[1] == 0u && here[2] == 0u && here[3] == 255u);
                    mixed++;
                }
            }
        }
        CHECK(mixed > 5u); /* the red triangle's left edge crosses many rows */
        CHECK(pixel_is(consumer, 4u, 4u, 0u, 0u, 0u, 255u)); /* the black field is still black */
    }
    rt_end();
}

/* Does a census line holding `first` and `second` exist, and how many draws does it count? */
static bool census_has(const char *first, const char *second, uint64_t *draws)
{
    char line[300];
    uint64_t count = 0u;
    for (size_t i = 0u; d3d8_swap_replay_texture_census_at(i, line, sizeof line, &count); i++) {
        if (strstr(line, first) != NULL && (second == NULL || strstr(line, second) != NULL)) {
            if (draws != NULL) {
                *draws = count;
            }
            return true;
        }
    }
    return false;
}

/* One refusal. A replay: require the latch, a refusal naming `words` and no frame. A census: nothing latches, one frame was
 * classified and the consumer's draw is counted once under `REFUSED stage 0 texture: <category>`, naming the target it draws into. */
static void expect_rt_refusal(const d3d8_swap_replay_stats *stats, bool census, const char *words, const char *more,
                              const char *category)
{
    if (census) {
        CHECK(!stats->latched && stats->frames_refused == 0u && stats->frames_replayed == 0u && stats->census_frames == 1u);
        CHECK(stats->rt_texture_draws == 0u && d3d8_swap_replay_last_frame() == NULL);
        char heading[120];
        char into[64];
        uint64_t draws = 0u;
        snprintf(heading, sizeof heading, "REFUSED stage 0 texture: %s,", category);
        snprintf(into, sizeof into, "(drawing into target 0x%08X)", (unsigned)RT_CONSUMER);
        CHECK(census_has(heading, into, &draws) && draws == 1u);
        return;
    }
    CHECK(stats->latched && stats->frames_refused == 1u && stats->frames_replayed == 0u);
    CHECK(stats->rt_texture_draws == 0u);
    if (stats->error[0] != '\0') {
        CHECK(strstr(stats->error, "render target texture") != NULL);
    }
    if (strstr(stats->error, words) == NULL || (more != NULL && strstr(stats->error, more) == NULL)) {
        printf("  expected \"%s\" and \"%s\" in: %s\n", words, more != NULL ? more : "", stats->error);
    }
    CHECK(strstr(stats->error, words) != NULL);
    CHECK(more == NULL || strstr(stats->error, more) != NULL);
    CHECK(d3d8_swap_replay_last_frame() == NULL);
}

/* A texture header of its own over the producer's bytes: the same Data, size and tight pitch, the A8R8G8B8 Format. */
static void rt_second_header(uint32_t data, uint32_t width, uint32_t height)
{
    make_rt_target(TARGET_C, data, width, height);
}

/* Every refusal of docs/d3d8-copy-composition.md "T510", each its own scene. As a replay the frame latches with the named refusal, as
 * a census (no device) the draw is counted under the refusal's category. */
static void rt_refusal_scenes(const char *selector, bool census)
{
    printf("test_rt_texture_refusals (%s, %s)\n", selector != NULL ? selector : "no device", census ? "census" : "replay");
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    d3d8_swap_replay_config config = rt_config(selector);
    config.render_target_texture_census = census;
    d3d8_swap_replay_stats stats;

    /* no earlier image: the texture is a surface no pass of the frame draws into */
    CHECK(rt_begin(&config));
    rt_second_header(0x00700000u, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    expect_rt_refusal(&stats, census, "no earlier image", "Data 0x0700000", "no earlier image");
    rt_end();

    /* out of order: the only pass that draws into the texture's surface comes AFTER the draw that samples it */
    CHECK(rt_begin(&config));
    d3d8_set_texture(0u, RT_PRODUCER); /* bound before anything is drawn */
    rt_consumer_pass(RT_CONSUMER, UINT32_MAX, RT_CLAMP, RT_LINEAR);
    rt_producer_pass(RT_PRODUCER);
    stats = rt_present();
    expect_rt_refusal(&stats, census, "out of order", "starts at draw 1", "out of order");
    rt_end();

    /* same-target feedback: the draw samples the target it draws into */
    CHECK(rt_begin(&config));
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, RT_CONSUMER, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    expect_rt_refusal(&stats, census, "same-target feedback", "Data 0x0600000", "same-target feedback");
    rt_end();

    /* ambiguous: two headers name the same bytes and both are drawn into this frame */
    CHECK(rt_begin(&config));
    rt_second_header(RT_PRODUCER_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    rt_producer_pass(RT_PRODUCER);
    rt_producer_pass(TARGET_C);
    rt_consumer_pass(RT_CONSUMER, RT_PRODUCER, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    expect_rt_refusal(&stats, census, "ambiguous mapping", "more than one pass or header", "ambiguous mapping");
    rt_end();

    /* ambiguous the other way: an exact producer, and another target drawn into this frame overlaps the texture at another offset */
    CHECK(rt_begin(&config));
    rt_second_header(RT_PRODUCER_DATA + 0x1000u, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    rt_producer_pass(RT_PRODUCER);
    rt_producer_pass(TARGET_C);
    rt_consumer_pass(RT_CONSUMER, RT_PRODUCER, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    expect_rt_refusal(&stats, census, "ambiguous mapping", "also overlaps render target", "ambiguous mapping");
    rt_end();

    /* an alias at another offset: the texture starts 0x1000 bytes inside the producer's surface */
    CHECK(rt_begin(&config));
    rt_second_header(RT_PRODUCER_DATA + 0x1000u, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    expect_rt_refusal(&stats, census, "aliases render target", "different offset", "alias");
    rt_end();

    /* the same from below: the texture starts 0x1000 bytes BEFORE the producer and runs into it (the whole texture, not one row, is the range) */
    CHECK(rt_begin(&config));
    rt_second_header(RT_PRODUCER_DATA - 0x1000u, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    expect_rt_refusal(&stats, census, "aliases render target", "different offset", "alias");
    rt_end();

    /* bytes that touch the producer without sharing one are no alias: the texture ends exactly where the producer starts, and starts exactly where it ends */
    CHECK(rt_begin(&config));
    rt_second_header(RT_PRODUCER_DATA - RT_EDGE_WIDTH * RT_EDGE_HEIGHT * 4u, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    expect_rt_refusal(&stats, census, "no earlier image", "Data 0x04F8000", "no earlier image");
    rt_end();
    CHECK(rt_begin(&config));
    rt_second_header(RT_PRODUCER_DATA + RT_EDGE_WIDTH * RT_EDGE_HEIGHT * 4u, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    expect_rt_refusal(&stats, census, "no earlier image", "Data 0x0508000", "no earlier image");
    rt_end();

    /* a texture header the producer does not match exactly: the size differs */
    CHECK(rt_begin(&config));
    rt_second_header(RT_PRODUCER_DATA, 64u, 64u);
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    expect_rt_refusal(&stats, census, "not exactly render target", "64x64", "inexact relationship");
    rt_end();

    /* each of the four things the relationship needs, differing alone: the producer's pitch (padded to 576), width (64 at the same
     * pitch), height (32) and Format word, against a tight 128x64 A8R8G8B8 texture header over the same bytes */
    static const struct {
        uint32_t producer_size;   /* the producer header's Size word, 0 to leave it */
        uint32_t producer_format; /* its Format word, 0 to leave it */
        const char *more;         /* what the refusal says of the producer */
    } mismatches[] = {
        {(8u << 24) | ((RT_EDGE_HEIGHT - 1u) << 12) | (RT_EDGE_WIDTH - 1u), 0u, "(128x64, pitch 576"},
        {(7u << 24) | ((RT_EDGE_HEIGHT - 1u) << 12) | 63u, 0u, "(64x64, pitch 512"},
        {(7u << 24) | (31u << 12) | (RT_EDGE_WIDTH - 1u), 0u, "(128x32, pitch 512"},
        {0u, 0x00011228u, "Format 0x00011228)"},
    };
    for (size_t i = 0u; i < sizeof mismatches / sizeof mismatches[0]; i++) {
        CHECK(rt_begin(&config));
        rt_second_header(RT_PRODUCER_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
        if (mismatches[i].producer_size != 0u) {
            store(RT_PRODUCER + D3D8_SURFACE_SIZE, mismatches[i].producer_size);
        }
        if (mismatches[i].producer_format != 0u) {
            store(RT_PRODUCER + D3D8_SURFACE_FORMAT, mismatches[i].producer_format);
        }
        rt_producer_pass(RT_PRODUCER);
        rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
        stats = rt_present();
        expect_rt_refusal(&stats, census, "not exactly render target", mismatches[i].more, "inexact relationship");
        rt_end();
    }

    /* T632: the title's swizzled DXT1 header is sampled now, a second level or another kind of DXT1 is still refused by name */
    CHECK(rt_begin(&config));
    rt_second_header(RT_PRODUCER_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    store(TARGET_C + D3D8_SURFACE_FORMAT, 0x09920C29u);
    store(TARGET_C + D3D8_SURFACE_SIZE, 0u);
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    expect_rt_refusal(&stats, census, "second level", "Format 0x09920C29", "second level");
    rt_end();
    CHECK(rt_begin(&config));
    rt_second_header(RT_PRODUCER_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    store(TARGET_C + D3D8_SURFACE_FORMAT, 0x08910C29u); /* not square */
    store(TARGET_C + D3D8_SURFACE_SIZE, 0u);
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    expect_rt_refusal(&stats, census, "unsupported format", "Format 0x08910C29", "unsupported format");
    rt_end();
    CHECK(rt_begin(&config));
    rt_second_header(RT_PRODUCER_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    store(TARGET_C + D3D8_SURFACE_FORMAT, 0x09910E29u); /* another colour byte */
    store(TARGET_C + D3D8_SURFACE_SIZE, 0u);
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    expect_rt_refusal(&stats, census, "unsupported format", "Format 0x09910E29", "unsupported format");
    rt_end();

    /* the Format word alone decides it: a measured header (it has a Size word) with another Format, and the right Format with no Size word */
    CHECK(rt_begin(&config));
    rt_second_header(RT_PRODUCER_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    store(TARGET_C + D3D8_SURFACE_FORMAT, 0x00011228u);
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    expect_rt_refusal(&stats, census, "unsupported format", "Format 0x00011228", "unsupported format");
    rt_end();
    CHECK(rt_begin(&config));
    rt_second_header(RT_PRODUCER_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    store(TARGET_C + D3D8_SURFACE_SIZE, 0u);
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    expect_rt_refusal(&stats, census, "unsupported format", "Format 0x00011229", "unsupported format");
    rt_end();

    /* a pitch that is not the tight four bytes per texel */
    CHECK(rt_begin(&config));
    rt_second_header(RT_PRODUCER_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    store(TARGET_C + D3D8_SURFACE_SIZE, (8u << 24) | ((RT_EDGE_HEIGHT - 1u) << 12) | (RT_EDGE_WIDTH - 1u));
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    expect_rt_refusal(&stats, census, "unsupported layout", "pitch 576", "unsupported layout");
    rt_end();

    /* a pitch narrower than the tight one is as unsupported as a wider one */
    CHECK(rt_begin(&config));
    rt_second_header(RT_PRODUCER_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    store(TARGET_C + D3D8_SURFACE_SIZE, (6u << 24) | ((RT_EDGE_HEIGHT - 1u) << 12) | (RT_EDGE_WIDTH - 1u));
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    expect_rt_refusal(&stats, census, "unsupported layout", "pitch 448", "unsupported layout");
    rt_end();

    /* an address mode the sampler does not implement (wrap) */
    CHECK(rt_begin(&config));
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, RT_PRODUCER, RT_WRAP, RT_LINEAR);
    stats = rt_present();
    expect_rt_refusal(&stats, census, "address mode is not clamp", "0x1B08", "address mode");
    rt_end();

    /* a filter other than the measured one */
    CHECK(rt_begin(&config));
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, RT_PRODUCER, RT_CLAMP, 0x01010000u);
    stats = rt_present();
    expect_rt_refusal(&stats, census, "filter is not the measured word", "0x02062000", "filter");
    CHECK(census || strstr(stats.error, "(0x1B14)") != NULL);
    rt_end();

    /* a stage that is unbound (SetTexture 0) and one whose binding was never observed */
    CHECK(rt_begin(&config));
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, 0u, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    expect_rt_refusal(&stats, census, "stage 0 is unbound", "SetTexture 0", "unbound");
    rt_end();
    CHECK(rt_begin(&config));
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, UINT32_MAX, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    expect_rt_refusal(&stats, census, "no SetTexture of stage 0 was observed", NULL, "no binding observed");
    rt_end();

    /* a texture the combiner does not read is not refused: an unsupported header on stage 1 beside a good stage 0 (bound first,
     * so the stage 1 note is the LAST one before the draw and must not answer for stage 0) */
    CHECK(rt_begin(&config));
    rt_second_header(RT_PRODUCER_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    store(TARGET_C + D3D8_SURFACE_FORMAT, 0x09910C29u);
    store(TARGET_C + D3D8_SURFACE_SIZE, 0u);
    rt_producer_pass(RT_PRODUCER);
    d3d8_set_texture(0u, RT_PRODUCER);
    d3d8_set_texture(1u, TARGET_C);
    rt_consumer_pass(RT_CONSUMER, UINT32_MAX, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    if (census) {
        CHECK(!stats.latched && stats.census_frames == 1u && stats.rt_texture_draws == 1u);
    } else {
        CHECK(!stats.latched && stats.frames_replayed == 1u && stats.rt_texture_draws == 1u);
    }
    rt_end();
}

static void test_rt_texture_refusals(const char *selector)
{
    rt_refusal_scenes(selector, false);
}

/* The binding follows the draws: a stage rebound between two draws of one pass gives each draw its own, and a frame that does
 * not call SetTexture keeps what the last frame left. */
static void test_rt_texture_binding_follows_the_draw(const char *selector)
{
    printf("test_rt_texture_binding_follows_the_draw (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    d3d8_swap_replay_config config = rt_config(selector);
    CHECK(rt_begin(&config));
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, RT_PRODUCER, RT_CLAMP, RT_LINEAR);
    d3d8_swap_replay_stats stats = rt_present();
    CHECK(!stats.latched && stats.rt_texture_draws == 1u);
    const uint64_t noted = stats.texture_bindings;
    /* frame 2: the same pass order, no SetTexture at all (the binding carries over), a repeat is not noted */
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, UINT32_MAX, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    if (stats.latched) {
        printf("  frame 2: %s\n", stats.error);
    }
    CHECK(!stats.latched && stats.frames_replayed == 2u && stats.rt_texture_draws == 2u);
    CHECK(stats.texture_bindings > noted); /* the library's own SetTexture calls inside Swap */
    rt_producer_pass(RT_PRODUCER);
    const uint64_t before = d3d8_swap_replay_get_stats().texture_bindings;
    d3d8_set_texture(0u, d3d8_device_load32(0xF88u)); /* the very binding the stage holds: not noted again */
    CHECK(d3d8_swap_replay_get_stats().texture_bindings == before);
    rt_end();
}

/* The option off, and the option on over a stream that reads no texture, give the same pixels. */
static void test_rt_texture_off_is_byte_identical(const char *selector)
{
    printf("test_rt_texture_off_is_byte_identical (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    gpu_image kept[2] = {{0}};
    for (int pass = 0; pass < 2; pass++) {
        d3d8_swap_replay_config config = rt_config(selector);
        config.render_target_texture = pass != 0;
        CHECK(rt_begin(&config));
        rt_producer_pass(RT_PRODUCER);
        bind_target(RT_CONSUMER);
        uint32_t words[GPU_PGRAPH_COMBINER_WORDS];
        build_combiner_words(pass_entries, sizeof pass_entries / sizeof pass_entries[0], words);
        stream_builder stream = {0};
        stream_pixel_shader(&stream, words);
        build_frame_two(&stream);
        push_pending(&stream);
        stream_free(&stream);
        const d3d8_swap_replay_stats stats = rt_present();
        CHECK(!stats.latched && stats.frames_replayed == 1u && stats.rt_texture_draws == 0u);
        CHECK(pass == 0 ? stats.texture_bindings == 0u : true);
        CHECK(pass == 0 || (stats.used_inferences & GPU_PGRAPH_INFER_OUTPUT_TEXTURE_BRIDGE) == 0u); /* nothing sampled a render target */
        const gpu_image *frame = d3d8_swap_replay_last_frame();
        CHECK(frame != NULL && frame->pixels != NULL);
        if (frame != NULL && frame->pixels != NULL) {
            kept[pass] = *frame;
            const size_t bytes = (size_t)frame->stride_bytes * frame->height;
            kept[pass].pixels = malloc(bytes);
            CHECK(kept[pass].pixels != NULL);
            if (kept[pass].pixels != NULL) {
                memcpy(kept[pass].pixels, frame->pixels, bytes);
            }
        }
        rt_end();
    }
    CHECK(images_equal(&kept[0], &kept[1]));
    free(kept[0].pixels);
    free(kept[1].pixels);
}

/* Census mode: every draw is classified once past the first refusal, no pixel is replayed and nothing latches. */
static void test_rt_texture_census(const char *selector)
{
    printf("test_rt_texture_census (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    d3d8_swap_replay_config config = rt_config(selector);
    config.render_target_texture_census = true;
    CHECK(rt_begin(&config));
    make_rt_target(TARGET_C, 0x00700000u, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR); /* refused: no earlier image */
    d3d8_swap_replay_stats stats = rt_present();
    CHECK(!stats.latched && stats.frames_refused == 0u && stats.frames_replayed == 0u && stats.census_frames == 1u);
    CHECK(stats.census_draws == 3u && stats.draws == 0u && d3d8_swap_replay_last_frame() == NULL);
    /* frame 2: the consumer now samples the producer: resolved. The refusal of frame 1 stopped nothing. */
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, RT_PRODUCER, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    CHECK(!stats.latched && stats.census_frames == 2u && stats.census_draws == 6u && stats.rt_texture_draws == 1u);
    char census[300];
    uint64_t draws = 0u;
    uint64_t total = 0u;
    bool refused = false;
    bool resolved = false;
    bool no_texture = false;
    for (size_t i = 0u; d3d8_swap_replay_texture_census_at(i, census, sizeof census, &draws); i++) {
        total += draws;
        refused = refused || (strstr(census, "REFUSED stage 0 texture: no earlier image") != NULL && draws == 1u);
        resolved = resolved || (strstr(census, "stage 0 from render target 0x") != NULL && draws == 1u);
        no_texture = no_texture || (strstr(census, "no texture read by the combiner") != NULL && draws == 4u);
    }
    CHECK(refused && resolved && no_texture && total == 6u);
    char text[1100];
    CHECK(d3d8_swap_replay_rt_texture_summary(text, sizeof text) > 0u);
    CHECK(strstr(text, "CENSUS ONLY, no pixel was replayed") != NULL && strstr(text, "6 draw(s) classified") != NULL);
    char counted[200];
    snprintf(counted, sizeof counted, "2 frame(s), 6 draw(s) classified by what their texture stage would resolve to or why it is refused, %llu SetTexture binding(s) noted",
             (unsigned long long)stats.texture_bindings);
    CHECK(strstr(text, counted) != NULL && stats.frames_empty == 2u);
    rt_end();
}

/* No device: what enable refuses by name, and the default being off. */
static void test_rt_texture_config(void)
{
    printf("test_rt_texture_config\n");
    char error[300];
    char text[1100];
    CHECK(!d3d8_swap_replay_default_config().render_target_texture);
    CHECK(!d3d8_swap_replay_default_config().render_target_texture_census);
    CHECK(!combiner_config(NULL).render_target_texture && !base_config(NULL).render_target_texture);
    d3d8_swap_replay_config config = combiner_config(NULL);
    CHECK(d3d8_swap_replay_enable(&config, error, sizeof error));
    text[0] = 'x';
    CHECK(d3d8_swap_replay_rt_texture_summary(text, sizeof text) == 0u && text[0] == '\0');
    CHECK(d3d8_swap_replay_texture_census_count() == 0u);
    CHECK(d3d8_swap_replay_get_stats().texture_bindings == 0u);
    d3d8_swap_replay_on_texture(0u, 0u); /* inert with the option off */
    CHECK(d3d8_swap_replay_get_stats().texture_bindings == 0u);
    d3d8_swap_replay_disable();

    config = rt_config(NULL);
    CHECK(d3d8_swap_replay_enable(&config, error, sizeof error));
    CHECK(d3d8_swap_replay_get_config().render_target_texture);
    CHECK(d3d8_swap_replay_rt_texture_summary(text, sizeof text) > 0u && strstr(text, "0 SetTexture binding(s) noted") != NULL);
    d3d8_swap_replay_on_texture(4u, 0x1000u); /* a stage past 3 is ignored, never read */
    CHECK(d3d8_swap_replay_get_stats().texture_bindings == 0u);
    d3d8_swap_replay_disable();

    static const struct {
        const char *what;
        const char *expected;
    } refusals[] = {
        {"combiner", "combiner option"},
        {"standin", "exclusive"},
        {"flip", "flip_y"},
        {"group", "TEXTURE output group"},
        {"inference", "D3D8_SWAP_REPLAY_INFER_RT_TEXTURE"},
        {"census", "needs the render target texture option"},
    };
    for (size_t i = 0u; i < sizeof refusals / sizeof refusals[0]; i++) {
        config = rt_config(NULL);
        if (strcmp(refusals[i].what, "combiner") == 0) {
            config.combiner = false;
        } else if (strcmp(refusals[i].what, "standin") == 0) {
            config.standin_texture = true;
            config.standin_width = 2u;
            config.standin_height = 2u;
        } else if (strcmp(refusals[i].what, "flip") == 0) {
            config.flip_y = true;
        } else if (strcmp(refusals[i].what, "group") == 0) {
            config.output_groups = 0u;
        } else if (strcmp(refusals[i].what, "inference") == 0) {
            config.allowed_inferences &= ~GPU_PGRAPH_INFER_OUTPUT_TEXTURE_BRIDGE;
        } else {
            config.render_target_texture = false;
            config.render_target_texture_census = true;
        }
        error[0] = '\0';
        CHECK(!d3d8_swap_replay_enable(&config, error, sizeof error));
        CHECK(strstr(error, refusals[i].expected) != NULL);
        CHECK(!d3d8_swap_replay_enabled());
    }
    /* the one inference bit is not in anything the host allows by default */
    CHECK((GPU_PGRAPH_INFER_OUTPUT_ALL & GPU_PGRAPH_INFER_OUTPUT_TEXTURE_BRIDGE) == 0u);
    CHECK((d3d8_swap_replay_host_inferences(true, true, true, true, true) & GPU_PGRAPH_INFER_OUTPUT_TEXTURE_BRIDGE) == 0u);
}

/* --- T510 mutation gaps: the notes, the census details, a rebinding inside one pass, a second producer ------------------------ */

static uint64_t rt_noted(void)
{
    return d3d8_swap_replay_get_stats().texture_bindings;
}

/* What counts as a new binding of a stage, and what a frame boundary and a device reset do to the bindings. No device. */
static void test_rt_texture_notes(void)
{
    printf("test_rt_texture_notes\n");
    d3d8_swap_replay_config config = rt_config(NULL);
    CHECK(rt_begin(&config));
    rt_second_header(RT_PRODUCER_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT); /* TARGET_C: the words of RT_PRODUCER under another header */
    d3d8_set_texture(0u, RT_PRODUCER);
    CHECK(rt_noted() == 1u);
    d3d8_set_texture(0u, RT_PRODUCER); /* a repeat */
    CHECK(rt_noted() == 1u);
    /* one word of the header differing under the same header is a new binding: Data, Format, Size */
    store(RT_PRODUCER + D3D8_SURFACE_DATA, RT_PRODUCER_DATA + 0x1000u);
    d3d8_set_texture(0u, RT_PRODUCER);
    CHECK(rt_noted() == 2u);
    store(RT_PRODUCER + D3D8_SURFACE_FORMAT, 0x00011228u);
    d3d8_set_texture(0u, RT_PRODUCER);
    CHECK(rt_noted() == 3u);
    store(RT_PRODUCER + D3D8_SURFACE_SIZE, (7u << 24) | (63u << 12) | 63u);
    d3d8_set_texture(0u, RT_PRODUCER);
    CHECK(rt_noted() == 4u);
    /* another header with the very same words is a new binding */
    store(TARGET_C + D3D8_SURFACE_DATA, RT_PRODUCER_DATA + 0x1000u);
    store(TARGET_C + D3D8_SURFACE_FORMAT, 0x00011228u);
    store(TARGET_C + D3D8_SURFACE_SIZE, (7u << 24) | (63u << 12) | 63u);
    d3d8_set_texture(0u, TARGET_C);
    CHECK(rt_noted() == 5u);
    /* an unbind is a binding, a second one a repeat */
    d3d8_set_texture(0u, 0u);
    CHECK(rt_noted() == 6u);
    d3d8_set_texture(0u, 0u);
    CHECK(rt_noted() == 6u);
    /* the stages do not share a binding: stage 1 never saw one, whatever stage 0 holds */
    d3d8_set_texture(0u, TARGET_C);
    d3d8_set_texture(1u, TARGET_C);
    CHECK(rt_noted() == 8u);
    /* a frame keeps what each stage held for the next one, stage 1 as well as stage 0 */
    d3d8_swap_replay_on_present(1u);
    d3d8_set_texture(1u, TARGET_C);
    d3d8_set_texture(0u, TARGET_C);
    CHECK(rt_noted() == 8u);
    /* CreateDevice unbinds every stage: the events of the frame and the carried bindings of both stages are forgotten */
    d3d8_set_texture(0u, RT_PRODUCER);
    CHECK(rt_noted() == 9u);
    d3d8_gpu_reset();
    d3d8_set_texture(0u, RT_PRODUCER);
    d3d8_set_texture(1u, TARGET_C);
    CHECK(rt_noted() == 11u);
    /* a latched replay notes nothing more */
    stream_builder open = {0};
    stream_pair(&open, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_TRIANGLES);
    push(&open);
    stream_free(&open);
    d3d8_swap_replay_on_present(2u);
    CHECK(d3d8_swap_replay_get_stats().latched);
    d3d8_set_texture(0u, RT_CONSUMER);
    CHECK(rt_noted() == 11u);
    rt_end();
}

/* The 4096 bindings of a frame: exactly that many are kept, the next frame starts empty again, one more refuses the frame by name,
 * and a reset forgets the overflow. No device. */
static void test_rt_texture_event_limit(void)
{
    printf("test_rt_texture_event_limit\n");
    d3d8_swap_replay_config config = rt_config(NULL);
    CHECK(rt_begin(&config));
    for (uint32_t i = 0u; i < 4096u; i++) {
        d3d8_set_texture(0u, (i & 1u) != 0u ? RT_CONSUMER : RT_PRODUCER);
    }
    CHECK(rt_noted() == 4096u);
    d3d8_swap_replay_on_present(1u);
    d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_refused == 0u && stats.frames_empty == 1u);
    for (uint32_t i = 0u; i < 4096u; i++) { /* the previous frame ended on RT_CONSUMER, so this one starts with a change */
        d3d8_set_texture(0u, (i & 1u) != 0u ? RT_CONSUMER : RT_PRODUCER);
    }
    CHECK(rt_noted() == 2u * 4096u);
    d3d8_swap_replay_on_present(2u);
    stats = d3d8_swap_replay_get_stats();
    CHECK(!stats.latched && stats.frames_refused == 0u && stats.frames_empty == 2u);
    for (uint32_t i = 0u; i < 4097u; i++) {
        d3d8_set_texture(0u, (i & 1u) != 0u ? RT_CONSUMER : RT_PRODUCER);
    }
    CHECK(rt_noted() == 3u * 4096u); /* the 4097th is not noted */
    d3d8_swap_replay_on_present(3u);
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.latched && stats.frames_refused == 1u && strstr(stats.error, "more than 4096 SetTexture bindings") != NULL);
    CHECK(strstr(stats.error, "render target texture") != NULL);
    rt_end();

    CHECK(rt_begin(&config));
    for (uint32_t i = 0u; i < 4097u; i++) {
        d3d8_set_texture(0u, (i & 1u) != 0u ? RT_CONSUMER : RT_PRODUCER);
    }
    d3d8_gpu_reset();
    d3d8_swap_replay_on_present(1u);
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.model_resets == 1u && !stats.latched && stats.frames_refused == 0u && stats.frames_empty == 1u);
    rt_end();
}

/* Rebinding inside one pass: two producers, then three draws of one consumer pass with a SetTexture between the first two and none
 * before the third. Each draw samples its own binding, the third the second's, and the last draw's image is what is left. */
static void rt_rebind_scene(const char *selector, bool census)
{
    printf("test_rt_texture_rebind (%s, %s)\n", selector != NULL ? selector : "no device", census ? "census" : "replay");
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    d3d8_swap_replay_config config = rt_config(selector);
    config.render_target_texture_census = census;
    CHECK(rt_begin(&config));
    make_rt_target(TARGET_D, 0x00700000u, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    rt_producer_pass(RT_PRODUCER);
    rt_red_pass(TARGET_D);
    rt_consumer_pass(RT_CONSUMER, RT_PRODUCER, RT_CLAMP, RT_LINEAR);
    rt_consumer_pass(RT_CONSUMER, TARGET_D, RT_CLAMP, RT_LINEAR);
    rt_consumer_pass(RT_CONSUMER, UINT32_MAX, RT_CLAMP, RT_LINEAR);
    const d3d8_swap_replay_stats stats = rt_present();
    CHECK(!stats.latched && stats.rt_texture_draws == 3u);
    char full[100];
    char red[100];
    uint64_t full_draws = 0u;
    uint64_t red_draws = 0u;
    snprintf(full, sizeof full, "stage 0 from render target 0x%08X (Data 0x0500000", (unsigned)RT_PRODUCER);
    snprintf(red, sizeof red, "stage 0 from render target 0x%08X (Data 0x0700000", (unsigned)TARGET_D);
    CHECK(census_has(full, NULL, &full_draws) && full_draws == 1u);
    CHECK(census_has(red, NULL, &red_draws) && red_draws == 2u);
    if (census) {
        uint64_t plain = 0u;
        CHECK(stats.census_frames == 1u && stats.census_draws == 6u && d3d8_swap_replay_texture_census_count() == 3u);
        CHECK(census_has("no texture read by the combiner", NULL, &plain) && plain == 3u);
    } else {
        CHECK(stats.frames_replayed == 1u && stats.draws == 6u && d3d8_swap_replay_texture_census_count() == 2u);
        const gpu_image *consumer = d3d8_swap_replay_last_frame();
        const gpu_image *producer = d3d8_swap_replay_offscreen_frame(0u, NULL);
        const gpu_image *red_only = d3d8_swap_replay_offscreen_frame(1u, NULL);
        CHECK(consumer != NULL && producer != NULL && red_only != NULL);
        if (consumer != NULL && producer != NULL && red_only != NULL && consumer->pixels != NULL && producer->pixels != NULL &&
            red_only->pixels != NULL) {
            CHECK(images_equal(consumer, red_only) && !images_equal(consumer, producer));
            CHECK(pixel_is(consumer, 32u, 12u, 255u, 0u, 0u, 255u) && pixel_is(consumer, 96u, 48u, 0u, 0u, 0u, 255u));
        }
    }
    rt_end();
}

static void test_rt_texture_rebind(const char *selector)
{
    rt_rebind_scene(selector, false);
}

/* A producer drawn AFTER an unrelated pass, sampled through a second header that holds the producer's Data with its top bits set:
 * the producer's picture is what the consumer shows (not the first pass's), and the census names the producer's header. */
static void test_rt_texture_second_producer(const char *selector)
{
    printf("test_rt_texture_second_producer (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    d3d8_swap_replay_config config = rt_config(selector);
    CHECK(rt_begin(&config));
    make_rt_target(TARGET_D, 0x00700000u, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    rt_second_header(RT_PRODUCER_DATA | 0xF0000000u, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    rt_red_pass(TARGET_D);
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    const d3d8_swap_replay_stats stats = rt_present();
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.rt_texture_draws == 1u);
    const gpu_image *consumer = d3d8_swap_replay_last_frame();
    const gpu_image *red_only = d3d8_swap_replay_offscreen_frame(0u, NULL);
    const gpu_image *producer = d3d8_swap_replay_offscreen_frame(1u, NULL);
    CHECK(consumer != NULL && producer != NULL && red_only != NULL);
    if (consumer != NULL && producer != NULL && red_only != NULL && consumer->pixels != NULL && producer->pixels != NULL &&
        red_only->pixels != NULL) {
        CHECK(images_equal(consumer, producer) && !images_equal(consumer, red_only));
        CHECK(pixel_is(consumer, 96u, 48u, 255u, 255u, 0u, 255u));
    }
    char want[120];
    char other[40];
    snprintf(want, sizeof want, "stage 0 from render target 0x%08X (Data 0x0500000", (unsigned)RT_PRODUCER);
    snprintf(other, sizeof other, "render target 0x%08X", (unsigned)TARGET_C);
    uint64_t draws = 0u;
    CHECK(census_has(want, NULL, &draws) && draws == 1u && !census_has(other, NULL, NULL));
    rt_end();
}

/* The census on a stage other than 0: the words of THAT stage decide, its own refusal is named with its own header's words, and a
 * resolved stage 0 beside it is not mistaken for the refused one. No device. */
static void test_rt_texture_census_stage_one(void)
{
    printf("test_rt_texture_census_stage_one\n");
    d3d8_swap_replay_config config = rt_config(NULL);
    config.render_target_texture_census = true;
    char want[160];
    uint64_t draws = 0u;

    CHECK(rt_begin(&config)); /* stage 1 resolved, stage 0 never bound and its words never written */
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_stage_pass(RT_CONSUMER, 1u, RT_PRODUCER, RT_CLAMP, RT_LINEAR);
    d3d8_swap_replay_stats stats = rt_present();
    CHECK(!stats.latched && stats.census_frames == 1u && stats.rt_texture_draws == 1u);
    snprintf(want, sizeof want, "stage 1 from render target 0x%08X (Data 0x0500000", (unsigned)RT_PRODUCER);
    CHECK(census_has(want, NULL, &draws) && draws == 1u);
    CHECK(!census_has("stage 0 from", NULL, NULL));
    rt_end();

    CHECK(rt_begin(&config)); /* stage 1 refused for its address word, stage 0 resolved and not read */
    rt_second_header(RT_PRODUCER_DATA, 64u, 64u);
    rt_producer_pass(RT_PRODUCER);
    stream_builder zero = {0};
    stream_pair(&zero, RT_ADDRESS_METHOD, RT_CLAMP);
    stream_pair(&zero, RT_FILTER_METHOD, RT_LINEAR);
    push_pending(&zero);
    stream_free(&zero);
    d3d8_set_texture(0u, RT_PRODUCER);
    rt_consumer_stage_pass(RT_CONSUMER, 1u, TARGET_C, RT_WRAP, RT_LINEAR);
    stats = rt_present();
    CHECK(!stats.latched && stats.census_frames == 1u && stats.rt_texture_draws == 0u);
    snprintf(want, sizeof want, "(drawing into target 0x%08X)", (unsigned)RT_CONSUMER);
    CHECK(census_has("REFUSED stage 1 texture: address mode, Format 0x00011229 Size 0x0303F03F", want, &draws) && draws == 1u);
    rt_end();
}

/* The detail of one refusal stays with it, and a refusal that is not a texture is told from one that is. No device. */
static void test_rt_texture_census_refusals(void)
{
    printf("test_rt_texture_census_refusals\n");
    d3d8_swap_replay_config config = rt_config(NULL);
    config.render_target_texture_census = true;
    uint64_t draws = 0u;

    CHECK(rt_begin(&config)); /* an unsupported header with a detail, then an unbound stage with none */
    rt_second_header(RT_PRODUCER_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    store(TARGET_C + D3D8_SURFACE_FORMAT, 0x09920C29u);
    store(TARGET_C + D3D8_SURFACE_SIZE, 0u);
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    rt_consumer_pass(RT_CONSUMER, 0u, RT_CLAMP, RT_LINEAR);
    d3d8_swap_replay_stats stats = rt_present();
    CHECK(!stats.latched && stats.census_frames == 1u && stats.census_draws == 4u);
    CHECK(census_has("REFUSED stage 0 texture: second level, Format 0x09920C29 Size 0x00000000 (drawing", NULL, &draws) && draws == 1u);
    CHECK(census_has("REFUSED stage 0 texture: unbound, (drawing into target", NULL, &draws) && draws == 1u);
    rt_end();

    /* a combiner the replay may not plan (the colour range inference withheld: the producer's) is refused for that, whatever the
     * texture stages say (none was bound yet), and the reason is kept past its first words */
    config.allowed_inferences &= ~GPU_PGRAPH_INFER_COMBINER_COLOUR_RANGE;
    CHECK(rt_begin(&config));
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, RT_PRODUCER, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    CHECK(!stats.latched && stats.census_frames == 1u && stats.census_draws == 3u && stats.rt_texture_draws == 1u);
    char line[300];
    uint64_t total = 0u;
    uint64_t plan_draws = 0u;
    for (size_t i = 0u; d3d8_swap_replay_texture_census_at(i, line, sizeof line, &draws); i++) {
        total += draws;
        if (strncmp(line, "REFUSED by the combiner plan, not a texture: ", 45u) == 0) {
            plan_draws += draws;
            CHECK(strlen(line) > 45u + 60u && strstr(line, "REFUSED stage") == NULL);
        }
    }
    CHECK(total == 3u && plan_draws == 2u); /* the two draws of the producer pass, the consumer's combiner needs no colour range */
    rt_end();
}

/* The census and the sample tally keep 16 distinct lines and drop the rest: 17 frames, each with a line of its own (a different target
 * drawn into for the census, a different producer for the tally). */
static void test_rt_texture_census_limit(void)
{
    printf("test_rt_texture_census_limit\n");
    d3d8_swap_replay_config config = rt_config(NULL);
    config.render_target_texture_census = true;
    CHECK(rt_begin(&config));
    for (uint32_t i = 0u; i < 17u; i++) {
        const uint32_t header = HEADER_SCRATCH + 0x200u + i * 0x40u;
        make_rt_target(header, 0x00800000u + i * 0x10000u, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
        rt_consumer_pass(header, UINT32_MAX, RT_CLAMP, RT_LINEAR); /* no binding observed: a refusal line naming `header` */
        (void)rt_present();
        CHECK(d3d8_swap_replay_texture_census_count() == (i < 16u ? i + 1u : 16u));
    }
    rt_end();
}

static void test_rt_texture_tally_limit(const char *selector)
{
    printf("test_rt_texture_tally_limit (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    d3d8_swap_replay_config config = rt_config(selector);
    CHECK(rt_begin(&config));
    for (uint32_t i = 0u; i < 17u; i++) {
        const uint32_t header = HEADER_SCRATCH + 0x200u + i * 0x40u;
        make_rt_target(header, 0x00800000u + i * 0x10000u, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
        rt_producer_pass(header);
        rt_consumer_pass(RT_CONSUMER, header, RT_CLAMP, RT_LINEAR);
        const d3d8_swap_replay_stats stats = rt_present();
        CHECK(!stats.latched && stats.rt_texture_draws == i + 1u);
        CHECK(d3d8_swap_replay_texture_census_count() == (i < 16u ? i + 1u : 16u));
    }
    rt_end();
}

/* A frame with no SetRenderTarget (no Swap either) draws into the target the device holds: sampling that very header is feedback, not
 * a texture with no earlier image. */
static void rt_no_switch_scene(const char *selector, bool census)
{
    printf("test_rt_texture_no_switch (%s, %s)\n", selector != NULL ? selector : "no device", census ? "census" : "replay");
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    d3d8_swap_replay_config config = rt_config(selector);
    config.render_target_texture_census = census;
    CHECK(rt_begin(&config));
    const uint32_t back = d3d8_device_load32(DEV_RENDER_TARGET);
    store(back + D3D8_SURFACE_FORMAT, 0x00011229u);
    store(back + D3D8_SURFACE_SIZE, ((640u * 4u / 64u - 1u) << 24) | (479u << 12) | 639u);
    rt_consumer_stage_pass(0u, 0u, back, RT_CLAMP, RT_LINEAR);
    d3d8_gpu_kick();                  /* straight to the present: Swap's own SetRenderTarget would be a switch */
    d3d8_swap_replay_on_present(1u);
    const d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    if (census) {
        CHECK(!stats.latched && stats.census_frames == 1u && census_has("REFUSED stage 0 texture: same-target feedback,", NULL, NULL));
    } else {
        CHECK(stats.latched && strstr(stats.error, "same-target feedback") != NULL);
    }
    rt_end();
}

static void test_rt_texture_no_switch(const char *selector)
{
    rt_no_switch_scene(selector, false);
}

/* T831: the frame hook receives the displayed frame, the composition with the overlay over it when the opt-in is on
 * and a layer is latched (xemu-level, d3d8_overlay_key.h), the composition itself otherwise. The replay's own
 * last frame is the composition either way. */
typedef struct {
    const gpu_image *image;
    gpu_image copy;
    uint64_t calls;
} displayed_log;

static void remember_displayed(const gpu_image *image, uint64_t frame, void *context)
{
    (void)frame;
    displayed_log *log = context;
    log->image = image;
    log->calls++;
    gpu_image_free(&log->copy);
    log->copy.pixels = malloc((size_t)image->width * image->height * 4u);
    memcpy(log->copy.pixels, image->pixels, (size_t)image->width * image->height * 4u);
    log->copy.width = image->width;
    log->copy.height = image->height;
    log->copy.stride_bytes = image->width * 4u;
}

static void test_frame_hook_gets_the_overlay_over_the_composition(const char *selector)
{
    printf("test_frame_hook_gets_the_overlay_over_the_composition (%s)\n", selector);
    uint8_t filled[100 * 100 * 3];
    for (size_t index = 0u; index < sizeof(filled); index += 3u) {
        filled[index] = 1u;
        filled[index + 1u] = 2u;
        filled[index + 2u] = 3u;
    }
    for (int phase = 0; phase < 3; phase++) {
        begin_device(true);
        d3d8_overlay_key_reset();
        d3d8_overlay_key_set_enabled(phase != 0);
        d3d8_swap_replay_config config = base_config(selector);
        CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
        displayed_log log;
        memset(&log, 0, sizeof(log));
        d3d8_swap_replay_set_frame_hook(remember_displayed, &log);
        /* phase 0: off, a layer latched anyway. phase 1: on, no layer. phase 2: on with the layer, bit clear. */
        const d3d8_overlay_key_layer layer = {filled, 100u, 100u, 100u, 100u, 0x10A00u, 0u};
        if (phase != 1) {
            CHECK(d3d8_overlay_key_latch(&layer));
        }
        stream_builder one = {0};
        build_frame_one(&one);
        push(&one);
        present();
        const gpu_image *frame = d3d8_swap_replay_last_frame();
        CHECK(frame != NULL && frame->pixels != NULL && log.calls == 1u && log.copy.pixels != NULL);
        if (frame != NULL && frame->pixels != NULL && log.copy.pixels != NULL) {
            CHECK(pixel_is(frame, 160u, 120u, 255u, 0u, 0u, 255u)); /* the composition is never modified */
            if (phase == 2) {
                CHECK(log.image != frame);
                CHECK(pixel_is(&log.copy, 160u, 120u, 1u, 2u, 3u, 255u)); /* the overlay hides the composition */
                CHECK(pixel_is(&log.copy, 100u, 100u, 1u, 2u, 3u, 255u));
                CHECK(pixel_is(&log.copy, 199u, 199u, 1u, 2u, 3u, 255u));
                CHECK(!pixel_is(&log.copy, 200u, 200u, 1u, 2u, 3u, 255u) && !pixel_is(&log.copy, 99u, 100u, 1u, 2u, 3u, 255u));
                CHECK(pixel_is(&log.copy, 480u, 336u, 0u, 0u, 0u, 255u)); /* outside the layer: the composition */
            } else {
                CHECK(log.image == frame); /* off, or nothing latched: the composition itself */
            }
        }
        d3d8_swap_replay_set_frame_hook(NULL, NULL);
        gpu_image_free(&log.copy);
        stream_free(&one);
        d3d8_swap_replay_disable();
        d3d8_overlay_key_set_enabled(false);
        d3d8_overlay_key_reset();
        environment_end();
    }
}

/* --- T633, T596: the surface model (INFERRED, opt-in) --------------------------------------------------------------------------------
 * A surface no pass of the frame drew is its kept image or the guest memory under it (a texture stage and the CopyRects blits), a render target
 * pass can start from the image it left, and the overlay is counted. docs/d3d8-copy-composition.md. Guest memory: one mapped region under the
 * Data words the scenes name (a synthetic region number nothing else holds), pixels as the guest writes them, dword 0xAARRGGBB. */

#define SURFACE_MAP_BASE 0x00480000u
#define SURFACE_MAP_BYTES 0x00300000u /* 0x480000 .. 0x780000: the producer 0x500000, the consumer 0x600000 and the texture 0x700000 */
#define BLIT_MAP_BASE 0x00A00000u
#define BLIT_MAP_BYTES 0x00100000u
#define UNMAPPED_DATA 0x00C00000u /* no region: unreadable */
#define BACK_BUFFER_DATA 0x01000000u /* a zeroed 640x480 A8R8G8B8 surface (the unit test device's own back buffer Data word is backed by no guest region) */
#define BACK_BUFFER_BYTES 0x00140000u
#define TEXTURE_DATA 0x00700000u  /* what rt_second_header's TARGET_C names in these scenes */
#define PITCHES_512 0x02000200u   /* source and destination pitch 512 bytes: 128 pixels */
#define PITCHES_256 0x01000100u   /* 64 pixels */
#define GRADIENT_ALPHA 0xC0u
#define GRADIENT_BLUE 0x77u
#define TEXTURE_BYTES (RT_EDGE_WIDTH * 4u * RT_EDGE_HEIGHT) /* 0x8000 */

/* A Size word of `width` x `height` texels and a pitch of `pitch` bytes (a multiple of 64), whatever the Format says. */
static void set_size_word(uint32_t header, uint32_t width, uint32_t height, uint32_t pitch)
{
    store(header + D3D8_SURFACE_SIZE, ((pitch / 64u - 1u) << 24) | ((height - 1u) << 12) | (width - 1u));
}

static d3d8_swap_replay_config surface_config(const char *selector, bool surface_source, bool target_persist)
{
    d3d8_swap_replay_config config = rt_config(selector);
    config.allowed_inferences |= GPU_PGRAPH_INFER_OUTPUT_BLIT_PLANNER_RULES; /* the host's --gpu-replay-output-state allows it (T832: the clamp, the ordered overlap, formats 7 and 6) */
    config.surface_source = surface_source;
    config.target_persist = target_persist;
    if (surface_source) {
        config.allowed_inferences |= D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE;
    }
    if (target_persist) {
        config.allowed_inferences |= D3D8_SWAP_REPLAY_INFER_TARGET_PERSIST;
    }
    return config;
}

/* Map the guest memory under the Data words of the scenes. No Data word of a scene may be a PHYSICAL address of a region the device made
 * (d3d8_gpu_read_guest would read that region instead), which is checked. */
static void map_surface_memory(void)
{
    map_fixed(SURFACE_MAP_BASE, SURFACE_MAP_BYTES);
    map_fixed(BLIT_MAP_BASE, BLIT_MAP_BYTES);
    map_fixed(BACK_BUFFER_DATA, BACK_BUFFER_BYTES);
    static const uint32_t labels[] = {0x004F8000u, 0x004FF000u, 0x00500000u, 0x00501000u, 0x00508000u, 0x00600000u, 0x00700000u,
                                      0x0077FFFFu, 0x00A00000u, 0x00A60000u, 0x00AFFFFFu, UNMAPPED_DATA, BACK_BUFFER_DATA, BACK_BUFFER_DATA + BACK_BUFFER_BYTES - 1u};
    for (size_t i = 0u; i < sizeof labels / sizeof labels[0]; i++) {
        kernel_guest_ptr resolved = 0u;
        CHECK(!guest_virtual_from_physical(labels[i], &resolved));
    }
}

static uint32_t gradient_argb(uint32_t x, uint32_t y)
{
    return (GRADIENT_ALPHA << 24) | ((x + 1u) << 16) | ((y + 1u) << 8) | GRADIENT_BLUE;
}

/* A8R8G8B8 gradient into guest memory: guest dword 0xAARRGGBB with R = x + 1, G = y + 1, so a swapped red and blue, a shifted row or a
 * wrong pitch cannot give the same picture. */
static void fill_gradient(uint32_t data, uint32_t pitch, uint32_t width, uint32_t height)
{
    for (uint32_t y = 0u; y < height; y++) {
        for (uint32_t x = 0u; x < width; x++) {
            store(data + y * pitch + x * 4u, gradient_argb(x, y));
        }
    }
}

/* Is pixel (x, y) of `image` the RGBA the gradient has at (gradient_x, gradient_y)? */
static bool pixel_is_gradient(const gpu_image *image, uint32_t x, uint32_t y, uint32_t gradient_x, uint32_t gradient_y)
{
    return pixel_is(image, x, y, (uint8_t)(gradient_x + 1u), (uint8_t)(gradient_y + 1u), GRADIENT_BLUE, GRADIENT_ALPHA);
}

static uint32_t gradient_mismatches(const gpu_image *image)
{
    uint32_t count = 0u;
    for (uint32_t y = 0u; y < image->height; y++) {
        for (uint32_t x = 0u; x < image->width; x++) {
            count += !pixel_is_gradient(image, x, y, x, y);
        }
    }
    return count;
}

/* A copy of an image the replay owns until its next frame. A zero image when there is none. */
static gpu_image copy_of(const gpu_image *image)
{
    gpu_image copy;
    memset(&copy, 0, sizeof copy);
    if (image == NULL || image->pixels == NULL) {
        return copy;
    }
    copy = *image;
    const size_t bytes = (size_t)image->stride_bytes * image->height;
    copy.pixels = malloc(bytes);
    if (copy.pixels != NULL) {
        memcpy(copy.pixels, image->pixels, bytes);
    }
    return copy;
}

/* TSFP_T633_LOOK=dir writes the images of the persistence and kept image scenes as PNGs, for looking at them (nothing is asserted from the files). */
static void surface_look(const char *name, const gpu_image *image)
{
    const char *directory = getenv("TSFP_T633_LOOK");
    if (directory == NULL || image == NULL || image->pixels == NULL) {
        return;
    }
    char path[600];
    snprintf(path, sizeof path, "%s/%s.png", directory, name);
    (void)gpu_png_write_rgba(path, image->pixels, image->width, image->height, image->stride_bytes);
}

/* A refusal of a frame that is not the first: the latch with the named text. (expect_rt_refusal also requires that no frame was ever replayed.) */
static void expect_surface_refusal(const d3d8_swap_replay_stats *stats, const char *words, const char *more)
{
    CHECK(stats->latched && stats->frames_refused == 1u);
    if (strstr(stats->error, words) == NULL || (more != NULL && strstr(stats->error, more) == NULL)) {
        printf("  expected \"%s\" and \"%s\" in: %s\n", words, more != NULL ? more : "", stats->error);
    }
    CHECK(strstr(stats->error, words) != NULL);
    CHECK(more == NULL || strstr(stats->error, more) != NULL);
}

/* The yellow triangle alone (the second one of write_vertices) with the state it needs, so a pass of a later frame does not depend on an earlier one. */
static void build_yellow_only(stream_builder *stream)
{
    static const float offset[4] = {320.0f, 240.0f, 0.0f, 0.0f};
    static const float scale[4] = {320.0f, -240.0f, 1.0f, 0.0f};
    static const float c3_green[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    stream_viewport(stream, offset, scale);
    stream_pair(stream, GPU_PGRAPH_EXECUTION_MODE, 6u);
    stream_program(stream, 0u, synthetic_program, 1u);
    stream_pair(stream, GPU_PGRAPH_PROGRAM_START, 0u);
    stream_constants(stream, 3u, c3_green, 4u);
    stream_array(stream, 1u, MEMORY_BASE, array_format(28u, 3u, GPU_PGRAPH_TYPE_F));
    stream_array(stream, 2u, MEMORY_BASE + 12u, array_format(28u, 4u, GPU_PGRAPH_TYPE_F));
    stream_draw_arrays(stream, GPU_PGRAPH_OP_TRIANGLES, 3u, 3u);
}

/* A pass that draws the yellow triangle only, optionally after a clear of x 24..40, y 8..15 (inclusive) to opaque green (it overlaps the red triangle
 * of rt_producer_pass's picture at (32, 12) and leaves (32, 20) alone). */
static void rt_yellow_pass(uint32_t header, bool with_clear)
{
    bind_target(header);
    uint32_t words[GPU_PGRAPH_COMBINER_WORDS];
    build_combiner_words(pass_entries, sizeof pass_entries / sizeof pass_entries[0], words);
    stream_builder stream = {0};
    stream_pair(&stream, 0x030Cu, 0u);
    stream_pair(&stream, 0x032Cu, 0u);
    stream_pixel_shader(&stream, words);
    if (with_clear) {
        stream_pair(&stream, 0x1D98u, 24u | (40u << 16));
        stream_pair(&stream, 0x1D9Cu, 8u | (15u << 16));
        stream_pair(&stream, 0x1D8Cu, 0u);
        stream_pair(&stream, 0x1D90u, 0xFF00FF00u);
        stream_pair(&stream, GPU_PGRAPH_CLEAR_SURFACE, 0xF0u);
    }
    build_yellow_only(&stream);
    push_pending(&stream);
    stream_free(&stream);
}

static uint64_t occurrences(const char *haystack, const char *needle)
{
    uint64_t count = 0u;
    for (const char *at = strstr(haystack, needle); at != NULL; at = strstr(at + 1, needle)) {
        count++;
    }
    return count;
}

/* The surface source with a texture no pass of the frame draws: the guest memory under it. A back buffer sized surface of zero bytes (MEASURED
 * zero for the back buffer on the retail boots) is transparent black, bytes the CPU wrote are the texels (guest dword 0xAARRGGBB is RGBA
 * R, G, B, A), and with the option off the very same scene refuses with the unchanged text. */
static void test_surface_source_guest(const char *selector)
{
    printf("test_surface_source_guest (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);

    /* a back buffer sized surface nothing drew (zero guest memory, as MEASURED on the retail boots): every texel is transparent black (0, 0, 0, 0), not the clear colour (0, 0, 0, 255) */
    d3d8_swap_replay_config config = surface_config(selector, true, false);
    CHECK(rt_begin(&config));
    map_surface_memory();
    const uint32_t back_data = BACK_BUFFER_DATA;
    rt_second_header(back_data, 640u, 480u);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    d3d8_swap_replay_stats stats = rt_present();
    if (stats.latched) {
        printf("  frame: %s\n", stats.error);
    }
    CHECK(!stats.latched && stats.frames_refused == 0u && stats.frames_replayed == 1u && stats.passes_replayed == 1u && stats.draws == 1u);
    CHECK(stats.rt_texture_draws == 1u && stats.surface_guest_samples == 1u && stats.surface_guest_zero_samples == 1u &&
          stats.surface_kept_samples == 0u && stats.blit_guest_surfaces == 0u && stats.targets_persisted == 0u);
    CHECK((stats.used_inferences & D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE) != 0u);
    CHECK((stats.used_inferences & GPU_PGRAPH_INFER_OUTPUT_TEXTURE_BRIDGE) != 0u);
    CHECK((stats.used_inferences & GPU_PGRAPH_INFER_OUTPUT_TARGET_PERSIST) == 0u);
    const gpu_image *consumer = d3d8_swap_replay_last_frame();
    CHECK(consumer != NULL && consumer->pixels != NULL && consumer->width == RT_EDGE_WIDTH && consumer->height == RT_EDGE_HEIGHT);
    if (consumer != NULL && consumer->pixels != NULL) {
        CHECK(count_not(consumer, 0u, 0u, 0u, 0u) == 0u); /* every pixel: the triangle covers the target and the texture is transparent black */
        CHECK(pixel_is(consumer, 0u, 0u, 0u, 0u, 0u, 0u) && pixel_is(consumer, 127u, 63u, 0u, 0u, 0u, 0u));
    }
    char want[160];
    snprintf(want, sizeof want, "stage 0 from guest memory surface (Data 0x%07X, 640x480, Format 0x00011229)", (unsigned)back_data);
    uint64_t draws = 0u;
    CHECK(d3d8_swap_replay_texture_census_count() == 1u && census_has(want, "all zero, transparent black", &draws) && draws == 1u);
    CHECK(census_has(want, "all zero, transparent black (MEASURED bytes, INFERRED unchanged at the draw)", NULL));
    char summary[2400];
    CHECK(d3d8_swap_replay_surface_summary(summary, sizeof summary) > 0u);
    CHECK(strstr(summary, "surface source ON") != NULL && strstr(summary, "(0 sample(s)), else the guest memory under it read as A8R8G8B8 (1 sample(s), 1 all zero = transparent black") != NULL);
    rt_end();

    /* the same scene with the option off: the unchanged refusal, no surface counter */
    config = surface_config(selector, false, false);
    CHECK(rt_begin(&config));
    map_surface_memory();
    rt_second_header(back_data, 640u, 480u);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    char data_text[40];
    snprintf(data_text, sizeof data_text, "Data 0x%07X", (unsigned)back_data);
    expect_surface_refusal(&stats, "no earlier image", data_text);
    CHECK(stats.frames_replayed == 0u && strstr(stats.error, "guest memory surface") == NULL);
    CHECK(stats.surface_guest_samples == 0u && stats.surface_guest_zero_samples == 0u && stats.surface_kept_samples == 0u);
    CHECK((stats.used_inferences & D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE) == 0u);
    rt_end();

    /* bytes the CPU wrote: the texels, red and blue not swapped. Counted as a guest sample and not as an all zero one */
    config = surface_config(selector, true, false);
    CHECK(rt_begin(&config));
    map_surface_memory();
    fill_gradient(TEXTURE_DATA, RT_EDGE_WIDTH * 4u, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    rt_second_header(TEXTURE_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.rt_texture_draws == 1u);
    CHECK(stats.surface_guest_samples == 1u && stats.surface_guest_zero_samples == 0u && stats.surface_kept_samples == 0u);
    CHECK((stats.used_inferences & D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE) != 0u);
    consumer = d3d8_swap_replay_last_frame();
    CHECK(consumer != NULL && consumer->pixels != NULL);
    if (consumer != NULL && consumer->pixels != NULL) {
        CHECK(gradient_mismatches(consumer) == 0u);
        CHECK(pixel_is(consumer, 10u, 20u, 11u, 21u, GRADIENT_BLUE, GRADIENT_ALPHA)); /* guest 0xC00B1577: A 0xC0, R 11, G 21, B 0x77 */
        CHECK(pixel_is(consumer, 100u, 5u, 101u, 6u, GRADIENT_BLUE, GRADIENT_ALPHA));
    }
    snprintf(want, sizeof want, "stage 0 from guest memory surface (Data 0x%07X, 128x64, Format 0x00011229)", (unsigned)TEXTURE_DATA);
    CHECK(census_has(want, "read at the present", &draws) && draws == 1u && !census_has("all zero", NULL, NULL));
    rt_end();

    /* a single set byte, the very last of the texture, is not zero (the bytes past it are not the texture's, and a set byte there is not seen) */
    config = surface_config(selector, true, false);
    CHECK(rt_begin(&config));
    map_surface_memory();
    store(TEXTURE_DATA + TEXTURE_BYTES - 4u, 0x01000000u); /* the alpha byte of texel (127, 63) */
    rt_second_header(TEXTURE_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    CHECK(!stats.latched && stats.surface_guest_samples == 1u && stats.surface_guest_zero_samples == 0u);
    consumer = d3d8_swap_replay_last_frame();
    CHECK(consumer != NULL && consumer->pixels != NULL && count_not(consumer, 0u, 0u, 0u, 0u) == 1u && pixel_is(consumer, 127u, 63u, 0u, 0u, 0u, 1u));
    rt_end();
    config = surface_config(selector, true, false);
    CHECK(rt_begin(&config));
    map_surface_memory();
    store(TEXTURE_DATA + TEXTURE_BYTES, 0xFFFFFFFFu); /* the first dword past the texture */
    rt_second_header(TEXTURE_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    CHECK(!stats.latched && stats.surface_guest_samples == 1u && stats.surface_guest_zero_samples == 1u);
    rt_end();

    /* the guest memory is the texture of a draw the title made later too: every draw of every frame reads it again (two frames, two samples) */
    config = surface_config(selector, true, false);
    CHECK(rt_begin(&config));
    map_surface_memory();
    rt_second_header(TEXTURE_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    CHECK(!stats.latched && stats.surface_guest_zero_samples == 1u);
    fill_gradient(TEXTURE_DATA, RT_EDGE_WIDTH * 4u, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    rt_consumer_pass(RT_CONSUMER, UINT32_MAX, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    CHECK(!stats.latched && stats.frames_replayed == 2u && stats.surface_guest_samples == 2u && stats.surface_guest_zero_samples == 1u);
    consumer = d3d8_swap_replay_last_frame();
    CHECK(consumer != NULL && consumer->pixels != NULL && gradient_mismatches(consumer) == 0u);
    CHECK(d3d8_swap_replay_surface_summary(summary, sizeof summary) > 0u);
    CHECK(strstr(summary, "(0 sample(s)), else the guest memory under it read as A8R8G8B8 (2 sample(s), 1 all zero = transparent black") != NULL);
    rt_end();
}

/* A texture the frame's pass drew is still the pass image, with the surface source on: no surface sample, no SURFACE_SOURCE inference. */
static void test_surface_source_pass_image_first(const char *selector)
{
    printf("test_surface_source_pass_image_first (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    d3d8_swap_replay_config config = surface_config(selector, true, false);
    CHECK(rt_begin(&config));
    map_surface_memory();
    rt_producer_pass(RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, RT_PRODUCER, RT_CLAMP, RT_LINEAR);
    const d3d8_swap_replay_stats stats = rt_present();
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.rt_texture_draws == 1u);
    CHECK(stats.surface_kept_samples == 0u && stats.surface_guest_samples == 0u && stats.surface_guest_zero_samples == 0u);
    CHECK((stats.used_inferences & D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE) == 0u);
    CHECK((stats.used_inferences & GPU_PGRAPH_INFER_OUTPUT_TEXTURE_BRIDGE) != 0u);
    const gpu_image *consumer = d3d8_swap_replay_last_frame();
    const gpu_image *producer = d3d8_swap_replay_offscreen_frame(0u, NULL);
    CHECK(consumer != NULL && producer != NULL && images_equal(consumer, producer) && count_not(consumer, 0u, 0u, 0u, 255u) > 300u);
    char census[300];
    CHECK(d3d8_swap_replay_texture_census_at(0u, census, sizeof census, NULL) && strstr(census, "drawn by an earlier pass of the same frame") != NULL);
    rt_end();
}

typedef enum {
    KEPT_EXACT,
    KEPT_PAST_RANGE,
    KEPT_SIZE,
    KEPT_HEIGHT,
    KEPT_FORMAT,
    KEPT_PITCH,
    KEPT_CPU_FIRST,
    KEPT_CPU_LAST,
    KEPT_ALIAS_ABOVE,
    KEPT_ALIAS_BELOW,
    KEPT_TOUCH_ABOVE,
    KEPT_TOUCH_BELOW,
    KEPT_UNMAPPED,
    KEPT_ALIAS_DEEP      /* overlaps only the far end of the kept image */
} kept_case;

/* Frame 1 draws the producer (red and yellow triangles, Data 0x500000, 128x64) and keeps its image. Frame 2 has no pass of the producer: its consumer
 * samples a texture header, which `which` varies, over guest memory the title may have written in between. */
static void kept_scene(const char *selector, kept_case which, const char *words, const char *more)
{
    d3d8_swap_replay_config config = surface_config(selector, true, false);
    CHECK(rt_begin(&config));
    map_surface_memory();
    uint32_t data = RT_PRODUCER_DATA;
    uint32_t width = RT_EDGE_WIDTH;
    uint32_t height = RT_EDGE_HEIGHT;
    switch (which) {
    case KEPT_SIZE: width = 64u; height = 64u; break;
    case KEPT_HEIGHT: height = 32u; break;
    case KEPT_ALIAS_ABOVE: data = RT_PRODUCER_DATA + 0x1000u; break;
    case KEPT_ALIAS_BELOW: data = RT_PRODUCER_DATA - 0x1000u; break;
    case KEPT_TOUCH_ABOVE: data = RT_PRODUCER_DATA + TEXTURE_BYTES; break;
    case KEPT_TOUCH_BELOW: data = RT_PRODUCER_DATA - TEXTURE_BYTES; break;
    case KEPT_UNMAPPED: data = UNMAPPED_DATA; break;
    case KEPT_ALIAS_DEEP: data = RT_PRODUCER_DATA + TEXTURE_BYTES - 0x1000u; break;
    default: break;
    }
    rt_second_header(data, width, height); /* cloned from the back buffer before the producer's header is changed below */

    if (which == KEPT_FORMAT) {
        store(RT_PRODUCER + D3D8_SURFACE_FORMAT, 0x00011228u);
    }
    if (which == KEPT_PITCH) {
        store(RT_PRODUCER + D3D8_SURFACE_SIZE, (8u << 24) | ((RT_EDGE_HEIGHT - 1u) << 12) | (RT_EDGE_WIDTH - 1u)); /* pitch 576 */
    }
    rt_producer_pass(RT_PRODUCER);
    d3d8_swap_replay_stats stats = rt_present();
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.rt_texture_draws == 0u);
    gpu_image first = copy_of(d3d8_swap_replay_last_frame());
    CHECK(first.pixels != NULL && pixel_is(&first, 32u, 12u, 255u, 0u, 0u, 255u) && pixel_is(&first, 96u, 48u, 255u, 255u, 0u, 255u));
    if (which == KEPT_CPU_FIRST) {
        store(RT_PRODUCER_DATA, 0x00000100u); /* one byte of the first dword */
    }
    if (which == KEPT_CPU_LAST) {
        store(RT_PRODUCER_DATA + TEXTURE_BYTES - 4u, 0x01000000u); /* the last byte of the texture's range */
    }
    if (which == KEPT_PAST_RANGE) {
        store(RT_PRODUCER_DATA + TEXTURE_BYTES, 0xFFFFFFFFu); /* the first byte past it: not the texture's */
    }

    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    if (words != NULL) {
        expect_surface_refusal(&stats, words, more);
        CHECK(stats.surface_kept_samples == 0u && stats.surface_guest_samples == 0u);
        free(first.pixels);
        rt_end();
        return;
    }
    if (stats.latched) {
        printf("  frame 2: %s\n", stats.error);
    }
    CHECK(!stats.latched && stats.frames_replayed == 2u && stats.rt_texture_draws == 1u);
    CHECK((stats.used_inferences & D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE) != 0u);
    const gpu_image *consumer = d3d8_swap_replay_last_frame();
    CHECK(consumer != NULL && consumer->pixels != NULL);
    if (which == KEPT_EXACT || which == KEPT_PAST_RANGE) {
        /* the kept image of frame 1, pixel for pixel (not frame 2's own clear colour, not the guest memory) */
        CHECK(stats.surface_kept_samples == 1u && stats.surface_guest_samples == 0u && stats.surface_guest_zero_samples == 0u);
        CHECK(images_equal(consumer, &first));
        CHECK(consumer != NULL && consumer->pixels != NULL && pixel_is(consumer, 32u, 12u, 255u, 0u, 0u, 255u) && pixel_is(consumer, 96u, 48u, 255u, 255u, 0u, 255u));
        CHECK(consumer != NULL && consumer->pixels != NULL && count_not(consumer, 0u, 0u, 0u, 255u) > 300u);
        char census[300];
        CHECK(d3d8_swap_replay_texture_census_at(0u, census, sizeof census, NULL) == true);
        CHECK(census_has("stage 0 from kept render target image (Data 0x0500000, 128x64, Format 0x00011229) of an earlier frame", "MEASURED in xemu", NULL));
        if (which == KEPT_EXACT) {
            char summary[2400];
            CHECK(d3d8_swap_replay_surface_summary(summary, sizeof summary) > 0u);
            CHECK(strstr(summary, "(1 sample(s)), else the guest memory under it read as A8R8G8B8 (0 sample(s), 0 all zero") != NULL);
            surface_look("kept_frame1_producer", &first);
            surface_look("kept_frame2_consumer_samples_the_kept_image", consumer);
        }
    } else {
        /* touching the kept image is no alias: the texture is a surface nothing drew, and its guest memory is zero */
        CHECK(stats.surface_kept_samples == 0u && stats.surface_guest_samples == 1u && stats.surface_guest_zero_samples == 1u);
        CHECK(consumer != NULL && consumer->pixels != NULL && count_not(consumer, 0u, 0u, 0u, 0u) == 0u);
    }
    free(first.pixels);
    rt_end();
}

/* A kept image of an earlier frame, and the refusals that name what makes it unusable. */
static void test_surface_kept_image(const char *selector)
{
    printf("test_surface_kept_image (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    kept_scene(selector, KEPT_EXACT, NULL, NULL);
    kept_scene(selector, KEPT_PAST_RANGE, NULL, NULL);
    kept_scene(selector, KEPT_TOUCH_ABOVE, NULL, NULL);
    kept_scene(selector, KEPT_TOUCH_BELOW, NULL, NULL);
    kept_scene(selector, KEPT_SIZE, "(64x64, pitch 256, Format 0x00011229) is not exactly the kept image of Data 0x0500000 (128x64, pitch 512, Format 0x00011229)",
               "the header, data and format relationship is not exact");
    kept_scene(selector, KEPT_HEIGHT, "(128x32, pitch 512, Format 0x00011229) is not exactly the kept image of Data 0x0500000 (128x64, pitch 512, Format 0x00011229)",
               "the header, data and format relationship is not exact");
    kept_scene(selector, KEPT_FORMAT, "(128x64, pitch 512, Format 0x00011229) is not exactly the kept image of Data 0x0500000 (128x64, pitch 512, Format 0x00011228)",
               "the header, data and format relationship is not exact");
    kept_scene(selector, KEPT_PITCH, "(128x64, pitch 512, Format 0x00011229) is not exactly the kept image of Data 0x0500000 (128x64, pitch 576, Format 0x00011229)",
               "the header, data and format relationship is not exact");
    kept_scene(selector, KEPT_CPU_FIRST, "cpu written surface", "Data 0x0500000 holds a kept image of an earlier frame but its guest memory is not zero");
    kept_scene(selector, KEPT_CPU_LAST, "cpu written surface", "the CPU wrote it");
    kept_scene(selector, KEPT_ALIAS_ABOVE, "aliases the kept image of Data 0x0500000 at a different offset", "Data 0x0501000");
    kept_scene(selector, KEPT_ALIAS_BELOW, "aliases the kept image of Data 0x0500000 at a different offset", "Data 0x04FF000");
    kept_scene(selector, KEPT_UNMAPPED, "unreadable bytes", "Data 0x0C00000");
    kept_scene(selector, KEPT_ALIAS_DEEP, "aliases the kept image of Data 0x0500000 at a different offset", "Data 0x0507000");
    /* only the width differs: the kept image is 64 texels wide with a pitch of 512 bytes (a pitch wider than its texels), the header 128 texels
     * with the same pitch, which is the tight one a texture needs. Height and pitch and Format agree, so the width term alone refuses it. */
    d3d8_swap_replay_config config = surface_config(selector, true, false);
    CHECK(rt_begin(&config));
    map_surface_memory();
    set_size_word(RT_PRODUCER, 64u, RT_EDGE_HEIGHT, 512u);
    rt_second_header(RT_PRODUCER_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    rt_producer_pass(RT_PRODUCER);
    d3d8_swap_replay_stats stats = rt_present();
    CHECK(!stats.latched && stats.frames_replayed == 1u);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    expect_surface_refusal(&stats, "(128x64, pitch 512, Format 0x00011229) is not exactly the kept image of Data 0x0500000 (64x64, pitch 512, Format 0x00011229)",
                           "the header, data and format relationship is not exact");
    rt_end();
}

/* A producer drawn LATER in the frame than the draw that samples it: with the surface source the surface still held what it held before the frame
 * (zero guest memory in frame 1, frame 1's own image in frame 2: a title that reads last frame's render target), without it the unchanged refusal. */
static void test_surface_out_of_order(const char *selector)
{
    printf("test_surface_out_of_order (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    d3d8_swap_replay_config config = surface_config(selector, false, false);
    CHECK(rt_begin(&config));
    map_surface_memory();
    d3d8_set_texture(0u, RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, UINT32_MAX, RT_CLAMP, RT_LINEAR);
    rt_producer_pass(RT_PRODUCER);
    d3d8_swap_replay_stats stats = rt_present();
    expect_surface_refusal(&stats, "out of order", "starts at draw 1");
    rt_end();

    config = surface_config(selector, true, false);
    CHECK(rt_begin(&config));
    map_surface_memory();
    d3d8_set_texture(0u, RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, UINT32_MAX, RT_CLAMP, RT_LINEAR);
    rt_producer_pass(RT_PRODUCER);
    stats = rt_present();
    if (stats.latched) {
        printf("  frame 1: %s\n", stats.error);
    }
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.passes_replayed == 2u && stats.rt_texture_draws == 1u);
    CHECK(stats.surface_guest_samples == 1u && stats.surface_guest_zero_samples == 1u && stats.surface_kept_samples == 0u);
    CHECK((stats.used_inferences & D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE) != 0u);
    const gpu_image *consumer = d3d8_swap_replay_offscreen_frame(0u, NULL);
    CHECK(consumer != NULL && consumer->pixels != NULL && count_not(consumer, 0u, 0u, 0u, 0u) == 0u); /* the producer's picture is not there yet */
    gpu_image first_producer = copy_of(d3d8_swap_replay_last_frame()); /* RT_PRODUCER is the target bound at the present */
    CHECK(first_producer.pixels != NULL && pixel_is(&first_producer, 32u, 12u, 255u, 0u, 0u, 255u) && pixel_is(&first_producer, 96u, 48u, 255u, 255u, 0u, 255u));
    /* frame 2: the producer now draws the red triangle only, the consumer still reads it first: what it sees is frame 1's picture */
    rt_consumer_pass(RT_CONSUMER, UINT32_MAX, RT_CLAMP, RT_LINEAR);
    rt_red_pass(RT_PRODUCER);
    stats = rt_present();
    if (stats.latched) {
        printf("  frame 2: %s\n", stats.error);
    }
    CHECK(!stats.latched && stats.frames_replayed == 2u && stats.rt_texture_draws == 2u);
    CHECK(stats.surface_kept_samples == 1u && stats.surface_guest_samples == 1u);
    consumer = d3d8_swap_replay_offscreen_frame(0u, NULL);
    const gpu_image *second_producer = d3d8_swap_replay_last_frame();
    CHECK(consumer != NULL && second_producer != NULL && consumer->pixels != NULL && second_producer->pixels != NULL);
    CHECK(images_equal(consumer, &first_producer));
    CHECK(consumer != NULL && consumer->pixels != NULL && pixel_is(consumer, 96u, 48u, 255u, 255u, 0u, 255u));
    CHECK(second_producer != NULL && second_producer->pixels != NULL && pixel_is(second_producer, 96u, 48u, 0u, 0u, 0u, 255u) && pixel_is(second_producer, 32u, 12u, 255u, 0u, 0u, 255u));
    surface_look("out_of_order_frame2_consumer_reads_frame1_producer", consumer);
    free(first_producer.pixels);
    rt_end();
}

/* A blit of the frame that writes the sampled surface before the draw that samples it: its result is applied after the passes ran, so the draw would
 * read a surface without it: refused by name rather than sampled stale. A blit AFTER the draw, or elsewhere, is fine, and the kept image has its pixels. */
static void blit_before_scene(const char *selector, uint32_t blit_destination, bool before, bool pass_producer, bool surface_source,
                              const char *refusal, d3d8_swap_replay_stats *out_stats)
{
    d3d8_swap_replay_config config = surface_config(selector, surface_source, false);
    CHECK(rt_begin(&config));
    map_surface_memory();
    fill_gradient(BLIT_MAP_BASE, 512u, 128u, 64u);
    rt_second_header(TEXTURE_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    const uint32_t source = pass_producer ? RT_PRODUCER_DATA : BLIT_MAP_BASE;
    const uint32_t from = pass_producer ? (28u | (10u << 16)) : 0u;
    const uint32_t to = pass_producer ? (4u | (40u << 16)) : 0u;
    const uint32_t size = pass_producer ? (8u | (8u << 16)) : (16u | (8u << 16));
    if (pass_producer) {
        rt_producer_pass(RT_PRODUCER);
    }
    if (before) {
        push_blit_packet(source, blit_destination, 0xAu, PITCHES_512, from, to, size);
    }
    rt_consumer_pass(RT_CONSUMER, pass_producer ? RT_PRODUCER : TARGET_C, RT_CLAMP, RT_LINEAR);
    if (!before) {
        push_blit_packet(source, blit_destination, 0xAu, PITCHES_512, from, to, size);
    }
    *out_stats = rt_present();
    if (refusal != NULL) {
        expect_surface_refusal(out_stats, "blit before the draw", refusal);
    }
}

static void test_surface_blit_before_the_draw(const char *selector)
{
    printf("test_surface_blit_before_the_draw (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    d3d8_swap_replay_stats stats;
    const gpu_image *consumer = NULL;

    /* a guest memory texture, the blit before the draw writes it */
    blit_before_scene(selector, TEXTURE_DATA, true, false, true, "writes Data 0x0700000 (to 0x0700000) before draw 0 samples it", &stats);
    CHECK(stats.surface_guest_samples == 0u && stats.copies_applied == 0u);
    rt_end();
    /* a blit that touches the texture's first byte only, and the last */
    blit_before_scene(selector, TEXTURE_DATA + TEXTURE_BYTES - 4u, true, false, true, "(to 0x0707FFC)", &stats);
    rt_end();
    /* exactly touching, below: the blit's extent (7 rows of 512 and 16 pixels) ends where the texture starts: no overlap, the draw samples the guest zero */
    const uint32_t extent = 7u * 512u + 16u * 4u;
    blit_before_scene(selector, TEXTURE_DATA - extent, true, false, true, NULL, &stats);
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.surface_guest_zero_samples == 1u && stats.copies_applied == 1u);
    rt_end();
    /* one byte inside: the same blit one byte higher overlaps */
    blit_before_scene(selector, TEXTURE_DATA - extent + 1u, true, false, true, "before draw 0 samples it", &stats);
    rt_end();
    /* above the texture: its first byte past the range is not written */
    blit_before_scene(selector, TEXTURE_DATA + TEXTURE_BYTES, true, false, true, NULL, &stats);
    CHECK(!stats.latched && stats.surface_guest_zero_samples == 1u);
    rt_end();

    /* the blit AFTER the draw: the draw read the surface without it (the guest zero), the kept image of the surface has the blit */
    blit_before_scene(selector, TEXTURE_DATA, false, false, true, NULL, &stats);
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.copies_applied == 1u && stats.blit_guest_surfaces == 2u);
    CHECK(stats.surface_guest_samples == 1u && stats.surface_guest_zero_samples == 1u);
    consumer = d3d8_swap_replay_last_frame();
    CHECK(consumer != NULL && consumer->pixels != NULL && count_not(consumer, 0u, 0u, 0u, 0u) == 0u);
    const gpu_image *kept = d3d8_swap_replay_surface_image(TEXTURE_DATA);
    CHECK(kept != NULL && kept->pixels != NULL && kept->width == 128u && kept->height == 8u);
    if (kept != NULL && kept->pixels != NULL) {
        CHECK(pixel_is_gradient(kept, 0u, 0u, 0u, 0u) && pixel_is_gradient(kept, 15u, 7u, 15u, 7u));
        CHECK(pixel_is(kept, 16u, 0u, 0u, 0u, 0u, 0u) && pixel_is(kept, 0u, 8u - 1u, 1u, 8u, GRADIENT_BLUE, GRADIENT_ALPHA));
    }
    rt_end();

    /* a pass image: the blit between the producer's pass and the consumer's draw writes the producer's surface, the consumer sampled its pass image
     * and would not see the blit: refused. With the surface source off the frame replays, the consumer shows the picture before the blit and the producer
     * image after it (a copy of the red block into the black corner). */
    blit_before_scene(selector, RT_PRODUCER_DATA, true, true, true, "samples the pass image of render target", &stats);
    rt_end();
    blit_before_scene(selector, RT_PRODUCER_DATA, true, true, false, NULL, &stats);
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.copies_applied == 1u && stats.rt_texture_draws == 1u);
    consumer = d3d8_swap_replay_last_frame();
    const gpu_image *producer = d3d8_swap_replay_offscreen_frame(0u, NULL);
    CHECK(consumer != NULL && producer != NULL && consumer->pixels != NULL && producer->pixels != NULL);
    if (consumer != NULL && producer != NULL && consumer->pixels != NULL && producer->pixels != NULL) {
        CHECK(pixel_is(consumer, 6u, 42u, 0u, 0u, 0u, 255u) && pixel_is(consumer, 32u, 12u, 255u, 0u, 0u, 255u));
        CHECK(pixel_is(producer, 6u, 42u, 255u, 0u, 0u, 255u) && !images_equal(consumer, producer));
    }
    rt_end();
}

/* Census mode with the surface source: what each draw's texture would resolve to, no pixel replayed, nothing latched. No device. */
static void test_surface_census(void)
{
    printf("test_surface_census\n");
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    d3d8_swap_replay_config config = surface_config(NULL, true, false);
    config.render_target_texture_census = true;
    uint64_t draws = 0u;
    char want[200];

    CHECK(rt_begin(&config)); /* the back buffer's Data: zero guest memory */
    map_surface_memory();
    const uint32_t back_data = BACK_BUFFER_DATA;
    rt_second_header(back_data, 640u, 480u);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    d3d8_swap_replay_stats stats = rt_present();
    if (stats.latched) {
        printf("  census: %s\n", stats.error);
    }
    CHECK(!stats.latched && stats.census_frames == 1u && stats.frames_replayed == 0u && stats.draws == 0u && d3d8_swap_replay_last_frame() == NULL);
    CHECK(stats.rt_texture_draws == 1u && stats.census_draws == 1u && stats.passes_replayed == 0u);
    CHECK(stats.surface_guest_samples == 0u && stats.surface_guest_zero_samples == 0u && stats.surface_kept_samples == 0u); /* nothing was sampled */
    snprintf(want, sizeof want, "stage 0 from guest memory surface (Data 0x%07X, 640x480, Format 0x00011229) no replayed pass drew: all zero, transparent black", (unsigned)back_data);
    CHECK(census_has(want, NULL, &draws) && draws == 1u && d3d8_swap_replay_texture_census_count() == 1u);
    rt_end();

    CHECK(rt_begin(&config)); /* bytes the CPU wrote */
    map_surface_memory();
    store(TEXTURE_DATA + 0x100u, 0x00000001u);
    rt_second_header(TEXTURE_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    CHECK(!stats.latched && stats.census_frames == 1u && stats.rt_texture_draws == 1u);
    CHECK(census_has("stage 0 from guest memory surface (Data 0x0700000, 128x64, Format 0x00011229) no replayed pass drew: read at the present", NULL, &draws) && draws == 1u);
    rt_end();

    CHECK(rt_begin(&config)); /* unreadable bytes: a named refusal, counted past it, no latch */
    map_surface_memory();
    rt_second_header(UNMAPPED_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    CHECK(!stats.latched && stats.census_frames == 1u && stats.rt_texture_draws == 0u);
    snprintf(want, sizeof want, "(drawing into target 0x%08X)", (unsigned)RT_CONSUMER);
    CHECK(census_has("REFUSED stage 0 texture: unreadable bytes,", want, &draws) && draws == 1u);
    rt_end();

    CHECK(rt_begin(&config)); /* a blit of the frame before the draw */
    map_surface_memory();
    rt_second_header(TEXTURE_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    push_blit_packet(BLIT_MAP_BASE, TEXTURE_DATA, 0xAu, PITCHES_512, 0u, 0u, 16u | (8u << 16));
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    CHECK(!stats.latched && stats.census_frames == 1u && stats.rt_texture_draws == 0u);
    CHECK(census_has("REFUSED stage 0 texture: blit before the draw,", want, &draws) && draws == 1u);
    rt_end();

    config.surface_source = false; /* the option off: the unchanged category */
    config.allowed_inferences &= ~D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE;
    CHECK(rt_begin(&config));
    map_surface_memory();
    rt_second_header(TEXTURE_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    CHECK(!stats.latched && stats.census_frames == 1u && stats.rt_texture_draws == 0u);
    CHECK(census_has("REFUSED stage 0 texture: no earlier image,", want, &draws) && draws == 1u && !census_has("guest memory surface", NULL, NULL));
    rt_end();
}

/* Frame 1 draws the red and yellow triangles into the producer (Data 0x500000, 128x64) and frame 2 draws the yellow one only, without clearing (or
 * after a small clear). The persistence scene: the images of both frames and their counters. */
static void persist_two_frames(const char *selector, bool persist, bool with_clear, gpu_image *first, gpu_image *second,
                               d3d8_swap_replay_stats *first_stats, d3d8_swap_replay_stats *second_stats)
{
    d3d8_swap_replay_config config = surface_config(selector, false, persist);
    CHECK(rt_begin(&config));
    map_surface_memory();
    rt_producer_pass(RT_PRODUCER);
    *first_stats = rt_present();
    *first = copy_of(d3d8_swap_replay_last_frame());
    rt_yellow_pass(RT_PRODUCER, with_clear);
    *second_stats = rt_present();
    CHECK(!first_stats->latched && !second_stats->latched);
    *second = copy_of(d3d8_swap_replay_last_frame());
    char text[2400];
    if (persist) {
        CHECK(d3d8_swap_replay_surface_summary(text, sizeof text) > 0u);
        CHECK(strstr(text, "target persistence ON: 1 pass(es) started from the kept image") != NULL && strstr(text, "surface source off") != NULL);
    } else {
        CHECK(d3d8_swap_replay_surface_summary(text, sizeof text) == 0u && text[0] == '\0'); /* neither option is on: silent */
    }
    rt_end();
}

static uint32_t green_pixels(const gpu_image *image)
{
    return image->width * image->height - count_not(image, 0u, 255u, 0u, 255u);
}

typedef enum {
    PERSIST_SIZE,
    PERSIST_HEIGHT,
    PERSIST_FORMAT,
    PERSIST_PITCH,
    PERSIST_NONZERO_FIRST,
    PERSIST_NONZERO_LAST,
    PERSIST_UNREADABLE,
    PERSIST_PAST_RANGE,
    PERSIST_WIDTH,          /* only the width differs: 64x64 over the 128x64 image, the same pitch */
    PERSIST_PAD_CLEAN,      /* the header's pitch is 576, wider than the 128 texel rows, in both frames */
    PERSIST_PAD_CPU_LAST    /* the same, and the CPU wrote the last dword of the padding of the last row */
} persist_case;

/* Frame 2 draws into a surface whose kept image no longer matches or whose guest memory the CPU wrote: refused by name (and the one case in which the
 * guest bytes lie past the range stays a persisted pass). */
static void persist_refusal_scene(const char *selector, persist_case which, const char *words)
{
    d3d8_swap_replay_config config = surface_config(selector, false, true);
    CHECK(rt_begin(&config));
    if (which != PERSIST_UNREADABLE) {
        map_surface_memory();
    }
    if (which == PERSIST_PAD_CLEAN || which == PERSIST_PAD_CPU_LAST) {
        set_size_word(RT_PRODUCER, RT_EDGE_WIDTH, RT_EDGE_HEIGHT, 576u);
    }
    rt_producer_pass(RT_PRODUCER);
    d3d8_swap_replay_stats stats = rt_present();
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.targets_persisted == 0u);
    switch (which) {
    case PERSIST_SIZE: make_rt_target(RT_PRODUCER, RT_PRODUCER_DATA, 64u, 64u); break;
    case PERSIST_HEIGHT: make_rt_target(RT_PRODUCER, RT_PRODUCER_DATA, 128u, 32u); break;
    case PERSIST_FORMAT: store(RT_PRODUCER + D3D8_SURFACE_FORMAT, 0x00011228u); break;
    case PERSIST_PITCH: store(RT_PRODUCER + D3D8_SURFACE_SIZE, (8u << 24) | ((RT_EDGE_HEIGHT - 1u) << 12) | (RT_EDGE_WIDTH - 1u)); break;
    case PERSIST_NONZERO_FIRST: store(RT_PRODUCER_DATA, 0x00000100u); break;
    case PERSIST_NONZERO_LAST: store(RT_PRODUCER_DATA + TEXTURE_BYTES - 4u, 0x01000000u); break;
    case PERSIST_PAST_RANGE: store(RT_PRODUCER_DATA + TEXTURE_BYTES, 0xFFFFFFFFu); break;
    case PERSIST_WIDTH: set_size_word(RT_PRODUCER, 64u, 64u, 512u); break;
    case PERSIST_PAD_CPU_LAST: store(RT_PRODUCER_DATA + 576u * RT_EDGE_HEIGHT - 4u, 0x01000000u); break;
    case PERSIST_PAD_CLEAN:
    case PERSIST_UNREADABLE: break;
    }
    rt_yellow_pass(RT_PRODUCER, false);
    stats = rt_present();
    if (words != NULL) {
        expect_surface_refusal(&stats, "target persistence:", words);
        CHECK(stats.targets_persisted == 0u && stats.frames_replayed == 1u);
    } else {
        if (stats.latched) {
            printf("  frame 2: %s\n", stats.error);
        }
        CHECK(!stats.latched && stats.frames_replayed == 2u && stats.targets_persisted == 1u);
    }
    rt_end();
}

/* A render target pass starts from the image its Data word held, not from the clear colour: what an earlier frame drew and the blits since survive,
 * the pass's own clear applies on top, a surface nothing drew starts fresh, and what does not match is refused by name. */
static void test_surface_target_persist(const char *selector)
{
    printf("test_surface_target_persist (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    gpu_image on_first = {0}, on_second = {0}, off_first = {0}, off_second = {0};
    d3d8_swap_replay_stats on_first_stats, on_second_stats, off_first_stats, off_second_stats;
    persist_two_frames(selector, true, false, &on_first, &on_second, &on_first_stats, &on_second_stats);
    persist_two_frames(selector, false, false, &off_first, &off_second, &off_first_stats, &off_second_stats);
    CHECK(on_first.pixels != NULL && on_second.pixels != NULL && off_first.pixels != NULL && off_second.pixels != NULL);
    if (on_first.pixels != NULL && on_second.pixels != NULL && off_first.pixels != NULL && off_second.pixels != NULL) {
        /* frame 1: nothing was kept yet, so persistence changes nothing: the pass starts from the clear colour as ever */
        CHECK(on_first_stats.targets_persisted == 0u && (on_first_stats.used_inferences & GPU_PGRAPH_INFER_OUTPUT_TARGET_PERSIST) == 0u);
        CHECK(images_equal(&on_first, &off_first));
        CHECK(pixel_is(&on_first, 32u, 12u, 255u, 0u, 0u, 255u) && pixel_is(&on_first, 96u, 48u, 255u, 255u, 0u, 255u) && pixel_is(&on_first, 2u, 2u, 0u, 0u, 0u, 255u));
        /* frame 2 with persistence: frame 1's red triangle survives under the new yellow one (the same picture) */
        CHECK(on_second_stats.targets_persisted == 1u && (on_second_stats.used_inferences & GPU_PGRAPH_INFER_OUTPUT_TARGET_PERSIST) != 0u);
        CHECK(on_second_stats.frames_replayed == 2u && on_second_stats.draws == 3u);
        CHECK(pixel_is(&on_second, 32u, 12u, 255u, 0u, 0u, 255u) && pixel_is(&on_second, 20u, 10u, 255u, 0u, 0u, 255u));
        CHECK(pixel_is(&on_second, 96u, 48u, 255u, 255u, 0u, 255u) && pixel_is(&on_second, 2u, 2u, 0u, 0u, 0u, 255u));
        CHECK(images_equal(&on_second, &on_first));
        /* the same frames without it: the clear colour, the yellow triangle alone */
        CHECK(off_second_stats.targets_persisted == 0u && (off_second_stats.used_inferences & GPU_PGRAPH_INFER_OUTPUT_TARGET_PERSIST) == 0u);
        CHECK(pixel_is(&off_second, 32u, 12u, 0u, 0u, 0u, 255u) && pixel_is(&off_second, 20u, 10u, 0u, 0u, 0u, 255u));
        CHECK(pixel_is(&off_second, 96u, 48u, 255u, 255u, 0u, 255u) && pixel_is(&off_second, 2u, 2u, 0u, 0u, 0u, 255u));
        CHECK(count_not(&off_second, 0u, 0u, 0u, 255u) > 100u && count_not(&off_second, 0u, 0u, 0u, 255u) < count_not(&on_second, 0u, 0u, 0u, 255u));
        CHECK(!images_equal(&on_second, &off_second));
        surface_look("persist_frame1_red_and_yellow", &on_first);
        surface_look("persist_frame2_yellow_only_persist_on", &on_second);
        surface_look("persist_frame2_yellow_only_persist_off", &off_second);
    }
    free(on_first.pixels);
    free(on_second.pixels);
    free(off_first.pixels);
    free(off_second.pixels);

    /* the pass's own clear (x 24..40, y 8..15 to green) applies ON TOP of the kept pixels: it wipes the kept red at (32, 12) and not at (32, 20) */
    persist_two_frames(selector, true, true, &on_first, &on_second, &on_first_stats, &on_second_stats);
    persist_two_frames(selector, false, true, &off_first, &off_second, &off_first_stats, &off_second_stats);
    CHECK(on_second.pixels != NULL && off_second.pixels != NULL);
    if (on_second.pixels != NULL && off_second.pixels != NULL) {
        CHECK(on_second_stats.targets_persisted == 1u && (on_second_stats.used_inferences & GPU_PGRAPH_INFER_OUTPUT_CLEAR_MODEL) != 0u);
        CHECK(pixel_is(&on_second, 32u, 12u, 0u, 255u, 0u, 255u) && pixel_is(&on_second, 32u, 20u, 255u, 0u, 0u, 255u));
        CHECK(pixel_is(&on_second, 96u, 48u, 255u, 255u, 0u, 255u) && pixel_is(&on_second, 20u, 30u, 0u, 0u, 0u, 255u));
        CHECK(pixel_is(&on_second, 24u, 8u, 0u, 255u, 0u, 255u) && pixel_is(&on_second, 40u, 15u, 0u, 255u, 0u, 255u));
        CHECK(pixel_is(&on_second, 23u, 12u, 255u, 0u, 0u, 255u) && pixel_is(&on_second, 41u, 12u, 255u, 0u, 0u, 255u) && pixel_is(&on_second, 32u, 16u, 255u, 0u, 0u, 255u));
        CHECK(green_pixels(&on_second) == 17u * 8u && green_pixels(&off_second) == 17u * 8u);
        CHECK(pixel_is(&off_second, 32u, 12u, 0u, 255u, 0u, 255u) && pixel_is(&off_second, 32u, 20u, 0u, 0u, 0u, 255u));
        CHECK(on_first.pixels != NULL && count_not(&on_first, 0u, 0u, 0u, 255u) > 300u && !images_equal(&on_second, &on_first));
        surface_look("persist_frame2_with_clear_persist_on", &on_second);
    }
    free(on_first.pixels);
    free(on_second.pixels);
    free(off_first.pixels);
    free(off_second.pixels);

    /* what a blit left in the surface is kept too: a red block copied into the black corner (frame 2, no draw) is under frame 3's yellow triangle */
    for (int persist = 1; persist >= 0; persist--) {
        d3d8_swap_replay_config config = surface_config(selector, false, persist != 0);
        CHECK(rt_begin(&config));
        map_surface_memory();
        rt_producer_pass(RT_PRODUCER);
        d3d8_swap_replay_stats stats = rt_present();
        CHECK(!stats.latched && stats.frames_replayed == 1u);
        push_blit_packet(RT_PRODUCER_DATA, RT_PRODUCER_DATA, 0xAu, PITCHES_512, 28u | (10u << 16), 4u | (40u << 16), 8u | (8u << 16));
        d3d8_gpu_kick();
        stats = rt_present();
        CHECK(!stats.latched && stats.frames_empty == 1u && stats.copies_applied == 1u && stats.targets_persisted == 0u);
        rt_yellow_pass(RT_PRODUCER, false);
        stats = rt_present();
        CHECK(!stats.latched && stats.frames_replayed == 2u && stats.targets_persisted == (persist != 0 ? 1u : 0u));
        const gpu_image *frame = d3d8_swap_replay_last_frame();
        CHECK(frame != NULL && frame->pixels != NULL);
        if (frame != NULL && frame->pixels != NULL) {
            CHECK(pixel_is(frame, 96u, 48u, 255u, 255u, 0u, 255u));
            CHECK(pixel_is(frame, 6u, 42u, persist != 0 ? 255u : 0u, 0u, 0u, 255u));
            CHECK(pixel_is(frame, 4u, 40u, persist != 0 ? 255u : 0u, 0u, 0u, 255u) && pixel_is(frame, 11u, 47u, persist != 0 ? 255u : 0u, 0u, 0u, 255u));
            CHECK(pixel_is(frame, 12u, 48u, 0u, 0u, 0u, 255u) && pixel_is(frame, 3u, 39u, 0u, 0u, 0u, 255u));
            CHECK(pixel_is(frame, 32u, 12u, persist != 0 ? 255u : 0u, 0u, 0u, 255u));
            if (persist != 0) {
                surface_look("persist_frame3_after_a_blit", frame);
            }
        }
        rt_end();
    }

    /* a surface with no kept image starts fresh: a second target drawn for the first time in frame 2 equals the same pass without persistence */
    for (int persist = 1; persist >= 0; persist--) {
        d3d8_swap_replay_config config = surface_config(selector, false, persist != 0);
        CHECK(rt_begin(&config));
        map_surface_memory();
        rt_producer_pass(RT_PRODUCER);
        d3d8_swap_replay_stats stats = rt_present();
        CHECK(!stats.latched);
        rt_yellow_pass(RT_CONSUMER, false); /* RT_CONSUMER (Data 0x600000) was never drawn */
        stats = rt_present();
        CHECK(!stats.latched && stats.frames_replayed == 2u && stats.targets_persisted == 0u);
        const gpu_image *frame = d3d8_swap_replay_last_frame();
        CHECK(frame != NULL && frame->pixels != NULL && pixel_is(frame, 96u, 48u, 255u, 255u, 0u, 255u) && pixel_is(frame, 32u, 12u, 0u, 0u, 0u, 255u));
        rt_end();
    }

    persist_refusal_scene(selector, PERSIST_SIZE,
                          "the kept image of Data 0x0500000 is 128x64 (pitch 512, Format 0x00011229) but the target drawn into it is 64x64 (pitch 256, Format 0x00011229)");
    persist_refusal_scene(selector, PERSIST_HEIGHT,
                          "the kept image of Data 0x0500000 is 128x64 (pitch 512, Format 0x00011229) but the target drawn into it is 128x32 (pitch 512, Format 0x00011229)");
    persist_refusal_scene(selector, PERSIST_FORMAT,
                          "the kept image of Data 0x0500000 is 128x64 (pitch 512, Format 0x00011229) but the target drawn into it is 128x64 (pitch 512, Format 0x00011228)");
    persist_refusal_scene(selector, PERSIST_PITCH,
                          "the kept image of Data 0x0500000 is 128x64 (pitch 512, Format 0x00011229) but the target drawn into it is 128x64 (pitch 576, Format 0x00011229)");
    persist_refusal_scene(selector, PERSIST_NONZERO_FIRST, "the guest memory under the kept image of Data 0x0500000 is not zero");
    persist_refusal_scene(selector, PERSIST_NONZERO_LAST, "the guest memory under the kept image of Data 0x0500000 is not zero");
    persist_refusal_scene(selector, PERSIST_UNREADABLE, "the guest memory under the kept image of Data 0x0500000 is unreadable");
    persist_refusal_scene(selector, PERSIST_PAST_RANGE, NULL);
    persist_refusal_scene(selector, PERSIST_WIDTH,
                          "the kept image of Data 0x0500000 is 128x64 (pitch 512, Format 0x00011229) but the target drawn into it is 64x64 (pitch 512, Format 0x00011229)");
    persist_refusal_scene(selector, PERSIST_PAD_CLEAN, NULL);
    persist_refusal_scene(selector, PERSIST_PAD_CPU_LAST, "the guest memory under the kept image of Data 0x0500000 is not zero");

    /* images are kept for persistence alone: no BLIT output group, no surface source, only the persistent target */
    d3d8_swap_replay_config config = surface_config(selector, false, true);
    config.output_groups &= ~GPU_PGRAPH_OUTPUT_BLIT;
    CHECK(rt_begin(&config));
    map_surface_memory();
    rt_producer_pass(RT_PRODUCER);
    CHECK(rt_present().frames_replayed == 1u);
    CHECK(d3d8_swap_replay_surface_image(RT_PRODUCER_DATA) != NULL);
    rt_yellow_pass(RT_PRODUCER, false);
    const d3d8_swap_replay_stats stats = rt_present();
    CHECK(!stats.latched && stats.targets_persisted == 1u);
    const gpu_image *frame = d3d8_swap_replay_last_frame();
    CHECK(frame != NULL && frame->pixels != NULL && pixel_is(frame, 32u, 12u, 255u, 0u, 0u, 255u));
    rt_end();
    config = surface_config(selector, false, false); /* and with nothing that reads them, none are kept */
    config.output_groups &= ~GPU_PGRAPH_OUTPUT_BLIT;
    CHECK(rt_begin(&config));
    map_surface_memory();
    rt_producer_pass(RT_PRODUCER);
    CHECK(rt_present().frames_replayed == 1u);
    CHECK(d3d8_swap_replay_surface_image(RT_PRODUCER_DATA) == NULL);
    rt_end();
}

/* --- T596: the blits over surfaces the replay holds no image of ---------------------------------------------------------------------------------- */

static d3d8_swap_replay_stats blit_present(uint32_t source, uint32_t destination, uint32_t format, uint32_t pitches, uint32_t in_x, uint32_t in_y,
                                           uint32_t out_x, uint32_t out_y, uint32_t width, uint32_t height)
{
    push_blit_packet(source, destination, format, pitches, in_x | (in_y << 16), out_x | (out_y << 16), width | (height << 16));
    d3d8_gpu_kick();
    return rt_present();
}

static uint8_t byte_pattern(uint32_t index)
{
    return (uint8_t)((index * 7u + 3u) % 251u + 1u); /* 1 .. 251, never zero */
}

static void fill_bytes(uint32_t data, uint32_t length)
{
    for (uint32_t index = 0u; index < length; index += 4u) {
        store(data + index, (uint32_t)byte_pattern(index) | ((uint32_t)byte_pattern(index + 1u) << 8) | ((uint32_t)byte_pattern(index + 2u) << 16) |
                                ((uint32_t)byte_pattern(index + 3u) << 24));
    }
}

/* The bytes of a byte surface as the model holds them (kept bytes over the guest's), `length` of them. */
static bool read_model(uint32_t data, uint8_t *out, size_t length)
{
    return d3d8_surface_model_read_bytes(data, out, length) == D3D8_SURFACE_MODEL_OK;
}

static uint32_t nonzero_bytes(const uint8_t *bytes, size_t length)
{
    uint32_t count = 0u;
    for (size_t i = 0u; i < length; i++) {
        count += bytes[i] != 0u;
    }
    return count;
}

#define BLIT_SOURCE BLIT_MAP_BASE
#define BLIT_DESTINATION (BLIT_MAP_BASE + 0x10000u)

/* Defined with the format tests below, used by the guest surface tests above them. */
static uint32_t blit_source_argb(uint32_t x, uint32_t y);
static void fill_generated(uint32_t data, uint32_t pitch, uint32_t width, uint32_t rows, uint32_t (*pixel)(uint32_t, uint32_t));

/* A8R8G8B8 blits between surfaces no pass drew: both are built from the guest rows the blit reaches, the blit moves pixels, a taller one grows them. */
static void test_surface_blit_guest(const char *selector)
{
    printf("test_surface_blit_guest (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    d3d8_swap_replay_config config = surface_config(selector, true, false);
    CHECK(rt_begin(&config));
    map_surface_memory();
    fill_gradient(BLIT_SOURCE, 256u, 64u, 64u);
    d3d8_swap_replay_stats stats = blit_present(BLIT_SOURCE, BLIT_DESTINATION, 0xAu, PITCHES_256, 4u, 2u, 8u, 4u, 16u, 8u);
    if (stats.latched) {
        printf("  frame 1: %s\n", stats.error);
    }
    CHECK(!stats.latched && stats.frames_empty == 1u && stats.copies_applied == 1u && stats.copies_empty == 0u);
    CHECK(stats.blit_guest_surfaces == 2u && stats.byte_copies_applied == 0u);
    CHECK((stats.used_inferences & GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL) != 0u);
    CHECK((stats.used_inferences & D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE) != 0u);
    const gpu_image *source = d3d8_swap_replay_surface_image(BLIT_SOURCE);
    const gpu_image *destination = d3d8_swap_replay_surface_image(BLIT_DESTINATION);
    CHECK(source != NULL && destination != NULL && source->pixels != NULL && destination->pixels != NULL);
    if (source != NULL && destination != NULL && source->pixels != NULL && destination->pixels != NULL) {
        /* only the rows the blit reaches: source rows 0 to 9 (2 + 8), destination rows 0 to 11 (4 + 8), the pitch's 64 pixels wide */
        CHECK(source->width == 64u && source->height == 10u && destination->width == 64u && destination->height == 12u);
        CHECK(pixel_is_gradient(source, 0u, 0u, 0u, 0u) && pixel_is_gradient(source, 63u, 9u, 63u, 9u) && pixel_is_gradient(source, 20u, 5u, 20u, 5u));
        uint32_t wrong = 0u;
        for (uint32_t y = 0u; y < 8u; y++) {
            for (uint32_t x = 0u; x < 16u; x++) {
                wrong += !pixel_is_gradient(destination, 8u + x, 4u + y, 4u + x, 2u + y);
            }
        }
        CHECK(wrong == 0u);
        CHECK(count_not(destination, 0u, 0u, 0u, 0u) == 16u * 8u);                                           /* nothing else moved: the rest is the guest's zero */
        CHECK(pixel_is(destination, 7u, 4u, 0u, 0u, 0u, 0u) && pixel_is(destination, 24u, 4u, 0u, 0u, 0u, 0u) && pixel_is(destination, 8u, 3u, 0u, 0u, 0u, 0u));
        CHECK(pixel_is(destination, 8u, 4u, 5u, 3u, GRADIENT_BLUE, GRADIENT_ALPHA) && pixel_is(destination, 23u, 11u, 20u, 10u, GRADIENT_BLUE, GRADIENT_ALPHA));
    }
    CHECK(d3d8_surface_model_probe(BLIT_DESTINATION, 256u * 64u) == D3D8_SURFACE_ZERO); /* the guest memory is never written */

    /* a taller blit grows the rows both surfaces hold: source rows 10 to 19, destination rows 12 to 19 come from the guest, the first blit stays */
    stats = blit_present(BLIT_SOURCE, BLIT_DESTINATION, 0xAu, PITCHES_256, 0u, 0u, 0u, 0u, 16u, 20u);
    if (stats.latched) {
        printf("  frame 2: %s\n", stats.error);
    }
    CHECK(!stats.latched && stats.frames_empty == 2u && stats.copies_applied == 2u && stats.blit_guest_surfaces == 2u);
    source = d3d8_swap_replay_surface_image(BLIT_SOURCE);
    destination = d3d8_swap_replay_surface_image(BLIT_DESTINATION);
    CHECK(source != NULL && destination != NULL && source->pixels != NULL && destination->pixels != NULL);
    if (source != NULL && destination != NULL && source->pixels != NULL && destination->pixels != NULL) {
        CHECK(source->width == 64u && source->height == 20u && destination->width == 64u && destination->height == 20u);
        CHECK(pixel_is_gradient(source, 3u, 19u, 3u, 19u) && pixel_is_gradient(source, 63u, 9u, 63u, 9u));
        CHECK(pixel_is_gradient(destination, 5u, 6u, 5u, 6u) && pixel_is_gradient(destination, 10u, 6u, 10u, 6u)); /* frame 2's copy over frame 1's */
        CHECK(pixel_is_gradient(destination, 0u, 15u, 0u, 15u) && pixel_is_gradient(destination, 15u, 19u, 15u, 19u));
        CHECK(pixel_is_gradient(destination, 20u, 6u, 16u, 4u));                                                  /* frame 1's copy, outside frame 2's */
        CHECK(pixel_is(destination, 16u, 19u, 0u, 0u, 0u, 0u) && pixel_is(destination, 20u, 14u, 0u, 0u, 0u, 0u));
    }
    rt_end();
    CHECK(d3d8_swap_replay_surface_image(BLIT_SOURCE) == NULL);

    /* the option off: the unchanged refusal, nothing is built from guest memory */
    config = surface_config(selector, false, false);
    CHECK(rt_begin(&config));
    map_surface_memory();
    fill_gradient(BLIT_SOURCE, 256u, 64u, 64u);
    stats = blit_present(BLIT_SOURCE, BLIT_DESTINATION, 0xAu, PITCHES_256, 4u, 2u, 8u, 4u, 16u, 8u);
    expect_surface_refusal(&stats, "holds no image of the source surface (Data 0x0A00000)", NULL);
    CHECK(stats.blit_guest_surfaces == 0u && stats.copies_applied == 0u);
    rt_end();

    /* refusals by name: an unreadable surface, an alias of the surface just built, a pitch the guest model cannot read, a surface held as bytes */
    config = surface_config(selector, true, false);
    CHECK(rt_begin(&config));
    map_surface_memory();
    stats = blit_present(UNMAPPED_DATA, BLIT_DESTINATION, 0xAu, PITCHES_256, 0u, 0u, 0u, 0u, 16u, 8u);
    expect_surface_refusal(&stats, "the source surface (Data 0x0C00000)", "cannot be read from guest memory");
    rt_end();
    CHECK(rt_begin(&config));
    map_surface_memory();
    stats = blit_present(BLIT_SOURCE, BLIT_SOURCE + 0x400u, 0xAu, PITCHES_256, 0u, 0u, 0u, 0u, 16u, 8u);
    expect_surface_refusal(&stats, "the destination surface (Data 0x0A00400) aliases the kept image of Data 0x0A00000 at a different offset", NULL);
    rt_end();
    CHECK(rt_begin(&config));
    map_surface_memory();
    stats = blit_present(BLIT_SOURCE, BLIT_DESTINATION, 0xAu, 0x01020102u, 60u, 0u, 0u, 0u, 16u, 8u);
    expect_surface_refusal(&stats, "the source surface (Data 0x0A00000) has a pitch of 258 bytes", "not an A8R8G8B8 surface the guest model can read");
    rt_end();
    CHECK(rt_begin(&config)); /* the source of an A8R8G8B8 blit that an earlier byte blit wrote */
    map_surface_memory();
    fill_bytes(BLIT_SOURCE, 512u);
    stats = blit_present(BLIT_SOURCE, BLIT_DESTINATION, 1u, 0x00400040u, 0u, 0u, 0u, 0u, 8u, 4u);
    CHECK(!stats.latched && stats.byte_copies_applied == 1u);
    stats = blit_present(BLIT_DESTINATION, BLIT_MAP_BASE + 0x20000u, 0xAu, PITCHES_256, 0u, 0u, 0u, 0u, 16u, 2u);
    expect_surface_refusal(&stats, "the source surface (Data 0x0A10000) is held as bytes by an earlier byte path blit", NULL);
    rt_end();
}

/* T769, the reference of the byte blit, per pixel and independent of the implementation: the row clamped in PIXELS to the narrower pitch,
 * rows ascending, a row read whole then written, flat addresses (a row past its pitch runs on into the next row's bytes). `destination` may be
 * `source`. */
static void reference_byte_blit(const gpu_pgraph_copy *copy, const uint8_t *source, uint8_t *destination)
{
    const uint32_t bytes_per_pixel = copy->color_format == GPU_PGRAPH_BLIT_FORMAT_Y8 ? 1u : 2u;
    uint32_t pixels = copy->width;
    pixels = copy->source_pitch / bytes_per_pixel < pixels ? copy->source_pitch / bytes_per_pixel : pixels;
    pixels = copy->destination_pitch / bytes_per_pixel < pixels ? copy->destination_pitch / bytes_per_pixel : pixels;
    uint8_t row[1024];
    for (uint32_t line = 0u; line < copy->height; line++) {
        for (uint32_t byte = 0u; byte < pixels * bytes_per_pixel; byte++) {
            row[byte] = source[(size_t)(copy->in_y + line) * copy->source_pitch + (size_t)copy->in_x * bytes_per_pixel + byte];
        }
        for (uint32_t byte = 0u; byte < pixels * bytes_per_pixel; byte++) {
            destination[(size_t)(copy->out_y + line) * copy->destination_pitch + (size_t)copy->out_x * bytes_per_pixel + byte] = row[byte];
        }
    }
}

/* The byte path (Y8 and R5G6B5, the CopyRects blits over guest byte surfaces): rows of bytes, never written to guest memory, named refusals. */
static void test_surface_blit_bytes(const char *selector)
{
    printf("test_surface_blit_bytes (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    const uint32_t a = BLIT_MAP_BASE + 0x20000u;
    const uint32_t b = BLIT_MAP_BASE + 0x30000u;
    const uint32_t c = BLIT_MAP_BASE + 0x40000u;
    const uint32_t e = BLIT_MAP_BASE + 0x50000u;
    const uint32_t f = BLIT_MAP_BASE + 0x60000u;
    uint8_t model[1024];
    uint8_t guest[1024];
    d3d8_swap_replay_config config = surface_config(selector, true, false);
    CHECK(rt_begin(&config));
    map_surface_memory();
    fill_bytes(a, 512u);
    fill_bytes(c, 1024u);

    /* Y8, pitch 64: (2, 1) 8 x 4 of A to (5, 3) of B */
    d3d8_swap_replay_stats stats = blit_present(a, b, 1u, 0x00400040u, 2u, 1u, 5u, 3u, 8u, 4u);
    if (stats.latched) {
        printf("  Y8: %s\n", stats.error);
    }
    CHECK(!stats.latched && stats.frames_empty == 1u && stats.copies_applied == 1u && stats.byte_copies_applied == 1u && stats.blit_guest_surfaces == 0u);
    CHECK((stats.used_inferences & GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL) != 0u && (stats.used_inferences & D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE) != 0u);
    CHECK(d3d8_swap_replay_surface_image(a) == NULL && d3d8_swap_replay_surface_image(b) == NULL); /* bytes, no image */
    CHECK(d3d8_surface_model_byte_surface_count() == 2u);
    CHECK(d3d8_surface_model_byte_surface_length(a) == 266u && d3d8_surface_model_byte_surface_length(b) == 397u); /* (y + h - 1) pitch + (x + w) bytes */
    CHECK(read_model(b, model, 512u));
    uint32_t wrong = 0u;
    for (uint32_t row = 0u; row < 4u; row++) {
        for (uint32_t column = 0u; column < 8u; column++) {
            wrong += model[(3u + row) * 64u + 5u + column] != byte_pattern((1u + row) * 64u + 2u + column);
        }
    }
    CHECK(wrong == 0u && nonzero_bytes(model, 512u) == 32u);
    CHECK(d3d8_surface_model_probe(b, 512u) == D3D8_SURFACE_ZERO); /* the guest memory under the destination is not written */
    CHECK(d3d8_gpu_read_guest(NULL, a, guest, 512u));
    wrong = 0u;
    for (uint32_t index = 0u; index < 512u; index++) {
        wrong += guest[index] != byte_pattern(index);
    }
    CHECK(wrong == 0u); /* nor the source */

    /* a second blit reads the KEPT bytes of B (its guest memory is zero): B (5, 3) 8 x 4 to F (0, 0) */
    stats = blit_present(b, f, 1u, 0x00400040u, 5u, 3u, 0u, 0u, 8u, 4u);
    CHECK(!stats.latched && stats.byte_copies_applied == 2u && stats.copies_applied == 2u && d3d8_surface_model_byte_surface_count() == 3u);
    CHECK(read_model(f, model, 512u));
    wrong = 0u;
    for (uint32_t row = 0u; row < 4u; row++) {
        for (uint32_t column = 0u; column < 8u; column++) {
            wrong += model[row * 64u + column] != byte_pattern((1u + row) * 64u + 2u + column);
        }
    }
    CHECK(wrong == 0u && nonzero_bytes(model, 512u) == 32u && d3d8_surface_model_byte_surface_length(f) == 200u);

    /* R5G6B5, pitch 128: 2 bytes a pixel, (3, 1) 6 x 3 of C to (10, 2) of E: 12 bytes a row */
    stats = blit_present(c, e, 4u, 0x00800080u, 3u, 1u, 10u, 2u, 6u, 3u);
    CHECK(!stats.latched && stats.byte_copies_applied == 3u && stats.copies_applied == 3u);
    CHECK(d3d8_surface_model_byte_surface_length(c) == 402u && d3d8_surface_model_byte_surface_length(e) == 544u);
    CHECK(read_model(e, model, 1024u));
    wrong = 0u;
    for (uint32_t row = 0u; row < 3u; row++) {
        for (uint32_t byte = 0u; byte < 12u; byte++) {
            wrong += model[(2u + row) * 128u + 20u + byte] != byte_pattern((1u + row) * 128u + 6u + byte);
        }
    }
    CHECK(wrong == 0u && nonzero_bytes(model, 1024u) == 36u);
    CHECK(d3d8_surface_model_probe(e, 1024u) == D3D8_SURFACE_ZERO);

    /* inside one surface with disjoint rectangles: Y8 (0, 0) 8 x 4 to (16, 0) of A, the rest of A is untouched */
    stats = blit_present(a, a, 1u, 0x00400040u, 0u, 0u, 16u, 0u, 8u, 4u);
    CHECK(!stats.latched && stats.byte_copies_applied == 4u);
    CHECK(read_model(a, model, 512u));
    wrong = 0u;
    for (uint32_t index = 0u; index < 512u; index++) {
        const uint32_t row = index / 64u;
        const uint32_t column = index % 64u;
        const bool moved = row < 4u && column >= 16u && column < 24u;
        wrong += model[index] != byte_pattern(moved ? row * 64u + (column - 16u) : index);
    }
    CHECK(wrong == 0u);
    /* a full row (width 64 == the pitch) is no clamp */
    stats = blit_present(a, b, 1u, 0x00400040u, 0u, 0u, 0u, 4u, 64u, 2u);
    CHECK(!stats.latched && stats.byte_copies_applied == 5u);
    rt_end();
    CHECK(d3d8_surface_model_byte_surface_count() == 0u); /* disable forgets the byte surfaces */

    /* T769: the clamp (in pixels, every format and either pitch), the overlap inside one surface, two pitches over one surface, a row that
     * runs on into the next one and an odd pitch are all copied as the xemu rules say, each from a fresh model, against a per byte reference */
    static const struct {
        uint32_t source;
        uint32_t destination;
        uint32_t format;
        uint32_t pitches;
        uint32_t in_x, in_y, out_x, out_y, width, height;
    } cases[] = {
        {0u, 0u, 1u, 0x00400040u, 0u, 0u, 4u, 2u, 8u, 8u},
        {0u, 0u, 1u, 0x00800040u, 0u, 0u, 16u, 0u, 8u, 4u},
        {0u, 1u, 1u, 0x00800040u, 40u, 0u, 0u, 0u, 40u, 4u},
        {0u, 0u, 1u, 0x00400040u, 0u, 0u, 0u, 1u, 64u, 6u},
        {0u, 0u, 4u, 0x00800080u, 0u, 0u, 8u, 3u, 24u, 5u},
    };
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i++) {
        CHECK(rt_begin(&config));
        map_surface_memory();
        fill_bytes(a, 512u);
        const uint32_t from = a + cases[i].source * 0x10000u;
        const uint32_t to = a + cases[i].destination * 0x10000u;
        stats = blit_present(from, to, cases[i].format, cases[i].pitches, cases[i].in_x, cases[i].in_y, cases[i].out_x, cases[i].out_y, cases[i].width,
                             cases[i].height);
        if (stats.latched) {
            printf("  byte case %zu: %s\n", i, stats.error);
        }
        CHECK(!stats.latched && stats.byte_copies_applied == 1u && stats.copies_applied == 1u);
        static uint8_t memory[4096];
        memset(memory, 0, sizeof memory);
        for (uint32_t index = 0u; index < 512u; index++) {
            memory[index] = byte_pattern(index);
        }
        gpu_pgraph_copy copy;
        memset(&copy, 0, sizeof copy);
        copy.color_format = cases[i].format;
        copy.source_pitch = cases[i].pitches & 0xFFFFu;
        copy.destination_pitch = cases[i].pitches >> 16;
        copy.in_x = cases[i].in_x;
        copy.in_y = cases[i].in_y;
        copy.out_x = cases[i].out_x;
        copy.out_y = cases[i].out_y;
        copy.width = cases[i].width;
        copy.height = cases[i].height;
        uint8_t *destination_memory = memory + cases[i].destination * 2048u; /* the destination surface is its own memory unless it is the source */
        if (cases[i].destination == cases[i].source) {
            destination_memory = memory;
        }
        reference_byte_blit(&copy, memory, destination_memory);
        const size_t held = d3d8_surface_model_byte_surface_length(to);
        CHECK(held > 0u && held <= sizeof model && read_model(to, model, held));
        CHECK(held > 0u && held <= sizeof model && memcmp(model, destination_memory, held) == 0);
        rt_end();
    }

    /* a Data the replay holds as an A8R8G8B8 image: a pass of this frame, then a kept image of an earlier frame, as the source and as the destination */
    for (int kept = 0; kept < 2; kept++) {
        for (int side = 0; side < 2; side++) {
            CHECK(rt_begin(&config));
            map_surface_memory();
            fill_bytes(a, 512u);
            rt_producer_pass(RT_PRODUCER);
            if (kept != 0) {
                CHECK(rt_present().frames_replayed == 1u);
            }
            const uint32_t from = side == 0 ? RT_PRODUCER_DATA : a;
            const uint32_t to = side == 0 ? a : RT_PRODUCER_DATA;
            push_blit_packet(from, to, 1u, 0x00400040u, 0u, 0u, 8u | (4u << 16));
            d3d8_gpu_kick();
            stats = rt_present();
            char words[160];
            snprintf(words, sizeof words, "%s surface Data 0x0500000 is held by the replay as an A8R8G8B8 image", side == 0 ? "source" : "destination");
            expect_surface_refusal(&stats, words, "a byte path blit (colour format 0x1)");
            CHECK(stats.byte_copies_applied == 0u);
            rt_end();
        }
    }

    /* a texture over a byte surface a blit wrote: the bytes hold no image */
    CHECK(rt_begin(&config));
    map_surface_memory();
    fill_bytes(a, 512u);
    stats = blit_present(a, b, 1u, 0x00400040u, 2u, 1u, 5u, 3u, 8u, 4u);
    CHECK(!stats.latched && stats.byte_copies_applied == 1u);
    rt_second_header(b, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    expect_surface_refusal(&stats, "byte surface", "Data 0x0A30000 overlaps a guest byte surface a CopyRects byte blit wrote");
    rt_end();

    /* the BLIT model inference withdrawn: the byte copy is refused by its OWN reason, named (not only that it was refused), and nothing moves */
    config = surface_config(selector, true, false);
    config.allowed_inferences &= ~GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL;
    CHECK(rt_begin(&config));
    map_surface_memory();
    fill_bytes(a, 512u);
    stats = blit_present(a, b, 1u, 0x00400040u, 2u, 1u, 5u, 3u, 8u, 4u);
    expect_surface_refusal(&stats, "INFERRED and not allowed", "a byte path blit (colour format 0x1");
    CHECK(stats.byte_copies_applied == 0u);
    rt_end();

    /* the option off: the byte formats stay refused as before, on a surface the replay holds */
    for (uint32_t format = 1u; format <= 4u; format += 3u) {
        config = surface_config(selector, false, false);
        CHECK(rt_begin(&config));
        map_surface_memory();
        rt_producer_pass(RT_PRODUCER);
        push_blit_packet(RT_PRODUCER_DATA, RT_PRODUCER_DATA, format, PITCHES_512, 0u, 0u, 64u | (32u << 16));
        d3d8_gpu_kick();
        stats = rt_present();
        expect_surface_refusal(&stats, "A8R8G8B8 images", NULL);
        CHECK(stats.byte_copies_applied == 0u && d3d8_surface_model_byte_surface_count() == 0u);
        rt_end();
    }
}

/* --- the edges of the surface model (T633, T596): what each refusal and bound is exact about ------------------------------------------------------- */

/* A blit of the frame before the draw that samples the texture (a guest memory texture at TEXTURE_DATA, `pitches` the source and destination pitch). */
static d3d8_swap_replay_stats blit_then_sample(const char *selector, uint32_t destination, uint32_t pitches, uint32_t to, uint32_t size, bool second_blit,
                                               uint32_t second_destination)
{
    d3d8_swap_replay_config config = surface_config(selector, true, false);
    CHECK(rt_begin(&config));
    map_surface_memory();
    fill_gradient(BLIT_MAP_BASE, 512u, 128u, 64u);
    rt_second_header(TEXTURE_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    push_blit_packet(BLIT_MAP_BASE, destination, 0xAu, pitches, 0u, to, size);
    if (second_blit) {
        push_blit_packet(BLIT_MAP_BASE, second_destination, 0xAu, pitches, 0u, to, size);
    }
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    return rt_present();
}

/* What `blit_written_before` takes for a write of the texture: the destination's extent with the destination's pitch and the rectangle's own corner,
 * a write that is empty is none, the newest blit is the one named, and a blit in the middle of a pass image counts. */
static void test_surface_blit_before_edges(const char *selector)
{
    printf("test_surface_blit_before_edges (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    d3d8_swap_replay_stats stats;

    /* an empty blit (width 0) with its destination inside the texture writes nothing: the draw samples the guest zero */
    stats = blit_then_sample(selector, TEXTURE_DATA + 0x100u, PITCHES_512, 0u, 0u | (8u << 16), false, 0u);
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.copies_empty == 1u && stats.surface_guest_zero_samples == 1u);
    rt_end();

    /* two blits write the texture before the draw: the refusal names the newest */
    stats = blit_then_sample(selector, TEXTURE_DATA, PITCHES_512, 0u, 16u | (8u << 16), true, TEXTURE_DATA + 0x400u);
    expect_surface_refusal(&stats, "before draw 0 samples it", "(to 0x0700400)");
    rt_end();

    /* the extent is sized with the DESTINATION pitch (256, the source's is 512): 7 rows of 256 and 16 pixels end where the texture starts */
    const uint32_t pitches = 0x01000200u; /* source 512, destination 256 */
    const uint32_t extent = 7u * 256u + 16u * 4u;
    stats = blit_then_sample(selector, TEXTURE_DATA - extent, pitches, 0u, 16u | (8u << 16), false, 0u);
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.copies_applied == 1u && stats.surface_guest_zero_samples == 1u);
    rt_end();
    stats = blit_then_sample(selector, TEXTURE_DATA - extent + 1u, pitches, 0u, 16u | (8u << 16), false, 0u);
    expect_surface_refusal(&stats, "before draw 0 samples it", "Data 0x0700000");
    rt_end();

    /* the extent starts at the DESTINATION corner (4, 2): 9 rows of 512 and 4 + 16 pixels */
    const uint32_t corner_extent = 9u * 512u + 4u * 4u + 16u * 4u;
    stats = blit_then_sample(selector, TEXTURE_DATA - corner_extent, PITCHES_512, 4u | (2u << 16), 16u | (8u << 16), false, 0u);
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.copies_applied == 1u && stats.surface_guest_zero_samples == 1u);
    rt_end();
    stats = blit_then_sample(selector, TEXTURE_DATA - corner_extent + 1u, PITCHES_512, 4u | (2u << 16), 16u | (8u << 16), false, 0u);
    expect_surface_refusal(&stats, "before draw 0 samples it", "Data 0x0700000");
    rt_end();

    /* a blit that READS the texture writes nothing of it (its destination is elsewhere) */
    d3d8_swap_replay_config config = surface_config(selector, true, false);
    CHECK(rt_begin(&config));
    map_surface_memory();
    rt_second_header(TEXTURE_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    push_blit_packet(TEXTURE_DATA, BLIT_MAP_BASE, 0xAu, PITCHES_512, 0u, 0u, 16u | (8u << 16));
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.copies_applied == 1u && stats.surface_guest_zero_samples == 1u);
    rt_end();

    /* a pass image sampled after a blit that writes the middle of its range, not its first byte */
    d3d8_swap_replay_stats middle;
    blit_before_scene(selector, RT_PRODUCER_DATA + 0x2000u, true, true, true, "samples the pass image of render target", &middle);
    rt_end();
}

/* The limits of a surface built from guest memory (pitch, rows), the growth of a built surface to the rows a later blit reaches, a growth that is not
 * a pass image's, a built surface over a byte surface, and the inference the built side puts on the frame. */
static void test_surface_blit_guest_edges(const char *selector)
{
    printf("test_surface_blit_guest_edges (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    d3d8_swap_replay_stats stats;
    d3d8_swap_replay_config config = surface_config(selector, true, false);
    char text[2400];

    /* a pitch that is not a whole number of pixels, or beyond 8192 pixels, is no surface the model can read (named, not a failed read). The decoder
     * refuses a pitch of 0 and a row wider than the pitch before the replay sees the blit. */
    static const struct {
        uint32_t pitches;
        uint32_t width;
        const char *words;
    } pitch_cases[] = {
        {0x01008004u, 4u, "has a pitch of 32772 bytes over 2 rows"},
    };
    for (size_t i = 0u; i < sizeof pitch_cases / sizeof pitch_cases[0]; i++) {
        CHECK(rt_begin(&config));
        map_surface_memory();
        stats = blit_present(BLIT_SOURCE, BLIT_DESTINATION, 0xAu, pitch_cases[i].pitches, 0u, 0u, 0u, 0u, pitch_cases[i].width, 2u);
        expect_surface_refusal(&stats, pitch_cases[i].words, "the guest model can read");
        CHECK(stats.blit_guest_surfaces == 0u && stats.copies_applied == 0u);
        rt_end();
    }
    /* the bound itself: a pitch of 8192 pixels (32768 bytes) is read, two rows of it */
    CHECK(rt_begin(&config));
    map_surface_memory();
    fill_gradient(BLIT_SOURCE, 0x8000u, 4u, 2u);
    stats = blit_present(BLIT_SOURCE, BLIT_DESTINATION, 0xAu, 0x01008000u, 0u, 0u, 0u, 0u, 4u, 2u);
    CHECK(!stats.latched && stats.copies_applied == 1u && stats.blit_guest_surfaces == 2u);
    const gpu_image *wide = d3d8_swap_replay_surface_image(BLIT_SOURCE);
    CHECK(wide != NULL && wide->pixels != NULL && wide->width == 8192u && wide->height == 2u && pixel_is_gradient(wide, 3u, 1u, 3u, 1u));
    rt_end();
    /* rows: 8192 are read (the last row of the rectangle is row 8191), 8193 are refused, for the source and for the destination */
    static const struct {
        bool source;
        uint32_t row;
        const char *words;
    } row_cases[] = {
        {true, 8191u, NULL},
        {true, 8192u, "over 8193 rows"},
        {false, 8191u, NULL},
        {false, 8192u, "over 8193 rows"},
    };
    for (size_t i = 0u; i < sizeof row_cases / sizeof row_cases[0]; i++) {
        CHECK(rt_begin(&config));
        map_surface_memory();
        const uint32_t source = BLIT_MAP_BASE;
        const uint32_t destination = BLIT_MAP_BASE + (row_cases[i].source ? 0x90000u : 0x40000u);
        const uint32_t in_y = row_cases[i].source ? row_cases[i].row : 0u;
        const uint32_t out_y = row_cases[i].source ? 0u : row_cases[i].row;
        stats = blit_present(source, destination, 0xAu, 0x00400040u, 0u, in_y, 0u, out_y, 16u, 1u);
        if (row_cases[i].words == NULL) {
            if (stats.latched) {
                printf("  rows: %s\n", stats.error);
            }
            CHECK(!stats.latched && stats.copies_applied == 1u && stats.blit_guest_surfaces == 2u);
            const gpu_image *tall = d3d8_swap_replay_surface_image(row_cases[i].source ? source : destination);
            CHECK(tall != NULL && tall->height == 8192u && tall->width == 16u);
        } else {
            expect_surface_refusal(&stats, row_cases[i].words, "the guest model can read");
            CHECK(stats.copies_applied == 0u);
        }
        rt_end();
    }

    /* a second blit that reaches further down (a small rectangle at a deep row) grows the surfaces to the row its last row reaches, from the guest bytes */
    CHECK(rt_begin(&config));
    map_surface_memory();
    fill_gradient(BLIT_SOURCE, 256u, 64u, 64u);
    stats = blit_present(BLIT_SOURCE, BLIT_DESTINATION, 0xAu, PITCHES_256, 0u, 0u, 0u, 0u, 16u, 8u);
    CHECK(!stats.latched && stats.copies_applied == 1u && stats.blit_guest_surfaces == 2u);
    CHECK(d3d8_swap_replay_surface_summary(text, sizeof text) > 0u &&
          strstr(text, "2 blit surface(s) built from guest memory, 0 byte path blit(s)") != NULL);
    stats = blit_present(BLIT_SOURCE, BLIT_DESTINATION, 0xAu, PITCHES_256, 0u, 20u, 0u, 16u, 16u, 4u);
    if (stats.latched) {
        printf("  growth: %s\n", stats.error);
    }
    CHECK(!stats.latched && stats.copies_applied == 2u && stats.blit_guest_surfaces == 2u);
    const gpu_image *grown_source = d3d8_swap_replay_surface_image(BLIT_SOURCE);
    const gpu_image *grown_destination = d3d8_swap_replay_surface_image(BLIT_DESTINATION);
    CHECK(grown_source != NULL && grown_destination != NULL && grown_source->pixels != NULL && grown_destination->pixels != NULL);
    if (grown_source != NULL && grown_destination != NULL && grown_source->pixels != NULL && grown_destination->pixels != NULL) {
        CHECK(grown_source->height == 24u && grown_destination->height == 20u);
        CHECK(pixel_is_gradient(grown_source, 5u, 22u, 5u, 22u) && pixel_is_gradient(grown_source, 63u, 23u, 63u, 23u));
        uint32_t wrong = 0u;
        for (uint32_t y = 0u; y < 4u; y++) {
            for (uint32_t x = 0u; x < 16u; x++) {
                wrong += !pixel_is_gradient(grown_destination, x, 16u + y, x, 20u + y);
            }
        }
        CHECK(wrong == 0u);
        CHECK(pixel_is_gradient(grown_destination, 3u, 5u, 3u, 5u)); /* the rows the first blit copied are still there */
    }
    rt_end();

    /* a blit that reaches past a pass image is clamped (refused), the image is not grown from the guest memory under it */
    CHECK(rt_begin(&config));
    map_surface_memory();
    rt_producer_pass(RT_PRODUCER);
    CHECK(rt_present().frames_replayed == 1u);
    stats = blit_present(RT_PRODUCER_DATA, BLIT_DESTINATION, 0xAu, PITCHES_512, 0u, 60u, 0u, 0u, 16u, 10u);
    CHECK(stats.latched && stats.copies_applied == 0u);
    const gpu_image *producer = d3d8_swap_replay_surface_image(RT_PRODUCER_DATA);
    CHECK(producer != NULL && producer->height == RT_EDGE_HEIGHT);
    rt_end();

    const uint32_t a = BLIT_MAP_BASE + 0x20000u;
    const uint32_t b = BLIT_MAP_BASE + 0x30000u;
    /* a texture that PARTLY overlaps a byte surface a byte path blit wrote (its range starts below the byte surface) is refused by name too */
    CHECK(rt_begin(&config));
    map_surface_memory();
    fill_bytes(a, 512u);
    stats = blit_present(a, b, 1u, 0x00400040u, 2u, 1u, 5u, 3u, 8u, 4u);
    CHECK(!stats.latched && stats.byte_copies_applied == 1u);
    rt_second_header(b - 0x100u, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    expect_surface_refusal(&stats, "byte surface", "Data 0x0A2FF00 overlaps a guest byte surface");
    rt_end();

    /* an image blit over a range that PARTLY overlaps a byte surface a byte path blit wrote is refused, not built from the guest */
    CHECK(rt_begin(&config));
    map_surface_memory();
    fill_bytes(a, 512u);
    stats = blit_present(a, b, 1u, 0x00400040u, 2u, 1u, 5u, 3u, 8u, 4u);
    CHECK(!stats.latched && stats.byte_copies_applied == 1u);
    CHECK(d3d8_swap_replay_surface_summary(text, sizeof text) > 0u &&
          strstr(text, "0 blit surface(s) built from guest memory, 1 byte path blit(s)") != NULL);
    stats = blit_present(b - 0x40u, BLIT_DESTINATION, 0xAu, PITCHES_256, 0u, 0u, 0u, 0u, 16u, 8u);
    expect_surface_refusal(&stats, "is held as bytes by an earlier byte path blit", "Data 0x0A2FFC0");
    rt_end();

    /* byte path blits: an unreadable source is named by the model's status, an empty blit is counted and moves nothing, and one surface with the
     * source further down than the destination holds the source's extent */
    CHECK(rt_begin(&config));
    map_surface_memory();
    stats = blit_present(UNMAPPED_DATA, b, 1u, 0x00400040u, 0u, 0u, 0u, 0u, 8u, 4u);
    expect_surface_refusal(&stats, "the guest memory range cannot be read: byte path", "Data 0x0C00000 to 0x0A30000");
    rt_end();
    CHECK(rt_begin(&config));
    map_surface_memory();
    fill_bytes(a, 512u);
    stats = blit_present(a, b, 1u, 0x00400040u, 0u, 0u, 0u, 0u, 0u, 4u);
    CHECK(!stats.latched && stats.copies_empty == 1u && stats.copies_applied == 0u && stats.byte_copies_applied == 0u && d3d8_surface_model_byte_surface_count() == 0u);
    stats = blit_present(a, a, 1u, 0x00400040u, 0u, 4u, 16u, 0u, 8u, 4u); /* source rows 4 to 7, destination rows 0 to 3 further right */
    if (stats.latched) {
        printf("  one surface: %s\n", stats.error);
    }
    CHECK(!stats.latched && stats.byte_copies_applied == 1u);
    uint8_t model[512];
    CHECK(read_model(a, model, 512u));
    uint32_t wrong = 0u;
    for (uint32_t row = 0u; row < 4u; row++) {
        for (uint32_t column = 0u; column < 8u; column++) {
            wrong += model[row * 64u + 16u + column] != byte_pattern((4u + row) * 64u + column);
        }
    }
    CHECK(wrong == 0u && d3d8_surface_model_byte_surface_length(a) == (4u + 4u - 1u) * 64u + 8u);
    rt_end();

    /* the SURFACE_SOURCE inference is on the frame for a built side, whichever side it is: a built source onto a pass image, a pass image onto a built destination */
    for (int built_source = 1; built_source >= 0; built_source--) {
        CHECK(rt_begin(&config));
        map_surface_memory();
        fill_gradient(BLIT_MAP_BASE, 512u, 128u, 64u);
        rt_producer_pass(RT_PRODUCER);
        push_blit_packet(built_source != 0 ? BLIT_MAP_BASE : RT_PRODUCER_DATA, built_source != 0 ? RT_PRODUCER_DATA : BLIT_DESTINATION, 0xAu, PITCHES_512,
                         0u, 0u, 16u | (8u << 16));
        stats = rt_present();
        if (stats.latched) {
            printf("  inference: %s\n", stats.error);
        }
        CHECK(!stats.latched && stats.copies_applied == 1u && stats.blit_guest_surfaces == 1u);
        CHECK((stats.used_inferences & D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE) != 0u);
        rt_end();
    }
}

/* Sixteen kept surfaces and a blit whose source is the oldest of them: its destination is built in the NEXT slot, never in the source's, and the claim
 * after it takes the one after that (the oldest is evicted, each is counted once). */
static void test_surface_blit_protect(const char *selector)
{
    printf("test_surface_blit_protect (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    d3d8_swap_replay_config config = surface_config(selector, true, false);
    CHECK(rt_begin(&config));
    map_surface_memory();
    CHECK(D3D8_SWAP_REPLAY_SURFACES == 16u);
    d3d8_swap_replay_stats stats;
    for (uint32_t pair = 0u; pair < 8u; pair++) {
        const uint32_t source = BLIT_MAP_BASE + (2u * pair) * 0x10000u;
        fill_gradient(source, 256u, 4u, 2u);
        stats = blit_present(source, source + 0x10000u, 0xAu, PITCHES_256, 0u, 0u, 0u, 0u, 4u, 2u);
        CHECK(!stats.latched && stats.copies_applied == pair + 1u && stats.blit_guest_surfaces == 2u * (pair + 1u));
    }
    CHECK(stats.surfaces_evicted == 0u);
    const uint32_t oldest = BLIT_MAP_BASE;
    const uint32_t second = BLIT_MAP_BASE + 0x10000u;
    const uint32_t third = BLIT_MAP_BASE + 0x20000u;
    const uint32_t first_new = RT_CONSUMER_DATA;
    const uint32_t second_new = RT_CONSUMER_DATA + 0x10000u;
    stats = blit_present(oldest, first_new, 0xAu, PITCHES_256, 0u, 0u, 0u, 0u, 4u, 2u);
    if (stats.latched) {
        printf("  protect: %s\n", stats.error);
    }
    CHECK(!stats.latched && stats.surfaces_evicted == 1u);
    CHECK(d3d8_swap_replay_surface_image(oldest) != NULL);   /* the source of the blit stays */
    CHECK(d3d8_swap_replay_surface_image(second) == NULL);   /* the next one went */
    CHECK(d3d8_swap_replay_surface_image(third) != NULL);
    const gpu_image *built = d3d8_swap_replay_surface_image(first_new);
    CHECK(built != NULL && built->pixels != NULL && pixel_is_gradient(built, 3u, 1u, 3u, 1u) && pixel_is_gradient(built, 0u, 0u, 0u, 0u));
    stats = blit_present(oldest, second_new, 0xAu, PITCHES_256, 0u, 0u, 0u, 0u, 4u, 2u);
    CHECK(!stats.latched && stats.surfaces_evicted == 2u);
    CHECK(d3d8_swap_replay_surface_image(oldest) != NULL && d3d8_swap_replay_surface_image(first_new) != NULL); /* what the last claim built is not the next one out */
    CHECK(d3d8_swap_replay_surface_image(third) == NULL);
    CHECK(d3d8_swap_replay_surface_image(second_new) != NULL);
    rt_end();
}

/* Both options with the target header unmeasured in frame 2 (its Size and Format words cleared by the title) and an explicit size: the kept image's
 * pitch and Format are not compared with what is not measured. */
static void test_surface_persist_unmeasured(const char *selector)
{
    printf("test_surface_persist_unmeasured (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    d3d8_swap_replay_config config = surface_config(selector, false, true);
    config.width = RT_EDGE_WIDTH;
    config.height = RT_EDGE_HEIGHT;
    CHECK(rt_begin(&config));
    map_surface_memory();
    rt_producer_pass(RT_PRODUCER);
    d3d8_swap_replay_stats stats = rt_present();
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.targets_persisted == 0u);
    store(RT_PRODUCER + D3D8_SURFACE_SIZE, 0u);
    store(RT_PRODUCER + D3D8_SURFACE_FORMAT, 0u);
    rt_yellow_pass(RT_PRODUCER, false);
    stats = rt_present();
    if (stats.latched) {
        printf("  unmeasured: %s\n", stats.error);
    }
    CHECK(!stats.latched && stats.frames_replayed == 2u && stats.targets_persisted == 1u);
    rt_end();
}

/* A slot the eviction took is a new surface: nothing of the pass image that was there is left (its Format is not the built surface's). Sixteen passes keep
 * sixteen images, the first with another Format; a blit builds a surface in its slot; the next frame samples that surface and it is exact. */
static void test_surface_slot_reuse(const char *selector)
{
    printf("test_surface_slot_reuse (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    d3d8_swap_replay_config config = surface_config(selector, true, false);
    CHECK(rt_begin(&config));
    map_surface_memory();
    rt_second_header(BLIT_SOURCE, RT_EDGE_WIDTH, RT_EDGE_HEIGHT); /* cloned from the back buffer before any header's Format is changed */
    d3d8_swap_replay_stats stats;
    for (uint32_t i = 0u; i < D3D8_SWAP_REPLAY_SURFACES; i++) {
        const uint32_t header = HEADER_SCRATCH + 0x200u + i * 0x40u;
        make_rt_target(header, 0x00800000u + i * 0x10000u, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
        if (i == 0u) {
            store(header + D3D8_SURFACE_FORMAT, 0x00011228u); /* the oldest image, the one the first eviction takes, has another Format */
        }
        rt_producer_pass(header);
        stats = rt_present();
        CHECK(!stats.latched && stats.frames_replayed == i + 1u);
    }
    CHECK(stats.surfaces_evicted == 0u && d3d8_swap_replay_surface_image(0x00800000u) != NULL);
    /* the source (128 texels of 512 bytes, 64 rows reached by the last 16) is built in the oldest slot */
    stats = blit_present(BLIT_SOURCE, BLIT_DESTINATION, 0xAu, PITCHES_512, 0u, 48u, 0u, 0u, 16u, 16u);
    if (stats.latched) {
        printf("  slot reuse blit: %s\n", stats.error);
    }
    CHECK(!stats.latched && stats.surfaces_evicted == 2u && stats.blit_guest_surfaces == 2u);
    CHECK(d3d8_swap_replay_surface_image(0x00800000u) == NULL && d3d8_swap_replay_surface_image(0x00810000u) == NULL);
    const gpu_image *built = d3d8_swap_replay_surface_image(BLIT_SOURCE);
    CHECK(built != NULL && built->width == 128u && built->height == 64u);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    if (stats.latched) {
        printf("  slot reuse sample: %s\n", stats.error);
    }
    CHECK(!stats.latched && stats.surface_kept_samples == 1u && stats.surface_guest_samples == 0u);
    rt_end();
}

/* The surface texture is sampled bilinear and in texels: a half texel right of the texel centres reads the mean of two texels (a kept image of an earlier
 * frame and the guest memory under a surface no pass drew). */
static void test_surface_filter(const char *selector)
{
    printf("test_surface_filter (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_half_words, sizeof draw_vertex_texel_half_words);
    d3d8_swap_replay_config config = surface_config(selector, true, false);
    CHECK(rt_begin(&config));
    map_surface_memory();
    rt_second_header(RT_PRODUCER_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    rt_producer_pass(RT_PRODUCER);
    d3d8_swap_replay_stats stats = rt_present();
    CHECK(!stats.latched && stats.frames_replayed == 1u);
    gpu_image first = copy_of(d3d8_swap_replay_last_frame());
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    CHECK(!stats.latched && stats.surface_kept_samples == 1u);
    const gpu_image *consumer = d3d8_swap_replay_last_frame();
    CHECK(consumer != NULL && consumer->pixels != NULL && first.pixels != NULL);
    if (consumer != NULL && consumer->pixels != NULL && first.pixels != NULL) {
        uint32_t mixed = 0u;
        for (uint32_t y = 0u; y < consumer->height; y++) {
            for (uint32_t x = 0u; x + 1u < consumer->width; x++) {
                const uint8_t *here = consumer->pixels + gpu_image_offset(consumer, x, y);
                const uint8_t *left = first.pixels + gpu_image_offset(&first, x, y);
                const uint8_t *right = first.pixels + gpu_image_offset(&first, x + 1u, y);
                if (left[0] == 0u && right[0] == 255u && left[1] == 0u && right[1] == 0u) { /* black next to red */
                    CHECK(here[0] >= 127u && here[0] <= 128u && here[1] == 0u && here[2] == 0u && here[3] == 255u);
                    mixed++;
                }
            }
        }
        CHECK(mixed > 5u);
    }
    free(first.pixels);
    rt_end();

    /* the guest memory: columns alternate between red 255 and red 0 (a texel's R is the guest dword's 0x00RR0000) */
    config = surface_config(selector, true, false);
    CHECK(rt_begin(&config));
    map_surface_memory();
    for (uint32_t y = 0u; y < RT_EDGE_HEIGHT; y++) {
        for (uint32_t x = 0u; x < RT_EDGE_WIDTH; x++) {
            store(TEXTURE_DATA + y * RT_EDGE_WIDTH * 4u + x * 4u, (x % 2u == 0u) ? 0xFFFF0000u : 0xFF000000u);
        }
    }
    rt_second_header(TEXTURE_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    CHECK(!stats.latched && stats.surface_guest_samples == 1u);
    consumer = d3d8_swap_replay_last_frame();
    CHECK(consumer != NULL && consumer->pixels != NULL);
    if (consumer != NULL && consumer->pixels != NULL) {
        const uint8_t *middle = consumer->pixels + gpu_image_offset(consumer, 10u, 10u);
        CHECK(middle[0] >= 127u && middle[0] <= 128u && middle[1] == 0u && middle[2] == 0u && middle[3] == 255u);
        const uint8_t *odd = consumer->pixels + gpu_image_offset(consumer, 11u, 10u);
        CHECK(odd[0] >= 127u && odd[0] <= 128u);
    }
    rt_end();
}

/* --- enable() refusals, the summary and the overlay note ------------------------------------------------------------------------------------------ */

/* No device: what enable refuses by name for the two options, the defaults, and the summary that is silent when nothing is on. */
static void test_surface_config(void)
{
    printf("test_surface_config\n");
    char error[400];
    char text[2400];
    CHECK(!d3d8_swap_replay_default_config().surface_source && !d3d8_swap_replay_default_config().target_persist);
    CHECK(!base_config(NULL).surface_source && !base_config(NULL).target_persist && !rt_config(NULL).surface_source && !rt_config(NULL).target_persist);
    CHECK((GPU_PGRAPH_INFER_ALL & (D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE | D3D8_SWAP_REPLAY_INFER_TARGET_PERSIST)) == 0u);
    CHECK((GPU_PGRAPH_INFER_OUTPUT_ALL & (D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE | D3D8_SWAP_REPLAY_INFER_TARGET_PERSIST)) == 0u);
    CHECK((d3d8_swap_replay_host_inferences(true, true, true, true, true) & (D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE | D3D8_SWAP_REPLAY_INFER_TARGET_PERSIST)) == 0u);
    CHECK(D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE != D3D8_SWAP_REPLAY_INFER_TARGET_PERSIST);
    CHECK(D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE == GPU_PGRAPH_INFER_OUTPUT_SURFACE_SOURCE && D3D8_SWAP_REPLAY_INFER_TARGET_PERSIST == GPU_PGRAPH_INFER_OUTPUT_TARGET_PERSIST);

    /* the summary is silent when neither option is on and no frame was under the overlay */
    d3d8_swap_replay_config config = rt_config(NULL);
    CHECK(d3d8_swap_replay_enable(&config, error, sizeof error));
    text[0] = 'x';
    CHECK(d3d8_swap_replay_surface_summary(text, sizeof text) == 0u && text[0] == '\0');
    CHECK(d3d8_swap_replay_surface_summary(NULL, sizeof text) == 0u && d3d8_swap_replay_surface_summary(text, 0u) == 0u);
    d3d8_swap_replay_disable();
    text[0] = 'x';
    CHECK(d3d8_swap_replay_surface_summary(text, sizeof text) == 0u && text[0] == '\0'); /* not enabled */

    /* each on its own: the summary names it, ON for its own and off for the other, every choice INFERRED and the colour key REFUSED by name */
    config = surface_config(NULL, true, false);
    CHECK(d3d8_swap_replay_enable(&config, error, sizeof error));
    CHECK(d3d8_swap_replay_get_config().surface_source && !d3d8_swap_replay_get_config().target_persist);
    CHECK(d3d8_swap_replay_surface_summary(text, sizeof text) > 0u);
    CHECK(strstr(text, "SURFACE MODEL (T633, T596)") != NULL && strstr(text, "surface source ON") != NULL && strstr(text, "target persistence off") != NULL);
    CHECK(strstr(text, "INFERRED") != NULL && strstr(text, "REFUSED by name") != NULL && strstr(text, "HQ60") != NULL);
    CHECK(strstr(text, "(0 sample(s)), else the guest memory under it read as A8R8G8B8 (0 sample(s), 0 all zero") != NULL);
    CHECK(strstr(text, "0 blit surface(s) built from guest memory, 0 byte path blit(s)") != NULL);
    CHECK(strstr(text, "0 replayed frame(s) were under the overlay, 0 with the key enabled") != NULL);
    CHECK(strstr(text, "pass the key DISABLED (MEASURED)") != NULL && strstr(text, "NOT composed over the replayed frames") != NULL);
    CHECK(strstr(text, "an update with the key ENABLED is REFUSED by name (measured by T770, not composed here, last key 0x") != NULL);
    CHECK(strstr(text, "all zero = transparent black, MEASURED bytes)") != NULL);
    CHECK(strstr(text, "and a blit before the draw are REFUSED by name. (2) target persistence") != NULL);
    char small[40];
    const size_t written = d3d8_swap_replay_surface_summary(small, sizeof small);
    CHECK(written == sizeof small - 1u && strlen(small) == written); /* truncated, NUL terminated */
    d3d8_swap_replay_disable();
    config = base_config(NULL);
    config.target_persist = true;
    config.allowed_inferences |= D3D8_SWAP_REPLAY_INFER_TARGET_PERSIST;
    CHECK(d3d8_swap_replay_enable(&config, error, sizeof error)); /* persistence needs no render target texture, no combiner and no blit group */
    CHECK(!d3d8_swap_replay_get_config().surface_source && d3d8_swap_replay_get_config().target_persist);
    CHECK(d3d8_swap_replay_surface_summary(text, sizeof text) > 0u && strstr(text, "surface source off") != NULL && strstr(text, "target persistence ON") != NULL);
    d3d8_swap_replay_disable();
    config = surface_config(NULL, true, true);
    CHECK(d3d8_swap_replay_enable(&config, error, sizeof error));
    CHECK(d3d8_swap_replay_get_config().surface_source && d3d8_swap_replay_get_config().target_persist);
    d3d8_swap_replay_disable();

    static const struct {
        const char *what;
        const char *expected;
    } refusals[] = {
        {"no render target texture", "needs the render target texture option"},
        {"no blit group", "needs the BLIT output group"},
        {"no source inference", "D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE"},
        {"persist with flip_y", "target persistence cannot be combined with flip_y"},
        {"no persist inference", "D3D8_SWAP_REPLAY_INFER_TARGET_PERSIST"},
    };
    for (size_t i = 0u; i < sizeof refusals / sizeof refusals[0]; i++) {
        config = surface_config(NULL, true, false);
        if (i == 0u) {
            config.render_target_texture = false;
        } else if (i == 1u) {
            config.output_groups &= ~GPU_PGRAPH_OUTPUT_BLIT;
        } else if (i == 2u) {
            config.allowed_inferences &= ~D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE;
        } else if (i == 3u) {
            config = base_config(NULL);
            config.target_persist = true;
            config.flip_y = true;
            config.allowed_inferences |= D3D8_SWAP_REPLAY_INFER_TARGET_PERSIST;
        } else {
            config = base_config(NULL);
            config.target_persist = true;
        }
        error[0] = '\0';
        CHECK(!d3d8_swap_replay_enable(&config, error, sizeof error));
        if (strstr(error, refusals[i].expected) == NULL) {
            printf("  %s: expected \"%s\" in: %s\n", refusals[i].what, refusals[i].expected, error);
        }
        CHECK(strstr(error, refusals[i].expected) != NULL);
        CHECK(!d3d8_swap_replay_enabled());
    }
    /* the surface source with the render target texture census is fine: the census classifies the same surfaces */
    config = surface_config(NULL, true, false);
    config.render_target_texture_census = true;
    CHECK(d3d8_swap_replay_enable(&config, error, sizeof error));
    d3d8_swap_replay_disable();
}

#define OVERLAY_SURFACE (ARENA + 0xA000u)
#define OVERLAY_SOURCE (OVERLAY_SURFACE + 0x40u)
#define OVERLAY_DESTINATION (OVERLAY_SURFACE + 0x60u)
#define OVERLAY_KEY 0x00FF00FFu
#define OVERLAY_NAMED_REFUSAL "REFUSED BY NAME (T633 (3), not a latch)"

static void overlay_rectangle(uint32_t address, uint32_t left, uint32_t top, uint32_t right, uint32_t bottom)
{
    store(address, left);
    store(address + 4u, top);
    store(address + 8u, right);
    store(address + 12u, bottom);
}

/* The 640x480 linear YUY2 header T250 measured (test_d3d8_overlay.c) and the two rectangles, after a clean overlay state. */
static void overlay_prepare(void)
{
    d3d8_overlay_reset();
    store(OVERLAY_SURFACE + 0x00u, 0x01050001u);
    store(OVERLAY_SURFACE + 0x04u, 0x81234560u);
    store(OVERLAY_SURFACE + 0x08u, 0u);
    store(OVERLAY_SURFACE + 0x0Cu, 0x00012429u);
    store(OVERLAY_SURFACE + 0x10u, 0x271DF27Fu);
    store(OVERLAY_SURFACE + 0x14u, 0u);
    overlay_rectangle(OVERLAY_SOURCE, 0u, 0u, 640u, 480u);
    overlay_rectangle(OVERLAY_DESTINATION, 0u, 0u, 640u, 480u);
}

/* One replayed, presented frame (the red triangle into the producer). */
static d3d8_swap_replay_stats overlay_frame(void)
{
    rt_red_pass(RT_PRODUCER);
    return rt_present();
}

/* T633 (3): a frame replayed while the overlay is on (an UpdateOverlay seen, EnableOverlay odd) is the framebuffer UNDER the overlay: counted (any replayed frame, the
 * composition's target is often not the bound one), and an update with the colour key ENABLED is counted again and named REFUSED once in the log. MEASURED: the title's
 * updates pass the key disabled, which is logged once as such. Nothing latches, and the replayed picture does not change. */
static void test_surface_overlay_note(const char *selector)
{
    printf("test_surface_overlay_note (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    d3d8_swap_replay_config config = surface_config(selector, false, false);
    CHECK(rt_begin(&config));
    overlay_prepare();
    const uint32_t back = d3d8_device_load32(DEV_RENDER_TARGET);
    char text[2400];
    d3d8_swap_replay_stats stats = overlay_frame(); /* no overlay call yet */
    CHECK(!stats.latched && stats.frames_replayed == 1u && stats.frames_under_overlay == 0u && stats.frames_under_color_key == 0u);
    gpu_image reference = copy_of(d3d8_swap_replay_last_frame());
    CHECK(reference.pixels != NULL && pixel_is(&reference, 32u, 12u, 255u, 0u, 0u, 255u));
    CHECK(d3d8_swap_replay_surface_summary(text, sizeof text) == 0u); /* nothing on and nothing under an overlay: silent */

    (void)d3d8_overlay_enable(); /* EnableOverlay(1) alone: no UpdateOverlay, nothing is shown */
    CHECK(d3d8_overlay_state().enables == 1u && d3d8_overlay_state().updates == 0u);
    stats = overlay_frame();
    CHECK(!stats.latched && stats.frames_replayed == 2u && stats.frames_under_overlay == 0u);

    (void)d3d8_overlay_update(OVERLAY_SURFACE, OVERLAY_SOURCE, OVERLAY_DESTINATION, 0u, 0u); /* on, the key disabled as the title passes it */
    CHECK(d3d8_overlay_state().updates == 1u && d3d8_overlay_state().color_key_enable == 0u);
    stats = overlay_frame();
    CHECK(!stats.latched && stats.frames_replayed == 3u && stats.frames_under_overlay == 1u && stats.frames_under_color_key == 0u);
    CHECK(captured_has("the overlay is on with its colour key DISABLED (MEASURED") && !captured_has(OVERLAY_NAMED_REFUSAL));
    stats = overlay_frame();
    CHECK(!stats.latched && stats.frames_under_overlay == 2u && stats.frames_under_color_key == 0u);
    CHECK(occurrences(captured, "colour key DISABLED (MEASURED") == 1u); /* named once */

    (void)d3d8_overlay_update(OVERLAY_SURFACE, OVERLAY_SOURCE, OVERLAY_DESTINATION, 1u, OVERLAY_KEY); /* the colour key enabled: measured by T770, not composed by this replay */
    stats = overlay_frame();
    CHECK(!stats.latched && stats.frames_replayed == 5u && stats.frames_under_overlay == 3u && stats.frames_under_color_key == 1u);
    CHECK(occurrences(captured, OVERLAY_NAMED_REFUSAL) == 1u && captured_has("key 0x00FF00FF") && captured_has("destination 0,0 to 640,480"));
    stats = overlay_frame();
    CHECK(!stats.latched && stats.frames_under_overlay == 4u && stats.frames_under_color_key == 2u);
    CHECK(occurrences(captured, OVERLAY_NAMED_REFUSAL) == 1u); /* named once */
    const gpu_image *frame = d3d8_swap_replay_last_frame();
    CHECK(images_equal(frame, &reference)); /* the frame is the framebuffer, not changed by the overlay */
    CHECK(d3d8_swap_replay_surface_summary(text, sizeof text) > 0u && strstr(text, "INFERRED") != NULL && strstr(text, "REFUSED by name") != NULL &&
          strstr(text, "HQ60") != NULL && strstr(text, "NOT composed over the replayed frames") != NULL);
    CHECK(strstr(text, "measured by T770, not composed here, last key 0x00FF00FF)") != NULL);
    CHECK(strstr(text, "4 replayed frame(s) were under the overlay, 2 with the key enabled") != NULL);
    CHECK(strstr(text, "surface source off") != NULL && strstr(text, "target persistence off") != NULL);

    /* EnableOverlay(0) at the end of a movie: the count of enables is even, the overlay is off, the frames are the display again */
    CHECK(d3d8_overlay_hardware_write(0x8700u, 0u));
    (void)d3d8_overlay_enable();
    CHECK(d3d8_overlay_state().enables == 2u && d3d8_overlay_state().updates == 2u);
    stats = overlay_frame();
    CHECK(!stats.latched && stats.frames_replayed == 7u && stats.frames_under_overlay == 4u && stats.frames_under_color_key == 2u);
    /* a frame with no draw replays nothing and is not counted, and the next odd call turns the overlay on again */
    stats = rt_present();
    CHECK(stats.frames_empty == 1u && stats.frames_under_overlay == 4u);
    (void)d3d8_overlay_enable();
    CHECK(d3d8_overlay_state().enables == 3u);
    stats = rt_present();
    CHECK(stats.frames_empty == 2u && stats.frames_under_overlay == 4u);
    stats = overlay_frame();
    CHECK(!stats.latched && stats.frames_replayed == 8u && stats.frames_under_overlay == 5u && stats.frames_under_color_key == 3u);
    /* a frame that drew only into a target other than the bound one (frames_offscreen_only) is a replayed frame too */
    rt_red_pass(RT_PRODUCER);
    bind_target(back);
    stats = rt_present();
    CHECK(!stats.latched && stats.frames_offscreen_only == 1u && stats.frames_replayed == 8u && stats.frames_under_overlay == 6u && stats.frames_under_color_key == 4u);
    free(reference.pixels);
    d3d8_overlay_reset();
    rt_end();
}


/* --- T736 xemu oracle: the surface semantics claude-6 measured in xemu (docs/t736-xemu-surfaces.md "Results: surface sampling and surfaces no pass drew",
 * tests/fixtures/t736_surface_probe). The geometry is the probe's: a 128x64 surface, a red region of exactly 2048 pixels, a blue clear. xemu-level
 * evidence, never NV2A silicon. --------------------------------------------------------------------------------------------------------------------- */

#define ORACLE_RECT_FIRST_VERTEX 9u     /* 6 vertices: the 64 x 32 pixel rectangle of the upper left quarter (clip -1..0), 2048 pixels */
#define ORACLE_GRADIENT_FIRST_VERTEX 15u /* 3 vertices over the whole target, red, green and blue corners */
#define ORACLE_PIXELS (RT_EDGE_WIDTH * RT_EDGE_HEIGHT) /* 8192 */

static void write_oracle_vertices(void)
{
    static const float corners[9][2] = {{-1.0f, -1.0f}, {0.0f, -1.0f}, {0.0f, 0.0f}, {-1.0f, -1.0f}, {0.0f, 0.0f}, {-1.0f, 0.0f},
                                        {-1.0f, -1.0f}, {3.0f, -1.0f}, {-1.0f, 3.0f}};
    static const float colours[3][4] = {{0.0f, 0.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}}; /* v2.zyxw: red, green, blue */
    for (uint32_t i = 0u; i < 9u; i++) {
        const float *colour = i >= 6u ? colours[i - 6u] : colours[0];
        const float_vertex vertex = {{corners[i][0], corners[i][1], 0.5f}, {colour[0], colour[1], colour[2], colour[3]}};
        uint32_t words[7];
        memcpy(words, &vertex, sizeof words);
        for (uint32_t word = 0u; word < 7u; word++) {
            store(MEMORY_BASE + (ORACLE_RECT_FIRST_VERTEX + i) * (uint32_t)sizeof vertex + word * 4u, words[word]);
        }
    }
}

/* One pass: `count` vertices from `first` through the pass-through combiner (red unless the vertices carry colours), then optionally a full target clear to blue. */
static void rt_vertices_pass(uint32_t header, uint32_t first, uint32_t count, bool clear_blue_after)
{
    static const float offset[4] = {320.0f, 240.0f, 0.0f, 0.0f};
    static const float scale[4] = {320.0f, -240.0f, 1.0f, 0.0f};
    static const float c3_zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    bind_target(header);
    uint32_t words[GPU_PGRAPH_COMBINER_WORDS];
    build_combiner_words(pass_entries, sizeof pass_entries / sizeof pass_entries[0], words);
    stream_builder stream = {0};
    stream_pair(&stream, 0x030Cu, 0u);
    stream_pair(&stream, 0x032Cu, 0u);
    stream_pixel_shader(&stream, words);
    stream_viewport(&stream, offset, scale);
    stream_pair(&stream, GPU_PGRAPH_EXECUTION_MODE, 6u);
    stream_program(&stream, 0u, synthetic_program, 1u);
    stream_pair(&stream, GPU_PGRAPH_PROGRAM_START, 0u);
    stream_constants(&stream, 3u, c3_zero, 4u);
    stream_array(&stream, 1u, MEMORY_BASE, array_format(28u, 3u, GPU_PGRAPH_TYPE_F));
    stream_array(&stream, 2u, MEMORY_BASE + 12u, array_format(28u, 4u, GPU_PGRAPH_TYPE_F));
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, first, count);
    if (clear_blue_after) {
        stream_pair(&stream, 0x1D98u, 0u | ((RT_EDGE_WIDTH - 1u) << 16));
        stream_pair(&stream, 0x1D9Cu, 0u | ((RT_EDGE_HEIGHT - 1u) << 16));
        stream_pair(&stream, 0x1D8Cu, 0u);
        stream_pair(&stream, 0x1D90u, 0xFF0000FFu);
        stream_pair(&stream, GPU_PGRAPH_CLEAR_SURFACE, 0xF0u);
    }
    push_pending(&stream);
    stream_free(&stream);
}

/* xemu: a pass in frame 1 without a clear, on a target cleared in frame 0, PERSISTS: 6144 blue and 2048 red pixels (the clear colour stays, the new region is
 * added). With persistence off the replay's pass is a fresh image: the clear colour (black) and the red region only, no blue of frame 0. */
static void test_surface_xemu_persistence(const char *selector)
{
    printf("test_surface_xemu_persistence (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    for (int persist = 1; persist >= 0; persist--) {
        d3d8_swap_replay_config config = surface_config(selector, false, persist != 0);
        CHECK(rt_begin(&config));
        write_oracle_vertices();
        map_surface_memory();
        rt_vertices_pass(RT_PRODUCER, 6u, 3u, true); /* frame 0: drawn, then cleared blue over the whole target */
        d3d8_swap_replay_stats stats = rt_present();
        const gpu_image *frame = d3d8_swap_replay_last_frame();
        CHECK(!stats.latched && stats.frames_replayed == 1u && frame != NULL && frame->pixels != NULL);
        if (frame != NULL && frame->pixels != NULL) {
            CHECK(ORACLE_PIXELS - count_not(frame, 0u, 0u, 255u, 255u) == ORACLE_PIXELS); /* 8192 of 8192 blue */
        }
        rt_vertices_pass(RT_PRODUCER, ORACLE_RECT_FIRST_VERTEX, 6u, false); /* frame 1: the red region, no clear */
        stats = rt_present();
        frame = d3d8_swap_replay_last_frame();
        if (stats.latched) {
            printf("  frame 1: %s\n", stats.error);
        }
        CHECK(!stats.latched && stats.frames_replayed == 2u && frame != NULL && frame->pixels != NULL);
        if (frame != NULL && frame->pixels != NULL) {
            const uint32_t red = ORACLE_PIXELS - count_not(frame, 255u, 0u, 0u, 255u);
            const uint32_t blue = ORACLE_PIXELS - count_not(frame, 0u, 0u, 255u, 255u);
            const uint32_t black = ORACLE_PIXELS - count_not(frame, 0u, 0u, 0u, 255u);
            CHECK(red == 2048u); /* the region is exactly 64 x 32 pixels */
            CHECK(pixel_is(frame, 10u, 10u, 255u, 0u, 0u, 255u) && pixel_is(frame, 63u, 31u, 255u, 0u, 0u, 255u));
            CHECK(pixel_is(frame, 64u, 10u, persist != 0 ? 0u : 0u, 0u, persist != 0 ? 255u : 0u, 255u) && pixel_is(frame, 10u, 32u, 0u, 0u, persist != 0 ? 255u : 0u, 255u));
            if (persist != 0) {
                CHECK(blue == 6144u && black == 0u); /* xemu: 6144 blue plus 2048 red */
                surface_look("xemu_persistence_frame1", frame);
            } else {
                CHECK(blue == 0u && black == 6144u); /* a fresh image: the clear colour and the red region only */
            }
        }
        rt_end();
    }
}

/* xemu: a surface a pass drew in frame 0, sampled in frame 3 with no pass of it (frames 1 and 2 draw elsewhere) is the kept image, exact (error 0.0). The
 * picture is a gradient (red, green and blue corners), so a shifted, swapped or re-rendered copy cannot equal it. */
static void test_surface_xemu_kept_gradient(const char *selector)
{
    printf("test_surface_xemu_kept_gradient (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    d3d8_swap_replay_config config = surface_config(selector, true, true);
    CHECK(rt_begin(&config));
    write_oracle_vertices();
    map_surface_memory();
    rt_vertices_pass(RT_PRODUCER, ORACLE_GRADIENT_FIRST_VERTEX, 3u, false); /* frame 0 */
    d3d8_swap_replay_stats stats = rt_present();
    CHECK(!stats.latched && stats.frames_replayed == 1u);
    gpu_image frame0 = copy_of(d3d8_swap_replay_last_frame());
    CHECK(frame0.pixels != NULL);
    if (frame0.pixels != NULL) {
        CHECK(!pixel_is(&frame0, 10u, 10u, 0u, 0u, 0u, 255u) && memcmp(frame0.pixels + gpu_image_offset(&frame0, 10u, 10u),
                                                                       frame0.pixels + gpu_image_offset(&frame0, 100u, 50u), 4u) != 0);
        CHECK(count_not(&frame0, 0u, 0u, 0u, 255u) > ORACLE_PIXELS / 2u);
    }
    for (int frame = 1; frame <= 2; frame++) { /* frames 1 and 2: another surface is drawn, the producer is not */
        rt_red_pass(RT_CONSUMER);
        stats = rt_present();
        CHECK(!stats.latched && stats.frames_replayed == (uint64_t)frame + 1u);
    }
    d3d8_set_texture(0u, RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, UINT32_MAX, RT_CLAMP, RT_LINEAR); /* frame 3: samples it, no pass draws into it */
    stats = rt_present();
    if (stats.latched) {
        printf("  frame 3: %s\n", stats.error);
    }
    CHECK(!stats.latched && stats.frames_replayed == 4u && stats.surface_kept_samples == 1u && stats.surface_guest_samples == 0u);
    const gpu_image *frame3 = d3d8_swap_replay_last_frame();
    CHECK(frame3 != NULL && frame3->pixels != NULL && images_equal(frame3, &frame0)); /* error 0 */
    surface_look("xemu_kept_gradient_frame3", frame3);
    free(frame0.pixels);
    rt_end();
}

/* xemu: the back buffer sampled before the frame draws into it holds the content of the frame that last drew it (frame 0 solid red, sampled in frame 1 by the
 * composition: 8192 of 8192 red). The composition's draw comes first in the frame and the back buffer's own pass later. */
static void test_surface_xemu_back_buffer_before_draw(const char *selector)
{
    printf("test_surface_xemu_back_buffer_before_draw (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    d3d8_swap_replay_config config = surface_config(selector, true, true);
    CHECK(rt_begin(&config));
    write_oracle_vertices();
    map_surface_memory();
    rt_vertices_pass(RT_PRODUCER, 6u, 3u, false); /* frame 0: the back buffer, solid red */
    d3d8_swap_replay_stats stats = rt_present();
    const gpu_image *back = d3d8_swap_replay_last_frame();
    CHECK(!stats.latched && back != NULL && back->pixels != NULL && count_not(back, 255u, 0u, 0u, 255u) == 0u);
    d3d8_set_texture(0u, RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, UINT32_MAX, RT_CLAMP, RT_LINEAR); /* frame 1: the composition samples the back buffer first */
    rt_yellow_pass(RT_PRODUCER, false);                              /* and the back buffer is drawn afterwards */
    stats = rt_present();
    if (stats.latched) {
        printf("  frame 1: %s\n", stats.error);
    }
    CHECK(!stats.latched && stats.frames_replayed == 2u && stats.surface_kept_samples == 1u);
    const gpu_image *composition = d3d8_swap_replay_offscreen_frame(0u, NULL);
    CHECK(composition != NULL && composition->pixels != NULL);
    if (composition != NULL && composition->pixels != NULL) {
        CHECK(ORACLE_PIXELS - count_not(composition, 255u, 0u, 0u, 255u) == ORACLE_PIXELS); /* 100 percent red */
    }
    const gpu_image *now = d3d8_swap_replay_last_frame();
    CHECK(now != NULL && now->pixels != NULL && pixel_is(now, 96u, 48u, 255u, 255u, 0u, 255u) && pixel_is(now, 32u, 12u, 255u, 0u, 0u, 255u)); /* the new triangle over the persisted red */
    rt_end();
}

/* xemu measured "GPU clear in frame 0, CPU overwrite of the LEFT half in frame 1" as ONE image: left half = the CPU value, right half = the GPU clear value (4096
 * pixels each): the surface IS guest memory, earlier GPU writes stay and later CPU writes win. This replay cannot order a CPU write against a pass it did not
 * observe (the host never writes a GPU result to guest memory), so it REFUSES by name, "cpu written surface". THE DAY THE MODEL ADOPTS THE XEMU RULE (the kept
 * image with the CPU's bytes laid over it) THIS TEST IS THE ONE TO CHANGE: the sampled image must then have 4096 pixels of the CPU value on the left and 4096 of
 * the clear colour on the right, and no refusal. */
static void test_surface_xemu_cpu_overwrite_refused(const char *selector)
{
    printf("test_surface_xemu_cpu_overwrite_refused (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    d3d8_swap_replay_config config = surface_config(selector, true, true);
    CHECK(rt_begin(&config));
    write_oracle_vertices();
    map_surface_memory();
    rt_vertices_pass(RT_PRODUCER, 6u, 3u, true); /* frame 0: the GPU clear, blue over the whole surface */
    d3d8_swap_replay_stats stats = rt_present();
    CHECK(!stats.latched && stats.frames_replayed == 1u);
    for (uint32_t y = 0u; y < RT_EDGE_HEIGHT; y++) { /* frame 1: the CPU overwrites the left half (64 of 128 columns, 4096 pixels) */
        for (uint32_t x = 0u; x < RT_EDGE_WIDTH / 2u; x++) {
            store(RT_PRODUCER_DATA + y * RT_EDGE_WIDTH * 4u + x * 4u, 0xFFFFFF00u); /* ARGB yellow */
        }
    }
    d3d8_set_texture(0u, RT_PRODUCER);
    rt_consumer_pass(RT_CONSUMER, UINT32_MAX, RT_CLAMP, RT_LINEAR);
    stats = rt_present();
    expect_surface_refusal(&stats, "cpu written surface", NULL);
    CHECK(stats.frames_replayed == 1u && stats.surface_kept_samples == 0u && stats.rt_texture_draws == 0u);
    rt_end();
}

/* A surface nothing wrote, in this host (all guest bytes zero): ARGB 0 for every one of the 8192 pixels. (xemu's own never written surface held stale boot
 * bytes, 5270 of 8192 zero: the guest bytes as they are, which here are zero.) */
static void test_surface_xemu_never_written(const char *selector)
{
    printf("test_surface_xemu_never_written (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    d3d8_swap_replay_config config = surface_config(selector, true, true);
    CHECK(rt_begin(&config));
    map_surface_memory();
    rt_second_header(TEXTURE_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    rt_consumer_pass(RT_CONSUMER, TARGET_C, RT_CLAMP, RT_LINEAR);
    const d3d8_swap_replay_stats stats = rt_present();
    const gpu_image *frame = d3d8_swap_replay_last_frame();
    CHECK(!stats.latched && stats.surface_guest_zero_samples == 1u && frame != NULL && frame->pixels != NULL);
    if (frame != NULL && frame->pixels != NULL) {
        CHECK(ORACLE_PIXELS - count_not(frame, 0u, 0u, 0u, 0u) == ORACLE_PIXELS && ORACLE_PIXELS == 8192u);
    }
    rt_end();
}



/* Source pixels with alphas that are neither 0 nor 0xFF (0x40..0x7F), and destination pixels with others (0x90..0xAF): a forced alpha and a copied one both show. */
static uint32_t blit_source_argb(uint32_t x, uint32_t y)
{
    return ((0x40u + ((x ^ y) & 0x3Fu)) << 24) | (((x * 2u + 1u) & 0xFFu) << 16) | (((y * 3u + 5u) & 0xFFu) << 8) | ((x + y * 5u + 9u) & 0xFFu);
}

static void fill_generated(uint32_t data, uint32_t pitch, uint32_t width, uint32_t rows, uint32_t (*pixel)(uint32_t, uint32_t))
{
    for (uint32_t y = 0u; y < rows; y++) {
        for (uint32_t x = 0u; x < width; x++) {
            store(data + y * pitch + x * 4u, pixel(x, y));
        }
    }
}



/* Formats 0xA, 7 and 6 between two PASS images, with the surface source on and off (the same behaviour): the destination keeps everything outside the rectangle. */
/* Formats 0xA, 7 and 6 over surfaces the replay holds NO image of (the surface source builds them from guest memory): exact pixels from the model, the rows the
 * blit reaches only, the destination's own guest pixels outside the rectangle. */
/* The clamp (MEASURED for 0xA only): a 96 pixel request over a 64 pixel pitch copies 64 pixels a row and leaves the rest of the destination row, whichever side is
 * narrow. Formats 7 and 6 with a clamped row are refused. Then the in-place overlaps of one guest built surface: a downward one smears with the period of the shift,
 * a rightward one of a row is exact. */
static void test_surface_blit_overlap(const char *selector)
{
    printf("test_surface_blit_overlap (%s)\n", selector);
    write_synthetic_vertex_module(draw_vertex_texel_words, sizeof draw_vertex_texel_words);
    d3d8_swap_replay_config config = surface_config(selector, true, false);
    d3d8_swap_replay_stats stats;

    const gpu_image *after = NULL;
    /* downward in place, 3 rows: rows ascend, row y of the destination is original row y % 3 (period 3), rows 0..2 untouched; 35 rows are held */
    CHECK(rt_begin(&config));
    map_surface_memory();
    fill_generated(BLIT_SOURCE, 256u, 64u, 35u, blit_source_argb);
    stats = blit_present(BLIT_SOURCE, BLIT_SOURCE, 0xAu, PITCHES_256, 0u, 0u, 0u, 3u, 64u, 32u);
    if (stats.latched) {
        printf("  downward: %s\n", stats.error);
    }
    CHECK(!stats.latched && stats.copies_applied == 1u && stats.blit_guest_surfaces == 1u);
    after = d3d8_swap_replay_surface_image(BLIT_SOURCE);
    CHECK(after != NULL && after->width == 64u && after->height == 35u);
    uint32_t wrong = 0u;
    for (uint32_t y = 0u; after != NULL && after->pixels != NULL && y < after->height; y++) {
        for (uint32_t x = 0u; x < after->width; x++) {
            const uint32_t argb = blit_source_argb(x, y < 3u ? y : y % 3u);
            wrong += !pixel_is(after, x, y, (uint8_t)(argb >> 16), (uint8_t)(argb >> 8), (uint8_t)argb, (uint8_t)(argb >> 24));
        }
    }
    CHECK(wrong == 0u);
    CHECK(after != NULL && after->pixels != NULL && !pixel_is(after, 5u, 34u, (uint8_t)(blit_source_argb(5u, 34u) >> 16), (uint8_t)(blit_source_argb(5u, 34u) >> 8),
                                                              (uint8_t)blit_source_argb(5u, 34u), (uint8_t)(blit_source_argb(5u, 34u) >> 24))); /* not a copy of the original rows */
    rt_end();

    /* rightward in place, 3 pixels, 4 rows: each row moves as a whole (memmove), destination x is original x - 3, x 0..2 untouched */
    CHECK(rt_begin(&config));
    map_surface_memory();
    fill_generated(BLIT_SOURCE, 256u, 64u, 4u, blit_source_argb);
    stats = blit_present(BLIT_SOURCE, BLIT_SOURCE, 0xAu, PITCHES_256, 0u, 0u, 3u, 0u, 61u, 4u);
    CHECK(!stats.latched && stats.copies_applied == 1u);
    after = d3d8_swap_replay_surface_image(BLIT_SOURCE);
    CHECK(after != NULL && after->width == 64u && after->height == 4u);
    wrong = 0u;
    for (uint32_t y = 0u; after != NULL && after->pixels != NULL && y < 4u; y++) {
        for (uint32_t x = 0u; x < 64u; x++) {
            const uint32_t argb = blit_source_argb(x < 3u ? x : x - 3u, y);
            wrong += !pixel_is(after, x, y, (uint8_t)(argb >> 16), (uint8_t)(argb >> 8), (uint8_t)argb, (uint8_t)(argb >> 24));
        }
    }
    CHECK(wrong == 0u);
    rt_end();
}

static int run_device(const char *selector)
{
    gpu_device *device = NULL;
    const gpu_result created = gpu_device_create_selected(selector, &device);
    if (created != GPU_OK) {
        printf("SKIP %s: %s\n", selector, gpu_result_string(created));
        return 0;
    }
    char expected_name[256];
    snprintf(expected_name, sizeof expected_name, "%s", gpu_device_name(device));
    gpu_device_destroy(device);
    test_selector(selector, expected_name);
    test_frames(selector);
    test_override_and_measured_sizes(selector);
    test_non_strict(selector);
    test_inference_refusal(selector);
    test_two_targets(selector);
    test_offscreen_only_frame(selector);
    test_same_target_is_one_pass(selector);
    test_target_drawn_twice_refuses(selector);
    test_override_with_two_targets_refuses(selector);
    test_size_measured_at_the_switch(selector);
    test_reset_forgets_decoder_state(selector);
    test_reset_then_switch(selector);
    test_reset_then_empty_ring_switch(selector);
    test_flip_y_option(selector);
    test_window_clip_option(selector);
    test_line_width_option(selector);
    test_vertex_snapshot_at_the_kick(selector);
    test_vertex_snapshot_across_passes(selector);
    test_one_kick_sees_the_later_bytes(selector);
    test_vertex_budget_is_per_frame(selector);
    test_swap_own_emission_reaches_the_replay(selector);
    test_output_state_scissor(selector);
    test_output_state_blend_alpha(selector);
    test_output_state_depth(selector);
    test_output_state_clear(selector);
    test_clear_in_a_pass_with_no_draw(selector);
    test_polygon_offset_counters(selector);
    test_frame_hook_gets_the_overlay_over_the_composition(selector);

    test_combiner_off_is_byte_identical(selector);
    test_combiner_on(selector);
    test_standin_texture(selector);
    test_dump_cadence(selector);
    test_corrupt_module_refuses(selector);
    test_three_targets(selector);
    test_dump_failure_is_counted(selector);
    test_blit_over_surfaces(selector);
    test_blit_refusals_in_the_replay(selector);
    test_blit_copy_rects_real(selector);
    test_blit_group_off_keeps_nothing(selector);
    test_blit_unnamed_surface_is_not_kept(selector);
    test_blit_kept_image_is_replaced(selector);
    test_blit_surface_eviction(selector);
    test_rt_texture_exact_pixels(selector);
    test_rt_texture_dxt1_guest(selector);
    test_rt_texture_dxt1_wrap(selector);
    test_rt_texture_refusals(selector);
    test_rt_texture_binding_follows_the_draw(selector);
    test_rt_texture_off_is_byte_identical(selector);
    test_rt_texture_census(selector);
    test_rt_texture_rebind(selector);
    test_rt_texture_second_producer(selector);
    test_rt_texture_tally_limit(selector);
    test_rt_texture_no_switch(selector);
    test_surface_source_guest(selector);
    test_surface_source_pass_image_first(selector);
    test_surface_kept_image(selector);
    test_surface_out_of_order(selector);
    test_surface_blit_before_the_draw(selector);
    test_surface_target_persist(selector);
    test_surface_blit_guest(selector);
    test_surface_blit_bytes(selector);
    test_surface_blit_before_edges(selector);
    test_surface_blit_guest_edges(selector);
    test_surface_blit_protect(selector);
    test_surface_slot_reuse(selector);
    test_surface_persist_unmeasured(selector);
    test_surface_filter(selector);
    test_surface_overlay_note(selector);
    test_surface_xemu_persistence(selector);
    test_surface_xemu_kept_gradient(selector);
    test_surface_xemu_back_buffer_before_draw(selector);
    test_surface_xemu_cpu_overwrite_refused(selector);
    test_surface_xemu_never_written(selector);
    test_surface_blit_overlap(selector);
    return 1;
}

/* --- T838: the live renderer's seam (live hook, live_only, binding history, target hook, backend) ----------------------------- */

static unsigned live_calls;
static size_t live_draws;
static uint64_t live_frame;
static uint32_t live_targets[8];
static unsigned live_target_calls;

static void live_model_cb(const gpu_pgraph *model, uint64_t frame, void *context)
{
    (void)context;
    live_calls++;
    live_draws = gpu_pgraph_draw_count(model);
    live_frame = frame;
}

static void live_target_cb(uint32_t header, void *context)
{
    (void)context;
    if (live_target_calls < 8u) {
        live_targets[live_target_calls] = header;
    }
    live_target_calls++;
}

/* T847: a maker that writes the module file, as live_module_maker does, and says how often it was asked. */
static unsigned maker_calls;
static bool maker_answer;

static bool module_maker_cb(void *context, bool fragment, const char *name, const uint8_t *bytes, size_t byte_count, char *error,
                            size_t error_bytes)
{
    (void)context;
    (void)bytes;
    (void)byte_count;
    (void)fragment;
    maker_calls++;
    if (!maker_answer) {
        snprintf(error, error_bytes, "no");
        return false;
    }
    write_combiner_module(name, draw_vertex_words, sizeof draw_vertex_words); /* any SPIR-V: the file is what is looked up */
    return true;
}

static void test_live_module_maker_seam(void)
{
    printf("test_live_module_maker_seam\n");
    maker_calls = 0u;
    gpu_pgraph_backend backend;
    CHECK(d3d8_swap_replay_live_backend(&backend) && backend.make_module == NULL); /* no maker, a miss stays a refusal */
    d3d8_swap_replay_set_live_module_maker(module_maker_cb, NULL);
    gpu_pgraph_backend with_maker;
    CHECK(d3d8_swap_replay_live_backend(&with_maker) && with_maker.make_module != NULL);
    const uint32_t vertex_before = with_maker.table->module_count;
    const uint32_t fragment_before = with_maker.fragment_table->module_count;
    const char *first = "generated_1111111111111111111111111111111111111111111111111111111111111111";
    const char *fragment_name = "combiner_2222222222222222222222222222222222222222222222222222222222222222";
    char error[64] = "";
    const uint8_t bytes[4] = {1u, 2u, 3u, 4u};
    uint32_t module = UINT32_MAX;

    /* refused by the maker: nothing is added */
    maker_answer = false;
    CHECK(!with_maker.make_module(NULL, false, first, bytes, sizeof bytes, error, sizeof error) && strcmp(error, "no") == 0);
    CHECK(with_maker.table->module_count == vertex_before && !gpu_vsh_lookup_name(with_maker.table, first, &module));

    /* made: appended after the existing modules (their indices do not move), found by name, its words load from the file */
    maker_answer = true;
    CHECK(with_maker.make_module(NULL, false, first, bytes, sizeof bytes, error, sizeof error));
    CHECK(with_maker.table->module_count == vertex_before + 1u && gpu_vsh_lookup_name(with_maker.table, first, &module) &&
          module == vertex_before);
    CHECK(with_maker.fragment_table->module_count == fragment_before);
    uint32_t existing = UINT32_MAX;
    CHECK(gpu_vsh_lookup_name(with_maker.table, SYNTHETIC_PROGRAM_NAME, &existing) && existing < vertex_before);
    const uint32_t *words = NULL;
    size_t word_count = 0u;
    CHECK(with_maker.load_module(NULL, module, &words, &word_count) && words != NULL && word_count == sizeof draw_vertex_words / 4u);
    CHECK(with_maker.load_module(NULL, existing, &words, &word_count)); /* the grown arrays kept the old modules loadable */

    /* a combiner goes to the fragment table, not the vertex table */
    CHECK(with_maker.make_module(NULL, true, fragment_name, bytes, sizeof bytes, error, sizeof error));
    CHECK(with_maker.fragment_table->module_count == fragment_before + 1u && with_maker.table->module_count == vertex_before + 1u);
    CHECK(gpu_vsh_lookup_name(with_maker.fragment_table, fragment_name, &module) && module == fragment_before);
    CHECK(with_maker.load_fragment_module(NULL, module, &words, &word_count));

    /* asked again for a name already in the table: true, not added twice */
    CHECK(with_maker.make_module(NULL, false, first, bytes, sizeof bytes, error, sizeof error));
    CHECK(with_maker.table->module_count == vertex_before + 1u);
    CHECK(maker_calls == 4u);

    /* the maker removed: a backend made before refuses, a new one has no maker */
    d3d8_swap_replay_set_live_module_maker(NULL, NULL);
    CHECK(!with_maker.make_module(NULL, false, "generated_3333333333333333333333333333333333333333333333333333333333333333", bytes,
                                  sizeof bytes, error, sizeof error));
    CHECK(d3d8_swap_replay_live_backend(&backend) && backend.make_module == NULL);
}

static void test_live_hook(void)
{
    printf("test_live_hook\n");
    live_calls = 0u;
    live_target_calls = 0u;
    begin_device(true);
    make_rt_target(RT_PRODUCER, RT_PRODUCER_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    make_rt_target(RT_CONSUMER, RT_CONSUMER_DATA, RT_EDGE_WIDTH, RT_EDGE_HEIGHT);
    d3d8_swap_replay_config config = base_config(NULL);
    config.strict = false;
    config.live_only = true;
    CHECK(d3d8_swap_replay_enable(&config, NULL, 0u));
    gpu_pgraph_backend backend;
    CHECK(d3d8_swap_replay_live_backend(&backend) && backend.table != NULL && backend.load_module != NULL &&
          backend.load_fragment_module != NULL && backend.read_guest != NULL);
    CHECK(backend.allowed_inferences == config.allowed_inferences && backend.output_groups == config.output_groups &&
          backend.combiner == config.combiner && backend.flip_y == config.flip_y);
    test_live_module_maker_seam();

    /* live_only with no hook: the frame is decoded and discarded, no replay, no device (nothing draws, nothing is hidden) */
    stream_builder one = {0};
    build_frame_one(&one);
    push(&one);
    present();
    d3d8_swap_replay_stats stats = d3d8_swap_replay_get_stats();
    CHECK(stats.presents == 1u && stats.frames_replayed == 0u && stats.frames_empty == 1u && stats.draws == 0u && !stats.latched);
    CHECK(d3d8_swap_replay_device_name() == NULL && d3d8_swap_replay_last_frame() == NULL);
    CHECK(live_calls == 0u && d3d8_swap_replay_live_frames() == 0u);

    /* the hook: once per present with the model while its draw list is still the frame's, the list reset after it */
    d3d8_swap_replay_set_live_hook(live_model_cb, NULL);
    d3d8_swap_replay_set_live_target_hook(live_target_cb, NULL);
    stream_builder two = {0};
    build_frame_two(&two);
    push(&two);
    present();
    CHECK(live_calls == 1u && live_draws == 1u && live_frame == presents_driven && d3d8_swap_replay_live_frames() == 1u);
    present(); /* nothing drawn: the hook still runs, with an empty list (draws 1 of the frame before are gone) */
    CHECK(live_calls == 2u && live_draws == 0u);
    stream_builder three = {0};
    build_frame_two(&three);
    push(&three);
    present();
    CHECK(live_calls == 3u && live_draws == 1u && live_frame == presents_driven);
    stats = d3d8_swap_replay_get_stats();
    CHECK(stats.frames_replayed == 0u && stats.draws == 0u && d3d8_swap_replay_device_name() == NULL); /* still no CPU replay */

    /* the SetTexture history: a stage never bound is named, the hook makes SetTexture noted (render_target_texture is off) */
    uint32_t header = 0u, format = 0u, size_word = 0u, data = 0u;
    const char *reason = NULL;
    CHECK(!d3d8_swap_replay_live_binding(0u, 0u, &header, &format, &size_word, &data, &reason) && reason != NULL && reason[0] != '\0');
    CHECK(!d3d8_swap_replay_live_binding(0u, 3u, &header, &format, &size_word, &data, &reason) && reason != NULL && reason[0] != '\0');
    d3d8_set_texture(0u, RT_PRODUCER);
    CHECK(d3d8_swap_replay_live_binding(0u, 0u, &header, &format, &size_word, &data, &reason));
    CHECK(header == RT_PRODUCER && data == RT_PRODUCER_DATA && format == d3d8_guest_load32(RT_PRODUCER + D3D8_SURFACE_FORMAT) &&
          size_word == d3d8_guest_load32(RT_PRODUCER + D3D8_SURFACE_SIZE));
    CHECK(!d3d8_swap_replay_live_binding(0u, 1u, &header, &format, &size_word, &data, &reason)); /* stage 1 never bound */
    d3d8_set_texture(0u, 0u);
    CHECK(!d3d8_swap_replay_live_binding(SIZE_MAX, 0u, &header, &format, &size_word, &data, &reason) && reason != NULL &&
          strstr(reason, "unbound") != NULL);
    CHECK(!d3d8_swap_replay_live_binding(0u, 9u, &header, &format, &size_word, &data, &reason)); /* no such stage */

    /* the render target hook: the new target, then the previous one (when there is one), a repeat is silent */
    const uint32_t before = d3d8_device_load32(DEV_RENDER_TARGET);
    live_target_calls = 0u; /* Swap's own front buffer binds above were reported as well */
    bind_target(RT_PRODUCER);
    CHECK(live_target_calls == 2u && live_targets[0] == RT_PRODUCER && live_targets[1] == before);
    bind_target(RT_PRODUCER);
    CHECK(live_target_calls == 2u);
    bind_target(RT_CONSUMER);
    CHECK(live_target_calls == 4u && live_targets[2] == RT_CONSUMER && live_targets[3] == RT_PRODUCER);
    bind_target(before);
    present();
    CHECK(live_calls == 4u); /* the frame with two switches still reaches the hook */

    /* removing the hooks stops the calls, SetTexture is no longer noted */
    d3d8_swap_replay_set_live_hook(NULL, NULL);
    d3d8_swap_replay_set_live_target_hook(NULL, NULL);
    const unsigned target_calls_removed = live_target_calls;
    d3d8_set_texture(1u, RT_PRODUCER);
    CHECK(!d3d8_swap_replay_live_binding(SIZE_MAX, 1u, &header, &format, &size_word, &data, &reason));
    bind_target(RT_CONSUMER);
    CHECK(live_target_calls == target_calls_removed);
    bind_target(before);
    present();
    CHECK(live_calls == 4u && live_target_calls == target_calls_removed);
    stream_free(&one);
    stream_free(&two);
    stream_free(&three);
    d3d8_swap_replay_disable();
    CHECK(!d3d8_swap_replay_live_backend(&backend));
    environment_end();
}

int main(void)
{
    make_directories();
    write_module();
    test_default_is_off();
    test_configuration_refusals();
    test_config_options();
    test_reset_clears_latch();
    test_reset_forgets_switches();
    test_reset_rebuilds_the_model();
    test_dimensions();
    test_strict_refusal_and_latch();
    test_fatal_on_refusal();
    test_dropped_commands_refuse();
    test_unmeasurable_size_is_a_refusal();
    test_stream_discard();
    test_open_bracket_at_present();
    test_unmeasured_target_refuses();
    test_open_bracket_at_switch();
    test_broken_switch_chain_refuses();
    test_too_many_switches_refuse();
    test_released_recording_refuses();
    test_hook_is_inert_when_disabled();
    test_vertex_budget_refuses_the_frame();
    test_vertex_snapshot_config_and_kick_decode();
    test_reset_reconfigures_the_model();
    test_commands_before_enable_decode_at_the_present();

    test_enable_validation();
    test_combiner_fixture_names();
    test_host_inferences();
    test_combiner_config();
    test_standin_plan();
    test_standin_config();
    test_combiner_words_are_decoded_only_with_the_option();
    test_switch_order_refuses();
    test_reset_forgets_switch_overflow();
    test_pairs_ignored_are_counted();
    test_rt_texture_config();
    test_rt_texture_notes();
    test_rt_texture_event_limit();
    rt_refusal_scenes(NULL, true);
    rt_rebind_scene(NULL, true);
    test_rt_texture_census_stage_one();
    test_rt_texture_census_limit();
    rt_no_switch_scene(NULL, true);
    test_rt_texture_census_refusals();
    test_live_hook();
    test_surface_config();
    test_surface_census();
    int ran = 0;
    if (!gpu_vulkan_available()) {
        printf("SKIP the device tests: no Vulkan loader on this machine\n");
    } else {
        ran += run_device("hardware");
        ran += run_device("software");
    }
    remove_tree(spv_directory);
    printf("%d checks, %d failures, %d device(s) exercised\n", checks, failures, ran);
    if (failures != 0 || checks < 100) {
        return 1;
    }
    return ran > 0 ? 0 : 77;
}
