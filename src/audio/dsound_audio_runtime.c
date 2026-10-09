/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsound_audio_runtime.h"

#include "dsound_mixer.h"
#include "dsound_adpcm.h"

#include <pthread.h>
#include <stdlib.h>

static dsound_mixer *runtime_mixer;
static uint64_t runtime_clock_frequency;
static pthread_mutex_t runtime_lock = PTHREAD_MUTEX_INITIALIZER;

void dsound_audio_runtime_reset_buffers(void)
{
    pthread_mutex_lock(&runtime_lock);
    dsound_mixer_reset_buffers(runtime_mixer);
    pthread_mutex_unlock(&runtime_lock);
}

bool dsound_audio_runtime_start(uint32_t output_rate_hz, uint64_t ticks_per_second)
{
    pthread_mutex_lock(&runtime_lock);
    if (runtime_mixer != NULL) {
        pthread_mutex_unlock(&runtime_lock);
        return false;
    }
    runtime_mixer = dsound_mixer_create(output_rate_hz, ticks_per_second);
    const bool ok = runtime_mixer != NULL;
    if(ok)runtime_clock_frequency=ticks_per_second;
    pthread_mutex_unlock(&runtime_lock);
    return ok;
}

bool dsound_audio_runtime_play_buffer(uint32_t key, uint64_t serial, uint64_t ticks,
                                      uint32_t frequency, const uint8_t *encoded, size_t bytes,
                                      bool paused, bool looping, size_t loop_start, size_t loop_end,
                                      int32_t volume)
{
    if (bytes == 0u || bytes > DSOUND_AUDIO_BUFFER_MAX_BYTES ||
        bytes % DSOUND_XBOX_ADPCM_MONO_BLOCK_BYTES != 0u) return false;
    const size_t frames = bytes / DSOUND_XBOX_ADPCM_MONO_BLOCK_BYTES * DSOUND_XBOX_ADPCM_FRAMES_PER_BLOCK;
    int16_t *pcm = malloc(frames * sizeof(*pcm));
    if (pcm == NULL) return false;
    size_t decoded = 0u;
    const bool valid = dsound_adpcm_xbox_decode_mono(encoded, bytes, pcm, frames, &decoded);
    pthread_mutex_lock(&runtime_lock);
    const bool accepted = valid && runtime_mixer != NULL && decoded == frames &&
        dsound_mixer_play_mono_buffer(runtime_mixer, key, serial, ticks, frequency, pcm, frames,
                                      paused, looping, loop_start, loop_end, volume);
    pthread_mutex_unlock(&runtime_lock);
    free(pcm);
    return accepted;
}

bool dsound_audio_runtime_pause_buffer(uint32_t key, uint64_t serial, uint64_t ticks, bool paused)
{
    pthread_mutex_lock(&runtime_lock);
    const bool accepted = runtime_mixer != NULL &&
        dsound_mixer_pause_buffer(runtime_mixer, key, serial, ticks, paused);
    pthread_mutex_unlock(&runtime_lock);
    return accepted;
}
const char *dsound_audio_runtime_last_buffer_refusal(void)
{
    pthread_mutex_lock(&runtime_lock);
    const char *reason = dsound_mixer_last_buffer_refusal(runtime_mixer);
    pthread_mutex_unlock(&runtime_lock);
    return reason;
}
bool dsound_audio_runtime_stop_buffer(uint32_t key, uint64_t serial, uint64_t ticks)
{
    pthread_mutex_lock(&runtime_lock);
    const bool accepted = runtime_mixer != NULL && dsound_mixer_stop_buffer(runtime_mixer, key, serial, ticks);
    pthread_mutex_unlock(&runtime_lock);
    return accepted;
}
bool dsound_audio_runtime_frequency_buffer(uint32_t key, uint64_t serial, uint64_t ticks,
                                           uint32_t frequency)
{
    pthread_mutex_lock(&runtime_lock);
    const bool accepted = runtime_mixer != NULL &&
        dsound_mixer_frequency_buffer(runtime_mixer, key, serial, ticks, frequency);
    pthread_mutex_unlock(&runtime_lock);
    return accepted;
}
bool dsound_audio_runtime_loop_buffer(uint32_t key, uint64_t serial, uint64_t ticks,
                                      size_t loop_start, size_t loop_end)
{
    pthread_mutex_lock(&runtime_lock);
    const bool accepted = runtime_mixer != NULL &&
        dsound_mixer_loop_buffer(runtime_mixer, key, serial, ticks, loop_start, loop_end);
    pthread_mutex_unlock(&runtime_lock);
    return accepted;
}
bool dsound_audio_runtime_volume_buffer(uint32_t key, uint64_t serial, uint64_t ticks, int32_t volume)
{
    pthread_mutex_lock(&runtime_lock);
    const bool accepted = runtime_mixer != NULL &&
        dsound_mixer_volume_buffer(runtime_mixer, key, serial, ticks, volume);
    pthread_mutex_unlock(&runtime_lock);
    return accepted;
}

void dsound_audio_runtime_stop(void)
{
    pthread_mutex_lock(&runtime_lock);
    dsound_mixer_destroy(runtime_mixer);
    runtime_mixer = NULL;
    runtime_clock_frequency = 0u;
    pthread_mutex_unlock(&runtime_lock);
}

bool dsound_audio_runtime_active(void)
{
    pthread_mutex_lock(&runtime_lock);
    const bool active = runtime_mixer != NULL;
    pthread_mutex_unlock(&runtime_lock);
    return active;
}
uint64_t dsound_audio_runtime_clock_frequency(void)
{
    pthread_mutex_lock(&runtime_lock);
    const uint64_t frequency=runtime_mixer!=NULL?runtime_clock_frequency:0u;
    pthread_mutex_unlock(&runtime_lock);
    return frequency;
}

bool dsound_audio_runtime_submit(uint32_t stream_key, uint64_t now_ticks, uint32_t source_rate_hz,
                                 const uint8_t *encoded, size_t encoded_bytes)
{
    pthread_mutex_lock(&runtime_lock);
    const bool submitted =
        runtime_mixer != NULL &&
        dsound_mixer_submit_xbox_adpcm(runtime_mixer, stream_key, now_ticks, source_rate_hz,
                                       encoded, encoded_bytes);
    pthread_mutex_unlock(&runtime_lock);
    return submitted;
}

bool dsound_audio_runtime_submit_mono(uint32_t stream_key, uint64_t now_ticks, uint32_t source_rate_hz,
                                      const uint8_t *encoded, size_t encoded_bytes)
{
    pthread_mutex_lock(&runtime_lock);
    const bool submitted =
        runtime_mixer != NULL &&
        dsound_mixer_submit_xbox_adpcm_mono(runtime_mixer, stream_key, now_ticks, source_rate_hz,
                                            encoded, encoded_bytes);
    pthread_mutex_unlock(&runtime_lock);
    return submitted;
}

bool dsound_audio_runtime_submit_pcm16_stereo(uint32_t stream_key, uint64_t now_ticks,
                                              uint32_t source_rate_hz, const int16_t *pcm,
                                              size_t frames)
{
    pthread_mutex_lock(&runtime_lock);
    const bool submitted = runtime_mixer != NULL &&
        dsound_mixer_submit_pcm16_stereo(runtime_mixer, stream_key, now_ticks, source_rate_hz, pcm, frames);
    pthread_mutex_unlock(&runtime_lock);
    return submitted;
}

bool dsound_audio_runtime_cut_stream(uint32_t stream_key, uint64_t now_ticks)
{
    pthread_mutex_lock(&runtime_lock);
    const bool cut = runtime_mixer != NULL && dsound_mixer_cut_stream(runtime_mixer, stream_key, now_ticks);
    pthread_mutex_unlock(&runtime_lock);
    return cut;
}

bool dsound_audio_runtime_set_stream_volume(uint32_t stream_key, int32_t millibels)
{
    pthread_mutex_lock(&runtime_lock);
    const bool updated = runtime_mixer != NULL && dsound_mixer_set_stream_volume(runtime_mixer, stream_key, millibels);
    pthread_mutex_unlock(&runtime_lock);
    return updated;
}

bool dsound_audio_runtime_reset_stream_volume(uint32_t stream_key)
{
    pthread_mutex_lock(&runtime_lock);
    const bool reset = runtime_mixer != NULL &&
                       dsound_mixer_reset_stream_volume(runtime_mixer, stream_key);
    pthread_mutex_unlock(&runtime_lock);
    return reset;
}

bool dsound_audio_runtime_set_stream_running(uint32_t stream_key, uint64_t now_ticks,
                                              bool running)
{
    pthread_mutex_lock(&runtime_lock);
    const bool updated = runtime_mixer != NULL &&
                         dsound_mixer_set_stream_running(runtime_mixer, stream_key, now_ticks,
                                                         running);
    pthread_mutex_unlock(&runtime_lock);
    return updated;
}

bool dsound_audio_runtime_set_stream_frequency(uint32_t stream_key, uint64_t ticks,
                                               uint32_t source_rate_hz)
{
    pthread_mutex_lock(&runtime_lock);
    const bool updated = runtime_mixer != NULL &&
        dsound_mixer_set_stream_frequency(runtime_mixer, stream_key, ticks, source_rate_hz);
    pthread_mutex_unlock(&runtime_lock);
    return updated;
}

bool dsound_audio_runtime_frequency_stream(uint32_t key,uint64_t serial,uint64_t ticks,
    uint32_t base_hz,uint32_t old_hz,uint32_t new_hz)
{
    pthread_mutex_lock(&runtime_lock);
    const bool accepted=dsound_mixer_frequency_stream(runtime_mixer,key,serial,ticks,base_hz,old_hz,new_hz);
    pthread_mutex_unlock(&runtime_lock);return accepted;
}

bool dsound_audio_runtime_format_stream(uint32_t key,uint64_t serial,uint64_t ticks,
    uint32_t old_hz,uint32_t new_hz)
{
    pthread_mutex_lock(&runtime_lock);
    const bool accepted=dsound_mixer_format_stream(runtime_mixer,key,serial,ticks,old_hz,new_hz);
    pthread_mutex_unlock(&runtime_lock);return accepted;
}

bool dsound_audio_runtime_route_stream(uint32_t key, uint64_t serial, uint64_t ticks,
                                       const dsound_stream_routing *routing)
{
    pthread_mutex_lock(&runtime_lock);
    const bool accepted = runtime_mixer != NULL &&
        dsound_mixer_route_stream(runtime_mixer,key,serial,ticks,routing);
    pthread_mutex_unlock(&runtime_lock);
    return accepted;
}

bool dsound_audio_runtime_render(uint64_t now_ticks, int16_t *output, size_t frame_capacity,
                                 size_t *frames_written)
{
    pthread_mutex_lock(&runtime_lock);
    if (runtime_mixer == NULL) {
        if (frames_written != NULL) *frames_written = 0u;
        pthread_mutex_unlock(&runtime_lock);
        return frames_written != NULL;
    }
    const bool rendered = dsound_mixer_render_until(runtime_mixer, now_ticks, output,
                                                   frame_capacity, frames_written);
    pthread_mutex_unlock(&runtime_lock);
    return rendered;
}

bool dsound_audio_runtime_enable_mixbin_headroom(bool enabled)
{
    pthread_mutex_lock(&runtime_lock);
    const bool ok=dsound_mixer_enable_mixbin_headroom(runtime_mixer,enabled);
    pthread_mutex_unlock(&runtime_lock);
    return ok;
}
bool dsound_audio_runtime_mixbin_headroom(uint64_t identity,uint64_t ticks,
                                          uint32_t bin,uint32_t headroom)
{
    pthread_mutex_lock(&runtime_lock);
    const bool ok=dsound_mixer_set_mixbin_headroom(runtime_mixer,identity,ticks,bin,headroom);
    pthread_mutex_unlock(&runtime_lock);
    return ok;
}

bool dsound_audio_runtime_bind_mixbin_headroom(uint64_t identity)
{
    pthread_mutex_lock(&runtime_lock);
    bool ok=dsound_mixer_bind_mixbin_headroom(runtime_mixer,identity);
    pthread_mutex_unlock(&runtime_lock);
    return ok;
}
