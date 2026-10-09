/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsound_stream_frequency.h"
#include <fenv.h>
#include <math.h>
#include <string.h>
/* The original multiplies exact integer hertz by guest float32 reciprocal,
 * stores float32, then FYL2X*4096 and FISTP with the guest rounding control. */
bool dsound_stream_frequency_pitch(uint32_t hertz,uint16_t control_word,int32_t *pitch)
{
    if(pitch==NULL||hertz<8000u||hertz>96000u||
       (control_word!=0x003Fu&&control_word!=0x027Fu&&control_word!=0x037Fu))return false;
    if(hertz==48000u) {*pitch=0;return true;}
    fenv_t previous;
    if(feholdexcept(&previous)!=0)return false;
    if(fesetround(FE_TONEAREST)!=0) {(void)fesetenv(&previous);return false;}
    const uint32_t reciprocal_bits=0x37AEC33Eu;float reciprocal;
    memcpy(&reciprocal,&reciprocal_bits,sizeof(reciprocal));
    const float normalized=(float)((double)hertz*(double)reciprocal);
    const long double scaled=4096.0L*log2l((long double)normalized);
    const long long result=llrintl(scaled);
    if(fesetenv(&previous)!=0)return false;
    if(result<INT32_MIN||result>INT32_MAX)return false;
    *pitch=(int32_t)result;return true;
}

bool dsound_stream_frequency_clock_init(dsound_frequency_clock *clock,uint32_t base_hertz)
{
    if(!clock||base_hertz==0u||base_hertz>200000u)return false;
    *clock=(dsound_frequency_clock){.base_hertz=base_hertz,.count=1u};
    clock->events[0].frequency_q32=(uint64_t)base_hertz<<32u;
    return true;
}
bool dsound_stream_frequency_clock_phase(const dsound_frequency_clock *clock,uint64_t active,
    dsound_frequency_phase *phase)
{
    if(!clock||!phase||clock->base_hertz==0u||clock->count==0u||
       clock->count>DSOUND_FREQUENCY_EVENTS||active<clock->events[0].active_ticks)return false;
    const dsound_frequency_event *event=&clock->events[0];
    for(uint32_t i=1u;i<clock->count;i++) {
        if(clock->events[i].active_ticks>active)break;
        event=&clock->events[i];
    }
    const uint64_t denominator=(uint64_t)clock->base_hertz<<32u;
    const unsigned __int128 product=(unsigned __int128)(active-event->active_ticks)*event->frequency_q32;
    const unsigned __int128 whole=product/denominator;
    const uint64_t fraction=(uint64_t)(((product%denominator)<<32u)/denominator)+event->fraction;
    const unsigned __int128 result=whole+event->source_ticks+(fraction>>32u);
    if(result>UINT64_MAX)return false;
    *phase=(dsound_frequency_phase){(uint64_t)result,(uint32_t)fraction};return true;
}
bool dsound_stream_frequency_clock_change(dsound_frequency_clock *clock,uint64_t active,
    uint64_t frequency_q32,uint64_t retire_through)
{
    if(!clock||clock->count==0u||clock->count>DSOUND_FREQUENCY_EVENTS||
       frequency_q32==0u||frequency_q32>((uint64_t)200000u<<32u)||retire_through>active||
       active<clock->events[clock->count-1u].active_ticks)return false;
    dsound_frequency_phase phase;
    if(!dsound_stream_frequency_clock_phase(clock,active,&phase))return false;
    const dsound_frequency_event next={active,phase.ticks,frequency_q32,phase.fraction};
    if(active==clock->events[clock->count-1u].active_ticks) {
        clock->events[clock->count-1u]=next;return true;
    }
    uint32_t keep=0u;
    while(keep+1u<clock->count&&clock->events[keep+1u].active_ticks<=retire_through)keep++;
    const uint32_t retained=clock->count-keep;
    if(retained==DSOUND_FREQUENCY_EVENTS)return false;
    memmove(clock->events,clock->events+keep,(size_t)retained*sizeof(clock->events[0]));
    clock->events[retained]=next;clock->count=retained+1u;return true;
}
