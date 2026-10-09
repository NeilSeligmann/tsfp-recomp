/* SPDX-License-Identifier: GPL-3.0-or-later
 * Plain-C guest GPU reader, shared by surface modeling and Vulkan consumers. */
#include "d3d8_gpu_memory.h"
#include "guest_mem.h"
#include "kernel_call.h"

bool d3d8_gpu_read_guest(void *context, uint32_t address, void *out, size_t bytes)
{
    (void)context;
    /* T441. An address in the recorded stream is what the ported emitters wrote for the hardware: the
     * buffer's Data word, the 28-bit PHYSICAL address (d3d8_resource.c d3d8_create_buffer), which in this
     * host is a synthetic per-region number, not the guest virtual address the title wrote the vertices
     * through. Read as a virtual address it lands on unrelated (zero) memory: MEASURED on the disc-less
     * steady state, every captured vertex byte was 0 before this translation. A span that lies wholly in
     * one region's physical range is read through it. An address no region holds as physical is read
     * as a guest virtual address, as before (the synthetic streams of the unit tests). */
    kernel_guest_ptr virtual_address = 0u;
    if (bytes != 0u && guest_virtual_from_physical(address, &virtual_address)) {
        kernel_guest_ptr last = 0u;
        if (!guest_virtual_from_physical(address + (uint32_t)(bytes - 1u), &last) ||
            last - virtual_address != (uint32_t)(bytes - 1u)) {
            return false;
        }
        return kernel_guest_read_bytes(virtual_address, out, bytes);
    }
    return kernel_guest_read_bytes(address, out, bytes);
}

bool d3d8_gpu_read_virtual(void *context, uint32_t address, void *out, size_t bytes)
{
    (void)context;
    return kernel_guest_read_bytes(address, out, bytes);
}

