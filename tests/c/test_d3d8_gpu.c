/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Synthetic guest-memory checks, not original-XBE equivalence evidence. */
#include "test_d3d8_support.h"
#include "d3d8_gpu.h"
#include "kernel_clock.h"

int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    CHECK(d3d8_gpu_create());
    CHECK(d3d8_pushbuffer_create());
    CHECK_EQ_U32(load(d3d8_device_load32(D3D8_DEV_SEMAPHORE)), 3u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_HISTORY_MASK), 15u);
    d3d8_pushbuffer_emit_pair(0x00041D70u, 0x12345678u);
    d3d8_gpu_kick();
    CHECK_EQ_U32(d3d8_gpu_stream_count(), 1u);
    CHECK_EQ_U32(d3d8_gpu_stream_at(0).method, 0x1D70u);
    CHECK_EQ_U32(d3d8_gpu_stream_at(0).data, 0x12345678u);
    d3d8_gpu_kick();
    CHECK_EQ_U32(d3d8_gpu_stream_count(), 1u); /* a drain never replays */
    /* CLEAR_SURFACE (0x1D94) is counted when it is recorded, other methods are not (T440). */
    CHECK_EQ_U32(d3d8_gpu_get_stats().clear_surfaces, 0u);
    d3d8_pushbuffer_emit_pair(0x00041D90u, 0u);
    d3d8_pushbuffer_emit_pair(0x00041D94u, 0xF3u);
    d3d8_pushbuffer_emit_pair(0x00041D94u, 0xF0u);
    d3d8_gpu_kick();
    CHECK_EQ_U32(d3d8_gpu_get_stats().clear_surfaces, 2u);
    CHECK_EQ_U32(d3d8_gpu_stream_count(), 4u);
    d3d8_gpu_stream_discard(3u);
    CHECK_EQ_U32(d3d8_gpu_stream_count(), 1u);
    CHECK_EQ_U32(d3d8_gpu_get_stats().clear_surfaces, 2u); /* discarding does not uncount */
    d3d8_gpu_stream_discard(1u);
    CHECK_EQ_U32(d3d8_gpu_stream_count(), 0u);

    /* T391: the header's subchannel (bits 13-15) is recorded, not dropped. The fence's software method is
     * subchannel 5 method 0x310, which is NOT the 3D class's SET_DITHER_ENABLE at 0x310 (subchannel 0).
     * A run keeps its subchannel on every dword, a non-3D CLEAR_SURFACE is not a clear, and only the 3D
     * subchannel's commands are counted as 3D. MUTATION: masking the header with 0x1FFC alone, or reading
     * the subchannel from the wrong bits, fails here. */
    CHECK_EQ_U32(d3d8_gpu_get_stats().commands_other_subchannel, 0u);
    d3d8_pushbuffer_emit_pair(0x0004A310u, 0x11111111u);          /* subchannel 5, method 0x310 */
    d3d8_pushbuffer_emit_pair(0x00040310u, 0x22222222u);          /* subchannel 0, method 0x310 */
    d3d8_pushbuffer_emit_pair(0x0004BD94u, 0xF3u);                /* subchannel 5, method 0x1D94 */
    d3d8_pushbuffer_emit_pair(0x0004E000u | 0x180u, 0x33333333u); /* subchannel 7, method 0x180 */
    d3d8_pushbuffer_emit_pair((2u << 18) | (3u << 13) | 0x184u, 0x44444444u); /* subchannel 3, count 2 */
    store(d3d8_device_load32(D3D8_DEV_CURSOR), 0x55555555u);
    d3d8_pushbuffer_end(d3d8_device_load32(D3D8_DEV_CURSOR) + 4u);
    d3d8_gpu_kick();
    CHECK_EQ_U32(d3d8_gpu_stream_count(), 6u);
    static const struct {
        uint32_t subchannel, method, data;
    } wanted[] = {{5u, 0x310u, 0x11111111u}, {0u, 0x310u, 0x22222222u}, {5u, 0x1D94u, 0xF3u},
                  {7u, 0x180u, 0x33333333u}, {3u, 0x184u, 0x44444444u}, {3u, 0x188u, 0x55555555u}};
    for (size_t index = 0u; index < 6u; index++) {
        CHECK_EQ_U32(d3d8_gpu_stream_at(index).subchannel, wanted[index].subchannel);
        CHECK_EQ_U32(d3d8_gpu_stream_at(index).method, wanted[index].method);
        CHECK_EQ_U32(d3d8_gpu_stream_at(index).data, wanted[index].data);
    }
    CHECK_EQ_U32(d3d8_gpu_get_stats().commands_other_subchannel, 5u);
    CHECK_EQ_U32(d3d8_gpu_get_stats().clear_surfaces, 2u); /* the subchannel-5 0x1D94 is not a clear */
    d3d8_gpu_stream_discard(6u);
    CHECK_EQ_U32(d3d8_gpu_stream_at(99u).subchannel, 0u); /* out of range reads as the zero command */
    d3d8_pushbuffer_emit_pair(0x00041D70u, 0x12345678u);
    d3d8_gpu_kick();
    CHECK_EQ_U32(d3d8_gpu_stream_count(), 1u);
    CHECK_EQ_U32(d3d8_gpu_stream_at(0).method, 0x1D70u);
    /* T391: the fence writes the eight-dword packet of 0x003D67B0 at the cursor and moves it on 0x20. The words are
     * the ones the original leaves (tests/test_d3d8_refill_oracle.py measures them): 0x0004A310 (subchannel 5,
     * method 0x310) with ((cursor * 8 | fence & 0x1F) << 2) | wrap count & 3, SEMAPHORE_RELEASE with the fence,
     * and the colour clear value zeroed twice. MUTATION: a swapped pair, a wrong shift or a missing wrap
     * term fails one of these. */
    const uint32_t packet_at = d3d8_device_load32(D3D8_DEV_CURSOR);
    d3d8_device_store32(D3D8_DEV_WRAP_COUNT, 6u);
    const uint32_t fence = d3d8_gpu_fence_insert(3u);
    CHECK_EQ_U32(fence, 5u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_FENCE), 7u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), packet_at + 0x20u);
    CHECK_EQ_U32(load(packet_at), 0x0004A310u);
    CHECK_EQ_U32(load(packet_at + 4u), (((packet_at << 3) | (fence & 0x1Fu)) << 2) | 2u);
    CHECK_EQ_U32(load(packet_at + 8u), 0x00041D70u);
    CHECK_EQ_U32(load(packet_at + 12u), fence);
    CHECK_EQ_U32(load(packet_at + 16u), 0x00041D90u);
    CHECK_EQ_U32(load(packet_at + 20u), 0u);
    CHECK_EQ_U32(load(packet_at + 24u), 0x00041D90u);
    CHECK_EQ_U32(load(packet_at + 28u), 0u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_HISTORY_TABLE + ((fence >> 1) & 0x3Fu) * 8u + 4u), packet_at);
    CHECK_EQ_U32(load(d3d8_device_load32(D3D8_DEV_HISTORY_RING) + 8u), fence);
    CHECK_EQ_U32(load(d3d8_device_load32(D3D8_DEV_HISTORY_RING) + 12u), packet_at);
    CHECK_EQ_U32(d3d8_gpu_stream_count(), 1u); /* flags 3 does not kick: nothing recorded yet */
    d3d8_gpu_fence_wait(fence, 0u);
    CHECK_EQ_U32(load(d3d8_device_load32(D3D8_DEV_SEMAPHORE)), fence);
    CHECK_EQ_U32(d3d8_gpu_get_stats().fence_waits_blocked, 1u);
    /* The wait's kick handed the packet to the consumer: one software command on subchannel 5 and three 3D
     * ones, in order, after the pair already held. */
    CHECK_EQ_U32(d3d8_gpu_stream_count(), 5u);
    CHECK_EQ_U32(d3d8_gpu_stream_at(1).subchannel, 5u);
    CHECK_EQ_U32(d3d8_gpu_stream_at(1).method, 0x310u);
    CHECK_EQ_U32(d3d8_gpu_stream_at(2).subchannel, 0u);
    CHECK_EQ_U32(d3d8_gpu_stream_at(2).method, 0x1D70u);
    CHECK_EQ_U32(d3d8_gpu_stream_at(2).data, fence);
    CHECK_EQ_U32(d3d8_gpu_stream_at(3).method, 0x1D90u);
    CHECK_EQ_U32(d3d8_gpu_stream_at(4).method, 0x1D90u);
    CHECK_EQ_U32(d3d8_gpu_get_stats().commands_other_subchannel, 6u); /* 5 above and this packet's */
    CHECK_EQ_U32(d3d8_gpu_get_stats().emissions_elided, 0u);          /* the packet is written now */
    d3d8_gpu_stream_discard(4u);
    /* The fence counter is odd in every real flow (it starts at 5 and moves by 2), which makes bit 2 of the
     * notification (fence bit 0 shifted by 2) always set. An even value, which only a transplanted state has,
     * shows the wrap count is folded in modulo FOUR: wrap 7 adds bits 0 and 1 and not bit 2. */
    d3d8_device_store32(D3D8_DEV_FENCE, 0x26u);
    d3d8_device_store32(D3D8_DEV_WRAP_COUNT, 7u);
    const uint32_t even_at = d3d8_device_load32(D3D8_DEV_CURSOR);
    (void)d3d8_gpu_fence_insert(2u);
    CHECK_EQ_U32(load(even_at + 4u), (((even_at << 3) | 6u) << 2) | 3u);
    d3d8_device_store32(D3D8_DEV_FENCE, 9u);
    d3d8_device_store32(D3D8_DEV_WRAP_COUNT, 0u);
    d3d8_gpu_kick();
    d3d8_gpu_stream_discard(d3d8_gpu_stream_count() - 1u);
    kernel_clock_reset();
    d3d8_device_store32(0x1DDCu, 0x00400000u);
    d3d8_gpu_wait_vblank();
    CHECK(kernel_clock_peek() == KERNEL_CLOCK_FREQUENCY_HZ / 60u);
    d3d8_device_store32(0x1DDCu, 0u);
    d3d8_gpu_wait_vblank();
    CHECK(kernel_clock_peek() == KERNEL_CLOCK_FREQUENCY_HZ / 60u +
                                KERNEL_CLOCK_FREQUENCY_HZ / 50u);
    CHECK_EQ_U32(d3d8_gpu_vblank_count(), 2u);
    d3d8_pushbuffer_emit_pair(0x000C0100u, 0u); /* count exceeds range */
    d3d8_gpu_kick();
    CHECK_EQ_U32(d3d8_gpu_get_stats().malformed_dwords, 1u);
    CHECK_EQ_U32(d3d8_gpu_stream_count(), 1u);
    /* Force rollover with a full tail; the old-style wrap jump is excluded from recording. The GPU is
     * kicked at the tail first (T391): a refill with the GPU still at the start of the ring would wait for
     * it, which the port refuses. */
    const uint32_t end = d3d8_device_load32(D3D8_DEV_PB_END);
    d3d8_pushbuffer_end(end - 8u);
    d3d8_gpu_kick();
    CHECK_EQ_U32(d3d8_pushbuffer_put(), end - 8u);
    d3d8_device_store32(D3D8_DEV_LIMIT, end - 8u);
    const uint32_t wraps_before = d3d8_device_load32(D3D8_DEV_WRAP_COUNT);
    const uint64_t fences_before = d3d8_gpu_get_stats().fences_inserted;
    d3d8_pushbuffer_emit_pair(0x00040100u, 0xABCDEFu);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_WRAP_COUNT), wraps_before + 1u);
    /* The refill ends with a fence (flags 3) and a kick: the packet is at the base, the cursor moved past it,
     * and the pair was written after it. The recording holds the fence's four commands, then the pair. */
    const uint32_t base = d3d8_device_load32(D3D8_DEV_PB_BASE);
    CHECK_EQ_U32(d3d8_gpu_get_stats().fences_inserted, fences_before + 1u);
    CHECK_EQ_U32(load(base), 0x0004A310u);
    CHECK_EQ_U32(load(base + 8u), 0x00041D70u);
    CHECK_EQ_U32(d3d8_pushbuffer_put(), base + 0x20u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), base + 0x28u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_GET_SHADOW), end - 8u);
    d3d8_gpu_kick();
    CHECK_EQ_U32(d3d8_gpu_stream_count(), 6u);
    CHECK_EQ_U32(d3d8_gpu_stream_at(1).subchannel, 5u);
    CHECK_EQ_U32(d3d8_gpu_stream_at(5).data, 0xABCDEFu);
    /* Device flag 0x800 sets 0x1000 and kicks instead of inserting a fence. */
    d3d8_pushbuffer_end(end - 8u);
    d3d8_gpu_kick();
    d3d8_device_store32(D3D8_DEV_LIMIT, end - 8u);
    d3d8_device_store32(D3D8_DEV_FLAGS, d3d8_device_load32(D3D8_DEV_FLAGS) | 0x800u);
    const uint32_t counter = d3d8_device_load32(D3D8_DEV_FENCE);
    d3d8_pushbuffer_emit_pair(0x00040100u, 0x13572468u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_FENCE), counter);
    CHECK((d3d8_device_load32(D3D8_DEV_FLAGS) & 0x1000u) != 0u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), base + 8u);
    CHECK_EQ_U32(d3d8_pushbuffer_put(), base);
    /* Ring rollover must not feed its jump word to the method decoder. */
    CHECK_EQ_U32(d3d8_gpu_get_stats().malformed_dwords, 1u);
    environment_end();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
