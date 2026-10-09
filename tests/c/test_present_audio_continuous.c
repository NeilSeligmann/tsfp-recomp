/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1248: continuous playback below real time and the cutout counters. A ring-only sink at 1000 frames per second so
 * the numbers are exact. A guest that produces audio at half real time used to drain the ring, after which EVERY voice
 * was cut (silence) until a refill completed. With continuous playback the device consumes the ring slower while the
 * fill is low (the pitch follows the guest's slow motion) and never hands the device silence. The tone here is a
 * constant non-zero value, so a zero output frame is a cutout and the interpolation of equal samples stays exact.
 */
#define _DEFAULT_SOURCE 1
#include "present_sink.h"

#include <stdio.h>
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

enum { RATE = 1000, TONE = 7 };

static present_audio_sink *make_sink(bool continuous)
{
    present_audio_sink *sink = present_audio_sink_open_ring(RATE, 2u);
    if (sink == NULL) {
        return NULL;
    }
    present_audio_sink_enable_clock(sink, 50u, 500u, 4u); /* prefill 50 frames, ring 4 s */
    if (continuous) {
        present_audio_sink_set_continuous_playback(sink, 100u, 350u); /* below 100 frames slow down, floor 0.35 */
    }
    return sink;
}

static void write_tone(present_audio_sink *sink, size_t frames)
{
    int16_t block[2 * 100];
    while (frames != 0u) {
        const size_t part = frames < 100u ? frames : 100u;
        for (size_t index = 0; index < 2u * part; index++) {
            block[index] = TONE;
        }
        CHECK(present_audio_sink_write(sink, block, part));
        frames -= part;
    }
}

/* One wall tick: the device pulls 20 frames, the guest writes `produced`. Returns the zero (cut) frames of the pull. */
static size_t tick(present_audio_sink *sink, size_t produced, int16_t *last)
{
    int16_t out[2 * 20];
    present_audio_sink_pull(sink, out, 20u);
    size_t zeros = 0u;
    for (size_t frame = 0; frame < 20u; frame++) {
        if (out[2u * frame] == 0 && out[2u * frame + 1u] == 0) {
            zeros++;
        } else {
            CHECK(out[2u * frame] == TONE && out[2u * frame + 1u] == TONE);
        }
    }
    *last = out[0];
    if (produced != 0u) {
        write_tone(sink, produced);
    }
    return zeros;
}

int main(void)
{
    int16_t last = 0;

    /* 1. Guest at half real time, continuous OFF: the ring runs dry again and again, every cut is counted and the
     * counter equals the silence the device really received (after the first audio). */
    present_audio_sink *sink = make_sink(false);
    CHECK(sink != NULL);
    write_tone(sink, 150u);
    size_t zero_frames = 0u;
    for (int index = 0; index < 600; index++) {
        zero_frames += tick(sink, 10u, &last);
    }
    present_audio_counts counts = present_audio_sink_counts(sink);
    CHECK(counts.rebuffer_events > 3u);
    CHECK(counts.cutout_events > 3u);
    CHECK(counts.cutout_frames == zero_frames);
    CHECK(counts.cutout_max_frames > 0u && counts.cutout_max_frames <= counts.cutout_frames);
    CHECK(counts.slow_pulls == 0u && counts.slow_low_frames == 0u);
    CHECK(zero_frames * 100u > 600u * 20u * 30u / 10u); /* more than 30 percent of the time silent */
    present_audio_sink_close(sink);

    /* 2. The same guest, continuous ON: no silence at all after the start, the playback is slowed instead. */
    sink = make_sink(true);
    CHECK(sink != NULL);
    write_tone(sink, 150u);
    zero_frames = 0u;
    for (int index = 0; index < 600; index++) {
        zero_frames += tick(sink, 10u, &last);
    }
    counts = present_audio_sink_counts(sink);
    CHECK(zero_frames == 0u);
    CHECK(counts.cutout_events == 0u && counts.cutout_frames == 0u);
    CHECK(counts.rebuffer_events == 0u);
    CHECK(counts.slow_pulls > 100u && counts.slow_frames > 1000u);
    CHECK(counts.slow_min_ratio_q16 >= 350u * 65536u / 1000u && counts.slow_min_ratio_q16 < 65536u);
    /* half real time guest: the device consumed about half of what it played (12,000 played, 150 + 6,000 written) */
    CHECK(counts.pulled < 8000u && counts.pulled > 5000u);
    CHECK(last == TONE);
    present_audio_sink_close(sink);

    /* 3. Guest at real time (20 frames per tick), continuous ON: the fill never falls under the low water, the
     * playback is never slowed and the consumption is exactly real time. */
    sink = make_sink(true);
    CHECK(sink != NULL);
    write_tone(sink, 150u);
    for (int index = 0; index < 300; index++) {
        CHECK(tick(sink, 20u, &last) == 0u);
    }
    counts = present_audio_sink_counts(sink);
    CHECK(counts.slow_pulls == 0u && counts.slow_frames == 0u && counts.slow_min_ratio_q16 == 0u);
    CHECK(counts.pulled == 300u * 20u && counts.cutout_events == 0u);
    present_audio_sink_close(sink);

    /* 4. The guest stops: the playback slows down to the floor but never below, then the ring runs dry and ONE cutout
     * run is counted (the pause), the refill resumes it and the run is closed. */
    sink = make_sink(true);
    CHECK(sink != NULL);
    write_tone(sink, 150u);
    zero_frames = 0u;
    for (int index = 0; index < 100; index++) {
        zero_frames += tick(sink, 0u, &last);
    }
    counts = present_audio_sink_counts(sink);
    CHECK(counts.slow_min_ratio_q16 >= 350u * 65536u / 1000u);
    CHECK(counts.slow_pulls > 5u);
    CHECK(counts.rebuffer_events == 1u && counts.cutout_events == 1u);
    CHECK(counts.cutout_frames == zero_frames && zero_frames > 1000u);
    CHECK(counts.cutout_max_frames == counts.cutout_frames);
    for (int index = 0; index < 10; index++) {
        tick(sink, 20u, &last);
    }
    counts = present_audio_sink_counts(sink);
    CHECK(counts.cutout_events == 1u); /* resumed: the run is closed */
    zero_frames = 0u;
    for (int index = 0; index < 20; index++) {
        zero_frames += tick(sink, 20u, &last);
    }
    CHECK(zero_frames == 0u);
    present_audio_sink_close(sink);

    /* 5. Min rate 0 turns it off, a rate above 1000 is clamped (never faster than real time). */
    sink = make_sink(false);
    present_audio_sink_set_continuous_playback(sink, 100u, 0u);
    counts = present_audio_sink_counts(sink);
    CHECK(counts.slow_low_frames == 0u);
    present_audio_sink_set_continuous_playback(sink, 100u, 5000u);
    counts = present_audio_sink_counts(sink);
    CHECK(counts.slow_low_frames == 100u);
    present_audio_sink_close(sink);

    /* 6. Refill after a dry ring: a deep refill target (400 frames) is capped at the low water (100 frames) with
     * continuous playback on, so the pause ends after 120 frames instead of 400. Off: it keeps waiting. */
    for (int mode = 0; mode < 2; mode++) {
        sink = present_audio_sink_open_ring(RATE, 2u);
        present_audio_sink_enable_clock(sink, 400u, 500u, 4u);
        if (mode == 1) {
            present_audio_sink_set_continuous_playback(sink, 100u, 200u);
        }
        write_tone(sink, 500u);
        for (int index = 0; index < 100; index++) {
            tick(sink, 0u, &last); /* drains and runs dry */
        }
        write_tone(sink, 120u);
        int16_t out[2 * 20];
        const size_t got = present_audio_sink_pull(sink, out, 20u);
        CHECK(mode == 1 ? got == 20u : got == 0u);
        present_audio_sink_close(sink);
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
