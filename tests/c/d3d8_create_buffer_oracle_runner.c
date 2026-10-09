/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Replay runner for CreateBuffer (0x003D4EE0, d3d8_create_buffer): one call with the length
 * from argv[1], then the header words and the registered data block's shape are written for
 * the Python side to compare with the ORIGINAL's own allocations.
 *
 * Output (7 dwords): header, header Common, Data, Lock, registered virtual of Data,
 * data block size, header block size. A fatal is reported as header 0xDEADFA7A.
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
    uint32_t length = 0u;
    if (!in || !out || fread(&length, 4, 1, in) != 1) {
        return 3;
    }
    environment_begin(KERNEL_AV_PACK_HDTV);
    uint32_t words[7] = {0xDEADFA7Au, 0u, 0u, 0u, 0u, 0u, D3D8_BUFFER_HEADER_BYTES};
    uint32_t header = 0u;
    RUN_EXPECTING_FATAL(header = d3d8_create_buffer(length));
    if (!fatal_seen) {
        words[0] = header;
        words[1] = load(header);
        words[2] = load(header + 4u);
        words[3] = load(header + 8u);
        words[4] = d3d8_resource_virtual_of_physical(words[2]);
        const guest_region *region = guest_region_at(words[4]);
        words[5] = region != NULL ? (uint32_t)region->size : 0u;
    }
    if (fwrite(words, sizeof(words), 1, out) != 1) {
        return 4;
    }
    fclose(in);
    fclose(out);
    environment_end();
    return 0;
}
