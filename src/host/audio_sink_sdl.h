/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * T761: the SDL3 audio device behind present_audio_sink kind sdl. Private to src/host. Compiled only
 * when the build found SDL3 (TSFP_HAVE_SDL3). The device callback only PULLS from the sink's ring
 * (present_audio_sink_pull), it never produces or drives the mix. Without SDL3 start fails with a named
 * reason, so the sdl kind is refused earlier by present_audio_select.
 */
#ifndef TSFP_AUDIO_SINK_SDL_H
#define TSFP_AUDIO_SINK_SDL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "present_sink.h"

#ifdef TSFP_HAVE_SDL3
void audio_sink_sdl_set_mute(bool mute);
bool audio_sink_sdl_start(present_audio_sink *sink, uint32_t sample_rate, uint16_t channels,
                          void **device, const char **error);
void audio_sink_sdl_stop(void *device);
size_t audio_sink_sdl_device_frames(void *device);
#else
static inline void audio_sink_sdl_set_mute(bool mute)
{
    (void)mute;
}
static inline bool audio_sink_sdl_start(present_audio_sink *sink, uint32_t sample_rate,
                                        uint16_t channels, void **device, const char **error)
{
    (void)sink;
    (void)sample_rate;
    (void)channels;
    (void)device;
    *error = "this build has no SDL3";
    return false;
}
static inline void audio_sink_sdl_stop(void *device)
{
    (void)device;
}
static inline size_t audio_sink_sdl_device_frames(void *device)
{
    (void)device;
    return 0u;
}
#endif

#endif
