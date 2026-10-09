/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shared by tests/c/test_gpu_pgraph.c, test_gpu_pgraph_replay.c and gpu_pgraph_runner.c: a builder
 * that writes streams the way d3d8_gpu.c's consumer records them (an incrementing header run
 * becomes method, method + 4, ..., a non-incrementing one repeats the method), and a flat fake
 * guest memory behind gpu_pgraph_read_fn.
 *
 * The runs mirror the measured emitters: a program upload is `0x1E9C slot` then chunks of at most
 * 32 dwords on 0x0B00, a constant upload is `0x1EA4 row` then chunks of at most 16 dwords on
 * 0x0B80, the vertex arrays are 16 formats on 0x1760 and one offset write per enabled slot on
 * 0x1720 + 4n, a DrawVertices is BEGIN_END(op), a non-incrementing 0x1810 run and BEGIN_END(0).
 */

#ifndef TSFP_GPU_PGRAPH_TEST_SUPPORT_H
#define TSFP_GPU_PGRAPH_TEST_SUPPORT_H

#include "gpu_pgraph.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    gpu_pgraph_command *pairs;
    size_t count;
    size_t capacity;
} stream_builder;

static inline void stream_free(stream_builder *builder)
{
    free(builder->pairs);
    memset(builder, 0, sizeof *builder);
}

static inline void stream_pair(stream_builder *builder, uint32_t method, uint32_t data)
{
    if (builder->count == builder->capacity) {
        builder->capacity = builder->capacity == 0u ? 256u : builder->capacity * 2u;
        builder->pairs = realloc(builder->pairs, builder->capacity * sizeof *builder->pairs);
    }
    builder->pairs[builder->count].method = method;
    builder->pairs[builder->count].data = data;
    builder->count++;
}

/* An incrementing run, as the consumer records `header(method, count)`. */
static inline void stream_run(stream_builder *builder, uint32_t method, const uint32_t *data,
                              uint32_t count)
{
    for (uint32_t i = 0u; i < count; i++) {
        stream_pair(builder, method + 4u * i, data[i]);
    }
}

static inline uint32_t float_bits_of(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

/* 0x1E9C slot, then the program in chunks of 32 dwords on 0x0B00 (d3d8_vertex_program.c). */
static inline void stream_program(stream_builder *builder, uint32_t slot, const uint32_t *words,
                                  uint32_t instructions)
{
    stream_pair(builder, GPU_PGRAPH_PROGRAM_LOAD, slot);
    const uint32_t dwords = instructions * 4u;
    for (uint32_t done = 0u; done < dwords;) {
        const uint32_t chunk = dwords - done < 32u ? dwords - done : 32u;
        stream_run(builder, GPU_PGRAPH_PROGRAM_DATA, words + done, chunk);
        done += chunk;
    }
}

/* 0x1EA4 row, then chunks of 16 dwords on 0x0B80 (d3d8_vertex_constants.c). */
static inline void stream_constants(stream_builder *builder, uint32_t row, const float *values,
                                    uint32_t count_dwords)
{
    stream_pair(builder, GPU_PGRAPH_CONSTANT_LOAD, row);
    for (uint32_t done = 0u; done < count_dwords;) {
        const uint32_t chunk = count_dwords - done < 16u ? count_dwords - done : 16u;
        uint32_t words[16];
        for (uint32_t i = 0u; i < chunk; i++) {
            words[i] = float_bits_of(values[done + i]);
        }
        stream_run(builder, GPU_PGRAPH_CONSTANT_DATA, words, chunk);
        done += chunk;
    }
}

/* The viewport pair the programmable branch of vertex_viewport_emit writes. */
static inline void stream_viewport(stream_builder *builder, const float offset[4],
                                   const float scale[4])
{
    for (uint32_t i = 0u; i < 4u; i++) {
        stream_pair(builder, GPU_PGRAPH_VIEWPORT_OFFSET + 4u * i, float_bits_of(offset[i]));
    }
    for (uint32_t i = 0u; i < 4u; i++) {
        stream_pair(builder, GPU_PGRAPH_VIEWPORT_SCALE + 4u * i, float_bits_of(scale[i]));
    }
}

/* One array: its format word on 0x1760 + 4 slot, and its address on 0x1720 + 4 slot. */
#define ARRAY_FORMAT(stride, size, type) \
    (((uint32_t)(stride) << 8) | ((uint32_t)(size) << 4) | (uint32_t)(type))
static inline uint32_t array_format(uint32_t stride, uint32_t size, uint32_t type)
{
    return ARRAY_FORMAT(stride, size, type);
}

static inline void stream_array(stream_builder *builder, uint32_t slot, uint32_t address,
                                uint32_t format)
{
    stream_pair(builder, GPU_PGRAPH_ARRAY_FORMAT + 4u * slot, format);
    stream_pair(builder, GPU_PGRAPH_ARRAY_OFFSET + 4u * slot, address);
}

/* BEGIN_END(op), DRAW_ARRAYS in chunks of 256, BEGIN_END(0): d3d8_draw_vertices. */
static inline void stream_draw_arrays(stream_builder *builder, uint32_t op, uint32_t first,
                                      uint32_t count)
{
    stream_pair(builder, GPU_PGRAPH_BEGIN_END, op);
    for (uint32_t done = 0u; done < count;) {
        const uint32_t chunk = count - done < 256u ? count - done : 256u;
        stream_pair(builder, GPU_PGRAPH_DRAW_ARRAYS, ((chunk - 1u) << 24) | (first + done));
        done += chunk;
    }
    stream_pair(builder, GPU_PGRAPH_BEGIN_END, 0u);
}

/* The writer of d3d8_shader.c pixel_shader_apply: contiguous runs, the final words last and only
 * when their OR is nonzero. */
static inline void stream_pixel_shader(stream_builder *stream, const uint32_t words[GPU_PGRAPH_COMBINER_WORDS])
{
    stream_run(stream, 0x0260u, words + 0u, 8u);
    stream_run(stream, 0x0A60u, words + 10u, 32u);
    stream_run(stream, 0x17F8u, words + 42u, 1u);
    stream_run(stream, 0x1E20u, words + 43u, 2u);
    stream_run(stream, 0x1E40u, words + 45u, 9u);
    stream_run(stream, 0x1E74u, words + 55u, 2u);
    if ((words[8] | words[9]) != 0u) {
        stream_run(stream, 0x0288u, words + 8u, 2u);
    }
}

/* Flat guest memory: one region starting at `base`. */
typedef struct {
    uint32_t base;
    uint8_t *bytes;
    size_t size;
} fake_guest;

static inline bool fake_guest_read(void *context, uint32_t address, void *out, size_t bytes)
{
    const fake_guest *guest = context;
    if (address < guest->base || (uint64_t)address - guest->base + bytes > guest->size) {
        return false;
    }
    memcpy(out, guest->bytes + (address - guest->base), bytes);
    return true;
}

/* A one-instruction program with the FINAL bit (dword 3 bit 0) set, and the digest of
 * `0x2078, 1, instruction` computed with Python's hashlib (the corpus's convention). */
static const uint32_t synthetic_program[4] = {0x00112233u, 0x44556677u, 0x8899AABBu, 0xCCDDEE01u};
#define SYNTHETIC_PROGRAM_NAME \
    "static_3ebe9b7e7c96baecc4c329dd008c862963e3d94b1a62ff2636e179b8ddc456aa"

#endif
