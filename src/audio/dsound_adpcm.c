/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsound_adpcm.h"

#include <limits.h>

static const int16_t step_table[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,
    23,    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,
    73,    80,    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,
    230,   253,   279,   307,   337,   371,   408,   449,   494,   544,   598,   658,
    724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
    7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767,
};

static const int8_t index_adjust[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8,
};

static int16_t read_s16le(const uint8_t *bytes)
{
    const uint16_t word = (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u));
    return (int16_t)word;
}

static int16_t decode_nibble(int16_t *predictor, int16_t *step_index, uint8_t nibble)
{
    const int32_t step = step_table[(unsigned)*step_index];
    int32_t difference = step >> 3u;
    if ((nibble & 1u) != 0u) difference += step >> 2u;
    if ((nibble & 2u) != 0u) difference += step >> 1u;
    if ((nibble & 4u) != 0u) difference += step;
    if ((nibble & 8u) != 0u) difference = -difference;

    int32_t sample = (int32_t)*predictor + difference;
    if (sample > INT16_MAX) sample = INT16_MAX;
    if (sample < INT16_MIN) sample = INT16_MIN;
    *predictor = (int16_t)sample;

    int32_t next_index = (int32_t)*step_index + index_adjust[nibble & 0x0fu];
    if (next_index < 0) next_index = 0;
    if (next_index > 88) next_index = 88;
    *step_index = (int16_t)next_index;
    return *predictor;
}

static bool block_headers_valid(const uint8_t *block, unsigned channels)
{
    for (unsigned channel = 0u; channel < channels; channel++) {
        const int16_t index = read_s16le(block + channel * 4u + 2u);
        if (index < 0 || index > 88) return false;
    }
    return true;
}

static bool decode(const uint8_t *encoded, size_t encoded_bytes,
                   int16_t *pcm, size_t pcm_frame_capacity,
                   size_t *frames_written, unsigned channels)
{
    const size_t block_bytes = (size_t)channels * DSOUND_XBOX_ADPCM_MONO_BLOCK_BYTES;
    if (frames_written == NULL || (encoded_bytes != 0u && encoded == NULL) ||
        (pcm_frame_capacity != 0u && pcm == NULL) ||
        encoded_bytes % block_bytes != 0u)
        return false;

    const size_t blocks = encoded_bytes / block_bytes;
    if (blocks > SIZE_MAX / DSOUND_XBOX_ADPCM_FRAMES_PER_BLOCK ||
        blocks * DSOUND_XBOX_ADPCM_FRAMES_PER_BLOCK > pcm_frame_capacity)
        return false;
    for (size_t block_index = 0u; block_index < blocks; block_index++) {
        if (!block_headers_valid(encoded + block_index * block_bytes, channels))
            return false;
    }

    for (size_t block_index = 0u; block_index < blocks; block_index++) {
        const uint8_t *block = encoded + block_index * block_bytes;
        int16_t predictor[DSOUND_XBOX_ADPCM_CHANNELS];
        int16_t step_index[DSOUND_XBOX_ADPCM_CHANNELS];
        const size_t frame_base = block_index * DSOUND_XBOX_ADPCM_FRAMES_PER_BLOCK;
        for (unsigned channel = 0u; channel < channels; channel++) {
            predictor[channel] = read_s16le(block + channel * 4u);
            step_index[channel] = read_s16le(block + channel * 4u + 2u);
            pcm[(size_t)channels * frame_base + channel] = predictor[channel];
        }

        size_t input = (size_t)channels * 4u;
        for (size_t group = 0u; group < 8u; group++) {
            for (unsigned channel = 0u; channel < channels; channel++) {
                for (unsigned byte_index = 0u; byte_index < 4u; byte_index++) {
                    const uint8_t packed = block[input++];
                    const size_t sample_base = group * 8u + byte_index * 2u;
                    const int16_t low =
                        decode_nibble(&predictor[channel], &step_index[channel], packed & 0x0fu);
                    if (sample_base < DSOUND_XBOX_ADPCM_FRAMES_PER_BLOCK - 1u) {
                        const size_t frame = frame_base + 1u + sample_base;
                        pcm[(size_t)channels * frame + channel] = low;
                    }
                    const int16_t high = decode_nibble(&predictor[channel], &step_index[channel],
                                                       (uint8_t)(packed >> 4u));
                    if (sample_base + 1u < DSOUND_XBOX_ADPCM_FRAMES_PER_BLOCK - 1u) {
                        const size_t frame = frame_base + 2u + sample_base;
                        pcm[(size_t)channels * frame + channel] = high;
                    }
                }
            }
        }
    }
    *frames_written = blocks * DSOUND_XBOX_ADPCM_FRAMES_PER_BLOCK;
    return true;
}

bool dsound_adpcm_xbox_decode_stereo(const uint8_t *encoded, size_t encoded_bytes,
                                     int16_t *pcm, size_t pcm_frame_capacity,
                                     size_t *frames_written)
{
    return decode(encoded, encoded_bytes, pcm, pcm_frame_capacity, frames_written, 2u);
}

bool dsound_adpcm_xbox_decode_mono(const uint8_t *encoded, size_t encoded_bytes,
                                  int16_t *pcm, size_t pcm_frame_capacity,
                                  size_t *frames_written)
{
    return decode(encoded, encoded_bytes, pcm, pcm_frame_capacity, frames_written, 1u);
}
