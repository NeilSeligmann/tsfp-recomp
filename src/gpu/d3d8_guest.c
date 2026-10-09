/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See d3d8_guest.h for why D3D8's state is guest memory and which words matter.
 */

#include "d3d8_guest.h"

#include <string.h>

#include "d3d8_hle.h"
#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "nt_status.h"

uint32_t d3d8_guest_load32(uint32_t address)
{
    uint32_t value = 0u;
    if (!kernel_guest_read_u32(address, &value)) {
        d3d8_hle_fatal(address, "guest dword at 0x%08x is not readable", (unsigned)address);
    }
    return value;
}

void d3d8_guest_store32(uint32_t address, uint32_t value)
{
    if (!kernel_guest_write_u32(address, value)) {
        d3d8_hle_fatal(address, "guest dword at 0x%08x is not writable", (unsigned)address);
    }
}

uint8_t d3d8_guest_load8(uint32_t address)
{
    uint8_t value = 0u;
    if (!kernel_guest_read_u8(address, &value)) {
        d3d8_hle_fatal(address, "guest byte at 0x%08x is not readable", (unsigned)address);
    }
    return value;
}

void d3d8_guest_store8(uint32_t address, uint8_t value)
{
    if (!kernel_guest_write_u8(address, value)) {
        d3d8_hle_fatal(address, "guest byte at 0x%08x is not writable", (unsigned)address);
    }
}

/* Room for a frame of eight arguments and the return slot, with margin. */
#define KERNEL_FRAME_BYTES 0x100u

static kernel_guest_ptr kernel_frame_scratch;
static d3d8_kernel_tap kernel_tap;

void d3d8_guest_set_kernel_tap(d3d8_kernel_tap tap)
{
    kernel_tap = tap;
}

void d3d8_guest_reset(void)
{
    kernel_frame_scratch = 0u;
}

static void ensure_scratch(void)
{
    if (kernel_frame_scratch == 0u) {
        guest_region_request request;
        memset(&request, 0, sizeof(request));
        request.bytes = GUEST_PAGE_SIZE;
        request.protect = PAGE_READWRITE;
        request.state = MEM_COMMIT;
        nt_status status = STATUS_SUCCESS;
        kernel_frame_scratch = guest_region_alloc(&request, &status);
        if (kernel_frame_scratch == 0u) {
            d3d8_hle_fatal(0u, "no guest memory for a kernel call frame (status 0x%08x)",
                           (unsigned)status);
        }
    }
}

/* The frame uses the first KERNEL_FRAME_BYTES of the page, this the rest. */
#define SCRATCH_OFFSET 0x200u

uint32_t d3d8_guest_scratch(void)
{
    ensure_scratch();
    return kernel_frame_scratch + SCRATCH_OFFSET;
}

uint32_t d3d8_kernel_call(unsigned ordinal, const uint32_t *args, unsigned argc)
{
    if (argc > 8u) {
        d3d8_hle_fatal(0u, "kernel call with %u arguments exceeds the 8 this seam carries",
                       argc);
    }
    ensure_scratch();

    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    if (!kernel_frame_build(&frame, kernel_frame_scratch, KERNEL_FRAME_BYTES, args, argc)) {
        d3d8_hle_fatal(0u, "could not lay out a %u-argument kernel call frame", argc);
    }
    /* Bounded to exactly the arguments laid out, so a handler that reads one more than
     * this call supplies fails instead of reading the scratch page's leftovers. */
    frame.stack_limit = kernel_frame_scratch + (argc + 1u) * 4u;
    const uint32_t result = kernel_hle_call(ordinal, &frame);
    if (kernel_tap != NULL) {
        kernel_tap(ordinal, args, argc, result);
    }
    return result;
}

uint32_t d3d8_device_load32(uint32_t offset)
{
    return d3d8_guest_load32(D3D8_DEVICE_BASE + offset);
}

void d3d8_device_store32(uint32_t offset, uint32_t value)
{
    d3d8_guest_store32(D3D8_DEVICE_BASE + offset, value);
}
