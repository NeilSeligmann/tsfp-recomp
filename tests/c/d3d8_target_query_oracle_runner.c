/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_target_query.h"
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    uint32_t entry; uint8_t state[0x30000], headers[0x1000], stream[64];
    if (!in || !out || fread(&entry, 4, 1, in) != 1 ||
        fread(state, sizeof(state), 1, in) != 1 ||
        fread(headers, sizeof(headers), 1, in) != 1 || fread(stream, 64, 1, in) != 1) return 3;
    environment_begin(KERNEL_AV_PACK_HDTV); map_fixed(0x00D00000u, 0x2000u);
    memcpy(kernel_guest_at(0x003D0000u, sizeof(state)), state, sizeof(state));
    memcpy(kernel_guest_at(0x00D00000u, sizeof(headers)), headers, sizeof(headers));
    memcpy(kernel_guest_at(0x00D01000u, 64u), stream, 64u);
    const uint32_t result = entry == 0x003D3E00u ? d3d8_get_render_target2() : d3d8_get_depth_stencil_surface2();
    if (fwrite(&result, 4, 1, out) != 1 ||
        fwrite(kernel_guest_at(0x003D0000u, sizeof(state)), sizeof(state), 1, out) != 1 ||
        fwrite(kernel_guest_at(0x00D00000u, sizeof(headers)), sizeof(headers), 1, out) != 1 ||
        fwrite(kernel_guest_at(0x00D01000u, 64u), 64u, 1, out) != 1) return 4;
    fclose(in); fclose(out); environment_end(); return 0;
}
