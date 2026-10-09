/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Replay runner for InsertFence 0x003D3630, BlockUntilFence 0x003D34B0 (T368) and BlockUntilIdle 0x003D34A0
 * (T549), dispatched through the real
 * D3D8 table. Input: ring base, ring bytes, control block address, entry, argument count, argument
 * (the entry's stdcall frame), the 0x30000-byte D3D region, the ring and the
 * 0x1000-byte control block. Output: the result (0xDEADFA7A after a fatal), the region, the ring
 * and the control block.
 */
#include "d3d8_gpu.h"
#include "test_d3d8_support.h"

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *in = fopen(argv[1], "rb");
    FILE *out = fopen(argv[2], "wb");
    uint32_t header[6];
    if (!in || !out || fread(header, sizeof(header), 1u, in) != 1u) return 2;
    environment_begin(KERNEL_AV_PACK_HDTV);
    static const d3d8_surface_entry rows[] = {
        {0x003D3630u, NULL, 1u}, {0x003D34B0u, NULL, 1u}, {0x003D67B0u, NULL, 1u},
        {0x003D34A0u, NULL, 1u}};
    d3d8_hle_shutdown();
    if (!d3d8_hle_init(rows, 4u)) return 2;
    (void)d3d8_gpu_register();
    map_fixed(header[0], header[1]);
    map_fixed(header[2], 0x1000u);
    for (uint32_t i = 0u; i < D3D_REGION_BYTES / 4u; i++) {
        uint32_t value;
        if (fread(&value, sizeof(value), 1u, in) != 1u) return 2;
        store(D3D_REGION_BASE + i * 4u, value);
    }
    for (uint32_t i = 0u; i < header[1] / 4u; i++) {
        uint32_t value;
        if (fread(&value, sizeof(value), 1u, in) != 1u) return 2;
        store(header[0] + i * 4u, value);
    }
    for (uint32_t i = 0u; i < 0x400u; i++) {
        uint32_t value;
        if (fread(&value, sizeof(value), 1u, in) != 1u) return 2;
        store(header[2] + i * 4u, value);
    }
    fclose(in);
    uint32_t result = 0xDEADFA7Au;
    const uint32_t argument[1] = {header[5]};
    RUN_EXPECTING_FATAL(result = call_stdcall(header[3], argument, header[4]));
    if (fatal_seen) {
        result = 0xDEADFA7Au;
        fprintf(stderr, "native fatal at %#x: %s\n", (unsigned)fatal_address, fatal_text);
    }
    if (fwrite(&result, 4u, 1u, out) != 1u) return 2;
    for (uint32_t i = 0u; i < D3D_REGION_BYTES / 4u; i++) {
        const uint32_t value = load(D3D_REGION_BASE + i * 4u);
        if (fwrite(&value, 4u, 1u, out) != 1u) return 2;
    }
    for (uint32_t i = 0u; i < header[1] / 4u; i++) {
        const uint32_t value = load(header[0] + i * 4u);
        if (fwrite(&value, 4u, 1u, out) != 1u) return 2;
    }
    for (uint32_t i = 0u; i < 0x400u; i++) {
        const uint32_t value = load(header[2] + i * 4u);
        if (fwrite(&value, 4u, 1u, out) != 1u) return 2;
    }
    fclose(out);
    environment_end();
    return 0;
}
