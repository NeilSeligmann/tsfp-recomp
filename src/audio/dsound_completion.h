/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_AUDIO_DSOUND_COMPLETION_H
#define TSFP_AUDIO_DSOUND_COMPLETION_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "dsound_stream_routing.h"
/* T681, option (a) of the owner decision T608: the PASSIVE completion model for the title's own stream and
 * sound state machines, opt-in (`--passive-audio-completion`, default off, a flags-off boot is byte identical).
 *
 * A passive stream (dsound_stream.c) takes `Process` 0x407424 packets (at most `max_packets`, the original
 * answers 0x88780032 for a further one without reading it), reports playing or paused and the free-packet bit
 * through `GetStatus` 0x4073D3, and a packet completes after `size / average_bytes_per_second` of RUNNING time
 * on the kernel virtual clock (a paused stream consumes none): the completed size and status words are written
 * (status 0) when it is observed at Process, GetStatus, Pause or DirectSoundDoWork. A passive buffer takes `Play`
 * 0x407A80 and reports `GetStatus` 0x407AF8 1 (5 when looping) until `samples / frequency` of clock time has
 * passed, then 0. Looping buffers advance a virtual sample cursor modulo their sample count; Stop turns that cursor
 * into a finite drain. The measured status words, the packet limit and the XMEDIAINFO answer are pinned against the
 * original bytes under Unicorn (`tests/test_dsound_completion_oracle.py`). What the original cannot show under
 * Unicorn is the hardware's drain or playhead, so "a drained stream stops playing", "a played buffer stops", and
 * the loop cursor are INFERRED, announced and named. No voice, mixer or sample is produced, volume is recorded as a value (never a list). */
void dsound_completion_set_enabled(bool enabled);
bool dsound_completion_enabled(void);
/* T855 + T871 (xemu-level, `--passive-audio-completion-at-dowork`, DEFAULT with the model, `--no-...` opts out): a packet that passes its deadline keeps its words
 * (pending status, size 0) and its list slot until the next DirectSoundDoWork the model sees, which delivers every finished packet in
 * order. The original recycles the record inside DoWork (the voice event 2 handler 0x40B98A walking voice+0xB8, T844). Off, words are
 * written when the packet is first observed past its deadline (T681). */
void dsound_completion_set_dowork_delivery(bool enabled);
bool dsound_completion_dowork_delivery(void);
typedef uint64_t (*dsound_completion_clock_fn)(void);
/* NULL restores the kernel virtual clock (`kernel_clock_peek`). `frequency` is the ticks per second. */
void dsound_completion_set_clock(dsound_completion_clock_fn clock,uint64_t frequency);
typedef void (*dsound_completion_fatal_fn)(uint32_t address,const char *reason);
void dsound_completion_set_fatal(dsound_completion_fatal_fn fatal);
/* dsound_stream.c tells the model a Pause mode (0 resumes, 1 pauses), under the stream lock, no re-entry. */
void dsound_completion_note_pause(uint32_t stream,uint32_t mode);
/* Stream lock -> completion -> PCM lock; never calls stream observation back. */
bool dsound_completion_stream_routing(uint32_t stream,uint64_t serial,
                                       const dsound_stream_routing *routing);
/* Change queued/playing packet source time at the current clock edge. Existing
 * exact lease state and matching prior rate required; refusal preserves state.
 * The PCM variant commits the renderer first under completion -> runtime locks,
 * then publishes its prevalidated completion candidate without a fallible step. */
bool dsound_completion_stream_frequency(uint32_t stream,uint64_t serial,uint64_t ticks,
                                        uint32_t old_rate_hz,uint32_t new_rate_hz);
bool dsound_completion_stream_frequency_pcm(uint32_t stream,uint64_t serial,uint64_t ticks,
                                            uint32_t old_rate_hz,uint32_t new_rate_hz);
bool dsound_completion_note_frequency(uint32_t stream,uint64_t serial,uint32_t old_hz,uint32_t new_hz);
/* dsound_buffer.c asks before it admits Pause or SetBufferData on a buffer that was started. */
bool dsound_completion_buffer_started(uint32_t buffer,uint64_t serial);
bool dsound_completion_buffer_voice_running(uint32_t buffer,uint64_t serial);
/* T1524: live SetLoopRegion of a running voice (see dsound_buffer_loop_fn). */
bool dsound_completion_buffer_loop(uint32_t buffer,uint64_t serial,uint32_t loop_start,uint32_t loop_length,uint32_t frequency);
/* T722: SetFrequency of a played buffer keeps the samples left: the time left is rescaled from the old to the new rate. */
bool dsound_completion_buffer_frequency(uint32_t buffer,uint64_t serial,uint32_t old_hertz,uint32_t new_hertz);
/* Called with buffer ownership/lock held: no buffer reentry. Applies real PCM
 * volume only when an ordinary buffer is currently playing. */
bool dsound_completion_buffer_volume(uint32_t buffer,uint64_t serial,int32_t volume);
/* T733/T756: Stop 0x407AA4 (stdcall RET 4): S_OK. A playing buffer loses its pause and drains from its virtual position (status 1);
 * an ordinary loop cursor is tracked modulo duration and its remaining tail drains after Stop. The cursor and hardware drain are
 * INFERRED because Unicorn's register model has no clocked APU. A buffer nothing played or one that finished is left alone. */
uint32_t dsound_completion_buffer_stop(uint32_t buffer);
/* T722: Pause of a played buffer, true when modelled (see the definition). */
bool dsound_completion_buffer_pause(uint32_t buffer,uint64_t serial,uint32_t mode);
/* The indirect stream methods (GetStatus 0x4073D3, Process 0x407424, GetInfo 0x4072D4) for the xdk_thunk
 * completion route. `owned` has no side effect. `route` returns false for what it does not answer. */
bool dsound_completion_method_owned(uint32_t stack_pointer);
bool dsound_completion_route_method(uint32_t entry,uint32_t stack_pointer,uint32_t *result,uint32_t *pop_bytes);
/* Direct entries, `frame` is the kernel call frame (return address at the stack pointer). */
uint32_t dsound_completion_buffer_play(uint32_t buffer,uint32_t flags);
uint32_t dsound_completion_buffer_status(uint32_t buffer,uint32_t output);
/* HOST observation for a mixer: advances to now_ticks(), then returns the virtual cursor in sample frames. False means the model
 * is disabled, the buffer is not an admitted owned buffer, or no clock frequency is configured. The reported playhead is inferred. */
typedef struct dsound_completion_buffer_position {
    uint64_t position_samples,total_samples;
    uint32_t frequency;
    bool playing,looping,paused;
} dsound_completion_buffer_position;
bool dsound_completion_buffer_get_position(uint32_t buffer,dsound_completion_buffer_position *position);
uint32_t dsound_completion_stream_process(uint32_t stream,uint32_t packet,uint32_t output_packet);
uint32_t dsound_completion_stream_status(uint32_t stream,uint32_t output);
uint32_t dsound_completion_stream_info(uint32_t stream,uint32_t output);
/* DirectSoundDoWork: observe every stream and buffer now so completion words are written on time (T855: delivered here when the mode is on). */
void dsound_completion_work(void);
/* Registers Play, Stop and buffer GetStatus ONLY while the policy is enabled, so a flags-off registry is unchanged. */
size_t dsound_completion_register(void);
/* HOST observation for tests. */
typedef struct dsound_completion_stats {
    uint32_t stream_packets,stream_completed,buffer_plays,buffer_finished,observations,buffer_stops,stream_aborted;
} dsound_completion_stats;
dsound_completion_stats dsound_completion_get_stats(void);
/* T1238: census of the stream packets by format, so a stream the mixer cannot voice is never silent in the report. */
#define DSOUND_COMPLETION_CENSUS_MAX 12u
typedef struct dsound_completion_census_entry {
    uint32_t tag,channels,rate,block,bits,packets,mixed,bytes;
    uint64_t first_ms,last_ms; /* guest clock ms of the first and the last packet: the dialogue trigger point */
} dsound_completion_census_entry;
size_t dsound_completion_stream_census(dsound_completion_census_entry *out,size_t capacity);
/* Forget every stream and buffer state (shutdown and tests). */
void dsound_completion_reset(void);
bool dsound_completion_mixbin_headroom(uint64_t identity,uint32_t bin,uint32_t headroom);
bool dsound_completion_bind_mixbin_headroom(uint64_t identity);
/* T1245: verified lease; abort pending packets at the format event. */
bool dsound_completion_note_format(uint32_t stream,uint64_t serial,uint32_t old_hz,uint32_t new_hz,
    uint32_t format_address);
#endif
