/* SPDX-License-Identifier: GPL-3.0-or-later
 * Replay the T1066 0x003D52F0 handler over the exact original D3D bytes and ring snapshot.
 */
#include "test_d3d8_support.h"

#include "d3d8_method_packet.h"

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *in = fopen(argv[1], "rb");
    FILE *out = fopen(argv[2], "wb");
    uint32_t header[2];
    static uint8_t d3d_region[D3D_REGION_BYTES];
    static uint8_t ring[0x2000];
    if (!in || !out || fread(header, sizeof(header), 1, in) != 1 ||
        fread(d3d_region, sizeof(d3d_region), 1, in) != 1 ||
        fread(ring, sizeof(ring), 1, in) != 1) return 3;

    environment_begin(KERNEL_AV_PACK_HDTV);
    memcpy(kernel_guest_at(D3D_REGION_BASE, sizeof(d3d_region)), d3d_region,
           sizeof(d3d_region));
    map_fixed(0x00D00000u, sizeof(ring));
    memcpy(kernel_guest_at(0x00D00000u, sizeof(ring)), ring, sizeof(ring));
    const d3d8_surface_entry row = {0x003D52F0u, NULL, 2u};
    if (!d3d8_hle_init(&row, 1u) || d3d8_device_register() != 1u) return 4;
    const uint32_t args[2] = {header[0], header[1]};
    const uint32_t result = call_stdcall(0x003D52F0u, args, 2u);
    if (fwrite(&result, sizeof(result), 1, out) != 1 ||
        fwrite(kernel_guest_at(D3D_REGION_BASE, sizeof(d3d_region)), sizeof(d3d_region), 1,
               out) != 1 ||
        fwrite(kernel_guest_at(0x00D00000u, sizeof(ring)), sizeof(ring), 1, out) != 1) return 5;
    fclose(in);
    fclose(out);
    environment_end();
    return 0;
}
