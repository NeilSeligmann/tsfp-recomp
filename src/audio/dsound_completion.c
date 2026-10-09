/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsound_completion.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsound_buffer.h"
#include "dsound_audio_runtime.h"
#include "dsound_hle.h"
#include "dsound_stream.h"
#include "kernel_call.h"
#include "kernel_clock.h"
#include "dsound_stream_frequency.h"
#define STREAM_GET_STATUS 0x004073D3u
#define STREAM_PROCESS 0x00407424u
#define STREAM_GET_INFO 0x004072D4u
#define BUFFER_PLAY 0x00407A80u
#define BUFFER_GET_STATUS 0x00407AF8u
#define BUFFER_STOP 0x00407AA4u
#define PENDING_STATUS 0x8000000Au
#define PACKET_LIST_FULL 0x88780032u
#define STREAM_STATES 32u
#define BUFFER_STATES 64u
#define MAX_QUEUE 8u
#define BUFFER_BLOCK 36u
#define BUFFER_BLOCK_SAMPLES 64u
typedef struct packet {
    uint32_t size,completed,status;
    unsigned __int128 remaining_q32;
} packet;
typedef struct stream_state {
    uint32_t address;
    uint64_t serial,last;
    bool used,running;
    uint32_t queued,done;  /* done: the first `done` packets passed their deadline and wait for DoWork (T855 mode only) */
    dsound_frequency_clock frequency_clock;
    dsound_frequency_phase source_phase;
    uint64_t active_ticks;
    uint32_t frequency_base_hz,frequency_current_hz;
    packet queue[MAX_QUEUE];
} stream_state;
typedef struct buffer_state {
    uint32_t address;
    uint64_t serial,last,remaining,duration,position,samples,loop_start,loop_end,loop_start_samples,loop_end_samples;
    bool used,playing,looping,paused,stopped;
} buffer_state;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static bool enabled,dowork_delivery,announced_stream,announced_buffer,announced_stop,announced_audio_skip;
static dsound_completion_clock_fn clock_fn;
static uint64_t clock_frequency=KERNEL_CLOCK_FREQUENCY_HZ;
static dsound_completion_fatal_fn fatal_handler;
static stream_state streams[STREAM_STATES];
static buffer_state buffers[BUFFER_STATES];
static dsound_completion_stats stats;
static stream_state *stream_find(uint32_t address,uint64_t serial,bool create);
static bool word_ok(uint32_t address,uint32_t stream_address);
static dsound_completion_census_entry census[DSOUND_COMPLETION_CENSUS_MAX];
static size_t census_count;
static void refuse(uint32_t entry,const char *reason) __attribute__((noreturn));
static void refuse(uint32_t entry,const char *reason)
{
    pthread_mutex_lock(&lock);dsound_completion_fatal_fn fatal=fatal_handler;pthread_mutex_unlock(&lock);
    dsound_hle_log()("dsound passive completion %#x refused: %s\n",entry,reason);
    if(fatal!=NULL)fatal(entry,reason);abort();
}
void dsound_completion_set_enabled(bool value)
{
    pthread_mutex_lock(&lock);enabled=value;pthread_mutex_unlock(&lock);
    /* Quiescent configuration: release completion lock before taking buffer
     * lock. Runtime hooks take the opposite buffer -> completion -> PCM order. */
    dsound_buffer_set_completion_volume(value?dsound_completion_buffer_volume:NULL);
}
void dsound_completion_set_dowork_delivery(bool value){pthread_mutex_lock(&lock);dowork_delivery=value;pthread_mutex_unlock(&lock);}
bool dsound_completion_dowork_delivery(void){pthread_mutex_lock(&lock);bool v=dowork_delivery;pthread_mutex_unlock(&lock);return v;}
bool dsound_completion_enabled(void){pthread_mutex_lock(&lock);bool v=enabled;pthread_mutex_unlock(&lock);return v;}
void dsound_completion_set_clock(dsound_completion_clock_fn clock,uint64_t frequency)
{
    pthread_mutex_lock(&lock);clock_fn=clock;clock_frequency=clock!=NULL?frequency:KERNEL_CLOCK_FREQUENCY_HZ;
    pthread_mutex_unlock(&lock);
}
void dsound_completion_set_fatal(dsound_completion_fatal_fn fatal){pthread_mutex_lock(&lock);fatal_handler=fatal;pthread_mutex_unlock(&lock);}
void dsound_completion_reset(void)
{
    pthread_mutex_lock(&lock);memset(streams,0,sizeof(streams));memset(buffers,0,sizeof(buffers));
    dsound_audio_runtime_reset_buffers();
    memset(&stats,0,sizeof(stats));memset(census,0,sizeof(census));census_count=0u;announced_stream=announced_buffer=announced_stop=announced_audio_skip=false;pthread_mutex_unlock(&lock);
}
size_t dsound_completion_stream_census(dsound_completion_census_entry *out,size_t capacity)
{
    pthread_mutex_lock(&lock);
    const size_t count=census_count<capacity?census_count:capacity;
    if(out!=NULL)memcpy(out,census,count*sizeof(*out));
    pthread_mutex_unlock(&lock);
    return count;
}
static uint32_t le32(const uint8_t *p);
static uint32_t le16(const uint8_t *p);
/* Caller holds the lock. */
static uint64_t now_ticks(void);
static void census_note(const uint8_t *format,uint32_t bytes,bool mixed)
{
    const uint64_t now_ms=now_ticks()/(clock_frequency/1000u?clock_frequency/1000u:1u);
    const uint32_t tag=le16(format),channels=le16(format+2u),rate=le32(format+4u),block=le16(format+12u),bits=le16(format+14u);
    size_t i=0u;
    for(;i<census_count;i++)
        if(census[i].tag==tag&&census[i].channels==channels&&census[i].rate==rate&&census[i].block==block&&census[i].bits==bits&&
           (census[i].mixed!=0u)==mixed)break;
    if(i==census_count){
        if(census_count==DSOUND_COMPLETION_CENSUS_MAX)i=DSOUND_COMPLETION_CENSUS_MAX-1u;
        else{census[i]=(dsound_completion_census_entry){tag,channels,rate,block,bits,0u,0u,0u,now_ms,now_ms};census_count++;}
    }
    census[i].packets++;census[i].bytes+=bytes;census[i].last_ms=now_ms;if(mixed)census[i].mixed=1u;
}
dsound_completion_stats dsound_completion_get_stats(void)
{pthread_mutex_lock(&lock);dsound_completion_stats v=stats;pthread_mutex_unlock(&lock);return v;}
static uint64_t now_ticks(void){return clock_fn!=NULL?clock_fn():kernel_clock_peek();}
bool dsound_completion_stream_routing(uint32_t stream,uint64_t serial,
                                       const dsound_stream_routing *routing)
{
    pthread_mutex_lock(&lock);
    const bool accepted=enabled && clock_frequency==dsound_audio_runtime_clock_frequency() &&
        dsound_audio_runtime_route_stream(stream,serial,now_ticks(),routing);
    pthread_mutex_unlock(&lock);
    return accepted;
}
/* Ticks of running time for `units` at `per_second` units a second, rounded up so a packet never completes early. */
static uint64_t ticks_for(uint64_t units,uint64_t per_second)
{return per_second==0u?UINT64_MAX:(units*clock_frequency+per_second-1u)/per_second;}
/* Called with the lock held. */
static stream_state *stream_find(uint32_t address,uint64_t serial,bool create)
{
    stream_state *free_slot=NULL;
    for(unsigned i=0u;i<STREAM_STATES;i++) {
        if(streams[i].used && streams[i].address==address) {
            if(streams[i].serial==serial)return &streams[i];
            memset(&streams[i],0,sizeof(streams[i]));free_slot=&streams[i];break;
        }
        if(!streams[i].used && free_slot==NULL)free_slot=&streams[i];
    }
    if(!create || free_slot==NULL)return NULL;
    memset(free_slot,0,sizeof(*free_slot));
    free_slot->used=true;free_slot->address=address;free_slot->serial=serial;free_slot->last=now_ticks();
    return free_slot;
}
static buffer_state *buffer_find(uint32_t address,uint64_t serial,bool create)
{
    buffer_state *free_slot=NULL;
    for(unsigned i=0u;i<BUFFER_STATES;i++) {
        if(buffers[i].used && buffers[i].address==address) {
            if(buffers[i].serial==serial)return &buffers[i];
            memset(&buffers[i],0,sizeof(buffers[i]));free_slot=&buffers[i];break;
        }
        if(!buffers[i].used && free_slot==NULL)free_slot=&buffers[i];
    }
    if(!create || free_slot==NULL)return NULL;
    memset(free_slot,0,sizeof(*free_slot));
    free_slot->used=true;free_slot->address=address;free_slot->serial=serial;free_slot->last=now_ticks();
    return free_slot;
}
/* Completion words are written when the packet is delivered (status 0, completed size). A write that fails means the title
 * freed them, which the original would equally fault on, so it is not hidden. Lock held. */
static void packet_deliver(stream_state *s)
{
    const packet head=s->queue[0];
    if(head.completed!=0u)(void)kernel_guest_write_u32(head.completed,head.size);
    if(head.status!=0u)(void)kernel_guest_write_u32(head.status,0u);
    stats.stream_completed++;
    for(uint32_t i=1u;i<s->queued;i++)s->queue[i-1u]=s->queue[i];
    s->queued--;memset(&s->queue[s->queued],0,sizeof(s->queue[0]));
}
/* Spends the running time since the last observation on the head packets. Lock held.
 * T855 (opt-in, INFERRED): with DoWork delivery a packet that passes its deadline stays queued with its words untouched (the
 * original recycles the record only inside DoWork, event 2 handler 0x40B98A) and the next packet starts counting at once. */
/* Pure candidate advance: completion writes and statistics happen only after
 * the candidate and optional PCM rate event have both been accepted. */
static bool stream_candidate_advance(stream_state *s,uint64_t now)
{
    const uint64_t elapsed=now>s->last?now-s->last:0u;
    s->last=now;
    if(!s->running)return true;
    if(elapsed>UINT64_MAX-s->active_ticks)return false;
    s->active_ticks+=elapsed;
    dsound_frequency_phase phase={s->active_ticks,0u};
    if(s->frequency_clock.count!=0u &&
       !dsound_stream_frequency_clock_phase(&s->frequency_clock,s->active_ticks,&phase))return false;
    const unsigned __int128 previous=((unsigned __int128)s->source_phase.ticks<<32u)|s->source_phase.fraction;
    const unsigned __int128 current=((unsigned __int128)phase.ticks<<32u)|phase.fraction;
    if(current<previous)return false;
    unsigned __int128 available=current-previous;
    s->source_phase=phase;
    while(s->done<s->queued && available!=0u) {
        packet *head=&s->queue[s->done];
        const unsigned __int128 take=available<head->remaining_q32?available:head->remaining_q32;
        head->remaining_q32-=take;available-=take;
        if(head->remaining_q32!=0u)break;
        s->done++;
    }
    return true;
}
static void stream_publish_advance(stream_state *state,const stream_state *candidate)
{
    *state=*candidate;stats.observations++;
    if(!dowork_delivery)while(state->done!=0u){packet_deliver(state);state->done--;}
}
static void stream_advance(stream_state *s,uint64_t now)
{
    stream_state candidate=*s;
    if(!stream_candidate_advance(&candidate,now)) {
        /* Overflow is outside the configured finite clock contract. */
        dsound_hle_log()("dsound: completion source clock overflow\n");abort();
    }
    stream_publish_advance(s,&candidate);
}
static bool stream_frequency_change(uint32_t stream,uint64_t serial,uint64_t ticks,
                                    uint32_t old_rate_hz,uint32_t new_rate_hz,bool pcm)
{
    if(stream==0u || serial==0u || old_rate_hz<8000u || old_rate_hz>96000u ||
       new_rate_hz<8000u || new_rate_hz>96000u)return false;
    pthread_mutex_lock(&lock);
    stream_state *state=NULL;
    for(unsigned i=0u;i<STREAM_STATES;i++)
        if(streams[i].used && streams[i].address==stream && (streams[i].serial==serial || streams[i].serial==UINT64_MAX))state=&streams[i];
    bool accepted=enabled && state!=NULL && ticks==now_ticks() && ticks>=state->last &&
        (state->frequency_current_hz==0u || state->frequency_current_hz==old_rate_hz);
    stream_state candidate={0};
    if(accepted) {
        candidate=*state;candidate.serial=serial;
        accepted=stream_candidate_advance(&candidate,ticks);
        const uint32_t base=candidate.frequency_base_hz!=0u?candidate.frequency_base_hz:old_rate_hz;
        if(accepted && candidate.frequency_clock.count==0u)
            accepted=dsound_stream_frequency_clock_init(&candidate.frequency_clock,base);
        if(accepted)accepted=dsound_stream_frequency_clock_change(&candidate.frequency_clock,
            candidate.active_ticks,(uint64_t)new_rate_hz<<32u,candidate.active_ticks);
        candidate.frequency_base_hz=base;candidate.frequency_current_hz=new_rate_hz;
        if(accepted && pcm)accepted=clock_frequency==dsound_audio_runtime_clock_frequency() &&
            dsound_audio_runtime_frequency_stream(stream,serial,ticks,base,old_rate_hz,new_rate_hz);
        if(accepted)stream_publish_advance(state,&candidate);
    }
    pthread_mutex_unlock(&lock);return accepted;
}
bool dsound_completion_note_format(uint32_t stream,uint64_t serial,uint32_t old_hz,uint32_t new_hz,
    uint32_t format_address)
{
    if(stream==0u || serial==0u || old_hz<8000u || old_hz>96000u || new_hz<8000u || new_hz>48000u)return false;
    pthread_mutex_lock(&lock);
    stream_state *state=NULL;
    for(unsigned i=0u;i<STREAM_STATES;i++)
        if(streams[i].used && streams[i].address==stream &&
           (streams[i].serial==serial || streams[i].serial==UINT64_MAX))state=&streams[i];
    const uint64_t ticks=now_ticks();
    bool accepted=enabled && state!=NULL && ticks>=state->last &&
        (state->frequency_current_hz==0u || state->frequency_current_hz==old_hz) &&
        clock_frequency==dsound_audio_runtime_clock_frequency();
    stream_state candidate={0};
    if(accepted) {
        candidate=*state;candidate.serial=serial;
        const uint32_t base=candidate.frequency_base_hz!=0u?candidate.frequency_base_hz:old_hz;
        accepted=stream_candidate_advance(&candidate,ticks);
        if(accepted && candidate.frequency_clock.count==0u)
            accepted=dsound_stream_frequency_clock_init(&candidate.frequency_clock,base);
        if(accepted)accepted=dsound_stream_frequency_clock_change(&candidate.frequency_clock,
            candidate.active_ticks,(uint64_t)new_hz<<32u,candidate.active_ticks);
        candidate.frequency_base_hz=base;candidate.frequency_current_hz=new_hz;
        /* Original SetFormat aborts every admitted packet synchronously. The
         * input format must not alias words it flushes before reading it. */
        for(uint32_t i=0u;accepted && i<candidate.queued;i++) {
            const uint32_t destinations[2]={candidate.queue[i].completed,candidate.queue[i].status};
            for(unsigned j=0u;accepted && j<2u;j++) {
                const uint32_t address=destinations[j];
                accepted=(address==0u || !((uint64_t)address<(uint64_t)format_address+20u &&
                    (uint64_t)format_address<(uint64_t)address+4u)) && word_ok(address,stream);
            }
        }
        if(accepted)accepted=dsound_audio_runtime_format_stream(stream,serial,ticks,old_hz,new_hz);
        if(accepted) {
            for(uint32_t i=0u;i<candidate.queued;i++) {
                const packet *pending=&candidate.queue[i];
                if(pending->completed!=0u)(void)kernel_guest_write_u32(pending->completed,pending->size);
                if(pending->status!=0u)(void)kernel_guest_write_u32(pending->status,0x80004004u);
            }
            stats.stream_aborted+=candidate.queued;
            candidate.queued=0u;candidate.done=0u;memset(candidate.queue,0,sizeof(candidate.queue));
            stream_publish_advance(state,&candidate);
        }
    }
    if(!accepted)dsound_hle_log()("dsound: format transaction refused stream=%#x serial=%llu state=%p state-serial=%llu queued=%u old=%u current=%u new=%u now=%llu last=%llu clock=%llu pcm-clock=%llu\n",
        stream,(unsigned long long)serial,(void *)state,(unsigned long long)(state!=NULL?state->serial:0u),
        state!=NULL?state->queued:0u,old_hz,state!=NULL?state->frequency_current_hz:0u,new_hz,
        (unsigned long long)ticks,(unsigned long long)(state!=NULL?state->last:0u),
        (unsigned long long)clock_frequency,(unsigned long long)dsound_audio_runtime_clock_frequency());
    pthread_mutex_unlock(&lock);return accepted;
}

bool dsound_completion_stream_frequency(uint32_t stream,uint64_t serial,uint64_t ticks,
                                        uint32_t old_rate_hz,uint32_t new_rate_hz)
{return stream_frequency_change(stream,serial,ticks,old_rate_hz,new_rate_hz,false);}
bool dsound_completion_stream_frequency_pcm(uint32_t stream,uint64_t serial,uint64_t ticks,
                                            uint32_t old_rate_hz,uint32_t new_rate_hz)
{return stream_frequency_change(stream,serial,ticks,old_rate_hz,new_rate_hz,true);}
bool dsound_completion_note_frequency(uint32_t stream,uint64_t serial,uint32_t old_hz,uint32_t new_hz)
{return dsound_completion_stream_frequency_pcm(stream,serial,now_ticks(),old_hz,new_hz);}
/* DoWork delivery: the finished packets complete, in order. Lock held. */
static void stream_deliver_done(stream_state *s)
{
    while(s->done!=0u){packet_deliver(s);s->done--;}
}
static void buffer_advance(buffer_state *b,uint64_t now)
{
    uint64_t elapsed=now>b->last?now-b->last:0u;b->last=now;
    stats.observations++;
    if(!b->playing || b->paused)return;
    if(b->looping) {
        /* INFERRED: play the prefix once, then wrap only the selected loop interval.
         * Absolute data position lets Stop drain the full remaining data tail. */
        if(b->loop_start==0u && b->loop_end==b->duration && b->duration!=0u) {
            /* Preserve the default full-data loop contract and its mutation seam. */
            const uint64_t step=elapsed%b->duration;
            const uint64_t to_wrap=b->duration-b->position;
            b->position=step>=to_wrap?step-to_wrap:b->position+step;
        } else if(b->loop_end>b->loop_start) {
            const uint64_t to_wrap=b->loop_end-b->position;
            if(elapsed<to_wrap)b->position+=elapsed;
            else b->position=b->loop_start+(elapsed-to_wrap)%(b->loop_end-b->loop_start);
        }
        return;
    }
    if(elapsed>=b->remaining){b->remaining=0u;b->position=b->duration;b->playing=false;stats.buffer_finished++;}
    else {b->remaining-=elapsed;b->position+=elapsed;}
}
void dsound_completion_note_pause(uint32_t stream_address,uint32_t mode)
{
    const uint64_t tick=now_ticks();
    pthread_mutex_lock(&lock);
    if(!enabled){pthread_mutex_unlock(&lock);return;}
    /* The stream lease is not known here, so adopt whatever serial the entry has. The state is keyed again by the
     * lease at the next method call, and a stale entry only costs one observation. */
    stream_state *s=NULL;
    for(unsigned i=0u;i<STREAM_STATES;i++)if(streams[i].used && streams[i].address==stream_address)s=&streams[i];
    if(s==NULL){
        for(unsigned i=0u;i<STREAM_STATES && s==NULL;i++)if(!streams[i].used)s=&streams[i];
        if(s!=NULL){memset(s,0,sizeof(*s));s->used=true;s->address=stream_address;s->serial=UINT64_MAX;s->last=tick;}
    }
    if(s!=NULL){stream_advance(s,tick);s->running=(mode==0u);}
    const bool audio_enabled=dsound_audio_runtime_active();
    pthread_mutex_unlock(&lock);
    if(audio_enabled)(void)dsound_audio_runtime_set_stream_running(stream_address,tick,mode==0u);
}
/* A state created by a Pause note carries the sentinel serial: bind it to the first lease that asks. */
static stream_state *stream_bind(uint32_t address,uint64_t serial)
{
    for(unsigned i=0u;i<STREAM_STATES;i++)
        if(streams[i].used && streams[i].address==address && streams[i].serial==UINT64_MAX)streams[i].serial=serial;
    return stream_find(address,serial,true);
}
/* T722, measured under Unicorn on a playing buffer: Pause(0) changes nothing (no hardware write), Pause(1) and Pause(2) move
 * the status 1 to 2 (looping 5 to 6), Pause(0) resumes it. A finished buffer (INFERRED, the drain is hardware) takes Pause(0)
 * as the same no-op, Pause(1) and (2) of it, and of a buffer nothing played, are not measured and are refused.
 * T733, measured: after Stop Pause(0) is the same no-op (S_OK, status 1, no write) but Pause(1) and Pause(2) return the MODE as the
 * HRESULT (1 and 2, nothing changes, the voice is no longer in the playing state), which is not modelled and is refused. */
bool dsound_completion_buffer_pause(uint32_t buffer,uint64_t serial,uint32_t mode)
{
    pthread_mutex_lock(&lock);
    buffer_state *b=NULL;
    for(unsigned i=0u;i<BUFFER_STATES;i++)if(buffers[i].used && buffers[i].address==buffer && buffers[i].serial==serial)b=&buffers[i];
    bool admitted=b!=NULL && mode<=2u;
    if(admitted){
        const uint64_t tick=now_ticks();
        buffer_advance(b,tick);
        const bool can_change=mode==0u || (b->playing && !b->stopped);
        if(!can_change || (b->playing && dsound_audio_runtime_active() &&
            (dsound_audio_runtime_clock_frequency()!=clock_frequency ||
             !dsound_audio_runtime_pause_buffer(buffer,serial,tick,mode!=0u)))) {
            pthread_mutex_unlock(&lock);return false;
        }
        if(mode==0u)b->paused=false;
        else if(b->playing && !b->stopped)b->paused=true;
        else admitted=false;
    }
    pthread_mutex_unlock(&lock);
    return admitted;
}
bool dsound_completion_buffer_frequency(uint32_t buffer,uint64_t serial,uint32_t old_hertz,uint32_t new_hertz)
{
    pthread_mutex_lock(&lock);
    buffer_state *b=NULL;
    for(unsigned i=0u;i<BUFFER_STATES;i++)if(buffers[i].used && buffers[i].address==buffer && buffers[i].serial==serial)b=&buffers[i];
    bool admitted=b!=NULL && old_hertz!=0u && new_hertz!=0u;
    if(admitted && b->looping && ticks_for(b->loop_end_samples,new_hertz)<=ticks_for(b->loop_start_samples,new_hertz))admitted=false;
    if(admitted){
        const uint64_t tick=now_ticks();
        buffer_advance(b,tick);
        if(b->playing && dsound_audio_runtime_active() &&
           (dsound_audio_runtime_clock_frequency()!=clock_frequency ||
            !dsound_audio_runtime_frequency_buffer(buffer,serial,tick,new_hertz))) {
            pthread_mutex_unlock(&lock);return false;
        }
        /* Keep samples left (ticks = samples * clock / hertz), rounded UP so a buffer never ends early.
         * A looping playhead is a tick position within its current duration; rescale it with the pitch. */
        if(b->playing){
            const unsigned __int128 scaled=(unsigned __int128)b->position*old_hertz;
            b->position=(uint64_t)(scaled/new_hertz);
            b->duration=ticks_for(b->samples,new_hertz);
            b->loop_start=ticks_for(b->loop_start_samples,new_hertz);
            b->loop_end=ticks_for(b->loop_end_samples,new_hertz);
            if(b->looping && b->position>=b->loop_end && b->loop_end>b->loop_start)
                b->position=b->loop_start+(b->position-b->loop_start)%(b->loop_end-b->loop_start);
            else if(!b->looping){
                const unsigned __int128 remaining_scaled=(unsigned __int128)b->remaining*old_hertz;
                b->remaining=(uint64_t)((remaining_scaled+new_hertz-1u)/new_hertz);
            }
        }
    }
    pthread_mutex_unlock(&lock);
    return admitted;
}
/* T1524: SetLoopRegion of a running voice. The region is in 36-byte blocks of 64 samples, the same conversion as Play. A looping voice's
 * cursor so far follows the old region, the new one applies from now (a cursor past its end wraps into it, one before its start plays on
 * to the end first), mirrored by the mixer voice. A voice that is not looping only records the region (Play's flag decides, Stop
 * clears it), so the mixer is untouched. INFERRED: the APU loop words are not observable, the tick position is the model's. */
bool dsound_completion_buffer_loop(uint32_t buffer,uint64_t serial,uint32_t loop_start,uint32_t loop_length,uint32_t frequency)
{
    pthread_mutex_lock(&lock);
    buffer_state *b=NULL;
    for(unsigned i=0u;i<BUFFER_STATES;i++)
        if(buffers[i].used && buffers[i].address==buffer && buffers[i].serial==serial)b=&buffers[i];
    bool accepted=b!=NULL && frequency!=0u && loop_length!=0u && loop_start%BUFFER_BLOCK==0u && loop_length%BUFFER_BLOCK==0u;
    if(accepted){
        const uint64_t start_samples=(uint64_t)(loop_start/BUFFER_BLOCK)*BUFFER_BLOCK_SAMPLES;
        const uint64_t end_samples=((uint64_t)loop_start+loop_length)/BUFFER_BLOCK*BUFFER_BLOCK_SAMPLES;
        const uint64_t start_ticks=ticks_for(start_samples,frequency),end_ticks=ticks_for(end_samples,frequency);
        const uint64_t tick=now_ticks();
        buffer_advance(b,tick);
        if(end_ticks<=start_ticks || end_samples>b->samples)accepted=false;
        else if(b->playing && b->looping && dsound_audio_runtime_active() &&
                (dsound_audio_runtime_clock_frequency()!=clock_frequency ||
                 !dsound_audio_runtime_loop_buffer(buffer,serial,tick,(size_t)start_samples,(size_t)end_samples)))accepted=false;
        if(accepted){
            b->loop_start_samples=start_samples;b->loop_end_samples=end_samples;
            b->loop_start=start_ticks;b->loop_end=end_ticks;
            if(b->playing && b->looping && b->position>=b->loop_end)
                b->position=b->loop_start+(b->position-b->loop_start)%(b->loop_end-b->loop_start);
        }
    }
    pthread_mutex_unlock(&lock);
    return accepted;
}
static bool ordinary_audio_volume(int32_t volume,int32_t *output)
{
    if(volume>0 || volume<-10000)return false;
    /* Original ordinary mono settings subtract their +0x20 default600
     * millibels. The floor is silent in the existing inferred PCM mixer. */
    const int64_t normalized=(int64_t)volume-600;
    *output=normalized<-10000?-10000:(int32_t)normalized;
    return true;
}
bool dsound_completion_buffer_volume(uint32_t buffer,uint64_t serial,int32_t volume)
{
    pthread_mutex_lock(&lock);
    buffer_state *b=NULL;
    for(unsigned i=0u;i<BUFFER_STATES;i++)
        if(buffers[i].used && buffers[i].address==buffer && buffers[i].serial==serial)b=&buffers[i];
    bool accepted=true;
    if(enabled && b!=NULL) {
        const uint64_t tick=now_ticks();
        buffer_advance(b,tick);
        if(b->playing && dsound_audio_runtime_active()) {
            int32_t normalized;
            accepted=dsound_audio_runtime_clock_frequency()==clock_frequency &&
                ordinary_audio_volume(volume,&normalized) &&
                dsound_audio_runtime_volume_buffer(buffer,serial,tick,normalized);
        }
    }
    pthread_mutex_unlock(&lock);return accepted;
}
/* T733: a state of the same address with another lease serial is a recycled address, never played under this lease. */
bool dsound_completion_buffer_started(uint32_t buffer,uint64_t serial)
{
    pthread_mutex_lock(&lock);bool started=false;
    for(unsigned i=0u;i<BUFFER_STATES;i++)if(buffers[i].used && buffers[i].address==buffer && buffers[i].serial==serial)started=true;
    pthread_mutex_unlock(&lock);return started;
}
/* T1209: running = played under this lease, not Stopped and not yet finished on the virtual clock (a Pause(1) buffer is still running). */
bool dsound_completion_buffer_voice_running(uint32_t buffer,uint64_t serial)
{
    pthread_mutex_lock(&lock);bool running=false;
    for(unsigned i=0u;i<BUFFER_STATES;i++)
        if(buffers[i].used && buffers[i].address==buffer && buffers[i].serial==serial){
            buffer_advance(&buffers[i],now_ticks());running=buffers[i].playing && !buffers[i].stopped;
        }
    pthread_mutex_unlock(&lock);return running;
}
static uint32_t le32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint32_t le16(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8);}
static bool stream_snapshot(uint32_t stream,dsound_stream_snapshot *snapshot)
{return dsound_stream_get_snapshot(stream,snapshot);}
static uint32_t status_word(const stream_state *s,uint32_t max_packets)
{
    uint32_t value=s->queued<max_packets?1u:0u;
    if(s->queued!=0u)value|=s->running?0x10000u:0x20000u;
    return value;
}
static bool word_ok(uint32_t address,uint32_t stream_address)
{
    uint32_t old;
    if(address==0u)return true;
    return (uint64_t)address+4u<=UINT64_C(0x100000000) &&
        !((uint64_t)address<(uint64_t)stream_address+40u && (uint64_t)stream_address<(uint64_t)address+4u) &&
        kernel_guest_read_u32(address,&old) && kernel_guest_write_u32(address,old);
}
uint32_t dsound_completion_stream_status(uint32_t stream,uint32_t output)
{
    dsound_stream_snapshot snap;
    if(!dsound_completion_enabled() || !stream_snapshot(stream,&snap))refuse(STREAM_GET_STATUS,"stream is not owned by the passive adapter");
    if(!snap.volume_seen)refuse(STREAM_GET_STATUS,"only a passive stream with its startup volume recorded has a status");
    uint32_t old;
    if(output==0u || (uint64_t)output+4u>UINT64_C(0x100000000) ||
       ((uint64_t)output<(uint64_t)stream+40u && (uint64_t)stream<(uint64_t)output+4u) ||
       !kernel_guest_read_u32(output,&old) || !kernel_guest_write_u32(output,old))
        refuse(STREAM_GET_STATUS,"status output is null, aliases the stream header or is not writable");
    pthread_mutex_lock(&lock);
    stream_state *s=stream_bind(stream,snap.lease.serial);
    if(s==NULL){pthread_mutex_unlock(&lock);refuse(STREAM_GET_STATUS,"no free passive stream state");}
    stream_advance(s,now_ticks());
    const uint32_t value=status_word(s,snap.scope.max_packets);
    pthread_mutex_unlock(&lock);
    if(!kernel_guest_write_u32(output,value))refuse(STREAM_GET_STATUS,"status output is not writable");
    return 0u;
}
uint32_t dsound_completion_stream_info(uint32_t stream,uint32_t output)
{
    dsound_stream_snapshot snap;
    if(!dsound_completion_enabled() || !stream_snapshot(stream,&snap))refuse(STREAM_GET_INFO,"stream is not owned by the passive adapter");
    const uint32_t block=snap.format_sets!=0u?le16(snap.format+12):snap.scope.block_align;
    const uint8_t *info_format=snap.format_sets!=0u?snap.format:snap.scope.format;
    /* T1185: measured on the original, GetInfo is {5, block, 0, 2 * block} for the stereo 72 byte and the spatial mono 36 byte
     * ADPCM block streams at every rate tried (8000, 22042, 44100, 48000), started or not, before and after SetFormat. */
    const uint32_t info_channels=le16(info_format+2);
    if(!((block==72u && info_channels==2u) || (block==36u && info_channels==1u)) || le16(info_format)!=0x69u || le16(info_format+14)!=4u)
        refuse(STREAM_GET_INFO,"only the measured 72 byte stereo and 36 byte mono ADPCM block streams answer GetInfo");
    uint32_t words[4]={5u,block,0u,2u*block};
    uint32_t old;
    if(output==0u || (uint64_t)output+16u>UINT64_C(0x100000000) ||
       ((uint64_t)output<(uint64_t)stream+40u && (uint64_t)stream<(uint64_t)output+16u) ||
       !kernel_guest_read_u32(output,&old) || !kernel_guest_write_u32(output,old))
        refuse(STREAM_GET_INFO,"info output is null, aliases the stream header or is not writable");
    for(unsigned i=0u;i<4u;i++)if(!kernel_guest_write_u32(output+4u*i,words[i]))refuse(STREAM_GET_INFO,"info output is not writable");
    return 0u;
}
uint32_t dsound_completion_stream_process(uint32_t stream,uint32_t packet_address,uint32_t output_packet)
{
    dsound_stream_snapshot snap;
    if(!dsound_completion_enabled() || !stream_snapshot(stream,&snap))refuse(STREAM_PROCESS,"stream is not owned by the passive adapter");
    if(!snap.volume_seen || !snap.discontinuity_seen)refuse(STREAM_PROCESS,"only a started passive stream takes packets");
    if(output_packet!=0u)refuse(STREAM_PROCESS,"Process output packet must be NULL (no codec output is modelled)");
    pthread_mutex_lock(&lock);
    stream_state *s=stream_bind(stream,snap.lease.serial);
    if(s==NULL){pthread_mutex_unlock(&lock);refuse(STREAM_PROCESS,"no free passive stream state");}
    const uint64_t process_ticks=now_ticks();
    stream_advance(s,process_ticks);
    const bool stream_running=s->running;
    const uint32_t max=snap.scope.max_packets<MAX_QUEUE?snap.scope.max_packets:MAX_QUEUE;
    if(s->queued>=max){pthread_mutex_unlock(&lock);return PACKET_LIST_FULL;}
    pthread_mutex_unlock(&lock);
    if(dsound_audio_runtime_active())
        (void)dsound_audio_runtime_set_stream_running(stream,process_ticks,stream_running);
    uint32_t words[6];
    if(!kernel_guest_read_bytes(packet_address,words,sizeof(words)))refuse(STREAM_PROCESS,"packet is unreadable");
    /* words: buffer, size, completed*, status*, completion event, timestamp*. The original accepts null words, any
     * size (oracle), an event it would signal and a timestamp it would store, which are not modelled. */
    if(words[1]==0u || words[0]==0u || !kernel_guest_range_readable(words[0],words[1]))refuse(STREAM_PROCESS,"packet buffer is null, empty or unreadable");
    if(words[4]!=0u)refuse(STREAM_PROCESS,"a packet completion event is not modelled");
    if(words[5]!=0u)refuse(STREAM_PROCESS,"a packet timestamp is not modelled");
    const uint32_t average=snap.format_sets!=0u?le32(snap.format+8):snap.scope.average_bytes_per_second;
    if(!word_ok(words[2],stream) || !word_ok(words[3],stream) || (words[2]!=0u && words[2]==words[3]))
        refuse(STREAM_PROCESS,"packet completion words alias the stream header, each other or are not writable");
    if(words[2]!=0u && !kernel_guest_write_u32(words[2],0u))refuse(STREAM_PROCESS,"completion size word is not writable");
    if(words[3]!=0u && !kernel_guest_write_u32(words[3],PENDING_STATUS))refuse(STREAM_PROCESS,"completion status word is not writable");
    uint8_t *audio_packet=NULL;
    uint32_t audio_rate=0u;
    bool audio_packet_mixed=false,audio_mono=false;
    const bool audio_enabled=dsound_audio_runtime_active();
    if(audio_enabled) {
        const uint8_t *format=snap.format_sets!=0u?snap.format:snap.scope.format;
        const bool measured_stereo_adpcm=le16(format)==0x69u && le16(format+2u)==2u &&
            le16(format+12u)==72u && le16(format+14u)==4u && le16(format+16u)==2u &&
            le16(format+18u)==64u;
        /* T1238: the mono 36 byte block ADPCM stream is the Story dialogue (MEASURED 13 packets, 387,072 bytes at 22042 Hz in 120 s). */
        audio_mono=le16(format)==0x69u && le16(format+2u)==1u &&
            le16(format+12u)==36u && le16(format+14u)==4u && le16(format+16u)==2u &&
            le16(format+18u)==64u;
        audio_rate=le32(format+4u);
        if((measured_stereo_adpcm || audio_mono) && audio_rate!=0u &&
           words[1]<=4u*1024u*1024u && words[1]%(audio_mono?36u:72u)==0u) {
            audio_packet=malloc(words[1]);
            if(audio_packet==NULL || !kernel_guest_read_bytes(words[0],audio_packet,words[1])) {
                free(audio_packet);
                refuse(STREAM_PROCESS,"opt-in stereo mixer could not copy the validated packet");
            }
        } else {
            pthread_mutex_lock(&lock);
            const bool announce_skip=!announced_audio_skip;announced_audio_skip=true;
            pthread_mutex_unlock(&lock);
            if(announce_skip)dsound_hle_log()("dsound: opt-in PCM mixer skipped an unsupported stream packet; guest packet completion remains modelled without PCM\n");
        }
    }
    pthread_mutex_lock(&lock);
    s=stream_bind(stream,snap.lease.serial);
    if(s==NULL || s->queued>=max){pthread_mutex_unlock(&lock);free(audio_packet);refuse(STREAM_PROCESS,"stream state changed during Process");}
    if(audio_packet!=NULL && !(audio_mono?dsound_audio_runtime_submit_mono(stream,process_ticks,audio_rate,audio_packet,words[1]):
                                          dsound_audio_runtime_submit(stream,process_ticks,audio_rate,audio_packet,words[1]))) {
        pthread_mutex_unlock(&lock);free(audio_packet);
        refuse(STREAM_PROCESS,"opt-in mixer rejected measured stereo or mono ADPCM packet or reached its bounded queue limit");
    }
    audio_packet_mixed=audio_packet!=NULL;
    free(audio_packet);
    unsigned __int128 remaining_q32=(unsigned __int128)ticks_for(words[1],average)<<32u;
    if(s->frequency_base_hz!=0u) {
        const uint8_t *format=snap.format_sets!=0u?snap.format:snap.scope.format;
        const uint32_t nominal_hz=le32(format+4u),base=s->frequency_base_hz;
        remaining_q32=(remaining_q32*nominal_hz)/base;
    }
    s->queue[s->queued++]=(packet){.size=words[1],.completed=words[2],.status=words[3],
        .remaining_q32=remaining_q32};
    stats.stream_packets++;
    census_note(snap.format_sets!=0u?snap.format:snap.scope.format,words[1],audio_packet_mixed);
    const bool announce=!announced_stream;announced_stream=true;
    pthread_mutex_unlock(&lock);
    if(announce && !audio_enabled)dsound_hle_log()("dsound: explicit PASSIVE COMPLETION streams; Process packets complete after size/average bytes "
        "of RUNNING virtual clock time (a paused stream consumes none), status words from the measured original, the drain "
        "stopping the voice is INFERRED; no samples are produced, no voice or mixer, no event or callback\n");
    if(announce && audio_enabled)dsound_hle_log()("dsound: explicit PASSIVE COMPLETION streams; Process packets complete after size/average bytes "
        "of RUNNING virtual clock time; opt-in stereo Xbox ADPCM packets feed the deterministic PCM mixer on the same virtual clock (timing INFERRED); no event or callback\n");
    return 0u;
}
/* T1185: the original Play, Stop, Pause and GetStatus of a SPATIAL startup buffer (flags 0x10, the three startup setters recorded) answer
 * exactly as the ordinary buffer's (HRESULT, status words, no header write) and make the same hardware write COUNT plus the omitted
 * 3D voice registers (0xFE820360.., 0xFE820318, 0xFE820374..). Measured under Unicorn (tests/test_t1185_spatial_buffer_play.py). The
 * spatial DSP itself (HRTF, distance, position) is not modelled and its PCM routing is INFERRED as the plain mono voice. */
static bool ordinary_or_spatial(const dsound_buffer_snapshot *snap)
{
    return snap->scope.flags==0u || (snap->scope.flags==0x10u && snap->cache_mask==7u);
}
uint32_t dsound_completion_buffer_play(uint32_t buffer,uint32_t flags)
{
    dsound_buffer_snapshot snap;
    if(!dsound_completion_enabled() || !dsound_buffer_get_snapshot(buffer,&snap))refuse(BUFFER_PLAY,"buffer is not owned by the passive adapter");
    if(snap.data_sets==0u || snap.volume_sets==0u || snap.pause_sets==0u || snap.frequency_sets==0u || !ordinary_or_spatial(&snap)) {
        char text[200];
        snprintf(text,sizeof text,"only an ordinary buffer with its whole start (data, volume, Pause(0), frequency) recorded can Play "
            "(data %u volume %u pause %u frequency %u flags %#x block %u)",snap.data_sets,snap.volume_sets,snap.pause_sets,
            snap.frequency_sets,snap.scope.flags,snap.scope.block_align);
        refuse(BUFFER_PLAY,text);
    }
    /* Measured (T722): flags 2 (FROMSTARTONLY) answers like 0 (status 1) and 3 like 1 (status 5), bit 0 is looping. */
    if(flags>3u){char text[96];snprintf(text,sizeof text,"only Play flags 0 to 3 are measured, got %#x",flags);refuse(BUFFER_PLAY,text);}
    if(snap.scope.block_align!=BUFFER_BLOCK || snap.frequency==0u || snap.data_length<BUFFER_BLOCK)
        refuse(BUFFER_PLAY,"only the measured 36 byte ADPCM block buffer with a frequency can Play");
    const bool audio_enabled=dsound_audio_runtime_active();
    uint8_t *encoded=NULL;
    int32_t normalized_volume=0;
    if(audio_enabled) {
        if(snap.data_length>DSOUND_AUDIO_BUFFER_MAX_BYTES ||
           !ordinary_audio_volume(snap.volume,&normalized_volume))
            refuse(BUFFER_PLAY,"real ordinary buffer PCM needs bounded data and volume -10000..0");
        encoded=malloc(snap.data_length);
        if(encoded==NULL || !kernel_guest_read_bytes(snap.data_address,encoded,snap.data_length)) {
            free(encoded);refuse(BUFFER_PLAY,"real buffer PCM data is unreadable or allocation failed");
        }
    }
    const uint64_t samples=(uint64_t)(snap.data_length/BUFFER_BLOCK)*BUFFER_BLOCK_SAMPLES;
    if((flags&1u)!=0u && (snap.loop_sets!=0u && (snap.loop_length==0u || snap.loop_start%BUFFER_BLOCK!=0u ||
       snap.loop_length%BUFFER_BLOCK!=0u || (uint64_t)snap.loop_start+snap.loop_length>snap.data_length))) {
        free(encoded);refuse(BUFFER_PLAY,"loop region must be nonempty, block-aligned and within buffer data");
    }
    const uint64_t loop_start_samples=(uint64_t)(snap.loop_start/BUFFER_BLOCK)*BUFFER_BLOCK_SAMPLES;
    const uint64_t loop_end_samples=(uint64_t)((snap.loop_start+snap.loop_length)/BUFFER_BLOCK)*BUFFER_BLOCK_SAMPLES;
    pthread_mutex_lock(&lock);
    if((flags&1u)!=0u && ticks_for(loop_end_samples,snap.frequency)<=ticks_for(loop_start_samples,snap.frequency)) {
        pthread_mutex_unlock(&lock);
        free(encoded);refuse(BUFFER_PLAY,"loop interval is below the configured virtual clock resolution");
    }
    buffer_state *candidate=NULL;
    for(unsigned i=0u;i<BUFFER_STATES;i++) {
        if(buffers[i].used && buffers[i].address==buffer){candidate=&buffers[i];break;}
        if(!buffers[i].used && candidate==NULL)candidate=&buffers[i];
    }
    if(candidate==NULL){pthread_mutex_unlock(&lock);free(encoded);refuse(BUFFER_PLAY,"no free passive buffer state");}
    const uint64_t tick=now_ticks();
    const bool paused=candidate->used && candidate->address==buffer &&
        candidate->serial==snap.lease.serial && candidate->paused;
    if(audio_enabled && (clock_frequency==0u ||
       dsound_audio_runtime_clock_frequency()!=clock_frequency ||
       !dsound_audio_runtime_play_buffer(buffer,snap.lease.serial,tick,snap.frequency,
            encoded,snap.data_length,paused,(flags&1u)!=0u,(size_t)loop_start_samples,
            (size_t)loop_end_samples,normalized_volume))) {
        pthread_mutex_unlock(&lock);free(encoded);
        refuse(BUFFER_PLAY,"real mono buffer decoder/mixer refused clock, data, identity, timestamp or bounded storage");
    }
    buffer_state *b=buffer_find(buffer,snap.lease.serial,true);
    if(b==NULL){pthread_mutex_unlock(&lock);refuse(BUFFER_PLAY,"no free passive buffer state");}
    buffer_advance(b,tick);
    /* Playing a playing buffer is a restart in the original (it reprograms the voice, oracle). */
    b->playing=true;b->stopped=false;b->looping=(flags&1u)!=0u;
    b->samples=samples;b->duration=ticks_for(samples,snap.frequency);b->position=0u;b->remaining=b->duration;b->last=tick;
    b->loop_start_samples=loop_start_samples;
    b->loop_end_samples=loop_end_samples;
    b->loop_start=ticks_for(b->loop_start_samples,snap.frequency);
    b->loop_end=ticks_for(b->loop_end_samples,snap.frequency);
    stats.buffer_plays++;
    const bool announce=!announced_buffer;announced_buffer=true;
    pthread_mutex_unlock(&lock);
    free(encoded);
    if(announce && !audio_enabled)dsound_hle_log()("dsound: explicit PASSIVE COMPLETION buffers; Play marks the buffer playing for samples/frequency "
        "of virtual clock time (looping never ends), status 1 or 5, then 0 (INFERRED, the drain is hardware); no voice, mixer or samples\n");
    if(announce && audio_enabled)dsound_hle_log()("dsound: opt-in real mono Xbox ADPCM buffer voices; original-backed restart/pause/loop/Stop-tail transport "
        "feeds PCM on the completion clock; timing/resampling/stereo routing/speaker gain INFERRED, immutable Play data snapshot, no spatial DSP/HRTF emulation\n");
    return 0u;
}
/* T733, measured on the ORIGINAL under Unicorn (tests/test_dsound_completion_oracle.py): Stop(this) (0x407AA4 -> 0x40708B -> 0x40E72F(settings, 0),
 * RET 4) is S_OK. On a buffer played with flags 0 to 3 it makes 6 hardware writes (7 when it was paused with Pause(2)) and the status is 1
 * afterwards whether it was 1, 2 (paused), 5 or 6: the loop and pause bits are gone and the voice keeps draining the rest of its data, so
 * Stop with flag 0 means "stop at the end of the current data". A second Stop is S_OK and writes nothing, Play after it restarts as usual
 * (19 or 20 writes, status 1 or 5), SetFrequency is S_OK, Pause(0) is the no-op and Pause(1 or 2) answer the mode (see buffer_pause).
 * A buffer nothing played is S_OK, status 0, no write. The model keeps the samples left (the time left is unchanged, a paused buffer
 * starts consuming it again). A buffer that finished on the clock is left alone (INFERRED: the drain is hardware, the original's Stop of a
 * voice that is not playing changes nothing, as for a buffer nothing played). A LOOPING playing buffer drains from where the voice IS, which
 * the model tracks the position on its announced virtual clock. The clock-side cursor and the hardware drain remain INFERRED. */
uint32_t dsound_completion_buffer_stop(uint32_t buffer)
{
    dsound_buffer_snapshot snap;
    if(!dsound_completion_enabled() || !dsound_buffer_get_snapshot(buffer,&snap))refuse(BUFFER_STOP,"buffer is not owned by the passive adapter");
    if(snap.data_sets==0u || snap.volume_sets==0u || snap.pause_sets==0u || snap.frequency_sets==0u || !ordinary_or_spatial(&snap))
        refuse(BUFFER_STOP,"only an ordinary buffer with its whole start (data, volume, Pause(0), frequency) recorded can Stop");
    pthread_mutex_lock(&lock);
    buffer_state *b=buffer_find(buffer,snap.lease.serial,false);
    if(b!=NULL){
        const uint64_t tick=now_ticks();
        buffer_advance(b,tick);
        if(b->playing && !b->stopped && dsound_audio_runtime_active() &&
           (dsound_audio_runtime_clock_frequency()!=clock_frequency ||
            !dsound_audio_runtime_stop_buffer(buffer,snap.lease.serial,tick))) {
            pthread_mutex_unlock(&lock);
            char text[220];
            snprintf(text,sizeof text,"real buffer PCM rejected Stop: %s",dsound_audio_runtime_last_buffer_refusal());
            refuse(BUFFER_STOP,text);
        }
        if(b->playing && b->looping){
            b->remaining=b->duration-b->position;
            b->looping=false;
        }
        if(b->playing){b->paused=false;b->stopped=true;}
    }
    stats.buffer_stops++;
    const bool announce=!announced_stop;announced_stop=true;
    pthread_mutex_unlock(&lock);
    if(announce)dsound_hle_log()("dsound: explicit PASSIVE COMPLETION buffer Stop; a playing buffer loses its pause and drains from its "
        "virtual play position (position and hardware drain INFERRED), one that finished or never played is left alone; active PCM voices follow the same finite tail\n");
    return 0u;
}
uint32_t dsound_completion_buffer_status(uint32_t buffer,uint32_t output)
{
    dsound_buffer_snapshot snap;
    if(!dsound_completion_enabled() || !dsound_buffer_get_snapshot(buffer,&snap))refuse(BUFFER_GET_STATUS,"buffer is not owned by the passive adapter");
    uint32_t old;
    if(output==0u || (uint64_t)output+4u>UINT64_C(0x100000000) || !kernel_guest_read_u32(output,&old) || !kernel_guest_write_u32(output,old))
        refuse(BUFFER_GET_STATUS,"status output is null or not writable");
    pthread_mutex_lock(&lock);
    buffer_state *b=buffer_find(buffer,snap.lease.serial,false);
    uint32_t value=0u;
    if(b!=NULL){buffer_advance(b,now_ticks());if(b->playing)value=(b->paused?2u:1u)|(b->looping?4u:0u);}
    pthread_mutex_unlock(&lock);
    if(!kernel_guest_write_u32(output,value))refuse(BUFFER_GET_STATUS,"status output is not writable");
    return 0u;
}
bool dsound_completion_buffer_get_position(uint32_t buffer,dsound_completion_buffer_position *position)
{
    dsound_buffer_snapshot snap;
    if(position==NULL || !dsound_buffer_get_snapshot(buffer,&snap))return false;
    pthread_mutex_lock(&lock);
    buffer_state *b=NULL;
    if(enabled && clock_frequency!=0u) {
        for(unsigned i=0u;i<BUFFER_STATES;i++)
            if(buffers[i].used && buffers[i].address==buffer && buffers[i].serial==snap.lease.serial)b=&buffers[i];
        if(b!=NULL)buffer_advance(b,now_ticks());
    }
    const bool available=b!=NULL;
    if(available) {
        const unsigned __int128 scaled=(unsigned __int128)b->position*snap.frequency;
        position->position_samples=(uint64_t)(scaled/clock_frequency);
        position->total_samples=b->samples;
        position->frequency=snap.frequency;
        position->playing=b->playing;
        position->looping=b->looping;
        position->paused=b->paused;
    }
    pthread_mutex_unlock(&lock);
    return available;
}
void dsound_completion_work(void)
{
    pthread_mutex_lock(&lock);
    if(enabled) {
        const uint64_t now=now_ticks();
        for(unsigned i=0u;i<STREAM_STATES;i++)if(streams[i].used && streams[i].serial!=UINT64_MAX){
            stream_advance(&streams[i],now);
            if(dowork_delivery)stream_deliver_done(&streams[i]);
        }
        for(unsigned i=0u;i<BUFFER_STATES;i++)if(buffers[i].used)buffer_advance(&buffers[i],now);
    }
    pthread_mutex_unlock(&lock);
}
bool dsound_completion_method_owned(uint32_t stack_pointer)
{
    uint32_t stream;dsound_stream_snapshot snap;
    return dsound_completion_enabled() && stack_pointer<=UINT32_MAX-8u &&
        kernel_guest_read_u32(stack_pointer+4u,&stream) && stream_snapshot(stream,&snap);
}
bool dsound_completion_route_method(uint32_t entry,uint32_t stack_pointer,uint32_t *result,uint32_t *pop_bytes)
{
    uint32_t args[3]={0u,0u,0u};unsigned count;
    switch(entry){
    case STREAM_GET_STATUS: case STREAM_GET_INFO: count=2u;break;
    case STREAM_PROCESS: count=3u;break;
    default: return false;
    }
    if(result==NULL || pop_bytes==NULL || !dsound_completion_enabled() || stack_pointer>UINT32_MAX-16u)return false;
    for(unsigned i=0u;i<count;i++)if(!kernel_guest_read_u32(stack_pointer+4u+4u*i,&args[i]))refuse(entry,"unreadable argument");
    if(entry==STREAM_GET_STATUS)*result=dsound_completion_stream_status(args[0],args[1]);
    else if(entry==STREAM_GET_INFO)*result=dsound_completion_stream_info(args[0],args[1]);
    else *result=dsound_completion_stream_process(args[0],args[1],args[2]);
    *pop_bytes=4u*count;return true;
}
static uint32_t frame_handler(void *context,uint32_t entry)
{
    /* T733: no caller gate. The originals (0x407A80, 0x407AA4, 0x407AF8 and what they call) never read their return address, so a call
     * site is not a property of the method. The state checks of the model decide, and the handlers exist only with the model on. */
    const kernel_call_frame *frame=context;uint32_t args[4];
    const unsigned count=entry==BUFFER_PLAY?4u:entry==BUFFER_STOP?1u:2u;
    if(frame==NULL)refuse(entry,"unreadable call frame");
    for(unsigned i=0u;i<count;i++)if(!kernel_frame_arg(frame,i,&args[i]))refuse(entry,"unreadable argument");
    if(entry==BUFFER_PLAY){
        if(args[1]!=0u || args[2]!=0u){
            char text[160];
            snprintf(text,sizeof text,"Play reserved arguments must be zero, got (%#x, %#x, %#x, %#x)",args[0],args[1],args[2],args[3]);
            refuse(entry,text);
        }
        return dsound_completion_buffer_play(args[0],args[3]);
    }
    if(entry==BUFFER_STOP)return dsound_completion_buffer_stop(args[0]);
    return dsound_completion_buffer_status(args[0],args[1]);
}
static uint32_t play_handler(void *c){return frame_handler(c,BUFFER_PLAY);}
static uint32_t status_handler(void *c){return frame_handler(c,BUFFER_GET_STATUS);}
static uint32_t stop_handler(void *c){return frame_handler(c,BUFFER_STOP);}
size_t dsound_completion_register(void)
{
    if(!dsound_completion_enabled())return 0u;
    size_t count=0u;count+=dsound_hle_register(BUFFER_PLAY,play_handler)?1u:0u;
    count+=dsound_hle_register(BUFFER_GET_STATUS,status_handler)?1u:0u;
    count+=dsound_hle_register(BUFFER_STOP,stop_handler)?1u:0u;return count;
}

bool dsound_completion_mixbin_headroom(uint64_t identity,uint32_t bin,uint32_t headroom)
{
    pthread_mutex_lock(&lock);
    const bool ok=enabled && clock_frequency==dsound_audio_runtime_clock_frequency() &&
        dsound_audio_runtime_mixbin_headroom(identity,now_ticks(),bin,headroom);
    pthread_mutex_unlock(&lock);
    return ok;
}

bool dsound_completion_bind_mixbin_headroom(uint64_t identity)
{
    pthread_mutex_lock(&lock);
    bool ok=enabled && clock_frequency==dsound_audio_runtime_clock_frequency() &&
        dsound_audio_runtime_bind_mixbin_headroom(identity);
    pthread_mutex_unlock(&lock);
    return ok;
}
