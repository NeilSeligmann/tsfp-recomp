/* SPDX-License-Identifier: GPL-3.0-or-later */
/* These assertions execute the qualification calls, including in Release builds. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "dsound_stream_frequency.h"
#include <assert.h>
#include <string.h>
int main(void) {
    dsound_frequency_clock c;dsound_frequency_phase p;
    assert(dsound_stream_frequency_clock_init(&c,8000u));
    assert(dsound_stream_frequency_clock_change(&c,10u,UINT64_C(16000)<<32u,0u));
    assert(dsound_stream_frequency_clock_phase(&c,9u,&p)&&p.ticks==9u&&p.fraction==0u);
    assert(dsound_stream_frequency_clock_phase(&c,15u,&p)&&p.ticks==20u);
    assert(dsound_stream_frequency_clock_change(&c,15u,UINT64_C(4000)<<32u,0u));
    assert(dsound_stream_frequency_clock_phase(&c,19u,&p)&&p.ticks==22u);
    assert(dsound_stream_frequency_clock_change(&c,19u,UINT64_C(12000)<<32u,0u));
    assert(dsound_stream_frequency_clock_phase(&c,20u,&p)&&p.ticks==23u&&p.fraction==0x80000000u);
    assert(dsound_stream_frequency_clock_change(&c,20u,UINT64_C(8000)<<32u,0u));
    assert(dsound_stream_frequency_clock_phase(&c,21u,&p)&&p.ticks==24u&&p.fraction==0x80000000u);
    dsound_frequency_clock before=c;
    assert(!dsound_stream_frequency_clock_change(&c,19u,UINT64_C(8000)<<32u,0u));
    assert(memcmp(&before,&c,sizeof(c))==0);
    assert(!dsound_stream_frequency_clock_change(&c,21u,0u,0u));
    assert(memcmp(&before,&c,sizeof(c))==0);
    assert(dsound_stream_frequency_clock_init(&c,8000u));
    for(uint32_t i=1u;i<DSOUND_FREQUENCY_EVENTS;i++)
        assert(dsound_stream_frequency_clock_change(&c,i,UINT64_C(8000)<<32u,0u));
    before=c;
    assert(!dsound_stream_frequency_clock_change(&c,64u,UINT64_C(8000)<<32u,0u));
    assert(memcmp(&before,&c,sizeof(c))==0);
    assert(dsound_stream_frequency_clock_change(&c,64u,UINT64_C(8000)<<32u,63u));
    assert(c.count==2u&&dsound_stream_frequency_clock_phase(&c,100u,&p)&&p.ticks==100u);
    p=(dsound_frequency_phase){123u,456u};
    assert(!dsound_stream_frequency_clock_phase(&c,62u,&p)&&p.ticks==123u&&p.fraction==456u);
    assert(dsound_stream_frequency_clock_init(&c,8000u));
    assert(dsound_stream_frequency_clock_change(&c,0u,UINT64_C(16000)<<32u,0u));
    assert(!dsound_stream_frequency_clock_phase(&c,UINT64_MAX,&p));
    assert(p.ticks==123u&&p.fraction==456u);
    return 0;
}
