/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_STANDIN_PATTERN_H
#define TSFP_GPU_STANDIN_PATTERN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define GPU_STANDIN_PATTERN_MAX_EDGE 4096u

typedef enum gpu_standin_pattern {
    GPU_STANDIN_SOLID = 0,
    GPU_STANDIN_CHECKER = 1,
} gpu_standin_pattern;

/* Diagnostic RGBA pixels, row-major with (0,0) at the first row's left edge.
 * The checker repeats red/yellow over blue/cyan: horizontal and vertical changes
 * differ, making transposed/constant coordinates visible. This is not a title texture
 * or a sampler model. Refusal leaves every output byte unchanged. */
static inline bool gpu_standin_pattern_fill(uint8_t *pixels, size_t capacity, uint32_t width,
                                           uint32_t height, gpu_standin_pattern pattern,
                                           const uint8_t rgba[4])
{
    if (pixels == NULL || width == 0u || height == 0u || width > GPU_STANDIN_PATTERN_MAX_EDGE ||
        height > GPU_STANDIN_PATTERN_MAX_EDGE ||
        (pattern != GPU_STANDIN_SOLID && pattern != GPU_STANDIN_CHECKER) ||
        (pattern == GPU_STANDIN_SOLID && rgba == NULL)) {
        return false;
    }
    const size_t bytes = (size_t)width * height * 4u;
    if (capacity < bytes) {
        return false;
    }
    static const uint8_t checker[4][4] = {
        {255u, 0u, 0u, 255u}, {255u, 255u, 0u, 255u},
        {0u, 0u, 255u, 255u}, {0u, 255u, 255u, 255u},
    };
    for (uint32_t y = 0u; y < height; y++) {
        for (uint32_t x = 0u; x < width; x++) {
            const uint8_t *colour = pattern == GPU_STANDIN_SOLID ? rgba : checker[(y & 1u) * 2u + (x & 1u)];
            memcpy(pixels + ((size_t)y * width + x) * 4u, colour, 4u);
        }
    }
    return true;
}

#endif
