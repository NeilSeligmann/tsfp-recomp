/* SPDX-License-Identifier: GPL-3.0-or-later */
#define main t1148_existing_fixture_main
#include "test_dsound_stream.c"
#undef main
#include "dsound_mixbin_headroom.h"
#include "dsound_audio_runtime.h"
#include "dsound_completion.h"
static uint64_t ticks;
static uint64_t clock_ticks(void){return ticks;}
static void unchanged(uint32_t interface,const uint8_t before[32])
{uint8_t after[32];CHECK(dsound_mixbin_headroom_snapshot(interface,after));CHECK(memcmp(before,after,32u)==0);}
int main(void)
{
 environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();dsound_device_set_fatal(catching_fatal);
 map_fixed(0x412000u,4096u);map_fixed(0x4A1000u,4096u);dsound_hle_set_codec_state(DSOUND_CODEC_READY);
 CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);uint32_t interface=load(SCRATCH_DATA);
 dsound_mixbin_headroom_configure(NULL,NULL,current_irql,catching_fatal);
 CHECK_EQ_U32(dsound_mixbin_headroom_register(),0u);
 RUN_EXPECTING_FATAL((void)dsound_mixbin_headroom_set(interface,0u,0u));CHECK(fatal_seen);
 CHECK(dsound_audio_runtime_start(1000u,1000u));CHECK(dsound_audio_runtime_enable_mixbin_headroom(true));
 dsound_completion_set_enabled(true);dsound_completion_set_clock(clock_ticks,1000u);
 dsound_mixbin_headroom_configure(dsound_completion_mixbin_headroom,dsound_completion_bind_mixbin_headroom,current_irql,catching_fatal);
 CHECK(dsound_device_reset_checked());
 CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);interface=load(SCRATCH_DATA);
 CHECK_EQ_U32(dsound_mixbin_headroom_register(),1u);
 uint8_t before[32];CHECK(dsound_mixbin_headroom_snapshot(interface,before));
 for(unsigned i=0u;i<32u;i++)CHECK_EQ_U32(before[i],i==31u?0u:1u);
 int16_t pcm[40];for(unsigned i=0u;i<20u;i++){pcm[2u*i]=10000;pcm[2u*i+1u]=-12000;}
 CHECK(dsound_audio_runtime_submit_pcm16_stereo(99u,0u,1000u,pcm,20u));
 /* Epoch protection must work even before the first headroom command. */
 CHECK(dsound_device_reset_checked());uint32_t initial_output=load(SCRATCH_DATA);
 size_t heaps_before=guest_mem_heap_count();
 RUN_EXPECTING_FATAL((void)dsound_device_create(0u,SCRATCH_DATA,0u));CHECK(fatal_seen);
 CHECK_EQ_U32(load(SCRATCH_DATA),initial_output);CHECK_EQ_U32(load(0x412B30u),0u);
 CHECK(guest_mem_heap_count()==heaps_before);
 dsound_audio_runtime_stop();CHECK(dsound_audio_runtime_start(1000u,1000u));
 CHECK(dsound_audio_runtime_enable_mixbin_headroom(true));
 CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);interface=load(SCRATCH_DATA);
 CHECK(dsound_audio_runtime_submit_pcm16_stereo(99u,0u,1000u,pcm,20u));
 ticks=5u;uint32_t args[3]={interface,0u,0x108u};
 CHECK_EQ_U32(invoke(0x407A2Cu,0x29AAAu,args,3u),0u);
 CHECK(dsound_mixbin_headroom_snapshot(interface,before));CHECK_EQ_U32(before[0],8u);
 int16_t out[40];size_t frames=0u;
 CHECK(dsound_audio_runtime_render(10u,out,20u,&frames));CHECK_EQ_U32(frames,10u);
 for(unsigned i=0u;i<10u;i++){
  CHECK_EQ_U32((uint32_t)(int32_t)out[2u*i],i<5u?5000u:10000u);
  CHECK_EQ_U32((uint32_t)(int32_t)out[2u*i+1u],(uint32_t)-6000);
 }
 ticks=9u;RUN_EXPECTING_FATAL((void)dsound_mixbin_headroom_set(interface,0u,7u));CHECK(fatal_seen);
 unchanged(interface,before);ticks=10u;
 RUN_EXPECTING_FATAL((void)dsound_mixbin_headroom_set(interface,32u,7u));CHECK(fatal_seen);
 unchanged(interface,before);
 uint32_t saved=load(interface-8u);store(interface-8u,saved^1u);
 RUN_EXPECTING_FATAL((void)dsound_mixbin_headroom_set(interface,0u,7u));CHECK(fatal_seen);
 store(interface-8u,saved);unchanged(interface,before);
 known_irql=false;RUN_EXPECTING_FATAL((void)dsound_mixbin_headroom_set(interface,0u,7u));CHECK(fatal_seen);
 known_irql=true;unchanged(interface,before);irql=1u;
 RUN_EXPECTING_FATAL((void)dsound_mixbin_headroom_set(interface,0u,7u));CHECK(fatal_seen);
 irql=0u;unchanged(interface,before);
 store(0x4124A8u,1u);CHECK_EQ_U32(dsound_mixbin_headroom_set(0u,0xFFFFFFFFu,0xFFFFFFFFu),0x80004005u);
 store(0x4124A8u,0u);unchanged(interface,before);
 dsound_completion_set_clock(clock_ticks,1001u);
 RUN_EXPECTING_FATAL((void)dsound_mixbin_headroom_set(interface,0u,7u));CHECK(fatal_seen);
 unchanged(interface,before);dsound_completion_set_clock(clock_ticks,1000u);
 CHECK(dsound_device_reset_checked());
 uint32_t previous=load(SCRATCH_DATA);
 RUN_EXPECTING_FATAL((void)dsound_device_create(0u,SCRATCH_DATA,0u));CHECK(fatal_seen);
 CHECK_EQ_U32(load(SCRATCH_DATA),previous);CHECK_EQ_U32(load(0x412B30u),0u);
 dsound_audio_runtime_stop();
 CHECK(dsound_audio_runtime_start(1000u,1000u));CHECK(dsound_audio_runtime_enable_mixbin_headroom(true));
 CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);interface=load(SCRATCH_DATA);
 CHECK(dsound_mixbin_headroom_snapshot(interface,before));CHECK_EQ_U32(before[0],1u);
 CHECK_EQ_U32(dsound_mixbin_headroom_set(interface,0u,3u),0u);
 CHECK(dsound_mixbin_headroom_snapshot(interface,before));CHECK_EQ_U32(before[0],3u);
 dsound_audio_runtime_stop();RUN_EXPECTING_FATAL((void)dsound_mixbin_headroom_set(interface,0u,7u));CHECK(fatal_seen);
 unchanged(interface,before);dsound_mixbin_headroom_configure(NULL,NULL,current_irql,catching_fatal);
 CHECK(dsound_device_reset_checked());environment_end();
 printf("T1148 global headroom: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
