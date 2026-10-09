/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See d3d8_surface.h. Every function carries the address of the original it ports.
 */

#include "d3d8_surface.h"

#include <stdbool.h>

#include "d3d8_guest.h"
#include "d3d8_hle.h"

/* Per-format information, one byte per hardware format number, in D3D8's `.data`.
 * Bit 0 marks a swizzled format and bits 2-5 are the bits per pixel (0x3C masks them). */
#define FORMAT_INFO_TABLE 0x003E1828u
#define FORMAT_INFO_BPP_MASK 0x3Cu

/* The legal pitches, 26 ascending entries in D3D8's `.data`. */
#define PITCH_TABLE 0x003E17C0u
#define PITCH_TABLE_ENTRIES 26u

uint32_t d3d8_surface_normalise_format(uint32_t format)
{
    switch (format) {
    case 0x02u:
        return 0x10u;
    case 0x03u:
        return 0x1Cu;
    case 0x05u:
        return 0x11u;
    case 0x06u:
        return 0x12u;
    case 0x07u:
        return 0x1Eu;
    case 0x2Au:
        return 0x2Eu;
    case 0x2Bu:
        return 0x2Fu;
    case 0x2Cu:
        return 0x30u;
    case 0x2Du:
        return 0x31u;
    default:
        return format;
    }
}

static uint32_t format_bits_per_pixel(uint32_t format)
{
    return (uint32_t)d3d8_guest_load8(FORMAT_INFO_TABLE + (format & 0xFFu)) &
           FORMAT_INFO_BPP_MASK;
}

uint32_t d3d8_surface_aligned_row_bytes(uint32_t width, uint32_t format)
{
    return (((format_bits_per_pixel(format) * width) >> 3) + 0x3Fu) & ~0x3Fu;
}

uint32_t d3d8_surface_pitch_for_width(uint32_t width, uint32_t format)
{
    const uint32_t row_bytes = d3d8_surface_aligned_row_bytes(width, format);
    for (uint32_t entry = 0u; entry < PITCH_TABLE_ENTRIES; entry++) {
        const uint32_t legal = d3d8_guest_load32(PITCH_TABLE + entry * 4u);
        if (row_bytes <= legal) {
            return legal;
        }
    }
    return row_bytes;
}

/* The formats 0x003DBB70 sends down its swizzled path: the table's bit 0, and the 0xC and
 * 0xE-0xF compressed formats. */
static bool format_is_swizzled_or_compressed(uint32_t format)
{
    if ((d3d8_guest_load8(FORMAT_INFO_TABLE + (format & 0xFFu)) & 1u) != 0u) {
        return true;
    }
    return format == 0x0Cu || (format > 0x0Du && format <= 0x0Fu);
}

uint32_t d3d8_surface_linear_words(uint32_t width, uint32_t height, uint32_t format,
                                   uint32_t pitch, uint32_t *format_word, uint32_t *size_word)
{
    if (format_is_swizzled_or_compressed(format) || pitch == 0u) {
        d3d8_hle_fatal(0x003DBB70u,
                       "surface format 0x%02x (or a zero pitch) takes the swizzled path, which "
                       "is not ported",
                       (unsigned)format);
    }

    /* Levels 1, no depth, no cube, not compressed: the level count nibble is 1, the format
     * byte follows, then the constant 0x29 (2 for the non-compressed marker in bits 4-7 and
     * 9 in the low nibble). */
    *format_word = (1u << 16) | ((format & 0xFFu) << 8) | 0x29u;
    *size_word = ((((pitch >> 6) - 1u) << 12 | (height - 1u)) << 12) | (width - 1u);
    return height * pitch;
}

void d3d8_surface_init_header(uint32_t header, uint32_t format_word, uint32_t size_word,
                              uint32_t data)
{
    d3d8_guest_store32(header + D3D8_SURFACE_COMMON, 0x01050001u);
    d3d8_guest_store32(header + D3D8_SURFACE_DATA, data & 0x0FFFFFFFu);
    d3d8_guest_store32(header + D3D8_SURFACE_LOCK, 0u);
    d3d8_guest_store32(header + D3D8_SURFACE_FORMAT, format_word);
    d3d8_guest_store32(header + D3D8_SURFACE_SIZE, size_word);
    d3d8_guest_store32(header + D3D8_SURFACE_PARENT, 0u);
}

uint32_t d3d8_surface_header_pitch(uint32_t header)
{
    const uint32_t size_word = d3d8_guest_load32(header + D3D8_SURFACE_SIZE);
    if (size_word != 0u) {
        return (((size_word >> 24) & 0xFFu) + 1u) << 6;
    }
    /* No Size word: a swizzled surface, whose pitch follows from its width exponent in the
     * Format word (bits 20-23) and its format byte (bits 8-15). */
    const uint32_t format_word = d3d8_guest_load32(header + D3D8_SURFACE_FORMAT);
    const uint32_t width = 1u << ((format_word >> 20) & 0xFu);
    const uint32_t format = (format_word >> 8) & 0xFFu;
    if (format == 0x0Cu) {
        return width * 2u;
    }
    if (format > 0x0Du && format <= 0x0Fu) {
        return width * 4u;
    }
    return (format_bits_per_pixel(format) * width) >> 3;
}

bool d3d8_surface_dimensions(uint32_t header, uint32_t *width, uint32_t *height)
{
    if (header == 0u || width == NULL || height == NULL) {
        return false;
    }
    const uint32_t size_word = d3d8_guest_load32(header + D3D8_SURFACE_SIZE);
    if (size_word != 0u) {
        *width = (size_word & 0xFFFu) + 1u;
        *height = ((size_word >> 12) & 0xFFFu) + 1u;
        return true;
    }
    const uint32_t format_word = d3d8_guest_load32(header + D3D8_SURFACE_FORMAT);
    const uint32_t width_exponent = (format_word >> 20) & 0xFu;
    const uint32_t height_exponent = (format_word >> 24) & 0xFu;
    if (width_exponent == 0u && height_exponent == 0u) {
        return false;
    }
    *width = 1u << width_exponent;
    *height = 1u << height_exponent;
    return true;
}

uint32_t d3d8_resource_add_ref(uint32_t header)
{
    /* The original recurses on the parent of a surface whose own count is zero. */
    const uint32_t common = d3d8_guest_load32(header + D3D8_SURFACE_COMMON);
    if ((common & 0xFFFFu) == 0u && (common & 0x70000u) == 0x50000u) {
        const uint32_t parent = d3d8_guest_load32(header + D3D8_SURFACE_PARENT);
        if (parent != 0u) {
            (void)d3d8_resource_add_ref(parent);
        }
    }
    const uint32_t counted = common + 1u;
    d3d8_guest_store32(header + D3D8_SURFACE_COMMON, counted);
    return counted & 0xFFFFu;
}

void d3d8_resource_add_render_target_ref(uint32_t header)
{
    const uint32_t common = d3d8_guest_load32(header + D3D8_SURFACE_COMMON);
    if ((common & 0x780000u) == 0u && (common & 0x70000u) == 0x50000u) {
        const uint32_t parent = d3d8_guest_load32(header + D3D8_SURFACE_PARENT);
        if (parent != 0u) {
            d3d8_guest_store32(parent + D3D8_SURFACE_COMMON,
                               d3d8_guest_load32(parent + D3D8_SURFACE_COMMON) + 0x80000u);
        }
    }
    d3d8_guest_store32(header + D3D8_SURFACE_COMMON, common + 0x80000u);
}

uint32_t d3d8_surface_max_depth_bits(uint32_t format)
{
    switch (format) {
    case 0x2Au:
    case 0x2Eu:
        return 0x4B7FFFFFu;
    case 0x2Bu:
    case 0x2Fu:
        return 0x7149F2CAu;
    case 0x2Cu:
    case 0x30u:
        return 0x477FFF00u;
    case 0x2Du:
    case 0x31u:
        return 0x43FFF800u;
    default:
        d3d8_hle_fatal(0x003DB620u, "depth format 0x%02x has no entry in the max-depth table",
                       (unsigned)format);
    }
}

/* The colour class of each hardware format number (byte table at 0x003DB5E5), 10 meaning
 * the original has no entry and jumps through a null pointer. Only the entries a render
 * target can legitimately have are populated: 0x04-0x07, 0x11-0x13, 0x17, 0x1C and 0x1E. */
static int colour_class(uint32_t format)
{
    switch (format) {
    case 0x05u:
        return 1;
    case 0x06u:
        return 2;
    case 0x07u:
        return 3;
    case 0x11u:
        return 4;
    case 0x12u:
        return 5;
    case 0x13u:
        return 6;
    case 0x17u:
        return 7;
    case 0x1Cu:
        return 8;
    case 0x1Eu:
        return 9;
    case 0x00u:
    case 0x01u:
    case 0x02u:
    case 0x03u:
        return 0;
    default:
        return -1;
    }
}

uint32_t d3d8_surface_format_word(uint32_t render_target_header, uint32_t depth_header)
{
    const uint32_t format_word = d3d8_guest_load32(render_target_header + D3D8_SURFACE_FORMAT);
    const uint32_t format = (format_word >> 8) & 0xFFu;
    const int class_index = colour_class(format);
    if (class_index < 0) {
        d3d8_hle_fatal(0x003DB500u, "render target format 0x%02x has no surface-format entry",
                       (unsigned)format);
    }

    /* The jump table at 0x003DB5BC: base value per class, and whether the swizzled tail at
     * 0x003DB51B adds the size exponents. Classes 0 to 3 are the swizzled ones. */
    static const uint32_t base_value[10] = {1u, 3u, 8u, 4u, 3u, 8u, 9u, 10u, 1u, 4u};
    uint32_t word = base_value[class_index];
    if (class_index <= 3) {
        word |= (((format_word & 0x00F00000u) | 0x2000u) >> 4) | (format_word & 0x0F000000u);
    } else {
        word |= 0x100u;
    }

    if (depth_header == 0u) {
        const uint32_t bits = format_bits_per_pixel(format);
        return word | (bits == 0x20u ? 0x20u : 0x10u);
    }
    const uint32_t depth_format =
        (d3d8_guest_load32(depth_header + D3D8_SURFACE_FORMAT) >> 8) & 0xFFu;
    if (depth_format < 0x2Au || depth_format > 0x31u) {
        return word;
    }
    /* The class table at 0x003DB60C, indexed by format - 0x2A: formats 0x2A, 0x2B, 0x2E and
     * 0x2F add 0x20, formats 0x2C, 0x2D, 0x30 and 0x31 add 0x10. */
    static const uint8_t depth_is_16bit[8] = {0u, 0u, 1u, 1u, 0u, 0u, 1u, 1u};
    return word | (depth_is_16bit[depth_format - 0x2Au] != 0u ? 0x10u : 0x20u);
}
