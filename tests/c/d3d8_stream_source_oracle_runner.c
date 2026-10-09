/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Replay runner for SetStreamSource (0x003D58F0, d3d8_set_stream_source): reads the ORIGINAL
 * guest region the Python side captured, performs one call, writes the return value and the
 * whole region back. The original emits nothing for this entry (measured), so the comparison
 * is whole-region byte-for-byte against the original's own after-state: the stream table row
 * at 0x003E2BA8, the header reference count, the released buffer's fence word and the dirty
 * mask all have to agree exactly.
 *
 * Input: stream, header, stride, then the 0x30000-byte D3D region. Output: the 32-bit result
 * then the region. A fatal is reported as the sentinel result 0xDEADFA7A.
 */
#include "test_d3d8_support.h"

#include "d3d8_resource.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        return 2;
    }
    FILE *in = fopen(argv[1], "rb");
    FILE *out = fopen(argv[2], "wb");
    uint32_t header[3];
    static uint8_t region[0x30000];
    if (!in || !out || fread(header, sizeof(header), 1, in) != 1 ||
        fread(region, sizeof(region), 1, in) != 1) {
        return 3;
    }

    environment_begin(KERNEL_AV_PACK_HDTV);
    memcpy(kernel_guest_at(D3D_REGION_BASE, sizeof(region)), region, sizeof(region));

    uint32_t result = 0xDEADFA7Au;
    RUN_EXPECTING_FATAL(result = d3d8_set_stream_source(header[0], header[1], header[2]));
    if (fatal_seen) {
        result = 0xDEADFA7Au;
    }
    if (fwrite(&result, 4, 1, out) != 1 ||
        fwrite(kernel_guest_at(D3D_REGION_BASE, sizeof(region)), sizeof(region), 1, out) != 1) {
        return 4;
    }
    fclose(in);
    fclose(out);
    environment_end();
    return 0;
}
