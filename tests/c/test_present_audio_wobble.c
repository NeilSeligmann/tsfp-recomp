/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1487: audible artefacts of the stretched playback (the "wobble") and the A/V media clock offset. The real sink
 * (ring only, 48 kHz stereo, a simulated 1 ms wall clock, the SDL device's 1024 frame pulls) plays a synthetic harmonic
 * "voice" (140 Hz, 10 harmonics, constant level) produced by a guest in 5 ms pump chunks at a speed profile. The output
 * is measured objectively: pitch deviation in cents (autocorrelation per 40 ms), amplitude modulation depth (stdev of
 * the 20 ms RMS over its mean, permille), discontinuity energy (third difference spikes above the clean signal's own
 * maximum, as a share of the signal power) and how often the stretch engaged. LEGACY = the pre T1487 ratio control.
 * Set WOBBLE_VERBOSE=1 for the table. No listening test: these are proxies.
 */
#define _DEFAULT_SOURCE 1
#include "present_sink.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            failures++;                                                      \
        }                                                                    \
    } while (0)

#define RATE 48000
#define PULL 1024
#define F0 140.0
#define PI 3.14159265358979323846

static int64_t g_now_ns;
static int64_t test_clock(void)
{
    return g_now_ns;
}

typedef enum { P_REAL, P_DIP, P_SLOW, P_LOADS, P_JITTER, P_MILD } profile;

static const char *profile_name(profile which)
{
    static const char *names[] = {"real(1.0, 60ms hitch/2s)", "dip(0.9 for 1s)", "slow(0.6 for 3s)", "loads(0.3 x4 0.5s)", "jitter(0.97..1.03)", "mild(0.98 for 10s)"};
    return names[which];
}

/* guest speed (audio frames produced per wall frame) at wall time ms */
static double speed_at(profile which, unsigned ms)
{
    switch (which) {
    case P_REAL:
        return (ms % 2000u) < 60u && ms > 3000u ? 0.0 : 1.0;
    case P_DIP:
        return ms >= 4000u && ms < 5000u ? 0.9 : 1.0;
    case P_SLOW:
        return ms >= 4000u && ms < 7000u ? 0.6 : 1.0;
    case P_LOADS:
        return (ms % 2500u) >= 2000u && ms > 3000u ? 0.3 : 1.0;
    case P_MILD:
        return ms >= 3000u ? 0.98 : 1.0;
    case P_JITTER:
        return 1.0 + 0.03 * sin((double)ms / 137.0) + 0.02 * sin((double)ms / 29.0);
    }
    return 1.0;
}

static double voice(double phase)
{
    double sum = 0.0;
    for (int harmonic = 1; harmonic <= 10; harmonic++) {
        sum += sin(phase * harmonic) / harmonic;
    }
    return 3000.0 * sum;
}

typedef struct {
    double pitch_max_cents;
    double pitch_rms_cents;
    double am_permille;
    double disc_db; /* spike energy relative to the signal power, dB (-200 = none) */
    size_t silent_windows;
    unsigned long long engagements;
    unsigned long long slow_ms;
    unsigned long long cutouts;
    unsigned long long fill_min_ms;
    unsigned long long max_step_q16;
    unsigned long long panic_pulls;
} measure;

static unsigned g_hysteresis_ms = 100u;

static measure run(profile which, const present_audio_stretch_tuning *tuning, unsigned target_ms, unsigned low_ms, unsigned seconds)
{
    present_audio_sink *sink = present_audio_sink_open_ring(RATE, 2u);
    present_audio_sink_enable_clock(sink, 400u, 1500u, 4u);
    present_audio_sink_set_latency_governor(sink, target_ms, g_hysteresis_ms, 50u);
    present_audio_sink_set_continuous_playback(sink, low_ms, 250u);
    if (tuning != NULL) {
        present_audio_sink_set_stretch_tuning(sink, tuning);
    }
    present_audio_sink_set_stretch(sink, true);
    const size_t total = (size_t)seconds * RATE;
    int16_t *heard = calloc(total + PULL, 2u * sizeof *heard);
    int16_t *clean = calloc(total + PULL, sizeof *clean); /* what an unstretched guest at speed 1 would give: for the power */
    size_t heard_frames = 0u;
    double produced = 0.0;
    double phase = 0.0;
    double next_pull = 0.0;
    int16_t chunk[2 * 480];
    for (unsigned ms = 0u; ms < seconds * 1000u && heard_frames + PULL <= total; ms++) {
        g_now_ns = (int64_t)ms * 1000000;
        if (ms % 5u == 0u) {
            produced += speed_at(which, ms) * 240.0;
            while (produced >= 240.0) {
                for (size_t frame = 0u; frame < 240u; frame++) {
                    const int16_t sample = (int16_t)voice(phase);
                    phase += 2.0 * PI * F0 / RATE;
                    chunk[2u * frame] = chunk[2u * frame + 1u] = sample;
                }
                present_audio_sink_write(sink, chunk, 240u);
                produced -= 240.0;
            }
        }
        while ((double)ms >= next_pull) {
            present_audio_sink_pull(sink, heard + 2u * heard_frames, PULL);
            heard_frames += PULL;
            next_pull += 1000.0 * PULL / RATE;
        }
    }
    measure result;
    memset(&result, 0, sizeof result);
    const present_audio_counts counts = present_audio_sink_counts(sink);
    result.engagements = counts.stretch_engagements;
    result.slow_ms = counts.slow_frames * 1000u / RATE;
    result.cutouts = counts.cutout_events;
    result.fill_min_ms = counts.fill_min * 1000u / RATE;
    result.max_step_q16 = counts.stretch_max_step_q16;
    result.panic_pulls = counts.stretch_panic_pulls;
    present_audio_sink_close(sink);
    /* reference: spike threshold of the clean voice's third difference */
    double ref_max = 0.0;
    double power = 0.0;
    for (size_t frame = 0u; frame < 4800u; frame++) {
        const double a = voice(2.0 * PI * F0 * (double)(frame + 3u) / RATE), b = voice(2.0 * PI * F0 * (double)(frame + 2u) / RATE);
        const double c = voice(2.0 * PI * F0 * (double)(frame + 1u) / RATE), d = voice(2.0 * PI * F0 * (double)frame / RATE);
        const double third = fabs(a - 3.0 * b + 3.0 * c - d);
        ref_max = third > ref_max ? third : ref_max;
        power += d * d;
    }
    power /= 4800.0;
    free(clean);
    const size_t first = (size_t)(2.0 * RATE); /* after the start up */
    double spike_energy = 0.0;
    for (size_t frame = first + 3u; frame < heard_frames; frame++) {
        const double third = fabs((double)heard[2u * frame] - 3.0 * heard[2u * (frame - 1u)] + 3.0 * heard[2u * (frame - 2u)] -
                                  heard[2u * (frame - 3u)]);
        if (third > 2.0 * ref_max) {
            spike_energy += third * third;
        }
    }
    const double seconds_measured = (double)(heard_frames - first) / RATE;
    result.disc_db = spike_energy > 0.0 ? 10.0 * log10(spike_energy / seconds_measured / RATE / (power * 0.01)) : -200.0;
    /* pitch per 40 ms hop, am per 20 ms rms */
    double cents_sum = 0.0;
    size_t cents_n = 0u;
    const size_t expected_period = (size_t)(RATE / F0);
    for (size_t at = first; at + 1920u + 2u * expected_period < heard_frames; at += 1920u) {
        double best = -1.0e300;
        size_t best_lag = expected_period;
        double scores[600];
        const size_t lag_lo = expected_period * 85u / 100u, lag_hi = expected_period * 115u / 100u;
        for (size_t lag = lag_lo; lag <= lag_hi; lag++) {
            double corr = 0.0, energy = 1.0;
            for (size_t frame = 0u; frame < 1920u; frame++) {
                corr += (double)heard[2u * (at + frame)] * heard[2u * (at + frame + lag)];
                energy += (double)heard[2u * (at + frame + lag)] * heard[2u * (at + frame + lag)];
            }
            scores[lag - lag_lo] = corr / sqrt(energy);
            if (scores[lag - lag_lo] > best) {
                best = scores[lag - lag_lo];
                best_lag = lag;
            }
        }
        double lag_f = (double)best_lag;
        if (best_lag > lag_lo && best_lag < lag_hi) {
            const double l = scores[best_lag - lag_lo - 1u], m = scores[best_lag - lag_lo], r = scores[best_lag - lag_lo + 1u];
            const double denom = l - 2.0 * m + r;
            lag_f += denom != 0.0 ? 0.5 * (l - r) / denom : 0.0;
        }
        const double cents = 1200.0 * log2((double)RATE / lag_f / F0);
        cents_sum += cents * cents;
        cents_n++;
        result.pitch_max_cents = fabs(cents) > result.pitch_max_cents ? fabs(cents) : result.pitch_max_cents;
    }
    result.pitch_rms_cents = cents_n ? sqrt(cents_sum / (double)cents_n) : 0.0;
    double mean = 0.0, mean_sq = 0.0;
    size_t windows = 0u;
    for (size_t at = first; at + 1029u <= heard_frames; at += 1029u) { /* three periods of the voice: its own ripple cancels */
        double sum = 0.0;
        for (size_t frame = 0u; frame < 1029u; frame++) {
            sum += (double)heard[2u * (at + frame)] * heard[2u * (at + frame)];
        }
        const double rms = sqrt(sum / 1029.0);
        mean += rms;
        mean_sq += rms * rms;
        windows++;
        result.silent_windows += rms < 30.0 ? 1u : 0u;
    }
    if (windows != 0u) {
        mean /= (double)windows;
        const double variance = mean_sq / (double)windows - mean * mean;
        result.am_permille = mean > 0.0 ? 1000.0 * sqrt(variance > 0.0 ? variance : 0.0) / mean : 0.0;
    }
    free(heard);
    return result;
}

static const present_audio_stretch_tuning LEGACY = {6667u, 1000u, 1000u, 0u, 1000u, 999u};
static const present_audio_stretch_tuning NEW = {9333u, 30u, 10u, 150u, 950u, 985u};

static void show(const char *label, profile which, const measure *m)
{
    if (getenv("WOBBLE_VERBOSE") != NULL) {
        printf("%-26s %-26s pitch max %6.1f rms %5.1f cents | AM %6.1f permille | spikes %7.1f dB | silent %zu | engaged %llu slowed %llu ms cutouts %llu "
               "fill_min %llu ms panic pulls %llu\n",
               label, profile_name(which), m->pitch_max_cents, m->pitch_rms_cents, m->am_permille, m->disc_db, m->silent_windows,
               m->engagements, m->slow_ms, m->cutouts, m->fill_min_ms, m->panic_pulls);
    }
}


/* A/V lead: an impulse stamped at guest time T is written by a real time guest. t_heard is the wall ms of the device pull that
 * carries it, t_video the wall ms at which the media clock (what the video follows) reaches T. */
static void av_run(int offset_ms, double *t_heard, double *t_video)
{
    present_audio_sink *sink = present_audio_sink_open_ring(RATE, 2u);
    present_audio_sink_enable_clock(sink, 50u, 500u, 4u);
    present_audio_sink_set_av_offset_ms(sink, offset_ms);
    const uint64_t base = 7000000000ull;
    const size_t impulse_frame = 48000u; /* guest time base + 1 s */
    int16_t chunk[2 * 240];
    int16_t out[2 * PULL];
    size_t written = 0u;
    double next_pull = 0.0;
    *t_heard = *t_video = -1.0;
    bool pulled_started = false;
    for (unsigned ms = 0u; ms < 3000u; ms++) {
        g_now_ns = (int64_t)ms * 1000000;
        if (ms % 5u == 0u && ms >= 100u) { /* the guest starts 100 ms in, then produces 5 ms per 5 ms of wall time */
            for (size_t frame = 0u; frame < 240u; frame++) {
                const int16_t value = written + frame == impulse_frame ? 20000 : 0;
                chunk[2u * frame] = chunk[2u * frame + 1u] = value;
            }
            written += 240u;
            present_audio_sink_write_at(sink, chunk, 240u, base + (uint64_t)written * 1000000000ull / RATE);
        }
        while ((double)ms >= next_pull) {
            present_audio_sink_pull(sink, out, PULL);
            pulled_started = true;
            for (size_t frame = 0u; frame < PULL; frame++) {
                if (*t_heard < 0.0 && out[2u * frame] == 20000) {
                    *t_heard = (double)ms;
                }
            }
            next_pull += 1000.0 * PULL / RATE;
        }
        uint64_t media = 0u;
        if (*t_video < 0.0 && pulled_started && present_audio_sink_media_time(sink, &media) &&
            media >= base + (uint64_t)impulse_frame * 1000000000ull / RATE) {
            *t_video = (double)ms;
        }
    }
    present_audio_sink_close(sink);
}

static void av_checks(void)
{
    double heard0, video0, heard40, video40, heard_neg, video_neg;
    av_run(0, &heard0, &video0);
    av_run(40, &heard40, &video40);
    av_run(-15, &heard_neg, &video_neg);
    if (getenv("WOBBLE_VERBOSE") != NULL) {
        printf("A/V: impulse reaches the device pull at %.0f ms, the media clock at %.0f ms (offset 0), %.0f (+40), %.0f (-15)\n", heard0, video0,
               video40, video_neg);
    }
    if (getenv("WOBBLE_VERBOSE") != NULL) {
        for (int offset = -30; offset <= 30; offset += 10) {
            double h, v;
            av_run(offset, &h, &v);
            printf("  offset %d: video at %.0f (heard %.0f)\n", offset, v, h);
        }
    }
    CHECK(heard0 > 0.0 && video0 > 0.0);
    /* video follows the pull that carries the sound: it never leads the pull by more than a ms of polling and trails it by less than one
     * pull (the chunk is played over its own duration), so it is ahead of the AUDIBLE sound by about the device buffer */
    CHECK(video0 >= heard0 - 1.0 && video0 <= heard0 + 22.0);
    CHECK(heard40 == heard0 && heard_neg == heard0);
    CHECK(video40 >= video0 + 39.0 && video40 <= video0 + 41.0); /* +40 ms: the video is shown 40 ms later */
    CHECK(video_neg >= video0 - 16.0 && video_neg <= video0 - 14.0); /* -15 ms: earlier */
}

static void media_clock_checks(void)
{
    /* The media clock follows the pulls; an offset moves it earlier (video later), never before the anchor. */
    for (int pass = 0; pass < 3; pass++) {
        present_audio_sink *sink = present_audio_sink_open_ring(RATE, 2u);
        present_audio_sink_set_test_clock(test_clock);
        g_now_ns = 1000000000;
        present_audio_sink_enable_clock(sink, 50u, 500u, 4u);
        present_audio_sink_set_av_offset_ms(sink, pass == 0 ? 0 : pass == 1 ? 21 : -40);
        int16_t block[2 * 4800];
        for (size_t index = 0u; index < 2u * 4800u; index++) {
            block[index] = 100;
        }
        const uint64_t anchor = 5000000000ull;
        CHECK(present_audio_sink_write_at(sink, block, 4800u, anchor + 100000000ull)); /* 100 ms of audio ending at anchor + 100 ms */
        int16_t out[2 * 2400];
        present_audio_sink_pull(sink, out, 2400u); /* 50 ms consumed */
        uint64_t media = 0u;
        g_now_ns += 60000000; /* well past the chunk */
        CHECK(present_audio_sink_media_time(sink, &media));
        const int offsets[3] = {0, 21, -40};
        const int64_t expected = (int64_t)anchor + 50000000 - (int64_t)offsets[pass] * 1000000;
        CHECK((int64_t)media == expected);
        if ((int64_t)media != expected) {
            printf("pass %d media %llu expected %lld\n", pass, (unsigned long long)media, (long long)expected);
        }
        present_audio_sink_close(sink);
    }
    {
        present_audio_sink *sink = present_audio_sink_open_ring(RATE, 2u);
        present_audio_sink_set_test_clock(test_clock);
        g_now_ns = 1000000000;
        present_audio_sink_enable_clock(sink, 50u, 500u, 4u);
        present_audio_sink_set_av_offset_ms(sink, 500);
        int16_t block[2 * 4800] = {0};
        CHECK(present_audio_sink_write_at(sink, block, 4800u, 5100000000ull));
        uint64_t media = 0u;
        CHECK(present_audio_sink_media_time(sink, &media));
        CHECK(media == 5000000000ull); /* clamped at the anchor */
        present_audio_sink_close(sink);
    }
}

static void sweep_blocks(void)
{
    g_hysteresis_ms = 30u;
    setenv("WOBBLE_VERBOSE", "1", 1);
    for (unsigned block = 6667u; block <= 12000u; block += 1333u) {
        present_audio_stretch_tuning tuning = NEW;
        tuning.block_us = block;
        for (profile which = P_REAL; which <= P_MILD; which++) {
            const measure m = run(which, &tuning, 80u, 60u, 20u);
            char label[64];
            snprintf(label, sizeof label, "block %u us", block);
            show(label, which, &m);
        }
    }
}

static void sweep(void)
{
    if (getenv("WOBBLE_SWEEP")[0] == 'b') {
        sweep_blocks();
        return;
    }
    const present_audio_stretch_tuning variants[] = {{10000u, 30u, 10u, 150u, 950u, 985u}, {10000u, 30u, 10u, 150u, 1000u, 999u},
                                                    {10000u, 30u, 10u, 150u, 900u, 970u}, {10000u, 30u, 10u, 400u, 950u, 985u},
                                                    {10000u, 30u, 10u, 0u, 1000u, 999u}, {10000u, 1000u, 1000u, 0u, 1000u, 999u}};
    for (size_t row = 0u; row < sizeof variants / sizeof variants[0]; row++) {
        for (profile which = P_REAL; which <= P_MILD; which++) {
            const measure m = run(which, &variants[row], 80u, 60u, 20u);
            char label[96];
            snprintf(label, sizeof label, "eng%u leave%u smooth%u slew%u/%u", variants[row].engage_permille, variants[row].leave_permille,
                     variants[row].smooth_ms, variants[row].slew_down_permille, variants[row].slew_up_permille);
            setenv("WOBBLE_VERBOSE", "1", 1);
            show(label, which, &m);
        }
    }
    if (getenv("WOBBLE_SWEEP")[0] == 'v') {
        return;
    }
    const profile all[] = {P_REAL, P_DIP, P_SLOW, P_LOADS, P_JITTER};
    const unsigned grid[][3] = {{150u, 100u, 100u}, {150u, 100u, 30u}, {100u, 66u, 30u}, {80u, 60u, 30u}, {60u, 40u, 20u}, {50u, 35u, 20u}, {40u, 30u, 15u}};
    for (size_t row = 0u; row < sizeof grid / sizeof grid[0]; row++) {
        g_hysteresis_ms = grid[row][2];
        for (size_t index = 0u; index < 5u; index++) {
            const measure m = run(all[index], &NEW, grid[row][0], grid[row][1], 14u);
            char label[64];
            snprintf(label, sizeof label, "NEW t%u low%u hys%u", grid[row][0], grid[row][1], grid[row][2]);
            setenv("WOBBLE_VERBOSE", "1", 1);
            show(label, all[index], &m);
        }
    }
    g_hysteresis_ms = 100u;
    /* WSOLA block length for the voice (search = half the block), at the dip and slow profiles */
    for (unsigned block = 4000u; block <= 30000u; block += (block < 10000u ? 2667u : 5000u)) {
        present_audio_stretch_tuning tuning = NEW;
        tuning.block_us = block;
        for (size_t index = 1u; index < 4u; index++) {
            const measure m = run(all[index], &tuning, 150u, 100u, 14u);
            char label[64];
            snprintf(label, sizeof label, "NEW block %u us", block);
            show(label, all[index], &m);
        }
    }
}

int main(void)
{
    present_audio_sink_set_test_clock(test_clock);
    if (getenv("WOBBLE_SWEEP") != NULL) {
        sweep();
        return 0;
    }
    const profile all[] = {P_REAL, P_DIP, P_SLOW, P_LOADS, P_JITTER};
    measure legacy[5], current[5];
    for (size_t index = 0u; index < 5u; index++) {
        legacy[index] = run(all[index], &LEGACY, 150u, 100u, 14u);
        current[index] = run(all[index], &NEW, 150u, 100u, 14u);
        show("LEGACY target150/low100", all[index], &legacy[index]);
        show("NEW    target150/low100", all[index], &current[index]);
        CHECK(legacy[index].silent_windows == 0u);
        CHECK(current[index].silent_windows == 0u);
        CHECK(current[index].cutouts == 0u);
    }
    /* the T1487 claims, measured on the synthetic voice */
    CHECK(legacy[P_REAL].engagements >= 4u && current[P_REAL].engagements < legacy[P_REAL].engagements); /* fewer onsets on a hitch */
    CHECK(legacy[P_LOADS].engagements >= 3u && current[P_LOADS].engagements < legacy[P_LOADS].engagements);
    CHECK(current[P_JITTER].engagements == 0u && current[P_JITTER].slow_ms == 0u); /* a +-3 percent guest is never stretched */
    CHECK(legacy[P_DIP].pitch_max_cents > 20.0); /* the step ratio and the 6.7 ms block wobble the 140 Hz voice ... */
    CHECK(current[P_DIP].pitch_max_cents < 2.0);  /* ... the slew limited ratio and the 10 ms block do not */
    CHECK(current[P_SLOW].pitch_max_cents < 2.0);
    CHECK(current[P_REAL].pitch_max_cents < 10.0 && current[P_LOADS].pitch_max_cents < 10.0);
    CHECK(current[P_DIP].am_permille < 1.0 && current[P_SLOW].am_permille < 1.0);
    CHECK(current[P_SLOW].slow_ms > 1000u); /* it does follow a long slowdown: the ring is not drained */
    /* the lower governor target of the host default (80 ms, low water 60 ms, hysteresis 30 ms): no cutout, no silent window */
    g_hysteresis_ms = 30u;
    for (size_t index = 0u; index < 5u; index++) {
        const measure low = run(all[index], &NEW, 80u, 60u, 14u);
        show("NEW t80 low60 hys30", all[index], &low);
        CHECK(low.silent_windows == 0u && low.cutouts == 0u);
        CHECK(low.fill_min_ms >= 10u);
    }
    g_hysteresis_ms = 100u;
    media_clock_checks();
    av_checks();
    /* defaults (no tuning call) are the NEW tuning */
    {
        const measure def = run(P_DIP, NULL, 150u, 100u, 14u);
        CHECK(def.engagements == current[P_DIP].engagements && def.slow_ms == current[P_DIP].slow_ms &&
              fabs(def.pitch_max_cents - current[P_DIP].pitch_max_cents) < 1.0e-9);
    }
    /* without the smoothed fill the slew limit alone bounds the ratio step per pull (30 permille, plus the 1.5 percent snap) */
    {
        const present_audio_stretch_tuning raw = {9333u, 30u, 10u, 0u, 950u, 985u};
        const measure bounded = run(P_LOADS, &raw, 150u, 100u, 14u);
        CHECK(bounded.engagements >= 1u);
        CHECK(bounded.max_step_q16 > 0u && bounded.max_step_q16 <= 50u * 65536u / 1000u);
        CHECK(bounded.cutouts == 0u && bounded.silent_windows == 0u);
    }
    /* a guest 2 percent slow: stretched slightly, no audible artefact by the measures (the onset count stays high, see the doc) */
    {
        const measure mild = run(P_MILD, &NEW, 80u, 60u, 20u);
        show("NEW t80 mild", P_MILD, &mild);
        CHECK(mild.slow_ms > 400u && mild.pitch_max_cents < 2.0 && mild.am_permille < 1.0 && mild.cutouts == 0u);
    }
    present_audio_sink_set_test_clock(NULL);
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
