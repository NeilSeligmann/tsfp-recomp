/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_vertex_program.h"
#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "d3d8_pushbuffer.h"
#include "kernel_call.h"
#include <stdbool.h>
#include <string.h>

#define ENTRY 0x003D59E0u
#define DEVICE_GLOBAL 0x003E3F58u
#define DEVICE_GUARD_BYTES 0x2500u
#define PROGRAM_OFFSET 0x10A8u
#define MAX_DWORDS (D3D8_VERTEX_PROGRAM_SLOTS * 4u)
#define MAX_SOURCE_BYTES (2u + MAX_DWORDS * 4u)
#define MAX_PACKET_DWORDS (2u + MAX_DWORDS + (MAX_DWORDS + 31u) / 32u)

static bool overlap(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    return an != 0u && bn != 0u && (uint64_t)a < (uint64_t)b + bn &&
           (uint64_t)b < (uint64_t)a + an;
}

static void read_span(uint32_t address, void *out, size_t bytes)
{
    if (bytes != 0u && !kernel_guest_read_bytes(address, out, bytes))
        d3d8_hle_fatal(ENTRY, "vertex program input span %#x/%zu is unreadable", address, bytes);
}

static void probe(uint32_t address, size_t bytes)
{
    uint8_t old[MAX_PACKET_DWORDS * 4u];
    if (bytes != 0u && (!kernel_guest_read_bytes(address, old, bytes) ||
                       !kernel_guest_write_bytes(address, old, bytes)))
        d3d8_hle_fatal(ENTRY, "vertex program output span %#x/%zu is not readable/writable",
                       address, bytes);
}

uint32_t d3d8_upload_vertex_program(uint32_t packed, uint32_t first_slot)
{
    if (first_slot >= D3D8_VERTEX_PROGRAM_SLOTS)
        d3d8_hle_fatal(ENTRY, "vertex program exceeds the host136-slot policy (original unchecked)");
    uint32_t device, control[3];
    uint16_t header;
    read_span(DEVICE_GLOBAL, &device, sizeof(device));
    if (device != D3D8_DEVICE_BASE)
        d3d8_hle_fatal(ENTRY, "vertex program requires the recovered fixed device");
    read_span(device, control, sizeof(control));
    if (packed > UINT32_MAX - 2u)
        d3d8_hle_fatal(ENTRY, "vertex program count address wraps");
    const uint32_t count_address = packed + 2u;
    read_span(count_address, &header, sizeof(header));
    const uint32_t instructions = header;
    if (instructions > D3D8_VERTEX_PROGRAM_SLOTS - first_slot)
        d3d8_hle_fatal(ENTRY, "vertex program exceeds the host136-slot policy (original unchecked)");
    const uint32_t dwords = instructions * 4u;
    const uint32_t source_bytes = 4u + dwords * 4u;
    const uint32_t chunks = dwords == 0u ? 1u : (dwords + 31u) / 32u;
    const uint32_t packet_bytes = (2u + dwords + chunks) * 4u;
    const uint32_t reserve_bytes = (dwords + 19u) * 4u;
    const uint32_t cursor = control[0], limit = control[1];
    if ((uint64_t)cursor + reserve_bytes > UINT32_MAX || (uint64_t)limit + 0x200u > UINT32_MAX)
        d3d8_hle_fatal(ENTRY, "vertex program reservation arithmetic overflows");
    /* 0x003D5A14 to 0x003D5A18: the sized reservation 0x003D6B30(dwords + 19), unconditionally. It rolls the ring
     * over (T487 refill, cursor published first) when cursor + n*4 >= limit + 0x200 and the packet then starts at the
     * new cursor. Plan it read-only first: the plan is fatal exactly where the real refill is, and every span the
     * refill and the packet write is checked against the source the original reads (sources are read before the
     * refill for the cache copy and after it for the packet, so a source a refill moves is not recovered). */
    d3d8_pushbuffer_sim sim = d3d8_pushbuffer_sim_start();
    (void)d3d8_pushbuffer_sim_reserve(&sim, dwords + 19u);
    const uint32_t start = d3d8_pushbuffer_sim_write(&sim, ENTRY, packet_bytes);
    if ((uint64_t)packed + source_bytes > UINT64_C(0x100000000) ||
        overlap(packed, source_bytes, device, DEVICE_GUARD_BYTES) ||
        overlap(packed, source_bytes, DEVICE_GLOBAL, 4u) ||
        overlap(start, packet_bytes, device, DEVICE_GUARD_BYTES) ||
        overlap(start, packet_bytes, DEVICE_GLOBAL, 4u))
        d3d8_hle_fatal(ENTRY, "vertex program source, command or device aliases are unsupported");
    for (uint32_t i = 0u; i < sim.span_count; i++)
        if (overlap(packed, source_bytes, sim.span[i].begin, sim.span[i].bytes))
            d3d8_hle_fatal(ENTRY, "vertex program source aliases a refill or packet write");

    /* Original reads only the high count word and payload: the low header word
     * can be inaccessible. Snapshot exactly those bytes before permission probes.
     * The alias policy still conservatively covers the complete header region. */
    uint8_t source[MAX_SOURCE_BYTES];
    read_span(count_address, source, 2u + dwords * 4u);
    uint16_t saved_header;
    memcpy(&saved_header, source, sizeof(saved_header));
    if (saved_header != header)
        d3d8_hle_fatal(ENTRY, "vertex program header changed during input snapshot");
    const uint32_t cache = device + PROGRAM_OFFSET + first_slot * 16u;
    probe(start, packet_bytes);
    probe(device, 4u);
    if ((control[2] & 0x10u) == 0u) probe(cache, dwords * 4u);

    uint32_t packet[MAX_PACKET_DWORDS];
    packet[0] = 0x00041E9Cu;
    packet[1] = first_slot;
    uint32_t at = 2u, copied = 0u;
    do {
        const uint32_t remaining = dwords - copied;
        const uint32_t chunk = remaining < 32u ? remaining : 32u;
        packet[at++] = (chunk << 18u) + 0x0B00u;
        if (chunk != 0u) memcpy(packet + at, source + 2u + copied * 4u, (size_t)chunk * 4u);
        at += chunk;
        copied += chunk;
    } while (copied != dwords);
    /* Original order: the CPU cache copy (0x003D59FC), then the reservation, then the packet. */
    if ((control[2] & 0x10u) == 0u && dwords != 0u &&
        !kernel_guest_write_bytes(cache, source + 2u, dwords * 4u))
        d3d8_hle_fatal(ENTRY, "vertex program mapping changed during publication");
    if (d3d8_pushbuffer_reserve(dwords + 19u) != start)
        d3d8_hle_fatal(ENTRY, "vertex program plan disagrees with the sized reservation");
    const uint32_t next = start + packet_bytes;
    if (!kernel_guest_write_bytes(start, packet, packet_bytes))
        d3d8_hle_fatal(ENTRY, "vertex program mapping changed during publication");
    /* Use the existing publisher so host command accounting stays identical to the old upload handler. */
    d3d8_pushbuffer_end(next);
    return next;
}
