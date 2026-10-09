/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The composed overlay frame at each modelled vblank (T839, xemu-level, src/gpu/d3d8_overlay_present.c). A recording
 * sink stands in for the present sink and a fake source for the swap replay, so the order of submit and vblank, what is
 * submitted, when a frame is held and the png files are all checked without a Vulkan device.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "d3d8_overlay_key.h"
#include "d3d8_overlay_present.h"

static int failures;
static int checks;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        checks++;                                                                         \
        if (!(cond)) {                                                                    \
            failures++;                                                                   \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                      \
        }                                                                                 \
    } while (0)

#define FRAME_W 8u
#define FRAME_H 6u
#define FRAME_STRIDE 40u /* 8 * 4 = 32 plus 8 bytes of padding that must never reach the sink */

typedef struct {
    int events; /* submits and vblanks, in order */
    int submit_event;
    int vblank_event;
    int submits;
    int vblanks;
    uint64_t number;
    uint32_t width;
    uint32_t height;
    uint8_t rgb[FRAME_W * FRAME_H * 3u];
} recording;

static void record_submit(void *context, uint64_t number, uint32_t width, uint32_t height, const uint8_t *rgb)
{
    recording *log = context;
    log->submits++;
    log->submit_event = ++log->events;
    log->number = number;
    log->width = width;
    log->height = height;
    memcpy(log->rgb, rgb, (size_t)width * height * 3u);
}

static void record_vblank(void *context)
{
    recording *log = context;
    log->vblanks++;
    log->vblank_event = ++log->events;
}

static uint8_t frame_pixels[FRAME_STRIDE * FRAME_H];
static gpu_image frame_image;
static bool frame_present;
static uint64_t frame_serial;

static const gpu_image *fake_source(uint64_t *serial)
{
    *serial = frame_serial;
    return frame_present ? &frame_image : NULL;
}

/* A frame whose pixel (x, y) is (base + x, base + y, 0x40) with alpha 0x7E, padding 0xEE. */
static void new_frame(uint8_t base)
{
    memset(frame_pixels, 0xEE, sizeof(frame_pixels));
    for (uint32_t y = 0u; y < FRAME_H; y++) {
        for (uint32_t x = 0u; x < FRAME_W; x++) {
            uint8_t *pixel = frame_pixels + y * FRAME_STRIDE + x * 4u;
            pixel[0] = (uint8_t)(base + x);
            pixel[1] = (uint8_t)(base + y);
            pixel[2] = 0x40u;
            pixel[3] = 0x7Eu;
        }
    }
    frame_image.pixels = frame_pixels;
    frame_image.width = FRAME_W;
    frame_image.height = FRAME_H;
    frame_image.stride_bytes = FRAME_STRIDE;
    frame_present = true;
    frame_serial++;
}

static bool pixel_is(const recording *log, uint32_t x, uint32_t y, uint8_t red, uint8_t green, uint8_t blue)
{
    const uint8_t *pixel = log->rgb + ((size_t)y * log->width + x) * 3u;
    return pixel[0] == red && pixel[1] == green && pixel[2] == blue;
}

static void start(recording *log, const char *directory, uint32_t maximum)
{
    memset(log, 0, sizeof(*log));
    const d3d8_overlay_present_sink sink = {record_submit, record_vblank, log, directory, maximum};
    d3d8_overlay_present_configure(&sink, fake_source);
}

static void test_off_and_no_frame(void)
{
    char text[64] = "x";
    d3d8_overlay_present_configure(NULL, NULL);
    CHECK(!d3d8_overlay_present_enabled());
    d3d8_overlay_present_vblank(); /* nothing happens, nothing crashes */
    CHECK(d3d8_overlay_present_get_counts().vblanks == 0u);
    CHECK(d3d8_overlay_present_summary(text, sizeof text) == 0u && text[0] == '\0');

    recording log;
    /* the very first frame of a source whose serial is 0 is still new (nothing was shown before it) */
    start(&log, NULL, 0u);
    new_frame(0x70u);
    frame_serial = 0u;
    d3d8_overlay_present_vblank();
    CHECK(log.submits == 1 && d3d8_overlay_present_get_counts().held == 0u);

    frame_present = false;
    start(&log, NULL, 0u);
    CHECK(d3d8_overlay_present_enabled());
    d3d8_overlay_present_vblank();
    d3d8_overlay_present_vblank();
    CHECK(log.submits == 0 && log.vblanks == 2); /* the sink's vblank is signalled even with no frame */
    d3d8_overlay_present_counts counts = d3d8_overlay_present_get_counts();
    CHECK(counts.vblanks == 2u && counts.no_frame == 2u && counts.submitted == 0u && counts.held == 0u);

    /* a frame object without pixels, or without size, is no frame either */
    frame_present = true;
    frame_image.pixels = NULL;
    d3d8_overlay_present_vblank();
    frame_image.pixels = frame_pixels;
    frame_image.width = 0u;
    d3d8_overlay_present_vblank();
    frame_image.width = FRAME_W;
    frame_image.height = 0u;
    d3d8_overlay_present_vblank();
    counts = d3d8_overlay_present_get_counts();
    CHECK(log.submits == 0 && counts.no_frame == 5u);
    d3d8_overlay_present_configure(NULL, NULL);
}

static void test_submit_hold_and_order(void)
{
    d3d8_overlay_key_set_enabled(false);
    d3d8_overlay_key_reset();
    recording log;
    start(&log, NULL, 0u);
    new_frame(0x10u);
    d3d8_overlay_present_vblank();
    CHECK(log.submits == 1 && log.vblanks == 1);
    CHECK(log.submit_event < log.vblank_event); /* the picture first, then the vblank that shows it */
    CHECK(log.number == 1u && log.width == FRAME_W && log.height == FRAME_H);
    CHECK(pixel_is(&log, 0u, 0u, 0x10u, 0x10u, 0x40u));
    CHECK(pixel_is(&log, 7u, 5u, 0x17u, 0x15u, 0x40u)); /* the stride padding (0xEE) and the alpha never reach the sink */
    CHECK(pixel_is(&log, 0u, 1u, 0x10u, 0x11u, 0x40u));

    /* nothing changed: held, but the vblank still reaches the sink */
    d3d8_overlay_present_vblank();
    d3d8_overlay_present_vblank();
    CHECK(log.submits == 1 && log.vblanks == 3);
    d3d8_overlay_present_counts counts = d3d8_overlay_present_get_counts();
    CHECK(counts.held == 2u && counts.submitted == 1u && counts.with_layer == 0u);

    /* a new replay frame: submitted again with the next number */
    new_frame(0x20u);
    d3d8_overlay_present_vblank();
    CHECK(log.submits == 2 && log.number == 2u && pixel_is(&log, 0u, 0u, 0x20u, 0x20u, 0x40u));

    /* with the option off the frame is shown as it is, even when a layer is latched */
    uint8_t layer_rgb[2 * 2 * 3];
    memset(layer_rgb, 0xAA, sizeof(layer_rgb));
    const d3d8_overlay_key_layer layer = {layer_rgb, 2u, 2u, 1u, 1u, 0x10A00u, 0u};
    CHECK(d3d8_overlay_key_latch(&layer));
    d3d8_overlay_present_vblank();
    CHECK(log.submits == 3 && pixel_is(&log, 1u, 1u, 0x21u, 0x21u, 0x40u));
    CHECK(d3d8_overlay_present_get_counts().with_layer == 0u);
    d3d8_overlay_key_reset();
    d3d8_overlay_present_configure(NULL, NULL);
}

static void test_the_layer(void)
{
    d3d8_overlay_key_set_enabled(true);
    d3d8_overlay_key_reset();
    recording log;
    start(&log, NULL, 0u);
    new_frame(0x30u);
    d3d8_overlay_present_vblank();
    CHECK(log.submits == 1 && pixel_is(&log, 2u, 2u, 0x32u, 0x32u, 0x40u)); /* on, nothing latched: the frame alone */
    CHECK(d3d8_overlay_present_get_counts().with_layer == 0u);

    /* an UpdateOverlay latches a layer: the same replay frame is submitted again, composed, key bit clear covers the box */
    uint8_t layer_rgb[3 * 2 * 3];
    for (size_t index = 0u; index < sizeof(layer_rgb); index += 3u) {
        layer_rgb[index] = 0xA0u;
        layer_rgb[index + 1u] = (uint8_t)index;
        layer_rgb[index + 2u] = 0xC0u;
    }
    d3d8_overlay_key_layer layer = {layer_rgb, 3u, 2u, 2u, 3u, 0x10A00u, 0u};
    CHECK(d3d8_overlay_key_latch(&layer));
    CHECK(d3d8_overlay_key_active());
    d3d8_overlay_present_vblank();
    CHECK(log.submits == 2 && log.number == 2u);
    CHECK(pixel_is(&log, 2u, 3u, 0xA0u, 0x00u, 0xC0u));
    CHECK(pixel_is(&log, 4u, 4u, 0xA0u, 15u, 0xC0u));
    CHECK(pixel_is(&log, 1u, 3u, 0x31u, 0x33u, 0x40u) && pixel_is(&log, 5u, 3u, 0x35u, 0x33u, 0x40u));
    CHECK(d3d8_overlay_present_get_counts().with_layer == 1u);
    d3d8_overlay_present_vblank();
    CHECK(log.submits == 2 && d3d8_overlay_present_get_counts().held == 1u);

    /* a new layer picture, same frame: recomposed without a new replay frame */
    memset(layer_rgb, 0x55, sizeof(layer_rgb));
    CHECK(d3d8_overlay_key_latch(&layer));
    d3d8_overlay_present_vblank();
    CHECK(log.submits == 3 && pixel_is(&log, 2u, 3u, 0x55u, 0x55u, 0x55u));

    /* key bit set: the overlay shows only where the frame RGB equals the key (0x00RRGGBB), here pixel (3, 3) */
    layer.control = 0x10A00u | D3D8_OVERLAY_KEY_CONTROL_BIT;
    layer.key = 0x00333340u;
    CHECK(d3d8_overlay_key_latch(&layer));
    d3d8_overlay_present_vblank();
    CHECK(log.submits == 4);
    CHECK(pixel_is(&log, 3u, 3u, 0x55u, 0x55u, 0x55u));
    CHECK(pixel_is(&log, 2u, 3u, 0x32u, 0x33u, 0x40u) && pixel_is(&log, 4u, 3u, 0x34u, 0x33u, 0x40u));
    CHECK(d3d8_overlay_present_get_counts().with_layer == 3u);

    /* dropping the layer shows the plain frame again at the next vblank */
    d3d8_overlay_key_drop();
    CHECK(!d3d8_overlay_key_active());
    d3d8_overlay_present_vblank();
    CHECK(log.submits == 5 && pixel_is(&log, 3u, 3u, 0x33u, 0x33u, 0x40u));
    CHECK(d3d8_overlay_present_get_counts().with_layer == 3u);

    /* a new replay frame and a new layer in the same vblank are one submit */
    CHECK(d3d8_overlay_key_latch(&layer));
    new_frame(0x38u);
    d3d8_overlay_present_vblank();
    CHECK(log.submits == 6);

    /* a reset (the guest was reset) takes the layer away: the plain frame is submitted again */
    d3d8_overlay_key_reset();
    d3d8_overlay_present_vblank();
    CHECK(log.submits == 7 && pixel_is(&log, 3u, 3u, 0x3Bu, 0x3Bu, 0x40u));

    /* EnableOverlay (T839, xemu-level) takes the layer away at the next vblank, the next update brings it back */
    memset(layer_rgb, 0x55, sizeof(layer_rgb));
    layer.control = 0x10A00u;
    CHECK(d3d8_overlay_key_latch(&layer));
    d3d8_overlay_present_vblank();
    CHECK(log.submits == 8 && pixel_is(&log, 3u, 3u, 0x55u, 0x55u, 0x55u));
    d3d8_overlay_key_disable();
    d3d8_overlay_present_vblank();
    CHECK(log.submits == 9 && pixel_is(&log, 3u, 3u, 0x3Bu, 0x3Bu, 0x40u));
    d3d8_overlay_present_vblank();
    CHECK(log.submits == 9); /* held: nothing more changed */
    CHECK(d3d8_overlay_key_latch(&layer));
    d3d8_overlay_present_vblank();
    CHECK(log.submits == 10 && pixel_is(&log, 3u, 3u, 0x55u, 0x55u, 0x55u));
    CHECK(d3d8_overlay_present_get_counts().with_layer == 6u);
    d3d8_overlay_key_set_enabled(false);
    d3d8_overlay_key_reset();
    d3d8_overlay_present_configure(NULL, NULL);
}

static long file_size(const char *path, unsigned char magic[4])
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }
    memset(magic, 0, 4u);
    if (fread(magic, 1u, 4u, file) != 4u) {
        fclose(file);
        return -2;
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fclose(file);
    return size;
}

/* gpu_png writes stored (uncompressed) deflate blocks, so the scanlines are in the file as they are: a filter byte 0 and
 * the row's RGBA pixels. True when the second row follows the first one at the frame's own pitch (pixel (0, 1) of a
 * frame made by new_frame(base)), which is what the stride the writer is given decides. */
static bool png_rows_are_right(const char *path, uint8_t base)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }
    static uint8_t bytes[4096];
    const size_t length = fread(bytes, 1u, sizeof(bytes), file);
    fclose(file);
    const uint8_t first[] = {0x00u, base, base, 0x40u, 0x7Eu, (uint8_t)(base + 1u), base, 0x40u, 0x7Eu};
    for (size_t at = 0u; at + sizeof(first) + 1u + FRAME_W * 4u <= length; at++) {
        if (memcmp(bytes + at, first, sizeof(first)) == 0) {
            const uint8_t *second = bytes + at + 1u + FRAME_W * 4u;
            return second[0] == 0x00u && second[1] == base && second[2] == (uint8_t)(base + 1u) && second[3] == 0x40u &&
                   second[4] == 0x7Eu;
        }
    }
    return false;
}

static void test_png_files(void)
{
    char directory[] = "/tmp/tsfp_overlay_present_XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    d3d8_overlay_key_set_enabled(false);
    d3d8_overlay_key_reset();
    recording log;
    start(&log, directory, 2u);
    new_frame(0x01u);
    d3d8_overlay_present_vblank();
    d3d8_overlay_present_vblank(); /* held, no file */
    new_frame(0x02u);
    d3d8_overlay_present_vblank();
    new_frame(0x03u);
    d3d8_overlay_present_vblank(); /* past the maximum of two files */
    d3d8_overlay_present_counts counts = d3d8_overlay_present_get_counts();
    CHECK(counts.submitted == 3u && counts.png_written == 2u && counts.png_failed == 0u);
    CHECK(log.submits == 3); /* the maximum limits the files, never the sink */
    char path[512];
    unsigned char magic[4];
    snprintf(path, sizeof(path), "%s/composed_000001.png", directory);
    CHECK(file_size(path, magic) > 40 && magic[0] == 0x89u && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G');
    CHECK(png_rows_are_right(path, 0x01u)); /* the image's own pitch (40), not width * 4 */
    remove(path);
    snprintf(path, sizeof(path), "%s/composed_000002.png", directory);
    CHECK(file_size(path, magic) > 40);
    remove(path);
    snprintf(path, sizeof(path), "%s/composed_000003.png", directory);
    CHECK(file_size(path, magic) == -1);

    /* no maximum: every submitted frame, and an unwritable directory is counted, never silent */
    start(&log, directory, 0u);
    new_frame(0x04u);
    d3d8_overlay_present_vblank();
    new_frame(0x05u);
    d3d8_overlay_present_vblank();
    CHECK(d3d8_overlay_present_get_counts().png_written == 2u);
    snprintf(path, sizeof(path), "%s/composed_000001.png", directory);
    remove(path);
    snprintf(path, sizeof(path), "%s/composed_000002.png", directory);
    remove(path);
    rmdir(directory);
    start(&log, "", 0u); /* an empty directory name is no directory */
    new_frame(0x07u);
    d3d8_overlay_present_vblank();
    counts = d3d8_overlay_present_get_counts();
    CHECK(counts.png_written == 0u && counts.png_failed == 0u && log.submits == 1);
    start(&log, "/nonexistent/tsfp_overlay_present", 0u);
    new_frame(0x06u);
    d3d8_overlay_present_vblank();
    counts = d3d8_overlay_present_get_counts();
    CHECK(counts.png_written == 0u && counts.png_failed == 1u && log.submits == 1);
    d3d8_overlay_present_configure(NULL, NULL);
}

static void test_summary_and_default_source(void)
{
    recording log;
    start(&log, NULL, 0u);
    new_frame(0x09u);
    d3d8_overlay_present_vblank();
    char text[1024];
    const size_t length = d3d8_overlay_present_summary(text, sizeof text);
    CHECK(length != 0u && length == strlen(text));
    CHECK(strstr(text, "T839") != NULL && strstr(text, D3D8_OVERLAY_KEY_LABEL) != NULL);
    CHECK(strstr(text, "frames submitted 1") != NULL && strstr(text, "vblanks 1,") != NULL);
    CHECK(strstr(text, ".png") == NULL); /* no directory, no file claim */

    /* reconfiguring clears the counters and the memory of the last frame */
    start(&log, NULL, 0u);
    CHECK(d3d8_overlay_present_get_counts().vblanks == 0u);
    d3d8_overlay_present_vblank();
    CHECK(log.submits == 1);

    /* the default source is the swap replay: not enabled here, so there is no frame and the vblank still goes through */
    memset(&log, 0, sizeof(log));
    const d3d8_overlay_present_sink sink = {record_submit, record_vblank, &log, NULL, 0u};
    d3d8_overlay_present_configure(&sink, NULL);
    d3d8_overlay_present_vblank();
    CHECK(log.submits == 0 && log.vblanks == 1 && d3d8_overlay_present_get_counts().no_frame == 1u);
    d3d8_overlay_present_configure(NULL, NULL);
}

int main(void)
{
    test_off_and_no_frame();
    test_submit_hold_and_order();
    test_the_layer();
    test_png_files();
    test_summary_and_default_source();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
