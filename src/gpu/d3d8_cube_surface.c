/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_cube_surface.h"
#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "guest_mem.h"
#include "kernel_call.h"
#include <stdlib.h>

#define ENTRY 0x003D4E90u
#define LEVEL_ENTRY 0x003D4E10u
static uint32_t surface_heap;
typedef struct owned_surface {
    uint32_t address;
    uint32_t heap_token;
    bool live;
    struct owned_surface *next;
} owned_surface;
static owned_surface *owned;

bool d3d8_cube_surface_owned(uint32_t address)
{
    if (!guest_heap_valid(surface_heap)) return false;
    for (const owned_surface *item = owned; item; item = item->next) {
        uint32_t bytes = 0u;
        if (item->address == address && item->heap_token == surface_heap && item->live)
            return guest_heap_block_size(surface_heap, address, &bytes) && bytes == 24u;
    }
    return false;
}
bool d3d8_cube_surface_retired(uint32_t address)
{
    if (!guest_heap_valid(surface_heap)) return false;
    for (const owned_surface *item = owned; item; item = item->next)
        if (item->address == address && item->heap_token == surface_heap)
            return !item->live;
    return false;
}
bool d3d8_cube_surface_free_owned(uint32_t address)
{
    if (!d3d8_cube_surface_owned(address)) return false;
    if (!guest_heap_free(surface_heap, address)) return false;
    for (owned_surface *item = owned; item; item = item->next)
        if (item->address == address) { item->live = false; break; }
    return true;
}

static uint32_t mip_bytes(uint32_t width, uint32_t height, uint32_t depth,
                          uint32_t floor, uint32_t bits)
{
    if (width < floor) width = floor;
    if (height < floor) height = floor;
    return ((1u << ((width + height + depth) & 31u)) * bits) >> 3u;
}
static uint32_t preceding_mips(uint32_t count, uint32_t width, uint32_t height,
                               uint32_t depth, uint32_t floor, uint32_t bits)
{
    uint32_t total = 0u;
    const uint32_t steps = count < 16u ? count : 16u;
    for (uint32_t i = 0u; i < steps; i++) {
        total += mip_bytes(width, height, depth, floor, bits);
        if (width != 0u) width--;
        if (height != 0u) height--;
        if (depth != 0u) depth--;
    }
    /* All exponents extracted from the header are <=15. Beyond the first16
     * mips the same addition repeats; multiply the tail modulo 2^32. */
    if (count > steps) total += (count - steps) * mip_bytes(0u, 0u, 0u, floor, bits);
    return total;
}
static uint32_t diminished(uint32_t exponent, uint32_t level)
{
    return exponent > level ? exponent - level : 0u;
}

static uint32_t get_surface(uint32_t entry, uint32_t texture, uint32_t face, uint32_t level)
{
    if (d3d8_cube_surface_retired(texture))
        d3d8_hle_fatal(entry, "texture %#x is a retired owned surface header", texture);
    uint32_t words[5];
    if (!kernel_guest_read_bytes(texture, words, sizeof(words)))
        d3d8_hle_fatal(entry, "texture header is unreadable for 20 bytes");
    const uint32_t common = words[0];
    if ((common & 0xFFFFu) == 0u && (common & 0x70000u) == 0x50000u) {
        uint32_t parent;
        if (texture > UINT32_MAX - 23u || !kernel_guest_read_u32(texture + 20u, &parent))
            d3d8_hle_fatal(entry, "resource parent is unreadable");
        if (parent != 0u)
            d3d8_hle_fatal(entry, "recursive resource AddRef requires original 0x003D4C50");
    }
    uint32_t format = words[3];
    const uint32_t format_number = (format >> 8u) & 0xFFu;
    if (format_number > 0x3Fu)
        d3d8_hle_fatal(entry, "unsupported surface format %#x", format_number);
    uint8_t metadata;
    if (!kernel_guest_read_u8(0x003E1828u + format_number, &metadata))
        d3d8_hle_fatal(entry, "surface format metadata is unreadable");
    const uint32_t bits = metadata & 0x3Cu;
    const uint32_t size = words[4];
    uint32_t data = words[1] | 0x80000000u;
    if (size == 0u) {
        uint32_t width = (format >> 20u) & 15u;
        uint32_t height = (format >> 24u) & 15u;
        uint32_t depth = format >> 28u;
        const uint32_t floor = format_number == 12u || format_number == 14u || format_number == 15u ? 2u : 0u;
        if (face != 0u) {
            /* Original face-chain summation uses width+height only; it decrements
             * depth too, but never includes depth in that face-size shift. */
            const uint32_t chain = preceding_mips((format >> 16u) & 15u, width, height, 0u, floor, bits);
            data += ((chain + 127u) & ~127u) * face;
        }
        data += preceding_mips(level, width, height, depth, floor, bits);
        width = diminished(width, level);
        height = diminished(height, level);
        depth = diminished(depth, level);
        format = (((depth << 4u) | height) << 4u) | width;
        format = (format << 20u) | (words[3] & 0xFFFFFu);
    }
    /* Identical-content permission probe before allocation; mapping permissions
     * must remain quiescent through publication and the Common increment. */
    if (!kernel_guest_write_bytes(texture, &common, sizeof(common)))
        d3d8_hle_fatal(entry, "resource Common is not writable");
    /* XMemAlloc(24,64800000) originally uses the title's LocalAlloc heap.
     * Keep a real, separately owned guest heap until title heap ownership and
     * last-reference destruction are recovered; never fabricate a header VA. */
    if (surface_heap != 0u && !guest_heap_valid(surface_heap)) surface_heap = 0u;
    if (surface_heap == 0u || !guest_heap_valid(surface_heap)) {
        while (owned) {
            owned_surface *next = owned->next;
            free(owned);
            owned = next;
        }
        surface_heap = guest_heap_create(0u, GUEST_HEAP_CHUNK_MIN, 0u);
        if (surface_heap == 0u) return 0u;
    }
    owned_surface *record = malloc(sizeof(*record));
    if (!record) return 0u;
    const uint32_t header = guest_heap_alloc(surface_heap, 24u);
    if (header == 0u) { free(record); return 0u; }
    /* Replace an old tombstone when this same heap reuses a released address. */
    owned_surface **link = &owned;
    while (*link) {
        if ((*link)->address == header) {
            owned_surface *previous = *link;
            *link = previous->next;
            free(previous);
            break;
        }
        link = &(*link)->next;
    }
    *record = (owned_surface){header, surface_heap, true, owned};
    owned = record;
    d3d8_guest_store32(header, 0x01050001u);
    d3d8_guest_store32(header + 4u, data & 0x0FFFFFFFu);
    d3d8_guest_store32(header + 8u, 0u);
    d3d8_guest_store32(header + 12u, format);
    d3d8_guest_store32(header + 16u, size);
    d3d8_guest_store32(header + 20u, texture);
    d3d8_guest_store32(texture, common + 1u);
    return header;
}
uint32_t d3d8_get_cube_map_surface2(uint32_t texture, uint32_t face, uint32_t level)
{
    return get_surface(ENTRY, texture, face, level);
}
uint32_t d3d8_get_surface_level2(uint32_t texture, uint32_t level)
{
    return get_surface(LEVEL_ENTRY, texture, 0u, level);
}
void d3d8_cube_surface_reset(void)
{
    if (surface_heap != 0u && guest_heap_valid(surface_heap)) (void)guest_heap_destroy(surface_heap);
    surface_heap = 0u;
    while (owned) {
        owned_surface *next = owned->next;
        free(owned);
        owned = next;
    }
}
static uint32_t handler_cube_surface(void *context)
{
    uint32_t arguments[3];
    for (unsigned i = 0u; i < 3u; i++) {
        if (!kernel_frame_arg((const kernel_call_frame *)context, i, &arguments[i]))
            d3d8_hle_fatal(ENTRY, "cube surface argument %u is unreadable", i);
    }
    return d3d8_get_cube_map_surface2(arguments[0], arguments[1], arguments[2]);
}
static uint32_t handler_surface_level(void *context)
{
    uint32_t texture, level;
    if (!kernel_frame_arg(context, 0u, &texture) || !kernel_frame_arg(context, 1u, &level))
        d3d8_hle_fatal(LEVEL_ENTRY, "surface level arguments are unreadable");
    return d3d8_get_surface_level2(texture, level);
}
size_t d3d8_cube_surface_register(void)
{
    return (size_t)d3d8_hle_register(ENTRY, handler_cube_surface)
        + (size_t)d3d8_hle_register(LEVEL_ENTRY, handler_surface_level);
}
