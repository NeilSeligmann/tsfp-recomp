/* T1029: consumers receive command values, not references into reallocatable storage. */
#include "test_d3d8_support.h"

#include "d3d8_gpu.h"

static void configure_device(size_t limit)
{
    d3d8_gpu_reset();
    CHECK(d3d8_gpu_set_stream_limit(limit));
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x800000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x400000u);
    CHECK(d3d8_gpu_create());
    CHECK(d3d8_pushbuffer_create());
}

static void emit_pairs(uint32_t first, size_t count)
{
    for (size_t index = 0u; index < count; index++) {
        d3d8_pushbuffer_emit_pair(0x00047778u, first + (uint32_t)index);
    }
    d3d8_gpu_kick();
}

static void check_command(d3d8_gpu_command command, uint32_t value)
{
    CHECK_EQ_U32(command.subchannel, 3u);
    CHECK_EQ_U32(command.method, 0x1778u);
    CHECK_EQ_U32(command.data, value);
}

int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    d3d8_gpu_reset();
    CHECK_EQ_U32(d3d8_gpu_stream_capacity(), D3D8_GPU_STREAM_CAPACITY);
    CHECK_EQ_U32(d3d8_gpu_stream_limit(), D3D8_GPU_STREAM_CAPACITY);
    configure_device(D3D8_GPU_STREAM_MAX_CAPACITY);

    emit_pairs(0x10000000u, D3D8_GPU_STREAM_CAPACITY + 1u);
    CHECK_EQ_U32(d3d8_gpu_stream_count(), D3D8_GPU_STREAM_CAPACITY + 1u);
    CHECK(d3d8_gpu_stream_capacity() > D3D8_GPU_STREAM_CAPACITY);

    /* These are independent returned values straddling the original backing boundary. */
    const d3d8_gpu_command first = d3d8_gpu_stream_at(0u);
    const d3d8_gpu_command last_static = d3d8_gpu_stream_at(D3D8_GPU_STREAM_CAPACITY - 1u);
    const d3d8_gpu_command first_grown = d3d8_gpu_stream_at(D3D8_GPU_STREAM_CAPACITY);
    check_command(first, 0x10000000u);
    check_command(last_static, 0x1000FFFFu);
    check_command(first_grown, 0x10010000u);

    /* Prefix compaction changes stream indices, while saved command values remain snapshots. */
    d3d8_gpu_stream_discard(D3D8_GPU_STREAM_CAPACITY);
    CHECK_EQ_U32(d3d8_gpu_stream_count(), 1u);
    check_command(d3d8_gpu_stream_at(0u), 0x10010000u);
    check_command(first, 0x10000000u);
    check_command(last_static, 0x1000FFFFu);
    check_command(first_grown, 0x10010000u);

    /* Reset frees grown storage; caller-owned command values remain valid. */
    d3d8_gpu_reset();
    CHECK_EQ_U32(d3d8_gpu_stream_capacity(), D3D8_GPU_STREAM_CAPACITY);
    CHECK_EQ_U32(d3d8_gpu_stream_limit(), D3D8_GPU_STREAM_MAX_CAPACITY);
    check_command(first, 0x10000000u);
    check_command(last_static, 0x1000FFFFu);
    check_command(first_grown, 0x10010000u);

    environment_end();
    printf("T1029 recording snapshots: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
