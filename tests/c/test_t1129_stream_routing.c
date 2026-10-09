/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Reuse only synthetic environment/lease fixture and fault boundary. */
#define main t1129_existing_fixture_main
#include "test_dsound_stream.c"
#undef main
static bool accept_route=true;
static unsigned route_calls;
static bool routing_note(uint32_t stream,uint64_t serial,const dsound_stream_routing *routing)
{CHECK(stream!=0u);CHECK(serial!=0u);CHECK(routing!=NULL);route_calls++;return accept_route;}
int main(void)
{
 environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();dsound_device_set_fatal(catching_fatal);
 dsound_stream_set_fatal(catching_fatal);dsound_stream_set_irql_provider(current_irql);
 map_fixed(0x412000u,4096u);map_fixed(0x4A1000u,4096u);
 for(unsigned i=0u;i<15u;i++)store(0x4A1CF0u+4u*i,0x406879u);
 dsound_hle_set_codec_state(DSOUND_CODEC_READY);CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);
 device=load(SCRATCH_DATA)-8u;desc=SCRATCH_DATA+256u;format=SCRATCH_DATA+320u;params=desc+20u;
 dsound_stream_set_enabled(true);dsound_stream_set_completion(true,NULL);
 dsound_stream_set_routing_note(routing_note);
 uint32_t stream=create_one(false,SCRATCH_DATA+128u);
 dsound_stream_snapshot before,after;CHECK(dsound_stream_get_snapshot(stream,&before));
 CHECK_EQ_U32(before.routing.headroom,600u);CHECK_EQ_U32(before.routing.aggregate,(uint32_t)-600);
 CHECK_EQ_U32(dsound_stream_set_headroom(stream,1200u),0u);
 CHECK(dsound_stream_get_snapshot(stream,&after));CHECK_EQ_U32(after.routing.headroom,1200u);
 CHECK_EQ_U32(after.routing.aggregate,(uint32_t)-1200);CHECK_EQ_U32(route_calls,1u);
 before=after;accept_route=false;
 RUN_EXPECTING_FATAL((void)dsound_stream_set_headroom(stream,0u));CHECK(fatal_seen);
 snapshot_unchanged(stream,&before);accept_route=true;
 uint32_t descriptor=SCRATCH_DATA+600u,pairs=SCRATCH_DATA+640u;
 store(descriptor,2u);store(descriptor+4u,pairs);store(pairs,1u);store(pairs+4u,0u);
 store(pairs+8u,0u);store(pairs+12u,(uint32_t)-600);
 CHECK_EQ_U32(dsound_stream_set_mix_bins(stream,descriptor),0u);
 CHECK(dsound_stream_get_snapshot(stream,&after));CHECK_EQ_U32(after.routing.bins[0],1u);
 CHECK_EQ_U32(after.routing.bins[1],0u);CHECK_EQ_U32(after.routing.bin_volume[0],(uint32_t)-600);
 before=after;store(pairs,6u);
 RUN_EXPECTING_FATAL((void)dsound_stream_set_mix_bins(stream,descriptor));CHECK(fatal_seen);
 snapshot_unchanged(stream,&before);store(pairs,0x101u);
 RUN_EXPECTING_FATAL((void)dsound_stream_set_mix_bin_volumes(stream,descriptor));CHECK(fatal_seen);
 snapshot_unchanged(stream,&before);
 store(descriptor,0u);store(descriptor+4u,0xFFFFFFFFu);
 CHECK_EQ_U32(dsound_stream_set_mix_bin_volumes(stream,descriptor),0u);
 CHECK_EQ_U32(dsound_stream_set_mix_bins(stream,0u),0u);
 CHECK(dsound_stream_get_snapshot(stream,&after));CHECK_EQ_U32(after.routing.bins[0],0u);
 CHECK_EQ_U32(after.routing.bins[1],1u);CHECK_EQ_U32(after.routing.bin_volume[0],0u);
 unsigned calls=route_calls;store(0x4124A8u,1u);
 CHECK_EQ_U32(dsound_stream_set_headroom(0u,0u),0x80004005u);
 CHECK_EQ_U32(dsound_stream_set_mix_bins(0u,0xFFFFFFFFu),0x80004005u);
 CHECK_EQ_U32(dsound_stream_set_mix_bin_volumes(0u,0xFFFFFFFFu),0x80004005u);
 CHECK_EQ_U32(route_calls,calls);store(0x4124A8u,0u);
 before=after;dsound_stream_set_routing_note(NULL);
 RUN_EXPECTING_FATAL((void)dsound_stream_set_headroom(stream,0u));CHECK(fatal_seen);
 snapshot_unchanged(stream,&before);
 CHECK(dsound_stream_reset_checked());CHECK(dsound_device_reset_checked());environment_end();
 printf("T1129 routing transaction: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
