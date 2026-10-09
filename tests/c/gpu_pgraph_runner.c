/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Runner for the T84 tests over the REAL corpus (tests/test_gpu_pgraph.py): a recorded stream is
 * decoded and replayed with the generated selector table, the way the renderer will. Not a ctest,
 * the Python test builds it against the generated, never committed, vsh_table.inc.
 *
 *   gpu_pgraph_runner [--device SELECTOR] --info
 *   gpu_pgraph_runner resolve MANIFEST
 *   gpu_pgraph_runner render [--device SELECTOR] [--flip-y] [--allow MASK] --spv-dir DIR
 *                            --stream FILE --memory FILE --base HEX --size WxH --out FILE
 *
 * resolve: MANIFEST lines are `SLOT BINFILE`. Each BINFILE is a corpus program (4 byte header,
 * then 16-byte instructions). The program is written as the title's upload would be (execution
 * mode 6, 0x1E9C SLOT, the instructions on 0x0B00 in chunks of 32 dwords, 0x1EA0 SLOT), decoded,
 * and resolved by its SHA-256 through the table. Output line per job: `<n> OK <module name> <count>`
 * or `<n> FAIL <why>`.
 *
 * render: STREAM is little-endian u32 pairs (method, data). MEMORY is the guest bytes the vertex
 * arrays point into, mapped at BASE. OUT is width x height x 4 bytes. One line: `OK drawn=<n>
 * vertices=<n> inferences=<n>` or `FAIL <result> <error>`.
 */
#include "gpu_device.h"
#include "gpu_pgraph.h"
#include "gpu_pgraph_replay.h"
#include "gpu_pgraph_test_support.h"
#include "gpu_vsh_select.h"

#include "vsh_table.inc"

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
    const char *spv_dir;
    uint32_t loaded_module;
    void *words;
    size_t size;
    fake_guest guest;
} runner_context;

static bool load_module(void *context, uint32_t module, const uint32_t **words, size_t *word_count)
{
    runner_context *run = context;
    if (run->words == NULL || run->loaded_module != module) {
        free(run->words);
        run->words = NULL;
        char path[1100];
        snprintf(path, sizeof path, "%s/%s.spv", run->spv_dir, vsh_module_names[module]);
        if (read_file(path, &run->words, &run->size) != 0 || run->size % 4u != 0u) {
            free(run->words);
            run->words = NULL;
            return false;
        }
        run->loaded_module = module;
    }
    *words = run->words;
    *word_count = run->size / 4u;
    return true;
}

static bool read_guest(void *context, uint32_t address, void *out, size_t bytes)
{
    runner_context *run = context;
    return fake_guest_read(&run->guest, address, out, bytes);
}

static int run_resolve(const char *manifest_path)
{
    char *text = NULL;
    size_t length = 0u;
    if (read_file(manifest_path, (void **)&text, &length) != 0) {
        printf("FAIL cannot read %s\n", manifest_path);
        return 1;
    }
    text = realloc(text, length + 1u);
    text[length] = '\0';
    gpu_pgraph_backend backend = {0};
    backend.table = &vsh_table;
    backend.allowed_inferences = GPU_PGRAPH_INFER_ALL;
    int job = 0;
    int bad = 0;
    for (char *line = strtok(text, "\n"); line != NULL; line = strtok(NULL, "\n"), job++) {
        unsigned slot = 0u;
        char bin[1024];
        if (sscanf(line, "%u %1023s", &slot, bin) != 2) {
            printf("%d FAIL malformed job\n", job);
            bad = 1;
            continue;
        }
        void *data = NULL;
        size_t size = 0u;
        if (read_file(bin, &data, &size) != 0 || size < 4u || (size - 4u) % 16u != 0u) {
            printf("%d FAIL cannot read %s\n", job, bin);
            free(data);
            bad = 1;
            continue;
        }
        const uint32_t instructions = (uint32_t)((size - 4u) / 16u);
        uint32_t *words = malloc(size - 4u);
        memcpy(words, (const uint8_t *)data + 4u, size - 4u);
        stream_builder stream = {0};
        stream_pair(&stream, GPU_PGRAPH_EXECUTION_MODE, 6u);
        stream_program(&stream, slot, words, instructions);
        stream_pair(&stream, GPU_PGRAPH_PROGRAM_START, slot);
        gpu_pgraph *pgraph = gpu_pgraph_create();
        gpu_pgraph_program program;
        gpu_pgraph_report report;
        memset(&report, 0, sizeof report);
        gpu_pgraph_result result = gpu_pgraph_decode(pgraph, stream.pairs, stream.count);
        if (result == GPU_PGRAPH_OK) {
            result = gpu_pgraph_resolve_program(gpu_pgraph_state_now(pgraph), &backend, &program, &report);
        }
        if (result == GPU_PGRAPH_OK) {
            printf("%d OK %s %u\n", job, vsh_module_names[program.module], (unsigned)program.instructions);
        } else {
            printf("%d FAIL %s %s\n", job, gpu_pgraph_result_string(result),
                   report.error[0] ? report.error : gpu_pgraph_error(pgraph));
            bad = 1;
        }
        gpu_pgraph_destroy(pgraph);
        stream_free(&stream);
        free(words);
        free(data);
    }
    free(text);
    return bad;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: gpu_pgraph_runner info|resolve|render ...\n");
        return 2;
    }
    const char *mode = NULL;
    const char *device_name = "";
    const char *spv_dir = ".", *stream_path = NULL, *memory_path = NULL, *out_path = NULL;
    unsigned base = 0u, width = 64u, height = 64u, allow = GPU_PGRAPH_INFER_ALL;
    bool flip_y = false;
    const char *manifest = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            device_name = argv[++i];
        } else if (strcmp(argv[i], "--spv-dir") == 0 && i + 1 < argc) {
            spv_dir = argv[++i];
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
        } else if (strcmp(argv[i], "--flip-y") == 0) {
            flip_y = true;
        } else if (strcmp(argv[i], "--info") == 0) {
            mode = "info";
        } else if (mode == NULL) {
            mode = argv[i];
        } else {
            manifest = argv[i];
        }
    }
    if (mode == NULL) {
        fprintf(stderr, "no mode\n");
        return 2;
    }
    if (strcmp(mode, "resolve") == 0) {
        return manifest != NULL ? run_resolve(manifest) : 2;
    }
    gpu_device *device = NULL;
    const gpu_result created = gpu_device_create_selected(device_name, &device);
    if (strcmp(mode, "info") == 0) {
        if (created != GPU_OK) {
            printf("FAIL %s\n", gpu_result_string(created));
            return 1;
        }
        printf("device: %s\ntable_valid: %d\nmodules: %u\n", gpu_device_name(device),
               gpu_vsh_table_valid(&vsh_table), (unsigned)vsh_table.module_count);
        gpu_device_destroy(device);
        return 0;
    }
    if (strcmp(mode, "render") != 0 || stream_path == NULL || out_path == NULL) {
        fprintf(stderr, "render needs --stream and --out\n");
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
    runner_context run = {0};
    run.spv_dir = spv_dir;
    run.guest.base = base;
    run.guest.bytes = memory_bytes;
    run.guest.size = memory_size;
    gpu_pgraph_backend backend = {0};
    backend.table = &vsh_table;
    backend.read_guest = read_guest;
    backend.load_module = load_module;
    backend.context = &run;
    backend.allowed_inferences = allow;
    backend.viewport_inverse_modules = true;
    backend.flip_y = flip_y;
    gpu_pgraph *pgraph = gpu_pgraph_create();
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
    printf("OK drawn=%u vertices=%u inferences=%u\n", (unsigned)report.drawn, (unsigned)report.vertices,
           (unsigned)report.used_inferences);
    gpu_image_free(&frame);
    gpu_pgraph_destroy(pgraph);
    gpu_device_destroy(device);
    return 0;
}
