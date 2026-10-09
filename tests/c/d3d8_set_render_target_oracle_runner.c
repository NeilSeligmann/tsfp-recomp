/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Ring-inclusive direct SetRenderTarget runner, using the T443 differential window protocol. */
#include "test_d3d8_support.h"

#include "d3d8_bind.h"

#define MAX_STEPS 8u
#define MAX_WINDOWS 16u
#define MAX_WINDOW_BYTES 0x200000u
#define NO_FATAL 0xFFFFFFFFu
#define STEP_WORDS 7u

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *in = fopen(argv[1], "rb");
    FILE *out = fopen(argv[2], "wb");
    uint32_t step_count = 0u;
    uint32_t steps[MAX_STEPS][STEP_WORDS];
    uint32_t window_count = 0u;
    uint32_t windows[MAX_WINDOWS][2];
    uint8_t *bytes[MAX_WINDOWS] = {0};
    if (in == NULL || out == NULL || fread(&step_count, 4u, 1u, in) != 1u || step_count == 0u ||
        step_count > MAX_STEPS) return 3;
    for (uint32_t i = 0u; i < step_count; i++) {
        if (fread(steps[i], sizeof(steps[i]), 1u, in) != 1u) return 3;
    }
    if (fread(&window_count, 4u, 1u, in) != 1u || window_count == 0u ||
        window_count > MAX_WINDOWS) return 3;
    for (uint32_t i = 0u; i < window_count; i++) {
        if (fread(windows[i], sizeof(windows[i]), 1u, in) != 1u || windows[i][1] == 0u ||
            windows[i][1] > MAX_WINDOW_BYTES) return 3;
        bytes[i] = malloc(windows[i][1]);
        if (bytes[i] == NULL) return 3;
    }
    for (uint32_t i = 0u; i < window_count; i++) {
        if (fread(bytes[i], windows[i][1], 1u, in) != 1u) return 3;
    }

    environment_begin(KERNEL_AV_PACK_HDTV);
    for (uint32_t i = 0u; i < window_count; i++) {
        const uint32_t base = windows[i][0];
        const uint32_t size = windows[i][1];
        if (!(base >= D3D_REGION_BASE && base + size <= D3D_REGION_BASE + D3D_REGION_BYTES) &&
            !(base >= RDATA_REGION_BASE && base + size <= RDATA_REGION_BASE + RDATA_REGION_BYTES)) {
            map_fixed(base, size);
        }
        memcpy(kernel_guest_at(base, size), bytes[i], size);
    }

    uint32_t results[MAX_STEPS] = {0u};
    uint32_t fatal_step = NO_FATAL;
    for (uint32_t i = 0u; i < step_count; i++) {
        if (steps[i][0] != 0x003D3800u) return 4;
        RUN_EXPECTING_FATAL(d3d8_set_render_target(steps[i][2], steps[i][3]));
        if (fatal_seen) {
            fatal_step = i;
            break;
        }
    }

    const uint32_t fatal_address_out = fatal_seen ? fatal_address : 0u;
    const uint32_t roll_count = 0u;
    if (fwrite(results, sizeof(uint32_t), step_count, out) != step_count ||
        fwrite(&fatal_step, 4u, 1u, out) != 1u || fwrite(&fatal_address_out, 4u, 1u, out) != 1u ||
        fwrite(&roll_count, 4u, 1u, out) != 1u) return 5;
    for (uint32_t i = 0u; i < window_count; i++) {
        if (fwrite(kernel_guest_at(windows[i][0], windows[i][1]), windows[i][1], 1u, out) != 1u)
            return 5;
    }
    for (uint32_t i = 0u; i < window_count; i++) free(bytes[i]);
    fclose(in);
    fclose(out);
    environment_end();
    return 0;
}
