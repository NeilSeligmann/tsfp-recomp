/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T1250: WSOLA time stretch, see audio_stretch.h. */
#include "audio_stretch.h"

#include <stdlib.h>
#include <string.h>

struct audio_stretch {
    uint16_t channels;
    size_t overlap;
    size_t search;
    bool primed;
    int16_t *tail;    /* overlap frames: the natural continuation of the last segment */
    int16_t *segment; /* 2 * overlap frames: the last segment, looped by a hold block */
};

audio_stretch *audio_stretch_create(uint16_t channels, size_t overlap)
{
    if (channels == 0u || overlap < 4u) {
        return NULL;
    }
    audio_stretch *engine = calloc(1u, sizeof *engine);
    if (engine == NULL) {
        return NULL;
    }
    engine->channels = channels;
    engine->overlap = overlap;
    engine->search = overlap / 2u;
    engine->tail = calloc(overlap * channels, sizeof *engine->tail);
    engine->segment = calloc(2u * overlap * channels, sizeof *engine->segment);
    if (engine->tail == NULL || engine->segment == NULL) {
        audio_stretch_destroy(engine);
        return NULL;
    }
    return engine;
}

void audio_stretch_destroy(audio_stretch *engine)
{
    if (engine == NULL) {
        return;
    }
    free(engine->tail);
    free(engine->segment);
    free(engine);
}

size_t audio_stretch_overlap(const audio_stretch *engine)
{
    return engine->overlap;
}

size_t audio_stretch_search(const audio_stretch *engine)
{
    return engine->search;
}

size_t audio_stretch_input_needed(const audio_stretch *engine, size_t nominal)
{
    return nominal + engine->search + 2u * engine->overlap;
}

bool audio_stretch_primed(const audio_stretch *engine)
{
    return engine->primed;
}

bool audio_stretch_prime(audio_stretch *engine, const int16_t *in, size_t in_frames, int16_t *out)
{
    const size_t block = engine->overlap * engine->channels;
    if (in_frames < 2u * engine->overlap) {
        return false;
    }
    memcpy(engine->segment, in, 2u * block * sizeof *in);
    memcpy(engine->tail, in + block, block * sizeof *in);
    memcpy(out, in, block * sizeof *in);
    engine->primed = true;
    return true;
}

/* out = tail faded out, `source` faded in (source may be NULL: silence). Linear ramps: the two are correlated. */
static void crossfade(const audio_stretch *engine, const int16_t *source, int16_t *out)
{
    const size_t overlap = engine->overlap;
    for (size_t frame = 0u; frame < overlap; frame++) {
        const int32_t weight = (int32_t)((frame + 1u) * 65536u / (overlap + 1u));
        for (uint16_t channel = 0u; channel < engine->channels; channel++) {
            const size_t index = frame * engine->channels + channel;
            const int32_t old_sample = engine->tail[index];
            const int32_t new_sample = source != NULL ? source[index] : 0;
            out[index] = (int16_t)(old_sample + (int32_t)(((int64_t)(new_sample - old_sample) * weight) >> 16));
        }
    }
}

static int32_t mono(const int16_t *frame, uint16_t channels)
{
    int32_t sum = 0;
    for (uint16_t channel = 0u; channel < channels; channel++) {
        sum += frame[channel];
    }
    return sum;
}

bool audio_stretch_next(audio_stretch *engine, const int16_t *in, size_t in_frames, size_t nominal, int16_t *out,
                        size_t *chosen_start)
{
    const size_t overlap = engine->overlap;
    const size_t search = engine->search;
    const uint16_t channels = engine->channels;
    if (!engine->primed || nominal + 2u * overlap > in_frames) {
        return false;
    }
    const size_t first = nominal > search ? nominal - search : 0u;
    size_t last = nominal + search;
    if (last + 2u * overlap > in_frames) {
        last = in_frames - 2u * overlap;
    }
    if (last < first) {
        return false;
    }
    /* Normalised cross correlation of the tail with each candidate, mono mix. The nominal offset wins ties and near
     * ties so an already continuous signal (ratio 1) is passed through bit exactly. */
    size_t best = nominal < first ? first : nominal > last ? last : nominal;
    double best_score = -1.0e300;
    for (size_t start = first; start <= last; start++) {
        double correlation = 0.0;
        double energy = 1.0;
        for (size_t frame = 0u; frame < overlap; frame++) {
            const int32_t candidate = mono(in + (start + frame) * channels, channels);
            correlation += (double)mono(engine->tail + frame * channels, channels) * (double)candidate;
            energy += (double)candidate * (double)candidate;
        }
        /* sign(c) * c^2 / energy orders candidates like c / sqrt(energy) without a square root */
        double score = correlation * (correlation < 0.0 ? -correlation : correlation) / energy;
        if (start == nominal) {
            score += (score < 0.0 ? -score : score) * 1.0e-3;
        }
        if (score > best_score) {
            best_score = score;
            best = start;
        }
    }
    const int16_t *segment = in + best * channels;
    crossfade(engine, segment, out);
    memcpy(engine->segment, segment, 2u * overlap * channels * sizeof *segment);
    memcpy(engine->tail, segment + overlap * channels, overlap * channels * sizeof *segment);
    if (chosen_start != NULL) {
        *chosen_start = best;
    }
    return true;
}

bool audio_stretch_hold(audio_stretch *engine, int16_t *out)
{
    if (!engine->primed) {
        return false;
    }
    const size_t block = engine->overlap * engine->channels;
    crossfade(engine, engine->segment, out);
    memcpy(engine->tail, engine->segment + block, block * sizeof *engine->tail);
    return true;
}

bool audio_stretch_leave(audio_stretch *engine, const int16_t *in, size_t in_frames, int16_t *out)
{
    if (!engine->primed || in_frames < engine->overlap) {
        return false;
    }
    crossfade(engine, in, out);
    engine->primed = false;
    return true;
}

bool audio_stretch_fade_out(audio_stretch *engine, int16_t *out)
{
    if (!engine->primed) {
        return false;
    }
    crossfade(engine, NULL, out);
    engine->primed = false;
    return true;
}
