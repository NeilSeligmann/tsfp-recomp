/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T819: the window sink's playback mode on SDL's dummy video driver. Pictures carry a modelled timestamp, wait in a
 * bounded queue and are shown by the presenter thread when the audio clock reaches them: the audio clock is the
 * master. A ring-only audio sink at 1000 frames per second plays the clock by hand (pull). Without SDL3 the test
 * SKIPS LOUDLY (exit 77).
 */
#define _DEFAULT_SOURCE 1
#include "present_sink.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

enum { WIDTH = 64, HEIGHT = 48, RATE = 1000 };
#define MS 1000000ull

static void fill(uint8_t *rgb, uint8_t red, uint8_t green, uint8_t blue)
{
    for (size_t pixel = 0; pixel < (size_t)WIDTH * HEIGHT; pixel++) {
        rgb[pixel * 3] = red;
        rgb[pixel * 3 + 1] = green;
        rgb[pixel * 3 + 2] = blue;
    }
}

static bool near_colour(const uint8_t got[3], uint8_t red, uint8_t green, uint8_t blue)
{
    return abs((int)got[0] - red) <= 2 && abs((int)got[1] - green) <= 2 && abs((int)got[2] - blue) <= 2;
}

static int64_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* Poll until `presented` pictures have been shown or a second has passed. */
static bool wait_presented(present_video_sink *sink, uint64_t presented)
{
    const int64_t until = now_ms() + 1500;
    while (now_ms() < until) {
        if (present_video_sink_counts(sink).presented >= presented) {
            return true;
        }
        usleep(2000);
    }
    return false;
}

static bool centre_is(present_video_sink *sink, uint8_t red, uint8_t green, uint8_t blue)
{
    uint8_t got[3] = {99, 99, 99};
    return present_video_sink_read_pixel(sink, 320u, 240u, got) && near_colour(got, red, green, blue);
}

/* Advance the audio clock to at least `frames` played by pulling in small chunks. */
static void play_to(present_audio_sink *audio, uint64_t frames)
{
    int16_t out[2 * 50];
    while (present_audio_sink_counts(audio).pulled < frames) {
        (void)present_audio_sink_pull(audio, out, 50u);
        usleep(500);
    }
}

typedef struct {
    present_video_sink *sink;
    const uint8_t *picture;
    unsigned count;
    uint64_t base_ms; /* the stamp of picture n is base + 100 ms * (n + 1) */
} producer_args;

typedef struct {
    present_video_sink *sink;
    bool playback;
    bool clock_valid;
    uint64_t media_time_ns;
} media_query;

static void query_media_clock(void *context)
{
    media_query *query = context;
    query->playback = present_video_sink_job_playback_enabled(query->sink);
    query->clock_valid = present_video_sink_job_media_time(query->sink, &query->media_time_ns);
}

static void *producer(void *argument)
{
    producer_args *args = argument;
    for (unsigned index = 0; index < args->count; index++) {
        present_video_sink_submit_at(args->sink, 1000u + index, WIDTH, HEIGHT, args->picture,
                                     (args->base_ms + (uint64_t)(index + 1u) * 100u) * MS);
        present_video_sink_vblank(args->sink);
    }
    return NULL;
}

int main(void)
{
    if (!present_window_available()) {
        printf("SKIPPED present_playback: this build has no SDL3 (headless default build, TSFP_HAVE_SDL3 is not "
               "defined). Install libsdl3-dev and reconfigure to run it.\n");
        return 77;
    }
    const char *error = NULL;
    present_video_sink *video = present_video_sink_open(PRESENT_VIDEO_WINDOW, "tsfp playback test", &error);
    present_audio_sink *audio = present_audio_sink_open_ring(RATE, 2u);
    CHECK(video != NULL && audio != NULL);
    if (video == NULL || audio == NULL) {
        printf("FAIL could not open the sinks: %s\n", error != NULL ? error : "(no text)");
        return 1;
    }
    uint8_t *red = malloc((size_t)WIDTH * HEIGHT * 3u);
    uint8_t *blue = malloc((size_t)WIDTH * HEIGHT * 3u);
    CHECK(red != NULL && blue != NULL);
    fill(red, 220, 20, 30);
    fill(blue, 10, 40, 200);

    present_audio_sink_enable_clock(audio, 10u, 10u, 30u); /* 10 frame prefill: playback starts at once */
    present_video_sink_set_playback(video, audio);
    CHECK(present_video_sink_playback_enabled(video));
    media_query clock_query = {.sink = video};
    CHECK(present_video_sink_run(video, query_media_clock, &clock_query));
    CHECK(clock_query.playback && !clock_query.clock_valid);
    present_video_sink_set_pace(video, 59940u); /* must NOT make the vblank hook sleep in playback mode */

    /* The vblank hook never waits for the wall clock: 40 hooks at 59.94 Hz pacing would take 667 ms. */
    int64_t start = now_ms();
    for (int count = 0; count < 40; count++) {
        present_video_sink_vblank(video);
    }
    CHECK(now_ms() - start < 150);
    CHECK(present_video_sink_counts(video).vblanks == 40u && present_video_sink_counts(video).presented == 0u);

    /* No media clock yet (nothing was written to the audio sink): the newest picture is shown at once. */
    present_video_sink_submit_at(video, 99u, WIDTH, HEIGHT, blue, 5000u * MS);
    CHECK(wait_presented(video, 1u));
    CHECK(centre_is(video, 10, 40, 200));

    /* Audio: 20 s at 1000 frames per second, anchored at modelled time 0. Nothing is played yet. */
    int16_t silence[2 * 1000];
    memset(silence, 0, sizeof silence);
    for (int second = 1; second <= 20; second++) {
        CHECK(present_audio_sink_write_at(audio, silence, 1000u, (uint64_t)second * 1000u * MS));
    }

    /* A picture stamped 0 is due at once, the later ones wait for the audio clock. */
    present_video_sink_submit_at(video, 0u, WIDTH, HEIGHT, red, 0u);
    present_video_sink_submit_at(video, 1u, WIDTH, HEIGHT, blue, 300u * MS);
    present_video_sink_submit_at(video, 2u, WIDTH, HEIGHT, red, 600u * MS);
    CHECK(wait_presented(video, 2u));
    CHECK(centre_is(video, 220, 20, 30));
    usleep(120000); /* the clock stands at 0 because nothing was pulled: the 300 ms picture must still wait */
    present_video_counts counts = present_video_sink_counts(video);
    CHECK(counts.submitted == 4u && counts.presented == 2u);
    CHECK(centre_is(video, 220, 20, 30));

    /* The device plays 350 ms: the blue picture appears, the 600 ms one does not. */
    play_to(audio, 350u);
    memset(&clock_query, 0, sizeof clock_query);
    clock_query.sink = video;
    CHECK(present_video_sink_run(video, query_media_clock, &clock_query));
    CHECK(clock_query.playback && clock_query.clock_valid && clock_query.media_time_ns > 0u);
    CHECK(wait_presented(video, 3u));
    CHECK(centre_is(video, 10, 40, 200));
    usleep(60000);
    CHECK(present_video_sink_counts(video).presented == 3u);
    /* 650 ms: the third. */
    play_to(audio, 650u);
    CHECK(wait_presented(video, 4u));
    CHECK(centre_is(video, 220, 20, 30));

    /* A clock that jumped past several pictures shows the newest due one and drops the others. */
    present_video_sink_submit_at(video, 3u, WIDTH, HEIGHT, red, 700u * MS);
    present_video_sink_submit_at(video, 4u, WIDTH, HEIGHT, red, 710u * MS);
    present_video_sink_submit_at(video, 5u, WIDTH, HEIGHT, blue, 720u * MS);
    play_to(audio, 1000u);
    CHECK(wait_presented(video, 5u));
    usleep(80000);
    CHECK(present_video_sink_counts(video).presented == 5u);
    CHECK(centre_is(video, 10, 40, 200)); /* the NEWEST due picture, not the oldest (red) */
    present_video_timing timing = present_video_sink_timing(video);
    CHECK(timing.dropped == 2u && timing.queued_at_end == 0u);
    CHECK(timing.late_shown >= 1u); /* the clock was 280 ms past the shown picture */
    /* T827: the longest modelled time between two submitted pictures (the title's own pause) is reported with the
     * picture before it. A picture stamped earlier than its predecessor (99 at 5 s, then 0) is no gap. */
    CHECK(timing.model_gap_max_ns == 300u * MS && timing.model_gap_after == 0u);

    /* Bounded run-ahead: a guest producing pictures stamped 100 ms apart fills the queue and then WAITS. */
    producer_args args = {video, red, 150u, 1000u}; /* all after the 1 s the clock has reached */
    pthread_t guest;
    CHECK(pthread_create(&guest, NULL, producer, &args) == 0);
    const int64_t until = now_ms() + 2000;
    while (now_ms() < until && present_video_sink_timing(video).queue_waits == 0u) {
        usleep(2000);
    }
    timing = present_video_sink_timing(video);
    CHECK(timing.queue_waits >= 1u && timing.queue_max == 96u);
    /* ... the picture count it managed is bounded by the queue, not 150. */
    counts = present_video_sink_counts(video);
    CHECK(counts.submitted == 7u + 96u + 1u); /* the earlier six, a full queue, and the one that is waiting */
    /* The audio clock frees the queue and the guest finishes without a single timeout. */
    play_to(audio, 16500u);
    CHECK(pthread_join(guest, NULL) == 0);
    play_to(audio, 18000u);
    usleep(200000);
    timing = present_video_sink_timing(video);
    CHECK(timing.queue_timeouts == 0u);
    counts = present_video_sink_counts(video);
    CHECK(counts.submitted == 7u + 150u);
    CHECK(timing.queued_at_end == 0u);
    CHECK(counts.presented + timing.dropped == counts.submitted); /* every picture was shown or dropped, none lost */

    /* The stop (hold) shows the newest queued picture even though the clock has not reached it. */
    present_video_sink_submit_at(video, 7000u, WIDTH, HEIGHT, blue, 900000u * MS);
    present_video_sink_submit_at(video, 7001u, WIDTH, HEIGHT, blue, 900100u * MS);
    timing = present_video_sink_timing(video);
    CHECK(timing.model_gap_max_ns >= 800000u * MS && timing.model_gap_after == 1149u);
    const uint64_t presented_before = present_video_sink_counts(video).presented;
    present_video_sink_hold(video, 30u);
    CHECK(present_video_sink_counts(video).presented == presented_before + 1u);
    CHECK(present_video_sink_timing(video).queued_at_end == 0u);

    /* A clock that never moves does not hold the guest for ever: the wait gives up after a second, the oldest
     * picture is dropped and counted. 97 pictures into a 96 picture queue: exactly one wait. */
    producer_args stuck = {video, red, 97u, 2000000u};
    const present_video_timing before = present_video_sink_timing(video);
    start = now_ms();
    pthread_t second_guest;
    CHECK(pthread_create(&second_guest, NULL, producer, &stuck) == 0);
    CHECK(pthread_join(second_guest, NULL) == 0);
    const int64_t waited = now_ms() - start;
    timing = present_video_sink_timing(video);
    CHECK(waited >= 900 && waited < 3000);
    CHECK(timing.queue_timeouts == 1u && timing.dropped == before.dropped + 1u);

    present_video_sink_close(video);
    CHECK(present_audio_sink_close(audio));
    free(red);
    free(blue);
    printf("present_playback: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
