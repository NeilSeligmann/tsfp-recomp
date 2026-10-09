/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T1246: gpu_pgraph_clone_frame, the copy the pipelined live renderer draws on the presenter thread while the decoder starts the next
 * frame. The clone must hold everything the draw reads (draws, indices, state snapshots, vertex bytes), must not alias the source, and
 * must stay intact when the source decodes the next frame over its own buffers. */
#include "gpu_pgraph.h"
#include "gpu_pgraph_test_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

#define BASE 0x00500000u

static void put_floats(uint8_t *memory, size_t offset, float first, unsigned count)
{
    for (unsigned i = 0u; i < count; i++) {
        const float value = first + (float)i;
        memcpy(memory + offset + 4u * i, &value, 4u);
    }
}

static void draw_triangle(gpu_pgraph *pgraph, stream_builder *stream)
{
    stream->count = 0u;
    stream_array(stream, 1u, BASE, array_format(12u, 3u, GPU_PGRAPH_TYPE_F));
    stream_pair(stream, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_TRIANGLES);
    stream_pair(stream, GPU_PGRAPH_DRAW_ARRAYS, (2u << 24) | 0u);
    stream_pair(stream, GPU_PGRAPH_BEGIN_END, 0u);
    CHECK(gpu_pgraph_decode(pgraph, stream->pairs, stream->count) == GPU_PGRAPH_OK);
}

int main(void)
{
    uint8_t memory[256] = {0};
    fake_guest guest = {BASE, memory, sizeof memory};
    gpu_pgraph *source = gpu_pgraph_create();
    gpu_pgraph *clone = gpu_pgraph_create();
    gpu_pgraph_set_vertex_capture(source, fake_guest_read, &guest, 1024u);
    stream_builder stream = {0};

    /* the state of the frame is not all zero (a viewport write), and the clone target starts holding another frame's state */
    {
        const float offset[4] = {320.0f, 240.0f, 0.0f, 0.0f}, scale[4] = {320.0f, -240.0f, 1.0f, 1.0f};
        stream_builder viewport = {0};
        stream_viewport(&viewport, offset, scale);
        CHECK(gpu_pgraph_decode(source, viewport.pairs, viewport.count) == GPU_PGRAPH_OK);
        stream_free(&viewport);
        const float other_offset[4] = {1.0f, 2.0f, 3.0f, 4.0f};
        gpu_pgraph *decoy = gpu_pgraph_create();
        gpu_pgraph_set_vertex_capture(decoy, fake_guest_read, &guest, 1024u);
        stream_builder decoy_stream = {0};
        stream_viewport(&decoy_stream, other_offset, scale);
        CHECK(gpu_pgraph_decode(decoy, decoy_stream.pairs, decoy_stream.count) == GPU_PGRAPH_OK);
        stream_free(&decoy_stream);
        draw_triangle(decoy, &stream);
        CHECK(gpu_pgraph_clone_frame(clone, decoy));
        CHECK(gpu_pgraph_snapshot_count(clone) == 1u);
        gpu_pgraph_destroy(decoy);
    }
    CHECK(!gpu_pgraph_clone_frame(NULL, source) && !gpu_pgraph_clone_frame(clone, NULL) && !gpu_pgraph_clone_frame(source, source));
    put_floats(memory, 0u, 1.0f, 9u);
    draw_triangle(source, &stream);
    draw_triangle(source, &stream);
    CHECK(gpu_pgraph_draw_count(source) == 2u);
    CHECK(gpu_pgraph_frame_self_contained(source));
    CHECK(!gpu_pgraph_frame_self_contained(NULL));

    CHECK(gpu_pgraph_clone_frame(clone, source));
    CHECK(gpu_pgraph_draw_count(clone) == 2u && gpu_pgraph_snapshot_count(clone) == gpu_pgraph_snapshot_count(source));
    CHECK(gpu_pgraph_draw_at(clone, 0u) != gpu_pgraph_draw_at(source, 0u));
    CHECK(gpu_pgraph_indices(clone) != gpu_pgraph_indices(source));
    CHECK(gpu_pgraph_snapshot(clone, 0u) != gpu_pgraph_snapshot(source, 0u));
    CHECK(memcmp(gpu_pgraph_snapshot(clone, 0u), gpu_pgraph_snapshot(source, 0u), sizeof(gpu_pgraph_state)) == 0);
    {
        static const uint8_t zero[sizeof(gpu_pgraph_state)];
        CHECK(memcmp(gpu_pgraph_snapshot(clone, 0u), zero, sizeof zero) != 0); /* the comparison above is not of two empty states */
    }
    uint32_t address = 0u, length = 0u;
    const uint8_t *bytes = gpu_pgraph_draw_vertex_bytes(clone, 0u, 1u, &address, &length);
    CHECK(bytes != NULL && address == BASE && length == 36u);
    CHECK(bytes != gpu_pgraph_draw_vertex_bytes(source, 0u, 1u, NULL, NULL));
    float first = 0.0f;
    if (bytes != NULL) memcpy(&first, bytes, sizeof first);
    CHECK(first == 1.0f);

    /* the guest rewrites its memory and the decoder starts the next frame over its own buffers: the clone keeps the old frame */
    put_floats(memory, 0u, 500.0f, 9u);
    CHECK(gpu_pgraph_begin_frame(source) == GPU_PGRAPH_OK);
    draw_triangle(source, &stream);
    CHECK(gpu_pgraph_draw_count(source) == 1u && gpu_pgraph_draw_count(clone) == 2u);
    bytes = gpu_pgraph_draw_vertex_bytes(clone, 1u, 1u, NULL, NULL);
    CHECK(bytes != NULL);
    if (bytes != NULL) memcpy(&first, bytes, sizeof first);
    CHECK(first == 1.0f);
    const uint8_t *fresh = gpu_pgraph_draw_vertex_bytes(source, 0u, 1u, NULL, NULL);
    float next = 0.0f;
    if (fresh != NULL) memcpy(&next, fresh, sizeof next);
    CHECK(next == 500.0f);

    /* a clone target is reusable: the next frame replaces the old one, buffers shrink and grow */
    CHECK(gpu_pgraph_clone_frame(clone, source));
    CHECK(gpu_pgraph_draw_count(clone) == 1u);
    bytes = gpu_pgraph_draw_vertex_bytes(clone, 0u, 1u, NULL, NULL);
    if (bytes != NULL) memcpy(&first, bytes, sizeof first);
    CHECK(bytes != NULL && first == 500.0f);

    /* a frame that did not snapshot its vertices reads guest memory when drawn, so it cannot be pipelined */
    gpu_pgraph *plain = gpu_pgraph_create();
    draw_triangle(plain, &stream);
    CHECK(gpu_pgraph_draw_count(plain) == 1u && !gpu_pgraph_frame_self_contained(plain));
    gpu_pgraph_destroy(plain);

    stream_free(&stream);
    gpu_pgraph_destroy(clone);
    gpu_pgraph_destroy(source);
    printf("gpu_pgraph_clone: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
