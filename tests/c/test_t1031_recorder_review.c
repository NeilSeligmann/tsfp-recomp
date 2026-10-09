/* SPDX-License-Identifier: GPL-3.0-or-later
 * Host recorder/index controls; no NV2A completion or query result is supplied. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks,failures,allocation_attempts,relocations;
#define CHECK(x) do {checks++;if(!(x)){failures++;if(failures<30)fprintf(stderr,"FAIL %u %s\n",__LINE__,#x);}}while(0)
#ifdef T1031_CANDIDATE
static unsigned frees;
static bool fail_next;
static void *heap;
static size_t heap_bytes;

static void *controlled_malloc(size_t bytes)
{
    allocation_attempts++;
    if(fail_next){fail_next=false;return NULL;}
    CHECK(heap==NULL);heap=malloc(bytes);heap_bytes=bytes;return heap;
}
static void *controlled_realloc(void *old,size_t bytes)
{
    allocation_attempts++;
    if(fail_next){fail_next=false;return NULL;}
    CHECK(old==heap && bytes>heap_bytes);
    void *fresh=malloc(bytes);if(!fresh)return NULL;
    memcpy(fresh,old,heap_bytes);free(old);heap=fresh;heap_bytes=bytes;relocations++;return fresh;
}
static void controlled_free(void *pointer)
{
    CHECK(pointer==heap && heap!=NULL);free(pointer);heap=NULL;heap_bytes=0;frees++;
}
/* Only recorder allocations are redirected. These real private allocations force
 * every realloc to relocate; failure is a declared synthetic allocator response. */
#define malloc controlled_malloc
#define realloc controlled_realloc
#define free controlled_free
#endif
#include "d3d8_gpu.c"
#undef malloc
#undef realloc
#undef free

static uint32_t packet[32];
/* Controlled readable input transport only; no guest pointer/fault fidelity. */
uint32_t d3d8_guest_load32(uint32_t address)
{
    CHECK(address>=0x1000u && address<0x1080u && address%4u==0u);
    return packet[(address-0x1000u)/4u];
}
static unsigned observations;
static size_t observed_count;
static void observer(void){observations++;observed_count=d3d8_gpu_stream_count();}
static uint32_t datum(size_t logical){return (uint32_t)logical ^ 0xA394170Bu;}
static void append(size_t first,size_t count)
{
    for(size_t i=first;i<first+count;i++)record((uint32_t)(i%7u),(uint32_t)(i%256u)*4u,datum(i));
}
static void verify(size_t first,size_t count)
{
    CHECK(d3d8_gpu_stream_count()==count);
    for(size_t i=0;i<count;i++) {
        d3d8_gpu_command c=d3d8_gpu_stream_at(i);size_t logical=first+i;
        CHECK(c.data==datum(logical) && c.method==(uint32_t)(logical%256u)*4u && c.subchannel==logical%7u);
    }
    d3d8_gpu_command absent=d3d8_gpu_stream_at(count);
    CHECK(absent.method==0u && absent.data==0u && absent.subchannel==0u);
}
static void parser(void)
{
    d3d8_gpu_reset();d3d8_gpu_set_recorded_observer(observer);
    /* Literal headers: increasing3 on sub0, nonincrementing2 on sub5,
     * padding, draw-like BEGIN_END marker. Values/expected triples are literal. */
    uint32_t words[]={0x000C0200u,11u,12u,13u,0x4008A310u,21u,22u,0u,0x000417FCu,4u};
    memcpy(packet,words,sizeof words);consume(NULL,0x1000u,0x1000u+sizeof words);
    CHECK(observations==1 && observed_count==6u);
    static const d3d8_gpu_command expected[]={{0x200u,11u,0u},{0x204u,12u,0u},{0x208u,13u,0u},
      {0x310u,21u,5u},{0x310u,22u,5u},{0x17FCu,4u,0u}};
    for(size_t i=0;i<6;i++){d3d8_gpu_command c=d3d8_gpu_stream_at(i);CHECK(memcmp(&c,&expected[i],sizeof c)==0);}
    CHECK(d3d8_gpu_get_stats().dwords_consumed==10u);
    CHECK(d3d8_gpu_get_stats().commands_other_subchannel==2u);
    d3d8_gpu_reset();packet[0]=0x000C0200u;packet[1]=0x76543210u;
    consume(NULL,0x1000u,0x1008u);CHECK(observations==2 && observed_count==0u);
    CHECK(d3d8_gpu_get_stats().dwords_consumed==1u && d3d8_gpu_get_stats().malformed_dwords==1u);
    packet[0]=0x00040200u;packet[1]=89u;packet[2]=0x000C0200u;packet[3]=90u;
    consume(NULL,0x1000u,0x1010u);CHECK(d3d8_gpu_stream_count()==1u);
    CHECK(d3d8_gpu_stream_at(0).data==89u && d3d8_gpu_stream_at(0).method==0x200u);
    CHECK(d3d8_gpu_get_stats().malformed_dwords==2u);
    packet[0]=1u;packet[1]=0x00040200u;packet[2]=777u;
    consume(NULL,0x1000u,0x100Cu);CHECK(d3d8_gpu_stream_count()==1u);
    CHECK(d3d8_gpu_get_stats().malformed_dwords==3u);
    d3d8_gpu_reset();consume(NULL,0x1000u,0x1000u);CHECK(observed_count==0u);
    CHECK(d3d8_gpu_get_stats().dwords_consumed==0u);d3d8_gpu_set_recorded_observer(NULL);
}
int main(void)
{
    parser();d3d8_gpu_reset();uint64_t epoch=d3d8_gpu_reset_count();
    append(0u,65539u);verify(0u,65536u);
    CHECK(d3d8_gpu_get_stats().commands_dropped==3u && d3d8_gpu_get_stats().commands_recorded==65536u);
    CHECK(allocation_attempts==0u);d3d8_gpu_stream_discard(17u);verify(17u,65519u);
    d3d8_gpu_stream_discard(SIZE_MAX);CHECK(d3d8_gpu_stream_count()==0u);
#ifdef T1031_CANDIDATE
    CHECK(d3d8_gpu_stream_limit()==65536u && d3d8_gpu_stream_capacity()==65536u);
    CHECK(!d3d8_gpu_set_stream_limit(4194304u)); /* prior drops remain evidence even after discard */
    d3d8_gpu_reset();CHECK(d3d8_gpu_reset_count()>epoch);
    CHECK(!d3d8_gpu_set_stream_limit(65535u) && !d3d8_gpu_set_stream_limit(4194305u));
    CHECK(d3d8_gpu_set_stream_limit(4194304u));
    append(0u,65536u);fail_next=true;append(65536u,1u);
    CHECK(d3d8_gpu_get_stats().stream_allocation_failures==1u && d3d8_gpu_stream_count()==65536u);
    append(65537u,1u); /* input65536 was really dropped */
    CHECK(d3d8_gpu_stream_at(65536u).data==datum(65537u));
    CHECK(d3d8_gpu_get_stats().commands_dropped==1u);
    d3d8_gpu_stream_discard(SIZE_MAX);d3d8_gpu_reset();CHECK(heap==NULL);
    CHECK(d3d8_gpu_stream_limit()==4194304u);
    /* A failed moving realloc retains the complete earlier recording. */
    append(0u,131072u);fail_next=true;append(131072u,1u);
    CHECK(d3d8_gpu_stream_count()==131072u && d3d8_gpu_stream_capacity()==131072u);
    CHECK(d3d8_gpu_get_stats().stream_allocation_failures==1u && d3d8_gpu_get_stats().commands_dropped==1u);
    verify(0u,131072u);append(131073u,1u);
    CHECK(d3d8_gpu_stream_at(131072u).data==datum(131073u));
    CHECK(d3d8_gpu_get_stats().commands_dropped==1u);
    d3d8_gpu_reset();CHECK(heap==NULL);
    /* Four-times-growth plus forced relocation preserves every event/draw marker index. */
    append(0u,524289u);verify(0u,524289u);
    CHECK(relocations>=3u && d3d8_gpu_stream_capacity()==1048576u);
    size_t event=65536u,draw=131072u,end=524289u;
    CHECK(d3d8_gpu_stream_at(event).data==datum(event));
    CHECK(d3d8_gpu_stream_at(draw).data==datum(draw));
    d3d8_gpu_stream_discard(65537u);event=0u;draw-=65537u;end-=65537u;
    CHECK(d3d8_gpu_stream_at(draw).data==datum(131072u));verify(65537u,end);
    append(524289u,4194304u-end); /* fill the exact configured cap */
    CHECK(d3d8_gpu_stream_capacity()==4194304u && d3d8_gpu_stream_count()==4194304u);
    verify(65537u,4194304u);
    size_t attempts=allocation_attempts;append(4259841u,2u);
    CHECK(allocation_attempts==attempts && d3d8_gpu_get_stats().stream_limit_failures==2u);
    CHECK(d3d8_gpu_get_stats().commands_dropped==2u && d3d8_gpu_get_stats().stream_commands_peak==4194304u);
    d3d8_gpu_reset();CHECK(heap==NULL && d3d8_gpu_stream_count()==0u);
    CHECK(d3d8_gpu_stream_capacity()==65536u && d3d8_gpu_stream_limit()==4194304u);
    CHECK(d3d8_gpu_get_stats().commands_dropped==0u && d3d8_gpu_get_stats().stream_commands_peak==0u);
    CHECK(d3d8_gpu_set_stream_limit(65541u));append(0u,65543u);
    CHECK(d3d8_gpu_stream_capacity()==65541u && d3d8_gpu_get_stats().stream_limit_failures==2u);
    verify(0u,65541u);d3d8_gpu_reset();CHECK(heap==NULL && frees>0u);
#else
    (void)epoch;
#endif
    printf("T1031 %u checks %u failures, %u relocations\n",checks,failures,relocations);
    return failures?1:0;
}
