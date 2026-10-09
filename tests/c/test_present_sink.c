/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T744: the presentation interface (src/host/present_sink.h). Headless: null sink, the png-dir
 * prerequisite, refusals for the window and the audio device and for wav-file while the HLE emits no
 * PCM, and the wav-file writer fed synthetic samples (the writer is NOT fed by the HLE today).
 */
#include "host_options.h"
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

static bool has(const char *text, const char *needle)
{
    return text != NULL && strstr(text, needle) != NULL;
}

static void test_video_select(void)
{
    present_video_kind kind = PRESENT_VIDEO_WINDOW;
    const char *reason = "x";
    CHECK(present_video_select(NULL, false, &kind, &reason) && kind == PRESENT_VIDEO_NONE && reason == NULL);
    CHECK(present_video_select("null", false, &kind, &reason) && kind == PRESENT_VIDEO_NULL);
    CHECK(present_video_select("png-dir", true, &kind, &reason) && kind == PRESENT_VIDEO_PNG_DIR);
    /* negative controls */
    CHECK(!present_video_select("png-dir", false, &kind, &reason) && kind == PRESENT_VIDEO_NONE &&
          has(reason, "--dump-overlay"));
    if (present_window_available()) {
        /* T760: built with SDL3, the named refusal is gone. */
        CHECK(present_video_select("window", false, &kind, &reason) && kind == PRESENT_VIDEO_WINDOW &&
              reason == NULL);
        CHECK(has(present_video_announce(PRESENT_VIDEO_WINDOW), "INFERRED"));
    } else {
        CHECK(!present_video_select("window", true, &kind, &reason) && kind == PRESENT_VIDEO_NONE &&
              has(reason, "SDL3") && has(reason, "owner decision") && has(reason, "no SDL3"));
        CHECK(present_video_sink_open(PRESENT_VIDEO_WINDOW, "t", &reason) == NULL && has(reason, "no SDL3"));
    }
    CHECK(!present_video_select("bogus", true, &kind, &reason) && has(reason, "unknown"));
    CHECK(!present_video_select("", true, &kind, &reason));
    CHECK(strcmp(present_video_name(PRESENT_VIDEO_PNG_DIR), "png-dir") == 0);
    CHECK(has(present_video_announce(PRESENT_VIDEO_PNG_DIR), "opt-in"));
    CHECK(present_video_announce(PRESENT_VIDEO_NONE)[0] == '\0');
}

static void test_counting_sinks(void)
{
    const uint8_t pixel[3 * 3] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    const present_video_kind kinds[2] = {PRESENT_VIDEO_NULL, PRESENT_VIDEO_PNG_DIR};
    CHECK(present_video_sink_open(PRESENT_VIDEO_NONE, "t", NULL) == NULL);
    for (size_t index = 0; index < 2; index++) {
        const char *error = "x";
        present_video_sink *sink = present_video_sink_open(kinds[index], "t", &error);
        CHECK(sink != NULL && error == NULL);
        present_video_counts counts = present_video_sink_counts(sink);
        CHECK(counts.submitted == 0u && counts.vblanks == 0u);
        present_video_sink_submit(sink, 0u, 3u, 1u, pixel);
        present_video_sink_submit(sink, 1u, 3u, 1u, pixel);
        present_video_sink_submit(sink, 2u, 0u, 0u, NULL); /* an unbuildable picture still counts */
        present_video_sink_vblank(sink);
        present_video_sink_vblank(sink);
        counts = present_video_sink_counts(sink);
        CHECK(counts.submitted == 3u && counts.failed == 1u && counts.vblanks == 2u &&
              counts.last_number == 2u);
        CHECK(counts.presented == 0u); /* nothing is shown by a counting sink */
        uint8_t color[3];
        CHECK(!present_video_sink_read_pixel(sink, 0u, 0u, color)); /* no window */
        CHECK(!present_video_sink_capture(sink, "never-written.bmp"));
        present_video_sink_hold(sink, 5000u);                       /* returns at once, no window */
        present_video_sink_close(sink);
    }
    present_video_counts none = present_video_sink_counts(NULL);
    CHECK(none.submitted == 0u);
    present_video_sink_submit(NULL, 0u, 1u, 1u, pixel); /* a NULL sink is a no-op, not a crash */
    present_video_sink_vblank(NULL);
}

static void test_audio_select(void)
{
    present_audio_kind kind = PRESENT_AUDIO_DEVICE;
    const char *reason = "x";
    CHECK(present_audio_select(NULL, false, &kind, &reason) && kind == PRESENT_AUDIO_NONE);
    CHECK(present_audio_select("null", false, &kind, &reason) && kind == PRESENT_AUDIO_NULL);
    CHECK(!present_audio_select("wav-file", false, &kind, &reason) && kind == PRESENT_AUDIO_NONE &&
          has(reason, "no PCM"));
    CHECK(present_audio_select("wav-file", true, &kind, &reason) && kind == PRESENT_AUDIO_WAV_FILE);
    CHECK(!present_audio_select("device", true, &kind, &reason) && has(reason, "--audio-sink sdl"));
    CHECK(!present_audio_select("bogus", true, &kind, &reason) && has(reason, "unknown"));
    CHECK(has(present_audio_announce(PRESENT_AUDIO_WAV_FILE), "INFERRED"));
}

static void test_options(void)
{
    options opts;
    char *none[] = {"host", "game.xbe"};
    char *both[] = {"host", "--present", "png-dir", "--audio-sink", "null", "game.xbe"};
    char *dangling[] = {"host", "game.xbe", "--present"};
    memset(&opts, 0xA5, sizeof opts);
    CHECK(parse_options(2, none, &opts) && opts.present == NULL && opts.audio_sink == NULL);
    CHECK(parse_options(6, both, &opts) && strcmp(opts.present, "png-dir") == 0 &&
          strcmp(opts.audio_sink, "null") == 0);
    CHECK(!parse_options(3, dangling, &opts));
}

static void test_hold_option(void)
{
    options opts;
    char *none[] = {"host", "game.xbe"};
    char *held[] = {"host", "--present", "window", "--present-hold-ms", "1500", "game.xbe"};
    char *orphan[] = {"host", "--present-hold-ms", "1500", "game.xbe"};
    char *bad[] = {"host", "--present", "window", "--present-hold-ms", "x", "game.xbe"};
    char *edge[] = {"host", "--present", "window", "--present-hold-ms", "86400000", "game.xbe"};
    char *over[] = {"host", "--present", "window", "--present-hold-ms", "86400001", "game.xbe"};
    CHECK(parse_options(2, none, &opts) && opts.present_hold_ms == 0u);
    CHECK(parse_options(6, held, &opts) && opts.present_hold_ms == 1500u);
    CHECK(!parse_options(4, orphan, &opts)); /* a hold without a sink */
    CHECK(!parse_options(6, bad, &opts));
    CHECK(parse_options(6, edge, &opts) && opts.present_hold_ms == 86400000u);
    CHECK(!parse_options(6, over, &opts));
    char *shot[] = {"host", "--present", "window", "--present-capture", "a.bmp", "game.xbe"};
    char *lone[] = {"host", "--present-capture", "a.bmp", "game.xbe"};
    CHECK(parse_options(6, shot, &opts) && strcmp(opts.present_capture, "a.bmp") == 0);
    CHECK(!parse_options(4, lone, &opts) && parse_options(2, none, &opts) && opts.present_capture == NULL);
}

static unsigned read32(const unsigned char *bytes)
{
    return (unsigned)bytes[0] | ((unsigned)bytes[1] << 8) | ((unsigned)bytes[2] << 16) | ((unsigned)bytes[3] << 24);
}

static void test_wav(void)
{
    const char *path = "present_sink_test.wav";
    const int16_t samples[] = {1, -2, 300, -400, 5, 6};
    present_audio_sink *sink = present_audio_sink_open(PRESENT_AUDIO_WAV_FILE, path, 48000u, 2u);
    CHECK(sink != NULL);
    CHECK(present_audio_sink_write(sink, samples, 3u));
    CHECK(present_audio_sink_frames(sink) == 3u);
    CHECK(present_audio_sink_close(sink));
    unsigned char file[64];
    FILE *handle = fopen(path, "rb");
    CHECK(handle != NULL);
    size_t got = handle != NULL ? fread(file, 1, sizeof file, handle) : 0u;
    if (handle != NULL) {
        fclose(handle);
    }
    remove(path);
    CHECK(got == 44u + 12u);
    CHECK(memcmp(file, "RIFF", 4) == 0 && memcmp(file + 8, "WAVEfmt ", 8) == 0);
    CHECK(read32(file + 4) == 36u + 12u && read32(file + 24) == 48000u && read32(file + 28) == 192000u);
    CHECK(file[22] == 2 && file[32] == 4 && file[34] == 16 && read32(file + 40) == 12u);
    CHECK(file[44] == 1 && file[45] == 0 && file[46] == 0xFE && file[47] == 0xFF);
    CHECK(file[48] == 0x2C && file[49] == 0x01);
    /* null sink counts and writes nothing, bad parameters refuse */
    present_audio_sink *null_sink = present_audio_sink_open(PRESENT_AUDIO_NULL, NULL, 48000u, 2u);
    CHECK(null_sink != NULL && present_audio_sink_write(null_sink, samples, 3u) &&
          present_audio_sink_frames(null_sink) == 3u && present_audio_sink_close(null_sink));
    CHECK(present_audio_sink_open(PRESENT_AUDIO_DEVICE, NULL, 48000u, 2u) == NULL);
    CHECK(present_audio_sink_open(PRESENT_AUDIO_WAV_FILE, NULL, 48000u, 2u) == NULL);
    CHECK(present_audio_sink_open(PRESENT_AUDIO_NULL, NULL, 0u, 2u) == NULL);
    CHECK(!present_audio_sink_write(NULL, samples, 1u));
}

int main(void)
{
    test_video_select();
    test_counting_sinks();
    test_audio_select();
    test_options();
    test_hold_option();
    test_wav();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
