/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Batch runner for the T100d tests (tests/test_gpu_vsh_draw.py): the translated vertex shader
 * selected by gpu_vsh_select and drawn through src/gpu's own device (gpu_vsh_draw.c) with several
 * vertices in ONE draw. Not a ctest, the Python test builds and drives it.
 *
 *   gpu_vsh_runner --info [--device SELECTOR]
 *   gpu_vsh_runner [--device SELECTOR] --spv-dir DIR MANIFEST
 *
 * Built with -I<dir holding vsh_table.inc>, the GENERATED table of tools/nv2a/vsh_modules.py
 * (never committed). Manifest lines, fields separated by blanks:
 *
 *   capture static|key ID COUNT IN OUT          transform feedback capture
 *   render  static|key ID COUNT W H IN OUT      RGBA8 pixels, cleared to 0.2 0.2 0.2 1
 *
 * ID is hexadecimal, a guest address for `static`, a raw builder key for `key` (masked by the
 * table). The module is looked up EXACTLY (gpu_vsh_lookup_static / gpu_vsh_lookup_key) and read
 * from DIR/<module name>.spv. IN is float32: the 192 x 4 constants, then COUNT x 64 attributes.
 * OUT is COUNT x 64 float32 (capture) or W x H x 4 bytes (render). One result line per job on
 * stdout:  `<n> OK <module name>`, `<n> MISS`, or `<n> FAIL <reason>`.
 */
#include "gpu_device.h"
#include "gpu_vsh_draw.h"
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

static int write_file(const char *path, const void *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    if (!file) {
        return -1;
    }
    const int ok = fwrite(data, 1u, size, file) == size;
    return (fclose(file) == 0 && ok) ? 0 : -1;
}

typedef struct {
    gpu_device *device;
    const char *spv_dir;
} runner;

/* 0 hit, 1 miss, -1 bad kind. */
static int select_module(const char *kind, uint32_t id, uint32_t *module)
{
    if (strcmp(kind, "static") == 0) {
        return gpu_vsh_lookup_static(&vsh_table, id, module) ? 0 : 1;
    }
    if (strcmp(kind, "key") == 0) {
        return gpu_vsh_lookup_key(&vsh_table, id, module) ? 0 : 1;
    }
    return -1;
}

static int run_job(const runner *run, int index, const char *line)
{
    char verb[16], kind[16], in_path[1024], out_path[1024];
    unsigned id = 0u, count = 0u, width = 0u, height = 0u;
    const int is_render = strncmp(line, "render", 6) == 0;
    int fields;
    if (is_render) {
        fields = sscanf(line, "%15s %15s %x %u %u %u %1023s %1023s", verb, kind, &id, &count,
                        &width, &height, in_path, out_path);
        fields = fields == 8 ? 0 : -1;
    } else {
        fields = sscanf(line, "%15s %15s %x %u %1023s %1023s", verb, kind, &id, &count, in_path,
                        out_path);
        fields = fields == 6 ? 0 : -1;
    }
    if (fields != 0 || (!is_render && strcmp(verb, "capture") != 0)) {
        printf("%d FAIL malformed job\n", index);
        return 1;
    }
    uint32_t module = 0u;
    const int found = select_module(kind, id, &module);
    if (found < 0) {
        printf("%d FAIL kind must be static or key\n", index);
        return 1;
    }
    if (found > 0) {
        printf("%d MISS\n", index);
        return 0;
    }
    const char *name = vsh_module_names[module];
    char spv_path[1100];
    snprintf(spv_path, sizeof spv_path, "%s/%s.spv", run->spv_dir, name);
    void *code = NULL, *input = NULL;
    size_t code_size = 0u, input_size = 0u;
    if (read_file(spv_path, &code, &code_size) != 0 || code_size % 4u != 0u) {
        printf("%d FAIL cannot read %s\n", index, spv_path);
        free(code);
        return 1;
    }
    const size_t want_input = ((size_t)GPU_VSH_CONSTANT_ROWS * 4u + (size_t)count * GPU_VSH_ATTRIBUTE_FLOATS) *
                              sizeof(float);
    if (read_file(in_path, &input, &input_size) != 0 || input_size != want_input) {
        printf("%d FAIL input file is not constants + COUNT x 64 float32\n", index);
        free(code);
        free(input);
        return 1;
    }
    const float *constants = input;
    const gpu_vsh_draw draw = {
        .words = code,
        .word_count = code_size / 4u,
        .vertex_count = count,
        .attributes = constants + GPU_VSH_CONSTANT_ROWS * 4u,
        .constants = constants,
    };
    gpu_result outcome;
    if (is_render) {
        static const float clear[4] = { 0.2f, 0.2f, 0.2f, 1.0f };
        gpu_image image = { 0 };
        outcome = gpu_vsh_render(run->device, width, height, clear, &draw, &image);
        if (outcome == GPU_OK) {
            outcome = write_file(out_path, image.pixels, (size_t)width * height * 4u) == 0
                          ? GPU_OK : GPU_ERR_ARGUMENT;
        }
        gpu_image_free(&image);
    } else {
        float *capture = malloc((size_t)count * GPU_VSH_CAPTURE_FLOATS * sizeof(float));
        outcome = capture ? gpu_vsh_capture(run->device, &draw, capture) : GPU_ERR_OUT_OF_MEMORY;
        if (outcome == GPU_OK) {
            outcome = write_file(out_path, capture,
                                 (size_t)count * GPU_VSH_CAPTURE_FLOATS * sizeof(float)) == 0
                          ? GPU_OK : GPU_ERR_ARGUMENT;
        }
        free(capture);
    }
    free(code);
    free(input);
    if (outcome != GPU_OK) {
        printf("%d FAIL %s\n", index, gpu_result_string(outcome));
        return 1;
    }
    printf("%d OK %s\n", index, name);
    return 0;
}

int main(int argc, char **argv)
{
    const char *selector = NULL;
    const char *spv_dir = NULL;
    const char *manifest = NULL;
    int info = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            selector = argv[++i];
        } else if (strcmp(argv[i], "--spv-dir") == 0 && i + 1 < argc) {
            spv_dir = argv[++i];
        } else if (strcmp(argv[i], "--info") == 0) {
            info = 1;
        } else if (argv[i][0] != '-' && !manifest) {
            manifest = argv[i];
        } else {
            fprintf(stderr, "usage: gpu_vsh_runner [--device SEL] (--info | --spv-dir DIR MANIFEST)\n");
            return 2;
        }
    }
    gpu_device *device = NULL;
    const gpu_result created = gpu_device_create_selected(selector, &device);
    if (created != GPU_OK) {
        printf("unavailable: %s\n", gpu_result_string(created));
        return 3;
    }
    if (info) {
        printf("device: %s\ntransform_feedback: %d\ntable_valid: %d\n", gpu_device_name(device),
               gpu_device_has_transform_feedback(device), gpu_vsh_table_valid(&vsh_table));
        gpu_device_destroy(device);
        return 0;
    }
    if (!spv_dir || !manifest) {
        fprintf(stderr, "need --spv-dir and a manifest\n");
        gpu_device_destroy(device);
        return 2;
    }
    FILE *file = fopen(manifest, "r");
    if (!file) {
        fprintf(stderr, "cannot open %s\n", manifest);
        gpu_device_destroy(device);
        return 2;
    }
    const runner run = { device, spv_dir };
    char line[4096];
    int index = 0;
    int failed = 0;
    while (fgets(line, sizeof line, file)) {
        if (line[0] == '\n' || line[0] == '#') {
            continue;
        }
        failed += run_job(&run, index++, line);
        fflush(stdout);
    }
    fclose(file);
    gpu_device_destroy(device);
    return failed ? 1 : 0;
}
