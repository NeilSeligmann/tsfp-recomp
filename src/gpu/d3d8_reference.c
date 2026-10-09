/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_reference.h"
#include "d3d8_cube_surface.h"
#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "kernel_call.h"
#include <stdbool.h>
#include <stddef.h>

#define LIMIT 64u
#define TYPE_MASK 0x70000u
#define SURFACE 0x50000u
#define BUSY 0x780000u
#define BINDING 0x80000u

typedef struct {
    uint32_t header, after, span;
    bool destroy;
} change;

/* Distinct guest headers can alias through unaligned pointers. A snapshot plan
 * cannot reproduce the original live reloads when one Common write overlaps
 * another header's required read span, so reject these before any mutation. */
static void reject_overlap(const change *left, const change *right, uint32_t entry)
{
    if ((uint64_t)left->header < (uint64_t)right->header + right->span &&
        (uint64_t)right->header < (uint64_t)left->header + left->span)
        d3d8_hle_fatal(entry, "overlapping resource headers %#x and %#x", left->header, right->header);
}
static uint32_t common_at(uint32_t header, uint32_t entry)
{
    if (d3d8_cube_surface_retired(header))
        d3d8_hle_fatal(entry, "resource %#x is a retired cube surface", header);
    uint32_t common = 0u;
    if (!kernel_guest_read_u32(header, &common))
        d3d8_hle_fatal(entry, "resource %#x Common is unreadable or overflows", header);
    return common;
}
static uint32_t parent_at(uint32_t header, uint32_t entry)
{
    uint32_t parent = 0u;
    if (header > UINT32_MAX - 23u || !kernel_guest_read_u32(header + 20u, &parent))
        d3d8_hle_fatal(entry, "surface %#x parent field is unreadable or overflows", header);
    return parent;
}
static void validate_destroy(uint32_t header, uint32_t common)
{
    if ((common & TYPE_MASK) != SURFACE || (common & 0x80000000u) != 0u ||
        !d3d8_cube_surface_owned(header))
        d3d8_hle_fatal(0x003D4B30u, "resource %#x destruction requires unsupported allocator or data ownership", header);
    if (d3d8_guest_load32(0x003E2BA4u) != 0u)
        d3d8_hle_fatal(0x003D4B30u, "resource private-data destruction is not recovered");
}
static void apply(const change *item)
{
    if (item->destroy) {
        if (!d3d8_cube_surface_free_owned(item->header))
            d3d8_hle_fatal(0x003D4B30u, "owned resource allocation changed after preflight");
    } else {
        d3d8_guest_store32(item->header, item->after);
    }
}
static uint32_t reference(uint32_t header, bool add)
{
    const uint32_t entry = add ? 0x003D4C50u : 0x003D4C90u;
    change plan[LIMIT];
    size_t count = 0u;
    uint32_t next = header;
    while (next != 0u || count == 0u) {
        if (count == LIMIT)
            d3d8_hle_fatal(entry, "resource parent chain exceeds %u entries", LIMIT);
        for (size_t i = 0u; i < count; i++)
            if (plan[i].header == next)
                d3d8_hle_fatal(entry, "cyclic resource parent chain at %#x", next);
        const uint32_t common = common_at(next, entry);
        const bool last = (common & 0xFFFFu) == (add ? 0u : 1u);
        const bool follows_parent = last && (common & TYPE_MASK) == SURFACE;
        const bool destroy = !add && last && (common & BUSY) == 0u;
        const change item = {next, add ? common + 1u : common - 1u,
                             follows_parent ? 24u : 4u, destroy};
        for (size_t i = 0u; i < count; i++) reject_overlap(&plan[i], &item, entry);
        if (destroy) validate_destroy(next, common);
        plan[count++] = item;
        next = follows_parent ? parent_at(next, entry) : 0u;
    }
    if (add) {
        /* Identical-byte probes publish no new guest content. The caller owns
         * exclusive, quiescent mappings through preflight and all publication. */
        for (size_t i = 0u; i < count; i++) {
            const uint32_t before = plan[i].after - 1u;
            if (!kernel_guest_write_bytes(plan[i].header, &before, sizeof(before)))
                d3d8_hle_fatal(entry, "resource %#x Common is not writable", plan[i].header);
        }
    }
    /* Recursion in both originals updates the deepest parent before its child.
     * The entire plan, including every destructor, is already proven supported. */
    for (size_t i = count; i != 0u; i--) apply(&plan[i - 1u]);
    return plan[0].destroy ? 0u : plan[0].after & 0xFFFFu;
}
uint32_t d3d8_reference_add_ref(uint32_t header) { return reference(header, true); }
uint32_t d3d8_reference_release(uint32_t header) { return reference(header, false); }

void d3d8_reference_release_binding(uint32_t header)
{
    const uint32_t entry = 0x003D4DA0u;
    const uint32_t common = common_at(header, entry);
    change child = {header, common - BINDING, 4u, false};
    change parent = {0u, 0u, 4u, false};
    if ((common & BUSY) == BINDING) {
        if ((common & TYPE_MASK) == SURFACE) {
            child.span = 24u;
            parent.header = parent_at(header, entry);
            if (parent.header == header && parent.header != 0u)
                d3d8_hle_fatal(entry, "cyclic binding parent at %#x", header);
            if (parent.header != 0u) {
                parent.after = common_at(parent.header, entry) - BINDING;
                parent.destroy = (parent.after & (BUSY | 0xFFFFu)) == 0u;
                if (parent.destroy) parent.span = 24u;
                reject_overlap(&child, &parent, entry);
                if (parent.destroy) validate_destroy(parent.header, parent.after);
            }
        }
        child.destroy = (common & 0xFFFFu) == 0u;
        if (child.destroy) validate_destroy(header, common);
    }
    if (parent.header != 0u) apply(&parent);
    apply(&child);
}
