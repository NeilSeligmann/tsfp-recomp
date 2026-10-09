/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsound_mixer.h"
#include "dsound_audio_runtime.h"

#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;

#define CHECK(condition)                                                                          \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);       \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static void make_block(uint8_t block[72], int16_t left, int16_t right)
{
    memset(block, 0, 72u);
    block[0] = (uint8_t)left;
    block[1] = (uint8_t)((uint16_t)left >> 8u);
    block[4] = (uint8_t)right;
    block[5] = (uint8_t)((uint16_t)right >> 8u);
}

#define MOVIE_TICKS UINT64_C(733333333)
#define SINE_FRAMES 13230u

/* A sine as 44100 Hz stereo S16, by rotation (no libm): c and s are cos and sin of the phase step. */
static void make_sine(int16_t *pcm, double c, double s)
{
    double x = 1.0, y = 0.0;
    for (unsigned i = 0u; i < SINE_FRAMES; i++) {
        const double v = 16000.0 * y;
        pcm[2u * i] = pcm[2u * i + 1u] = (int16_t)(v < 0.0 ? v - 0.5 : v + 0.5);
        const double nx = x * c - y * s;
        y = x * s + y * c;
        x = nx;
    }
}

/* Render the 0.3 s sine as `parts` back to back packets, calling render_until `steps` times. */
static size_t render_sine(const int16_t *pcm, unsigned parts, unsigned steps, int16_t *out)
{
    dsound_mixer *mixer = dsound_mixer_create(48000u, MOVIE_TICKS);
    size_t total = 0u, written = 0u;
    if (mixer == NULL) return 0u;
    for (unsigned part = 0u; part < parts; part++) {
        const size_t frames = SINE_FRAMES / parts;
        if (!dsound_mixer_submit_pcm16_stereo(mixer, 1u, 1000u, 44100u, pcm + 2u * part * frames, frames)) return 0u;
    }
    for (unsigned step = 1u; step <= steps; step++) {
        const uint64_t now = 1000u + MOVIE_TICKS * 3u / 10u * step / steps + (step == steps ? 2u : 0u);
        if (!dsound_mixer_render_until(mixer, now, out + 2u * total, 20000u - total, &written)) return 0u;
        total += written;
    }
    dsound_mixer_destroy(mixer);
    return total;
}

/* Goertzel power of the left channel over n frames from `first`, coefficient = 2 cos(2 pi f / 48000). */
static double goertzel_power(const int16_t *out, size_t first, size_t n, double coefficient)
{
    double s1 = 0.0, s2 = 0.0;
    for (size_t i = 0u; i < n; i++) {
        const double s0 = out[2u * (first + i)] + coefficient * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coefficient * s1 * s2;
}

static int test_resampler(void)
{
    static int16_t pcm[2u * SINE_FRAMES], whole[2u * 20000u], split[2u * 20000u], chunked[2u * 20000u];
    const struct { double c, s; } tones[] = {{0.9898674727799416, 0.14199431795762676},
                                              {-0.5365483771762628, 0.8438695627580834}};
    for (unsigned t = 0u; t < 2u; t++) {
        make_sine(pcm, tones[t].c, tones[t].s);
        const size_t total = render_sine(pcm, 1u, 1u, whole);
        CHECK(total > 14000u);
        CHECK(render_sine(pcm, 3u, 1u, split) == total);
        CHECK(render_sine(pcm, 3u, 2999u, chunked) == total);
        /* Joins are continuous: three packets render the same as one even when the mixer drops finished
         * packets between small render calls. The limit is 1 LSB, 4 for 15 kHz where the 1 tick (1.4 ns)
         * ceil of each packet duration, a phase step of 7e-5 sample, is worth up to 3 LSB of slope. */
        const unsigned limit[] = {1u, 4u};
        unsigned worst_split = 0u, worst_chunked = 0u;
        for (size_t i = 0u; i < total * 2u; i++) {
            const unsigned a = (unsigned)abs(whole[i] - split[i]), b = (unsigned)abs(whole[i] - chunked[i]);
            if (a > worst_split) worst_split = a;
            if (b > worst_chunked) worst_chunked = b;
        }
        CHECK(worst_split <= limit[t] && worst_chunked <= limit[t]);
    }
    /* 19 kHz: the image of 44100 - 19000 = 25100 folds to 22900 Hz. Linear interpolation leaves it near
     * -6 dB, the band limited path must stay below -60 dB (measured -84), and the tone must not droop by
     * more than 0.1 dB (measured 0.00). */
    make_sine(pcm, -0.90705901057569, 0.4210035051320241);
    const size_t total = render_sine(pcm, 3u, 1u, split);
    CHECK(total >= 960u + 9600u);
    const double tone = goertzel_power(split, 960u, 9600u, -1.58670668058247);
    const double image = goertzel_power(split, 960u, 9600u, -1.9793027736393405);
    CHECK(tone > 0.0 && image < tone * 1e-6);
    const double amplitude = 16000.0 * 9600.0 / 2.0;
    CHECK(tone > amplitude * amplitude * 0.9772 && tone < amplitude * amplitude * 1.0233);
    return 0;
}

static int test_ratio_one_unchanged(void)
{
    /* Source rate = output rate = tick rate: the output is the input, bit for bit, across a join. */
    int16_t pcm[2u * 64u], out[2u * 128u];
    size_t written = 0u;
    for (unsigned i = 0u; i < 64u; i++) { pcm[2u * i] = (int16_t)(i * 997 - 20000); pcm[2u * i + 1u] = (int16_t)(20000 - (int)i * 613); }
    dsound_mixer *mixer = dsound_mixer_create(48000u, 48000u);
    CHECK(mixer != NULL);
    CHECK(dsound_mixer_submit_pcm16_stereo(mixer, 1u, 0u, 48000u, pcm, 32u));
    CHECK(dsound_mixer_submit_pcm16_stereo(mixer, 1u, 0u, 48000u, pcm + 64u, 32u));
    CHECK(dsound_mixer_render_until(mixer, 64u, out, 128u, &written) && written == 64u);
    CHECK(memcmp(out, pcm, 64u * 2u * sizeof(int16_t)) == 0);
    dsound_mixer_destroy(mixer);
    return 0;
}

static int test_reset_volume_is_existing_only_and_stream_local(void)
{
    int16_t pcm_a[64], pcm_b[64], out[64];
    for (unsigned i = 0u; i < 32u; i++) {
        pcm_a[i * 2u] = pcm_a[i * 2u + 1u] = 1000;
        pcm_b[i * 2u] = pcm_b[i * 2u + 1u] = 1000;
    }
    size_t written = 0u;
    dsound_mixer *mixer = dsound_mixer_create(32000u, 32000u);
    CHECK(mixer != NULL);
    /* A missing-key reset is a no-op, not a reservation of a persistent mixer stream slot. */
    for (uint32_t key = 1u; key <= 64u; key++)
        CHECK(dsound_mixer_reset_stream_volume(mixer, key));
    for (uint32_t key = 1u; key <= 64u; key++)
        CHECK(dsound_mixer_submit_pcm16_stereo(mixer, key, 0u, 32000u, pcm_a, 32u));
    CHECK(!dsound_mixer_submit_pcm16_stereo(mixer, 65u, 0u, 32000u, pcm_a, 32u));
    dsound_mixer_destroy(mixer);

    mixer = dsound_mixer_create(32000u, 32000u);
    CHECK(mixer != NULL);
    CHECK(dsound_mixer_set_stream_volume(mixer, 1u, -10000));
    CHECK(dsound_mixer_set_stream_volume(mixer, 2u, -2000));
    CHECK(dsound_mixer_reset_stream_volume(mixer, 1u));
    CHECK(dsound_mixer_submit_pcm16_stereo(mixer, 1u, 0u, 32000u, pcm_a, 32u));
    CHECK(dsound_mixer_submit_pcm16_stereo(mixer, 2u, 0u, 32000u, pcm_b, 32u));
    CHECK(dsound_mixer_render_until(mixer, 32u, out, 32u, &written) && written == 32u);
    for (unsigned i = 0u; i < 32u; i++) {
        CHECK(out[i * 2u] == 1100 && out[i * 2u + 1u] == 1100);
    }
    dsound_mixer_destroy(mixer);
    return 0;
}

int main(void)
{
    uint8_t positive[72];
    uint8_t negative[72];
    make_block(positive, 20000, 10000);
    make_block(negative, 20000, -10000);
    int16_t first[256];
    int16_t second[256];
    size_t written = 0u;

    dsound_mixer *mixer = dsound_mixer_create(32000u, 32000u);
    CHECK(mixer != NULL);
    CHECK(dsound_mixer_submit_xbox_adpcm(mixer, 1u, 0u, 32000u, positive, sizeof(positive)));
    CHECK(dsound_mixer_submit_xbox_adpcm(mixer, 1u, 0u, 32000u, negative, sizeof(negative)));
    CHECK(dsound_mixer_render_until(mixer, 128u, first, 128u, &written));
    CHECK(written == 128u);
    CHECK(first[0] == 20000 && first[1] == 10000);
    CHECK(first[64u * 2u] == 20000 && first[64u * 2u + 1u] == -10000);
    CHECK(dsound_mixer_get_counts(mixer).packets_queued == 2u);
    CHECK(dsound_mixer_get_counts(mixer).frames_emitted == 128u);
    dsound_mixer_destroy(mixer);

    mixer = dsound_mixer_create(32000u, 32000u);
    CHECK(mixer != NULL);
    CHECK(dsound_mixer_submit_xbox_adpcm(mixer, 1u, 0u, 32000u, positive, sizeof(positive)));
    CHECK(dsound_mixer_render_until(mixer, 32u, first, 32u, &written));
    CHECK(written == 32u);
    CHECK(dsound_mixer_submit_xbox_adpcm(mixer, 2u, 96u, 32000u, negative, sizeof(negative)));
    CHECK(dsound_mixer_render_until(mixer, 160u, first, 128u, &written));
    CHECK(written == 128u);
    CHECK(first[0] == 20000 && first[1] == 10000);
    CHECK(first[32u * 2u] == 0 && first[32u * 2u + 1u] == 0);
    CHECK(first[64u * 2u] == 20000 && first[64u * 2u + 1u] == -10000);
    CHECK(dsound_mixer_get_counts(mixer).late_packets_refused == 0u);
    CHECK(!dsound_mixer_submit_xbox_adpcm(mixer, 3u, 1u, 32000u, positive, sizeof(positive)));
    CHECK(dsound_mixer_get_counts(mixer).late_packets_refused == 1u);
    dsound_mixer_destroy(mixer);

    mixer = dsound_mixer_create(32000u, 32000u);
    dsound_mixer *twin = dsound_mixer_create(32000u, 32000u);
    CHECK(mixer != NULL && twin != NULL);
    CHECK(dsound_mixer_submit_xbox_adpcm(mixer, 1u, 0u, 32000u, positive, sizeof(positive)));
    CHECK(dsound_mixer_submit_xbox_adpcm(twin, 1u, 0u, 32000u, positive, sizeof(positive)));
    CHECK(dsound_mixer_submit_xbox_adpcm(mixer, 2u, 0u, 32000u, positive, sizeof(positive)));
    CHECK(dsound_mixer_submit_xbox_adpcm(twin, 2u, 0u, 32000u, positive, sizeof(positive)));
    CHECK(dsound_mixer_render_until(mixer, 64u, first, 64u, &written));
    CHECK(written == 64u);
    CHECK(dsound_mixer_render_until(twin, 64u, second, 64u, &written));
    CHECK(written == 64u && memcmp(first, second, 64u * 2u * sizeof(int16_t)) == 0);
    CHECK(first[0] == INT16_MAX && first[1] == 20000);
    dsound_mixer_destroy(mixer);
    dsound_mixer_destroy(twin);

    CHECK(dsound_audio_runtime_start(32000u, 32000u));
    CHECK(dsound_audio_runtime_active());
    CHECK(dsound_audio_runtime_submit(1u, 0u, 32000u, positive, sizeof(positive)));
    CHECK(dsound_audio_runtime_render(64u, first, 64u, &written));
    CHECK(written == 64u && first[0] == 20000 && first[1] == 10000);
    dsound_audio_runtime_stop();
    CHECK(!dsound_audio_runtime_active());

    /* T1238: a mono 36 byte block ADPCM stream packet (the Story dialogue) feeds both speakers at full gain. */
    {
        uint8_t mono_block[36];
        memset(mono_block, 0, sizeof(mono_block));
        mono_block[0] = (uint8_t)12000;
        mono_block[1] = (uint8_t)((uint16_t)12000 >> 8u);
        CHECK(!dsound_mixer_submit_xbox_adpcm_mono(NULL, 1u, 0u, 32000u, mono_block, sizeof(mono_block)));
        dsound_mixer *mono = dsound_mixer_create(32000u, 32000u);
        CHECK(mono != NULL);
        CHECK(!dsound_mixer_submit_xbox_adpcm_mono(mono, 1u, 0u, 32000u, mono_block, 35u));
        CHECK(!dsound_mixer_submit_xbox_adpcm_mono(mono, 1u, 0u, 32000u, mono_block, 0u));
        CHECK(!dsound_mixer_submit_xbox_adpcm_mono(mono, 0u, 0u, 32000u, mono_block, sizeof(mono_block)));
        CHECK(dsound_mixer_submit_xbox_adpcm_mono(mono, 1u, 0u, 32000u, mono_block, sizeof(mono_block)));
        CHECK(dsound_mixer_render_until(mono, 64u, first, 64u, &written));
        CHECK(written == 64u && first[0] == 12000 && first[1] == 12000);
        CHECK(first[2u * 63u] == first[2u * 63u + 1u]);
        CHECK(dsound_mixer_get_counts(mono).packets_queued == 1u);
        dsound_mixer_destroy(mono);
        CHECK(dsound_audio_runtime_start(32000u, 32000u));
        CHECK(dsound_audio_runtime_submit_mono(1u, 0u, 32000u, mono_block, sizeof(mono_block)));
        CHECK(dsound_audio_runtime_render(64u, first, 64u, &written));
        CHECK(written == 64u && first[0] == 12000 && first[1] == 12000);
        dsound_audio_runtime_stop();
    }

    mixer = dsound_mixer_create(48000u, 32000u);
    twin = dsound_mixer_create(48000u, 32000u);
    CHECK(mixer != NULL && twin != NULL);
    CHECK(dsound_mixer_submit_xbox_adpcm(mixer, 1u, 0u, 32000u, positive, sizeof(positive)));
    CHECK(dsound_mixer_submit_xbox_adpcm(twin, 1u, 0u, 32000u, positive, sizeof(positive)));
    CHECK(dsound_mixer_render_until(mixer, 64u, first, 128u, &written));
    CHECK(written == 96u && first[0] == 20000 && first[1] == 10000);
    CHECK(first[94u * 2u] == 20000 && first[94u * 2u + 1u] == 10000);
    CHECK(dsound_mixer_render_until(twin, 31u, second, 128u, &written));
    CHECK(written == 47u);
    size_t tail_written = 0u;
    CHECK(dsound_mixer_render_until(twin, 64u, second + written * 2u, 128u - written,
                                    &tail_written));
    CHECK(tail_written == 49u);
    CHECK(memcmp(first, second, 96u * 2u * sizeof(int16_t)) == 0);
    dsound_mixer_destroy(mixer);
    dsound_mixer_destroy(twin);

    mixer = dsound_mixer_create(32000u, 32000u);
    CHECK(mixer != NULL);
    CHECK(dsound_mixer_submit_xbox_adpcm(mixer, 7u, 0u, 32000u, positive, sizeof(positive)));
    CHECK(dsound_mixer_render_until(mixer, 16u, first, 16u, &written));
    CHECK(written == 16u && first[0] == 20000);
    CHECK(dsound_mixer_set_stream_running(mixer, 7u, 16u, false));
    CHECK(dsound_mixer_render_until(mixer, 32u, first, 16u, &written));
    CHECK(written == 16u && first[0] == 0 && first[1] == 0);
    CHECK(dsound_mixer_set_stream_running(mixer, 7u, 64u, true));
    CHECK(dsound_mixer_render_until(mixer, 112u, first, 80u, &written));
    CHECK(written == 80u);
    CHECK(first[0] == 0 && first[1] == 0);
    CHECK(first[32u * 2u] == 20000 && first[32u * 2u + 1u] == 10000);
    CHECK(first[79u * 2u] == 20000 && first[79u * 2u + 1u] == 10000);
    dsound_mixer_destroy(mixer);

    /* Raw PCM16 stereo (the XMV movie soundtrack), back to back, then a cut. */
    int16_t pcm[64];
    for (unsigned i = 0u; i < 32u; i++) { pcm[2u * i] = (int16_t)(100 + i); pcm[2u * i + 1u] = (int16_t)(-100 - (int)i); }
    mixer = dsound_mixer_create(32000u, 32000u);
    CHECK(mixer != NULL);
    CHECK(!dsound_mixer_submit_pcm16_stereo(mixer, 0u, 0u, 32000u, pcm, 32u));
    CHECK(!dsound_mixer_submit_pcm16_stereo(mixer, 1u, 0u, 32000u, pcm, 0u));
    CHECK(dsound_mixer_submit_pcm16_stereo(mixer, 1u, 0u, 32000u, pcm, 32u));
    CHECK(dsound_mixer_submit_pcm16_stereo(mixer, 1u, 0u, 32000u, pcm, 32u));
    CHECK(dsound_mixer_render_until(mixer, 64u, first, 64u, &written));
    CHECK(written == 64u);
    CHECK(first[0] == 100 && first[1] == -100 && first[31u * 2u] == 131);
    CHECK(first[32u * 2u] == 100 && first[63u * 2u + 1u] == -131);
    dsound_mixer_destroy(mixer);
    mixer = dsound_mixer_create(32000u, 32000u);
    CHECK(dsound_mixer_submit_pcm16_stereo(mixer, 1u, 0u, 32000u, pcm, 32u));
    CHECK(dsound_mixer_submit_pcm16_stereo(mixer, 1u, 0u, 32000u, pcm, 32u));
    CHECK(dsound_mixer_render_until(mixer, 8u, first, 8u, &written));
    CHECK(dsound_mixer_cut_stream(mixer, 1u, 8u));
    CHECK(dsound_mixer_render_until(mixer, 72u, first, 64u, &written));
    CHECK(written == 64u);
    for (unsigned i = 0u; i < 64u; i++) CHECK(first[2u * i] == 0 && first[2u * i + 1u] == 0);
    CHECK(dsound_mixer_submit_pcm16_stereo(mixer, 1u, 72u, 32000u, pcm, 32u));
    CHECK(dsound_mixer_render_until(mixer, 104u, first, 32u, &written));
    CHECK(written == 32u && first[0] == 100);
    dsound_mixer_destroy(mixer);

    /* T818: per stream SetVolume gain (hundredths of a dB of amplitude, 0 full, -10000 mute). */
    {
        const int32_t table_mb[] = {0, -1, -326, -602, -1000, -2000, -6000, -9999, -10000};
        const uint32_t table_q16[] = {65536u, 65461u, 45028u, 32770u, 20724u, 6554u, 66u, 1u, 0u};
        for (unsigned i = 0u; i < sizeof(table_mb) / sizeof(table_mb[0]); i++) {
            uint32_t gain = 12345u;
            CHECK(dsound_mixer_volume_to_gain_q16(table_mb[i], &gain) && gain == table_q16[i]);
        }
        uint32_t untouched = 777u;
        const int32_t refused_mb[] = {1, 2000, INT32_MAX, -10001, INT32_MIN};
        for (unsigned i = 0u; i < sizeof(refused_mb) / sizeof(refused_mb[0]); i++) {
            CHECK(!dsound_mixer_volume_to_gain_q16(refused_mb[i], &untouched) && untouched == 777u);
        }
        CHECK(!dsound_mixer_volume_to_gain_q16(0, NULL));
        int16_t reference[64], scaled[64];
        const int32_t render_mb[] = {0, -2000, -10000};
        dsound_mixer *base = dsound_mixer_create(32000u, 32000u);
        CHECK(base != NULL && dsound_mixer_submit_pcm16_stereo(base, 1u, 0u, 32000u, pcm, 32u));
        CHECK(dsound_mixer_render_until(base, 32u, reference, 32u, &written) && written == 32u);
        dsound_mixer_destroy(base);
        for (unsigned i = 0u; i < 3u; i++) {
            mixer = dsound_mixer_create(32000u, 32000u);
            CHECK(mixer != NULL);
            CHECK(!dsound_mixer_set_stream_volume(mixer, 1u, 1));
            CHECK(!dsound_mixer_set_stream_volume(mixer, 1u, -10001));
            CHECK(!dsound_mixer_set_stream_volume(mixer, 0u, 0));
            CHECK(dsound_mixer_set_stream_volume(mixer, 1u, render_mb[i]));
            CHECK(dsound_mixer_submit_pcm16_stereo(mixer, 1u, 0u, 32000u, pcm, 32u));
            CHECK(dsound_mixer_render_until(mixer, 32u, scaled, 32u, &written) && written == 32u);
            if (i == 0u) CHECK(memcmp(scaled, reference, 32u * 2u * sizeof(int16_t)) == 0);
            for (unsigned j = 0u; j < 64u; j++) {
                if (i == 1u) {
                    const int32_t expect = (int32_t)(((int64_t)reference[j] * 6554 + 32768) >> 16);
                    CHECK(scaled[j] == expect && scaled[j] != reference[j]);
                }
                if (i == 2u) CHECK(scaled[j] == 0);
            }
            dsound_mixer_destroy(mixer);
        }
        /* The gain is per stream: a second stream at full volume is not scaled. */
        mixer = dsound_mixer_create(32000u, 32000u);
        CHECK(mixer != NULL && dsound_mixer_set_stream_volume(mixer, 1u, -10000));
        CHECK(dsound_mixer_submit_pcm16_stereo(mixer, 1u, 0u, 32000u, pcm, 32u));
        CHECK(dsound_mixer_submit_pcm16_stereo(mixer, 2u, 0u, 32000u, pcm, 32u));
        CHECK(dsound_mixer_render_until(mixer, 32u, scaled, 32u, &written) && written == 32u);
        CHECK(memcmp(scaled, reference, 32u * 2u * sizeof(int16_t)) == 0);
        /* A later SetVolume applies to the unplayed rest: back to full, bit identical. */
        CHECK(dsound_mixer_set_stream_volume(mixer, 1u, 0));
        dsound_mixer_destroy(mixer);
    }

    if (test_resampler() != 0 || test_ratio_one_unchanged() != 0 ||
        test_reset_volume_is_existing_only_and_stream_local() != 0)
        return 1;
    printf("dsound_mixer: %u checks passed\n", checks);
    return 0;
}
