/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See kernel_identity.h for the policy and its evidence. This file only lays the
 * bytes out: both structs are written little-endian, field by field, so the layout
 * is in the code rather than in a host struct whose padding could drift.
 */

#include "kernel_identity.h"

#include "kernel_call.h"

bool kernel_identity_publish(uint32_t hardware_info_va, uint32_t krnl_version_va)
{
    /* XboxHardwareInfo: ULONG Flags, UCHAR GpuRevision, UCHAR McpRevision,
     * UCHAR Unknown[2]. The unknown tail bytes stay zero. */
    uint8_t hardware[KERNEL_IDENTITY_HARDWARE_INFO_BYTES] = {
        (uint8_t)(KERNEL_IDENTITY_HARDWARE_FLAGS & 0xFFu),
        (uint8_t)((KERNEL_IDENTITY_HARDWARE_FLAGS >> 8) & 0xFFu),
        (uint8_t)((KERNEL_IDENTITY_HARDWARE_FLAGS >> 16) & 0xFFu),
        (uint8_t)((KERNEL_IDENTITY_HARDWARE_FLAGS >> 24) & 0xFFu),
        (uint8_t)KERNEL_IDENTITY_GPU_REVISION,
        (uint8_t)KERNEL_IDENTITY_MCP_REVISION,
        0u,
        0u,
    };
    /* XboxKrnlVersion: USHORT Major, Minor, Build, Qfe. */
    uint8_t version[KERNEL_IDENTITY_KRNL_VERSION_BYTES] = {
        (uint8_t)(KERNEL_IDENTITY_KRNL_MAJOR & 0xFFu),
        (uint8_t)((KERNEL_IDENTITY_KRNL_MAJOR >> 8) & 0xFFu),
        (uint8_t)(KERNEL_IDENTITY_KRNL_MINOR & 0xFFu),
        (uint8_t)((KERNEL_IDENTITY_KRNL_MINOR >> 8) & 0xFFu),
        (uint8_t)(KERNEL_IDENTITY_KRNL_BUILD & 0xFFu),
        (uint8_t)((KERNEL_IDENTITY_KRNL_BUILD >> 8) & 0xFFu),
        (uint8_t)(KERNEL_IDENTITY_KRNL_QFE & 0xFFu),
        (uint8_t)((KERNEL_IDENTITY_KRNL_QFE >> 8) & 0xFFu),
    };
    if (!kernel_guest_write_bytes(hardware_info_va, hardware, sizeof(hardware))) {
        return false;
    }
    return kernel_guest_write_bytes(krnl_version_va, version, sizeof(version));
}
