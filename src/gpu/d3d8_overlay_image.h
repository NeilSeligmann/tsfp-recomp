/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pixels for the overlay dump (T537): YUY2 to RGB, a nearest neighbour scale and a PNG writer.
 * Pure functions with no guest, device or global state, so a test can compile this file alone.
 *
 * THE COLOUR MATRIX IS UNMEASURED FOR THE OVERLAY HARDWARE, its family is evidenced. What the
 * console's display does with a YUY2 overlay is fixed function and there is no hardware or
 * reference capture in this repository to read the coefficients from. Two pieces of evidence:
 *
 *   1. The driver's own write of the overlay format register 0x8958 (MEASURED, d3d8_overlay.c, the
 *      0x003D9810 port) is pitch | 0x10000 | key 0x100000 and never sets bit 24. In the NV10 class
 *      PVIDEO layout that nouveau documents (nvreg.h, recalled and confirmed on the web while
 *      writing T537, not stored here) bit 16 is the YUYV pixel format, bit 20 the display colour
 *      key and bit 24 `MATRIX_ITURBT709`, so the driver leaves the default ITU-R BT.601 matrix
 *      selected. The coefficients and the range handling inside the hardware stay UNMEASURED,
 *      and so does the filter that scales and upsamples the chroma.
 *   2. The matrix used here is the one the title's OWN retained XMV library applies when it
 *      converts 4:2:0 planes to 32 bit RGB itself (the output formats 0x12 and 0x1E of 0x4463AE,
 *      the twin wrappers 0x445DF7 and 0x4461D2 over the block workers 0x445C0C and 0x445FD3),
 *      read from the constants the wrapper loads at 0x445E1C..0x445E78 and proven equal to the
 *      original bytes in tests/test_overlay_image_original.py. It is BT.601 with the video
 *      (studio) range, in 15 bit fixed point, so it is a best-effort choice that agrees with
 *      point 1 in kind. The raw planes are written beside every picture for any other matrix.
 *
 *   L = (Y - 16) * 38139                       38139 / 32768 = 1.16391
 *   R = clamp((L + 52298 * (V - 128)) >> 15)   1.59601
 *   G = clamp((L - 26640 * (V - 128) - 12812 * (U - 128)) >> 15)   0.81299 and 0.39099
 *   B = clamp((L + 66126 * (U - 128)) >> 15)   2.01801
 *
 * The shift is an arithmetic shift of the exact sum, so it floors (no rounding offset).
 *
 * THE XEMU-LEVEL ALTERNATIVE (T831, labelled D3D8_OVERLAY_IMAGE_XEMU_LABEL, opt-in, never the default).
 * T770 measured in xemu v0.8.136 (HQ60, docs/t770-xemu-overlay-colour-key.md, never NV2A silicon) what the
 * emulated overlay draws. The functions suffixed `_xemu` below reproduce it and the XMV library functions above
 * stay the default converter:
 *
 *   R = clamp((298 * (Y - 16) + 409 * (V - 128) + 128) >> 8)
 *   G = clamp((298 * (Y - 16) - 100 * (U - 128) - 208 * (V - 128) + 128) >> 8)
 *   B = clamp((298 * (Y - 16) + 516 * (U - 128) + 128) >> 8)
 *
 * 40 of 40 flat cells of the probe exact (the library constants above: 21 of 40, within 1). The scale is bilinear
 * in RGB after the conversion, a destination box one pixel wider and taller than declared, output pixel i sampling
 * texel coordinate i * source / out (the corner, texel centres at j + 1/2), 8 bit weights, REPEAT edges. The
 * registers' steps are not read: xemu reads them (T770 phase 40) but the mapping away from the driver's own formula
 * is not fitted, and the retail title only ever writes the driver's formula (T831).
 */

#ifndef TSFP_GPU_D3D8_OVERLAY_IMAGE_H
#define TSFP_GPU_D3D8_OVERLAY_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** The label every announcement of the xemu alternative carries. */
#define D3D8_OVERLAY_IMAGE_XEMU_LABEL "xemu-level (T770, xemu v0.8.136), not NV2A silicon"

/** One sample. `rgb` receives red, green, blue. */
void d3d8_overlay_image_yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v, uint8_t rgb[3]);

/** Bytes a YUY2 row of `width` pixels must hold: whole Y U Y V pairs, so an odd width needs the
 * second half of its last pair too. */
size_t d3d8_overlay_image_yuy2_row_bytes(uint32_t width);

/**
 * Convert `height` rows of `width` YUY2 pixels, rows `pitch` bytes apart, to packed RGB (3 bytes
 * per pixel, no row padding, red first). Pixel x takes Y from byte 2x and the chroma of its pair
 * (U at byte 4 * (x / 2) + 1, V at +3), copied not filtered. False, and nothing written, when a
 * pointer is NULL, width or height is 0, or `pitch` is smaller than one row (see row_bytes).
 */
bool d3d8_overlay_image_yuy2_to_rgb(const uint8_t *yuy2, size_t pitch, uint32_t width,
                                    uint32_t height, uint8_t *rgb);

/**
 * Split the same pixels into raw planes, 4:2:2: `y` holds width * height bytes, `u` and `v` hold
 * ((width + 1) / 2) * height bytes each. The unconverted data, for a matrix this code did not
 * choose. Same refusals as the conversion.
 */
bool d3d8_overlay_image_yuy2_to_planes(const uint8_t *yuy2, size_t pitch, uint32_t width,
                                       uint32_t height, uint8_t *y, uint8_t *u, uint8_t *v);

/**
 * Nearest neighbour scale of packed RGB. Destination pixel (x, y) takes source pixel
 * ((x * horizontal_step + 0x80000) >> 20, (y * vertical_step + 0x80000) >> 20), clamped to the
 * source, where the steps are the 12.20 fixed point values the overlay registers 0x8938 and
 * 0x8940 hold (corner aligned: ((source - 1) << 20) / (destination - 1)), so the sample is the
 * nearest source pixel and the last destination pixel is the last source pixel. The hardware's
 * filter is not modelled. False for a NULL pointer or a zero dimension.
 */
bool d3d8_overlay_image_scale(const uint8_t *source, uint32_t source_width, uint32_t source_height,
                              uint32_t horizontal_step, uint32_t vertical_step, uint8_t *destination,
                              uint32_t destination_width, uint32_t destination_height);

/** One sample with the xemu matrix (see above). `rgb` receives red, green, blue. */
void d3d8_overlay_image_yuv_to_rgb_xemu(uint8_t y, uint8_t u, uint8_t v, uint8_t rgb[3]);

/** The same conversion of whole YUY2 rows as d3d8_overlay_image_yuy2_to_rgb, with the xemu matrix. */
bool d3d8_overlay_image_yuy2_to_rgb_xemu(const uint8_t *yuy2, size_t pitch, uint32_t width,
                                         uint32_t height, uint8_t *rgb);

/**
 * The xemu scale of packed RGB: the destination box is (destination_width + 1) x (destination_height + 1)
 * pixels, `destination` must hold that many (3 bytes each, no padding, red first). Pixel (i, j) is the bilinear
 * mix of the four texels around texel coordinate (i * source_width / destination_width, j * source_height /
 * destination_height), texel centres at j + 1/2, edges REPEAT (texel -1 is the last, texel `width` the first), the
 * weight of the second texel quantised to 8 bits (`floor(fraction * 256 + 1/2)`), each channel mixed
 * horizontally first, then vertically, each stage `(a * (256 - w) + b * w + 128) >> 8`. At 1:1 every pixel is
 * therefore the mean of source pixels i - 1 and i. False for a NULL pointer or a zero dimension.
 */
bool d3d8_overlay_image_scale_xemu(const uint8_t *source, uint32_t source_width,
                                   uint32_t source_height, uint8_t *destination,
                                   uint32_t destination_width, uint32_t destination_height);

/**
 * Write packed RGB as an 8 bit PNG with uncompressed (stored) deflate blocks, so no compression
 * library is needed. False when the file cannot be created or written completely.
 */
bool d3d8_overlay_image_write_png(const char *path, const uint8_t *rgb, uint32_t width,
                                  uint32_t height);

/** CRC-32 (IEEE) and Adler-32 as PNG and zlib define them, exposed so a test can pin them. */
uint32_t d3d8_overlay_image_crc32(uint32_t crc, const uint8_t *data, size_t length);
uint32_t d3d8_overlay_image_adler32(uint32_t adler, const uint8_t *data, size_t length);

#endif /* TSFP_GPU_D3D8_OVERLAY_IMAGE_H */
