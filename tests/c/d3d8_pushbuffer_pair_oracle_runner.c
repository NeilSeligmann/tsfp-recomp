/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Replay runner for the pushbuffer pair emitter (0x003D6C90): reads the ORIGINAL guest region
 * and pushbuffer window the Python side captured, dispatches one register-argument call
 * through the real D3D8 table, and writes the result, the region and the window back for
 * byte-for-byte comparison: the emitted header/value pair, the advanced cursor at device+0
 * and the advanced-cursor return value all have to agree with the original.
 *
 * Input: header, value, then the 0x30000-byte D3D region, then the 0x2000-byte pushbuffer
 * window at 0x00D00000 (cursor, limit, base and end live inside the region at device+0,
 * device+4, device+0x24 and device+0x28). Output: the 32-bit result, the region, the window.
 * A fatal is reported as the sentinel result 0xDEADFA7A.
 */
#include "test_d3d8_support.h"

#define WINDOW_BASE 0x00D00000u
#define WINDOW_BYTES 0x2000u

int main(int argc, char **argv)
{
    if (argc != 3) {
        return 2;
    }
    FILE *in = fopen(argv[1], "rb");
    FILE *out = fopen(argv[2], "wb");
    uint32_t header[2];
    static uint8_t region[0x30000];
    static uint8_t window[WINDOW_BYTES];
    if (!in || !out || fread(header, sizeof(header), 1, in) != 1 ||
        fread(region, sizeof(region), 1, in) != 1 ||
        fread(window, sizeof(window), 1, in) != 1) {
        return 3;
    }

    environment_begin(KERNEL_AV_PACK_HDTV);
    (void)d3d8_device_register();
    map_fixed(WINDOW_BASE, WINDOW_BYTES);
    memcpy(kernel_guest_at(D3D_REGION_BASE, sizeof(region)), region, sizeof(region));
    memcpy(kernel_guest_at(WINDOW_BASE, WINDOW_BYTES), window, WINDOW_BYTES);

    uint32_t result = 0xDEADFA7Au;
    RUN_EXPECTING_FATAL(result = call_fastcall(0x003D6C90u, header[0], header[1]));
    if (fatal_seen) {
        result = 0xDEADFA7Au;
    }
    if (fwrite(&result, 4, 1, out) != 1 ||
        fwrite(kernel_guest_at(D3D_REGION_BASE, sizeof(region)), sizeof(region), 1, out) != 1 ||
        fwrite(kernel_guest_at(WINDOW_BASE, WINDOW_BYTES), WINDOW_BYTES, 1, out) != 1) {
        return 4;
    }
    fclose(in);
    fclose(out);
    environment_end();
    return 0;
}
