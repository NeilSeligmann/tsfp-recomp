/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_PNG_H
#define TSFP_GPU_PNG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Write 8-bit RGBA pixels as a PNG.
 *
 * `stride_bytes` is the distance between row starts in `pixels`, which is not
 * necessarily width*4 -- a Vulkan linear image has a driver-chosen row pitch.
 * Returns false on a bad argument or any I/O failure.
 */
bool gpu_png_write_rgba(const char *path, const uint8_t *pixels,
                        uint32_t width, uint32_t height, uint32_t stride_bytes);

#endif /* TSFP_GPU_PNG_H */
