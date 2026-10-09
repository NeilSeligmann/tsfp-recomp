/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xgrph_shader_query.h"
#include "kernel_call.h"
#include "xgrph_hle.h"
#define LENGTH_ENTRY 0x003E9546u

bool xgrph_vertex_shader_length(uint32_t input, uint32_t *out)
{
    if (out == NULL) {
        return false;
    }
    const uint32_t word = kernel_guest_add(input, 2u);
    uint8_t bytes[2];
    if (word == 0u || !kernel_guest_read_bytes(word, bytes, sizeof(bytes))) {
        return false;
    }
    *out = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8);
    return true;
}

static uint32_t length_handler(void *context)
{
    uint32_t input = 0u, length = 0u;
    if (!kernel_frame_arg(context, 0u, &input)) {
        xgrph_hle_fatal(LENGTH_ENTRY, "vertex shader length argument is unreadable");
    }
    if (!xgrph_vertex_shader_length(input, &length)) {
        xgrph_hle_fatal(LENGTH_ENTRY, "vertex shader length word at input %#x is unreadable", input);
    }
    return length;
}

size_t xgrph_shader_query_register(void)
{
    return (size_t)xgrph_hle_register(LENGTH_ENTRY, length_handler);
}
