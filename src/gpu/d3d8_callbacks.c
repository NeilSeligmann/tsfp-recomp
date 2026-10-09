/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_callbacks.h"
#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "kernel_call.h"
#define ENTRY 0x003D3530u
static d3d8_vblank_registration_observer registration_observer;
void d3d8_callbacks_set_vblank_observer(d3d8_vblank_registration_observer observer)
{
    registration_observer = observer;
}
uint32_t d3d8_set_vertical_blank_callback(uint32_t callback)
{
    if (kernel_guest_at(0x003E3F58u, 4u) == NULL)
        d3d8_hle_fatal(ENTRY, "device pointer is not mapped");
    const uint32_t device = d3d8_guest_load32(0x003E3F58u);
    const uint64_t slot = (uint64_t)device + 0x1DB8u;
    if (device == 0u || slot + 4u > UINT64_C(0x100000000) ||
        kernel_guest_at((uint32_t)slot, 4u) == NULL)
        d3d8_hle_fatal(ENTRY, "vertical blank callback slot is not mapped");
    if (registration_observer != NULL) registration_observer(callback);
    d3d8_guest_store32((uint32_t)slot, callback);
    return callback;
}
static uint32_t handler(void *context)
{
    uint32_t callback;
    if (!kernel_frame_arg((const kernel_call_frame *)context, 0u, &callback))
        d3d8_hle_fatal(ENTRY, "vertical blank callback argument is unreadable");
    return d3d8_set_vertical_blank_callback(callback);
}
size_t d3d8_callbacks_register(void)
{
    return (size_t)d3d8_hle_register(ENTRY, handler);
}
