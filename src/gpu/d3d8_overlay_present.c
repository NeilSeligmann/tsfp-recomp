/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See d3d8_overlay_present.h.
 */

#include "d3d8_overlay_present.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d8_overlay_key.h"
#include "d3d8_swap_replay.h"
#include "gpu_png.h"

static bool configured;
static d3d8_overlay_present_sink sink;
static d3d8_overlay_present_source source;
static d3d8_overlay_present_counts counts;
static bool have_last;
static uint64_t last_serial;
static uint64_t last_generation;
static uint8_t *rgb;
static size_t rgb_capacity;
static char png_directory[512];

static const gpu_image *replay_source(uint64_t *serial)
{
    *serial = d3d8_swap_replay_get_stats().frames_replayed;
    return d3d8_swap_replay_last_frame();
}

void d3d8_overlay_present_configure(const d3d8_overlay_present_sink *new_sink,
                                    d3d8_overlay_present_source new_source)
{
    memset(&counts, 0, sizeof(counts));
    have_last = false;
    last_serial = 0u;
    last_generation = 0u;
    configured = new_sink != NULL;
    if (!configured) {
        memset(&sink, 0, sizeof(sink));
        source = NULL;
        return;
    }
    sink = *new_sink;
    sink.png_directory = NULL;
    if (new_sink->png_directory != NULL && new_sink->png_directory[0] != '\0') {
        snprintf(png_directory, sizeof(png_directory), "%s", new_sink->png_directory);
        sink.png_directory = png_directory;
    }
    source = new_source != NULL ? new_source : replay_source;
}

bool d3d8_overlay_present_enabled(void)
{
    return configured;
}

/* The composed RGBA frame as the tight RGB24 picture the sinks take. False when the buffer cannot grow. */
static bool to_rgb24(const gpu_image *image)
{
    const size_t bytes = (size_t)image->width * image->height * 3u;
    if (bytes > rgb_capacity) {
        uint8_t *grown = realloc(rgb, bytes);
        if (grown == NULL) {
            return false;
        }
        rgb = grown;
        rgb_capacity = bytes;
    }
    for (uint32_t row = 0u; row < image->height; row++) {
        const uint8_t *from = image->pixels + (size_t)row * image->stride_bytes;
        uint8_t *to = rgb + (size_t)row * image->width * 3u;
        for (uint32_t column = 0u; column < image->width; column++) {
            memcpy(to + (size_t)column * 3u, from + (size_t)column * 4u, 3u);
        }
    }
    return true;
}

static void write_png(uint64_t number, const gpu_image *image)
{
    if (sink.png_directory == NULL || (sink.png_maximum != 0u && counts.png_written >= sink.png_maximum)) {
        return;
    }
    char path[640];
    snprintf(path, sizeof(path), "%s/composed_%06llu.png", sink.png_directory, (unsigned long long)number);
    if (gpu_png_write_rgba(path, image->pixels, image->width, image->height, image->stride_bytes)) {
        counts.png_written++;
    } else {
        counts.png_failed++;
    }
}

void d3d8_overlay_present_vblank(void)
{
    if (!configured) {
        return;
    }
    counts.vblanks++;
    uint64_t serial = 0u;
    const gpu_image *frame = source(&serial);
    if (frame == NULL || frame->pixels == NULL || frame->width == 0u || frame->height == 0u) {
        counts.no_frame++;
    } else {
        const uint64_t generation = d3d8_overlay_key_generation();
        if (have_last && serial == last_serial && generation == last_generation) {
            counts.held++;
        } else {
            const bool layer = d3d8_overlay_key_active();
            const gpu_image *shown = d3d8_overlay_key_displayed(frame);
            have_last = true;
            last_serial = serial;
            last_generation = generation;
            if (to_rgb24(shown)) {
                counts.submitted++;
                counts.with_layer += layer ? 1u : 0u;
                if (sink.submit != NULL) {
                    sink.submit(sink.context, counts.submitted, shown->width, shown->height, rgb);
                }
                write_png(counts.submitted, shown);
            } else {
                counts.failed++;
            }
        }
    }
    if (sink.vblank != NULL) {
        sink.vblank(sink.context);
    }
}

d3d8_overlay_present_counts d3d8_overlay_present_get_counts(void)
{
    return counts;
}

size_t d3d8_overlay_present_summary(char *text, size_t size)
{
    if (!configured) {
        if (text != NULL && size != 0u) {
            text[0] = '\0';
        }
        return 0u;
    }
    const int written = snprintf(
        text, size,
        "composed frame to the present sink ON (opt-in, T839, %s): at each modelled vblank the sink gets the swap replay "
        "frame with the overlay layer composed by d3d8_overlay_key_displayed (the one composition) when either changed, "
        "instead of the raw overlay pictures%s; vblanks %llu, frames submitted %llu (with the layer %llu), held %llu, no "
        "replay frame yet %llu, failed %llu, png written %llu failed %llu",
        D3D8_OVERLAY_KEY_LABEL, sink.png_directory != NULL ? " (composed_NNNNNN.png files)" : "",
        (unsigned long long)counts.vblanks, (unsigned long long)counts.submitted,
        (unsigned long long)counts.with_layer, (unsigned long long)counts.held,
        (unsigned long long)counts.no_frame, (unsigned long long)counts.failed,
        (unsigned long long)counts.png_written,
        (unsigned long long)counts.png_failed);
    return written < 0 ? 0u : (size_t)written;
}
