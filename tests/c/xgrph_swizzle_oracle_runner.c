/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "xgrph_swizzle.h"

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *input = fopen(argv[1], "rb"), *output = fopen(argv[2], "wb");
    if (input == NULL || output == NULL) return 2;
    uint32_t args[8], rectangle[4];
    uint8_t source[16416], destination[16416];
    if (fread(args, sizeof(args), 1u, input) != 1u ||
        fread(rectangle, sizeof(rectangle), 1u, input) != 1u ||
        fread(source, sizeof(source), 1u, input) != 1u ||
        fread(destination, sizeof(destination), 1u, input) != 1u) return 2;
    fclose(input);
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed((args[0] - 16u) & ~0xFFFu, 0x6000u);
    map_fixed((args[3] - 16u) & ~0xFFFu, 0x6000u);
    if (args[2] != 0u) {
        map_fixed(args[2] & ~0xFFFu, 0x2000u);
        memcpy(kernel_guest_at(args[2], sizeof(rectangle)), rectangle, sizeof(rectangle));
    }
    memcpy(kernel_guest_at(args[0] - 16u, sizeof(source)), source, sizeof(source));
    memcpy(kernel_guest_at(args[3] - 16u, sizeof(destination)), destination, sizeof(destination));
    const uint32_t result = xgrph_swizzle_rect(args[0], args[1], args[2], args[3],
        args[4], args[5], args[6], args[7]);
    if (fwrite(&result, sizeof(result), 1u, output) != 1u ||
        fwrite(kernel_guest_at(args[0] - 16u, sizeof(source)), sizeof(source), 1u, output) != 1u ||
        fwrite(kernel_guest_at(args[3] - 16u, sizeof(destination)), sizeof(destination), 1u, output) != 1u)
        return 2;
    fclose(output);
    environment_end();
    return 0;
}
