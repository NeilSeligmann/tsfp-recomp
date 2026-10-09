/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1235: the audio sink's latency governor. A ring-only sink at 1000 frames per second so the numbers are exact.
 * The ring fill IS the latency the host adds: the guest writes at its own clock, the device drains at wall rate, so
 * nothing but the governor ever shortens a lead built by a rebuffer pause or a virtual clock burst. Checked here:
 * a lead of tone is shed by a bounded speed-up (media clock follows the frames consumed, constant tone stays
 * constant under the interpolation), digital silence is dropped first and counted, the hysteresis, a short run of
 * zeros inside a waveform is kept, and the governor off leaves the fill alone.
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

enum { RATE = 1000 };

static present_audio_sink *make_sink(uint32_t target_ms)
{
    present_audio_sink *sink = present_audio_sink_open_ring(RATE, 2u);
    if (sink == NULL) {
        return NULL;
    }
    present_audio_sink_enable_clock(sink, 50u, 500u, 4u); /* prefill 50 frames, ring 4 s */
    if (target_ms != 0u) {
        present_audio_sink_set_latency_governor(sink, target_ms, 50u, 50u); /* target, +50 hysteresis, 5% trim */
    }
    return sink;
}

static void write_value(present_audio_sink *sink, int16_t value, size_t frames)
{
    int16_t block[2 * 100];
    while (frames != 0u) {
        const size_t part = frames < 100u ? frames : 100u;
        for (size_t index = 0; index < 2u * part; index++) {
            block[index] = value;
        }
        CHECK(present_audio_sink_write(sink, block, part));
        frames -= part;
    }
}

static size_t fill_of(present_audio_sink *sink)
{
    const present_audio_counts counts = present_audio_sink_counts(sink);
    return (size_t)(counts.written - counts.pulled);
}

int main(void)
{
    int16_t out[2 * 20];

    /* 1. Governor off: a 1000 frame lead stays, the fill only falls by what the device took. */
    present_audio_sink *sink = make_sink(0u);
    CHECK(sink != NULL);
    write_value(sink, 7, 1000u);
    for (int pull = 0; pull < 10; pull++) {
        CHECK(present_audio_sink_pull(sink, out, 20u) == 20u);
    }
    present_audio_counts counts = present_audio_sink_counts(sink);
    CHECK(counts.pulled == 200u && fill_of(sink) == 800u);
    CHECK(counts.latency_target_frames == 0u && counts.latency_trim_frames == 0u && counts.latency_silence_frames == 0u);
    present_audio_sink_close(sink);

    /* 2. Tone lead: above target + hysteresis (100 + 50) the sink consumes up to 5% faster, the output stays the
     * constant tone (linear interpolation of equal samples), the media clock follows the frames consumed. */
    sink = make_sink(100u);
    CHECK(sink != NULL);
    write_value(sink, 7, 1000u);
    present_audio_sink_write_at(sink, out, 0u, UINT64_MAX);
    counts = present_audio_sink_counts(sink);
    CHECK(counts.latency_target_frames == 100u);
    size_t asked = 0u;
    for (int pull = 0; pull < 20; pull++) {
        memset(out, 0, sizeof out);
        CHECK(present_audio_sink_pull(sink, out, 20u) == 20u);
        CHECK(out[0] == 7 && out[39] == 7);
        asked += 20u;
    }
    counts = present_audio_sink_counts(sink);
    CHECK(counts.latency_trim_frames > 0u);
    CHECK(counts.pulled == asked + counts.latency_trim_frames); /* every consumed frame is counted played */
    CHECK(counts.latency_trim_frames <= asked * 50u / 1000u + 20u); /* never above the 5% bound */
    CHECK(fill_of(sink) == 1000u - counts.pulled);
    CHECK(counts.latency_silence_frames == 0u);
    present_audio_sink_close(sink);

    /* 3. Silence first: tone, then a 600 frame gap of digital silence, then tone. Once the tone before the gap is
     * played, the gap is dropped (not played): fill collapses to the target and silence frames are counted. */
    sink = make_sink(100u);
    CHECK(sink != NULL);
    write_value(sink, 7, 100u);
    write_value(sink, 0, 600u);
    write_value(sink, 9, 300u);
    for (int pull = 0; pull < 10; pull++) {
        CHECK(present_audio_sink_pull(sink, out, 20u) == 20u);
    }
    counts = present_audio_sink_counts(sink);
    CHECK(counts.latency_silence_frames >= 590u && counts.latency_silence_frames <= 600u); /* the gap, a few frames of it went to the trim */
    CHECK(fill_of(sink) <= 300u); /* only the 300 tone frames after the gap are left */
    CHECK(counts.pulled == counts.written - fill_of(sink));
    /* the tone after the gap is intact */
    memset(out, 0, sizeof out);
    CHECK(present_audio_sink_pull(sink, out, 20u) == 20u && out[0] == 9);
    present_audio_sink_close(sink);

    /* 4. A short run of zeros (a waveform's own zero samples) is not a gap. */
    sink = make_sink(100u);
    CHECK(sink != NULL);
    write_value(sink, 7, 20u);
    write_value(sink, 0, 100u); /* below the 128 frame run minimum */
    write_value(sink, 9, 900u);
    for (int pull = 0; pull < 6; pull++) {
        CHECK(present_audio_sink_pull(sink, out, 20u) == 20u);
    }
    counts = present_audio_sink_counts(sink);
    CHECK(counts.latency_silence_frames == 0u);
    present_audio_sink_close(sink);

    /* 5. No excess inside target + hysteresis: a fill held at 148 (target 100, hysteresis 50) is never trimmed. */
    sink = make_sink(100u);
    CHECK(sink != NULL);
    write_value(sink, 7, 148u);
    for (int round = 0; round < 200; round++) {
        CHECK(present_audio_sink_pull(sink, out, 20u) == 20u);
        write_value(sink, 7, 20u);
    }
    counts = present_audio_sink_counts(sink);
    CHECK(counts.pulled == 4000u && counts.latency_trim_frames == 0u && counts.latency_silence_frames == 0u);
    present_audio_sink_close(sink);

    /* 6. The trim is bounded: a 3 s lead (twice the full-trim excess) still consumes at most 5% faster. */
    sink = make_sink(100u);
    CHECK(sink != NULL);
    for (int block = 0; block < 3; block++) {
        write_value(sink, 7, 1000u);
    }
    for (int pull = 0; pull < 50; pull++) {
        CHECK(present_audio_sink_pull(sink, out, 20u) == 20u);
    }
    counts = present_audio_sink_counts(sink);
    CHECK(counts.latency_trim_frames >= 40u && counts.latency_trim_frames <= 1000u * 50u / 1000u + 1u);
    present_audio_sink_close(sink);

    printf("present_audio_latency: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
