/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Replay runner for the vertex shader binders 0x003D5630 and 0x003D5A50 near a pushbuffer limit
 * (T368). Like d3d8_draw_refill_oracle_runner.c it reports every roll-over cursor, the whole ring and
 * the D3D region, so the sites and arguments of the refills can be compared with the original.
 *
 * Input: entry, argument 0, argument 1, ring base, ring bytes, the 0x30000-byte region, the 4096-byte
 * window at 0x00D03000 (the declaration source), then the ring. Output: the result (0xDEADFA7A after
 * a fatal), the roll-over count, the cursor at each, the ring, the region and the window.
 */
#include "d3d8_shader.h"
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
    map_fixed(header[3], header[4]);
    map_fixed(0x00D00000u, 0x5000u);
    if (!read_words(in, D3D_REGION_BASE, D3D_REGION_BYTES / 4u) ||
        !read_words(in, 0x00D03000u, 1024u) || !read_words(in, header[3], header[4] / 4u))
        return 2;
    fclose(in);
    d3d8_pushbuffer_set_consumer(log_roll, NULL);
    uint32_t result = 0xDEADFA7Au;
    RUN_EXPECTING_FATAL(result = header[0] == 0x003D5630u ? d3d8_set_vertex_shader(header[1], header[2])
                                                            : d3d8_set_vertex_shader_handle(header[1], header[2]));
    if (fatal_seen) {
        result = 0xDEADFA7Au;
        fprintf(stderr, "native fatal at %#x: %s\n", (unsigned)fatal_address, fatal_text);
    }
    const uint32_t logged = roll_count < MAX_ROLLS ? roll_count : MAX_ROLLS;
    if (fwrite(&result, 4u, 1u, out) != 1u || fwrite(&roll_count, 4u, 1u, out) != 1u ||
        fwrite(roll_cursors, 4u, logged, out) != logged) return 2;
    for (uint32_t i = 0u; i < header[4] / 4u; i++) {
        const uint32_t value = load(header[3] + i * 4u);
        if (fwrite(&value, 4u, 1u, out) != 1u) return 2;
    }
    for (uint32_t i = 0u; i < D3D_REGION_BYTES / 4u; i++) {
        const uint32_t value = load(D3D_REGION_BASE + i * 4u);
        if (fwrite(&value, 4u, 1u, out) != 1u) return 2;
    }
    for (uint32_t i = 0u; i < 1024u; i++) {
        const uint32_t value = load(0x00D03000u + i * 4u);
        if (fwrite(&value, 4u, 1u, out) != 1u) return 2;
    }
    fclose(out);
    environment_end();
    return 0;
}
