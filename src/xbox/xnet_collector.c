/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xnet_collector.h"
#include "kernel_call.h"
#include <string.h>

static bool append(uint32_t *output, uint32_t *remaining, uint32_t data, uint32_t bytes)
{
    if (bytes > *remaining) return true;
    if (!bytes) return true;
    unsigned char segment[512];
    if (!kernel_guest_read_bytes(data, segment, bytes) ||
        !kernel_guest_write_bytes(*output, segment, bytes)) return false;
    *output += bytes;
    *remaining -= bytes;
    return true;
}
static bool call(const xnet_collector_source *source, unsigned ordinal,
                 const uint32_t *arguments, size_t count, uint64_t *result)
{
    return source->kernel_call(source->context, ordinal, arguments, count, result);
}

bool xnet_collect_entropy(uint32_t output, uint32_t capacity, uint32_t scratch,
                          const xnet_collector_source *source, uint32_t *written)
{
    if (!source || !source->kernel_call || !source->volume_space || !source->disk_identity_ready || !written ||
        capacity > 512u || !scratch || (uint64_t)scratch + 0x200u > UINT32_MAX ||
        (capacity && !output) || (uint64_t)output + capacity > UINT32_MAX ||
        (capacity && ((uint64_t)output < (uint64_t)scratch + 0x200u &&
                      (uint64_t)scratch < (uint64_t)output + capacity))) return false;
    uint32_t next = output, remaining = capacity, tick_pointer, tick;
    uint64_t result;
    if (!kernel_guest_read_u32(0x475868u, &tick_pointer) ||
        !kernel_guest_read_u32(tick_pointer, &tick) ||
        !kernel_guest_write_u32(scratch + 0x150u, tick) ||
        !append(&next, &remaining, scratch + 0x150u, 4u)) return false;
    const uint32_t time_arguments[] = {scratch + 0x12cu};
    if (!call(source, 128u, time_arguments, 1u, &result) ||
        !append(&next, &remaining, time_arguments[0], 8u)) return false;
    if (!call(source, 126u, NULL, 0u, &result) ||
        !kernel_guest_write_u32(scratch + 0x150u, (uint32_t)result ^ (uint32_t)(result >> 32)) ||
        !append(&next, &remaining, scratch + 0x150u, 4u)) return false;
    const uint32_t statistics_arguments[] = {scratch + 0x100u};
    /* Original ignores NTSTATUS; bytes are consumed even on a real failing call. */
    if (!call(source, 181u, statistics_arguments, 1u, &result) ||
        !append(&next, &remaining, scratch + 0x108u, 2u) ||
        !append(&next, &remaining, scratch + 0x118u, 2u) ||
        !append(&next, &remaining, scratch + 0x111u, 1u)) return false;
    const uint32_t volume_output[] = {scratch + 0x140u, scratch + 0x124u, scratch + 0x138u};
    uint32_t volume_result;
    if (!source->volume_space(source->context, 0x44481cu, volume_output, &volume_result))
        return false;
    if (volume_result && (!append(&next, &remaining, volume_output[0], 8u) ||
                          !append(&next, &remaining, volume_output[2], 8u))) return false;
    const uint32_t nv_arguments[] = {0xffffu, scratch + 0x134u, scratch, 256u,
                                     scratch + 0x15cu};
    if (!call(source, 24u, nv_arguments, 5u, &result)) return false;
    if ((uint32_t)result < 0x80000000u) {
        uint32_t length;
        if (!kernel_guest_read_u32(nv_arguments[4], &length) || length > 256u ||
            !append(&next, &remaining, scratch, length)) return false;
    }
    if (!source->disk_identity_ready(source->context)) return false;
    const uint32_t disk_slots[] = {0x4759b8u, 0x4759b4u};
    for (unsigned i = 0u; i < 2u; ++i) {
        uint32_t descriptor, buffer;
        uint8_t length_bytes[2];
        if (!kernel_guest_read_u32(disk_slots[i], &descriptor) ||
            !kernel_guest_read_bytes(descriptor, length_bytes, sizeof(length_bytes)) ||
            !kernel_guest_read_u32(kernel_guest_add(descriptor, 4u), &buffer)) return false;
        const uint32_t length = length_bytes[0] | ((uint32_t)length_bytes[1] << 8);
        if (!append(&next, &remaining, buffer, length)) return false;
    }
    *written = capacity - remaining;
    return true;
}
