/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_scissor.h"
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */
#define STREAM 0xD00000u
#define RECT 0xD02000u
static uint8_t state[D3D_REGION_BYTES],stream[64],rectangle[16];
static void snapshot(void)
{
    CHECK(kernel_guest_read_bytes(D3D_REGION_BASE,state,sizeof(state)));
    CHECK(kernel_guest_read_bytes(STREAM,stream,sizeof(stream)));
    CHECK(kernel_guest_read_bytes(RECT,rectangle,sizeof(rectangle)));
}
static void unchanged(void)
{
    CHECK(memcmp(state,kernel_guest_at(D3D_REGION_BASE,sizeof(state)),sizeof(state))==0);
    CHECK(memcmp(stream,kernel_guest_at(STREAM,sizeof(stream)),sizeof(stream))==0);
    CHECK(memcmp(rectangle,kernel_guest_at(RECT,sizeof(rectangle)),sizeof(rectangle))==0);
}
/* T546: a real ring. The original refills at the entry only, so the packet lands where the refill leaves the cursor. With
 * no GPU module attached the refill is the arithmetic alone (no fence packet): the limit becomes cursor + kickoff - 0x204,
 * the wrap writes the jump word (the base's low 28 bits plus one) at the old cursor and restarts at the base. */
#define KICKOFF 0x10000u
#define RING_BYTES 0x100000u
static unsigned hand_offs;
static uint32_t hand_off_begin,hand_off_end;
static void consume(void *context,uint32_t begin,uint32_t end)
{
    (void)context;hand_off_begin=begin;hand_off_end=end;hand_offs++;
}
static uint32_t seed_ring(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE,RING_BYTES);store(D3D8_GLOBAL_KICKOFF_SIZE,KICKOFF);
    CHECK(d3d8_pushbuffer_create());hand_offs=0u;d3d8_pushbuffer_set_consumer(consume,NULL);
    store(0x3E3F58u,D3D8_DEVICE_BASE);
    store(D3D8_DEVICE_BASE+0x95Cu,0u);store(D3D8_DEVICE_BASE+0x960u,0u);
    store(D3D8_DEVICE_BASE+0x954u,0u);store(D3D8_DEVICE_BASE+0x958u,0u);
    store(0x475CD4u,0x3F000000u);
    map_fixed(0xD02000u,0x1000u);
    const uint32_t rect[4]={0u,0u,640u,480u};
    CHECK(kernel_guest_write_bytes(RECT,rect,sizeof(rect)));
    return load(D3D8_DEVICE_BASE+0x24u);
}
static void place(uint32_t cursor,uint32_t limit)
{
    store(D3D8_DEVICE_BASE,cursor);store(D3D8_DEVICE_BASE+4u,limit);
}
static void test_refills(void)
{
    uint32_t ring=seed_ring();
    const uint32_t normal=ring+0x2000u;
    /* No refill: the limit is above the cursor, whatever the packet's end. */
    place(normal,normal+4u);
    CHECK_EQ_U32(d3d8_set_scissors(1u,0u,RECT),16u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE),normal+36u);CHECK_EQ_U32(load(D3D8_DEVICE_BASE+4u),normal+4u);
    CHECK_EQ_U32(hand_offs,0u);CHECK_EQ_U32(load(normal),0x80200u);
    /* At the limit, and past it: the refill, then the packet at the cursor it leaves. */
    for(unsigned past=0u;past<2u;past++){
        place(normal,past!=0u?normal-0x40u:normal);hand_offs=0u;
        store(normal,0xDEADBEEFu);
        CHECK_EQ_U32(d3d8_set_scissors(1u,0u,RECT),16u);
        /* The hand-off is [segment start, cursor): the second pass starts at the cursor, so there is nothing to hand over. */
        CHECK_EQ_U32(hand_offs,past!=0u?0u:1u);if(past==0u)CHECK_EQ_U32(hand_off_end,normal);
        CHECK_EQ_U32(load(D3D8_DEVICE_BASE+4u),normal+KICKOFF-D3D8_PUSHBUFFER_SLACK_BYTES);
        CHECK_EQ_U32(load(D3D8_DEVICE_BASE),normal+36u);CHECK_EQ_U32(load(normal),0x80200u);
    }
    /* The cursor near the end of the ring: the refill wraps, the jump word is at the old cursor, the packet at the base. */
    const uint32_t end=load(D3D8_DEVICE_BASE+0x28u),near_end=end-0x100u;
    place(near_end,near_end);hand_offs=0u;
    CHECK_EQ_U32(d3d8_set_scissors(1u,0u,RECT),16u);
    CHECK_EQ_U32(load(near_end),(ring&0x0FFFFFFFu)+1u);
    CHECK_EQ_U32(load(ring),0x80200u);CHECK_EQ_U32(load(D3D8_DEVICE_BASE),ring+36u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+4u),ring+KICKOFF-D3D8_PUSHBUFFER_SLACK_BYTES);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+0x40u),1u);
    /* The original reads the rectangle AFTER the refill wrote the jump word: a rectangle over it is refused by name before
     * a single write (the ring window and the cursor are as they were). */
    const uint32_t over_jump=near_end-8u;
    CHECK(kernel_guest_write_bytes(over_jump,(const uint32_t[4]){0u,0u,640u,480u},16u));
    place(near_end,near_end);store(near_end,0x12345678u);
    RUN_EXPECTING_FATAL((void)d3d8_set_scissors(1u,0u,over_jump));CHECK(fatal_seen);
    CHECK_EQ_U32(load(near_end),0x12345678u);CHECK_EQ_U32(load(D3D8_DEVICE_BASE),near_end);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+4u),near_end);
    environment_end();
}
int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);map_fixed(STREAM,0x3000u);
    store(0x3E3F58u,D3D8_DEVICE_BASE);
    store(D3D8_DEVICE_BASE,STREAM);store(D3D8_DEVICE_BASE+4u,STREAM+0x1000u);
    store(D3D8_DEVICE_BASE+0x95Cu,0u);store(D3D8_DEVICE_BASE+0x960u,0u);
    store(D3D8_DEVICE_BASE+0x954u,0u);store(D3D8_DEVICE_BASE+0x958u,0u);
    store(0x475CD4u,0x3F000000u);
    const uint32_t rect[4]={0u,0u,640u,480u};
    CHECK(kernel_guest_write_bytes(RECT,rect,sizeof(rect)));
    for(unsigned i=0u;i<16u;i++)store(STREAM+i*4u,0xAAAAAAAAu);
    CHECK_EQ_U32(d3d8_set_scissors(1u,0u,RECT),16u);
    const uint32_t packet[9]={0x80200u,0u,0u,0x402B4u,0u,0x402C0u,0u,0x402E0u,0u};
    for(unsigned i=0u;i<9u;i++)CHECK_EQ_U32(load(STREAM+i*4u),packet[i]);
    for(unsigned i=9u;i<16u;i++)CHECK_EQ_U32(load(STREAM+i*4u),0xAAAAAAAAu);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE),STREAM+36u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+0x1C00u),1u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+0x1C04u),0u);
    CHECK(memcmp(rect,kernel_guest_at(D3D8_DEVICE_BASE+0x1B80u,16u),16u)==0);
    store(D3D8_DEVICE_BASE,STREAM);snapshot();
    RUN_EXPECTING_FATAL((void)d3d8_set_scissors(0u,0u,RECT));CHECK(fatal_seen);unchanged();
    RUN_EXPECTING_FATAL((void)d3d8_set_scissors(1u,1u,RECT));CHECK(fatal_seen);unchanged();
    RUN_EXPECTING_FATAL((void)d3d8_set_scissors(1u,0u,0xFFFFFFF8u));CHECK(fatal_seen);unchanged();
    RUN_EXPECTING_FATAL((void)d3d8_set_scissors(1u,0u,STREAM));CHECK(fatal_seen);unchanged();
    RUN_EXPECTING_FATAL((void)d3d8_set_scissors(1u,0u,D3D8_DEVICE_BASE+0x1B80u));CHECK(fatal_seen);unchanged();
    RUN_EXPECTING_FATAL((void)d3d8_set_scissors(1u,0u,D3D8_DEVICE_BASE+0x2000u));
    CHECK(fatal_seen);unchanged();
    /* A command span in the unused device tail is still an unproven alias. */
    store(D3D8_DEVICE_BASE,D3D8_DEVICE_BASE+0x2000u);
    store(D3D8_DEVICE_BASE+4u,D3D8_DEVICE_BASE+0x2100u);snapshot();
    RUN_EXPECTING_FATAL((void)d3d8_set_scissors(1u,0u,RECT));CHECK(fatal_seen);unchanged();
    /* This packet ends BEFORE the device, but covers its global pointer slot. */
    store(D3D8_DEVICE_BASE,0x3E3F38u);store(D3D8_DEVICE_BASE+4u,0x3E3F5Cu);snapshot();
    RUN_EXPECTING_FATAL((void)d3d8_set_scissors(1u,0u,RECT));CHECK(fatal_seen);unchanged();
    store(D3D8_DEVICE_BASE,STREAM);
    /* 0x003D4490 checks `cursor < limit` once and never tests the packet's end: 35 bytes of room is room, the 36 bytes run
     * into the slack (T546, the port used to refuse this). */
    store(D3D8_DEVICE_BASE+4u,STREAM+35u);
    CHECK_EQ_U32(d3d8_set_scissors(1u,0u,RECT),16u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE),STREAM+36u);
    CHECK_EQ_U32(load(STREAM),0x80200u);
    store(D3D8_DEVICE_BASE,STREAM);
    /* At the limit the original refills. This environment has no ring (device+0x24 is 0), so the roll-over is refused
     * by name, before the packet or the cursor is written. */
    store(D3D8_DEVICE_BASE+4u,STREAM);snapshot();
    RUN_EXPECTING_FATAL((void)d3d8_set_scissors(1u,0u,RECT));CHECK(fatal_seen);unchanged();
    store(D3D8_DEVICE_BASE+4u,STREAM+0x1000u);snapshot();
    uint16_t cw,changed;
    __asm__ volatile("fnstcw %0":"=m"(cw));changed=(uint16_t)((cw&~0xC00u)|0x400u);
    __asm__ volatile("fldcw %0"::"m"(changed));
    RUN_EXPECTING_FATAL((void)d3d8_set_scissors(1u,0u,RECT));CHECK(fatal_seen);
    __asm__ volatile("fldcw %0"::"m"(cw));unchanged();
    uint32_t mxcsr,mxchanged;
    __asm__ volatile("stmxcsr %0":"=m"(mxcsr));mxchanged=mxcsr|0x8040u;
    __asm__ volatile("ldmxcsr %0"::"m"(mxchanged));
    RUN_EXPECTING_FATAL((void)d3d8_set_scissors(1u,0u,RECT));CHECK(fatal_seen);
    __asm__ volatile("ldmxcsr %0"::"m"(mxcsr));unchanged();
    /* Packet crossing into a protected page must preserve even its first word. */
    store(D3D8_DEVICE_BASE,STREAM+0xFF0u);store(D3D8_DEVICE_BASE+4u,STREAM+0x2000u);
    store(STREAM+0xFF0u,0x12345678u);
    CHECK(mprotect((void *)(uintptr_t)(STREAM+0x1000u),0x1000u,PROT_READ)==0);
    RUN_EXPECTING_FATAL((void)d3d8_set_scissors(1u,0u,RECT));CHECK(fatal_seen);
    CHECK_EQ_U32(load(STREAM+0xFF0u),0x12345678u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE),STREAM+0xFF0u);
    CHECK(mprotect((void *)(uintptr_t)(STREAM+0x1000u),0x1000u,PROT_READ|PROT_WRITE)==0);
    store(D3D8_DEVICE_BASE,STREAM);store(D3D8_DEVICE_BASE+4u,STREAM+0x1000u);snapshot();
    const uint32_t pages[]={STREAM,0x3E3000u,0x3E5000u};
    for(unsigned i=0u;i<3u;i++){
        CHECK(mprotect((void *)(uintptr_t)pages[i],0x1000u,PROT_READ)==0);
        RUN_EXPECTING_FATAL((void)d3d8_set_scissors(1u,0u,RECT));CHECK(fatal_seen);
        CHECK(mprotect((void *)(uintptr_t)pages[i],0x1000u,PROT_READ|PROT_WRITE)==0);
        unchanged();
    }
    CHECK(mprotect((void *)(uintptr_t)RECT,0x1000u,PROT_NONE)==0);
    RUN_EXPECTING_FATAL((void)d3d8_set_scissors(1u,0u,RECT));CHECK(fatal_seen);
    CHECK(mprotect((void *)(uintptr_t)RECT,0x1000u,PROT_READ|PROT_WRITE)==0);unchanged();
    const d3d8_surface_entry row={0x3D4470u,"SetScissors",1u};
    CHECK(d3d8_hle_init(&row,1u));CHECK_EQ_U32(d3d8_scissor_register(),1u);
    const uint32_t args[3]={1u,0u,RECT};kernel_call_frame frame;
    memset(&frame,0,sizeof(frame));CHECK(kernel_frame_build(&frame,call_scratch,0x100u,args,3u));
    CHECK_EQ_U32(d3d8_hle_call(0x3D4470u,&frame),16u);
    d3d8_hle_shutdown();environment_end();
    test_refills();
    printf("%d checks, %d failures\n",checks,failures);return failures==0?0:1;
}
