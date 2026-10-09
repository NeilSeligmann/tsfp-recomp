/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Runner for tests/test_gpu_combiner_replay.py (T75): the combiner plan and the combiner fragment
 * stage of the pushbuffer replay, driven with configurations and modules the Python test builds at
 * run time. Not a ctest, and it needs no generated table: the fragment table is the list of the
 * `combiner_*.spv` files of a directory, the vertex table is one name.
 *
 *   gpu_combiner_runner [--device SELECTOR] --info
 *   gpu_combiner_runner plan --blocks FILE [--allow MASK] [--textures]
 *   gpu_combiner_runner render [--device SELECTOR] --vsh FILE --vsh-name NAME --fsh-dir DIR
 *                              --stream FILE --memory FILE --base HEX --size WxH --out FILE
 *                              [--allow MASK] [--no-combiner] [--texture N:WxH:FILE]...
 *
 * plan: FILE is concatenated 240-byte pixel-shader definitions (60 dwords). Each is written into a
 * stream as the writer of d3d8_shader.c would, plus 0x1E70 with its word 54, decoded with the
 * combiner on and planned. One line per block, `<n> OK <name> reads=<hex> stages=<n> tex=<hex>
 * inf=<hex>` or `<n> REFUSED <message>`. --textures supplies a 1x1 test texture for all four stages.
 *
 * render: STREAM is little-endian u32 pairs (method, data), MEMORY the guest bytes at BASE, OUT is
 * width x height x 4 bytes. One line: `OK drawn=<n> vertices=<n> inferences=<hex>` or `FAIL <result>
 * <error>`.
 */
#include "gpu_combiner.h"
#include "gpu_device.h"
#include "gpu_pgraph.h"
#include "gpu_pgraph_replay.h"
#include "gpu_pgraph_test_support.h"
#include "gpu_vsh_select.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_file(const char *path, void **data, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        return -1;
    }
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length < 0) {
        fclose(file);
        return -1;
    }
    *data = malloc(length ? (size_t)length : 1u);
    if (!*data || fread(*data, 1u, (size_t)length, file) != (size_t)length) {
        fclose(file);
        free(*data);
        *data = NULL;
        return -1;
    }
    fclose(file);
    *size = (size_t)length;
    return 0;
}

typedef struct {
    const char *fsh_dir;
    const char *vsh_path;
    char **fragment_names;
    uint32_t fragment_count;
    void *vertex_words;
    size_t vertex_size;
    void *fragment_words;
    size_t fragment_size;
    fake_guest guest;
} runner_context;

static bool load_vertex(void *context, uint32_t module, const uint32_t **words, size_t *word_count)
{
    runner_context *run = context;
    if (module != 0u) {
        return false;
    }
    if (run->vertex_words == NULL &&
        (read_file(run->vsh_path, &run->vertex_words, &run->vertex_size) != 0 ||
         run->vertex_size % 4u != 0u)) {
        return false;
    }
    *words = run->vertex_words;
    *word_count = run->vertex_size / 4u;
    return true;
}

static bool load_fragment(void *context, uint32_t module, const uint32_t **words, size_t *word_count)
{
    runner_context *run = context;
    if (module >= run->fragment_count) {
        return false;
    }
    free(run->fragment_words);
    run->fragment_words = NULL;
    char path[1200];
    snprintf(path, sizeof path, "%s/%s.spv", run->fsh_dir, run->fragment_names[module]);
    if (read_file(path, &run->fragment_words, &run->fragment_size) != 0 ||
        run->fragment_size % 4u != 0u) {
        return false;
    }
    *words = run->fragment_words;
    *word_count = run->fragment_size / 4u;
    return true;
}

static bool read_guest(void *context, uint32_t address, void *out, size_t bytes)
{
    runner_context *run = context;
    return fake_guest_read(&run->guest, address, out, bytes);
}

static int collect_fragments(runner_context *run)
{
    DIR *directory = opendir(run->fsh_dir);
    if (directory == NULL) {
        return -1;
    }
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        const size_t length = strlen(entry->d_name);
        if (length > 4u && strcmp(entry->d_name + length - 4u, ".spv") == 0) {
            run->fragment_names = realloc(run->fragment_names, (run->fragment_count + 1u) * sizeof(char *));
            run->fragment_names[run->fragment_count] = strndup(entry->d_name, length - 4u);
            run->fragment_count++;
        }
    }
    closedir(directory);
    return 0;
}

static int run_plan(const char *blocks_path, unsigned allow, bool textures)
{
    void *data = NULL;
    size_t size = 0u;
    if (read_file(blocks_path, &data, &size) != 0 || size % 240u != 0u) {
        printf("FAIL cannot read the blocks\n");
        return 1;
    }
    static const uint8_t texel[4] = {1u, 2u, 3u, 255u};
    gpu_combiner_texture provided[GPU_COMBINER_TEXTURE_STAGES] = {{0}};
    if (textures) {
        for (uint32_t stage = 0u; stage < GPU_COMBINER_TEXTURE_STAGES; stage++) {
            provided[stage].rgba = texel;
            provided[stage].width = 1u;
            provided[stage].height = 1u;
        }
    }
    for (size_t job = 0u; job < size / 240u; job++) {
        uint32_t words[60];
        memcpy(words, (const uint8_t *)data + job * 240u, sizeof words);
        stream_builder stream = {0};
        stream_pixel_shader(&stream, words);
        stream_pair(&stream, 0x1E70u, words[54]);
        gpu_pgraph *pgraph = gpu_pgraph_create();
        gpu_pgraph_set_combiner(pgraph, true);
        gpu_combiner_plan plan;
        char error[300];
        gpu_pgraph_result result = gpu_pgraph_decode(pgraph, stream.pairs, stream.count);
        if (result == GPU_PGRAPH_OK) {
            result = gpu_combiner_plan_build(gpu_pgraph_state_now(pgraph), allow, provided, &plan, error,
                                             sizeof error);
        } else {
            snprintf(error, sizeof error, "decode: %s", gpu_pgraph_error(pgraph));
        }
        if (result == GPU_PGRAPH_OK) {
            printf("%zu OK %s reads=%x stages=%u tex=%x inf=%x\n", job, plan.name, (unsigned)plan.reads,
                   (unsigned)plan.stage_count, (unsigned)plan.texture_stages,
                   (unsigned)plan.used_inferences);
        } else {
            printf("%zu REFUSED %s\n", job, error);
        }
        gpu_pgraph_destroy(pgraph);
        stream_free(&stream);
    }
    free(data);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: gpu_combiner_runner info|plan|render ...\n");
        return 2;
    }
    const char *mode = NULL, *device_name = "", *blocks = NULL;
    runner_context run = {0};
    const char *stream_path = NULL, *memory_path = NULL, *out_path = NULL, *vsh_name = NULL;
    unsigned base = 0u, width = 64u, height = 64u, allow = GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_COMBINER_ALL;
    bool combiner = true, textures = false;
    gpu_standin_unit_rule unit_rules[GPU_STANDIN_UNIT_RULES];
    uint32_t unit_rule_count = 0u;
    struct {
        unsigned stage, width, height;
        void *data;
    } texture_files[GPU_COMBINER_TEXTURE_STAGES] = {{0}};
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            device_name = argv[++i];
        } else if (strcmp(argv[i], "--blocks") == 0 && i + 1 < argc) {
            blocks = argv[++i];
        } else if (strcmp(argv[i], "--vsh") == 0 && i + 1 < argc) {
            run.vsh_path = argv[++i];
        } else if (strcmp(argv[i], "--vsh-name") == 0 && i + 1 < argc) {
            vsh_name = argv[++i];
        } else if (strcmp(argv[i], "--fsh-dir") == 0 && i + 1 < argc) {
            run.fsh_dir = argv[++i];
        } else if (strcmp(argv[i], "--stream") == 0 && i + 1 < argc) {
            stream_path = argv[++i];
        } else if (strcmp(argv[i], "--memory") == 0 && i + 1 < argc) {
            memory_path = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(argv[i], "--base") == 0 && i + 1 < argc) {
            base = (unsigned)strtoul(argv[++i], NULL, 16);
        } else if (strcmp(argv[i], "--allow") == 0 && i + 1 < argc) {
            allow = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%ux%u", &width, &height) != 2) {
                fprintf(stderr, "bad --size\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--no-combiner") == 0) {
            combiner = false;
        } else if (strcmp(argv[i], "--textures") == 0) {
            textures = true;
        } else if (strcmp(argv[i], "--texture") == 0 && i + 1 < argc) {
            unsigned stage = 0u, w = 0u, h = 0u;
            char path[1024];
            if (sscanf(argv[++i], "%u:%ux%u:%1023s", &stage, &w, &h, path) != 4 ||
                stage >= GPU_COMBINER_TEXTURE_STAGES) {
                fprintf(stderr, "bad --texture\n");
                return 2;
            }
            size_t bytes = 0u;
            if (read_file(path, &texture_files[stage].data, &bytes) != 0 || bytes != (size_t)w * h * 4u) {
                printf("FAIL cannot read texture %s\n", path);
                return 1;
            }
            texture_files[stage].width = w;
            texture_files[stage].height = h;
        } else if (strcmp(argv[i], "--unit") == 0 && i + 1 < argc) {
            /* T719: PREFIX:texel or PREFIX:normalised, a per-draw stand-in unit rule (repeatable) */
            char prefix[GPU_STANDIN_UNIT_MAX_DIGITS + 2u], unit[16];
            if (sscanf(argv[++i], "%65[^:]:%15s", prefix, unit) != 2 ||
                (strcmp(unit, "texel") != 0 && strcmp(unit, "normalised") != 0) ||
                !gpu_standin_unit_rule_add(unit_rules, &unit_rule_count, prefix, strcmp(unit, "texel") == 0)) {
                fprintf(stderr, "bad --unit\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--info") == 0) {
            mode = "info";
        } else if (mode == NULL) {
            mode = argv[i];
        }
    }
    if (mode == NULL) {
        fprintf(stderr, "no mode\n");
        return 2;
    }
    if (strcmp(mode, "plan") == 0) {
        return blocks != NULL ? run_plan(blocks, allow, textures) : 2;
    }
    gpu_device *device = NULL;
    const gpu_result created = gpu_device_create_selected(device_name, &device);
    if (strcmp(mode, "info") == 0) {
        if (created != GPU_OK) {
            printf("FAIL %s\n", gpu_result_string(created));
            return 1;
        }
        printf("device: %s\n", gpu_device_name(device));
        gpu_device_destroy(device);
        return 0;
    }
    if (strcmp(mode, "render") != 0 || stream_path == NULL || out_path == NULL || run.vsh_path == NULL ||
        vsh_name == NULL || run.fsh_dir == NULL) {
        fprintf(stderr, "render needs --vsh, --vsh-name, --fsh-dir, --stream and --out\n");
        return 2;
    }
    if (created != GPU_OK) {
        printf("FAIL no device %s\n", gpu_result_string(created));
        return 1;
    }
    void *stream_bytes = NULL, *memory_bytes = NULL;
    size_t stream_size = 0u, memory_size = 0u;
    if (read_file(stream_path, &stream_bytes, &stream_size) != 0 || stream_size % 8u != 0u) {
        printf("FAIL cannot read the stream\n");
        return 1;
    }
    if (memory_path != NULL && read_file(memory_path, &memory_bytes, &memory_size) != 0) {
        printf("FAIL cannot read the memory\n");
        return 1;
    }
    if (collect_fragments(&run) != 0) {
        printf("FAIL cannot list %s\n", run.fsh_dir);
        return 1;
    }
    run.guest.base = base;
    run.guest.bytes = memory_bytes;
    run.guest.size = memory_size;
    const char *vertex_names[1] = {vsh_name};
    const struct gpu_vsh_table vertex_table = {0u, 0u, NULL, 0u, NULL, 1u, vertex_names};
    const struct gpu_vsh_table fragment_table = {0u, 0u, NULL, 0u, NULL, run.fragment_count,
                                                 (const char *const *)run.fragment_names};
    gpu_pgraph_backend backend = {0};
    backend.table = &vertex_table;
    backend.read_guest = read_guest;
    backend.load_module = load_vertex;
    backend.context = &run;
    backend.allowed_inferences = allow;
    backend.combiner = combiner;
    backend.fragment_table = &fragment_table;
    backend.load_fragment_module = load_fragment;
    for (uint32_t stage = 0u; stage < GPU_COMBINER_TEXTURE_STAGES; stage++) {
        backend.test_textures[stage].rgba = texture_files[stage].data;
        backend.test_textures[stage].width = texture_files[stage].width;
        backend.test_textures[stage].height = texture_files[stage].height;
    }
    memcpy(backend.standin_unit_rules, unit_rules, sizeof unit_rules);
    backend.standin_unit_rule_count = unit_rule_count;
    gpu_pgraph *pgraph = gpu_pgraph_create();
    gpu_pgraph_set_combiner(pgraph, combiner);
    const gpu_pgraph_result decoded =
        gpu_pgraph_decode(pgraph, (const gpu_pgraph_command *)stream_bytes, stream_size / 8u);
    if (decoded != GPU_PGRAPH_OK) {
        printf("FAIL %s %s\n", gpu_pgraph_result_string(decoded), gpu_pgraph_error(pgraph));
        return 1;
    }
    static const float clear[4] = {0.2f, 0.2f, 0.2f, 1.0f};
    gpu_image frame = {0};
    gpu_pgraph_report report;
    const gpu_pgraph_result replayed =
        gpu_pgraph_replay(pgraph, device, &backend, width, height, clear, &frame, &report);
    if (replayed != GPU_PGRAPH_OK) {
        printf("FAIL %s %s\n", gpu_pgraph_result_string(replayed), report.error);
        return 1;
    }
    FILE *out = fopen(out_path, "wb");
    if (out == NULL || fwrite(frame.pixels, 1u, (size_t)frame.stride_bytes * frame.height, out) !=
                           (size_t)frame.stride_bytes * frame.height) {
        printf("FAIL cannot write the frame\n");
        return 1;
    }
    fclose(out);
    printf("OK drawn=%u vertices=%u inferences=%x\n", (unsigned)report.drawn, (unsigned)report.vertices,
           (unsigned)report.used_inferences);
    gpu_image_free(&frame);
    gpu_pgraph_destroy(pgraph);
    gpu_device_destroy(device);
    return 0;
}
