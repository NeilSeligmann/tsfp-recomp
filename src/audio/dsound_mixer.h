/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_AUDIO_DSOUND_MIXER_H
#define TSFP_AUDIO_DSOUND_MIXER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "dsound_stream_routing.h"

typedef struct dsound_mixer dsound_mixer;

typedef struct dsound_mixer_counts {
    uint64_t packets_queued;
    uint64_t frames_emitted;
    uint64_t late_packets_refused;
    uint64_t buffer_commands_late; /* T1216: buffer commands applied at the render horizon instead of their earlier timestamp */
} dsound_mixer_counts;

/*
 * Create the deterministic offline PCM mixer. The output is stereo signed
 * 16-bit PCM at output_rate_hz. AC97/kernel virtual ticks drive its clock;
 * this timing relationship is INFERRED until measured in xemu.
 */
dsound_mixer *dsound_mixer_create(uint32_t output_rate_hz, uint64_t ticks_per_second);
void dsound_mixer_destroy(dsound_mixer *mixer);
void dsound_mixer_reset_buffers(dsound_mixer *mixer);

/*
 * Decode and enqueue one stereo Xbox ADPCM packet for a DirectSound stream.
 * Packets for one stream are played consecutively if submitted before the
 * queued tail drains; packets arriving after the tail start at now_ticks.
 * The packet bytes are copied/decoded immediately. This packet scheduling is
 * INFERRED from the passive completion byte-rate timeline.
 */
bool dsound_mixer_submit_xbox_adpcm(dsound_mixer *mixer, uint32_t stream_key,
                                    uint64_t now_ticks, uint32_t source_rate_hz,
                                    const uint8_t *encoded, size_t encoded_bytes);

/* T1238: the mono (36 byte block) Xbox ADPCM stream packet, the Story dialogue format. Same scheduling as the stereo submit,
 * the decoded mono samples feed both speakers. */
bool dsound_mixer_submit_xbox_adpcm_mono(dsound_mixer *mixer, uint32_t stream_key,
                                         uint64_t now_ticks, uint32_t source_rate_hz,
                                         const uint8_t *encoded, size_t encoded_bytes);

/*
 * T-movie: enqueue interleaved stereo signed 16-bit PCM (`frames` frames at source_rate_hz), the
 * XMV movie soundtrack. Same scheduling, pause and late rules as the ADPCM submit. The samples are
 * copied.
 */
bool dsound_mixer_submit_pcm16_stereo(dsound_mixer *mixer, uint32_t stream_key, uint64_t now_ticks,
                                      uint32_t source_rate_hz, const int16_t *pcm, size_t frames);
/* Abort a stream's unplayed audio at now_ticks (flush): later packets are dropped, the playing one
 * is truncated, the stream tail moves back so a following packet starts at now. */
bool dsound_mixer_cut_stream(dsound_mixer *mixer, uint32_t stream_key, uint64_t now_ticks);

/* Per stream DirectSound volume (hundredths of a dB of amplitude, 0 full .. -10000 mute), applied at
 * render time to every packet of the stream, queued ones included. Anything outside the range is
 * refused (false, nothing changes). Volume 0 leaves the PCM bit identical. */
#define DSOUND_MIXER_VOLUME_MIN (-10000)
bool dsound_mixer_volume_to_gain_q16(int32_t millibels, uint32_t *gain_q16);
bool dsound_mixer_set_stream_volume(dsound_mixer *mixer, uint32_t stream_key, int32_t millibels);
/* Timestamped source-rate event changes queued and playing samples through a
 * continuous source clock, with pauses removed. Events before the render horizon
 * or earlier than the last frequency event are refused without mutation. Timing
 * remains INFERRED. This consumer does not register the guest method. */
bool dsound_mixer_set_stream_frequency(dsound_mixer *mixer, uint32_t stream_key,
                                       uint64_t ticks, uint32_t source_rate_hz);
/* Verified lease/rate variant for the shared completion transaction. Requires
 * an existing stream routing identity; rejection preserves the full stream. */
bool dsound_mixer_frequency_stream(dsound_mixer *mixer,uint32_t key,uint64_t serial,
    uint64_t ticks,uint32_t base_hz,uint32_t old_hz,uint32_t new_hz);
/* Restore unity only when the stream key already has mixer state; this never allocates a slot or
 * changes queued packet timing. Used when a guest stream lifetime ends or a guest address is reused. */
bool dsound_mixer_reset_stream_volume(dsound_mixer *mixer, uint32_t stream_key);

/* T1148 opt-in global submix headroom. Configure before any emitted PCM;
 * original constructor defaults bins0..30=1, bin31=0. Per-bin effective
 * amount is the low three bits, applied before final stereo saturation. */
bool dsound_mixer_enable_mixbin_headroom(dsound_mixer *mixer, bool enabled);
bool dsound_mixer_bind_mixbin_headroom(dsound_mixer *mixer,uint64_t identity);
bool dsound_mixer_set_mixbin_headroom(dsound_mixer *mixer, uint64_t identity, uint64_t ticks,
                                      uint32_t bin, uint32_t headroom);

/* Ordinary mono buffer voices, separate from queued streaming packets. A Play
 * snapshots decoded samples, restarts at zero and preserves already-scheduled
 * earlier audio. Commands are timestamped so delayed render calls retain their
 * original chronology. Identity includes the verified buffer lease serial.
 * Timing/resampling/stereo routing and speaker gain are INFERRED; this is not
 * an APU or spatial DSP emulator.
 * Loop boundaries are source sample frames; Stop drains from the current cursor
 * to the full data end, matching the existing original-backed transport model.
 * Storage/history limits or commands affecting already-emitted samples
 * fail without changing the existing voice. */
bool dsound_mixer_play_mono_buffer(dsound_mixer *mixer, uint32_t key, uint64_t serial,
                                  uint64_t ticks, uint32_t frequency,
                                  const int16_t *pcm, size_t frames,
                                  bool paused, bool looping, size_t loop_start, size_t loop_end,
                                  int32_t volume);
/* T1524: live SetLoopRegion of a running buffer voice. The cursor so far follows the old region, the new one applies from
 * `ticks` on (horizon rule as for every buffer command). Source sample frames, loop_start < loop_end <= frames. */
bool dsound_mixer_loop_buffer(dsound_mixer *mixer, uint32_t key, uint64_t serial,
                              uint64_t ticks, size_t loop_start, size_t loop_end);
bool dsound_mixer_pause_buffer(dsound_mixer *mixer, uint32_t key, uint64_t serial,
                               uint64_t ticks, bool paused);
const char *dsound_mixer_last_buffer_refusal(const dsound_mixer *mixer);
bool dsound_mixer_stop_buffer(dsound_mixer *mixer, uint32_t key, uint64_t serial,
                              uint64_t ticks);
bool dsound_mixer_frequency_buffer(dsound_mixer *mixer, uint32_t key, uint64_t serial,
                                   uint64_t ticks, uint32_t frequency);
bool dsound_mixer_volume_buffer(dsound_mixer *mixer, uint32_t key, uint64_t serial,
                                uint64_t ticks, int32_t volume);

/* Pause/resume segments are retained so a later render can account for virtual-time gaps. */
/* T1129: original routing/attenuation with timestamped PCM effects. Speaker
 * projection to stereo is INFERRED; DSP/HRTF routes are refused. Lease serial
 * rejects a reused address while earlier PCM still exists. */
bool dsound_mixer_route_stream(dsound_mixer *mixer, uint32_t stream_key,
                               uint64_t serial, uint64_t ticks,
                               const dsound_stream_routing *routing);
bool dsound_mixer_set_stream_running(dsound_mixer *mixer, uint32_t stream_key,
                                     uint64_t now_ticks, bool running);

/*
 * Emit interleaved stereo PCM for all mixer timeline sample points before
 * now_ticks, up to frame_capacity. A later call with the same clock time
 * continues from the previous output frame. Zero fills gaps and saturated
 * integer addition mixes overlapping streams.
 */
bool dsound_mixer_render_until(dsound_mixer *mixer, uint64_t now_ticks, int16_t *output,
                               size_t frame_capacity, size_t *frames_written);

dsound_mixer_counts dsound_mixer_get_counts(const dsound_mixer *mixer);

/* T1245: verified lease; abort pending packets at the format event. */
bool dsound_mixer_format_stream(dsound_mixer *mixer,uint32_t key,uint64_t serial,
    uint64_t ticks,uint32_t old_hz,uint32_t new_hz);
#endif
