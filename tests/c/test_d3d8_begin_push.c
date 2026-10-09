/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * BeginPush 0x003D6660 and EndPush 0x003D6680 (T854): the raw pushbuffer pointer the title's game function 0x1EB00
 * writes its 34 dword vertex binding block through (docs/d3d8-usage.md 9.2).
 *
 * The expected numbers are hand derived from the original's bytes (tests/test_d3d8_begin_push_oracle.py replays the
 * original under emulation and compares word for word): BeginPush(Count) flushes the deferred state like
 * DrawVertices does (0x003DEE00 with its argument zero), reserves Count + 1 dwords with 0x003D6B30 and returns the
 * cursor, which it does NOT advance. The title writes through the pointer and EndPush(p) stores p as the cursor. The
 * block is in the recorded stream the consumer sees after the kick, and the decoder takes its 16 parameter
 * SET_VERTEX_DATA_ARRAY_FORMAT and _OFFSET runs as the vertex arrays of the next draw.
 */
#include "test_d3d8_support.h"

#include "d3d8_gpu.h"
#include "d3d8_gpu_pgraph.h"
#include "gpu_pgraph.h"

#define RING_BYTES 0x40000u
#define KICKOFF 0x8000u
#define BLOCK_DWORDS 0x22u
#define DECLARATION 0x3E2C68u
#define STREAM_ROW 0x3E2BA8u
#define BUFFER_HEADER 0x3E2D00u
#define VERTEX_DATA 0x00A00000u

static uint32_t block_word(uint32_t index)
{
    if (index == 0u) return 0x00401760u;
    if (index <= 16u) return ((index - 1u) * 4u << 8) + (index % 2u ? 0x12u : 0x32u);
    if (index == 17u) return 0x00401720u;
    return (0x80100000u + (index - 18u) * 0x40u) & 0x7FFFFFFFu;
}

static void seed(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    CHECK(d3d8_device_register() >= 3u);
    map_fixed(0x00540000u, 0x10000u); /* the attribute to slot map the stream work reads */
    map_fixed(VERTEX_DATA, 0x1000u);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, RING_BYTES);
    store(D3D8_GLOBAL_KICKOFF_SIZE, KICKOFF);
    store(0x003E3F58u, D3D8_DEVICE_BASE);
    CHECK(d3d8_gpu_create());
    CHECK(d3d8_pushbuffer_create());
    d3d8_device_store32(D3D8_DEV_FLAGS, 0x4203u);
    d3d8_device_store32(0x20u, 0u);
    d3d8_device_store32(0x794u, DECLARATION);
    store(DECLARATION + 4u, 0u);
    /* every attribute maps to descriptor 0: stream 0, offset 4, format 0x12 (not the disabled 2) */
    store(DECLARATION + 0x14u, 0u);
    store(DECLARATION + 0x18u, 4u);
    store(DECLARATION + 0x1Cu, 0x12u);
    store(STREAM_ROW, 20u);
    store(STREAM_ROW + 4u, 8u);
    store(STREAM_ROW + 8u, BUFFER_HEADER);
    store(BUFFER_HEADER + 4u, VERTEX_DATA);
    store(D3D8_GLOBAL_DIRTY_MASK, 0u);
}

static uint32_t cursor_now(void) { return d3d8_device_load32(D3D8_DEV_CURSOR); }

static void write_block(uint32_t pointer)
{
    for (uint32_t index = 0u; index < BLOCK_DWORDS; index++) store(pointer + index * 4u, block_word(index));
}

static uint32_t begin_push(uint32_t count)
{
    return call_stdcall(0x003D6660u, &count, 1u);
}

static uint32_t end_push(uint32_t pointer)
{
    return call_stdcall(0x003D6680u, &pointer, 1u);
}

static void test_clean_state_reserves_and_publishes(void)
{
    seed();
    const uint32_t start = cursor_now();
    const uint64_t written = d3d8_pushbuffer_dwords_written();
    const uint32_t pointer = begin_push(BLOCK_DWORDS);
    /* nothing to flush: the pointer is the cursor, which BeginPush leaves where it was */
    CHECK_EQ_U32(pointer, start);
    CHECK_EQ_U32(cursor_now(), start);
    write_block(pointer);
    CHECK_EQ_U32(cursor_now(), start);
    CHECK_EQ_U32(end_push(pointer + BLOCK_DWORDS * 4u), pointer + BLOCK_DWORDS * 4u);
    CHECK_EQ_U32(cursor_now(), start + 0x88u);
    CHECK(d3d8_pushbuffer_dwords_written() == written + BLOCK_DWORDS);
}

static void test_the_block_is_in_the_recorded_stream(void)
{
    seed();
    const size_t before = d3d8_gpu_stream_count();
    const uint32_t pointer = begin_push(BLOCK_DWORDS);
    write_block(pointer);
    (void)end_push(pointer + BLOCK_DWORDS * 4u);
    d3d8_gpu_kick();
    /* two headers (count 16) become 32 (method, data) pairs, in order */
    CHECK(d3d8_gpu_stream_count() - before == 32u);
    for (uint32_t slot = 0u; slot < 16u; slot++) {
        const d3d8_gpu_command format = d3d8_gpu_stream_at(before + slot);
        const d3d8_gpu_command offset = d3d8_gpu_stream_at(before + 16u + slot);
        CHECK_EQ_U32(format.method, 0x1760u + slot * 4u);
        CHECK_EQ_U32(format.data, block_word(1u + slot));
        CHECK_EQ_U32(offset.method, 0x1720u + slot * 4u);
        CHECK_EQ_U32(offset.data, block_word(18u + slot));
    }
    CHECK_EQ_U32(d3d8_gpu_get_stats().malformed_dwords, 0u);
}

static void test_the_decoder_takes_the_block_as_the_vertex_arrays(void)
{
    seed();
    const uint32_t pointer = begin_push(BLOCK_DWORDS);
    write_block(pointer);
    (void)end_push(pointer + BLOCK_DWORDS * 4u);
    (void)d3d8_draw_vertices(5u, 0u, 3u);
    d3d8_gpu_kick();
    gpu_pgraph *model = gpu_pgraph_create();
    size_t next = 0u;
    CHECK(d3d8_gpu_decode_recording(model, &next) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_unhandled_count(model) == 0u);
    CHECK(gpu_pgraph_draw_count(model) == 1u);
    const gpu_pgraph_draw *draw = gpu_pgraph_draw_at(model, 0u);
    CHECK(draw != NULL);
    if (draw != NULL) {
        for (uint32_t slot = 0u; slot < 16u; slot++) {
            CHECK(draw->arrays[slot].format_set && draw->arrays[slot].address_set);
            CHECK_EQ_U32(draw->arrays[slot].format, block_word(1u + slot));
            CHECK_EQ_U32(draw->arrays[slot].address, block_word(18u + slot));
        }
    }
    gpu_pgraph_destroy(model);
}

static void test_the_stream_work_is_flushed_in_front_of_the_pointer(void)
{
    seed();
    store(D3D8_GLOBAL_DIRTY_MASK, 0x70u); /* what SetStreamSource leaves after a stride change */
    const uint32_t start = cursor_now();
    const uint32_t pointer = begin_push(BLOCK_DWORDS);
    /* 17 dwords of formats plus 16 pairs of offsets, all attributes on the one enabled descriptor */
    CHECK_EQ_U32(pointer, start + (17u + 32u) * 4u);
    CHECK_EQ_U32(cursor_now(), pointer);
    CHECK_EQ_U32(load(start), 0x00401760u);
    CHECK_EQ_U32(load(start + 4u), (20u << 8) + 0x12u);
    CHECK_EQ_U32(load(start + 17u * 4u), 0x00041720u);
    CHECK_EQ_U32(load(start + 17u * 4u + 4u), VERTEX_DATA + 4u + 8u);
    CHECK_EQ_U32(load(start + 17u * 4u + 8u * 15u), 0x00041720u + 15u * 4u);
    CHECK_EQ_U32(d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK), 0x20u); /* 0x50 cleared, the rest stays */
    CHECK_EQ_U32(d3d8_device_load32(0x20u), 0u);
    write_block(pointer);
    CHECK_EQ_U32(end_push(pointer + BLOCK_DWORDS * 4u), pointer + BLOCK_DWORDS * 4u);
    CHECK_EQ_U32(cursor_now(), pointer + 0x88u);
}

static void test_the_reservation_rolls_the_ring_over_at_the_limit(void)
{
    seed();
    const uint32_t limit = d3d8_device_load32(D3D8_DEV_LIMIT);
    /* 0x23 dwords are 0x8C bytes: from 0x200 - 0x8C = 0x174 past the limit the ring rolls over */
    d3d8_device_store32(D3D8_DEV_CURSOR, limit + 0x170u);
    uint32_t pointer = begin_push(BLOCK_DWORDS);
    CHECK_EQ_U32(pointer, limit + 0x170u);
    write_block(pointer);
    (void)end_push(pointer + BLOCK_DWORDS * 4u);
    d3d8_device_store32(D3D8_DEV_CURSOR, limit + 0x174u);
    pointer = begin_push(BLOCK_DWORDS);
    CHECK(pointer != limit + 0x174u);
    CHECK_EQ_U32(pointer, cursor_now());
    CHECK(pointer > limit + 0x174u); /* the next segment of the ring, past the old cursor and the fence packet */
    CHECK(d3d8_device_load32(D3D8_DEV_LIMIT) > limit);
}

static void expect_endpush_refusal(uint32_t pointer, const char *why)
{
    const uint32_t cursor = cursor_now();
    RUN_EXPECTING_FATAL((void)end_push(pointer));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003D6680u);
    CHECK(strstr(fatal_text, why) != NULL);
    CHECK_EQ_U32(cursor_now(), cursor); /* nothing was published */
}

static void test_endpush_refusals(void)
{
    seed();
    expect_endpush_refusal(cursor_now(), "no BeginPush block open");
    const uint32_t pointer = begin_push(BLOCK_DWORDS);
    expect_endpush_refusal(pointer - 4u, "not a dword position inside the block");
    expect_endpush_refusal(pointer + 2u, "not a dword position inside the block");
    expect_endpush_refusal(pointer + (BLOCK_DWORDS + 1u) * 4u + 4u, "not a dword position inside the block");
    /* the whole reservation (Count + 1 dwords) and the empty block are fine */
    CHECK_EQ_U32(end_push(pointer + (BLOCK_DWORDS + 1u) * 4u), pointer + (BLOCK_DWORDS + 1u) * 4u);
    expect_endpush_refusal(pointer, "no BeginPush block open"); /* a second EndPush for one BeginPush */
    const uint32_t again = begin_push(BLOCK_DWORDS);
    CHECK_EQ_U32(end_push(again), again);
    CHECK_EQ_U32(cursor_now(), again);
}

static void test_beginpush_refusals_come_before_any_write(void)
{
    seed();
    const uint32_t start = cursor_now();
    store(D3D8_GLOBAL_DIRTY_MASK, 0x70u);
    /* a count the ring cannot hold, and the 32 bit wrap of Count + 1 to no dwords at all */
    uint32_t count = RING_BYTES / 4u;
    RUN_EXPECTING_FATAL((void)begin_push(count));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003D6660u);
    CHECK(strstr(fatal_text, "BeginPush") != NULL);
    /* the ring less its slack would hold the dwords themselves, the reservation adds the 0x204 bytes the writers rely on */
    count = RING_BYTES / 4u - 0x20u;
    RUN_EXPECTING_FATAL((void)begin_push(count));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003D6660u);
    count = 0xFFFFFFFFu;
    RUN_EXPECTING_FATAL((void)begin_push(count));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003D6660u);
    /* a dirty bit the flush has no port for: refused by name with the cursor and the mask untouched */
    store(D3D8_GLOBAL_DIRTY_MASK, 0x8000u);
    RUN_EXPECTING_FATAL((void)begin_push(BLOCK_DWORDS));
    CHECK(fatal_seen);
    CHECK(strstr(fatal_text, "unsupported draw dirty mask") != NULL);
    CHECK_EQ_U32(cursor_now(), start);
    CHECK_EQ_U32(d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK), 0x8000u);
    /* and a refused BeginPush opened no block */
    RUN_EXPECTING_FATAL((void)end_push(start));
    CHECK(fatal_seen);
    CHECK(strstr(fatal_text, "no BeginPush block open") != NULL);
}

static void test_a_reservation_over_unmapped_memory_is_refused(void)
{
    seed();
    const uint32_t end = d3d8_device_load32(0x28u);
    CHECK(kernel_guest_at(end, 4u) == NULL); /* the ring ends where the mapping does, or this test proves nothing */
    /* the cursor 0x20 bytes short of the end with a limit far enough on that no roll-over is due: the 0x8C byte block
     * would run 0x6C bytes past the mapping, and the refusal comes before anything is published */
    d3d8_device_store32(D3D8_DEV_CURSOR, end - 0x20u);
    d3d8_device_store32(D3D8_DEV_LIMIT, end + 0x1000u);
    RUN_EXPECTING_FATAL((void)begin_push(BLOCK_DWORDS));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003D6660u);
    CHECK(strstr(fatal_text, "not mapped guest memory") != NULL);
    CHECK_EQ_U32(cursor_now(), end - 0x20u);
}

static void test_a_second_beginpush_replaces_the_open_block(void)
{
    seed();
    const uint32_t first = begin_push(4u);
    const uint32_t second = begin_push(BLOCK_DWORDS);
    CHECK_EQ_U32(first, second); /* the first was never published, the cursor did not move */
    CHECK_EQ_U32(end_push(second + BLOCK_DWORDS * 4u), second + BLOCK_DWORDS * 4u);
}

int main(void)
{
    test_clean_state_reserves_and_publishes();
    test_the_block_is_in_the_recorded_stream();
    test_the_decoder_takes_the_block_as_the_vertex_arrays();
    test_the_stream_work_is_flushed_in_front_of_the_pointer();
    test_the_reservation_rolls_the_ring_over_at_the_limit();
    test_endpush_refusals();
    test_beginpush_refusals_come_before_any_write();
    test_a_reservation_over_unmapped_memory_is_refused();
    test_a_second_beginpush_replaces_the_open_block();
    environment_end();
    printf("d3d8_begin_push: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
