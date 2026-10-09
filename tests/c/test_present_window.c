/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T760: the SDL3 window sink on SDL's dummy video driver (ctest sets SDL_VIDEODRIVER=dummy, no display is
 * touched). The window is real, the renderer is SDL's, and the pixels are read back from the window after
 * the present. Built without SDL3 the test SKIPS LOUDLY (exit 77, ctest reports Skipped).
 */
#define _POSIX_C_SOURCE 200809L
#include "present_sink.h"

#include <pthread.h>
#include <stdio.h>
#include <time.h>
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

enum { WIDTH = 64, HEIGHT = 48 };

static void fill(uint8_t *rgb, uint8_t red, uint8_t green, uint8_t blue)
{
    for (size_t pixel = 0; pixel < (size_t)WIDTH * HEIGHT; pixel++) {
        rgb[pixel * 3] = red;
        rgb[pixel * 3 + 1] = green;
        rgb[pixel * 3 + 2] = blue;
    }
}

/* The window opens 640 x 480 and the picture is letterboxed, so the centre is always inside it. */
static bool centre(present_video_sink *sink, uint8_t rgb[3])
{
    return present_video_sink_read_pixel(sink, 320u, 240u, rgb);
}

static bool near_colour(const uint8_t got[3], uint8_t red, uint8_t green, uint8_t blue)
{
    return abs((int)got[0] - red) <= 2 && abs((int)got[1] - green) <= 2 && abs((int)got[2] - blue) <= 2;
}

/* The host's guest threads call the hooks, not the thread that opened the window. */
typedef struct {
    present_video_sink *sink;
    const uint8_t *picture;
} guest_args;

static void *guest_thread(void *argument)
{
    guest_args *args = argument;
    for (uint64_t number = 100u; number < 105u; number++) {
        present_video_sink_submit(args->sink, number, WIDTH, HEIGHT, args->picture);
        present_video_sink_vblank(args->sink);
    }
    return NULL;
}

static int64_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int main(void)
{
    if (!present_window_available()) {
        printf("SKIPPED present_window: this build has no SDL3 (headless default build, TSFP_HAVE_SDL3 is not "
               "defined). Install libsdl3-dev and reconfigure to run it.\n");
        return 77;
    }
    const char *error = NULL;
    present_video_sink *sink = present_video_sink_open(PRESENT_VIDEO_WINDOW, "tsfp test", &error);
    if (sink == NULL) {
        printf("FAIL window could not open on the dummy driver: %s\n", error != NULL ? error : "(no text)");
        return 1;
    }
    uint8_t *red = malloc((size_t)WIDTH * HEIGHT * 3u);
    uint8_t *blue = malloc((size_t)WIDTH * HEIGHT * 3u);
    CHECK(red != NULL && blue != NULL);
    fill(red, 220, 20, 30);
    fill(blue, 10, 40, 200);
    uint8_t got[3] = {99, 99, 99};

    /* Before any vblank nothing has been presented: black. Picture latched, still not shown. */
    CHECK(centre(sink, got) && near_colour(got, 0, 0, 0));
    present_video_sink_submit(sink, 0u, WIDTH, HEIGHT, red);
    CHECK(centre(sink, got) && near_colour(got, 0, 0, 0)); /* the overlay shows one vblank late */
    present_video_sink_vblank(sink);
    CHECK(centre(sink, got) && near_colour(got, 220, 20, 30));
    present_video_counts counts = present_video_sink_counts(sink);
    CHECK(counts.submitted == 1u && counts.vblanks == 1u && counts.presented == 1u);

    /* A vblank with no new picture presents nothing new. */
    present_video_sink_vblank(sink);
    counts = present_video_sink_counts(sink);
    CHECK(counts.vblanks == 2u && counts.presented == 1u);
    CHECK(centre(sink, got) && near_colour(got, 220, 20, 30));

    /* Two pictures in one vblank interval: one picture per vblank, the later one wins. */
    present_video_sink_submit(sink, 1u, WIDTH, HEIGHT, blue);
    present_video_sink_submit(sink, 2u, WIDTH, HEIGHT, red);
    present_video_sink_submit(sink, 3u, WIDTH, HEIGHT, blue);
    present_video_sink_vblank(sink);
    counts = present_video_sink_counts(sink);
    CHECK(counts.submitted == 4u && counts.presented == 2u && counts.last_number == 3u);
    CHECK(centre(sink, got) && near_colour(got, 10, 40, 200));

    /* A failed picture keeps the last good one on screen and is counted. */
    present_video_sink_submit(sink, 4u, 0u, 0u, NULL);
    present_video_sink_vblank(sink);
    counts = present_video_sink_counts(sink);
    CHECK(counts.failed == 1u && counts.presented == 2u);
    CHECK(centre(sink, got) && near_colour(got, 10, 40, 200));

    /* A corner outside the letterboxed 4:3 picture stays black on a 640 x 480 window it fits exactly: the
     * picture fills the window, so the corner has the picture's colour. */
    CHECK(present_video_sink_read_pixel(sink, 2u, 2u, got) && near_colour(got, 10, 40, 200));

    /* hold returns after its time, and presents a picture latched after the last vblank. */
    present_video_sink_submit(sink, 5u, WIDTH, HEIGHT, red);
    present_video_sink_hold(sink, 30u);
    counts = present_video_sink_counts(sink);
    CHECK(counts.presented == 3u);
    CHECK(centre(sink, got) && near_colour(got, 220, 20, 30));
    /* Letterbox: a 64 x 24 picture (8:3) in the 640 x 480 window is 640 x 240 centred, the bars stay black. */
    uint8_t *wide = malloc((size_t)WIDTH * 24u * 3u);
    CHECK(wide != NULL);
    for (size_t pixel = 0; pixel < (size_t)WIDTH * 24u; pixel++) {
        wide[pixel * 3] = 220;
        wide[pixel * 3 + 1] = 20;
        wide[pixel * 3 + 2] = 30;
    }
    present_video_sink_submit(sink, 6u, WIDTH, 24u, wide);
    present_video_sink_vblank(sink);
    CHECK(centre(sink, got) && near_colour(got, 220, 20, 30));
    CHECK(present_video_sink_read_pixel(sink, 320u, 10u, got) && near_colour(got, 0, 0, 0));
    CHECK(present_video_sink_read_pixel(sink, 320u, 470u, got) && near_colour(got, 0, 0, 0));
    CHECK(present_video_sink_read_pixel(sink, 5u, 240u, got) && near_colour(got, 220, 20, 30));
    free(wide);

    /* capture: the window's frame as a BMP, a real file with the BMP magic and a plausible size */
    const char *shot = "present_window_test.bmp";
    CHECK(present_video_sink_capture(sink, shot));
    FILE *file = fopen(shot, "rb");
    unsigned char header[2] = {0, 0};
    CHECK(file != NULL && fread(header, 1, 2, file) == 2 && header[0] == 'B' && header[1] == 'M');
    if (file != NULL) {
        fseek(file, 0, SEEK_END);
        CHECK(ftell(file) > 640L * 480L * 3L / 2L);
        fclose(file);
    }
    remove(shot);
    CHECK(!present_video_sink_capture(sink, NULL));

    /* Root cause of the black window on a real display (OpenGL and Wayland bind the renderer to the creating
     * thread, a present from a guest thread failed with "window has not been made current"): every SDL call
     * must run on the presenter thread, wherever the hook is called from. */
    {
        const present_video_counts before = present_video_sink_counts(sink);
        guest_args args = {sink, blue};
        pthread_t guest;
        CHECK(pthread_create(&guest, NULL, guest_thread, &args) == 0);
        pthread_join(guest, NULL);
        const present_video_counts after = present_video_sink_counts(sink);
        CHECK(after.presented == before.presented + 5u);
        CHECK(after.presented_nonblack == before.presented_nonblack + 5u);
        CHECK(after.off_thread_sdl_calls == 0u && after.present_errors == 0u);
        CHECK(centre(sink, got) && near_colour(got, 10, 40, 200));
        /* a black picture counts as presented but not as non-black */
        uint8_t *black = calloc((size_t)WIDTH * HEIGHT * 3u, 1u);
        CHECK(black != NULL);
        present_video_sink_submit(sink, 200u, WIDTH, HEIGHT, black);
        present_video_sink_vblank(sink);
        const present_video_counts dark = present_video_sink_counts(sink);
        CHECK(dark.presented == after.presented + 1u && dark.presented_nonblack == after.presented_nonblack);
        free(black);
        /* the report line a user can paste from a host whose display cannot be seen */
        char line[320];
        CHECK(present_video_sink_describe(sink, line, sizeof line) > 0u);
        CHECK(strstr(line, "SDL renderer ") != NULL && strstr(line, "texture 64x48") != NULL &&
              strstr(line, "non-black") != NULL && strstr(line, "SDL errors 0") != NULL);
        CHECK(strstr(line, "window output 640x480") != NULL);
        printf("describe: %s\n", line);
    }

    /* Pacing: 50 vblanks at 500 Hz take at least 90 ms of wall time, unpaced they take about nothing, and a
     * stall does not make the next vblanks burst to catch up. */
    {
        int64_t start = now_ms();
        for (int i = 0; i < 50; i++) present_video_sink_vblank(sink);
        const int64_t unpaced = now_ms() - start;
        present_video_sink_set_pace(sink, 500000u);
        start = now_ms();
        for (int i = 0; i < 50; i++) present_video_sink_vblank(sink);
        const int64_t paced = now_ms() - start;
        CHECK(paced >= 90);
        CHECK(unpaced < paced);
        const struct timespec stall = {0, 400 * 1000 * 1000};
        nanosleep(&stall, NULL);
        start = now_ms();
        for (int i = 0; i < 25; i++) present_video_sink_vblank(sink);
        CHECK(now_ms() - start >= 40); /* 25 slots of 2 ms from the resync, no burst of 200 late slots */
        CHECK(now_ms() - start < 300);
        present_video_sink_set_pace(sink, 0u);
    }

    /* T908: audio clock lifetime ends before final video flush/hold. Detaching under
     * the sink lock prevents presenter jobs querying a freed audio sink. */
    present_audio_sink *audio = present_audio_sink_open(PRESENT_AUDIO_NULL, NULL, 48000u, 2u);
    CHECK(audio != NULL);
    present_video_sink_set_playback(sink, audio);
    CHECK(present_video_sink_playback_enabled(sink));
    present_video_sink_set_playback(sink, NULL);
    CHECK(!present_video_sink_playback_enabled(sink));
    CHECK(present_audio_sink_close(audio));
    present_video_sink_hold(sink, 0u);
    present_video_sink_close(sink);
    free(red);
    free(blue);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
