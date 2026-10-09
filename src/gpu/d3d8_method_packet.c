/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Retail D3D entry 0x003D52F0. Its two Story callers select method 0x41948 and pass a
 * scalar from 0x00665448. The original transforms that scalar's bytes and appends one
 * header/value pair to the pushbuffer.
 */
#include "d3d8_method_packet.h"

#include "d3d8_hle.h"
#include "d3d8_guest.h"
#include "d3d8_pushbuffer.h"
#include "kernel_call.h"

#define METHOD_PACKET_VA 0x003D52F0u

static uint32_t frame_arg(void *context, unsigned index)
{
    uint32_t value = 0u;
    if (!kernel_frame_arg((const kernel_call_frame *)context, index, &value)) {
        d3d8_hle_fatal(METHOD_PACKET_VA, "method packet argument %u cannot be read", index);
    }
    return value;
}

static uint32_t handler_method_packet(void *context)
{
    const uint32_t method_index = frame_arg(context, 0u);
    const uint32_t input = frame_arg(context, 1u);
    const uint32_t value = (input & 0xFF00FF00u) | ((input & 0xFFu) << 16) |
                           ((input >> 16) & 0xFFu);
    const uint32_t cursor = d3d8_pushbuffer_begin();

    /* Match the original's write-before-publish order. The retail body's cursor test is PUT >=
     * limit (implemented by begin); it does not reserve against PUT+8. */
    d3d8_guest_store32(cursor, 0x00041940u + method_index * 4u);
    d3d8_guest_store32(cursor + 4u, value);
    d3d8_pushbuffer_end(cursor + 8u);
    return cursor + 8u;
}

size_t d3d8_method_packet_register(void)
{
    return (size_t)d3d8_hle_register(METHOD_PACKET_VA, handler_method_packet);
}
