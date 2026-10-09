/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_target_query.h"
#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "d3d8_reference.h"
#include "kernel_call.h"
static uint32_t query(uint32_t entry, uint32_t offset)
{
    uint32_t device = 0u, header = 0u;
    if (!kernel_guest_read_u32(0x003E3F58u, &device) || device != D3D8_DEVICE_BASE)
        d3d8_hle_fatal(entry, "target query requires the recovered live fixed device");
    if (!kernel_guest_read_u32(device + offset, &header))
        d3d8_hle_fatal(entry, "target binding field is unreadable");
    if (header != 0u) (void)d3d8_reference_add_ref(header);
    return header;
}
uint32_t d3d8_get_render_target2(void) { return query(0x003D3E00u, 0x1A04u); }
uint32_t d3d8_get_depth_stencil_surface2(void) { return query(0x003D3E20u, 0x1A08u); }
static uint32_t target(void *context) { (void)context; return d3d8_get_render_target2(); }
static uint32_t depth(void *context) { (void)context; return d3d8_get_depth_stencil_surface2(); }
size_t d3d8_target_query_register(void)
{
    size_t count = 0u;
    if (d3d8_hle_register(0x003D3E00u, target)) count++;
    if (d3d8_hle_register(0x003D3E20u, depth)) count++;
    return count;
}
