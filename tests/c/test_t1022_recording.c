/* Host storage controls, not fabricated NV2A/query completion. */
#include "test_d3d8_support.h"
#include "d3d8_gpu.h"
#include <stdlib.h>

static bool deny_allocation;
void *__real_malloc(size_t size);
void *__real_realloc(void *pointer, size_t size);
void *__wrap_malloc(size_t size) { return deny_allocation ? NULL : __real_malloc(size); }
void *__wrap_realloc(void *pointer, size_t size) { return deny_allocation ? NULL : __real_realloc(pointer,size); }

static void setup(size_t limit)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    d3d8_gpu_reset();
    CHECK(d3d8_gpu_set_stream_limit(limit));
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE,0x800000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE,0x400000u);
    CHECK(d3d8_gpu_create());
    CHECK(d3d8_gpu_stream_limit()==limit); /* CreateDevice resets the recorder. */
    CHECK(d3d8_pushbuffer_create());
}
static void emit(size_t begin,size_t count)
{
    for(size_t i=0u;i<count;i++)
        d3d8_pushbuffer_emit_pair(0x00047778u,(uint32_t)(begin+i));
    d3d8_gpu_kick();
}
static void verify(size_t first,size_t count)
{
    for(size_t i=0u;i<count;i++) {
        const d3d8_gpu_command command=d3d8_gpu_stream_at(i);
        CHECK(command.subchannel==3u && command.method==0x1778u && command.data==first+i);
    }
}
int main(void)
{
    CHECK(!d3d8_gpu_set_stream_limit(0u));
    CHECK(!d3d8_gpu_set_stream_limit(D3D8_GPU_STREAM_CAPACITY-1u));
    CHECK(!d3d8_gpu_set_stream_limit(D3D8_GPU_STREAM_MAX_CAPACITY+1u));
    setup(D3D8_GPU_STREAM_CAPACITY);
    emit(0u,D3D8_GPU_STREAM_CAPACITY+3u);
    CHECK(d3d8_gpu_stream_count()==D3D8_GPU_STREAM_CAPACITY);
    CHECK(d3d8_gpu_get_stats().commands_dropped==3u);
    CHECK(d3d8_gpu_get_stats().stream_limit_failures==3u);
    CHECK(d3d8_gpu_get_stats().stream_growths==0u);
    verify(0u,D3D8_GPU_STREAM_CAPACITY);
    environment_end();

    setup(D3D8_GPU_STREAM_MAX_CAPACITY);
    emit(0u,200000u);
    CHECK(d3d8_gpu_stream_count()==200000u);
    CHECK(d3d8_gpu_get_stats().stream_growths==2u);
    CHECK(d3d8_gpu_stream_capacity()==262144u);
    CHECK(d3d8_gpu_get_stats().commands_dropped==0u);
    CHECK(d3d8_gpu_get_stats().stream_commands_peak==200000u);
    verify(0u,200000u);
    CHECK(!d3d8_gpu_set_stream_limit(D3D8_GPU_STREAM_CAPACITY));
    d3d8_gpu_stream_discard(100001u);
    CHECK(d3d8_gpu_stream_count()==99999u);
    verify(100001u,99999u);
    emit(200000u,170000u);
    CHECK(d3d8_gpu_stream_count()==269999u);
    CHECK(d3d8_gpu_get_stats().stream_growths==3u);
    verify(100001u,269999u);
    d3d8_gpu_stream_discard(d3d8_gpu_stream_count());
    CHECK(!d3d8_gpu_set_stream_limit(D3D8_GPU_STREAM_CAPACITY));
    d3d8_gpu_reset();
    CHECK(d3d8_gpu_stream_capacity()==D3D8_GPU_STREAM_CAPACITY);
    CHECK(d3d8_gpu_stream_limit()==D3D8_GPU_STREAM_MAX_CAPACITY);
    CHECK(d3d8_gpu_get_stats().stream_growths==0u);
    environment_end();

    setup(D3D8_GPU_STREAM_CAPACITY+17u);
    emit(0u,D3D8_GPU_STREAM_CAPACITY+20u);
    CHECK(d3d8_gpu_stream_capacity()==D3D8_GPU_STREAM_CAPACITY+17u);
    CHECK(d3d8_gpu_get_stats().commands_dropped==3u);
    CHECK(d3d8_gpu_get_stats().stream_limit_failures==3u);
    verify(0u,D3D8_GPU_STREAM_CAPACITY+17u);
    environment_end();

    setup(D3D8_GPU_STREAM_MAX_CAPACITY);
    emit(0u,D3D8_GPU_STREAM_CAPACITY);
    deny_allocation=true;
    emit(D3D8_GPU_STREAM_CAPACITY,3u);
    deny_allocation=false;
    CHECK(d3d8_gpu_stream_count()==D3D8_GPU_STREAM_CAPACITY);
    CHECK(d3d8_gpu_get_stats().commands_dropped==3u);
    CHECK(d3d8_gpu_get_stats().stream_allocation_failures==3u);
    CHECK(d3d8_gpu_get_stats().stream_limit_failures==0u);
    verify(0u,D3D8_GPU_STREAM_CAPACITY);
    emit(D3D8_GPU_STREAM_CAPACITY+3u,1u);
    CHECK(d3d8_gpu_stream_count()==D3D8_GPU_STREAM_CAPACITY+1u);
    CHECK(d3d8_gpu_stream_at(D3D8_GPU_STREAM_CAPACITY).data==D3D8_GPU_STREAM_CAPACITY+3u);
    CHECK(d3d8_gpu_get_stats().commands_dropped==3u); /* Failure remains visible. */
    emit(D3D8_GPU_STREAM_CAPACITY+4u,D3D8_GPU_STREAM_CAPACITY-1u);
    CHECK(d3d8_gpu_stream_count()==131072u);
    CHECK(d3d8_gpu_stream_capacity()==131072u);
    const d3d8_gpu_command retained=d3d8_gpu_stream_at(131071u);
    deny_allocation=true;
    emit(131075u,3u);
    deny_allocation=false;
    CHECK(d3d8_gpu_stream_count()==131072u);
    CHECK(d3d8_gpu_stream_capacity()==131072u);
    CHECK(d3d8_gpu_stream_at(131071u).data==retained.data);
    CHECK(d3d8_gpu_get_stats().commands_dropped==6u);
    CHECK(d3d8_gpu_get_stats().stream_allocation_failures==6u);
    environment_end();
    d3d8_gpu_reset();
    CHECK(d3d8_gpu_set_stream_limit(D3D8_GPU_STREAM_CAPACITY));
    printf("T1022 recording: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
