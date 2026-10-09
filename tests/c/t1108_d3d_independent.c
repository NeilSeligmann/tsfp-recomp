/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Independent snapshot driver: frozen production, no renderer or live GPU. */
#include "test_d3d8_support.h"
#include "d3d8_method_packet.h"
#include "d3d8_state.h"

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *in = fopen(argv[1], "rb");
    FILE *out = fopen(argv[2], "wb");
    uint32_t h[4]; /* API, index, value, mapped ring bytes */
    static uint8_t d3d[D3D_REGION_BYTES], ring[0x2000];
    if (!in || !out || fread(h, sizeof(h), 1, in) != 1 ||
        fread(d3d, sizeof(d3d), 1, in) != 1 || h[3] > sizeof(ring) ||
        fread(ring, h[3], 1, in) != 1) return 3;
    environment_begin(KERNEL_AV_PACK_HDTV);
    memcpy(kernel_guest_at(D3D_REGION_BASE, sizeof(d3d)), d3d, sizeof(d3d));
    map_fixed(0x00D00000u, h[3]);
    memcpy(kernel_guest_at(0x00D00000u, h[3]), ring, h[3]);
    const d3d8_surface_entry row = {h[0], NULL, 1u};
    if (!d3d8_hle_init(&row, 1u)) return 4;
    if (h[0] == 0x003D52F0u) (void)d3d8_method_packet_register();
    else if (h[0] == 0x003D7010u) (void)d3d8_state_register();
    else return 4;
    volatile uint32_t result = 0xDEADBEEFu;
    const uint32_t args[2] = {h[1], h[2]};
    RUN_EXPECTING_FATAL(result = call_stdcall(h[0], h[0] == 0x003D52F0u ? args : args + 1,
                                            h[0] == 0x003D52F0u ? 2u : 1u));
    const uint32_t receipt[3] = {fatal_seen, fatal_seen ? fatal_address : 0u, result};
    if (fwrite(receipt, sizeof(receipt), 1, out) != 1 ||
        fwrite(kernel_guest_at(D3D_REGION_BASE, sizeof(d3d)), sizeof(d3d), 1, out) != 1 ||
        fwrite(kernel_guest_at(0x00D00000u, h[3]), h[3], 1, out) != 1) return 5;
    fclose(in);
    fclose(out);
    environment_end();
    return 0;
}
