/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_AUDIO_DSOUND_ADPCM_H
#define TSFP_AUDIO_DSOUND_ADPCM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    DSOUND_XBOX_ADPCM_CHANNELS = 2,
    DSOUND_XBOX_ADPCM_BLOCK_BYTES = 72,
    DSOUND_XBOX_ADPCM_FRAMES_PER_BLOCK = 64
};
enum { DSOUND_XBOX_ADPCM_MONO_BLOCK_BYTES = 36 };

/*
 * Decode whole stereo Xbox ADPCM blocks into interleaved signed 16-bit PCM.
 *
 * Each 72-byte block has two little-endian predictor/index headers, followed
 * by 4-byte groups interleaved by channel. The declared 64 samples per block
 * are the predictor plus the first 63 decoded nibbles; the final nibble is
 * padding for this format's sample count. This matches the public IMA-Xbox
 * decoder layout, but has not been compared with a lifted retail decoder.
 *
 * The function refuses partial blocks, invalid step indexes, insufficient
 * output capacity and invalid pointers before writing any output.
 */
bool dsound_adpcm_xbox_decode_stereo(const uint8_t *encoded, size_t encoded_bytes,
                                     int16_t *pcm, size_t pcm_frame_capacity,
                                     size_t *frames_written);
/* Ordinary admitted buffers use mono 36-byte blocks; each yields 64 samples.
 * The same all-input validation and unchanged-output refusal apply. */
bool dsound_adpcm_xbox_decode_mono(const uint8_t *encoded, size_t encoded_bytes,
                                  int16_t *pcm, size_t pcm_frame_capacity,
                                  size_t *frames_written);

#endif
