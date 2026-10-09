/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T440 x T267: the commands the ported Clear writes (src/gpu/d3d8_frame.c), over a real pushbuffer ring,
 * kicked, recorded by the instantaneous GPU and decoded by gpu_pgraph.c with the CLEAR group on. This is
 * what ties the emitted method sequence to the decoder's clear event: the rectangle run at 0x1D98, the count
 * 3 run at 0x1D8C and CLEAR_SURFACE (0x1D94) as the event. Device free, synthetic guest memory. The state
 * seeded is a created device's (a 640x480 target, scale 1.0, the retail library constants), the packet
 * words are the ones the original produces for it (tests/test_d3d8_clear_oracle.py).
 */
#include "test_d3d8_support.h"

#include "d3d8_frame.h"
#include "d3d8_gpu.h"
#include "d3d8_gpu_pgraph.h"
#include "gpu_pgraph.h"

#define ARENA 0x00D00000u
#define RENDER_TARGET (ARENA + 0x100u)
#define DEPTH_STENCIL (ARENA + 0x200u)
#define RECTANGLES (ARENA + 0x400u)
#define CONSTANTS 0x004A1000u

static void surface(uint32_t address, uint32_t format)
{
    store(address, 0x01050001u);
    store(address + 12u, (8u << 24) | (9u << 20) | (format << 8) | 1u);
    store(address + 16u, 0u);
}

static void seed(uint32_t colour_format, uint32_t depth_format)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(ARENA, 0x2000u);
    map_fixed(CONSTANTS, 0x1000u);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    store(D3D8_DEVICE_POINTER_SLOT, D3D8_DEVICE_BASE);
    CHECK(d3d8_gpu_create());
    CHECK(d3d8_pushbuffer_create());
    (void)d3d8_device_register();
    static const uint8_t class_table[26] = {0, 2, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
                                            2, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0};
    for (uint32_t index = 0u; index < 26u; index++) {
        store_byte(0x003D625Cu + index, class_table[index]);
    }
    store_byte(GUEST_FORMAT_INFO + 0x06u, 0xA1u);
    store(0x00475CD4u, 0x3F000000u);
    store(0x004A1BA0u, 0x4B7FFFFFu);
    store(0x004A1BA4u, 0x477FFF00u);
    surface(RENDER_TARGET, colour_format);
    store(D3D8_DEVICE_BASE + 0x1A04u, RENDER_TARGET);
    surface(DEPTH_STENCIL, depth_format);
    store(D3D8_DEVICE_BASE + 0x1A08u, DEPTH_STENCIL);
    store(D3D8_DEVICE_BASE + 0xEE8u, 640u);
    store(D3D8_DEVICE_BASE + 0xEECu, 480u);
    store(D3D8_DEVICE_BASE + 0x95Cu, 0x3F800000u);
    store(D3D8_DEVICE_BASE + 0x960u, 0x3F800000u);
}

static void clear(uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, uint32_t depth,
                  uint32_t stencil)
{
    const uint32_t args[6] = {count, rects, flags, color, depth, stencil};
    (void)call_stdcall(0x003D5EB0u, args, 6u);
}

int main(void)
{
    /* The title's own Clear (Count 0, flags 0xF3, colour 0, Z 1.0): five pairs, one clear event. */
    seed(0x12u, 0x2Eu);
    clear(0u, 0u, 0xF3u, 0u, 0x3F800000u, 0u);
    d3d8_gpu_kick();
    CHECK_EQ_U32(d3d8_gpu_stream_count(), 5u);
    CHECK_EQ_U32(d3d8_gpu_get_stats().malformed_dwords, 0u);
    CHECK_EQ_U32(d3d8_gpu_get_stats().clear_surfaces, 1u);
    static const uint32_t methods[5] = {0x1D98u, 0x1D9Cu, 0x1D8Cu, 0x1D90u, 0x1D94u};
    static const uint32_t data[5] = {0x027F0000u, 0x01DF0000u, 0xFFFFFF00u, 0u, 0xF3u};
    for (uint32_t index = 0u; index < 5u; index++) {
        CHECK_EQ_U32(d3d8_gpu_stream_at(index).method, methods[index]);
        CHECK_EQ_U32(d3d8_gpu_stream_at(index).data, data[index]);
    }

    /* Group off: five unhandled methods, no event. Group on and strict: every method is handled. */
    gpu_pgraph *off = gpu_pgraph_create();
    size_t next = 0u;
    CHECK(d3d8_gpu_decode_recording(off, &next) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_unhandled_count(off) == 5u);
    CHECK(gpu_pgraph_clear_count(off) == 0u);
    gpu_pgraph_destroy(off);

    gpu_pgraph *model = gpu_pgraph_create();
    gpu_pgraph_set_strict(model, true);
    gpu_pgraph_set_output_groups(model, GPU_PGRAPH_OUTPUT_CLEAR);
    next = 0u;
    CHECK(d3d8_gpu_decode_recording(model, &next) == GPU_PGRAPH_OK);
    CHECK(next == 5u && gpu_pgraph_unhandled_count(model) == 0u);
    CHECK(gpu_pgraph_clear_count(model) == 1u);
    const gpu_pgraph_clear *event = gpu_pgraph_clear_at(model, 0u);
    CHECK(event != NULL);
    if (event != NULL) {
        CHECK_EQ_U32(event->before_draw, 0u);
        CHECK_EQ_U32(event->command, 4u);
        CHECK_EQ_U32(event->flags, 0xF3u);
        CHECK_EQ_U32(event->zstencil, 0xFFFFFF00u);
        CHECK_EQ_U32(event->color, 0u);
        /* the decoder's rectangle is INCLUSIVE: 640 wide is 639 in the high half */
        CHECK_EQ_U32(event->rect_horizontal, 0x027F0000u);
        CHECK_EQ_U32(event->rect_vertical, 0x01DF0000u);
        CHECK(event->zstencil_written && event->color_written && event->rect_horizontal_written &&
              event->rect_vertical_written);
    }
    gpu_pgraph_destroy(model);
    environment_end();

    /* Two rectangles make two events in draw order, each with its own words. */
    seed(0x12u, 0x2Au);
    store(RECTANGLES, 10u);
    store(RECTANGLES + 4u, 20u);
    store(RECTANGLES + 8u, 300u);
    store(RECTANGLES + 12u, 200u);
    store(RECTANGLES + 16u, 320u);
    store(RECTANGLES + 20u, 240u);
    store(RECTANGLES + 24u, 639u);
    store(RECTANGLES + 28u, 479u);
    clear(2u, RECTANGLES, 0xF0u, 0x00FF8040u, 0u, 0u);
    d3d8_gpu_kick();
    model = gpu_pgraph_create();
    gpu_pgraph_set_strict(model, true);
    gpu_pgraph_set_output_groups(model, GPU_PGRAPH_OUTPUT_CLEAR);
    next = 0u;
    CHECK(d3d8_gpu_decode_recording(model, &next) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_clear_count(model) == 2u);
    const gpu_pgraph_clear *first = gpu_pgraph_clear_at(model, 0u);
    const gpu_pgraph_clear *second = gpu_pgraph_clear_at(model, 1u);
    CHECK(first != NULL && second != NULL);
    if (first != NULL && second != NULL) {
        CHECK_EQ_U32(first->rect_horizontal, 0x012B000Au);
        CHECK_EQ_U32(first->rect_vertical, 0x00C70014u);
        CHECK_EQ_U32(second->rect_horizontal, 0x027E0140u);
        CHECK_EQ_U32(second->rect_vertical, 0x01DE00F0u);
        CHECK_EQ_U32(first->color, 0x00FF8040u);
        CHECK_EQ_U32(second->flags, 0xF0u);
        CHECK(second->command > first->command);
    }
    gpu_pgraph_destroy(model);
    environment_end();

    /* A swizzled target brings SET_SURFACE_FORMAT (0x0208) around the clear: the decoder does not decode
     * it (T267 lists it as not done), so lenient mode counts two unhandled pairs and strict refuses at
     * the first one, naming it. The clear event itself decodes. */
    seed(0x06u, 0x2Au);
    clear(0u, 0u, 0xF1u, 0x00FF8040u, 0x3F000000u, 0u);
    d3d8_gpu_kick();
    CHECK_EQ_U32(d3d8_gpu_stream_count(), 7u);
    CHECK_EQ_U32(d3d8_gpu_stream_at(0u).method, 0x0208u);
    CHECK_EQ_U32(d3d8_gpu_stream_at(0u).data, 0x08090128u);
    CHECK_EQ_U32(d3d8_gpu_stream_at(6u).method, 0x0208u);
    CHECK_EQ_U32(d3d8_gpu_stream_at(6u).data, 0x08090228u);
    model = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(model, GPU_PGRAPH_OUTPUT_CLEAR);
    next = 0u;
    CHECK(d3d8_gpu_decode_recording(model, &next) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_unhandled_count(model) == 1u);
    CHECK(gpu_pgraph_clear_count(model) == 1u);
    gpu_pgraph_destroy(model);
    model = gpu_pgraph_create();
    gpu_pgraph_set_strict(model, true);
    gpu_pgraph_set_output_groups(model, GPU_PGRAPH_OUTPUT_CLEAR);
    next = 0u;
    CHECK(d3d8_gpu_decode_recording(model, &next) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(next == 0u);
    gpu_pgraph_destroy(model);
    environment_end();

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0 || checks < 40;
}
