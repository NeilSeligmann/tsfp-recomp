/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Reuse the real-address facade setup and existing original-backed transport
 * controls. The additional PCM expectations use independent literal blocks. */
#define main completion_baseline_main
#include "test_dsound_completion.c"
#undef main
#include "dsound_audio_runtime.h"

#define GAIN_600_OF_2005 1005 /* 2005 * 10^(-6/20) = 1004.9 */
static void audible_render(uint64_t tick, size_t expected_frames, int16_t expected)
{
    int16_t pcm[512] = {0};
    size_t frames = 0u;
    CHECK(dsound_audio_runtime_render(tick, pcm, 256u, &frames));
    CHECK(frames == expected_frames);
    for (size_t i = 0u; i < frames * 2u; i++) CHECK(pcm[i] == expected);
}

int main(void)
{
    CHECK_EQ_U32(completion_baseline_main(), 0u);
    environment_begin(KERNEL_AV_PACK_HDTV); dsound_hle_init();
    dsound_device_set_fatal(catching_fatal); dsound_buffer_set_fatal(catching_fatal);
    dsound_completion_set_fatal(catching_fatal);
    dsound_buffer_set_irql_provider(current_irql);
    map_fixed(0x412000u,4096u); map_fixed(0x4A1000u,4096u);
    map_fixed(0x4B9000u,4096u); map_fixed(0x581000u,4096u); map_fixed(0x583000u,4096u);
    map_fixed(DATA_BASE,0x10000u);
    for(unsigned i=0u;i<15u;i++)store(0x4A1CF0u+4u*i,0x406879u);
    for(unsigned i=0u;i<4u;i++)store(0x4A1CE0u+4u*i,0x406879u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);
    device=load(SCRATCH_DATA)-8u; desc_at=SCRATCH_DATA+0x100u;
    format_at=SCRATCH_DATA+0x140u; words_at=SCRATCH_DATA+0x400u;
    dsound_buffer_set_enabled(true); (void)dsound_buffer_register();
    dsound_completion_set_clock(fake_clock,22042u); dsound_completion_set_enabled(true);
    dsound_buffer_set_completion(true,dsound_completion_buffer_started);
    dsound_buffer_set_completion_pause(dsound_completion_buffer_pause);
    dsound_buffer_set_completion_frequency(dsound_completion_buffer_frequency);
    (void)dsound_completion_register(); dsound_completion_reset(); fake_now=100u;
    CHECK(dsound_audio_runtime_start(22042u,22042u));
    uint8_t block[36]={0}; block[0]=0xA0u; block[1]=0x0Fu; /* predictor 4000, zero nibbles */
    CHECK(kernel_guest_write_bytes(DATA_BASE,block,sizeof(block)));
    const uint32_t buffer=make_buffer(0x5818E8u,false);
    CHECK_EQ_U32(dsound_buffer_set_data(buffer,DATA_BASE,sizeof(block)),0u);
    CHECK_EQ_U32(dsound_buffer_set_volume(buffer,-10000),0u);
    CHECK_EQ_U32(dsound_buffer_pause(buffer,0u),0u);
    CHECK_EQ_U32(dsound_buffer_set_frequency(buffer,22042u),0u);
    CHECK_EQ_U32(dsound_buffer_set_volume(buffer,0),0u);
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,1u,4u),0u);
    CHECK_EQ_U32(buffer_status(buffer),5u);
    fake_now=104u;
    CHECK_EQ_U32(call_frame(0x407ABCu,0x2751Eu,buffer,1u,0u,0u,2u),0u);
    CHECK_EQ_U32(buffer_status(buffer),6u);
    audible_render(104u,4u,2005);
    fake_now=110u; audible_render(110u,6u,0);
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,1u,4u),0u);
    CHECK_EQ_U32(buffer_status(buffer),6u); /* original Play retains pause */
    CHECK_EQ_U32(call_frame(0x407ABCu,0x2751Eu,buffer,0u,0u,0u,2u),0u);
    fake_now=114u; audible_render(114u,4u,2005);
    CHECK_EQ_U32(dsound_buffer_set_volume(buffer,-10000),0u);
    fake_now=118u; audible_render(118u,4u,0);
    CHECK_EQ_U32(dsound_buffer_set_volume(buffer,0),0u);
    block[2]=89u; CHECK(kernel_guest_write_bytes(DATA_BASE,block,sizeof(block)));
    RUN_EXPECTING_FATAL((void)call_frame(0x407A80u,0x28698u,buffer,0u,0u,1u,4u)); CHECK(fatal_seen);
    /* Invalid new data cannot retire the immutable old play. */
    fake_now=122u; audible_render(122u,4u,2005);
    CHECK_EQ_U32(call_frame(0x407AA4u,0x29881u,buffer,0u,0u,0u,1u),0u);
    CHECK_EQ_U32(buffer_status(buffer),1u);
    fake_now=174u; audible_render(174u,52u,2005);
    CHECK_EQ_U32(buffer_status(buffer),0u);
    fake_now=178u; audible_render(178u,4u,0);
    block[2]=0u; CHECK(kernel_guest_write_bytes(DATA_BASE,block,sizeof(block)));
    dsound_completion_set_clock(fake_clock,22043u);
    RUN_EXPECTING_FATAL((void)call_frame(0x407A80u,0x28698u,buffer,0u,0u,0u,4u)); CHECK(fatal_seen);
    CHECK_EQ_U32(buffer_status(buffer),0u);
    dsound_completion_set_clock(fake_clock,22042u);
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,0u,4u),0u);
    dsound_buffer_snapshot before={0},after={0};
    CHECK(dsound_buffer_get_snapshot(buffer,&before));
    /* T1216 contract: a command stamped before the audio already emitted (render horizon 178) takes effect at the
     * horizon and never rewrites emitted PCM, it is no longer refused (tests/test_t1216_late_buffer_commands.py). */
    fake_now=177u;
    CHECK_EQ_U32(dsound_buffer_set_volume(buffer,-600),0u);
    CHECK(dsound_buffer_get_snapshot(buffer,&after));
    CHECK(after.volume==-600 && after.volume_sets==before.volume_sets+1u);
    before=after;
    fake_now=182u; audible_render(182u,4u,GAIN_600_OF_2005);
    RUN_EXPECTING_FATAL((void)dsound_buffer_set_volume(buffer,1)); CHECK(fatal_seen);
    CHECK(dsound_buffer_get_snapshot(buffer,&after));
    CHECK(before.volume==after.volume && before.volume_sets==after.volume_sets);
    const uint32_t original_header=load(buffer);
    store(buffer,original_header^4u);
    RUN_EXPECTING_FATAL((void)dsound_buffer_pause(buffer,1u)); CHECK(fatal_seen);
    store(buffer,original_header);
    CHECK_EQ_U32(buffer_status(buffer),1u);
    dsound_completion_reset();
    fake_now=186u; audible_render(186u,4u,0);
    CHECK_EQ_U32(buffer_status(buffer),0u);
    dsound_audio_runtime_stop();
    CHECK(dsound_buffer_reset_checked()); CHECK(dsound_device_reset_checked()); environment_end();
    printf("T1102 owned buffer PCM: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
