/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_callbacks.h"
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *input = fopen(argv[1], "rb"), *output = fopen(argv[2], "wb");
    uint32_t callback;
    uint8_t state[D3D_REGION_BYTES];
    if (!input || !output || fread(&callback, 4u, 1u, input) != 1u ||
        fread(state, sizeof(state), 1u, input) != 1u) return 2;
    fclose(input);
    environment_begin(KERNEL_AV_PACK_HDTV);
    memcpy(kernel_guest_at(D3D_REGION_BASE, sizeof(state)), state, sizeof(state));
    const uint32_t result = d3d8_set_vertical_blank_callback(callback);
    if (fwrite(&result, 4u, 1u, output) != 1u ||
        fwrite(kernel_guest_at(D3D_REGION_BASE, sizeof(state)), sizeof(state), 1u, output) != 1u)
        return 2;
    fclose(output);
    environment_end();
    return 0;
}
