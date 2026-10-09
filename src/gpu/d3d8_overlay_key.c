/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See d3d8_overlay_key.h.
 */

#include "d3d8_overlay_key.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool enabled;
static d3d8_overlay_key_layer latched;
static uint8_t *latched_rgb;
static size_t latched_capacity;
static bool have_layer;
static gpu_image displayed;
static size_t displayed_capacity;
static uint64_t composed_frames;
static uint64_t covered_pixels;
static uint64_t layer_generation;
static uint64_t disabled_layers;

bool d3d8_overlay_key_shows(uint32_t control, uint32_t key, const uint8_t frame_rgb[3])
{
    if ((control & D3D8_OVERLAY_KEY_CONTROL_BIT) == 0u) {
        return true;
    }
    return frame_rgb[0] == ((key >> 16) & 0xFFu) && frame_rgb[1] == ((key >> 8) & 0xFFu) &&
           frame_rgb[2] == (key & 0xFFu);
}

bool d3d8_overlay_key_compose(const uint8_t *pixels, uint32_t width, uint32_t height,
                              uint32_t stride_bytes, const d3d8_overlay_key_layer *layer,
                              uint8_t *out)
{
    if (pixels == NULL || out == NULL || layer == NULL || layer->rgb == NULL || width == 0u ||
        height == 0u || stride_bytes < (size_t)width * 4u) {
        return false;
    }
    for (uint32_t row = 0u; row < height; row++) {
        memcpy(out + (size_t)row * width * 4u, pixels + (size_t)row * stride_bytes, (size_t)width * 4u);
    }
    for (uint32_t row = 0u; row < layer->height; row++) {
        const uint64_t y = (uint64_t)layer->top + row;
        if (y >= height) {
            break;
        }
        for (uint32_t column = 0u; column < layer->width; column++) {
            const uint64_t x = (uint64_t)layer->left + column;
            if (x >= width) {
                break;
            }
            uint8_t *pixel = out + ((size_t)y * width + (size_t)x) * 4u;
            if (d3d8_overlay_key_shows(layer->control, layer->key, pixel)) {
                memcpy(pixel, layer->rgb + ((size_t)row * layer->width + column) * 3u, 3u);
                pixel[3] = 0xFFu;
            }
        }
    }
    return true;
}

void d3d8_overlay_key_set_enabled(bool value)
{
    enabled = value;
}

bool d3d8_overlay_key_enabled(void)
{
    return enabled;
}

bool d3d8_overlay_key_latch(const d3d8_overlay_key_layer *layer)
{
    have_layer = false;
    layer_generation++;
    if (layer == NULL || layer->rgb == NULL || layer->width == 0u || layer->height == 0u) {
        return false;
    }
    const size_t bytes = (size_t)layer->width * layer->height * 3u;
    if (bytes > latched_capacity) {
        uint8_t *grown = realloc(latched_rgb, bytes);
        if (grown == NULL) {
            return false;
        }
        latched_rgb = grown;
        latched_capacity = bytes;
    }
    memcpy(latched_rgb, layer->rgb, bytes);
    latched = *layer;
    latched.rgb = latched_rgb;
    have_layer = true;
    return true;
}

void d3d8_overlay_key_drop(void)
{
    have_layer = false;
    layer_generation++;
}

void d3d8_overlay_key_disable(void)
{
    if (have_layer) {
        disabled_layers++;
    }
    have_layer = false;
    layer_generation++;
}

void d3d8_overlay_key_reset(void)
{
    have_layer = false;
    layer_generation++;
    composed_frames = 0u;
    covered_pixels = 0u;
    disabled_layers = 0u;
}

const gpu_image *d3d8_overlay_key_displayed(const gpu_image *composition)
{
    if (!enabled || !have_layer || composition == NULL || composition->pixels == NULL ||
        composition->width == 0u || composition->height == 0u) {
        return composition;
    }
    const size_t bytes = (size_t)composition->width * composition->height * 4u;
    if (bytes > displayed_capacity) {
        uint8_t *grown = realloc(displayed.pixels, bytes);
        if (grown == NULL) {
            return composition;
        }
        displayed.pixels = grown;
        displayed_capacity = bytes;
    }
    if (!d3d8_overlay_key_compose(composition->pixels, composition->width, composition->height,
                                  composition->stride_bytes, &latched, displayed.pixels)) {
        return composition;
    }
    displayed.width = composition->width;
    displayed.height = composition->height;
    displayed.stride_bytes = composition->width * 4u;
    composed_frames++;
    for (uint32_t row = 0u; row < latched.height && (uint64_t)latched.top + row < displayed.height;
         row++) {
        for (uint32_t column = 0u;
             column < latched.width && (uint64_t)latched.left + column < displayed.width; column++) {
            const uint8_t *shown = displayed.pixels +
                                   (((size_t)latched.top + row) * displayed.width + latched.left + column) * 4u;
            const uint8_t *source = composition->pixels +
                                    (((size_t)latched.top + row) * composition->stride_bytes) +
                                    ((size_t)latched.left + column) * 4u;
            covered_pixels += memcmp(shown, source, 4u) != 0 ? 1u : 0u;
        }
    }
    return &displayed;
}

uint64_t d3d8_overlay_key_generation(void)
{
    return layer_generation;
}

bool d3d8_overlay_key_active(void)
{
    return enabled && have_layer;
}

uint64_t d3d8_overlay_key_frames(void)
{
    return composed_frames;
}

uint64_t d3d8_overlay_key_disabled_layers(void)
{
    return disabled_layers;
}

uint64_t d3d8_overlay_key_covered_pixels(void)
{
    return covered_pixels;
}

size_t d3d8_overlay_key_summary(char *text, size_t size)
{
    if (!enabled) {
        if (text != NULL && size != 0u) {
            text[0] = '\0';
        }
        return 0u;
    }
    const int written = snprintf(
        text, size,
        "overlay over the replayed frame ON (opt-in, T831, %s): key bit clear shows the overlay on every pixel of its "
        "rectangle, key bit set only where the framebuffer RGB equals the key RGB (integer compare, alpha ignored; "
        "xemu's float artifact not reproduced); the last UpdateOverlay picture stays displayed until the next "
        "EnableOverlay (either argument, T839: xemu hides the overlay when it runs) or UpdateOverlay; frames composed "
        "%llu, pixels changed %llu, layers dropped by EnableOverlay %llu",
        D3D8_OVERLAY_KEY_LABEL, (unsigned long long)composed_frames,
        (unsigned long long)covered_pixels, (unsigned long long)disabled_layers);
    return written < 0 ? 0u : (size_t)written;
}
