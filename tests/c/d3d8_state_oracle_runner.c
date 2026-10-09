/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Replay runner for the d3d8_state.c family plus the one-store dispatch entries 0x003D9000
 * and 0x003D3530 (the vertical blank callback setter): reads the
 * ORIGINAL guest region the Python side captured, dispatches ONE call through the real D3D8
 * table (the same path xdk_thunk.c takes), and writes the return value and the whole region
 * back for byte-for-byte comparison against the original's measured after-state.
 *
 * Addresses this runner serves (all dispatched, never called directly):
 *   0x003D7060 0x003D7F70 0x003D8010 0x003D7150 0x003D80B0 0x003D8190 0x003D81B0
 *   0x003D7EE0 0x003D81F0 0x003D72A0 0x003D5AF0 0x003D5670 0x003D9000 0x003D3530
 *
 * Input: address, use_registers, ecx, edx, argc, args[2], the two .rdata floats the state
 * 0x95 helper reads (2^32 at 0x00475CCC and 0.25 at 0x00475D4C, read from the real image so
 * the C side consumes oracle-derived bytes), then the 0x30000-byte D3D region. Output: the
 * 32-bit dispatch result then the region. A fatal from the handler is reported as the
 * sentinel result 0xDEADFA7A, which no expected value equals. The pushbuffer window 0xD00000+0x8000 is
 * mapped (the handlers emit the original's packets there) and written after the region.
 */
#include "test_d3d8_support.h"

/* The .rdata window the handlers read: the render state dirty table (0x004758E8), the header table
 * (0x00475B08) and the two floats, taken from the real image. */
#define RDATA_BASE 0x004758E8u
#define RDATA_BYTES 0x470u

int main(int argc, char **argv)
{
    if (argc != 3) {
        return 2;
    }
    FILE *in = fopen(argv[1], "rb");
    FILE *out = fopen(argv[2], "wb");
    uint32_t header[9];
    static uint8_t region[0x30000];
    static uint8_t rdata[RDATA_BYTES];
    if (!in || !out || fread(header, sizeof(header), 1, in) != 1 ||
        fread(region, sizeof(region), 1, in) != 1 || fread(rdata, sizeof(rdata), 1, in) != 1) {
        return 3;
    }
    const uint32_t address = header[0];
    const uint32_t use_registers = header[1];
    const uint32_t ecx = header[2];
    const uint32_t edx = header[3];
    const uint32_t count = header[4];
    if (count > 2u) {
        return 5;
    }

    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(0xD00000u, 0x8000u);
    (void)d3d8_device_register();
    store(GUEST_FLOAT_TWO_POW_32, header[7]);
    store(GUEST_FLOAT_QUARTER, header[8]);
    memcpy(kernel_guest_at(RDATA_BASE, sizeof(rdata)), rdata, sizeof(rdata));
    memcpy(kernel_guest_at(D3D_REGION_BASE, sizeof(region)), region, sizeof(region));

    uint32_t result = 0xDEADFA7Au;
    RUN_EXPECTING_FATAL(result = use_registers != 0u ? call_fastcall(address, ecx, edx)
                                                     : call_stdcall(address, &header[5], count));
    if (fatal_seen) {
        result = 0xDEADFA7Au;
    }
    if (fwrite(&result, 4, 1, out) != 1 ||
        fwrite(kernel_guest_at(D3D_REGION_BASE, sizeof(region)), sizeof(region), 1, out) != 1 ||
        fwrite(kernel_guest_at(0xD00000u, 0x8000u), 0x8000u, 1, out) != 1) {
        return 4;
    }
    fclose(in);
    fclose(out);
    environment_end();
    return 0;
}
