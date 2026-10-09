/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio_sink_sdl.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

static bool s_mute;

void audio_sink_sdl_set_mute(bool mute)
{
    s_mute = mute;
}

typedef struct {
    present_audio_sink *sink;
    SDL_AudioStream *stream;
    uint16_t channels;
    int16_t *scratch;
    size_t scratch_frames;
    int device_frames; /* T1235: the device's own buffer in frames (SDL_GetAudioDeviceFormat), the latency below the ring */
} sdl_device;

/* Pull model: SDL asks for `additional` bytes, we hand over what the timeline produced, silence for
 * the rest (counted as an underrun by the sink). */
static void SDLCALL device_callback(void *user, SDL_AudioStream *stream, int additional, int total)
{
    (void)total;
    sdl_device *device = user;
    if (additional <= 0) {
        return;
    }
    const size_t frame_bytes = device->channels * sizeof(int16_t);
    size_t frames = (size_t)additional / frame_bytes;
    if (frames == 0u) {
        return;
    }
    if (frames > device->scratch_frames) {
        int16_t *grown = realloc(device->scratch, frames * frame_bytes);
        if (grown == NULL) {
            return;
        }
        device->scratch = grown;
        device->scratch_frames = frames;
    }
    /* Muted: still pull, so the ring drains and the audio clock stays the master, then hand over silence. */
    present_audio_sink_pull(device->sink, device->scratch, frames);
    if (s_mute) {
        memset(device->scratch, 0, frames * frame_bytes);
    }
    SDL_PutAudioStreamData(stream, device->scratch, (int)(frames * frame_bytes));
}

bool audio_sink_sdl_start(present_audio_sink *sink, uint32_t sample_rate, uint16_t channels,
                          void **out_device, const char **error)
{
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        *error = SDL_GetError();
        return false;
    }
    sdl_device *device = calloc(1, sizeof *device);
    if (device == NULL) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        *error = "out of memory";
        return false;
    }
    device->sink = sink;
    device->channels = channels;
    SDL_AudioSpec spec = {.format = SDL_AUDIO_S16LE, .channels = channels, .freq = (int)sample_rate};
    device->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, device_callback, device);
    if (device->stream == NULL) {
        *error = SDL_GetError();
        free(device);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    if (!SDL_ResumeAudioStreamDevice(device->stream)) {
        *error = SDL_GetError();
        SDL_DestroyAudioStream(device->stream);
        free(device);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    SDL_AudioSpec device_spec;
    int device_frames = 0;
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(device->stream), &device_spec, &device_frames)) {
        device->device_frames = device_frames;
    }
    *out_device = device;
    return true;
}

void audio_sink_sdl_stop(void *opaque)
{
    sdl_device *device = opaque;
    if (device == NULL) {
        return;
    }
    SDL_DestroyAudioStream(device->stream); /* joins the callback before the ring goes away */
    free(device->scratch);
    free(device);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

size_t audio_sink_sdl_device_frames(void *opaque)
{
    const sdl_device *device = opaque;
    return device != NULL && device->device_frames > 0 ? (size_t)device->device_frames : 0u;
}
