/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_DSOUND_STREAM_FREQUENCY_H
#define TSFP_DSOUND_STREAM_FREQUENCY_H
#include <stdbool.h>
#include <stdint.h>
/* Original406AD5/40C083 numeric adapter. Explicit nearest-rounding guest CW
 * 003F/027F/037F and finite positive rate8000..96000 only. Failure preserves output.
 * Does not update a stream, program an APU or register an XDK success path. */
bool dsound_stream_frequency_pitch(uint32_t hertz,uint16_t control_word,int32_t *pitch);
#define DSOUND_FREQUENCY_EVENTS 64u
typedef struct dsound_frequency_phase {uint64_t ticks;uint32_t fraction;} dsound_frequency_phase;
typedef struct dsound_frequency_event {
    uint64_t active_ticks,source_ticks,frequency_q32;
    uint32_t fraction;
} dsound_frequency_event;
typedef struct dsound_frequency_clock {
    uint32_t base_hertz,count;
    dsound_frequency_event events[DSOUND_FREQUENCY_EVENTS];
} dsound_frequency_clock;
/* INFERRED source-time clock, in running ticks (pauses removed by embedding
 * mixer). Retire only before already rendered time. Failure is transactional. */
bool dsound_stream_frequency_clock_init(dsound_frequency_clock *clock,uint32_t base_hertz);
bool dsound_stream_frequency_clock_phase(const dsound_frequency_clock *clock,uint64_t active,
    dsound_frequency_phase *phase);
bool dsound_stream_frequency_clock_change(dsound_frequency_clock *clock,uint64_t active,
    uint64_t frequency_q32,uint64_t retire_through);
#endif
