/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * T1250: pitch preserving time stretch (WSOLA, waveform similarity overlap add) for the audio sink's slowed playback.
 * Private to src/host. The engine works on blocks of `overlap` output frames. Each block crossfades the saved natural
 * continuation of the previous segment (the tail) with the next segment, which is searched around its nominal start for
 * the offset whose waveform best continues the tail. Playing the nominal starts at ratio < 1 of the output rate slows the
 * audio without lowering its pitch. A hold block loops the last segment instead (no input needed, the ring is too thin).
 * The engine never touches the sink's ring: the caller hands it a linear window of input frames (interleaved int16).
 */
#ifndef TSFP_AUDIO_STRETCH_H
#define TSFP_AUDIO_STRETCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct audio_stretch audio_stretch;

/* Allocates an engine for `channels` and an output block of `overlap` frames (the search range is overlap / 2). */
audio_stretch *audio_stretch_create(uint16_t channels, size_t overlap);
void audio_stretch_destroy(audio_stretch *engine);
size_t audio_stretch_overlap(const audio_stretch *engine);
size_t audio_stretch_search(const audio_stretch *engine);
/* Input frames a block needs ahead of the window start when its nominal start is `nominal`: nominal + search + 2 * overlap. */
size_t audio_stretch_input_needed(const audio_stretch *engine, size_t nominal);
bool audio_stretch_primed(const audio_stretch *engine);
/* Starts the engine on in[0 .. 2 * overlap): emits in[0 .. overlap) unchanged into out, keeps the rest as the tail.
 * Needs 2 * overlap input frames, false otherwise. */
bool audio_stretch_prime(audio_stretch *engine, const int16_t *in, size_t in_frames, int16_t *out);
/* One block at `nominal` (a frame index into in): picks the best offset within the search range, crossfades the tail
 * into it, emits `overlap` frames into out and stores the offset-adjusted start in *chosen_start. False (nothing
 * emitted) when the input is too short or the engine is not primed. */
bool audio_stretch_next(audio_stretch *engine, const int16_t *in, size_t in_frames, size_t nominal, int16_t *out,
                        size_t *chosen_start);
/* One block from the stored segment alone (a loop): used while the input is too thin. False when not primed. */
bool audio_stretch_hold(audio_stretch *engine, int16_t *out);
/* Ends stretching: crossfades the tail into in[0 .. overlap) (the caller aligns in with the tail's natural source) and
 * emits it. The engine is unprimed afterwards. */
bool audio_stretch_leave(audio_stretch *engine, const int16_t *in, size_t in_frames, int16_t *out);
/* Ends stretching into silence: the tail faded out over one block. The engine is unprimed afterwards. */
bool audio_stretch_fade_out(audio_stretch *engine, int16_t *out);

#endif
