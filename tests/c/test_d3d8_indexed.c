/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_indexed.h"
#include "d3d8_gpu.h"

static uint32_t setup(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    store(D3D8_DEVICE_POINTER_SLOT,D3D8_DEVICE_BASE);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE,0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE,0x10000u);
    CHECK(d3d8_gpu_create()); CHECK(d3d8_pushbuffer_create());
    map_fixed(0x00A00000u,0x10000u);
    for(uint32_t i=0u;i<0x10000u;i++)store_byte(0x00A00000u+i,(uint8_t)i);
    return load(D3D8_DEVICE_BASE);
}
static void test_small_packets(void)
{
    for(uint32_t count=0u;count<4u;count++) {
        const uint32_t start=setup();
        store(D3D8_DEVICE_BASE+8u,0x400u);
        CHECK_EQ_U32(d3d8_draw_indexed_vertices(8u,count,count?0xA00001u:0u),0xC00u);
        CHECK_EQ_U32(load(D3D8_DEVICE_BASE+8u),0x400u);
        CHECK_EQ_U32(load(start),0x417FCu); CHECK_EQ_U32(load(start+4u),8u);
        CHECK_EQ_U32(load(start+8u),0x40001800u+((count>>1u)<<18u));
        uint32_t tail=start+12u+(count>>1u)*4u;
        if(count>=2u)CHECK_EQ_U32(load(start+12u),0x04030201u);
        if(count&1u) {
            CHECK_EQ_U32(load(tail),0x41808u);
            CHECK_EQ_U32(load(tail+4u),count==1u?0x0201u:0x0605u);
            tail+=8u;
        }
        CHECK_EQ_U32(load(tail),0x417FCu);CHECK_EQ_U32(load(tail+4u),0u);
        CHECK_EQ_U32(load(D3D8_DEVICE_BASE),tail+8u);
        CHECK_EQ_U32(load(0xA00000u),0x03020100u);
        CHECK_EQ_U32(load(0xA0FFFCu),0xFFFEFDFCu);
        environment_end();
    }
}
static void test_chunk_boundary(void)
{
    uint32_t start=setup();
    start+=((0u-start-8u)&31u);
    store(D3D8_DEVICE_BASE,start);
    CHECK_EQ_U32(d3d8_draw_indexed_vertices(8u,1022u,0xA00000u),0x800u);
    CHECK_EQ_U32(load(start+8u),0x47FC1800u);
    CHECK_EQ_U32(load(start+12u),0x03020100u);
    CHECK_EQ_U32(load(start+2056u),0x40001800u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE),start+2068u);
    environment_end();
}
static void test_atomic_refusals(void)
{
    const uint32_t start=setup();
    store(start,0xDEADBEEFu);
    store(D3D8_GLOBAL_DIRTY_MASK,0xFFFFFFFFu);
    RUN_EXPECTING_FATAL((void)d3d8_draw_indexed_vertices(8u,1u,0xA00000u));
    CHECK(fatal_seen);CHECK_EQ_U32(fatal_address,0x3D4FB0u);
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK),0xFFFFFFFFu);
    CHECK_EQ_U32(load(start),0xDEADBEEFu);
    store(D3D8_GLOBAL_DIRTY_MASK,0u);
    RUN_EXPECTING_FATAL((void)d3d8_draw_indexed_vertices(8u,2u,0xA0FFFFu));
    CHECK(fatal_seen);CHECK_EQ_U32(load(start),0xDEADBEEFu);
    const uint32_t limit=load(D3D8_DEVICE_BASE+4u);
    store(D3D8_DEVICE_BASE+4u,start+0x624u); /* 0x209*4 == limit+0x200-start: the sized reservation refills (T546) */
    const uint32_t fence=load(D3D8_DEVICE_BASE+0x2Cu);
    /* A refill under device flag 0x800 inserts no fence packet and leaves flag 0x1000, which the draw answers with one
     * fence at its end (0x003D5298): the draw is 20 bytes (header 8, an empty group 4, the end 8), then the 32-byte
     * fence packet, and the result is the fence it inserted. */
    CHECK_EQ_U32(d3d8_draw_indexed_vertices(8u,0u,0u),fence);
    CHECK_EQ_U32(load(start),0x417FCu);CHECK_EQ_U32(load(start+4u),8u);
    CHECK_EQ_U32(load(start+8u),0x40001800u);
    CHECK_EQ_U32(load(start+12u),0x417FCu);CHECK_EQ_U32(load(start+16u),0u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE),start+20u+D3D8_PUSHBUFFER_FENCE_PACKET_BYTES);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+0x2Cu),fence+2u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+8u)&0x1800u,0u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+4u),start+0x10000u-0x204u);
    store(start,0xDEADBEEFu);store(D3D8_DEVICE_BASE,start);
    store(D3D8_DEVICE_BASE+4u,limit);
    RUN_EXPECTING_FATAL((void)d3d8_draw_indexed_vertices(8u,1u,start));
    CHECK(fatal_seen);CHECK_EQ_U32(load(start),0xDEADBEEFu);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE),start);CHECK_EQ_U32(load(D3D8_DEVICE_BASE+8u),0u);
    environment_end();
}
static void test_kickoff_flag_at_the_entry(void)
{
    /* T546: flag 0x1000 left by an earlier refill under flag 0x800 is kept through the draw (0x003D5076 only ORs 0x800) and
     * answered with one fence at its end, whether or not this draw refilled (it used to be refused). */
    const uint32_t start=setup();
    store(D3D8_DEVICE_BASE+8u,0x1000u);
    const uint32_t fence=load(D3D8_DEVICE_BASE+0x2Cu);
    CHECK_EQ_U32(d3d8_draw_indexed_vertices(8u,0u,0u),fence);
    CHECK_EQ_U32(load(start),0x417FCu);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE),start+20u+D3D8_PUSHBUFFER_FENCE_PACKET_BYTES);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+0x2Cu),fence+2u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+8u)&0x1800u,0u);
    environment_end();
}
static void test_dispatch(void)
{
    (void)setup();
    const d3d8_surface_entry rows[]={{0x003D5050u,NULL,1u}};
    d3d8_hle_init(rows,1u);CHECK_EQ_U32(d3d8_indexed_register(),1u);
    const uint32_t args[]={8u,0u,0u};
    CHECK_EQ_U32(call_stdcall(0x3D5050u,args,3u),0x800u);
    environment_end();
}
int main(void)
{
    test_small_packets();test_chunk_boundary();test_atomic_refusals();test_kickoff_flag_at_the_entry();test_dispatch();
    printf("indexed draw: %d checks, %d failures\n",checks,failures);
    return failures?EXIT_FAILURE:EXIT_SUCCESS;
}
