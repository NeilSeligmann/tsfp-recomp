/* SPDX-License-Identifier: GPL-3.0-or-later */
/* These assertions execute the qualification calls, including in Release builds. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "dsound_mixer.h"
#include "dsound_adpcm.h"
#include <assert.h>
#include <stdint.h>

int main(void)
{
    dsound_mixer *mixer = dsound_mixer_create(48000u, 48000u);
    assert(mixer != NULL);
    assert(dsound_mixer_set_stream_frequency(mixer, 7u, 10u, 22042u));
    int16_t pcm[64] = {0};
    assert(dsound_mixer_submit_pcm16_stereo(mixer, 7u, 0u, 44100u, pcm, 64u));
    dsound_mixer_counts counts = dsound_mixer_get_counts(mixer);
    assert(counts.packets_queued == 1u);
    assert(!dsound_mixer_set_stream_frequency(mixer, 7u, 0u, 32000u));
    assert(!dsound_mixer_set_stream_frequency(mixer, 7u, 11u, 7999u));
    int16_t rendered[8];size_t emitted;
    assert(dsound_mixer_render_until(mixer,1u,rendered,4u,&emitted));
    /* Late events for fresh identities must not consume bounded stream slots. */
    for (uint32_t key = 100u; key < 300u; key++)
        assert(!dsound_mixer_set_stream_frequency(mixer, key, 0u, 32000u));
    assert(dsound_mixer_set_stream_frequency(mixer, 400u, 20u, 32000u));
    dsound_mixer_destroy(mixer);
    /* A queued ramp keeps its old samples before the event and advances twice
     * as fast afterward, even when the renderer observes the event late. */
    mixer=dsound_mixer_create(48000u,48000u);
    assert(mixer!=NULL);
    int16_t ramp[128];
    for(size_t i=0u;i<64u;i++)ramp[2u*i]=ramp[2u*i+1u]=(int16_t)(200u*i);
    assert(dsound_mixer_submit_pcm16_stereo(mixer,1u,0u,48000u,ramp,64u));
    assert(dsound_mixer_set_stream_frequency(mixer,1u,16u,96000u));
    int16_t output[96];
    assert(dsound_mixer_render_until(mixer,48u,output,48u,&emitted));
    assert(emitted==48u);
    for(size_t i=0u;i<48u;i++) {
        const size_t source=i<16u?i:16u+2u*(i-16u);
        const int16_t expected=source<64u?(int16_t)(200u*source):0;
        assert(output[2u*i]==expected && output[2u*i+1u]==expected);
    }
    assert(!dsound_mixer_set_stream_frequency(mixer,1u,47u,48000u));
    dsound_mixer_destroy(mixer);
    mixer=dsound_mixer_create(48000u,48000u);
    assert(mixer!=NULL);
    assert(dsound_mixer_submit_pcm16_stereo(mixer,1u,0u,48000u,ramp,32u));
    assert(dsound_mixer_submit_pcm16_stereo(mixer,1u,0u,48000u,ramp+64u,32u));
    assert(dsound_mixer_set_stream_running(mixer,1u,8u,false));
    assert(dsound_mixer_set_stream_frequency(mixer,1u,16u,96000u));
    assert(dsound_mixer_set_stream_running(mixer,1u,24u,true));
    int16_t paused_output[128];
    assert(dsound_mixer_render_until(mixer,64u,paused_output,64u,&emitted));
    assert(emitted==64u);
    for(size_t i=0u;i<64u;i++) {
        const bool paused=i>=8u && i<24u;
        const size_t source=i<8u?i:8u+2u*(i>=24u?i-24u:0u);
        const int16_t expected=!paused && source<64u?(int16_t)(200u*source):0;
        assert(paused_output[2u*i]==expected && paused_output[2u*i+1u]==expected);
    }
    dsound_mixer_destroy(mixer);
    /* A packet submitted at wall tick 1 after a 3/2 rate event starts at
     * source tick 1.5, not 1. The sample at wall tick 2 is 1.5 frames in. */
    mixer=dsound_mixer_create(48000u,48000u);
    assert(mixer!=NULL);
    const int16_t first_packet[2]={100,100};
    const int16_t fractional_packet[8]={1000,1000,1200,1200,1400,1400,1600,1600};
    assert(dsound_mixer_submit_pcm16_stereo(mixer,1u,0u,48000u,first_packet,1u));
    assert(dsound_mixer_set_stream_frequency(mixer,1u,0u,72000u));
    assert(dsound_mixer_submit_pcm16_stereo(mixer,1u,1u,48000u,fractional_packet,4u));
    int16_t fractional_output[8];
    assert(dsound_mixer_render_until(mixer,4u,fractional_output,4u,&emitted));
    assert(emitted==4u);
    assert(fractional_output[0]==100 && fractional_output[1]==100);
    assert(fractional_output[2]==1000 && fractional_output[3]==1000);
    assert(fractional_output[4]==1300 && fractional_output[5]==1300);
    assert(fractional_output[6]==1600 && fractional_output[7]==1600);
    dsound_mixer_destroy(mixer);
    /* Empty format reset rebases the nominal clock as well as effective rate.
     * A stale lease/prior rate or retained PCM cannot reset it. */
    mixer=dsound_mixer_create(48000u,48000u);
    assert(mixer!=NULL);
    dsound_stream_routing routing;dsound_stream_routing_init(&routing);
    dsound_stream_routing_headroom(&routing,0u);
    assert(dsound_mixer_route_stream(mixer,1u,41u,0u,&routing));
    assert(dsound_mixer_frequency_stream(mixer,1u,41u,0u,48000u,48000u,96000u));
    assert(!dsound_mixer_format_stream(mixer,1u,42u,0u,96000u,32000u));
    assert(!dsound_mixer_format_stream(mixer,1u,41u,0u,48000u,32000u));
    assert(dsound_mixer_format_stream(mixer,1u,41u,0u,96000u,32000u));
    assert(dsound_mixer_frequency_stream(mixer,1u,41u,0u,48000u,32000u,64000u));
    assert(dsound_mixer_submit_pcm16_stereo(mixer,1u,0u,32000u,fractional_packet,4u));
    assert(!dsound_mixer_format_stream(mixer,1u,42u,0u,64000u,48000u));
    int16_t format_output[6];
    assert(dsound_mixer_render_until(mixer,3u,format_output,3u,&emitted));
    assert(emitted==3u);
    const int16_t format_expected[3]={1000,1266,1533};
    for(size_t i=0u;i<3u;i++)
        assert(format_output[2u*i]==format_expected[i] && format_output[2u*i+1u]==format_expected[i]);
    dsound_mixer_destroy(mixer);
    /* Published T1238 mono decoding must follow the same live source clock.
     * Compare sampled PCM before/after the event to the decoded source. */
    mixer=dsound_mixer_create(48000u,48000u);
    assert(mixer!=NULL);
    uint8_t mono_block[36]={0xE8,0x03,0,0};
    for(size_t i=4u;i<sizeof(mono_block);i++)mono_block[i]=0x11u;
    int16_t decoded_mono[64];size_t decoded_frames=0u;
    assert(dsound_adpcm_xbox_decode_mono(mono_block,sizeof(mono_block),decoded_mono,64u,&decoded_frames));
    assert(decoded_frames==64u && decoded_mono[20]!=decoded_mono[16]);
    assert(dsound_mixer_submit_xbox_adpcm_mono(mixer,1u,0u,48000u,mono_block,sizeof(mono_block)));
    assert(dsound_mixer_set_stream_frequency(mixer,1u,16u,96000u));
    int16_t mono_output[96];
    assert(dsound_mixer_render_until(mixer,48u,mono_output,48u,&emitted));
    assert(emitted==48u);
    for(size_t i=0u;i<48u;i++) {
        const size_t source=i<16u?i:16u+2u*(i-16u);
        const int16_t expected=source<64u?decoded_mono[source]:0;
        assert(mono_output[2u*i]==expected && mono_output[2u*i+1u]==expected);
    }
    dsound_mixer_destroy(mixer);
    /* Abort at wall tick 8 preserves the already queued prefix until 8 and
     * removes its tail. A new-rate packet starts at the exact cut boundary. */
    mixer=dsound_mixer_create(48000u,48000u);
    assert(mixer!=NULL);
    assert(dsound_mixer_route_stream(mixer,1u,41u,0u,&routing));
    assert(dsound_mixer_frequency_stream(mixer,1u,41u,0u,48000u,48000u,96000u));
    assert(dsound_mixer_submit_pcm16_stereo(mixer,1u,0u,48000u,ramp,64u));
    assert(dsound_mixer_format_stream(mixer,1u,41u,8u,96000u,48000u));
    assert(dsound_mixer_submit_pcm16_stereo(mixer,1u,8u,48000u,fractional_packet,4u));
    int16_t cut_output[24];
    assert(dsound_mixer_render_until(mixer,12u,cut_output,12u,&emitted));
    assert(emitted==12u);
    for(size_t i=0u;i<12u;i++) {
        const int16_t expected=i<8u?(int16_t)(400u*i):fractional_packet[2u*(i-8u)];
        assert(cut_output[2u*i]==expected && cut_output[2u*i+1u]==expected);
    }
    dsound_mixer_destroy(mixer);
    /* Events between the last emitted sample (tick30) and the next sample
     * (tick32) are valid. They change only future PCM, never the prefix. */
    mixer=dsound_mixer_create(48000u,96000u);
    assert(mixer!=NULL);
    assert(dsound_mixer_route_stream(mixer,1u,41u,0u,&routing));
    assert(dsound_mixer_submit_pcm16_stereo(mixer,1u,0u,48000u,ramp,64u));
    int16_t boundary_prefix[32];
    assert(dsound_mixer_render_until(mixer,31u,boundary_prefix,16u,&emitted));
    assert(emitted==16u);
    for(size_t i=0u;i<16u;i++)
        assert(boundary_prefix[2u*i]==(int16_t)(200u*i) &&
               boundary_prefix[2u*i+1u]==boundary_prefix[2u*i]);
    assert(!dsound_mixer_frequency_stream(mixer,1u,41u,30u,48000u,48000u,96000u));
    assert(dsound_mixer_frequency_stream(mixer,1u,41u,31u,48000u,48000u,96000u));
    int16_t boundary_tail[6];
    assert(dsound_mixer_render_until(mixer,38u,boundary_tail,3u,&emitted));
    assert(emitted==3u);
    for(size_t i=0u;i<3u;i++)
        assert(boundary_tail[2u*i]==(int16_t)(3300u+400u*i) &&
               boundary_tail[2u*i+1u]==boundary_tail[2u*i]);
    dsound_mixer_destroy(mixer);
    /* Same-rate SetFormat has the same boundary rule and cuts the old tail.
     * A rejected already-rendered event leaves that packet intact. */
    mixer=dsound_mixer_create(48000u,96000u);
    assert(mixer!=NULL);
    assert(dsound_mixer_route_stream(mixer,1u,41u,0u,&routing));
    assert(dsound_mixer_submit_pcm16_stereo(mixer,1u,0u,48000u,ramp,64u));
    assert(dsound_mixer_render_until(mixer,31u,boundary_prefix,16u,&emitted));
    assert(emitted==16u);
    assert(!dsound_mixer_submit_pcm16_stereo(mixer,999u,30u,48000u,fractional_packet,4u));
    assert(!dsound_mixer_format_stream(mixer,1u,41u,30u,48000u,48000u));
    assert(dsound_mixer_format_stream(mixer,1u,41u,31u,48000u,48000u));
    assert(dsound_mixer_submit_pcm16_stereo(mixer,1u,31u,48000u,fractional_packet,4u));
    assert(dsound_mixer_render_until(mixer,38u,boundary_tail,3u,&emitted));
    assert(emitted==3u);
    for(size_t i=0u;i<3u;i++)
        assert(boundary_tail[2u*i]==(int16_t)(1100u+200u*i) &&
               boundary_tail[2u*i+1u]==boundary_tail[2u*i]);
    dsound_mixer_destroy(mixer);
    return 0;
}
