/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T761: the audio sink's pull model (ring between the timeline producer and the device callback), with a
 * fake device that pulls by hand, then the real SDL3 device on SDL's dummy audio driver (ctest sets
 * SDL_AUDIODRIVER=dummy). The device half SKIPS LOUDLY (exit 77) in a build without SDL3.
 */
#define _DEFAULT_SOURCE 1
#include "present_sink.h"

#include <pthread.h>
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

static void pull_model(void)
{
    present_audio_sink *sink = present_audio_sink_open_ring(100u, 2u); /* ring = 100 frames */
    CHECK(sink != NULL);
    int16_t out[2 * 40];
    int16_t in[2 * 30];
    for (int index = 0; index < 60; index++) {
        in[index] = (int16_t)(index + 1);
    }
    /* before any PCM: silence is idle, not an underrun */
    memset(out, 0x55, sizeof out);
    CHECK(present_audio_sink_pull(sink, out, 10u) == 0u);
    CHECK(out[0] == 0 && out[19] == 0);
    present_audio_counts counts = present_audio_sink_counts(sink);
    CHECK(counts.idle_frames == 10u && counts.underrun_frames == 0u && counts.underrun_events == 0u);

    CHECK(present_audio_sink_write(sink, in, 30u));
    CHECK(present_audio_sink_frames(sink) == 30u);
    CHECK(present_audio_sink_pull(sink, out, 20u) == 20u);
    CHECK(out[0] == 1 && out[1] == 2 && out[39] == 40);
    /* short pull: 10 real frames then silence, counted as one underrun of 30 frames */
    memset(out, 0x55, sizeof out);
    CHECK(present_audio_sink_pull(sink, out, 40u) == 10u);
    CHECK(out[0] == 41 && out[19] == 60 && out[20] == 0 && out[79] == 0);
    counts = present_audio_sink_counts(sink);
    CHECK(counts.written == 30u && counts.pulled == 30u);
    CHECK(counts.underrun_events == 1u && counts.underrun_frames == 30u && counts.idle_frames == 10u);

    /* wrap around the ring and overrun: 100 frame ring, write 70 then 70 */
    int16_t big[2 * 70];
    for (int index = 0; index < 140; index++) {
        big[index] = (int16_t)(1000 + index);
    }
    CHECK(present_audio_sink_write(sink, big, 70u));
    CHECK(present_audio_sink_pull(sink, out, 40u) == 40u);
    CHECK(present_audio_sink_write(sink, big, 70u)); /* fill 30 + 70 = 100, fits exactly */
    counts = present_audio_sink_counts(sink);
    CHECK(counts.overrun_frames == 0u);
    CHECK(present_audio_sink_write(sink, big, 5u)); /* full, the producer never blocks */
    counts = present_audio_sink_counts(sink);
    CHECK(counts.overrun_frames == 5u && counts.written == 30u + 70u + 70u);
    int16_t drain[2 * 100];
    CHECK(present_audio_sink_pull(sink, drain, 100u) == 100u);
    CHECK(drain[0] == 1080 && drain[59] == 1139); /* oldest first across the wrap: frames 40..69 of the first write */
    CHECK(drain[60] == 1000 && drain[199] == 1139); /* then the second write intact */
    CHECK(present_audio_sink_pull(NULL, out, 1u) == 0u);
    CHECK(present_audio_sink_close(sink));
}

/* The reported bug: the producer (virtual clock) bursts far ahead of the device. A fake device thread pulls
 * at a fixed pace; every produced frame must come out, in order, none dropped (dropping skipped the music
 * ahead and sped it up). */
#define BURST_FRAMES 3000u /* 30 ring lengths at 100 frames */
typedef struct {
    present_audio_sink *sink;
    int32_t *heard;
    size_t count;
} fake_device;

static void *fake_device_run(void *opaque)
{
    fake_device *dev = opaque;
    int16_t chunk[2 * 10];
    for (int polls = 0; dev->count < BURST_FRAMES && polls < 20000; polls++) { /* bounded: a dropping sink must fail, not hang */
        size_t real = present_audio_sink_pull(dev->sink, chunk, 10u);
        for (size_t frame = 0; frame < real && dev->count < BURST_FRAMES; frame++) {
            dev->heard[dev->count++] = chunk[frame * 2];
        }
        usleep(200); /* the device clock: far slower than the producer */
    }
    return NULL;
}

static void burst_is_paced(void)
{
    present_audio_sink *sink = present_audio_sink_open_ring(100u, 2u);
    CHECK(sink != NULL);
    present_audio_sink_set_pacing(sink, 5000u);
    static int32_t heard[BURST_FRAMES];
    fake_device dev = {sink, heard, 0u};
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, fake_device_run, &dev) == 0);
    int16_t block[2 * 250];
    for (size_t base = 0; base < BURST_FRAMES; base += 250u) {
        for (size_t frame = 0; frame < 250u; frame++) {
            block[frame * 2] = (int16_t)(base + frame);
            block[frame * 2 + 1] = (int16_t)(base + frame);
        }
        CHECK(present_audio_sink_write(sink, block, 250u)); /* 250 > ring: must wait, not drop */
    }
    pthread_join(thread, NULL);
    present_audio_counts counts = present_audio_sink_counts(sink);
    CHECK(counts.overrun_frames == 0u);
    CHECK(counts.written == BURST_FRAMES && counts.pulled == BURST_FRAMES);
    size_t in_order = 0u;
    for (size_t index = 0; index < dev.count; index++) {
        in_order += heard[index] == (int32_t)index;
    }
    CHECK(dev.count == BURST_FRAMES && in_order == BURST_FRAMES);
    CHECK(present_audio_sink_close(sink));

    /* A device that never pulls must not hang the producer: it drops after the stall timeout, counted. */
    sink = present_audio_sink_open_ring(100u, 2u);
    present_audio_sink_set_pacing(sink, 50u);
    CHECK(present_audio_sink_write(sink, block, 250u));
    counts = present_audio_sink_counts(sink);
    CHECK(counts.written == 100u && counts.overrun_frames == 150u);
    CHECK(present_audio_sink_close(sink));
}

static void refusals(void)
{
    present_audio_kind kind = PRESENT_AUDIO_NONE;
    const char *reason = NULL;
    CHECK(present_audio_sink_open(PRESENT_AUDIO_SDL, NULL, 0u, 2u) == NULL);
    if (!present_audio_device_available()) {
        CHECK(!present_audio_select("sdl", true, &kind, &reason) && kind == PRESENT_AUDIO_NONE);
        CHECK(reason != NULL && strstr(reason, "no SDL3") != NULL);
        CHECK(present_audio_sink_open(PRESENT_AUDIO_SDL, NULL, 48000u, 2u) == NULL);
        return;
    }
    CHECK(present_audio_select("sdl", false, &kind, &reason) && kind == PRESENT_AUDIO_SDL);
    CHECK(strstr(present_audio_announce(PRESENT_AUDIO_SDL), "INFERRED") != NULL);
}

static void device(void)
{
    present_audio_sink *sink = present_audio_sink_open(PRESENT_AUDIO_SDL, NULL, 48000u, 2u);
    if (sink == NULL) {
        const char *why = present_audio_sink_last_error();
        printf("FAIL device open: %s\n", why != NULL ? why : "?");
        failures++;
        return;
    }
    /* Before PCM exists the dummy device drains silence, that is idle, never an underrun. */
    usleep(120000);
    present_audio_counts counts = present_audio_sink_counts(sink);
    CHECK(counts.idle_frames > 0u && counts.underrun_frames == 0u && counts.pulled == 0u);
    int16_t tone[2 * 4800];
    for (int index = 0; index < 4800; index++) {
        tone[2 * index] = (int16_t)(index % 200);
        tone[2 * index + 1] = (int16_t)(-(index % 200));
    }
    CHECK(present_audio_sink_write(sink, tone, 4800u)); /* 100 ms */
    usleep(500000);                                     /* the dummy driver is real time, drains it */
    counts = present_audio_sink_counts(sink);
    CHECK(counts.written == 4800u && counts.pulled == 4800u);
    CHECK(counts.underrun_events >= 1u && counts.underrun_frames > 0u); /* it ran dry after the tone */
    CHECK(present_audio_sink_close(sink));
}

int main(void)
{
    pull_model();
    burst_is_paced();
    refusals();
    if (present_audio_device_available()) {
        device();
    }
    printf("present_audio: %d checks, %d failures\n", checks, failures);
    if (failures == 0 && !present_audio_device_available()) {
        printf("SKIPPED device half: this build has no SDL3\n");
        return 77;
    }
    return failures == 0 ? 0 : 1;
}
