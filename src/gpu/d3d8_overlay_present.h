/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The composed overlay frame at each modelled vblank (T839), opt-in, XEMU-LEVEL (T770, T831), never NV2A silicon.
 *
 * With --overlay-xemu-key and --gpu-replay the picture of the console is the swap replay's frame with the video overlay on
 * top (d3d8_overlay_key.h). This module hands that picture to a present sink: at every modelled vblank it asks for the
 * replay's last frame, takes d3d8_overlay_key_displayed of it (the ONE composition, there is no second compositor here)
 * and, when the frame or the overlay layer changed since the last vblank, submits it as an RGB picture, then signals the
 * sink's vblank. A frame that did not change is not submitted again (the sink holds what it shows). In this mode the sink
 * receives the composed frame INSTEAD of the raw overlay picture (the overlay is part of it already).
 *
 * The [front, overlay] schedule of the live renderer (live_target.h, live_present_vblank, T793) takes the same layer: its
 * `overlay_active` is d3d8_overlay_key_active(), and the layer's pixels are d3d8_overlay_key_displayed's, so the live path
 * and the replay path read one latch. The host does not run the live path yet (T838).
 *
 * The png-dir sink writes composed_NNNNNN.png of every submitted frame into its directory (--dump-overlay DIR, at most
 * `png_maximum` when non-zero), the window sink shows them, the null sink counts them. Pure host side glue, no guest state.
 */

#ifndef TSFP_GPU_D3D8_OVERLAY_PRESENT_H
#define TSFP_GPU_D3D8_OVERLAY_PRESENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gpu_device.h"

/** Where the frames go. `submit` receives a tight RGB24 picture valid for the call only, `vblank` is the sink's vblank. */
typedef struct {
    void (*submit)(void *context, uint64_t number, uint32_t width, uint32_t height, const uint8_t *rgb);
    void (*vblank)(void *context);
    void *context;
    const char *png_directory; /* NULL: no files */
    uint32_t png_maximum;      /* 0: every frame */
} d3d8_overlay_present_sink;

/** The frame to compose over and a serial that changes with each new frame. NULL pixels: no frame yet. */
typedef const gpu_image *(*d3d8_overlay_present_source)(uint64_t *serial);

/** Start (or, with a NULL `sink`, stop) feeding the sink. A NULL `source` is the swap replay's last frame, serial
 * frames_replayed. Clears the counters. */
void d3d8_overlay_present_configure(const d3d8_overlay_present_sink *sink,
                                    d3d8_overlay_present_source source);
bool d3d8_overlay_present_enabled(void);

/** One modelled vblank: compose, submit when changed, then the sink's vblank. Does nothing when not configured. */
void d3d8_overlay_present_vblank(void);

typedef struct {
    uint64_t vblanks;     /* modelled vblanks seen */
    uint64_t submitted;   /* composed frames handed to the sink */
    uint64_t held;        /* vblanks where nothing changed and nothing was submitted */
    uint64_t no_frame;    /* vblanks before the replay had presented a frame */
    uint64_t failed;      /* changed frames that could not be converted (out of memory), nothing submitted */
    uint64_t with_layer;  /* submitted frames that had an overlay layer composed in */
    uint64_t png_written;
    uint64_t png_failed;
} d3d8_overlay_present_counts;
d3d8_overlay_present_counts d3d8_overlay_present_get_counts(void);

/** One line saying what the sink receives and its xemu label, empty (returns 0) when not configured. */
size_t d3d8_overlay_present_summary(char *text, size_t size);

#endif /* TSFP_GPU_D3D8_OVERLAY_PRESENT_H */
