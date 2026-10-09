/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Replay runner for the pixel constant setter 0x003D9520 and the pixel shader binders 0x003D92E0 / 0x003D9320 over a
 * pushbuffer that refills (T525): reads the ORIGINAL guest state the Python side captured, attaches the GPU module's
 * refill tail, installs a consumer that records every range the refill hands over (with an FNV-1a of the dwords AT THE
 * TIME of the hand-off), runs the native routine and writes back the D3D region, the whole ring, the control block page,
 * the second-history page, the source page and the usage table.
 *
 * Input: mode (0 constants(index, count), 1 SetPixelShader(definition), 2 SetPixelShaderV(wrapper)), argument 0,
 * argument 1, ring base, ring bytes, the DMA put the original's GPU holds, the control block page, the second-history
 * page (all page aligned, mapped at the original's addresses), the float4 array of the constant setter, then the 0x30000-byte region, the ring, the control
 * page, the history page, the source page (0xA00000) and 64 bytes of usage table (0x005496E0).
 * Output: the result (0xDEADFA7A after a fatal), the same six areas, then u32 consumer calls (at most 64) and 64
 * {begin, end, fnv} records (zero padded).
 */
#include "d3d8_gpu.h"
#include "d3d8_shader.h"
#include "test_d3d8_support.h"

#define PAGE_BYTES 0x1000u
#define SOURCE_PAGE 0xA00000u
#define USAGE_TABLE 0x005496E0u
#define USAGE_BYTES 64u
#define LOG_CAPACITY 64u

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
    uint32_t header[9];
    if (!in || !out || fread(header, sizeof(header), 1u, in) != 1u) return 2;
    const uint32_t mode = header[0], first = header[1], second = header[2];
    const uint32_t ring = header[3], ring_bytes = header[4], put = header[5];
    const uint32_t control = header[6], history = header[7], data = header[8];
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(ring, ring_bytes);
    map_fixed(control, PAGE_BYTES);
    map_fixed(history, PAGE_BYTES);
    map_fixed(SOURCE_PAGE, PAGE_BYTES);
    map_fixed(0x540000u, 0x10000u);
    if (!copy_in(in, D3D_REGION_BASE, D3D_REGION_BYTES) || !copy_in(in, ring, ring_bytes) ||
        !copy_in(in, control, PAGE_BYTES) || !copy_in(in, history, PAGE_BYTES) ||
        !copy_in(in, SOURCE_PAGE, PAGE_BYTES) || !copy_in(in, USAGE_TABLE, USAGE_BYTES)) {
        return 2;
    }
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
        RUN_EXPECTING_FATAL(result = d3d8_set_pixel_shader_constants(first, data, second));
    } else if (mode == 1u) {
        RUN_EXPECTING_FATAL(result = d3d8_set_pixel_shader(first));
    } else {
        RUN_EXPECTING_FATAL(result = d3d8_set_pixel_shader_v(first));
    }
    if (fatal_seen) {
        result = 0xDEADFA7Au;
        fprintf(stderr, "native fatal at %#x: %s\n", (unsigned)fatal_address, fatal_text);
    }
    if (fwrite(&result, 4u, 1u, out) != 1u) return 2;
    if (!copy_out(out, D3D_REGION_BASE, D3D_REGION_BYTES) || !copy_out(out, ring, ring_bytes) ||
        !copy_out(out, control, PAGE_BYTES) || !copy_out(out, history, PAGE_BYTES) ||
        !copy_out(out, SOURCE_PAGE, PAGE_BYTES) || !copy_out(out, USAGE_TABLE, USAGE_BYTES)) {
        return 2;
    }
    if (fwrite(&log_count, 4u, 1u, out) != 1u || fwrite(log_words, sizeof(log_words), 1u, out) != 1u) return 2;
    fclose(out);
    environment_end();
    return 0;
}
