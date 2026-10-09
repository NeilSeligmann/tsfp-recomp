/* SPDX-License-Identifier: GPL-3.0-or-later
 * T8d thunk pops. Each ABI row in src/host/kernel_thunk.c is checked through the real
 * dispatcher: the thunk must pop the return address plus exactly the measured argument
 * bytes, and the handler must have run (observed through its own effect).
 *   210 NtQueryFullAttributesFile(oa, info)   TWO stdcall arguments, pop 8
 *   176 MmLockUnlockPhysicalPage(physical, 1) TWO stdcall arguments, pop 8
 */
#include "guest_mem.h"
#include "guest_structs.h"
#include "kernel_call.h"
#include "kernel_file.h"
#include "kernel_hle.h"
#include "kernel_io.h"
#include "kernel_memory.h"
#include "kernel_thunk.h"
#include "recomp_abi.h"

#include <stdio.h>

recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;

#define RETURN_ADDRESS 0x00381C07u

/* Push `argument_count` dwords, call the thunk, and report whether esp ended exactly
 * `4 + 4 * argument_count` above where the call started (the pop of a `ret N`). */
static bool call_thunk(unsigned ordinal, uint32_t stack, const uint32_t *arguments,
                       unsigned argument_count)
{
    recomp_func_t fn = recomp_lookup_kernel(KERNEL_THUNK_VA(ordinal));
    if (fn == NULL || !kernel_guest_write_u32(stack, RETURN_ADDRESS)) {
        return false;
    }
    for (unsigned i = 0u; i < argument_count; i++) {
        if (!kernel_guest_write_u32(stack + 4u + 4u * i, arguments[i])) {
            return false;
        }
    }
    g_esp = stack;
    fn();
    return g_esp == stack + 4u + 4u * argument_count;
}

int main(void)
{
    const guest_region_request request = {
        .bytes = 4096u, .alignment = 4096u,
        .protect = PAGE_READWRITE, .state = MEM_COMMIT,
    };
    nt_status status = STATUS_SUCCESS;
    const uint32_t stack = guest_region_alloc(&request, &status);
    if (stack == 0u) {
        return 1;
    }
    kernel_hle_init();
    kernel_file_reset();
    bool okay = true;

    /* 210: a declared openable name answers an empty file through the real thunk, with the
     * OBJECT_ATTRIBUTES laid out as the XAPI site builds it (root, name, attributes 0x40) and
     * the 0x38-byte result written (attributes 0x20 at +0x30). Two arguments are popped. */
    okay = kernel_io_register() == 10u && okay;
    okay = kernel_file_add_openable("\\Device\\Harddisk0\\partition1\\a.bin") && okay;
    static const char name[] = "\\Device\\Harddisk0\\partition1\\a.bin";
    const uint32_t text = stack + 0x400u;
    const uint32_t descriptor = stack + 0x380u;
    const uint32_t object_attributes = stack + 0x390u;
    const uint32_t info = stack + 0x500u;
    okay = kernel_guest_write_bytes(text, name, sizeof(name) - 1u) && okay;
    const guest_object_string ansi = {(uint16_t)(sizeof(name) - 1u), (uint16_t)sizeof(name), text};
    okay = kernel_guest_write_bytes(descriptor, &ansi, sizeof(ansi)) && okay;
    okay = kernel_guest_write_u32(object_attributes, 0u) && okay;
    okay = kernel_guest_write_u32(object_attributes + 4u, descriptor) && okay;
    okay = kernel_guest_write_u32(object_attributes + 8u, 0x40u) && okay;
    okay = kernel_guest_write_u32(info + 0x30u, 0xFFFFFFFFu) && okay;
    const uint32_t arguments[2] = {object_attributes, info};
    g_eax = 0xDEADBEEFu;
    okay = call_thunk(210u, stack, arguments, 2u) && okay;
    uint32_t attributes = 0u;
    okay = g_eax == STATUS_SUCCESS && kernel_guest_read_u32(info + 0x30u, &attributes) &&
           attributes == 0x20u && kernel_file_attribute_query_count() == 1u && okay;

    /* 176: lock a contiguous page through 175's door, then release it by PHYSICAL address through
     * the real thunk. Two arguments popped (12 bytes in all) and the page lock really went. */
    okay = kernel_memory_register() == 16u && okay;
    const guest_region_request contiguous = {
        .bytes = 4096u, .alignment = 4096u,
        .protect = PAGE_READWRITE, .state = MEM_COMMIT, .contiguous = true,
    };
    const uint32_t buffer = guest_region_alloc(&contiguous, &status);
    okay = buffer != 0u && okay;
    const uint32_t lock_arguments[3] = {buffer, 16u, 0u};
    okay = call_thunk(175u, stack, lock_arguments, 3u) && okay;
    okay = kernel_memory_page_lock_count(buffer) == 1u && okay;
    const uint32_t unlock_arguments[2] = {guest_physical_address(buffer + 8u), 1u};
    okay = unlock_arguments[0] != 0u && okay;
    g_eax = 0xDEADBEEFu;
    okay = call_thunk(176u, stack, unlock_arguments, 2u) && okay;
    okay = kernel_memory_page_lock_count(buffer) == 0u && kernel_memory_physical_refused_count() == 0u && okay;

    kernel_file_reset();
    guest_mem_reset();
    puts(okay ? "kernel t8d thunk: passed" : "kernel t8d thunk: FAILED");
    return okay ? 0 : 1;
}
