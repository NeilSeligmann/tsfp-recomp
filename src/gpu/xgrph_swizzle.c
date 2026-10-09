/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xgrph_swizzle.h"
#include <string.h>
#include "kernel_call.h"
#include "xgrph_hle.h"

#define SWIZZLE_ENTRY 0x003EEABEu
#define IMAGE_BYTES 16384u

static uint32_t morton_index(uint32_t x, uint32_t y)
{
    uint32_t index = 0u;
    for (uint32_t bit = 0u; bit < 6u; bit++) {
        index |= ((x >> bit) & 1u) << (2u * bit);
        index |= ((y >> bit) & 1u) << (2u * bit + 1u);
    }
    return index;
}

uint32_t xgrph_swizzle_rect(uint32_t source, uint32_t pitch, uint32_t rectangle,
    uint32_t destination, uint32_t width, uint32_t height, uint32_t point,
    uint32_t bytes_per_pixel)
{
    if (width != 64u || height != 64u || bytes_per_pixel != 4u ||
        (pitch != 0u && pitch != 256u) || point != 0u)
        xgrph_hle_fatal(SWIZZLE_ENTRY,
            "unsupported swizzle geometry %ux%u, bpp %u, pitch %u, point %#x",
            width, height, bytes_per_pixel, pitch, point);
    if (rectangle != 0u) {
        const void *mapped = kernel_guest_at(rectangle, 16u);
        uint32_t words[4];
        if (mapped == NULL)
            xgrph_hle_fatal(SWIZZLE_ENTRY, "swizzle RECT %#x is unreadable", rectangle);
        memcpy(words, mapped, sizeof(words));
        if (words[0] != 0u || words[1] != 0u || words[2] != 64u || words[3] != 64u)
            xgrph_hle_fatal(SWIZZLE_ENTRY, "only a full swizzle RECT is recovered");
    }
    /* Preflight both entire images and all inputs before the first output byte.
     * uint64_t ends keep overlap detection correct at the guest address limit. */
    const uint8_t *input = kernel_guest_at(source, IMAGE_BYTES);
    uint8_t *output = kernel_guest_at(destination, IMAGE_BYTES);
    if (input == NULL || output == NULL)
        xgrph_hle_fatal(SWIZZLE_ENTRY, "swizzle image is not mapped for 16384 bytes");
    if ((uint64_t)source < (uint64_t)destination + IMAGE_BYTES &&
        (uint64_t)destination < (uint64_t)source + IMAGE_BYTES)
        xgrph_hle_fatal(SWIZZLE_ENTRY, "overlapping swizzle images are not recovered");
    for (uint32_t y = 0u; y < 64u; y++) {
        for (uint32_t x = 0u; x < 64u; x++) {
            memcpy(output + morton_index(x, y) * 4u, input + (y * 64u + x) * 4u, 4u);
        }
    }
    return pitch == 0u && rectangle == 0u ? 0xFE0u : 256u;
}

static uint32_t swizzle_handler(void *context)
{
    uint32_t arguments[8];
    for (unsigned i = 0u; i < 8u; i++) {
        if (!kernel_frame_arg((const kernel_call_frame *)context, i, &arguments[i]))
            xgrph_hle_fatal(SWIZZLE_ENTRY, "swizzle argument %u is unreadable", i);
    }
    return xgrph_swizzle_rect(arguments[0], arguments[1], arguments[2], arguments[3],
        arguments[4], arguments[5], arguments[6], arguments[7]);
}

size_t xgrph_swizzle_register(void)
{
    return xgrph_hle_register(SWIZZLE_ENTRY, swizzle_handler) ? 1u : 0u;
}
