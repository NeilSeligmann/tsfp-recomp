/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See d3d8_immediate.h. Every function carries the address of the original it ports.
 */

#include "d3d8_immediate.h"

#include "d3d8_gpu.h"
#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "d3d8_pushbuffer.h"
#include "d3d8_resource.h"
#include "kernel_call.h"

#define PUSH_BEGIN_END 0x000417FCu
#define PUSH_VERTEX_DATA_2F 0x00081880u
#define FLAG_IMMEDIATE 0x0800u
#define FLAG_FENCE_OWED 0x1000u

void d3d8_begin(uint32_t primitive)
{
    d3d8_run_dirty_cascade();
    const uint32_t cursor = d3d8_pushbuffer_begin();
    d3d8_guest_store32(cursor, PUSH_BEGIN_END);
    d3d8_guest_store32(cursor + 4u, primitive);
    d3d8_pushbuffer_end(cursor + 8u);
    d3d8_device_store32(D3D8_DEV_FLAGS, d3d8_device_load32(D3D8_DEV_FLAGS) | FLAG_IMMEDIATE);
}

void d3d8_set_vertex_data_2f(uint32_t slot, uint32_t x, uint32_t y)
{
    const uint32_t cursor = d3d8_pushbuffer_begin();
    d3d8_guest_store32(cursor, PUSH_VERTEX_DATA_2F + slot * 8u);
    d3d8_guest_store32(cursor + 4u, x);
    d3d8_guest_store32(cursor + 8u, y);
    d3d8_pushbuffer_end(cursor + 12u);
}

void d3d8_end(void)
{
    const uint32_t cursor = d3d8_pushbuffer_begin();
    d3d8_guest_store32(cursor, PUSH_BEGIN_END);
    d3d8_guest_store32(cursor + 4u, 0u);
    d3d8_pushbuffer_end(cursor + 8u);
    const uint32_t flags = d3d8_device_load32(D3D8_DEV_FLAGS);
    d3d8_device_store32(D3D8_DEV_FLAGS, flags & 0xFFFFE7FFu);
    if ((flags & FLAG_FENCE_OWED) != 0u) {
        (void)d3d8_gpu_fence_insert(1u);
    }
}

static uint32_t argument(const void *context, unsigned index, uint32_t address)
{
    uint32_t value = 0u;
    if (!kernel_frame_arg((const kernel_call_frame *)context, index, &value)) {
        d3d8_hle_fatal(address, "argument %u of the call cannot be read", index);
    }
    return value;
}

static uint32_t handler_begin(void *context)
{
    d3d8_begin(argument(context, 0u, 0x003D5340u));
    return 0u;
}

static uint32_t handler_vertex_data_2f(void *context)
{
    d3d8_set_vertex_data_2f(argument(context, 0u, 0x003D52B0u), argument(context, 1u, 0x003D52B0u),
                            argument(context, 2u, 0x003D52B0u));
    return 0u;
}

static uint32_t handler_end(void *context)
{
    (void)context;
    d3d8_end();
    return 0u;
}

size_t d3d8_immediate_register(void)
{
    static const struct {
        uint32_t address;
        d3d8_fn handler;
    } handlers[] = {
        {0x003D5340u, handler_begin},
        {0x003D52B0u, handler_vertex_data_2f},
        {0x003D5380u, handler_end},
    };
    size_t registered = 0u;
    for (size_t index = 0u; index < sizeof(handlers) / sizeof(handlers[0]); index++) {
        if (d3d8_hle_register(handlers[index].address, handlers[index].handler)) {
            registered++;
        }
    }
    return registered;
}
