/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_lock.h"
#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "d3d8_resource.h"
#include "live_texture_watch.h"
#include "kernel_call.h"
#include <stdbool.h>

#define SURFACE_ENTRY 0x003D4AC0u
#define TEXTURE_ENTRY 0x003D4E60u
#define DEVICE_GLOBAL 0x003E3F58u
#define DEVICE_BYTES 0x2500u
#define FORMAT_TABLE 0x003E1828u

static void read_span(uint32_t entry, uint32_t address, void *out, size_t bytes)
{
    if (!kernel_guest_read_bytes(address, out, bytes))
        d3d8_hle_fatal(entry, "lock input span %#x/%zu is unreadable", address, bytes);
}

static bool overlaps(uint32_t a, uint32_t a_bytes, uint32_t b, uint32_t b_bytes)
{
    return (uint64_t)a < (uint64_t)b + b_bytes && (uint64_t)b < (uint64_t)a + a_bytes;
}

static uint32_t parent_of(uint32_t entry, uint32_t resource, uint32_t common)
{
    if ((common & 0x70000u) != 0x50000u) return 0u;
    uint32_t parent;
    read_span(entry, resource + 20u, &parent, sizeof(parent));
    return parent;
}

/* The shared level-zero layout matches original 003DB8A0. Surface headers already
 * contain their face/level adjustment; texture callers must separately prove mip0. */
static uint32_t lock_level_zero(uint32_t entry, uint32_t resource, uint32_t locked_rect,
                                uint32_t rect, uint32_t flags, bool texture)
{
    uint32_t header[5], parent_header[5], rectangle[2] = {0u, 0u};
    read_span(entry, resource, header, sizeof(header));
    if (rect != 0u) read_span(entry, rect, rectangle, sizeof(rectangle));
    /* Flag 0x40 (surface only, texture callers filter it out) selects the uncached
     * alias 0xF0000000 where the original ORs 0x80000000 for the cached one. Nothing
     * else in Lock2DSurface 0x003DBAD0 differs (measured T393, original 0x3D4AC0).
     * Guest memory here is coherent with one mapping per allocation, so both Bits
     * values are the same registered address. Only write-combining is unmodelled. */
    if ((header[0] & 0xFFFFu) == 0u)
        d3d8_hle_fatal(entry, "locked resource has no live reference");
    const uint32_t parent = parent_of(entry, resource, header[0]);
    const uint32_t owner = parent != 0u ? parent : resource;
    const uint32_t *owner_header = header;
    if (parent != 0u) {
        read_span(entry, parent, parent_header, sizeof(parent_header));
        owner_header = parent_header;
    }
    if ((owner_header[0] & 0xFFFFu) == 0u)
        d3d8_hle_fatal(entry, "registered lock owner has no live reference");
    uint32_t device = 0u;
    if (texture || (flags & 0x20u) == 0u)
        read_span(entry, DEVICE_GLOBAL, &device, sizeof(device));
    const bool busy = ((header[0] | owner_header[0]) & 0x780000u) != 0u;
    if (texture && (busy || owner_header[2] != 0u))
        d3d8_hle_fatal(entry, "texture lock is outside the idle registered resource scope");
    if ((flags & 0x20u) == 0u && device != 0u) {
        bool owner_parent_busy = false;
        /* Preserve the previous surface preflight's second resource_busy(parent)
         * check. A registered class5 parent can itself name a grandparent. The
         * previous gate observes only that next common word, not an unbounded
         * parent chain, and flags20 / no-device bypass this read entirely. */
        if (!busy && parent != 0u) {
            const uint32_t owner_parent = parent_of(entry, owner, owner_header[0]);
            if (owner_parent != 0u) {
                uint32_t owner_parent_common;
                read_span(entry, owner_parent, &owner_parent_common, sizeof(owner_parent_common));
                owner_parent_busy = (owner_parent_common & 0x780000u) != 0u;
            }
        }
        if (busy || owner_parent_busy || owner_header[2] != 0u)
            d3d8_hle_fatal(entry, "lock needs resource fence wait 0x003D6BA0");
    }

    const uint32_t format = header[3];
    const uint32_t format_number = (format >> 8u) & 0xFFu;
    if (texture && ((header[0] & 0x70000u) != 0x40000u || header[4] != 0u ||
                    (format & 0x000FFFFFu) != 0x00010629u || (format >> 28u) != 0u))
        d3d8_hle_fatal(entry, "texture lock requires single-level swizzled 2D format6");
    if (format_number > 63u)
        d3d8_hle_fatal(entry, "lock format %#x is not recovered", format_number);
    uint8_t info;
    read_span(entry, FORMAT_TABLE + format_number, &info, sizeof(info));
    const uint32_t bits = info & 0x3Cu;
    if (bits == 0u || (texture && bits != 32u))
        d3d8_hle_fatal(entry, "lock format has unsupported bits per pixel");
    const uint32_t size = header[4];
    uint32_t pitch;
    uint64_t extent;
    if (size != 0u) {
        pitch = ((size >> 24u) + 1u) << 6u;
        extent = (uint64_t)(((size >> 12u) & 0xFFFu) + 1u) * pitch;
    } else {
        uint32_t width = (format >> 20u) & 15u;
        uint32_t height = (format >> 24u) & 15u;
        const uint32_t depth = format >> 28u;
        const bool compressed = format_number == 12u || format_number == 14u || format_number == 15u;
        if (compressed && width < 2u) width = 2u;
        if (compressed && height < 2u) height = 2u;
        pitch = format_number == 12u ? (1u << width) << 1u
              : format_number == 14u || format_number == 15u ? (1u << width) << 2u
              : ((1u << width) * bits) >> 3u;
        extent = ((UINT64_C(1) << (width + height + depth)) * bits) >> 3u;
    }
    if (extent == 0u || extent > UINT32_MAX)
        d3d8_hle_fatal(entry, "lock extent overflows the supported alias span");
    const uint32_t address = d3d8_resource_resolve_registered_alias_at(entry, owner, header[1],
                                                                    (uint32_t)extent);
    uint64_t offset = 0u;
    if (rect != 0u) {
        const uint64_t left_bits = (uint64_t)rectangle[0] * bits;
        const uint64_t top_bytes = (uint64_t)rectangle[1] * pitch;
        offset = top_bytes + (left_bits >> 3u);
        if (left_bits > UINT32_MAX || top_bytes > UINT32_MAX || offset >= extent)
            d3d8_hle_fatal(entry, "rectangle offset overflows or exceeds the lock span");
    }
    if (texture && (overlaps(locked_rect, 8u, resource, 20u) ||
                    overlaps(locked_rect, 8u, address, (uint32_t)extent) ||
                    overlaps(locked_rect, 8u, DEVICE_GLOBAL, 4u) ||
                    overlaps(locked_rect, 8u, FORMAT_TABLE, 64u) ||
                    (device != 0u && overlaps(locked_rect, 8u, device, DEVICE_BYTES))))
        d3d8_hle_fatal(entry, "texture lock output aliases resource, backing or device state");

    /* Mappings/content must remain quiescent across this compound operation.
     * This identical-byte probe IS a guest write. It preserves all bytes on the
     * protected/read-only/cross-page refusals and precedes publication. */
    uint32_t previous[2];
    read_span(entry, locked_rect, previous, sizeof(previous));
    if (!kernel_guest_write_bytes(locked_rect, previous, sizeof(previous)))
        d3d8_hle_fatal(entry, "locked rectangle output span is not writable");
    /* T792: the title writes the texels through this pointer, so the live texture cache must treat the Data range as stale. */
    live_texture_watch_note(header[1] & 0x0FFFFFFFu, (uint32_t)extent);
    const uint32_t result[2] = {pitch, address + (uint32_t)offset};
    if (!kernel_guest_write_bytes(locked_rect, result, sizeof(result)))
        d3d8_hle_fatal(entry, "locked rectangle mapping changed during publication");
    return rect == 0u ? locked_rect : pitch;
}

uint32_t d3d8_surface_lock_rect(uint32_t surface, uint32_t locked_rect, uint32_t rect, uint32_t flags)
{
    return lock_level_zero(SURFACE_ENTRY, surface, locked_rect, rect, flags, false);
}

uint32_t d3d8_texture_lock_rect(uint32_t texture, uint32_t level, uint32_t locked_rect,
                              uint32_t rect, uint32_t flags)
{
    if (level != 0u || rect != 0u || (flags != 0u && flags != 0x20u))
        d3d8_hle_fatal(TEXTURE_ENTRY, "texture lock supports mip0, NULLrect and flags0/20 only");
    return lock_level_zero(TEXTURE_ENTRY, texture, locked_rect, rect, flags, true);
}

static uint32_t handler_surface_lock_rect(void *context)
{
    uint32_t arguments[4];
    for (unsigned i = 0u; i < 4u; i++) {
        if (!kernel_frame_arg((const kernel_call_frame *)context, i, &arguments[i]))
            d3d8_hle_fatal(SURFACE_ENTRY, "surface lock argument %u is unreadable", i);
    }
    return d3d8_surface_lock_rect(arguments[0], arguments[1], arguments[2], arguments[3]);
}

static uint32_t handler_texture_lock_rect(void *context)
{
    uint32_t arguments[5];
    for (unsigned i = 0u; i < 5u; i++) {
        if (!kernel_frame_arg((const kernel_call_frame *)context, i, &arguments[i]))
            d3d8_hle_fatal(TEXTURE_ENTRY, "texture lock argument %u is unreadable", i);
    }
    return d3d8_texture_lock_rect(arguments[0], arguments[1], arguments[2], arguments[3], arguments[4]);
}

size_t d3d8_lock_register(void)
{
    size_t registered = (size_t)d3d8_hle_register(SURFACE_ENTRY, handler_surface_lock_rect);
    registered += (size_t)d3d8_hle_register(TEXTURE_ENTRY, handler_texture_lock_rect);
    return registered;
}
