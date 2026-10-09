/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T681: the opt-in passive completion model (option (a) of T608). Admission, the measured status words, packet
 * completion on a stated virtual clock, Play and buffer status, every named refusal, and that the model is inert
 * (nothing admitted, nothing registered) while the policy is off. The measured facts themselves (status words, the
 * packet limit, the XMEDIAINFO answer, Play's effects) are pinned against the ORIGINAL bytes by
 * tests/test_dsound_completion_oracle.py. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <string.h>
#include <sys/mman.h>
#include "probe_cache_wrap.h"
#include "test_d3d8_support.h"
#include "dsound_buffer.h"
#include "dsound_audio_runtime.h"
#include "dsound_completion.h"
#include "dsound_device.h"
#include "dsound_hle.h"
#include "dsound_stream.h"
#define DATA_BASE 0x23000000u
#define PENDING 0x8000000Au
#define FULL 0x88780032u
int recomp_has_stop_boundary(uint32_t address)
{return address==0x408040u || address==0x406FA9u || address==0x406FF0u || address==0x406879u;}
static uint32_t call_frame(uint32_t entry,uint32_t caller,uint32_t a,uint32_t b,uint32_t c,uint32_t d,unsigned count);
static uint64_t fake_now;
static uint64_t fake_clock(void){return fake_now;}
static bool current_irql(uint8_t *out){*out=0u;return true;}
static uint32_t device,desc_at,format_at,packet_at,words_at,out_at;
static const uint8_t stereo_startup[20]={0x69,0,2,0,0x44,0xAC,0,0,0xCC,0xC1,0,0,72,0,4,0,2,0,64,0};
static const uint8_t buffer_format[20]={0x69,0,1,0,0xE0,0xAB,0,0,0xAE,0x60,0,0,36,0,4,0,2,0,64,0};
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
static uint32_t status_of(uint32_t stream)
{store(words_at,0xA5A5A5A5u);CHECK_EQ_U32(dsound_completion_stream_status(stream,words_at),0u);return load(words_at);}
/* One packet slot k: completed word, status word (0xDEAD/0xBEEF sentinels so an unwritten word shows). */
static uint32_t packet_words(unsigned k){return words_at+64u+16u*k;}
static uint32_t process(uint32_t stream,uint32_t size,unsigned k,bool with_words)
{
    const uint32_t packet[6]={DATA_BASE,size,with_words?packet_words(k):0u,with_words?packet_words(k)+4u:0u,0u,0u};
    store(packet_words(k),0xDEADu);store(packet_words(k)+4u,0xBEEFu);
    CHECK(kernel_guest_write_bytes(packet_at,packet,sizeof(packet)));
    return dsound_completion_stream_process(stream,packet_at,0u);
}
static void run(uint64_t ticks){fake_now+=ticks;}
static void test_stream_frequency_cumulative(void)
{
    dsound_completion_reset();fake_now=1000u;
    const uint32_t stream=make_stream(SCRATCH_DATA+0x780u);
    dsound_stream_snapshot snap;
    CHECK(dsound_stream_get_snapshot(stream,&snap));
    (void)status_of(stream); /* Create and bind completion identity. */
    CHECK(!dsound_completion_stream_frequency(stream,snap.lease.serial+1u,fake_now,44100u,22050u));
    CHECK(dsound_completion_stream_frequency(stream,snap.lease.serial,fake_now,44100u,22050u));
    CHECK(dsound_completion_stream_frequency(stream,snap.lease.serial,fake_now,22050u,44100u));
    CHECK(!dsound_completion_stream_frequency(stream,snap.lease.serial,fake_now+1u,44100u,22050u));
    CHECK(!dsound_completion_stream_frequency(stream,snap.lease.serial,fake_now,22050u,44100u));
    CHECK_EQ_U32(process(stream,100u,0u,true),0u);
    CHECK_EQ_U32(dsound_stream_cache_pause(stream,0u),0u);
    run(99u);dsound_completion_work();CHECK_EQ_U32(load(packet_words(0u)+4u),PENDING);
    run(1u);dsound_completion_work();CHECK_EQ_U32(load(packet_words(0u)+4u),0u);
    CHECK(dsound_stream_reset_checked());
}
static void test_stream_frequency_live_queue(void)
{
    dsound_completion_reset();fake_now=1000u;
    const uint32_t stream=make_stream(SCRATCH_DATA+0x790u);
    dsound_stream_snapshot snap;CHECK(dsound_stream_get_snapshot(stream,&snap));
    CHECK_EQ_U32(process(stream,100u,0u,true),0u);
    CHECK_EQ_U32(process(stream,100u,1u,true),0u);
    CHECK_EQ_U32(dsound_stream_cache_pause(stream,0u),0u);
    run(40u);
    CHECK(dsound_completion_stream_frequency(stream,snap.lease.serial,fake_now,44100u,88200u));
    run(29u);dsound_completion_work();CHECK_EQ_U32(load(packet_words(0u)+4u),PENDING);
    run(1u);dsound_completion_work();CHECK_EQ_U32(load(packet_words(0u)+4u),0u);
    CHECK_EQ_U32(load(packet_words(1u)+4u),PENDING);
    run(49u);dsound_completion_work();CHECK_EQ_U32(load(packet_words(1u)+4u),PENDING);
    run(1u);dsound_completion_work();CHECK_EQ_U32(load(packet_words(1u)+4u),0u);
    CHECK(dsound_stream_reset_checked());
}
static void test_stream_frequency_paused(void)
{
    dsound_completion_reset();fake_now=1000u;
    const uint32_t stream=make_stream(SCRATCH_DATA+0x7C0u);
    dsound_stream_snapshot snap;CHECK(dsound_stream_get_snapshot(stream,&snap));
    CHECK_EQ_U32(process(stream,100u,0u,true),0u);
    CHECK_EQ_U32(dsound_stream_cache_pause(stream,0u),0u);
    run(40u);CHECK_EQ_U32(dsound_stream_cache_pause(stream,1u),0u);
    run(10u);CHECK(dsound_completion_stream_frequency(stream,snap.lease.serial,fake_now,44100u,88200u));
    run(10u);CHECK_EQ_U32(dsound_stream_cache_pause(stream,0u),0u);
    run(29u);dsound_completion_work();CHECK_EQ_U32(load(packet_words(0u)+4u),PENDING);
    run(1u);dsound_completion_work();CHECK_EQ_U32(load(packet_words(0u)+4u),0u);
    CHECK(dsound_stream_reset_checked());
}
static void test_stream_frequency_transaction(void)
{
    dsound_completion_reset();fake_now=1000u;
    const uint32_t stream=make_stream(SCRATCH_DATA+0x7A0u);
    dsound_stream_snapshot snap;CHECK(dsound_stream_get_snapshot(stream,&snap));
    CHECK_EQ_U32(process(stream,100u,0u,true),0u);
    CHECK_EQ_U32(dsound_stream_cache_pause(stream,0u),0u);
    run(100u);
    const dsound_completion_stats before=dsound_completion_get_stats();
    CHECK(!dsound_completion_stream_frequency_pcm(stream,snap.lease.serial,fake_now,44100u,88200u));
    /* The prepared candidate would finish the packet; no words/statistics are
     * published when the inactive PCM consumer refuses it. */
    CHECK_EQ_U32(load(packet_words(0u)+4u),PENDING);
    CHECK_EQ_U32(dsound_completion_get_stats().stream_completed,before.stream_completed);
    CHECK_EQ_U32(dsound_completion_get_stats().observations,before.observations);
    dsound_completion_work();CHECK_EQ_U32(load(packet_words(0u)+4u),0u);

    dsound_completion_reset();fake_now=2000u;
    const uint32_t live=make_stream(SCRATCH_DATA+0x7B0u);
    CHECK(dsound_stream_get_snapshot(live,&snap));
    CHECK(dsound_audio_runtime_start(48000u,49612u));
    CHECK(dsound_audio_runtime_route_stream(live,snap.lease.serial,fake_now,&snap.routing));
    CHECK_EQ_U32(process(live,72u,0u,true),0u);
    CHECK_EQ_U32(dsound_stream_cache_pause(live,0u),0u);
    run(18u);
    CHECK(!dsound_completion_stream_frequency_pcm(live,snap.lease.serial+1u,fake_now,44100u,88200u));
    CHECK(dsound_completion_stream_frequency_pcm(live,snap.lease.serial,fake_now,44100u,88200u));
    run(26u);dsound_completion_work();CHECK_EQ_U32(load(packet_words(0u)+4u),PENDING);
    run(1u);dsound_completion_work();CHECK_EQ_U32(load(packet_words(0u)+4u),0u);
    dsound_audio_runtime_stop();
    CHECK(dsound_stream_reset_checked());
}
static uint16_t frequency_cw=0x027Fu;
static bool frequency_control_word(uint16_t *word){*word=frequency_cw;return true;}
static uint32_t invoke_stream_frequency(uint32_t caller,uint32_t stream,uint32_t hertz)
{
    const uint32_t frame_words[3]={caller,stream,hertz};
    const uint32_t stack=SCRATCH_DATA+0x900u;
    CHECK(kernel_guest_write_bytes(stack,frame_words,sizeof(frame_words)));
    kernel_call_frame frame={0};frame.stack_ptr=stack;frame.stack_limit=stack+sizeof(frame_words);
    return dsound_hle_call(0x4085CFu,&frame);
}
static uint32_t make_frequency_stream(uint32_t output,bool spatial)
{
    if(!spatial)return make_stream(output);
    uint8_t format[20];memcpy(format,stereo_startup,sizeof(format));
    const uint32_t average=24806u;
    format[2]=1u;format[12]=36u;memcpy(format+8u,&average,4u);
    const uint32_t descriptor[6]={16u,3u,format_at,0u,0u,0u};
    CHECK(kernel_guest_write_bytes(desc_at,descriptor,sizeof(descriptor)));
    CHECK(kernel_guest_write_bytes(format_at,format,sizeof(format)));
    CHECK_EQ_U32(dsound_stream_create(desc_at,output),0u);
    const uint32_t stream=load(output);
    const uint32_t params[9]={0u,0u,0xFFFFF448u,0u,0u,0u,0u,0u,0u};
    CHECK(kernel_guest_write_bytes(SCRATCH_DATA+0x500u,params,sizeof(params)));
    CHECK_EQ_U32(dsound_stream_cache_i3dl2(stream,SCRATCH_DATA+0x500u,0u),0u);
    CHECK_EQ_U32(dsound_stream_cache_min_distance(stream,0x3F800000u,0u),0u);
    CHECK_EQ_U32(dsound_stream_cache_rolloff(stream,0x4B914Cu,4u,0u),0u);
    CHECK_EQ_U32(dsound_stream_cache_volume(stream,-10000),0u);
    CHECK_EQ_U32(dsound_stream_cache_pause(stream,1u),0u);
    CHECK_EQ_U32(dsound_stream_cache_flush_ex(stream,0u,0u,1u),0u);
    CHECK_EQ_U32(dsound_stream_cache_discontinuity(stream),0u);
    return stream;
}
static void test_stream_frequency_owned_route(void)
{
    for(unsigned profile=0u;profile<2u;profile++) {
    dsound_completion_reset();fake_now=3000u;
    CHECK(dsound_audio_runtime_start(48000u,49612u));
    dsound_stream_set_routing_note(dsound_completion_stream_routing);
    dsound_stream_set_frequency_note(dsound_completion_note_frequency,frequency_control_word);
    CHECK_EQ_U32(dsound_stream_register(),14u);
    dsound_stream_set_format_note(dsound_completion_note_format);
    const uint32_t stream=make_frequency_stream(SCRATCH_DATA+0x7D0u,profile==1u);
    dsound_stream_snapshot before,after;
    CHECK_EQ_U32(invoke_stream_frequency(0x29B35u,stream,0u),0u);
    CHECK(dsound_stream_get_snapshot(stream,&before));
    CHECK_EQ_U32(before.source_rate_hz,44100u);CHECK_EQ_U32((uint32_t)before.frequency_pitch,0xFFFFFE0Bu);
    CHECK_EQ_U32(before.frequency_sets,1u);
    RUN_EXPECTING_FATAL((void)invoke_stream_frequency(0x29B30u,stream,32000u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)invoke_stream_frequency(0x29B35u,stream,7999u));CHECK(fatal_seen);
    frequency_cw=0x0C7Fu;
    RUN_EXPECTING_FATAL((void)invoke_stream_frequency(0x29B35u,stream,32000u));CHECK(fatal_seen);
    frequency_cw=0x037Fu;
    const uint32_t ref=load(stream+8u);store(stream+8u,ref+1u);
    store(0x4124A8u,1u);
    CHECK_EQ_U32(invoke_stream_frequency(0u,0xDEADBEEFu,0u),0x80004005u);
    store(0x4124A8u,0u);
    RUN_EXPECTING_FATAL((void)invoke_stream_frequency(0x29B35u,stream,32000u));CHECK(fatal_seen);
    store(stream+8u,ref);
    CHECK(dsound_stream_get_snapshot(stream,&after));CHECK(memcmp(&before,&after,sizeof(after))==0);
    /* Exercise zero before any SetFormat as well as afterward. */
    CHECK_EQ_U32(invoke_stream_frequency(0x29B35u,stream,88200u),0u);
    CHECK_EQ_U32(invoke_stream_frequency(0x29B35u,stream,0u),0u);
    CHECK(dsound_stream_get_snapshot(stream,&after));CHECK_EQ_U32(after.source_rate_hz,44100u);
    CHECK_EQ_U32(invoke_stream_frequency(0x29B35u,stream,88200u),0u);
    /* Original SetFormat resets effective pitch and the zero-frequency base.
     * The empty owned completion and PCM clocks commit together. */
    uint8_t new_format[20];memcpy(new_format,stereo_startup,sizeof(new_format));
    const uint32_t format_rate=32000u,format_average=profile==1u?18000u:36000u;
    if(profile==1u){new_format[2]=1u;new_format[12]=36u;}
    memcpy(new_format+4u,&format_rate,4u);memcpy(new_format+8u,&format_average,4u);
    CHECK(kernel_guest_write_bytes(format_at,new_format,sizeof(new_format)));
    CHECK_EQ_U32(dsound_stream_cache_set_format(stream,format_at),0u);
    CHECK(dsound_stream_get_snapshot(stream,&after));
    CHECK_EQ_U32(after.source_rate_hz,32000u);
    CHECK_EQ_U32((uint32_t)after.frequency_pitch,0xFFFFF6A4u);
    CHECK_EQ_U32(invoke_stream_frequency(0x29B35u,stream,96000u),0u);
    CHECK_EQ_U32(invoke_stream_frequency(0x29B35u,stream,0u),0u);
    CHECK(dsound_stream_get_snapshot(stream,&after));CHECK_EQ_U32(after.source_rate_hz,32000u);
    CHECK_EQ_U32(invoke_stream_frequency(0x29B35u,stream,88200u),0u);
    CHECK_EQ_U32(process(stream,profile==1u?36u:72u,0u,true),0u);
    CHECK_EQ_U32(process(stream,profile==1u?36u:72u,1u,true),0u);
    dsound_completion_census_entry census_rows[DSOUND_COMPLETION_CENSUS_MAX];
    const size_t census_count=dsound_completion_stream_census(census_rows,DSOUND_COMPLETION_CENSUS_MAX);
    CHECK_EQ_U32(census_count,1u);CHECK_EQ_U32(census_rows[0].channels,profile==1u?1u:2u);
    CHECK_EQ_U32(census_rows[0].rate,32000u);CHECK_EQ_U32(census_rows[0].mixed,1u);
    CHECK_EQ_U32(dsound_stream_cache_set_format(stream,format_at),0u);
    CHECK_EQ_U32(load(packet_words(0u)),profile==1u?36u:72u);
    CHECK_EQ_U32(load(packet_words(0u)+4u),0x80004004u);
    CHECK_EQ_U32(dsound_completion_get_stats().stream_aborted,2u);
    CHECK_EQ_U32(load(packet_words(1u)),profile==1u?36u:72u);
    CHECK_EQ_U32(load(packet_words(1u)+4u),0x80004004u);
    CHECK(dsound_stream_get_snapshot(stream,&after));CHECK_EQ_U32(after.source_rate_hz,32000u);
    /* Fresh packets use the new format even though the historical source clock
     * retains its old base to preserve PCM before the format timestamp. */
    CHECK_EQ_U32(process(stream,profile==1u?36u:72u,2u,true),0u);
    CHECK_EQ_U32(dsound_stream_cache_pause(stream,0u),0u);
    run(99u);dsound_completion_work();CHECK_EQ_U32(load(packet_words(2u)+4u),PENDING);
    run(1u);dsound_completion_work();CHECK_EQ_U32(load(packet_words(2u)+4u),0u);
    CHECK_EQ_U32(load(packet_words(0u)+4u),0x80004004u);
    CHECK_EQ_U32(invoke_stream_frequency(0x29B35u,stream,0u),0u);
    CHECK(dsound_stream_get_snapshot(stream,&after));CHECK_EQ_U32(after.source_rate_hz,32000u);
    dsound_stream_set_format_note(NULL);
    dsound_stream_set_frequency_note(NULL,NULL);dsound_stream_set_routing_note(NULL);
    dsound_audio_runtime_stop();CHECK(dsound_stream_reset_checked());frequency_cw=0x027Fu;
    dsound_hle_init();(void)dsound_stream_register();(void)dsound_buffer_register();(void)dsound_completion_register();    }
}

static void test_stream_format_abort_pcm_rejection(void)
{
    for(unsigned failure=0u;failure<2u;failure++) {
    dsound_completion_reset();fake_now=3000u;
    CHECK(dsound_audio_runtime_start(48000u,49612u));
    dsound_stream_set_routing_note(dsound_completion_stream_routing);
    dsound_stream_set_frequency_note(dsound_completion_note_frequency,frequency_control_word);
    dsound_stream_set_format_note(dsound_completion_note_format);
    const uint32_t stream=make_stream(SCRATCH_DATA+0x7D0u);
    if(failure==0u)CHECK_EQ_U32(dsound_stream_set_frequency(stream,88200u,0x027Fu),0u);
    const uint32_t saved_words_at=words_at;
    if(failure==1u)words_at=DATA_BASE+0x2000u;
    CHECK_EQ_U32(process(stream,72u,0u,true),0u);
    CHECK(kernel_guest_write_bytes(format_at,stereo_startup,20u));
    dsound_stream_snapshot before,after;CHECK(dsound_stream_get_snapshot(stream,&before));
    const dsound_completion_stats before_stats=dsound_completion_get_stats();
    if(failure==0u)dsound_audio_runtime_stop();
    else CHECK(mprotect((void *)(uintptr_t)words_at,4096u,PROT_READ)==0);
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_set_format(stream,format_at));CHECK(fatal_seen);
    CHECK(dsound_stream_get_snapshot(stream,&after));CHECK(memcmp(&before,&after,sizeof(after))==0);
    const dsound_completion_stats after_stats=dsound_completion_get_stats();
    CHECK(memcmp(&before_stats,&after_stats,sizeof(before_stats))==0);
    CHECK_EQ_U32(load(packet_words(0u)),0u);CHECK_EQ_U32(load(packet_words(0u)+4u),PENDING);
    if(failure==1u)CHECK(mprotect((void *)(uintptr_t)words_at,4096u,PROT_READ|PROT_WRITE)==0);
    words_at=saved_words_at;dsound_audio_runtime_stop();
    dsound_stream_set_format_note(NULL);dsound_stream_set_frequency_note(NULL,NULL);
    dsound_stream_set_routing_note(NULL);dsound_completion_reset();CHECK(dsound_stream_reset_checked());    }
}

static void test_stream_timeline(void)
{
    dsound_completion_reset();fake_now=1000u;
    const uint32_t stream=make_stream(SCRATCH_DATA+0x700u);
    /* Measured: a started, empty, paused stream is 1 (free list non-empty), nothing else. */
    CHECK_EQ_U32(status_of(stream),1u);
    CHECK_EQ_U32(process(stream,288u,0u,true),0u);
    CHECK_EQ_U32(load(packet_words(0)),0u);CHECK_EQ_U32(load(packet_words(0)+4u),PENDING);
    /* Measured: paused with data is 0x20001, a paused stream consumes no clock. */
    CHECK_EQ_U32(status_of(stream),0x20001u);
    run(1000000u);CHECK_EQ_U32(status_of(stream),0x20001u);CHECK_EQ_U32(load(packet_words(0)+4u),PENDING);
    /* Pause(0) resumes: measured 0x10001, the packet needs exactly size/average clock ticks (frequency 49612 so ticks == bytes). */
    CHECK_EQ_U32(dsound_stream_cache_pause(stream,0u),0u);
    CHECK_EQ_U32(status_of(stream),0x10001u);
    run(287u);CHECK_EQ_U32(status_of(stream),0x10001u);CHECK_EQ_U32(load(packet_words(0)),0u);
    run(1u);CHECK_EQ_U32(load(packet_words(0)),0u);  /* not observed yet, nothing wrote the words */
    CHECK_EQ_U32(status_of(stream),1u);               /* drained: the playing bit is gone (INFERRED) */
    CHECK_EQ_U32(load(packet_words(0)),288u);CHECK_EQ_U32(load(packet_words(0)+4u),0u);
    /* Measured: three packets fill the list (bit 0 clears), the fourth is 0x88780032 and untouched. */
    CHECK_EQ_U32(process(stream,288u,0u,true),0u);CHECK_EQ_U32(status_of(stream),0x10001u);
    CHECK_EQ_U32(process(stream,288u,1u,true),0u);CHECK_EQ_U32(status_of(stream),0x10001u);
    CHECK_EQ_U32(process(stream,100u,2u,true),0u);CHECK_EQ_U32(status_of(stream),0x10000u);
    CHECK_EQ_U32(process(stream,288u,3u,true),FULL);
    CHECK_EQ_U32(load(packet_words(3)),0xDEADu);CHECK_EQ_U32(load(packet_words(3)+4u),0xBEEFu);
    /* The refused packet is never read, and nothing writes into it either (the descriptor words stay as the title left them). */
    CHECK_EQ_U32(load(packet_at+8u),packet_words(3));
    /* In order, back to back: 288 + 288 + 100 ticks. */
    run(287u);CHECK_EQ_U32(status_of(stream),0x10000u);CHECK_EQ_U32(load(packet_words(0)+4u),PENDING);
    run(1u);CHECK_EQ_U32(status_of(stream),0x10001u);CHECK_EQ_U32(load(packet_words(0)+4u),0u);
    CHECK_EQ_U32(load(packet_words(1)+4u),PENDING);
    run(288u);CHECK_EQ_U32(status_of(stream),0x10001u);CHECK_EQ_U32(load(packet_words(1)),288u);
    run(99u);CHECK_EQ_U32(status_of(stream),0x10001u);CHECK_EQ_U32(load(packet_words(2)+4u),PENDING);
    run(1u);CHECK_EQ_U32(status_of(stream),1u);CHECK_EQ_U32(load(packet_words(2)),100u);
    /* A pause in the middle keeps the remaining time. */
    CHECK_EQ_U32(process(stream,288u,0u,true),0u);run(100u);
    CHECK_EQ_U32(dsound_stream_cache_pause(stream,1u),0u);run(5000u);
    CHECK_EQ_U32(status_of(stream),0x20001u);
    CHECK_EQ_U32(dsound_stream_cache_pause(stream,0u),0u);
    run(187u);CHECK_EQ_U32(status_of(stream),0x10001u);run(1u);CHECK_EQ_U32(status_of(stream),1u);
    /* Null completion words are accepted by the original and never written. */
    CHECK_EQ_U32(process(stream,72u,0u,false),0u);run(72u);CHECK_EQ_U32(status_of(stream),1u);
    /* DirectSoundDoWork writes the words on time even when nobody asks for the status. */
    CHECK_EQ_U32(process(stream,72u,0u,true),0u);run(72u);
    CHECK_EQ_U32(load(packet_words(0)+4u),PENDING);dsound_completion_work();CHECK_EQ_U32(load(packet_words(0)+4u),0u);
    /* GetInfo is the measured XMEDIAINFO {5, block, 0, 2 blocks}. */
    store(words_at,0u);CHECK_EQ_U32(dsound_completion_stream_info(stream,words_at),0u);
    CHECK_EQ_U32(load(words_at),5u);CHECK_EQ_U32(load(words_at+4u),72u);CHECK_EQ_U32(load(words_at+8u),0u);CHECK_EQ_U32(load(words_at+12u),144u);
    dsound_completion_stats stats=dsound_completion_get_stats();
    CHECK_EQ_U32(stats.stream_packets,7u);CHECK_EQ_U32(stats.stream_completed,7u);
    /* T1238: every packet is in the format census, none mixed (no audio runtime here), at the startup format 44100 Hz stereo ADPCM. */
    dsound_completion_census_entry census[DSOUND_COMPLETION_CENSUS_MAX];
    const size_t census_count=dsound_completion_stream_census(census,DSOUND_COMPLETION_CENSUS_MAX);
    CHECK_EQ_U32((uint32_t)census_count,1u);
    CHECK_EQ_U32(census[0].tag,0x69u);CHECK_EQ_U32(census[0].channels,2u);CHECK_EQ_U32(census[0].rate,44100u);
    CHECK_EQ_U32(census[0].block,72u);CHECK_EQ_U32(census[0].bits,4u);
    CHECK_EQ_U32(census[0].packets,7u);CHECK_EQ_U32(census[0].mixed,0u);
    CHECK(census[0].bytes>=census[0].packets);
}
/* T855 (INFERRED, opt-in): with DoWork delivery a packet past its deadline keeps its words and its list slot until DoWork. */
static void test_dowork_delivery(void)
{
    dsound_completion_reset();fake_now=5000u;
    const uint32_t stream=make_stream(SCRATCH_DATA+0x7C0u);
    CHECK(!dsound_completion_dowork_delivery());
    dsound_completion_set_dowork_delivery(true);CHECK(dsound_completion_dowork_delivery());
    CHECK_EQ_U32(dsound_stream_cache_pause(stream,0u),0u);
    /* 288 + 72 + 72 ticks, the list (3 packets) is full */
    CHECK_EQ_U32(process(stream,288u,0u,true),0u);CHECK_EQ_U32(process(stream,72u,1u,true),0u);CHECK_EQ_U32(process(stream,72u,2u,true),0u);
    CHECK_EQ_U32(status_of(stream),0x10000u);
    run(287u);CHECK_EQ_U32(status_of(stream),0x10000u);dsound_completion_work();CHECK_EQ_U32(load(packet_words(0)+4u),PENDING);
    /* past the first deadline: the status poll still sees the packet, the words are untouched, a fourth Process is refused */
    run(1u);CHECK_EQ_U32(status_of(stream),0x10000u);
    CHECK_EQ_U32(load(packet_words(0)),0u);CHECK_EQ_U32(load(packet_words(0)+4u),PENDING);
    CHECK_EQ_U32(process(stream,72u,3u,true),FULL);CHECK_EQ_U32(dsound_completion_get_stats().stream_completed,0u);
    /* time keeps running into the second packet while the first waits: it is not charged again, the second is not finished */
    run(10u);CHECK_EQ_U32(status_of(stream),0x10000u);CHECK_EQ_U32(load(packet_words(1)+4u),PENDING);
    /* DoWork delivers it: size and status written, the slot recycled */
    dsound_completion_work();
    CHECK_EQ_U32(load(packet_words(0)),288u);CHECK_EQ_U32(load(packet_words(0)+4u),0u);
    CHECK_EQ_U32(load(packet_words(1)+4u),PENDING);CHECK_EQ_U32(status_of(stream),0x10001u);
    CHECK_EQ_U32(dsound_completion_get_stats().stream_completed,1u);
    /* the next packet counted from the first deadline, not from DoWork: two more deadlines pass, both wait, one DoWork delivers both in order */
    run(144u);CHECK_EQ_U32(status_of(stream),0x10001u);CHECK_EQ_U32(load(packet_words(1)+4u),PENDING);CHECK_EQ_U32(load(packet_words(2)+4u),PENDING);
    CHECK_EQ_U32(process(stream,72u,3u,true),0u);CHECK_EQ_U32(status_of(stream),0x10000u);
    dsound_completion_work();
    CHECK_EQ_U32(load(packet_words(1)),72u);CHECK_EQ_U32(load(packet_words(1)+4u),0u);CHECK_EQ_U32(load(packet_words(2)),72u);CHECK_EQ_U32(load(packet_words(2)+4u),0u);
    CHECK_EQ_U32(load(packet_words(3)+4u),PENDING);CHECK_EQ_U32(dsound_completion_get_stats().stream_completed,3u);
    /* a paused stream consumes nothing and DoWork delivers nothing more */
    CHECK_EQ_U32(call_frame(0x407B23u,0x29ED1u,stream,1u,0u,0u,2u),0u);run(5000u);dsound_completion_work();
    CHECK_EQ_U32(load(packet_words(3)+4u),PENDING);CHECK_EQ_U32(dsound_completion_get_stats().stream_completed,3u);
    CHECK_EQ_U32(call_frame(0x407B23u,0x29F13u,stream,0u,0u,0u,2u),0u);run(72u);dsound_completion_work();
    CHECK_EQ_U32(load(packet_words(3)),72u);CHECK_EQ_U32(status_of(stream),1u);
    CHECK_EQ_U32(dsound_completion_get_stats().stream_completed,4u);
    /* mode off again: the T681 behaviour, words at the first observation */
    dsound_completion_set_dowork_delivery(false);
    CHECK_EQ_U32(process(stream,72u,0u,true),0u);run(72u);CHECK_EQ_U32(status_of(stream),1u);CHECK_EQ_U32(load(packet_words(0)+4u),0u);
}
static void test_stream_refusals(void)
{
    dsound_completion_reset();fake_now=1u;
    const uint32_t stream=make_stream(SCRATCH_DATA+0x704u);
    uint32_t packet[6]={DATA_BASE,288u,packet_words(0),packet_words(0)+4u,0u,0u};
    /* The output packet, an event and a timestamp are not modelled. */
    CHECK(kernel_guest_write_bytes(packet_at,packet,sizeof(packet)));
    RUN_EXPECTING_FATAL((void)dsound_completion_stream_process(stream,packet_at,SCRATCH_DATA+0x40u));CHECK(fatal_seen);
    packet[4]=0x1234u;CHECK(kernel_guest_write_bytes(packet_at,packet,sizeof(packet)));
    RUN_EXPECTING_FATAL((void)dsound_completion_stream_process(stream,packet_at,0u));CHECK(fatal_seen);
    packet[4]=0u;packet[5]=0x1234u;CHECK(kernel_guest_write_bytes(packet_at,packet,sizeof(packet)));
    RUN_EXPECTING_FATAL((void)dsound_completion_stream_process(stream,packet_at,0u));CHECK(fatal_seen);
    packet[5]=0u;packet[1]=0u;CHECK(kernel_guest_write_bytes(packet_at,packet,sizeof(packet)));
    RUN_EXPECTING_FATAL((void)dsound_completion_stream_process(stream,packet_at,0u));CHECK(fatal_seen);
    packet[1]=288u;packet[0]=0u;CHECK(kernel_guest_write_bytes(packet_at,packet,sizeof(packet)));
    RUN_EXPECTING_FATAL((void)dsound_completion_stream_process(stream,packet_at,0u));CHECK(fatal_seen);
    packet[0]=DATA_BASE;packet[3]=packet[2];CHECK(kernel_guest_write_bytes(packet_at,packet,sizeof(packet)));
    RUN_EXPECTING_FATAL((void)dsound_completion_stream_process(stream,packet_at,0u));CHECK(fatal_seen);
    packet[3]=stream+8u;CHECK(kernel_guest_write_bytes(packet_at,packet,sizeof(packet)));
    RUN_EXPECTING_FATAL((void)dsound_completion_stream_process(stream,packet_at,0u));CHECK(fatal_seen);
    /* A stream the passive adapter does not own, a null or aliasing status output. */
    RUN_EXPECTING_FATAL((void)dsound_completion_stream_status(0x12345678u,words_at));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_completion_stream_status(stream,0u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_completion_stream_status(stream,stream+4u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_completion_stream_info(stream,0u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_completion_stream_info(0x12345678u,words_at));CHECK(fatal_seen);
    /* Nothing refused changed the queue. */
    CHECK_EQ_U32(dsound_completion_get_stats().stream_packets,0u);
    /* A stream whose startup is not recorded has no status. */
    const uint32_t desc[6]={0u,3u,format_at,0u,0u,0u};
    CHECK(kernel_guest_write_bytes(desc_at,desc,sizeof(desc)));CHECK(kernel_guest_write_bytes(format_at,stereo_startup,20u));
    CHECK_EQ_U32(dsound_stream_create(desc_at,SCRATCH_DATA+0x708u),0u);
    RUN_EXPECTING_FATAL((void)dsound_completion_stream_status(load(SCRATCH_DATA+0x708u),words_at));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_completion_stream_process(load(SCRATCH_DATA+0x708u),packet_at,0u));CHECK(fatal_seen);
    /* Policy off: nothing is admitted. */
    dsound_completion_set_enabled(false);
    RUN_EXPECTING_FATAL((void)dsound_completion_stream_status(stream,words_at));CHECK(fatal_seen);
    CHECK(!dsound_completion_method_owned(0u));
    dsound_completion_set_enabled(true);
}
static uint32_t call_frame(uint32_t entry,uint32_t caller,uint32_t a,uint32_t b,uint32_t c,uint32_t d,unsigned count)
{
    uint32_t args[4]={a,b,c,d};kernel_call_frame frame={0};
    CHECK(kernel_frame_build(&frame,SCRATCH_DATA+0x200u,0x100u,args,count));store(frame.stack_ptr,caller);
    return dsound_hle_call(entry,&frame);
}
static uint32_t make_buffer(uint32_t output,bool started)
{
    uint8_t d[24]={0};const uint32_t words[6]={24u,0u,0u,format_at,0u,0u};memcpy(d,words,sizeof(d));
    CHECK(kernel_guest_write_bytes(desc_at,d,sizeof(d)));CHECK(kernel_guest_write_bytes(format_at,buffer_format,20u));
    CHECK_EQ_U32(dsound_buffer_create(device+8u,desc_at,output,0u),0u);
    const uint32_t buffer=load(output);
    if(started){
        CHECK_EQ_U32(dsound_buffer_set_data(buffer,DATA_BASE,1440u),0u);CHECK_EQ_U32(dsound_buffer_set_volume(buffer,-10000),0u);
        CHECK_EQ_U32(dsound_buffer_pause(buffer,0u),0u);CHECK_EQ_U32(dsound_buffer_set_frequency(buffer,22042u),0u);
    }
    return buffer;
}
static uint32_t buffer_status(uint32_t buffer)
{store(words_at,0xA5A5A5A5u);CHECK_EQ_U32(dsound_completion_buffer_status(buffer,words_at),0u);return load(words_at);}
static dsound_completion_buffer_position buffer_position(uint32_t buffer)
{dsound_completion_buffer_position position={0};CHECK(dsound_completion_buffer_get_position(buffer,&position));return position;}
static void test_buffer_play(void)
{
    dsound_completion_reset();fake_now=500u;
    const uint32_t buffer=make_buffer(0x5818E8u,true);
    /* Measured: a started buffer nothing played is 0. */
    CHECK_EQ_U32(buffer_status(buffer),0u);
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,0u,4u),0u);
    /* 1440 bytes = 40 blocks of 64 samples = 2560 samples at 22042 Hz: 2560 ticks at a 22042 Hz clock. */
    CHECK_EQ_U32(buffer_status(buffer),1u);
    run(2559u);CHECK_EQ_U32(buffer_status(buffer),1u);
    run(1u);CHECK_EQ_U32(buffer_status(buffer),0u);
    /* Looping (Play flags 1): measured 5, it never finishes. */
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,1u,4u),0u);
    CHECK_EQ_U32(buffer_status(buffer),5u);run(1000000u);CHECK_EQ_U32(buffer_status(buffer),5u);
    /* Measured under Unicorn (T722): flags 2 (FROMSTARTONLY) answers status 1 like 0, flags 3 answers 5 like 1. */
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,2u,4u),0u);
    CHECK_EQ_U32(buffer_status(buffer),1u);run(2559u);CHECK_EQ_U32(buffer_status(buffer),1u);
    run(1u);CHECK_EQ_U32(buffer_status(buffer),0u);
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,3u,4u),0u);
    CHECK_EQ_U32(buffer_status(buffer),5u);run(1000000u);CHECK_EQ_U32(buffer_status(buffer),5u);
    /* Measured: Play(0) after Play(3) ends the looping (status 1), flags 2 after 1 is status 1. */
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,0u,4u),0u);CHECK_EQ_U32(buffer_status(buffer),1u);
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,1u,4u),0u);
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,2u,4u),0u);CHECK_EQ_U32(buffer_status(buffer),1u);run(2560u);
    CHECK_EQ_U32(buffer_status(buffer),0u);
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,3u,4u),0u);
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,0u,4u),0u);CHECK_EQ_U32(buffer_status(buffer),1u);run(2560u);
    CHECK_EQ_U32(buffer_status(buffer),0u);
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,1u,4u),0u);
    /* Pause and SetFrequency of a played buffer are modelled (test_buffer_pause, test_buffer_set_frequency_played). */
    /* Play restarts the countdown. */
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,0u,4u),0u);run(2560u);CHECK_EQ_U32(buffer_status(buffer),0u);
    dsound_completion_stats stats=dsound_completion_get_stats();
    CHECK_EQ_U32(stats.buffer_plays,11u);CHECK_EQ_U32(stats.buffer_finished,5u);
    /* DirectSoundDoWork finishes a played buffer on time without anyone asking for its status. */
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,0u,4u),0u);run(2560u);
    CHECK_EQ_U32(dsound_completion_get_stats().buffer_finished,5u);
    dsound_completion_work();
    CHECK_EQ_U32(dsound_completion_get_stats().buffer_finished,6u);
    /* T733: the call site is not part of the state check (the original never reads its caller), any return address Plays,
     * the call address 0x28693 of the T681 constant, a neighbour, a stack address and zero among them. */
    const uint32_t callers[]={0x28693u,0x28694u,0x28698u,0x1234u,0x7FFFFFF0u,0u};
    for(unsigned i=0u;i<sizeof(callers)/sizeof(callers[0]);i++){
        CHECK_EQ_U32(call_frame(0x407A80u,callers[i],buffer,0u,0u,2u,4u),0u);CHECK_EQ_U32(buffer_status(buffer),1u);run(2560u);
        CHECK_EQ_U32(buffer_status(buffer),0u);
    }
    /* Refusals: flags, reserved words, an unstarted buffer, an unowned buffer, a null output. */
    RUN_EXPECTING_FATAL((void)call_frame(0x407A80u,0x28698u,buffer,0u,0u,4u,4u));CHECK(strstr(fatal_text,"got 0x4")!=NULL);
    RUN_EXPECTING_FATAL((void)call_frame(0x407A80u,0x28698u,buffer,0u,0u,4u,4u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)call_frame(0x407A80u,0x28698u,buffer,1u,0u,0u,4u));CHECK(fatal_seen);
    CHECK(strstr(fatal_text,"reserved arguments must be zero, got (")!=NULL);
    RUN_EXPECTING_FATAL((void)call_frame(0x407A80u,0x28698u,buffer,0u,1u,0u,4u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)call_frame(0x407A80u,0x28698u,0x12345678u,0u,0u,0u,4u));CHECK(fatal_seen);
    const uint32_t fresh=make_buffer(0x5818ECu,false);
    RUN_EXPECTING_FATAL((void)call_frame(0x407A80u,0x28698u,fresh,0u,0u,0u,4u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_completion_buffer_status(buffer,0u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_completion_buffer_status(0x12345678u,words_at));CHECK(fatal_seen);
    /* A buffer Play never started and the unstarted one report 0. */
    CHECK_EQ_U32(buffer_status(fresh),0u);
}
static void test_buffer_pause(void)
{
    dsound_completion_reset();fake_now=500u;
    const uint32_t buffer=make_buffer(0x581940u,true);
    const uint32_t PAUSE=0x407ABCu,CALLER=0x2751Eu;
    /* A buffer nothing played: Pause(0) is the recorded startup no-op, Pause(1) stays refused (measured result 1, not modelled). */
    CHECK_EQ_U32(call_frame(PAUSE,CALLER,buffer,0u,0u,0u,2u),0u);CHECK_EQ_U32(buffer_status(buffer),0u);
    RUN_EXPECTING_FATAL((void)call_frame(PAUSE,CALLER,buffer,1u,0u,0u,2u));CHECK(fatal_seen);
    /* Measured on a playing buffer: Pause(0) changes nothing, Pause(1) and (2) move 1 to 2 and 5 to 6, Pause(0) resumes. */
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,2u,4u),0u);
    CHECK_EQ_U32(call_frame(PAUSE,CALLER,buffer,0u,0u,0u,2u),0u);CHECK_EQ_U32(buffer_status(buffer),1u);
    run(1000u);
    CHECK_EQ_U32(call_frame(PAUSE,CALLER,buffer,1u,0u,0u,2u),0u);CHECK_EQ_U32(buffer_status(buffer),2u);
    /* A paused buffer consumes no time. */
    run(5000000u);CHECK_EQ_U32(buffer_status(buffer),2u);
    CHECK_EQ_U32(call_frame(PAUSE,CALLER,buffer,2u,0u,0u,2u),0u);CHECK_EQ_U32(buffer_status(buffer),2u);
    CHECK_EQ_U32(call_frame(PAUSE,CALLER,buffer,0u,0u,0u,2u),0u);CHECK_EQ_U32(buffer_status(buffer),1u);
    /* 2560 ticks in all, 1000 spent before the pause. */
    run(1559u);CHECK_EQ_U32(buffer_status(buffer),1u);run(1u);CHECK_EQ_U32(buffer_status(buffer),0u);
    /* A finished buffer takes Pause(0) as a no-op, Pause(1) and Pause(2) and other modes are refused. */
    CHECK_EQ_U32(call_frame(PAUSE,CALLER,buffer,0u,0u,0u,2u),0u);CHECK_EQ_U32(buffer_status(buffer),0u);
    RUN_EXPECTING_FATAL((void)call_frame(PAUSE,CALLER,buffer,1u,0u,0u,2u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)call_frame(PAUSE,CALLER,buffer,2u,0u,0u,2u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)call_frame(PAUSE,CALLER,buffer,3u,0u,0u,2u));CHECK(fatal_seen);
    /* Looping: 5, paused 6, Play(0) on the paused buffer keeps it paused (2), Pause(0) then 1, a looping Play(1) while paused is 6. */
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,3u,4u),0u);CHECK_EQ_U32(buffer_status(buffer),5u);
    CHECK_EQ_U32(call_frame(PAUSE,CALLER,buffer,1u,0u,0u,2u),0u);CHECK_EQ_U32(buffer_status(buffer),6u);
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,0u,4u),0u);CHECK_EQ_U32(buffer_status(buffer),2u);
    CHECK_EQ_U32(call_frame(PAUSE,CALLER,buffer,0u,0u,0u,2u),0u);CHECK_EQ_U32(buffer_status(buffer),1u);
    CHECK_EQ_U32(call_frame(PAUSE,CALLER,buffer,1u,0u,0u,2u),0u);
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,1u,4u),0u);CHECK_EQ_U32(buffer_status(buffer),6u);
    CHECK_EQ_U32(call_frame(PAUSE,CALLER,buffer,0u,0u,0u,2u),0u);CHECK_EQ_U32(buffer_status(buffer),5u);
    /* An unmeasured mode of a PLAYING buffer is refused too. */
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,0u,4u),0u);
    RUN_EXPECTING_FATAL((void)call_frame(PAUSE,CALLER,buffer,3u,0u,0u,2u));CHECK(fatal_seen);
    CHECK_EQ_U32(buffer_status(buffer),1u);
    /* T733: the call site is not part of the state check, any caller of Pause answers the same (the original never reads it). */
    CHECK_EQ_U32(call_frame(PAUSE,CALLER+1u,buffer,0u,0u,0u,2u),0u);CHECK_EQ_U32(call_frame(PAUSE,0u,buffer,1u,0u,0u,2u),0u);
    CHECK_EQ_U32(buffer_status(buffer),2u);CHECK_EQ_U32(call_frame(PAUSE,0x1234u,buffer,0u,0u,0u,2u),0u);CHECK_EQ_U32(buffer_status(buffer),1u);
}
/* T722: SetFrequency of a played buffer (measured under Unicorn: S_OK, status unchanged, only the voice pitch changes). */
static void test_buffer_set_frequency_played(void)
{
    dsound_completion_reset();fake_now=500u;
    const uint32_t buffer=make_buffer(0x581950u,true);
    const uint32_t FREQUENCY=0x4084F2u;
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,2u,4u),0u);
    /* 2560 ticks at 22042 Hz, 1001 spent: 1559 left, doubling the rate halves the time left (779.5 rounds UP to 780). */
    run(1001u);CHECK_EQ_U32(dsound_buffer_set_frequency(buffer,44084u),0u);CHECK_EQ_U32(buffer_status(buffer),1u);
    run(779u);CHECK_EQ_U32(buffer_status(buffer),1u);run(1u);CHECK_EQ_U32(buffer_status(buffer),0u);
    /* The title restarts the same buffer: the old rate is the recorded one, halving it doubles the time left. */
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,2u,4u),0u);
    CHECK_EQ_U32(call_frame(FREQUENCY,0x27547u,buffer,22042u,0u,0u,2u),0u);
    run(2559u);CHECK_EQ_U32(buffer_status(buffer),1u);
    CHECK_EQ_U32(call_frame(FREQUENCY,0x27547u,buffer,22042u,0u,0u,2u),0u);
    /* Looping and paused buffers keep their status, a finished one stays 0. */
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,3u,4u),0u);
    CHECK_EQ_U32(dsound_buffer_set_frequency(buffer,11021u),0u);CHECK_EQ_U32(buffer_status(buffer),5u);
    run(1000000u);CHECK_EQ_U32(buffer_status(buffer),5u);
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,0u,4u),0u);
    CHECK_EQ_U32(call_frame(0x407ABCu,0x2751Eu,buffer,1u,0u,0u,2u),0u);
    CHECK_EQ_U32(dsound_buffer_set_frequency(buffer,22042u),0u);CHECK_EQ_U32(buffer_status(buffer),2u);
    run(5000000u);CHECK_EQ_U32(buffer_status(buffer),2u);
    CHECK_EQ_U32(call_frame(0x407ABCu,0x2751Eu,buffer,0u,0u,0u,2u),0u);CHECK_EQ_U32(buffer_status(buffer),1u);
    /* 11021 Hz left 5120 ticks of samples, now at 22042 Hz 2560 ticks. */
    run(2559u);CHECK_EQ_U32(buffer_status(buffer),1u);run(1u);CHECK_EQ_U32(buffer_status(buffer),0u);
    CHECK_EQ_U32(dsound_buffer_set_frequency(buffer,22042u),0u);CHECK_EQ_U32(buffer_status(buffer),0u);
    /* A frequency outside the title's clamp is still refused on a played buffer. */
    RUN_EXPECTING_FATAL((void)dsound_buffer_set_frequency(buffer,0xBBu));CHECK(fatal_seen);
    CHECK(strstr(fatal_text,"(buffer ")!=NULL);CHECK(strstr(fatal_text,", argument 0xbb,")!=NULL);
}
/* T733: Stop 0x407AA4 (stdcall RET 4), measured on the original by tests/test_dsound_completion_oracle.py: S_OK, a playing buffer loses its
 * pause and keeps draining (status 1), a buffer nothing played is left alone (status 0). The model keeps the samples left. */
static uint32_t stop_call(uint32_t buffer,uint32_t caller){return call_frame(0x407AA4u,caller,buffer,0u,0u,0u,1u);}
static void test_buffer_stop(void)
{
    dsound_completion_reset();fake_now=500u;
    const uint32_t PAUSE=0x407ABCu,CALLER=0x2751Eu,STOP_CALLER=0x27468u;
    const uint32_t buffer=make_buffer(0x581970u,true);
    /* Measured: a buffer nothing played is S_OK and stays status 0. */
    CHECK_EQ_U32(stop_call(buffer,STOP_CALLER),0u);CHECK_EQ_U32(buffer_status(buffer),0u);
    /* Measured: Stop of a played buffer leaves status 1 and the voice keeps draining, 2560 ticks in all, 1000 spent. */
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,0u,4u),0u);run(1000u);
    CHECK_EQ_U32(stop_call(buffer,STOP_CALLER),0u);CHECK_EQ_U32(buffer_status(buffer),1u);
    run(1559u);CHECK_EQ_U32(buffer_status(buffer),1u);run(1u);CHECK_EQ_U32(buffer_status(buffer),0u);
    /* A buffer that finished is left alone (INFERRED, the drain is hardware), also by a second Stop. */
    CHECK_EQ_U32(stop_call(buffer,STOP_CALLER),0u);CHECK_EQ_U32(buffer_status(buffer),0u);
    /* Measured: a PAUSED buffer loses the pause and resumes (status 2 to 1), flags 2 as the title plays it. */
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,2u,4u),0u);run(1000u);
    CHECK_EQ_U32(call_frame(PAUSE,CALLER,buffer,1u,0u,0u,2u),0u);CHECK_EQ_U32(buffer_status(buffer),2u);
    run(5000000u);CHECK_EQ_U32(buffer_status(buffer),2u);
    CHECK_EQ_U32(stop_call(buffer,STOP_CALLER),0u);CHECK_EQ_U32(buffer_status(buffer),1u);
    run(1559u);CHECK_EQ_U32(buffer_status(buffer),1u);run(1u);CHECK_EQ_U32(buffer_status(buffer),0u);
    /* Measured: a second Stop answers the same and writes nothing, here it does not restart or extend the time left. */
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,2u,4u),0u);run(100u);
    CHECK_EQ_U32(stop_call(buffer,STOP_CALLER),0u);run(100u);CHECK_EQ_U32(stop_call(buffer,STOP_CALLER),0u);
    CHECK_EQ_U32(buffer_status(buffer),1u);run(2359u);CHECK_EQ_U32(buffer_status(buffer),1u);run(1u);CHECK_EQ_U32(buffer_status(buffer),0u);
    /* Measured: after Stop Pause(0) is the no-op (status 1), Pause(1) and Pause(2) answer the mode as the HRESULT and change nothing,
     * which is not modelled and refused by name. */
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,2u,4u),0u);CHECK_EQ_U32(stop_call(buffer,STOP_CALLER),0u);
    CHECK_EQ_U32(call_frame(PAUSE,CALLER,buffer,0u,0u,0u,2u),0u);CHECK_EQ_U32(buffer_status(buffer),1u);
    RUN_EXPECTING_FATAL((void)call_frame(PAUSE,CALLER,buffer,1u,0u,0u,2u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)call_frame(PAUSE,CALLER,buffer,2u,0u,0u,2u));CHECK(fatal_seen);
    CHECK_EQ_U32(buffer_status(buffer),1u);
    /* Measured: SetFrequency after Stop is S_OK and the voice pitch changes, the time left is rescaled: 1559 left at 22042 Hz, 44084 Hz
     * halves it (779.5 rounds UP to 780), the frequency 1001 ticks in. */
    run(2560u);CHECK_EQ_U32(buffer_status(buffer),0u);
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,0u,4u),0u);run(1001u);CHECK_EQ_U32(stop_call(buffer,STOP_CALLER),0u);
    CHECK_EQ_U32(dsound_buffer_set_frequency(buffer,44084u),0u);CHECK_EQ_U32(buffer_status(buffer),1u);
    run(779u);CHECK_EQ_U32(buffer_status(buffer),1u);run(1u);CHECK_EQ_U32(buffer_status(buffer),0u);
    /* Measured: Play after Stop restarts as usual, Pause(1) works again and the loop bit follows the new Play (status 5, 6). */
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,2u,4u),0u);CHECK_EQ_U32(stop_call(buffer,STOP_CALLER),0u);
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,3u,4u),0u);CHECK_EQ_U32(buffer_status(buffer),5u);
    CHECK_EQ_U32(call_frame(PAUSE,CALLER,buffer,1u,0u,0u,2u),0u);CHECK_EQ_U32(buffer_status(buffer),6u);
    CHECK_EQ_U32(call_frame(PAUSE,CALLER,buffer,0u,0u,0u,2u),0u);CHECK_EQ_U32(buffer_status(buffer),5u);
    /* A loop wraps twice plus part of a third pass. The virtual position holds through Pause(2),
     * rescales its ticks when pitch halves; Stop then drains the 560 ticks left. */
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,3u,4u),0u);
    const uint64_t loop_duration=(2560u*22042u+44084u-1u)/44084u;
    run(loop_duration*2u+1000u);CHECK_EQ_U32(buffer_status(buffer),5u);
    dsound_completion_buffer_position position=buffer_position(buffer);
    CHECK_EQ_U32(position.position_samples,2000u);CHECK_EQ_U32(position.total_samples,2560u);
    CHECK_EQ_U32(position.frequency,44084u);CHECK(position.playing && position.looping && !position.paused);
    CHECK_EQ_U32(call_frame(PAUSE,CALLER,buffer,2u,0u,0u,2u),0u);CHECK_EQ_U32(buffer_status(buffer),6u);
    run(5000000u);CHECK_EQ_U32(buffer_status(buffer),6u);
    position=buffer_position(buffer);CHECK_EQ_U32(position.position_samples,2000u);CHECK(position.paused && position.looping);
    CHECK_EQ_U32(dsound_buffer_set_frequency(buffer,22042u),0u);CHECK_EQ_U32(buffer_status(buffer),6u);
    position=buffer_position(buffer);CHECK_EQ_U32(position.position_samples,2000u);CHECK_EQ_U32(position.frequency,22042u);
    CHECK_EQ_U32(call_frame(PAUSE,CALLER,buffer,0u,0u,0u,2u),0u);CHECK_EQ_U32(buffer_status(buffer),5u);
    const uint32_t before=dsound_completion_get_stats().buffer_stops;
    CHECK_EQ_U32(stop_call(buffer,STOP_CALLER),0u);CHECK_EQ_U32(buffer_status(buffer),1u);
    position=buffer_position(buffer);CHECK_EQ_U32(position.position_samples,2000u);CHECK(position.playing && !position.looping && !position.paused);
    run(559u);CHECK_EQ_U32(buffer_status(buffer),1u);run(1u);CHECK_EQ_U32(buffer_status(buffer),0u);
    position=buffer_position(buffer);CHECK_EQ_U32(position.position_samples,2560u);CHECK(!position.playing);
    CHECK_EQ_U32(dsound_completion_get_stats().buffer_stops,before+1u);
    /* Play(0) ends the loop, then Stop is the ordinary one. */
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,3u,4u),0u);CHECK_EQ_U32(buffer_status(buffer),5u);
    position=buffer_position(buffer);CHECK_EQ_U32(position.position_samples,0u);
    run(1000u);
    /* Play restarts the loop at position zero; 2559 more ticks leave exactly one at 22042 Hz. */
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,3u,4u),0u);CHECK_EQ_U32(buffer_status(buffer),5u);
    position=buffer_position(buffer);CHECK_EQ_U32(position.position_samples,0u);
    run(2559u);CHECK_EQ_U32(buffer_status(buffer),5u);
    CHECK_EQ_U32(stop_call(buffer,STOP_CALLER),0u);CHECK_EQ_U32(buffer_status(buffer),1u);
    position=buffer_position(buffer);CHECK_EQ_U32(position.position_samples,2559u);
    run(1u);CHECK_EQ_U32(buffer_status(buffer),0u);
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,0u,4u),0u);
    CHECK_EQ_U32(call_frame(PAUSE,CALLER,buffer,1u,0u,0u,2u),0u);CHECK_EQ_U32(buffer_status(buffer),2u);
    CHECK_EQ_U32(stop_call(buffer,STOP_CALLER),0u);CHECK_EQ_U32(buffer_status(buffer),1u);
    CHECK_EQ_U32(dsound_completion_get_stats().buffer_stops,before+3u);
    /* Stop is RET 4: its frame is the return address and ONE argument, nothing past it is read (a tight frame has no second slot). */
    uint32_t tight_args[1]={buffer};kernel_call_frame tight={0};
    CHECK(kernel_frame_build(&tight,SCRATCH_DATA+0x200u,8u,tight_args,1u));store(tight.stack_ptr,STOP_CALLER);
    CHECK_EQ_U32(dsound_hle_call(0x407AA4u,&tight),0u);
    /* The call site is not part of the check (the original never reads its caller). */
    CHECK_EQ_U32(stop_call(buffer,0u),0u);CHECK_EQ_U32(stop_call(buffer,0x1234u),0u);CHECK_EQ_U32(stop_call(buffer,0x28698u),0u);
    /* Refusals: a buffer the adapter does not own, an ordinary buffer whose start is not recorded, the policy off. */
    RUN_EXPECTING_FATAL((void)stop_call(0x12345678u,STOP_CALLER));CHECK(fatal_seen);
    const uint32_t fresh=make_buffer(0x581974u,false);
    RUN_EXPECTING_FATAL((void)stop_call(fresh,STOP_CALLER));CHECK(fatal_seen);CHECK(strstr(fatal_text,"whole start")!=NULL);
    dsound_completion_set_enabled(false);
    RUN_EXPECTING_FATAL((void)stop_call(buffer,STOP_CALLER));CHECK(fatal_seen);
    dsound_completion_set_enabled(true);
}
/* T733: the buffer-method call sites are free under the model, the startup creators keep their measured callers, and Stop needs the start. */
static uint32_t make_spatial_buffer(uint32_t output,bool whole_start)
{
    uint8_t d[24]={0};const uint32_t words[6]={24u,16u,0u,format_at,0u,0u};memcpy(d,words,sizeof(d));
    CHECK(kernel_guest_write_bytes(desc_at,d,sizeof(d)));CHECK(kernel_guest_write_bytes(format_at,buffer_format,20u));
    CHECK_EQ_U32(dsound_buffer_create(device+8u,desc_at,output,0u),0u);
    const uint32_t buffer=load(output);
    const uint32_t params[9]={0u,0u,0xFFFFF448u,0u,0u,0u,0u,0u,0u};
    CHECK(kernel_guest_write_bytes(SCRATCH_DATA+0x500u,params,sizeof(params)));
    CHECK_EQ_U32(dsound_buffer_cache_i3dl2(buffer,SCRATCH_DATA+0x500u,0u),0u);
    CHECK_EQ_U32(dsound_buffer_cache_min_distance(buffer,0x3F800000u,0u),0u);
    CHECK_EQ_U32(dsound_buffer_cache_rolloff(buffer,0x64BC98u,1u,0u),0u);
    if(whole_start){
        CHECK_EQ_U32(dsound_buffer_set_data(buffer,DATA_BASE,1440u),0u);CHECK_EQ_U32(dsound_buffer_set_volume(buffer,-10000),0u);
        CHECK_EQ_U32(dsound_buffer_pause(buffer,0u),0u);CHECK_EQ_U32(dsound_buffer_set_frequency(buffer,22042u),0u);
    }
    return buffer;
}
static void test_buffer_stop_scope_and_callers(void)
{
    dsound_completion_reset();fake_now=500u;
    /* Stop needs the same whole start as Play: data, volume, Pause(0) and frequency, each missing step refused. */
    uint32_t partial[4];
    for(unsigned i=0u;i<4u;i++){
        partial[i]=make_buffer(0x581920u+4u*i,false);
        if(i>=1u)CHECK_EQ_U32(dsound_buffer_set_data(partial[i],DATA_BASE,1440u),0u);
        if(i>=2u)CHECK_EQ_U32(dsound_buffer_set_volume(partial[i],-10000),0u);
        if(i>=3u)CHECK_EQ_U32(dsound_buffer_pause(partial[i],0u),0u);
        RUN_EXPECTING_FATAL((void)call_frame(0x407AA4u,0x27468u,partial[i],0u,0u,0u,1u));CHECK(fatal_seen);
    }
    CHECK_EQ_U32(dsound_completion_get_stats().buffer_stops,0u);
    /* T1185: a spatial buffer (flags 0x10) WITHOUT its whole start is still refused, WITH it Play and Stop answer as the ordinary buffer
     * (measured on the original: S_OK, status 1 after Play(0), 5 after Play(1), Stop leaves 1). */
    const uint32_t spatial_partial=make_spatial_buffer(0x5835F4u,false);
    RUN_EXPECTING_FATAL((void)call_frame(0x407A80u,0x28698u,spatial_partial,0u,0u,0u,4u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)call_frame(0x407AA4u,0x27468u,spatial_partial,0u,0u,0u,1u));CHECK(fatal_seen);
    const uint32_t spatial=make_spatial_buffer(0x5835F0u,true);
    store(words_at,0xA5A5A5A5u);CHECK_EQ_U32(dsound_completion_buffer_status(spatial,words_at),0u);CHECK_EQ_U32(load(words_at),0u);
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,spatial,0u,0u,0u,4u),0u);
    CHECK_EQ_U32(dsound_completion_buffer_status(spatial,words_at),0u);CHECK_EQ_U32(load(words_at),1u);
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,spatial,0u,0u,0u,4u),0u);
    CHECK_EQ_U32(call_frame(0x407AA4u,0x27468u,spatial,0u,0u,0u,1u),0u);
    CHECK_EQ_U32(dsound_completion_get_stats().buffer_stops,1u);
    /* The creators and the three startup caches keep their measured callers with the model on (only the four buffer methods are free). */
    uint8_t d[24]={0};const uint32_t words[6]={24u,0u,0u,format_at,0u,0u};memcpy(d,words,sizeof(d));
    CHECK(kernel_guest_write_bytes(desc_at,d,sizeof(d)));CHECK(kernel_guest_write_bytes(format_at,buffer_format,20u));
    RUN_EXPECTING_FATAL((void)call_frame(0x4093C8u,0x1234u,device+8u,desc_at,0x581934u,0u,4u));CHECK(fatal_seen);
    CHECK(strstr(fatal_text,"only measured startup caller")!=NULL);
    CHECK_EQ_U32(call_frame(0x4093C8u,0x27B54u,device+8u,desc_at,0x581934u,0u,4u),0u);
    uint8_t spatial_descriptor[24]={0};const uint32_t spatial_words[6]={24u,16u,0u,format_at,0u,0u};memcpy(spatial_descriptor,spatial_words,sizeof(spatial_descriptor));
    CHECK(kernel_guest_write_bytes(desc_at,spatial_descriptor,sizeof(spatial_descriptor)));
    CHECK_EQ_U32(call_frame(0x4093C8u,0x27AA9u,device+8u,desc_at,0x583600u,0u,4u),0u);
    const uint32_t second=load(0x583600u);
    const uint32_t params[9]={0u,0u,0xFFFFF448u,0u,0u,0u,0u,0u,0u};
    CHECK(kernel_guest_write_bytes(SCRATCH_DATA+0x500u,params,sizeof(params)));
    RUN_EXPECTING_FATAL((void)call_frame(0x4085AFu,0x1234u,second,SCRATCH_DATA+0x500u,0u,0u,3u));CHECK(fatal_seen);
    CHECK_EQ_U32(call_frame(0x4085AFu,0x27AE8u,second,SCRATCH_DATA+0x500u,0u,0u,3u),0u);
    RUN_EXPECTING_FATAL((void)call_frame(0x408532u,0x1234u,second,0x3F800000u,0u,0u,3u));CHECK(fatal_seen);
    CHECK_EQ_U32(call_frame(0x408532u,0x27AF6u,second,0x3F800000u,0u,0u,3u),0u);
    RUN_EXPECTING_FATAL((void)call_frame(0x40858Bu,0x1234u,second,0x64BC98u,1u,0u,4u));CHECK(fatal_seen);
    CHECK_EQ_U32(call_frame(0x40858Bu,0x27B06u,second,0x64BC98u,1u,0u,4u),0u);
}
/* T733: `started` is keyed by the buffer address AND its lease serial, a recycled address is not a played buffer. */
static void test_started_by_serial(void)
{
    dsound_completion_reset();fake_now=500u;
    const uint32_t buffer=make_buffer(0x581980u,true);
    dsound_buffer_snapshot snap;CHECK(dsound_buffer_get_snapshot(buffer,&snap));
    CHECK(!dsound_completion_buffer_started(buffer,snap.lease.serial));
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,buffer,0u,0u,0u,4u),0u);
    CHECK(dsound_completion_buffer_started(buffer,snap.lease.serial));
    CHECK(!dsound_completion_buffer_started(buffer,snap.lease.serial+1u));
    CHECK(!dsound_completion_buffer_started(buffer,UINT64_MAX));
    CHECK(!dsound_completion_buffer_started(buffer+4u,snap.lease.serial));
    CHECK(!dsound_completion_buffer_started(0u,snap.lease.serial));
}
static void test_rounding_and_scopes(void)
{
    dsound_completion_reset();fake_now=0u;
    /* 1000 ticks a second: 288 bytes at 49612 bytes/s need 5.80 ticks, rounded UP to 6 so a packet never completes early. */
    dsound_completion_set_clock(fake_clock,1000u);
    const uint32_t stream=make_stream(SCRATCH_DATA+0x718u);
    CHECK_EQ_U32(dsound_stream_cache_pause(stream,0u),0u);
    CHECK_EQ_U32(process(stream,288u,0u,true),0u);
    run(5u);CHECK_EQ_U32(status_of(stream),0x10001u);
    run(1u);CHECK_EQ_U32(status_of(stream),1u);
    /* T1185: GetInfo answers the spatial mono stream (block 36) as the original does, {5, 36, 0, 72}. */
    uint8_t spatial_format[20];memcpy(spatial_format,stereo_startup,20u);
    spatial_format[2]=1u;spatial_format[8]=0xE6u;spatial_format[9]=0x60u;spatial_format[12]=36u;
    const uint32_t desc[6]={16u,3u,format_at,0u,0u,0u};
    CHECK(kernel_guest_write_bytes(desc_at,desc,sizeof(desc)));CHECK(kernel_guest_write_bytes(format_at,spatial_format,20u));
    CHECK_EQ_U32(dsound_stream_create(desc_at,SCRATCH_DATA+0x71Cu),0u);
    const uint32_t spatial=load(SCRATCH_DATA+0x71Cu);
    const uint32_t params[9]={0u,0u,0xFFFFF448u,0u,0u,0u,0u,0u,0u};
    CHECK(kernel_guest_write_bytes(SCRATCH_DATA+0x500u,params,sizeof(params)));
    CHECK_EQ_U32(dsound_stream_cache_i3dl2(spatial,SCRATCH_DATA+0x500u,0u),0u);
    CHECK_EQ_U32(dsound_stream_cache_min_distance(spatial,0x3F800000u,0u),0u);
    CHECK_EQ_U32(dsound_stream_cache_rolloff(spatial,0x4B914Cu,4u,0u),0u);
    CHECK_EQ_U32(dsound_stream_cache_volume(spatial,-10000),0u);CHECK_EQ_U32(dsound_stream_cache_pause(spatial,1u),0u);
    CHECK_EQ_U32(dsound_stream_cache_flush_ex(spatial,0u,0u,1u),0u);CHECK_EQ_U32(dsound_stream_cache_discontinuity(spatial),0u);
    store(words_at,0u);store(words_at+12u,0u);CHECK_EQ_U32(dsound_completion_stream_info(spatial,words_at),0u);
    CHECK_EQ_U32(load(words_at),5u);CHECK_EQ_U32(load(words_at+4u),36u);CHECK_EQ_U32(load(words_at+8u),0u);CHECK_EQ_U32(load(words_at+12u),72u);
    /* A Play of a buffer whose start is partial is refused at each missing step. */
    dsound_completion_set_clock(fake_clock,22042u);
    uint32_t buffers[4];
    for(unsigned i=0u;i<4u;i++){
        buffers[i]=make_buffer(0x581900u+4u*i,false);
        if(i>=1u)CHECK_EQ_U32(dsound_buffer_set_data(buffers[i],DATA_BASE,1440u),0u);
        if(i>=2u)CHECK_EQ_U32(dsound_buffer_set_volume(buffers[i],-10000),0u);
        if(i>=3u)CHECK_EQ_U32(dsound_buffer_pause(buffers[i],0u),0u);
        RUN_EXPECTING_FATAL((void)dsound_completion_buffer_play(buffers[i],0u));CHECK(fatal_seen);
    }
    CHECK_EQ_U32(dsound_completion_get_stats().buffer_plays,0u);
    /* Data shorter than one block cannot be played. */
    const uint32_t tiny=make_buffer(0x581910u,false);
    CHECK_EQ_U32(dsound_buffer_set_data(tiny,DATA_BASE,35u),0u);CHECK_EQ_U32(dsound_buffer_set_volume(tiny,-10000),0u);
    CHECK_EQ_U32(dsound_buffer_pause(tiny,0u),0u);CHECK_EQ_U32(dsound_buffer_set_frequency(tiny,22042u),0u);
    RUN_EXPECTING_FATAL((void)dsound_completion_buffer_play(tiny,0u));CHECK(fatal_seen);
    dsound_completion_set_clock(fake_clock,49612u);
}
static void test_values_not_lists(void)
{
    dsound_completion_reset();
    const uint32_t buffer=make_buffer(0x5818F0u,true);
    /* The sound update's volume is a value: -3204 and every other one is recorded (caller 0x28643). */
    const int32_t volumes[]={-3204,-1,0,-9999,-777,1};
    for(unsigned i=0u;i<sizeof(volumes)/sizeof(volumes[0]);i++){
        CHECK_EQ_U32(call_frame(0x407A64u,0x28643u,buffer,(uint32_t)volumes[i],0u,0u,2u),0u);
        dsound_buffer_snapshot snap;CHECK(dsound_buffer_get_snapshot(buffer,&snap));CHECK_EQ_U32((uint32_t)snap.volume,(uint32_t)volumes[i]);
    }
    /* T722: the sound start (caller 0x28348) records every volume of a later sound too (measured 0 for the second sound),
     * after SetBufferData, and the whole start then plays. Before SetBufferData it is still refused. */
    const uint32_t second=make_buffer(0x581930u,false);
    RUN_EXPECTING_FATAL((void)call_frame(0x407A64u,0x28348u,second,0u,0u,0u,2u));CHECK(fatal_seen);
    CHECK_EQ_U32(dsound_buffer_set_data(second,DATA_BASE,1440u),0u);
    const int32_t start_volumes[]={0,-777,-10000,1,-1};
    for(unsigned i=0u;i<sizeof(start_volumes)/sizeof(start_volumes[0]);i++){
        CHECK_EQ_U32(call_frame(0x407A64u,0x28348u,second,(uint32_t)start_volumes[i],0u,0u,2u),0u);
        dsound_buffer_snapshot snap;CHECK(dsound_buffer_get_snapshot(second,&snap));CHECK_EQ_U32((uint32_t)snap.volume,(uint32_t)start_volumes[i]);
        CHECK_EQ_U32(snap.volume_sets,i+1u);
    }
    CHECK_EQ_U32(call_frame(0x407A64u,0x28348u,second,0u,0u,0u,2u),0u);
    /* T733: with the model on the call site is not part of the check, the state is: a buffer whose SetBufferData is recorded takes any
     * volume from any caller (the original never reads it), one without is still refused. */
    const uint32_t half=make_buffer(0x581960u,false);
    RUN_EXPECTING_FATAL((void)call_frame(0x407A64u,0x28643u,half,0u,0u,0u,2u));CHECK(fatal_seen);
    CHECK(strstr(fatal_text,"before SetBufferData")!=NULL);
    CHECK_EQ_U32(dsound_buffer_set_data(half,DATA_BASE,1440u),0u);
    const uint32_t odd_callers[]={0x28643u,0x28348u,0x1234u,0u};
    for(unsigned i=0u;i<sizeof(odd_callers)/sizeof(odd_callers[0]);i++){
        CHECK_EQ_U32(call_frame(0x407A64u,odd_callers[i],half,(uint32_t)-(int32_t)(100+i),0u,0u,2u),0u);
        dsound_buffer_snapshot snap;CHECK(dsound_buffer_get_snapshot(half,&snap));CHECK_EQ_U32((uint32_t)snap.volume,(uint32_t)-(int32_t)(100+i));
    }
    /* SetBufferData and SetFrequency of any caller too, Pause(0) after the volume. */
    CHECK_EQ_U32(call_frame(0x408C0Du,0x1234u,half,DATA_BASE,1440u,0u,3u),0u);
    CHECK_EQ_U32(call_frame(0x407ABCu,0x1234u,half,0u,0u,0u,2u),0u);CHECK_EQ_U32(call_frame(0x4084F2u,0x1234u,half,30000u,0u,0u,2u),0u);
    CHECK_EQ_U32(dsound_buffer_pause(second,0u),0u);CHECK_EQ_U32(dsound_buffer_set_frequency(second,22042u),0u);
    CHECK_EQ_U32(call_frame(0x407A80u,0x28698u,second,0u,0u,2u,4u),0u);CHECK_EQ_U32(buffer_status(second),1u);
    /* The refusal names the caller and volume. */
    dsound_buffer_set_completion(false,NULL);
    RUN_EXPECTING_FATAL((void)call_frame(0x407A64u,0x28348u,second,(uint32_t)-5,0u,0u,2u));
    CHECK(strstr(fatal_text,"return address 0x28348 args (")!=NULL);CHECK(strstr(fatal_text,", -5)")!=NULL);
    dsound_buffer_set_completion(true,dsound_completion_buffer_started);dsound_buffer_set_completion_pause(dsound_completion_buffer_pause);dsound_buffer_set_completion_frequency(dsound_completion_buffer_frequency);
    /* Any frequency in the title's clamp, nothing outside it. */
    const uint32_t fresh=make_buffer(0x5818F4u,false);
    CHECK_EQ_U32(dsound_buffer_set_data(fresh,DATA_BASE,1440u),0u);CHECK_EQ_U32(dsound_buffer_set_volume(fresh,-10000),0u);
    CHECK_EQ_U32(dsound_buffer_pause(fresh,0u),0u);
    RUN_EXPECTING_FATAL((void)dsound_buffer_set_frequency(fresh,0xBBu));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_buffer_set_frequency(fresh,0x2EDF0u));CHECK(fatal_seen);
    CHECK_EQ_U32(dsound_buffer_set_frequency(fresh,0xBCu),0u);CHECK_EQ_U32(dsound_buffer_set_frequency(fresh,0x2EDEFu),0u);
    /* The stream update takes -1137 and every other value after the start, from caller 0x29F68. */
    const uint32_t stream=make_stream(SCRATCH_DATA+0x70Cu);
    const int32_t stream_volumes[]={-1137,-1,0,-10000,-5000};
    for(unsigned i=0u;i<sizeof(stream_volumes)/sizeof(stream_volumes[0]);i++){
        CHECK_EQ_U32(call_frame(0x407B14u,0x29F68u,stream,(uint32_t)stream_volumes[i],0u,0u,2u),0u);
        dsound_stream_snapshot snap;CHECK(dsound_stream_get_snapshot(stream,&snap));CHECK_EQ_U32((uint32_t)snap.volume,(uint32_t)stream_volumes[i]);
        /* The completion status takes the recorded volume (the startup status rule demanded -10000). */
        CHECK_EQ_U32(status_of(stream),1u);
    }
    /* A stream that never started keeps the startup silence rule even with the model on. */
    const uint32_t desc[6]={0u,3u,format_at,0u,0u,0u};
    CHECK(kernel_guest_write_bytes(desc_at,desc,sizeof(desc)));CHECK(kernel_guest_write_bytes(format_at,stereo_startup,20u));
    CHECK_EQ_U32(dsound_stream_create(desc_at,SCRATCH_DATA+0x710u),0u);
    RUN_EXPECTING_FATAL((void)call_frame(0x407B14u,0x29F68u,load(SCRATCH_DATA+0x710u),(uint32_t)-1137,0u,0u,2u));CHECK(fatal_seen);
    /* A stream whose startup is only partly recorded (the startup volume, no Pause or Discontinuity yet) keeps the startup rule. */
    CHECK_EQ_U32(dsound_stream_create(desc_at,SCRATCH_DATA+0x720u),0u);
    const uint32_t partial=load(SCRATCH_DATA+0x720u);
    CHECK_EQ_U32(dsound_stream_cache_volume(partial,-10000),0u);
    RUN_EXPECTING_FATAL((void)call_frame(0x407B14u,0x29F68u,partial,(uint32_t)-1137,0u,0u,2u));CHECK(fatal_seen);
    /* Pause mode 2 and other modes stay refused. */
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_pause(stream,2u));CHECK(fatal_seen);
    /* Pause 0 on a never started stream stays refused. */
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_pause(load(SCRATCH_DATA+0x710u),0u));CHECK(fatal_seen);
}
static void test_inert_while_off(void)
{
    /* Default off: the same calls the model admits are refused, and no row is registered. */
    dsound_completion_reset();dsound_completion_set_enabled(false);
    dsound_stream_set_completion(false,NULL);dsound_buffer_set_completion(false,NULL);
    const uint32_t stream=make_stream(SCRATCH_DATA+0x714u);
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_volume(stream,-1137));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_pause(stream,0u));CHECK(fatal_seen);
    const uint32_t buffer=make_buffer(0x5818F8u,true);
    RUN_EXPECTING_FATAL((void)dsound_buffer_set_volume(buffer,-777));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_buffer_set_frequency(buffer,30000u));CHECK(fatal_seen);
    CHECK_EQ_U32(dsound_completion_register(),0u);
    dsound_completion_set_enabled(true);
    dsound_stream_set_completion(true,dsound_completion_note_pause);dsound_buffer_set_completion(true,dsound_completion_buffer_started);dsound_buffer_set_completion_pause(dsound_completion_buffer_pause);dsound_buffer_set_completion_frequency(dsound_completion_buffer_frequency);
}
int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();
    dsound_device_set_fatal(catching_fatal);dsound_stream_set_fatal(catching_fatal);dsound_buffer_set_fatal(catching_fatal);
    dsound_completion_set_fatal(catching_fatal);
    dsound_stream_set_irql_provider(current_irql);dsound_buffer_set_irql_provider(current_irql);
    map_fixed(0x412000u,4096u);map_fixed(0x4A1000u,4096u);map_fixed(0x4B9000u,4096u);map_fixed(0x581000u,4096u);map_fixed(0x583000u,4096u);
    map_fixed(DATA_BASE,0x10000u);
    for(unsigned i=0u;i<15u;i++)store(0x4A1CF0u+4u*i,0x406879u);
    for(unsigned i=0u;i<4u;i++)store(0x4A1CE0u+4u*i,0x406879u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);
    device=load(SCRATCH_DATA)-8u;desc_at=SCRATCH_DATA+0x100u;format_at=SCRATCH_DATA+0x140u;packet_at=SCRATCH_DATA+0x300u;
    words_at=SCRATCH_DATA+0x400u;out_at=SCRATCH_DATA+0x600u;
    dsound_stream_set_enabled(true);dsound_buffer_set_enabled(true);
    (void)dsound_stream_register();(void)dsound_buffer_register();
    /* The clock: 49612 ticks a second so one tick is one byte of the 49612 byte/s startup stream, and the buffer
     * duration sample count is then compared at the same rate by choosing the frequency 22042 below. */
    dsound_completion_set_clock(fake_clock,49612u);
    dsound_completion_set_enabled(true);
    dsound_stream_set_completion(true,dsound_completion_note_pause);dsound_buffer_set_completion(true,dsound_completion_buffer_started);dsound_buffer_set_completion_pause(dsound_completion_buffer_pause);dsound_buffer_set_completion_frequency(dsound_completion_buffer_frequency);
    CHECK_EQ_U32(dsound_completion_register(),3u);
    CHECK(dsound_hle_entry(0x407AA4u)->state==DSOUND_ENTRY_IMPLEMENTED);
    CHECK(dsound_hle_entry(0x407A80u)->state==DSOUND_ENTRY_IMPLEMENTED);
    CHECK(dsound_hle_entry(0x407AF8u)->state==DSOUND_ENTRY_IMPLEMENTED);
    test_stream_frequency_cumulative();
    test_stream_frequency_live_queue();
    test_stream_frequency_transaction();
    test_stream_frequency_paused();
    test_stream_frequency_owned_route();
    test_stream_format_abort_pcm_rejection();
    test_stream_timeline();
    test_dowork_delivery();
    test_stream_refusals();
    dsound_completion_set_clock(fake_clock,22042u);
    test_buffer_play();test_buffer_pause();test_buffer_set_frequency_played();test_buffer_stop();test_buffer_stop_scope_and_callers();test_started_by_serial();
    test_values_not_lists();
    test_rounding_and_scopes();
    test_inert_while_off();
    (void)out_at;
    CHECK(dsound_stream_reset_checked());CHECK(dsound_buffer_reset_checked());CHECK(dsound_device_reset_checked());
    environment_end();
    return failures?1:0;
}
