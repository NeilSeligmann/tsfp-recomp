/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xnet_config.h"
#include "xnet_hle.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "kernel_io.h"

static bool disjoint(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    return a && b && (uint64_t)a + an <= UINT32_MAX &&
        (uint64_t)b + bn <= UINT32_MAX &&
        ((uint64_t)a + an <= b || (uint64_t)b + bn <= a);
}
bool xnet_config_read_sector_kernel(const xnet_config_kernel *source,
    uint32_t handle, uint32_t slot, uint32_t payload, bool *valid)
{
    if (!valid) return false;
    if (handle == UINT32_MAX || slot >= 24u) {
        *valid = false;
        return true;
    }
    if (!source || source->stack_bytes < 36u ||
        !disjoint(source->sector, 512u, source->iosb, 16u) ||
        !disjoint(source->sector, 512u, source->call_stack, source->stack_bytes) ||
        !disjoint(source->sector, 512u, payload, 492u) ||
        !disjoint(source->iosb, 16u, source->call_stack, source->stack_bytes) ||
        !disjoint(source->iosb, 16u, payload, 492u) ||
        !disjoint(source->call_stack, source->stack_bytes, payload, 492u)) return false;
    const kernel_entry *entry = kernel_hle_entry(219u);
    if (!entry || entry->state != KERNEL_ENTRY_IMPLEMENTED || !entry->handler) return false;
    if (!kernel_guest_write_u32(source->iosb + 8u, (slot + 8u) << 9) ||
        !kernel_guest_write_u32(source->iosb + 12u, 0u)) return false;
    const uint32_t arguments[8] = {handle, 0u, 0u, 0u, source->iosb,
                                   source->sector, 512u, source->iosb + 8u};
    kernel_call_frame call;
    uint32_t status;
    if (!kernel_frame_build(&call, source->call_stack, source->stack_bytes, arguments, 8u) ||
        !kernel_io_read_file_stored(&call, &status)) return false;
    *valid = status < 0x80000000u && xnet_hle_decode_config_sector(source->sector, payload);
    return true;
}
