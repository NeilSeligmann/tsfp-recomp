/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Replay runner for Clear, 0x003D5EB0 (T440). Like d3d8_shader_refill_oracle_runner.c it reports every
 * roll-over cursor, the whole ring, the D3D region and the 0x5000-byte window at 0x00D00000 (surface
 * headers and rectangles), so the commands, the cursor and the sites of the refills can be compared
 * with the original.
 *
 * Input: Count, pRects, Flags, Color, Z, Stencil, ring base, ring bytes, then the 0x30000-byte D3D
 * region, the window, the 0x200-byte constants at 0x00475C00, the 0x100-byte constants at 0x004A1B00
 * and the ring. Output: the result (0xDEADFA7A after a fatal), the roll-over count, the cursor at each,
 * the ring, the region and the window.
 */
#include "d3d8_frame.h"
#include "test_d3d8_support.h"

#define MAX_ROLLS 64u
#define WINDOW_BASE 0x00D00000u
#define WINDOW_WORDS 0x1400u
#define CONSTANTS_A 0x00475C00u
#define CONSTANTS_A_WORDS 0x80u
#define CONSTANTS_B 0x004A1B00u
#define CONSTANTS_B_WORDS 0x40u

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

static int write_words(FILE *out, uint32_t base, uint32_t count)
{
    for (uint32_t i = 0u; i < count; i++) {
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
    uint32_t header[8];
    if (!in || !out || fread(header, sizeof(header), 1u, in) != 1u) return 2;
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(header[6], header[7]);
    map_fixed(WINDOW_BASE, WINDOW_WORDS * 4u);
    map_fixed(0x004A1000u, 0x1000u);
    if (!read_words(in, D3D_REGION_BASE, D3D_REGION_BYTES / 4u) ||
        !read_words(in, WINDOW_BASE, WINDOW_WORDS) || !read_words(in, CONSTANTS_A, CONSTANTS_A_WORDS) ||
        !read_words(in, CONSTANTS_B, CONSTANTS_B_WORDS) || !read_words(in, header[6], header[7] / 4u))
        return 2;
    fclose(in);
    d3d8_pushbuffer_set_consumer(log_roll, NULL);
    uint32_t result = 0u;
    const d3d8_clear_call call = {header[0], header[1], header[2], header[3], header[4], header[5]};
    RUN_EXPECTING_FATAL(d3d8_clear(&call));
    if (fatal_seen) {
        result = 0xDEADFA7Au;
        fprintf(stderr, "native fatal at %#x: %s\n", (unsigned)fatal_address, fatal_text);
    }
    const uint32_t logged = roll_count < MAX_ROLLS ? roll_count : MAX_ROLLS;
    if (fwrite(&result, 4u, 1u, out) != 1u || fwrite(&roll_count, 4u, 1u, out) != 1u ||
        fwrite(roll_cursors, 4u, logged, out) != logged)
        return 2;
    if (!write_words(out, header[6], header[7] / 4u) ||
        !write_words(out, D3D_REGION_BASE, D3D_REGION_BYTES / 4u) ||
        !write_words(out, WINDOW_BASE, WINDOW_WORDS))
        return 2;
    fclose(out);
    environment_end();
    return 0;
}
