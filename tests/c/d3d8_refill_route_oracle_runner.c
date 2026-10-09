/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Replay runner for the routes T546 moved onto the carried refill simulation (the scissor 0x003D4470, the fog emitter
 * 0x003DDEA0, the indexed draw 0x003D5050 and DrawVertices 0x003D4FB0) over a pushbuffer that refills: reads the ORIGINAL
 * guest state the Python side captured, attaches the GPU module's refill tail, installs a consumer that records every
 * range the refill hands over (with an FNV-1a of the dwords AT THE TIME of the hand-off), runs the native routine and
 * writes the state back.
 *
 * Input: u32 mode (0 scissors(a0, a1, a2), 1 fog, 2 indexed draw(a0, a1, a2), 3 DrawVertices(a0, a1, a2)), a0, a1, a2, ring
 * base, ring bytes, the DMA put the original's GPU holds, the control block page, the second-history page (page aligned,
 * mapped at the original's addresses), u32 window count, {base, bytes} per window, then the 0x30000-byte D3D region, the
 * ring, the control page, the history page and every window's bytes. A window inside the region the test environment already
 * maps is copied over it, any other is mapped first.
 * Output: the result (0xDEADFA7A after a fatal), the same areas in the same order, then u32 consumer calls (at most 64) and
 * 64 {begin, end, fnv} records (zero padded).
 */
#include "d3d8_gpu.h"
#include "d3d8_indexed.h"
#include "d3d8_scissor.h"
#include "d3d8_texture_dirty.h"
#include "test_d3d8_support.h"

#define PAGE_BYTES 0x1000u
#define LOG_CAPACITY 64u
#define MAX_WINDOWS 16u

static uint32_t log_words[LOG_CAPACITY][3];
static uint32_t log_count;

static void consume(void *context, uint32_t begin, uint32_t end)
{
    (void)context;
    uint32_t hash = 2166136261u;
    for (uint32_t at = begin; at < end; at += 4u) {
        hash = (hash ^ load(at)) * 16777619u;
    }
    if (log_count < LOG_CAPACITY) {
        log_words[log_count][0] = begin;
        log_words[log_count][1] = end;
        log_words[log_count][2] = hash;
    }
    log_count++;
}

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
    uint32_t header[10];
    if (!in || !out || fread(header, sizeof(header), 1u, in) != 1u) return 2;
    const uint32_t mode = header[0], a0 = header[1], a1 = header[2], a2 = header[3];
    const uint32_t ring = header[4], ring_bytes = header[5], put = header[6];
    const uint32_t control = header[7], history = header[8], window_count = header[9];
    uint32_t windows[MAX_WINDOWS][2];
    if (window_count > MAX_WINDOWS || fread(windows, sizeof(windows[0]), window_count, in) != window_count) return 2;
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(ring, ring_bytes);
    map_fixed(control, PAGE_BYTES);
    map_fixed(history, PAGE_BYTES);
    for (uint32_t i = 0u; i < window_count; i++) {
        if (kernel_guest_at(windows[i][0], windows[i][1]) == NULL) map_fixed(windows[i][0], windows[i][1]);
    }
    int ok = copy_in(in, D3D_REGION_BASE, D3D_REGION_BYTES) && copy_in(in, ring, ring_bytes) &&
             copy_in(in, control, PAGE_BYTES) && copy_in(in, history, PAGE_BYTES);
    for (uint32_t i = 0u; ok && i < window_count; i++) ok = copy_in(in, windows[i][0], windows[i][1]);
    if (!ok) return 2;
    fclose(in);
    d3d8_gpu_attach();
    /* The transplanted ring has no segment start: take it to be the routine's first byte, so the consumer's range is
     * exactly what it writes. The drain also moves the put, which is set afterwards. */
    d3d8_pushbuffer_set_consumer(NULL, NULL);
    d3d8_pushbuffer_drain();
    d3d8_pushbuffer_set_consumer(consume, NULL);
    d3d8_pushbuffer_set_put(0x80000000u | put);
    uint32_t result = 0xDEADFA7Au;
    if (mode == 0u) {
        RUN_EXPECTING_FATAL(result = d3d8_set_scissors(a0, a1, a2));
    } else if (mode == 1u) {
        RUN_EXPECTING_FATAL(result = d3d8_emit_fog());
    } else if (mode == 2u) {
        RUN_EXPECTING_FATAL(result = d3d8_draw_indexed_vertices(a0, a1, a2));
    } else {
        RUN_EXPECTING_FATAL(result = d3d8_draw_vertices(a0, a1, a2));
    }
    if (fatal_seen) {
        result = 0xDEADFA7Au;
        fprintf(stderr, "native fatal at %#x: %s\n", (unsigned)fatal_address, fatal_text);
    }
    if (fwrite(&result, 4u, 1u, out) != 1u) return 2;
    ok = copy_out(out, D3D_REGION_BASE, D3D_REGION_BYTES) && copy_out(out, ring, ring_bytes) &&
         copy_out(out, control, PAGE_BYTES) && copy_out(out, history, PAGE_BYTES);
    for (uint32_t i = 0u; ok && i < window_count; i++) ok = copy_out(out, windows[i][0], windows[i][1]);
    if (!ok) return 2;
    if (fwrite(&log_count, 4u, 1u, out) != 1u || fwrite(log_words, sizeof(log_words), 1u, out) != 1u) return 2;
    fclose(out);
    environment_end();
    return 0;
}
