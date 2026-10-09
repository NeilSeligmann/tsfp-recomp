/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T819: the audio sink's clock mode (the media clock the window follows). A ring-only sink at 1000 frames per
 * second so the numbers are exact: prefill gating, the rebuffer pause with a refill target adapted from the guest's
 * measured speed (T827, between a floor and the maximum), the kick, the anchor from the first stamped write, and a media clock that stands still while the
 * device gets silence.
 */
#define _DEFAULT_SOURCE 1
#include "present_sink.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

int main(void)
{
    present_audio_sink *sink = present_audio_sink_open_ring(RATE, 2u);
    CHECK(sink != NULL);
    if (sink == NULL) {
        return 1;
    }
    present_audio_sink_enable_clock(sink, 100u, 1000u, 2u); /* prefill 100 frames, at most 1000, ring 2 s */
    present_audio_counts counts = present_audio_sink_counts(sink);
    CHECK(counts.ring_frames == 2000u && counts.prefill_frames == 100u);

    int16_t in[2 * 1000];
    int16_t out[2 * 1000];
    for (size_t index = 0; index < 2u * 1000u; index++) {
        in[index] = 7;
    }
    uint64_t media = 0u;
    CHECK(!present_audio_sink_media_time(sink, &media)); /* no stamped write yet */

    /* The first stamped write anchors the clock at the START of its frames: end 10 s, 60 frames = 60 ms. */
    const uint64_t anchor = 10000000000ull - 60000000ull;
    CHECK(present_audio_sink_write_at(sink, in, 60u, 10000000000ull));
    CHECK(present_audio_sink_media_time(sink, &media) && media == anchor);
    /* An unstamped write never moves the anchor. */
    CHECK(present_audio_sink_write(sink, in, 10u));
    CHECK(present_audio_sink_media_time(sink, &media) && media == anchor);

    /* 70 frames are below the 100 frame prefill: the device gets silence and the clock does not move. */
    memset(out, 0x55, sizeof out);
    CHECK(present_audio_sink_pull(sink, out, 20u) == 0u);
    CHECK(out[0] == 0 && out[39] == 0);
    counts = present_audio_sink_counts(sink);
    CHECK(counts.pulled == 0u && counts.rebuffer_frames == 20u && counts.underrun_frames == 0u);
    CHECK(present_audio_sink_media_time(sink, &media) && media == anchor);

    /* Reaching the prefill starts playback. */
    CHECK(present_audio_sink_write(sink, in, 40u)); /* fill 110 */
    CHECK(present_audio_sink_pull(sink, out, 50u) == 50u);
    CHECK(out[0] == 7 && out[99] == 7);
    /* The clock moves smoothly inside the chunk: just after the pull it has not jumped a whole 50 ms ahead. */
    CHECK(present_audio_sink_media_time(sink, &media) && media < anchor + 25000000ull);
    usleep(120000); /* longer than the 50 frame (50 ms) chunk: the clock has played all of it */
    CHECK(present_audio_sink_media_time(sink, &media) && media == anchor + 50000000ull);

    /* Running dry: 60 frames left, 100 asked. Playback pauses, no underrun is counted. The refill target is adapted
     * from the guest's measured speed when the stall ENDS, so it is still the configured 100 frames here. */
    memset(out, 0x55, sizeof out);
    CHECK(present_audio_sink_pull(sink, out, 100u) == 60u);
    CHECK(out[0] == 7 && out[2 * 60] == 0);
    counts = present_audio_sink_counts(sink);
    CHECK(counts.rebuffer_events == 1u && counts.prefill_frames == 100u);
    CHECK(counts.underrun_events == 0u && counts.underrun_frames == 0u);
    usleep(120000);
    CHECK(present_audio_sink_media_time(sink, &media) && media == anchor + 110000000ull);

    /* The pause holds until the 100 frame target is queued. */
    CHECK(present_audio_sink_write(sink, in, 50u));
    CHECK(present_audio_sink_pull(sink, out, 10u) == 0u);
    usleep(30000);
    CHECK(present_audio_sink_media_time(sink, &media) && media == anchor + 110000000ull); /* still held */
    CHECK(present_audio_sink_write(sink, in, 60u)); /* fill 110 */
    CHECK(present_audio_sink_pull(sink, out, 10u) == 10u);
    /* About 110 frames arrived in about 160 ms of stall: the next target is what that speed delivers in 600 ms, far
     * from both the 200 frame floor and the 1000 frame maximum. */
    counts = present_audio_sink_counts(sink);
    CHECK(counts.prefill_frames >= 250u && counts.prefill_frames <= 700u);

    /* A slow guest (60 frames in 300 ms, a fifth of real time) gets the floor, 200 frames, not a long wait. */
    CHECK(present_audio_sink_pull(sink, out, 100u) == 100u);
    CHECK(present_audio_sink_pull(sink, out, 50u) == 0u); /* dry */
    usleep(300000);
    CHECK(present_audio_sink_write(sink, in, 60u));
    CHECK(present_audio_sink_pull(sink, out, 10u) == 0u); /* 60 is below the target: held */
    present_audio_sink_kick(sink);                        /* the refill is ended by hand */
    CHECK(present_audio_sink_counts(sink).prefill_frames == 200u);

    /* A fast guest (300 frames the instant the stall begins, too brief to measure) gets the maximum, 1000 frames. */
    CHECK(present_audio_sink_pull(sink, out, 100u) == 60u); /* dry */
    CHECK(present_audio_sink_write(sink, in, 300u));        /* 300 >= the 200 target: resumes at once */
    CHECK(present_audio_sink_counts(sink).prefill_frames == 1000u);

    /* A middle guest: 100 frames in 200 ms is half of real time, 300 frames in 600 ms. */
    CHECK(present_audio_sink_pull(sink, out, 400u) == 300u); /* dry */
    usleep(200000);
    CHECK(present_audio_sink_write(sink, in, 100u));
    present_audio_sink_kick(sink);
    counts = present_audio_sink_counts(sink);
    CHECK(counts.prefill_frames >= 250u && counts.prefill_frames <= 320u);

    /* The stall log: the start, then each dry ring with how it ended and how long it lasted. */
    present_audio_stall log[PRESENT_AUDIO_STALL_MAX];
    CHECK(present_audio_sink_stalls(sink, log, PRESENT_AUDIO_STALL_MAX) == 5u);
    CHECK(log[0].initial && log[0].reason == 1u);
    CHECK(!log[1].initial && log[1].reason == 1u && log[1].fill_start == 0u && log[1].fill_end == 110u);
    CHECK(log[1].duration_ms >= 150u && log[1].writes_during == 2u);
    CHECK(log[2].reason == 3u && log[2].duration_ms >= 290u && log[2].fill_end == 60u && log[2].writes_during == 1u);
    CHECK(log[3].reason == 1u && log[3].duration_ms < 20u && log[3].target_frames == 200u);
    CHECK(log[4].reason == 3u && log[4].duration_ms >= 190u && log[4].fill_end == 100u);

    /* kick: a refill that cannot complete (the producer is blocked elsewhere) is started by hand. */
    CHECK(present_audio_sink_pull(sink, out, 1000u) == 100u); /* dry */
    CHECK(present_audio_sink_write(sink, in, 5u));
    CHECK(present_audio_sink_pull(sink, out, 5u) == 0u);
    present_audio_sink_kick(sink);
    CHECK(present_audio_sink_pull(sink, out, 5u) == 5u);
    CHECK(out[0] == 7);

    /* A kick with an empty ring does nothing (there is nothing to play). */
    present_audio_sink_kick(sink);
    CHECK(present_audio_sink_pull(sink, out, 5u) == 0u);

    /* The sink without clock mode is the T802 sink: the same dry spell is an underrun, never a pause. */
    present_audio_sink *plain = present_audio_sink_open_ring(RATE, 2u);
    CHECK(plain != NULL);
    uint64_t unused = 0u;
    CHECK(!present_audio_sink_media_time(plain, &unused));
    CHECK(present_audio_sink_write_at(plain, in, 20u, 1000000000ull));
    CHECK(!present_audio_sink_media_time(plain, &unused));
    CHECK(present_audio_sink_pull(plain, out, 30u) == 20u);
    counts = present_audio_sink_counts(plain);
    CHECK(counts.underrun_events == 1u && counts.underrun_frames == 10u && counts.rebuffer_events == 0u);

    /* enable_clock after the first write is ignored (the ring is already in use). */
    present_audio_sink_enable_clock(plain, 100u, 400u, 4u);
    CHECK(present_audio_sink_counts(plain).ring_frames != 4000u);

    /* The measured target is capped at the configured maximum: 100 frames after 60 ms is far above 150 frames per
     * 600 ms, the target is 150. */
    present_audio_sink *capped = present_audio_sink_open_ring(RATE, 2u);
    CHECK(capped != NULL);
    present_audio_sink_enable_clock(capped, 100u, 150u, 2u);
    CHECK(present_audio_sink_write_at(capped, in, 100u, 1000000000ull));
    CHECK(present_audio_sink_pull(capped, out, 100u) == 100u);
    CHECK(present_audio_sink_pull(capped, out, 10u) == 0u); /* dry */
    usleep(60000);
    CHECK(present_audio_sink_write(capped, in, 100u)); /* the target is still 100: playback resumes */
    CHECK(present_audio_sink_counts(capped).prefill_frames == 150u);
    /* A refill that never reaches its target is not waited for for ever: after 700 ms it plays what it has. */
    CHECK(present_audio_sink_pull(capped, out, 400u) == 100u); /* dry */
    CHECK(present_audio_sink_write(capped, in, 10u));
    CHECK(present_audio_sink_pull(capped, out, 5u) == 0u); /* 10 frames, target 150: held */
    usleep(780000);
    CHECK(present_audio_sink_pull(capped, out, 5u) == 5u); /* gave up waiting: resumed with what it had */
    present_audio_stall capped_log[PRESENT_AUDIO_STALL_MAX];
    const size_t capped_total = present_audio_sink_stalls(capped, capped_log, PRESENT_AUDIO_STALL_MAX);
    CHECK(capped_total == 2u && capped_log[1].reason == 2u && capped_log[1].duration_ms >= 700u);
    CHECK(present_audio_sink_close(capped));
    CHECK(present_audio_sink_close(plain));
    CHECK(present_audio_sink_close(sink));

    printf("present_audio_clock: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
