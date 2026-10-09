/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_AUDIO_DSOUND_AUDIO_RUNTIME_H
#define TSFP_AUDIO_DSOUND_AUDIO_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "dsound_stream_routing.h"

bool dsound_audio_runtime_start(uint32_t output_rate_hz, uint64_t ticks_per_second);
void dsound_audio_runtime_stop(void);
void dsound_audio_runtime_reset_buffers(void);
bool dsound_audio_runtime_active(void);
uint64_t dsound_audio_runtime_clock_frequency(void);
bool dsound_audio_runtime_submit(uint32_t stream_key, uint64_t now_ticks, uint32_t source_rate_hz,
                                 const uint8_t *encoded, size_t encoded_bytes);
bool dsound_audio_runtime_submit_mono(uint32_t stream_key, uint64_t now_ticks, uint32_t source_rate_hz,
                                      const uint8_t *encoded, size_t encoded_bytes);
bool dsound_audio_runtime_submit_pcm16_stereo(uint32_t stream_key, uint64_t now_ticks,
                                              uint32_t source_rate_hz, const int16_t *pcm,
                                              size_t frames);
bool dsound_audio_runtime_cut_stream(uint32_t stream_key, uint64_t now_ticks);
bool dsound_audio_runtime_set_stream_running(uint32_t stream_key, uint64_t now_ticks,
                                             bool running);
bool dsound_audio_runtime_set_stream_frequency(uint32_t stream_key, uint64_t ticks,
                                               uint32_t source_rate_hz);
bool dsound_audio_runtime_frequency_stream(uint32_t key,uint64_t serial,uint64_t ticks,
    uint32_t base_hz,uint32_t old_hz,uint32_t new_hz);
bool dsound_audio_runtime_set_stream_volume(uint32_t stream_key, int32_t millibels);
bool dsound_audio_runtime_reset_stream_volume(uint32_t stream_key);
bool dsound_audio_runtime_route_stream(uint32_t key, uint64_t serial, uint64_t ticks,
                                       const dsound_stream_routing *routing);
bool dsound_audio_runtime_render(uint64_t now_ticks, int16_t *output, size_t frame_capacity,
                                 size_t *frames_written);

#define DSOUND_AUDIO_BUFFER_MAX_BYTES (36u * 32768u)
/* Original-backed ordinary buffer transport with real mono ADPCM samples;
 * verified buffer lease serial distinguishes recycled guest addresses.
 * Timing/resampling remains INFERRED under the existing opt-in audio runtime. */
bool dsound_audio_runtime_play_buffer(uint32_t key, uint64_t serial, uint64_t ticks,
                                      uint32_t frequency, const uint8_t *encoded, size_t bytes,
                                      bool paused, bool looping, size_t loop_start, size_t loop_end,
                                      int32_t volume);
bool dsound_audio_runtime_pause_buffer(uint32_t key, uint64_t serial, uint64_t ticks, bool paused);
bool dsound_audio_runtime_stop_buffer(uint32_t key, uint64_t serial, uint64_t ticks);
/* T1216: the mixer's reason for its last refused buffer command (a static string, for the stop text). */
const char *dsound_audio_runtime_last_buffer_refusal(void);
bool dsound_audio_runtime_frequency_buffer(uint32_t key, uint64_t serial, uint64_t ticks,
                                           uint32_t frequency);
bool dsound_audio_runtime_volume_buffer(uint32_t key, uint64_t serial, uint64_t ticks, int32_t volume);
/* T1524: live loop region of a running buffer voice, source sample frames. */
bool dsound_audio_runtime_loop_buffer(uint32_t key, uint64_t serial, uint64_t ticks,
                                      size_t loop_start, size_t loop_end);

bool dsound_audio_runtime_enable_mixbin_headroom(bool enabled);
bool dsound_audio_runtime_mixbin_headroom(uint64_t identity, uint64_t ticks,
                                          uint32_t bin, uint32_t headroom);
bool dsound_audio_runtime_bind_mixbin_headroom(uint64_t identity);
/* T1245: verified lease; abort pending packets at the format event. */
bool dsound_audio_runtime_format_stream(uint32_t key,uint64_t serial,uint64_t ticks,
    uint32_t old_hz,uint32_t new_hz);
#endif
