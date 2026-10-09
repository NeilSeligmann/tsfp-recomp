/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Replay runner for DrawVertices (0x003D4FB0) with a pushbuffer near its limit (T368). It reads the
 * ORIGINAL guest state the Python side captured, runs the native draw, and writes back the result,
 * every pushbuffer roll-over the draw took (the cursor at each), the whole ring and the D3D region
 * for byte-for-byte comparison.
 *
 * Input: ring base, ring bytes, primitive, first, count, the 0x30000-byte D3D region, 64 words at
 * 0x00A00000, 60 words at 0x00A00100, 16 words at 0x005496E0, 8 words at 0x005496A0, then the ring.
 * Output: the result (0xDEADFA7A when the draw stopped on a fatal), the roll-over count, the cursor
 * at each roll-over, the ring and the region.
 */
#include "test_d3d8_support.h"

#define MAX_ROLLS 64u

static uint32_t roll_cursors[MAX_ROLLS];
static uint32_t roll_count;

static void log_roll(void *context, uint32_t begin, uint32_t end)
{
    (void)context;
    (void)begin;
    if (roll_count < MAX_ROLLS) roll_cursors[roll_count] = end;
    roll_count++;
}

static int read_words(FILE *in, uint32_t base, uint32_t count)
{
    for (uint32_t i = 0u; i < count; i++) {
        uint32_t value;
        if (fread(&value, sizeof(value), 1u, in) != 1u) return 0;
        store(base + i * 4u, value);
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *in = fopen(argv[1], "rb");
    FILE *out = fopen(argv[2], "wb");
    uint32_t header[5];
    if (!in || !out || fread(header, sizeof(header), 1u, in) != 1u) return 2;
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(header[0], header[1]);
    map_fixed(0x00A00000u, 0x1000u);
    map_fixed(0x00540000u, 0x10000u);
    if (!read_words(in, D3D_REGION_BASE, D3D_REGION_BYTES / 4u) || !read_words(in, 0x00A00000u, 64u) ||
        !read_words(in, 0x00A00100u, 60u) || !read_words(in, 0x005496E0u, 16u) ||
        !read_words(in, 0x005496A0u, 8u) || !read_words(in, header[0], header[1] / 4u))
        return 2;
    fclose(in);
    d3d8_pushbuffer_set_consumer(log_roll, NULL);
    uint32_t result = 0xDEADFA7Au;
    RUN_EXPECTING_FATAL(result = d3d8_draw_vertices(header[2], header[3], header[4]));
    if (fatal_seen) {
        result = 0xDEADFA7Au;
        fprintf(stderr, "native fatal at %#x: %s\n", (unsigned)fatal_address, fatal_text);
    }
    const uint32_t logged = roll_count < MAX_ROLLS ? roll_count : MAX_ROLLS;
    if (fwrite(&result, 4u, 1u, out) != 1u || fwrite(&roll_count, 4u, 1u, out) != 1u ||
        fwrite(roll_cursors, 4u, logged, out) != logged) return 2;
    for (uint32_t i = 0u; i < header[1] / 4u; i++) {
        const uint32_t value = load(header[0] + i * 4u);
        if (fwrite(&value, 4u, 1u, out) != 1u) return 2;
    }
    for (uint32_t i = 0u; i < D3D_REGION_BYTES / 4u; i++) {
        const uint32_t value = load(D3D_REGION_BASE + i * 4u);
        if (fwrite(&value, 4u, 1u, out) != 1u) return 2;
    }
    fclose(out);
    environment_end();
    return 0;
}
