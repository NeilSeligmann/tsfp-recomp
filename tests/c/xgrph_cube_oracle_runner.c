/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "xgrph_texture.h"
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *input = fopen(argv[1], "rb"), *output = fopen(argv[2], "wb");
    if (input == NULL || output == NULL) return 2;
    uint32_t args[8];
    uint8_t metadata[64], initial[32];
    if (fread(args, sizeof(args), 1u, input) != 1u ||
        fread(metadata, sizeof(metadata), 1u, input) != 1u ||
        fread(initial, sizeof(initial), 1u, input) != 1u) return 2;
    fclose(input);
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(0x00402000u, 0x1000u);
    map_fixed(args[5] & ~0xFFFu, 0x2000u);
    memcpy(kernel_guest_at(0x00402D70u, sizeof(metadata)), metadata, sizeof(metadata));
    memcpy(kernel_guest_at(args[5], sizeof(initial)), initial, sizeof(initial));
    const uint32_t result = xgrph_set_cube_texture_header(args[0], args[1], args[2], args[3],
                                                         args[4], args[5], args[6], args[7]);
    if (fwrite(&result, sizeof(result), 1u, output) != 1u ||
        fwrite(kernel_guest_at(args[5], sizeof(initial)), sizeof(initial), 1u, output) != 1u) return 2;
    fclose(output);
    environment_end();
    return 0;
}
