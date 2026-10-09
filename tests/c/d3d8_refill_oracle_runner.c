/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Replay runner for the pushbuffer refill (T368, T391): reads the ORIGINAL guest state the Python side
 * captured, attaches the GPU module's consumer and refill tail, runs the native reservation
 * (d3d8_pushbuffer_begin for 0x003D6B20, or the sized d3d8_pushbuffer_reserve for 0x003D6B30) and writes
 * back the returned cursor, the D3D region, the whole ring, the control block page and the second-history
 * page, so the refill's fence insertion and kick are compared with the original's.
 *
 * Input: ring base, ring bytes, dwords (0 selects the unsized 0x003D6B20 form), the DMA put the original's
 * GPU holds (its GET register), the control block page and the second-history page (both page aligned,
 * mapped at the original's addresses), then the 0x30000-byte region, the ring, the control page and the
 * history page. Output: the cursor (0xDEADFA7A after a fatal), then the same four areas.
 */
#include "d3d8_gpu.h"
#include "test_d3d8_support.h"

#define PAGE_BYTES 0x1000u

static int copy_in(FILE *in, uint32_t base, uint32_t bytes)
{
    for (uint32_t i = 0u; i < bytes / 4u; i++) {
        uint32_t value;
        if (fread(&value, sizeof(value), 1u, in) != 1u) return 0;
        store(base + i * 4u, value);
    }
    return 1;
}

static int copy_out(FILE *out, uint32_t base, uint32_t bytes)
{
    for (uint32_t i = 0u; i < bytes / 4u; i++) {
        const uint32_t value = load(base + i * 4u);
        if (fwrite(&value, 4u, 1u, out) != 1u) return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *in = fopen(argv[1], "rb");
    FILE *out = fopen(argv[2], "wb");
    uint32_t header[6];
    if (!in || !out || fread(header, sizeof(header), 1u, in) != 1u) return 2;
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(header[0], header[1]);
    map_fixed(header[4], PAGE_BYTES);
    map_fixed(header[5], PAGE_BYTES);
    if (!copy_in(in, D3D_REGION_BASE, D3D_REGION_BYTES) || !copy_in(in, header[0], header[1]) ||
        !copy_in(in, header[4], PAGE_BYTES) || !copy_in(in, header[5], PAGE_BYTES)) {
        return 2;
    }
    fclose(in);
    d3d8_gpu_attach();
    /* The consumer would record [segment start, cursor), and the transplanted ring has no segment start: the
     * comparison is of guest memory, not of the recording. */
    d3d8_pushbuffer_set_consumer(NULL, NULL);
    d3d8_pushbuffer_set_put(0x80000000u | header[3]);
    uint32_t result = 0xDEADFA7Au;
    RUN_EXPECTING_FATAL(result = header[2] == 0u ? d3d8_pushbuffer_begin()
                                                 : d3d8_pushbuffer_reserve(header[2]));
    if (fatal_seen) {
        result = 0xDEADFA7Au;
        fprintf(stderr, "native fatal at %#x: %s\n", (unsigned)fatal_address, fatal_text);
    }
    if (fwrite(&result, 4u, 1u, out) != 1u) return 2;
    if (!copy_out(out, D3D_REGION_BASE, D3D_REGION_BYTES) || !copy_out(out, header[0], header[1]) ||
        !copy_out(out, header[4], PAGE_BYTES) || !copy_out(out, header[5], PAGE_BYTES)) {
        return 2;
    }
    fclose(out);
    environment_end();
    return 0;
}
