/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T743: the title's stream START (sub_00029F30 -> sub_00029EE0, Pause(stream, 0) with return address 0x29F13).
 * The original 0x407B23 never reads its caller, so the passive stream model admits this third Pause caller ONLY with the
 * completion model on (the resume of a started stream is a T681 behaviour), refuses it with the model off, and a refusal
 * of any other caller now names the actual return address. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <string.h>
#include "test_d3d8_support.h"
#include "dsound_buffer.h"
#include "dsound_completion.h"
#include "dsound_device.h"
#include "dsound_hle.h"
#include "dsound_stream.h"
#define START_CALLER 0x29F13u
int recomp_has_stop_boundary(uint32_t address)
{return address==0x408040u || address==0x406FA9u || address==0x406FF0u || address==0x406879u;}
static bool current_irql(uint8_t *out){*out=0u;return true;}
static uint32_t desc_at,format_at,frame_at;
static const uint8_t stereo_startup[20]={0x69,0,2,0,0x44,0xAC,0,0,0xCC,0xC1,0,0,72,0,4,0,2,0,64,0};
static uint32_t make_stream(uint32_t output)
{
    const uint32_t desc[6]={0u,3u,format_at,0u,0u,0u};
    CHECK(kernel_guest_write_bytes(desc_at,desc,sizeof(desc)));CHECK(kernel_guest_write_bytes(format_at,stereo_startup,20u));
    CHECK_EQ_U32(dsound_stream_create(desc_at,output),0u);
    const uint32_t stream=load(output);
    CHECK_EQ_U32(dsound_stream_cache_volume(stream,-10000),0u);CHECK_EQ_U32(dsound_stream_cache_pause(stream,1u),0u);
    CHECK_EQ_U32(dsound_stream_cache_flush_ex(stream,0u,0u,1u),0u);CHECK_EQ_U32(dsound_stream_cache_discontinuity(stream),0u);
    return stream;
}
static uint32_t pause_from(uint32_t caller, uint32_t stream, uint32_t mode)
{
    const uint32_t args[2]={stream,mode};
    kernel_call_frame frame={0};
    CHECK(kernel_frame_build(&frame,frame_at,0x100u,args,2u));
    store(frame.stack_ptr,caller);
    return dsound_hle_call(0x407B23u,&frame);
}
static void test_the_start_caller_needs_the_completion_model(void)
{
    const uint32_t stream=make_stream(SCRATCH_DATA+0x700u);
    dsound_stream_snapshot snap;
    /* With the model on the start (Pause 0 of the started stream) is the resume, recorded, mode 0. */
    CHECK_EQ_U32(pause_from(START_CALLER,stream,0u),0u);
    CHECK(dsound_stream_get_snapshot(stream,&snap));
    CHECK(snap.pause_seen&&snap.pause_mode==0u);
    /* The measured callers still work, other return addresses are refused and the text names the actual one. */
    CHECK_EQ_U32(pause_from(0x29B5Au,stream,1u),0u);
    RUN_EXPECTING_FATAL((void)pause_from(START_CALLER+1u,stream,0u));
    CHECK(fatal_seen);CHECK(strstr(fatal_text,"only measured startup caller is supported, got caller 0x29f14")!=NULL);
    /* With the model off the same call is refused like every unmeasured caller (default boots are unchanged). */
    dsound_stream_set_completion(false,NULL);
    RUN_EXPECTING_FATAL((void)pause_from(START_CALLER,stream,0u));
    CHECK(fatal_seen);CHECK(strstr(fatal_text,"only measured startup caller is supported, got caller 0x29f13")!=NULL);
    dsound_stream_set_completion(true,dsound_completion_note_pause);
}
static void test_runtime_pause_caller(void)
{
    const uint32_t stream=make_stream(SCRATCH_DATA+0x704u);
    CHECK_EQ_U32(pause_from(START_CALLER,stream,0u),0u);
    CHECK_EQ_U32(pause_from(0x29ED1u,stream,1u),0u);
    dsound_stream_snapshot before,after;
    CHECK(dsound_stream_get_snapshot(stream,&before));CHECK(before.pause_seen&&before.pause_mode==1u);
    RUN_EXPECTING_FATAL((void)pause_from(0x29ED1u,stream,0u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)pause_from(0x29ED1u,stream,2u));CHECK(fatal_seen);
    CHECK(dsound_stream_get_snapshot(stream,&after));CHECK(memcmp(&before,&after,sizeof(before))==0);
    RUN_EXPECTING_FATAL((void)pause_from(0x29ED2u,stream,1u));CHECK(fatal_seen);
    dsound_stream_set_completion(true,NULL);
    RUN_EXPECTING_FATAL((void)pause_from(0x29ED1u,stream,1u));CHECK(fatal_seen);
    CHECK(dsound_stream_get_snapshot(stream,&after));CHECK(memcmp(&before,&after,sizeof(before))==0);
    dsound_stream_set_completion(false,NULL);
    RUN_EXPECTING_FATAL((void)pause_from(0x29ED1u,stream,1u));CHECK(fatal_seen);
    dsound_stream_set_completion(true,dsound_completion_note_pause);
    CHECK_EQ_U32(pause_from(START_CALLER,stream,0u),0u);
    CHECK_EQ_U32(dsound_stream_create(desc_at,SCRATCH_DATA+0x708u),0u);
    const uint32_t inactive=load(SCRATCH_DATA+0x708u);
    CHECK(dsound_stream_get_snapshot(inactive,&before));
    RUN_EXPECTING_FATAL((void)pause_from(0x29ED1u,inactive,1u));CHECK(fatal_seen);
    CHECK(dsound_stream_get_snapshot(inactive,&after));CHECK(memcmp(&before,&after,sizeof(before))==0);
}
int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();
    dsound_device_set_fatal(catching_fatal);dsound_stream_set_fatal(catching_fatal);dsound_buffer_set_fatal(catching_fatal);
    dsound_completion_set_fatal(catching_fatal);
    dsound_stream_set_irql_provider(current_irql);dsound_buffer_set_irql_provider(current_irql);
    map_fixed(0x412000u,4096u);map_fixed(0x4A1000u,4096u);map_fixed(0x4B9000u,4096u);map_fixed(0x581000u,4096u);map_fixed(0x583000u,4096u);
    for(unsigned i=0u;i<15u;i++)store(0x4A1CF0u+4u*i,0x406879u);
    for(unsigned i=0u;i<4u;i++)store(0x4A1CE0u+4u*i,0x406879u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);
    desc_at=SCRATCH_DATA+0x100u;format_at=SCRATCH_DATA+0x140u;frame_at=SCRATCH_DATA+0x800u;
    dsound_stream_set_enabled(true);dsound_buffer_set_enabled(true);
    (void)dsound_stream_register();(void)dsound_buffer_register();
    dsound_completion_set_enabled(true);
    dsound_stream_set_completion(true,dsound_completion_note_pause);
    test_the_start_caller_needs_the_completion_model();
    test_runtime_pause_caller();
    CHECK(dsound_stream_reset_checked());CHECK(dsound_buffer_reset_checked());CHECK(dsound_device_reset_checked());
    environment_end();
    return failures?1:0;
}
