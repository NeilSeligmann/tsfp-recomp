/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_cube_surface.h"
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *input = fopen(argv[1], "rb"), *output = fopen(argv[2], "wb");
    if (!input || !output) return 2;
    uint32_t arguments[2];
    uint8_t initial[32], metadata[64], state[D3D_REGION_BYTES];
    if (fread(arguments, sizeof(arguments), 1u, input) != 1u ||
        fread(initial, sizeof(initial), 1u, input) != 1u ||
        fread(metadata, sizeof(metadata), 1u, input) != 1u ||
        fread(state, sizeof(state), 1u, input) != 1u) return 2;
    fclose(input);
    environment_begin(KERNEL_AV_PACK_HDTV);
    d3d8_cube_surface_reset();
    memcpy(kernel_guest_at(D3D_REGION_BASE, sizeof(state)), state, sizeof(state));
    map_fixed(arguments[0] & ~0xFFFu, 0x2000u);
    memcpy(kernel_guest_at(arguments[0], sizeof(initial)), initial, sizeof(initial));
    memcpy(kernel_guest_at(0x003E1828u, sizeof(metadata)), metadata, sizeof(metadata));
    const uint32_t result = d3d8_get_surface_level2(arguments[0], arguments[1]);
    if (fwrite(&result, sizeof(result), 1u, output) != 1u || result == 0u ||
        fwrite(kernel_guest_at(result, 32u), 32u, 1u, output) != 1u ||
        fwrite(kernel_guest_at(arguments[0], sizeof(initial)), sizeof(initial), 1u, output) != 1u ||
        fwrite(kernel_guest_at(D3D_REGION_BASE, sizeof(state)), sizeof(state), 1u, output) != 1u) return 2;
    fclose(output);
    d3d8_cube_surface_reset();
    environment_end();
    return 0;
}
