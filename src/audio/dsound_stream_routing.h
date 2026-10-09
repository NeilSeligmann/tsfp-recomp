/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_AUDIO_DSOUND_STREAM_ROUTING_H
#define TSFP_AUDIO_DSOUND_STREAM_ROUTING_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Original settings +20 headroom, +1C aggregate volume, +24 route count,
 * +28 ordered byte bins, +30 32 DWORD bin volumes. Stereo voice grouping is
 * floor(route_count/2). Checked adapter scope: <=8 routes, speaker bins0..5;
 * DSP/HRTF bins are not silently redirected. All arithmetic is modulo32. */
#define DSOUND_STREAM_ROUTE_SLOTS 8u
#define DSOUND_STREAM_ROUTE_BINS 32u

typedef struct dsound_stream_routing {
    uint32_t volume, headroom, aggregate, count;
    uint8_t bins[DSOUND_STREAM_ROUTE_SLOTS];
    uint32_t bin_volume[DSOUND_STREAM_ROUTE_BINS];
} dsound_stream_routing;

typedef struct dsound_stream_route_pair {
    uint32_t bin, volume;
} dsound_stream_route_pair;

static inline void dsound_stream_routing_init(dsound_stream_routing *state)
{
    memset(state, 0, sizeof(*state));
    state->headroom = 600u;
    state->aggregate = 0u - state->headroom;
    state->count = 2u;
    state->bins[1] = 1u;
}

static inline void dsound_stream_routing_volume(dsound_stream_routing *state,
                                                uint32_t volume)
{
    state->aggregate += volume - state->volume;
    state->volume = volume;
}

static inline void dsound_stream_routing_headroom(dsound_stream_routing *state,
                                                  uint32_t headroom)
{
    state->aggregate += state->headroom - headroom;
    state->headroom = headroom;
}

/* SetMixBins uses a BYTE bin identity in both route and indexed volume write.
 * Duplicate bins keep every route slot but share the last-written volume.
 * Empty input leaves earlier volume entries untouched. NULL restores stereo
 * defaults0/1, including their volumes0, and preserves other volume entries. */
static inline bool dsound_stream_routing_bins(dsound_stream_routing *state,
                                               const dsound_stream_route_pair *pairs,
                                               uint32_t count, bool defaults)
{
    const dsound_stream_route_pair normal[2] = {{0u, 0u}, {1u, 0u}};
    if (defaults) { pairs = normal; count = 2u; }
    if (count > DSOUND_STREAM_ROUTE_SLOTS || (count != 0u && pairs == NULL)) return false;
    for (uint32_t i = 0u; i < count; i++)
        if ((uint8_t)pairs[i].bin > 5u) return false;
    dsound_stream_routing candidate = *state;
    for (uint32_t i = 0u; i < count; i++) {
        const uint8_t bin = (uint8_t)pairs[i].bin;
        candidate.bins[i] = bin;
        candidate.bin_volume[bin] = pairs[i].volume;
    }
    candidate.count = count;
    *state = candidate;
    return true;
}

/* SetMixBinVolumes uses the full DWORD bin, unlike SetMixBins. */
static inline bool dsound_stream_routing_volumes(dsound_stream_routing *state,
                                                  const dsound_stream_route_pair *pairs,
                                                  uint32_t count)
{
    if (count > DSOUND_STREAM_ROUTE_BINS || (count != 0u && pairs == NULL)) return false;
    for (uint32_t i = 0u; i < count; i++)
        if (pairs[i].bin >= DSOUND_STREAM_ROUTE_BINS) return false;
    dsound_stream_routing candidate = *state;
    for (uint32_t i = 0u; i < count; i++) candidate.bin_volume[pairs[i].bin] = pairs[i].volume;
    *state = candidate;
    return true;
}

/* Original40CBC0: NEG, SHL6 (32-bit wrap), unsigned DIV100, cap0FFF.
 * xemu vp/vp.c volume0FFF means silence, not the finite63.984dB gain. */
static inline uint16_t dsound_stream_routing_attenuation(const dsound_stream_routing *state,
                                                         uint32_t slot)
{
    if (slot >= state->count || slot >= DSOUND_STREAM_ROUTE_SLOTS ||
        state->bins[slot] >= DSOUND_STREAM_ROUTE_BINS) return 4095u;
    uint32_t value = 0u - (state->aggregate + state->bin_volume[state->bins[slot]]);
    value = (value << 6u) / 100u;
    return (uint16_t)(value > 4095u ? 4095u : value);
}
#endif
