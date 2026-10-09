/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Replay runner for BeginPush (0x003D6660) and EndPush (0x003D6680) (T854). It reads the ORIGINAL guest state the
 * Python side captured, dispatches BeginPush(count) through the real D3D8 table, writes the block's words at the pointer
 * it returns, dispatches EndPush(pointer + 4 * end_dwords) and writes back both results, every roll-over the pair took
 * (the cursor at each), the whole ring and the D3D region for byte-for-byte comparison.
 *
 * Input: ring base, ring bytes, count, block dwords, end dwords, the 0x30000-byte D3D region, 64 words at 0x00A00000,
 * 8 words at 0x005496A0, the ring, then the block words. Output: the BeginPush result, the EndPush result (each
 * 0xDEADFA7A when the call stopped on a fatal), the roll-over count, the cursor at each roll-over, the ring and the
 * region.
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
    (void)d3d8_device_register();
    map_fixed(header[0], header[1]);
    map_fixed(0x00A00000u, 0x1000u);
    map_fixed(0x00540000u, 0x10000u);
    static uint32_t block[0x10000];
    if (header[3] > 0x10000u || !read_words(in, D3D_REGION_BASE, D3D_REGION_BYTES / 4u) ||
        !read_words(in, 0x00A00000u, 64u) || !read_words(in, 0x005496A0u, 8u) ||
        !read_words(in, header[0], header[1] / 4u) || fread(block, 4u, header[3], in) != header[3])
        return 2;
    fclose(in);
    d3d8_pushbuffer_set_consumer(log_roll, NULL);
    uint32_t begun = 0xDEADFA7Au;
    uint32_t ended = 0xDEADFA7Au;
    uint32_t argument = header[2];
    RUN_EXPECTING_FATAL(begun = call_stdcall(0x003D6660u, &argument, 1u));
    if (fatal_seen) {
        begun = 0xDEADFA7Au;
        fprintf(stderr, "native BeginPush fatal at %#x: %s\n", (unsigned)fatal_address, fatal_text);
    } else {
        for (uint32_t i = 0u; i < header[3]; i++) store(begun + i * 4u, block[i]);
        argument = begun + header[4] * 4u;
        RUN_EXPECTING_FATAL(ended = call_stdcall(0x003D6680u, &argument, 1u));
        if (fatal_seen) {
            ended = 0xDEADFA7Au;
            fprintf(stderr, "native EndPush fatal at %#x: %s\n", (unsigned)fatal_address, fatal_text);
        }
    }
    const uint32_t logged = roll_count < MAX_ROLLS ? roll_count : MAX_ROLLS;
    if (fwrite(&begun, 4u, 1u, out) != 1u || fwrite(&ended, 4u, 1u, out) != 1u ||
        fwrite(&roll_count, 4u, 1u, out) != 1u || fwrite(roll_cursors, 4u, logged, out) != logged)
        return 2;
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
