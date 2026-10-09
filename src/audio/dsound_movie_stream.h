/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_AUDIO_DSOUND_MOVIE_STREAM_H
#define TSFP_AUDIO_DSOUND_MOVIE_STREAM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "kernel_call.h"

/* T392: the XMV movie PCM stream, a bounded CPU model. Default off (--headless-movie-audio).
 *
 * This is NOT the startup stream node (dsound_stream.c): the startup checks stay exact. A separate
 * list owns movie streams, found by guest address. The model is a policy, announced once:
 *
 *  - No audio is decoded, mixed, produced or played. No APU voice, DSP, interrupt or DPC exists.
 *  - Every guest-visible effect of a call (return value, status and completed-size words, header
 *    reference count, GetStatus bits, the callback arguments) is pinned against the ORIGINAL code
 *    in the Unicorn oracle (tests/test_dsound_movie_stream_oracle.py).
 *  - The ONE thing the original leaves to hardware is WHEN a packet completes. The stated rule:
 *    a stream that is not paused plays its attached packets back to back at the format's byte rate
 *    (avg bytes per second) on the virtual clock (kernel_clock, never wall time). A packet's
 *    deadline is the previous deadline (or the attach/resume tick when the stream was idle) plus
 *    ceil(size * clock_hz / avg_bytes). Completion is observed ONLY inside DirectSoundDoWork from a
 *    movie caller, on the first call at or after the deadline. It then writes *completed = size,
 *    *status = 0 and runs callback(streamContext, packetContext, 0), exactly the original's
 *    completion helper 0x40B244. FlushEx(0, 0, 1) aborts every pending packet at the next DoWork
 *    with status 0x80004004 (the status the original writes when it aborts at final Release).
 *  - Pause(2) holds the stream until SynchPlayback, which GetNextFrame reaches through the original
 *    0x407A4C. That body reads the omitted APU object, so the call is routed here instead (T421, one
 *    stop boundary in tools/config/movie_synch_boundaries.json and a re-lift).
 *    dsound_movie_stream_route_synch() validates it and dsound_movie_stream_synch_playback() is the model.
 *
 * Anything outside the measured scope stops the run through the fatal handler.
 * All entry points but create validate that `stream` is owned here. Create only admits the exact
 * descriptor the retained XMV EnableAudioStream builds.
 */
#define DSOUND_MOVIE_STREAM_BYTES 40u
#define DSOUND_MOVIE_MAX_PACKETS 2u
#define DSOUND_MOVIE_CALLBACK 0x00445071u
#define DSOUND_MOVIE_ABORT_STATUS 0x80004004u
#define DSOUND_MOVIE_PENDING_STATUS 0x8000000Au
#define DSOUND_MOVIE_TOO_MANY_PACKETS 0x88780032u

void dsound_movie_stream_set_enabled(bool enabled);
bool dsound_movie_stream_enabled(void);
typedef bool (*dsound_movie_stream_irql_provider)(uint8_t *out);
typedef void (*dsound_movie_stream_fatal_fn)(uint32_t address, const char *reason);
void dsound_movie_stream_set_irql_provider(dsound_movie_stream_irql_provider provider);
void dsound_movie_stream_set_fatal(dsound_movie_stream_fatal_fn fatal);
/* The virtual clock in ticks. Default is kernel_clock_peek (does not advance the clock). */
typedef uint64_t (*dsound_movie_stream_clock_fn)(void);
void dsound_movie_stream_set_clock(dsound_movie_stream_clock_fn clock, uint64_t frequency);
/* Runs the guest completion callback `callback(stream_context, packet_context, status)` on the
 * calling guest thread. Returns false when it could not. A false return is fatal, a callback is
 * never skipped. Called with no module lock held. */
typedef bool (*dsound_movie_stream_callback_fn)(uint32_t callback, uint32_t stream_context,
                                                uint32_t packet_context, uint32_t status);
void dsound_movie_stream_set_callback_runner(dsound_movie_stream_callback_fn runner);

/* True when `return_address` lies in the XMV library or the title's movie function, the only
 * callers of the movie stream. */
bool dsound_movie_stream_caller_ok(uint32_t return_address);
/* Create is admitted only from the one measured call site in EnableAudioStream. */
bool dsound_movie_stream_create_caller_ok(uint32_t return_address);
bool dsound_movie_stream_owns(uint32_t stream);

uint32_t dsound_movie_stream_create(uint32_t descriptor, uint32_t output);
uint32_t dsound_movie_stream_add_ref(uint32_t stream);
uint32_t dsound_movie_stream_release(uint32_t stream);
uint32_t dsound_movie_stream_set_volume(uint32_t stream, int32_t volume);
uint32_t dsound_movie_stream_pause(uint32_t stream, uint32_t mode);
uint32_t dsound_movie_stream_flush_ex(uint32_t stream, uint32_t time_low, uint32_t time_high,
                                      uint32_t flags);
/* T394: log every model event on stderr with its virtual tick (--trace-xmv). Default off. */
void dsound_movie_stream_set_trace(bool enabled);
uint32_t dsound_movie_stream_discontinuity(uint32_t stream);
/* IDirectSoundStream::Flush (0x407388, T394): the synchronous abort of every attached packet. */
uint32_t dsound_movie_stream_flush(uint32_t stream);
uint32_t dsound_movie_stream_get_status(uint32_t stream, uint32_t output);
uint32_t dsound_movie_stream_process(uint32_t stream, uint32_t packet, uint32_t output_packet);
/* Model of IDirectSound::SynchPlayback, see above. Resumes every Pause(2) stream at the clock. */
void dsound_movie_stream_synch_playback(void);
/* The thunk route body of SynchPlayback (T421, xdk_thunk_set_synch_handler). `stack_pointer` is the
 * guest stack at the return address, the one argument above it must be the device interface the
 * device facade owns. Admitted only with the policy on, a known IRQL 0, the original global audio state
 * zero (the original returns E_FAIL otherwise, which the facade never produces) and the one measured
 * return address 0x0044571B. Result 0, as measured. Anything else stops the run, never a silent 0. */
bool dsound_movie_stream_route_synch(uint32_t stack_pointer, uint32_t *result);
/* The DirectSoundDoWork body for movie callers: complete every due packet. Returns the number
 * of completions delivered. */
size_t dsound_movie_stream_do_work(void);

typedef struct dsound_movie_stream_snapshot {
    uint32_t stream_address, refs, callback, context;
    int32_t volume;
    bool volume_seen, discontinuity_seen, flush_pending;
    uint32_t pause_mode, queued;
    uint32_t packet_size[DSOUND_MOVIE_MAX_PACKETS];
    uint64_t head_deadline;
} dsound_movie_stream_snapshot;
bool dsound_movie_stream_get_snapshot(uint32_t stream, dsound_movie_stream_snapshot *output);
uint64_t dsound_movie_stream_completion_count(void);
/* Movie stream operations answered (create, methods, DoWork) since the last reset. */
uint64_t dsound_movie_stream_operation_count(void);
size_t dsound_movie_stream_count(void);
/* T-movie: PCM routed into the audio runtime mixer (packets, frames) and packets the mixer refused. */
uint64_t dsound_movie_stream_pcm_packets(void);
uint64_t dsound_movie_stream_pcm_frames(void);
uint64_t dsound_movie_stream_pcm_refused(void);

/* Entry routing for the public DSOUND rows that the startup streams also use (Create, Pause,
 * FlushEx, SetVolume), called by dsound_stream.c with the guest return address. True when the
 * movie model answered (result set). False means not a movie call, and the startup policy decides.
 * A movie stream called from outside the movie callers is fatal. */
bool dsound_movie_stream_route_public(uint32_t entry, const kernel_call_frame *frame,
                                      uint32_t return_address, uint32_t *result);
/* The indirect vtable methods (AddRef 0x40723F, Release 0x407286, GetStatus 0x4073D3, Process
 * 0x407424, Discontinuity 0x40733B), called by the thunk layer with the guest stack pointer at the
 * return address. True when a movie stream answered, `pop_bytes` is the stdcall argument bytes. */
/* No side effect: true when the `this` argument above the return address at `stack_pointer` is a
 * movie stream and the policy is on. */
bool dsound_movie_stream_method_owned(uint32_t stack_pointer);
bool dsound_movie_stream_route_method(uint32_t entry, uint32_t stack_pointer, uint32_t *result,
                                      uint32_t *pop_bytes);
/* DirectSoundDoWork from a movie caller while at least one movie stream exists. True when
 * handled. Otherwise the existing owner decides (and refuses a non-startup caller). */
bool dsound_movie_stream_route_work(uint32_t return_address);

/* Quiescent shutdown. Pending packets are dropped without completion or callback (the host is
 * ending, nothing runs). Refuses and keeps allocations when ownership changed. */
bool dsound_movie_stream_reset_checked(void);
void dsound_movie_stream_reset(void);
#endif
