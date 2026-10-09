/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "dsound_buffer.h"
#include "dsound_completion.h"
#include "dsound_hle.h"
#include <stdlib.h>
#include <sys/mman.h>
#include "probe_cache_wrap.h"
#define DATA 0x23000000u
static uint64_t now;
static uint64_t clock_now(void){return now;}
static uint8_t irql;
static bool current_irql(uint8_t *out){*out=irql;return true;}
int recomp_has_stop_boundary(uint32_t a)
{return a==0x408040u || a==0x406FA9u || a==0x406FF0u || a==0x406879u;}
static uint32_t buffer,device;
static dsound_buffer_snapshot snapshot(void)
{dsound_buffer_snapshot s;CHECK(dsound_buffer_get_snapshot(buffer,&s));return s;}
static void unchanged(dsound_buffer_snapshot *before)
{dsound_buffer_snapshot after=snapshot();CHECK(memcmp(before,&after,sizeof(after))==0);}
static void reject(uint32_t start,uint32_t length)
{dsound_buffer_snapshot before=snapshot();RUN_EXPECTING_FATAL((void)dsound_buffer_set_loop_region(buffer,start,length));CHECK(fatal_seen);CHECK_EQ_U32(fatal_address,0x407AD8u);unchanged(&before);}
static uint64_t position(void)
{dsound_completion_buffer_position p;CHECK(dsound_completion_buffer_get_position(buffer,&p));return p.position_samples;}
static void setup(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();
    map_fixed(0x412000u,4096u);map_fixed(0x4A1000u,4096u);map_fixed(0x581000u,4096u);map_fixed(0x583000u,4096u);map_fixed(DATA,65536u);
    for(unsigned i=0u;i<4u;i++)store(0x4A1CE0u+4u*i,0x406879u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);device=load(SCRATCH_DATA)-8u;
    dsound_buffer_set_enabled(true);dsound_buffer_set_irql_provider(current_irql);dsound_buffer_set_fatal(catching_fatal);
    dsound_completion_set_fatal(catching_fatal);dsound_completion_set_enabled(true);dsound_completion_set_clock(clock_now,22042u);
    CHECK_EQ_U32(dsound_buffer_register(),10u);CHECK(!dsound_hle_entry(0x407AD8u)->handler);
    dsound_buffer_set_completion(true,dsound_completion_buffer_started);
    dsound_buffer_set_completion_pause(dsound_completion_buffer_pause);dsound_buffer_set_completion_frequency(dsound_completion_buffer_frequency);dsound_buffer_set_completion_voice_running(dsound_completion_buffer_voice_running);
    CHECK_EQ_U32(dsound_buffer_register(),11u);CHECK(dsound_hle_entry(0x407AD8u)->handler!=NULL);
    const uint32_t desc[6]={24u,0u,0u,SCRATCH_DATA+320u,0u,0u};
    const uint8_t format[20]={0x69,0,1,0,0xE0,0xAB,0,0,0xAE,0x60,0,0,36,0,4,0,2,0,64,0};
    CHECK(kernel_guest_write_bytes(SCRATCH_DATA+256u,desc,sizeof(desc)));CHECK(kernel_guest_write_bytes(SCRATCH_DATA+320u,format,20u));
    CHECK_EQ_U32(dsound_buffer_create(device+8u,SCRATCH_DATA+256u,0x5818E8u,0u),0u);buffer=load(0x5818E8u);
    reject(0u,0u);CHECK_EQ_U32(dsound_buffer_set_data(buffer,DATA,1440u),0u);
    CHECK_EQ_U32(dsound_buffer_set_volume(buffer,-10000),0u);CHECK_EQ_U32(dsound_buffer_pause(buffer,0u),0u);CHECK_EQ_U32(dsound_buffer_set_frequency(buffer,22042u),0u);
}
static void controls(void)
{
    uint32_t parent[11],header[9];CHECK(kernel_guest_read_bytes(device,parent,sizeof(parent)));
    dsound_buffer_snapshot s=snapshot();CHECK(kernel_guest_read_bytes(s.header_address,header,sizeof(header)));
    CHECK_EQ_U32(dsound_buffer_set_loop_region(buffer,36u,72u),0u);s=snapshot();CHECK_EQ_U32(s.loop_start,36u);CHECK_EQ_U32(s.loop_length,72u);
    CHECK_EQ_U32(dsound_buffer_set_data(buffer,DATA,1440u),0u);s=snapshot();CHECK_EQ_U32(s.loop_start,36u);CHECK_EQ_U32(s.loop_length,72u);
    CHECK_EQ_U32(dsound_buffer_set_data(buffer,DATA+36u,1440u),0u);s=snapshot();CHECK_EQ_U32(s.loop_start,0u);CHECK_EQ_U32(s.loop_length,1440u);
    CHECK_EQ_U32(dsound_buffer_set_loop_region(buffer,36u,0u),0u);s=snapshot();CHECK_EQ_U32(s.loop_length,1404u);
    CHECK_EQ_U32(dsound_buffer_set_loop_region(buffer,0u,0u),0u);s=snapshot();CHECK_EQ_U32(s.loop_length,1440u);
    CHECK_EQ_U32(dsound_buffer_set_loop_region(buffer,0u,1441u),0x88780032u);unchanged(&s);
    const uint32_t bad[][2]={{1440u,0u},{1441u,0u},{0xFFFFFFFCu,8u},{1u,35u},{36u,1u}};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++)reject(bad[i][0],bad[i][1]);
    dsound_buffer_set_enabled(false);reject(0u,0u);dsound_buffer_set_enabled(true);
    dsound_buffer_set_completion(false,NULL);reject(0u,0u);dsound_buffer_set_completion(true,dsound_completion_buffer_started);
    dsound_buffer_set_completion_pause(dsound_completion_buffer_pause);dsound_buffer_set_completion_frequency(dsound_completion_buffer_frequency);dsound_buffer_set_completion_voice_running(dsound_completion_buffer_voice_running);
    irql=2u;reject(0u,0u);irql=0u;store(0x4124A8u,1u);
    RUN_EXPECTING_FATAL(CHECK_EQ_U32(dsound_buffer_set_loop_region(buffer,0u,0u),0x80004005u));CHECK(!fatal_seen);
    RUN_EXPECTING_FATAL(CHECK_EQ_U32(dsound_buffer_set_loop_region(0u,0xFFFFFFFFu,0xFFFFFFFFu),0x80004005u));CHECK(!fatal_seen);
    RUN_EXPECTING_FATAL(CHECK_EQ_U32(dsound_buffer_set_loop_region(0xDEADBEEFu,1u,1u),0x80004005u));CHECK(!fatal_seen);
    unchanged(&s);dsound_buffer_set_enabled(false);reject(0u,0u);dsound_buffer_set_enabled(true);
    dsound_buffer_set_completion(false,NULL);reject(0u,0u);
    dsound_buffer_set_completion(true,dsound_completion_buffer_started);
    dsound_buffer_set_completion_pause(dsound_completion_buffer_pause);
    dsound_buffer_set_completion_frequency(dsound_completion_buffer_frequency);
    dsound_buffer_set_completion_voice_running(dsound_completion_buffer_voice_running);
    irql=2u;reject(0u,0u);irql=0u;store(0x4124A8u,0u);
    for(unsigned i=0u;i<9u;i++){store(s.header_address+4u*i,header[i]^1u);RUN_EXPECTING_FATAL((void)dsound_buffer_set_loop_region(buffer,0u,0u));CHECK(fatal_seen);store(s.header_address+4u*i,header[i]);}
    s=snapshot();RUN_EXPECTING_FATAL((void)dsound_buffer_set_loop_region(buffer+4u,0u,0u));CHECK(fatal_seen);unchanged(&s);
    /* Dispatch at the actual sound start return address. Three arguments only. */
    const uint32_t args[4]={buffer,36u,72u,0xBADCAFEu};store(SCRATCH_DATA+0x800u,0x275AFu);
    CHECK(kernel_guest_write_bytes(SCRATCH_DATA+0x804u,args,sizeof(args)));
    kernel_call_frame frame={.stack_ptr=SCRATCH_DATA+0x800u};uint32_t result;
    result=dsound_hle_call(0x407AD8u,&frame);CHECK_EQ_U32(result,0u);
    s=snapshot();CHECK_EQ_U32(s.loop_start,36u);CHECK_EQ_U32(s.loop_length,72u);
    RUN_EXPECTING_FATAL((void)dsound_hle_call(0x407AD8u,NULL));CHECK(fatal_seen);unchanged(&s);
    map_fixed(0x23100000u,4096u);frame.stack_ptr=0x23100FF4u;
    store(frame.stack_ptr,0x275C0u);store(frame.stack_ptr+4u,buffer);store(frame.stack_ptr+8u,0u);
    RUN_EXPECTING_FATAL((void)dsound_hle_call(0x407AD8u,&frame));CHECK(fatal_seen);unchanged(&s);
    uint32_t actual[11];CHECK(kernel_guest_read_bytes(device,actual,sizeof(parent)));CHECK(memcmp(parent,actual,sizeof(parent))==0);
    CHECK(kernel_guest_read_bytes(s.header_address,actual,sizeof(header)));CHECK(memcmp(header,actual,sizeof(header))==0);
    CHECK(mprotect((void *)(uintptr_t)(s.header_address&~4095u),4096u,PROT_READ)==0);
    CHECK(mprotect((void *)(uintptr_t)(device&~4095u),4096u,PROT_READ)==0);
    CHECK_EQ_U32(dsound_buffer_set_loop_region(buffer,36u,72u),0u);
    CHECK(mprotect((void *)(uintptr_t)(s.header_address&~4095u),4096u,PROT_READ|PROT_WRITE)==0);
    CHECK(mprotect((void *)(uintptr_t)(device&~4095u),4096u,PROT_READ|PROT_WRITE)==0);
    CHECK_EQ_U32(dsound_completion_buffer_play(buffer,1u),0u);CHECK_EQ_U32(position(),0u);
    reject(0u,0u); /* T1209: the voice is running, a live loop change is the APU reprogram the model does not do. */
    now=191u;CHECK_EQ_U32(position(),191u);now=192u;CHECK_EQ_U32(position(),64u);now=192u+128u*1000000u+17u;CHECK_EQ_U32(position(),81u);
    CHECK_EQ_U32(dsound_buffer_pause(buffer,1u),0u);now+=10000u;CHECK_EQ_U32(position(),81u);CHECK_EQ_U32(dsound_buffer_pause(buffer,0u),0u);
    CHECK_EQ_U32(dsound_buffer_set_frequency(buffer,44084u),0u);CHECK_EQ_U32(position(),80u);now+=56u;CHECK_EQ_U32(position(),64u);
    CHECK_EQ_U32(dsound_completion_buffer_stop(buffer),0u);CHECK_EQ_U32(position(),64u);
    CHECK_EQ_U32(dsound_buffer_set_loop_region(buffer,36u,72u),0u); /* T1209: Stopped but still draining, the voice is no longer running */now+=1247u;CHECK_EQ_U32(position(),2558u);now+=1u;CHECK_EQ_U32(position(),2560u);
    CHECK_EQ_U32(dsound_completion_buffer_status(buffer,SCRATCH_DATA+0x900u),0u);CHECK_EQ_U32(load(SCRATCH_DATA+0x900u),0u);
    /* T1209: after Stop (and its drain) the original's SetLoopRegion makes no hardware write, like an unplayed buffer, so it is the record. */
    CHECK_EQ_U32(dsound_buffer_set_loop_region(buffer,0u,0u),0u);s=snapshot();CHECK_EQ_U32(s.loop_start,0u);CHECK_EQ_U32(s.loop_length,1440u);
    CHECK_EQ_U32(dsound_buffer_set_loop_region(buffer,36u,72u),0u);
    dsound_buffer_set_completion_voice_running(NULL);reject(0u,0u); /* no hook: a played buffer counts as running, as before T1209 */
    dsound_buffer_set_completion_voice_running(dsound_completion_buffer_voice_running);
    CHECK_EQ_U32(dsound_completion_buffer_play(buffer,0u),0u);now+=1280u;CHECK_EQ_U32(position(),2560u);
    dsound_completion_reset();now=0u;
    /* Empty interval at this resolution is a refusal, not frozen success. */
    CHECK_EQ_U32(dsound_buffer_set_loop_region(buffer,36u,36u),0u);dsound_completion_set_clock(clock_now,1u);
    RUN_EXPECTING_FATAL((void)dsound_completion_buffer_play(buffer,1u));CHECK(fatal_seen);
    dsound_completion_set_clock(clock_now,22042u);CHECK_EQ_U32(dsound_completion_buffer_play(buffer,1u),0u);
    /* Recycled lease never inherits region or playback state. */
    const uint64_t old_serial=s.lease.serial;
    CHECK(dsound_buffer_reset_checked());
    CHECK_EQ_U32(dsound_buffer_create(device+8u,SCRATCH_DATA+256u,0x5818E8u,0u),0u);buffer=load(0x5818E8u);
    s=snapshot();CHECK(s.lease.serial!=old_serial);CHECK_EQ_U32(s.loop_sets,0u);CHECK_EQ_U32(s.loop_start,0u);CHECK_EQ_U32(s.loop_length,0u);
    CHECK_EQ_U32(dsound_buffer_set_data(buffer,DATA,1440u),0u);s=snapshot();CHECK_EQ_U32(s.loop_length,1440u);
    CHECK_EQ_U32(dsound_buffer_set_loop_region(buffer,72u,36u),0u);
    CHECK_EQ_U32(dsound_buffer_set_volume(buffer,-10000),0u);CHECK_EQ_U32(dsound_buffer_pause(buffer,0u),0u);
    CHECK_EQ_U32(dsound_buffer_set_frequency(buffer,188u),0u);
    dsound_completion_set_clock(clock_now,10u);CHECK_EQ_U32(dsound_completion_buffer_play(buffer,1u),0u);
    s=snapshot();RUN_EXPECTING_FATAL((void)dsound_buffer_set_frequency(buffer,192000u));CHECK(fatal_seen);unchanged(&s);
}
/* T1209: a spatial startup buffer (flags 0x10, three 3D caches, data) takes the loop region like the ordinary one. */
static void spatial_loop(void)
{
    const uint32_t params[9]={0u,0u,0xFFFFF448u,0u,0u,0u,0u,0u,0u};
    uint8_t d[24]={0};const uint32_t words[6]={24u,16u,0u,SCRATCH_DATA+320u,0u,0u};memcpy(d,words,sizeof(d));
    CHECK(kernel_guest_write_bytes(SCRATCH_DATA+256u,d,sizeof(d)));
    CHECK_EQ_U32(dsound_buffer_create(device+8u,SCRATCH_DATA+256u,0x5835F0u,0u),0u);buffer=load(0x5835F0u);
    CHECK(kernel_guest_write_bytes(SCRATCH_DATA+0x500u,params,sizeof(params)));
    CHECK_EQ_U32(dsound_buffer_cache_i3dl2(buffer,SCRATCH_DATA+0x500u,0u),0u);
    CHECK_EQ_U32(dsound_buffer_cache_min_distance(buffer,0x3F800000u,0u),0u);
    CHECK_EQ_U32(dsound_buffer_cache_rolloff(buffer,0x64BC98u,1u,0u),0u);
    reject(0u,0u); /* no data recorded yet */
    CHECK_EQ_U32(dsound_buffer_set_data(buffer,DATA,1440u),0u);
    CHECK_EQ_U32(dsound_buffer_set_loop_region(buffer,0u,0u),0u);
    dsound_buffer_snapshot s=snapshot();CHECK_EQ_U32(s.loop_start,0u);CHECK_EQ_U32(s.loop_length,1440u);CHECK_EQ_U32(s.loop_sets,1u);
    CHECK_EQ_U32(dsound_buffer_set_loop_region(buffer,36u,72u),0u);s=snapshot();CHECK_EQ_U32(s.loop_start,36u);CHECK_EQ_U32(s.loop_length,72u);
    CHECK_EQ_U32(dsound_buffer_set_loop_region(buffer,0u,1441u),0x88780032u);
    reject(1u,35u);
}
/* T1524: SetLoopRegion of a RUNNING voice (the minigun mode change) is the completion model's, not a stop. The clock is 22042 Hz at 22042 Hz
 * data, so a tick is a sample. Region (36,72) bytes is samples [64,192), the full data is [0,2560). */
static void live_loop(void)
{
    CHECK_EQ_U32(dsound_buffer_set_loop_region(buffer,36u,72u),0u);
    CHECK_EQ_U32(dsound_completion_buffer_play(buffer,1u),0u);
    now=100u;CHECK_EQ_U32(position(),100u);
    reject(0u,0u); /* no loop hook: the running voice is refused as before */
    dsound_buffer_set_completion_loop(dsound_completion_buffer_loop);
    CHECK_EQ_U32(dsound_buffer_set_loop_region(buffer,0u,0u),0u); /* the stop at 0x407AD8: argument 0, count 0 is the whole data */
    dsound_buffer_snapshot s=snapshot();CHECK_EQ_U32(s.loop_start,0u);CHECK_EQ_U32(s.loop_length,1440u);CHECK_EQ_U32(s.loop_sets,2u);
    CHECK_EQ_U32(position(),100u);now=2559u;CHECK_EQ_U32(position(),2559u);now=2600u;CHECK_EQ_U32(position(),40u);
    /* a region ahead of the cursor: play on to its end, then wrap into it */
    CHECK_EQ_U32(dsound_buffer_set_loop_region(buffer,72u,36u),0u);CHECK_EQ_U32(position(),40u);
    now=2600u+151u;CHECK_EQ_U32(position(),191u);now+=1u;CHECK_EQ_U32(position(),128u);now+=64u;CHECK_EQ_U32(position(),128u);
    /* a region behind the cursor: the cursor wraps into it at once */
    now+=10u;CHECK_EQ_U32(position(),138u);
    CHECK_EQ_U32(dsound_buffer_set_loop_region(buffer,0u,72u),0u);s=snapshot();CHECK_EQ_U32(s.loop_length,72u);CHECK_EQ_U32(position(),10u);
    /* refusals leave the record and the cursor alone */
    s=snapshot();CHECK_EQ_U32(dsound_buffer_set_loop_region(buffer,0u,1441u),0x88780032u);unchanged(&s);CHECK_EQ_U32(position(),10u);
    reject(1u,35u);
    /* an empty interval at this clock resolution is a refusal, not a frozen cursor */
    dsound_completion_set_clock(clock_now,1u);
    RUN_EXPECTING_FATAL((void)dsound_buffer_set_loop_region(buffer,36u,36u));CHECK(fatal_seen);unchanged(&s);
    dsound_completion_set_clock(clock_now,22042u);
    /* Stop ends the running voice: the region is the plain record again, with or without the hook */
    CHECK_EQ_U32(dsound_completion_buffer_stop(buffer),0u);
    CHECK_EQ_U32(dsound_buffer_set_loop_region(buffer,36u,72u),0u);
}
int main(int argc,char **argv)
{
    setup();
    if(argc==2 && strcmp(argv[1],"live")==0)live_loop();
    else if(argc==2 && strcmp(argv[1],"spatial")==0){
        /* the ordinary setup() buffer is fine, add the spatial one beside it */
        spatial_loop();
    }
    else
    if(argc==3){uint32_t r=dsound_buffer_set_loop_region(buffer,(uint32_t)strtoul(argv[1],NULL,0),(uint32_t)strtoul(argv[2],NULL,0));dsound_buffer_snapshot s=snapshot();printf("REGION %u %u %u\n",r,s.loop_start,s.loop_length);}
    else controls();
    CHECK(dsound_buffer_reset_checked());CHECK(dsound_device_reset_checked());environment_end();
    printf("CHECKS %d FAILURES %d\n",checks,failures);return failures?1:0;
}
