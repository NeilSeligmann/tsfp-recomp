/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The overlay over the replayed frame (T831), opt-in, XEMU-LEVEL (T770, xemu v0.8.136, HQ60,
 * docs/t770-xemu-overlay-colour-key.md), never NV2A silicon. The displayed picture of the console is the
 * framebuffer (the frame the swap replay draws) with the video overlay on top, and the register 0x8958 decides
 * where. xemu measured (41 committed screenshots, 40 flat cells in each of 35 key phases):
 *
 *   key bit CLEAR (control & 0x100000 == 0, what the retail boot writes in all 169 UpdateOverlay calls, T537):
 *     the overlay replaces EVERY pixel of its rectangle, the composition is invisible there.
 *   key bit SET: the overlay shows only where the framebuffer's 24 bit RGB equals the key's RGB, exactly, an
 *     integer compare, alpha ignored, no mask or tolerance, 0x00RRGGBB order. Everywhere else the framebuffer.
 *
 * xemu compares in float, so only 20 of the 35 keys of the probe ever key (130 of 256 channel values:
 * float32(v * (1 / 255)) == float32(v / 255)). That is an emulator artifact and is NOT reproduced here: this
 * module keys by integer equality, the rule the hardware would plausibly use and the one every key that can key
 * in xemu follows (tests/test_t831_overlay_xemu.py scores both against the screenshots).
 *
 * Default OFF and then nothing changes: d3d8_overlay_key_displayed returns its argument. When on, the layer is
 * the picture of the LAST UpdateOverlay (d3d8_overlay.c latches it). It stays on screen until the title runs
 * EnableOverlay, with 1 or 0 alike (the code ignores its argument, d3d8_overlay.h, MEASURED): xemu hides the overlay then
 * (T839, docs/t770-xemu-overlay-colour-key.md, phases 41 to 57) and shows it again at the next UpdateOverlay, so
 * d3d8_overlay_key_disable drops the layer. The overlay's one pixel larger xemu box is clipped by the frame, an overlay
 * outside the frame shows nothing. Pure functions plus one small latch, no guest or device state.
 */

#ifndef TSFP_GPU_D3D8_OVERLAY_KEY_H
#define TSFP_GPU_D3D8_OVERLAY_KEY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gpu_device.h"

/** The colour key enable bit of the overlay format register 0x8958, as the driver writes it. */
#define D3D8_OVERLAY_KEY_CONTROL_BIT 0x100000u

/** The label every announcement of this behaviour carries. */
#define D3D8_OVERLAY_KEY_LABEL "xemu-level (T770, xemu v0.8.136), not NV2A silicon"

/** One overlay picture and where and how it sits over the frame. `rgb` is width * height * 3 bytes. */
typedef struct {
    const uint8_t *rgb;
    uint32_t width;
    uint32_t height;
    uint32_t left;    /* destination origin on the frame, in pixels */
    uint32_t top;
    uint32_t control; /* register 0x8958 */
    uint32_t key;     /* register 0x8B00, 0x00RRGGBB */
} d3d8_overlay_key_layer;

/** True when the overlay shows over a framebuffer pixel of `frame_rgb` (red, green, blue) for this control and key. */
bool d3d8_overlay_key_shows(uint32_t control, uint32_t key, const uint8_t frame_rgb[3]);

/**
 * Compose `layer` over the RGBA frame (`pixels`, `width` x `height`, `stride_bytes`) into `out` (same size, tightly
 * packed width * 4, so `out` must hold width * height * 4 bytes). Pixels outside the layer's box and the pixels the
 * key leaves to the framebuffer are copied, the overlay's pixels get alpha 0xFF. The box is clipped by the frame.
 * False for a NULL pointer or a zero size, `out` untouched.
 */
bool d3d8_overlay_key_compose(const uint8_t *pixels, uint32_t width, uint32_t height,
                              uint32_t stride_bytes, const d3d8_overlay_key_layer *layer,
                              uint8_t *out);

/** Opt in (default off, survives reset): the layer is latched and displayed frames are composed. */
void d3d8_overlay_key_set_enabled(bool enabled);
bool d3d8_overlay_key_enabled(void);

/** Latch the layer of an UpdateOverlay (copies the picture). False when it cannot be stored (the latch is then empty). */
bool d3d8_overlay_key_latch(const d3d8_overlay_key_layer *layer);

/** Drop the latched layer only (an UpdateOverlay whose picture could not be built must not leave the old one on screen). */
void d3d8_overlay_key_drop(void);

/**
 * EnableOverlay ran (T839, xemu-level): xemu does not display the overlay after the retail EnableOverlay writes, for either
 * argument (STOP, or SIZE_IN = 0xFFFFFFFF), and shows it again at the next UpdateOverlay. Drops the layer and counts it when one
 * was latched.
 */
void d3d8_overlay_key_disable(void);

/** Drop the latched layer and the counters. The enable setting stays. */
void d3d8_overlay_key_reset(void);

/**
 * The frame to display for `composition` (a replayed frame): `composition` itself when the option is off, nothing is
 * latched or the frame has no pixels, otherwise a composed image owned here, valid until the next call.
 */
const gpu_image *d3d8_overlay_key_displayed(const gpu_image *composition);

/** Counts every latch, drop and reset of the layer (T839): a consumer that shows the displayed frame at each vblank
 * recomposes only when this or the frame changed. */
uint64_t d3d8_overlay_key_generation(void);

/** True when the option is on and a layer is latched, so the displayed frame differs from the composition. This is the
 * `overlay_active` a [front, overlay] present schedule (live_present_vblank, T793) takes. */
bool d3d8_overlay_key_active(void);

/** Frames returned composed, and the pixels of them the overlay covered. */
uint64_t d3d8_overlay_key_frames(void);
uint64_t d3d8_overlay_key_covered_pixels(void);
/** Layers dropped by d3d8_overlay_key_disable (one latched at the time). */
uint64_t d3d8_overlay_key_disabled_layers(void);

/** One line saying what the option does and its label, empty (returns 0) when off. */
size_t d3d8_overlay_key_summary(char *text, size_t size);

#endif /* TSFP_GPU_D3D8_OVERLAY_KEY_H */
