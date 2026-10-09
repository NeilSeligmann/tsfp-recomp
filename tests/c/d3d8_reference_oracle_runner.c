/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_reference.h"
#include "d3d8_cube_surface.h"
#define ROOT 0x00665504u
#define BYTES 4096u
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *input = fopen(argv[1], "rb"), *output = fopen(argv[2], "wb");
    if (!input || !output) return 2;
    uint32_t mode;
    uint8_t state[BYTES];
    if (fread(&mode, sizeof(mode), 1u, input) != 1u ||
        fread(state, sizeof(state), 1u, input) != 1u) return 2;
    fclose(input);
    environment_begin(KERNEL_AV_PACK_HDTV);
    d3d8_cube_surface_reset();
    map_fixed(0x00665000u, 0x2000u);
    memcpy(kernel_guest_at(ROOT, BYTES), state, BYTES);
    store(0x3E2BA4u, 0u);
    store_byte(0x3E182Fu, 0xA1u);
    uint32_t header = 0u, before = 0u, after = 0u;
    uint32_t result;
    if (mode == 3u) {
        header = d3d8_get_cube_map_surface2(ROOT, 0u, 0u);
        if (header == 0u) return 2;
        before = (uint32_t)d3d8_cube_surface_owned(header);
        result = d3d8_reference_release(header);
        after = (uint32_t)d3d8_cube_surface_owned(header);
    } else if (mode == 0u) result = d3d8_reference_add_ref(ROOT);
    else if (mode == 1u) result = d3d8_reference_release(ROOT);
    else { d3d8_reference_release_binding(ROOT); result = 0u; }
    const uint32_t summary[] = {header, result, before, after};
    if (fwrite(summary, sizeof(summary), 1u, output) != 1u ||
        fwrite(kernel_guest_at(ROOT, BYTES), BYTES, 1u, output) != 1u) return 2;
    fclose(output);
    d3d8_cube_surface_reset();
    environment_end();
    return 0;
}
