/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Replay runner for the one-vector constants emitters5670/56D0 over a pushbuffer that refills (T487): reads the
 * ORIGINAL guest state the Python side captured, attaches the GPU module's refill tail, installs a consumer that
 * records every range the refill hands over (with an FNV-1a of the dwords AT THE TIME of the hand-off, which proves
 * the first run was in the ring before the refill), calls the registered fixed-one handler and writes back the D3D
 * region, the whole ring, the control block page, the second-history page and the source page.
 *
 * Input: ring base, ring bytes, index, source offset, the DMA put the original's GPU holds, the control block page, the
 * second-history page and the source page (all page aligned, mapped at the original's addresses), then the
 * 0x30000-byte region, the ring, the control page, the history page and the source page.
 * Output: the result (0xDEADFA7A after a fatal), the same five areas, then u32 consumer calls (at most 4) and 4
 * {begin, end, fnv} records (zero padded).
 */
#include "d3d8_gpu.h"
#include "d3d8_vertex_constants.h"
#include "test_d3d8_support.h"

#ifndef T990_ENTRY
#error "compile with an independently selected original entry5670 or56D0"
#endif

#define PAGE_BYTES 0x1000u
#define LOG_CAPACITY 4u

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
    uint32_t header[8];
    if (!in || !out || fread(header, sizeof(header), 1u, in) != 1u) return 2;
    const uint32_t ring = header[0], ring_bytes = header[1], index = header[2], source_offset = header[3];
    const uint32_t put = header[4], control = header[5], history = header[6], source = header[7];
    environment_begin(KERNEL_AV_PACK_HDTV);
    const d3d8_surface_entry rows[]={{T990_ENTRY,NULL,2u}};
    if(!d3d8_hle_init(rows,1u))return 2;
    (void)d3d8_vertex_constants_register();
    map_fixed(ring, ring_bytes);
    map_fixed(control, PAGE_BYTES);
    map_fixed(history, PAGE_BYTES);
    map_fixed(source, PAGE_BYTES);
    if (!copy_in(in, D3D_REGION_BASE, D3D_REGION_BYTES) || !copy_in(in, ring, ring_bytes) ||
        !copy_in(in, control, PAGE_BYTES) || !copy_in(in, history, PAGE_BYTES) ||
        !copy_in(in, source, PAGE_BYTES)) {
        return 2;
    }
    fclose(in);
    d3d8_gpu_attach();
    /* The transplanted ring has no segment start: take it to be the packet's first byte, so the consumer's range
     * is exactly the first run. The drain also moves the put, which is set afterwards. */
    d3d8_pushbuffer_set_consumer(NULL, NULL);
    d3d8_pushbuffer_drain();
    d3d8_pushbuffer_set_consumer(consume, NULL);
    d3d8_pushbuffer_set_put(0x80000000u | put);
    uint32_t result = 0xDEADFA7Au;
    RUN_EXPECTING_FATAL(result = call_fastcall(T990_ENTRY,index,source+source_offset));
    if (fatal_seen) {
        result = 0xDEADFA7Au;
        fprintf(stderr, "native fatal at %#x: %s\n", (unsigned)fatal_address, fatal_text);
    }
    if (fwrite(&result, 4u, 1u, out) != 1u) return 2;
    if (!copy_out(out, D3D_REGION_BASE, D3D_REGION_BYTES) || !copy_out(out, ring, ring_bytes) ||
        !copy_out(out, control, PAGE_BYTES) || !copy_out(out, history, PAGE_BYTES) ||
        !copy_out(out, source, PAGE_BYTES)) {
        return 2;
    }
    if (fwrite(&log_count, 4u, 1u, out) != 1u || fwrite(log_words, sizeof(log_words), 1u, out) != 1u) return 2;
    fclose(out);
    environment_end();
    return 0;
}
