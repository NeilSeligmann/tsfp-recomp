/* SPDX-License-Identifier: GPL-3.0-or-later
 * T8e thunk pops. Each hand ABI row in src/host/kernel_thunk.c is checked through the real
 * dispatcher: the thunk must pop the return address plus exactly the measured argument
 * bytes, and the handler must have run (observed through its own result).
 *   221 NtReleaseMutant(handle, previous)   TWO stdcall arguments, pop 8
 *   197 NtDuplicateObject(src, &dst, opts)  THREE stdcall arguments, pop 12
 */
#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "kernel_object.h"
#include "kernel_thunk.h"
#include "recomp_abi.h"

#include <stdio.h>

recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;

#define RETURN_ADDRESS 0x00380015u

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
    kernel_object_reset();
    bool okay = kernel_object_register() == 7u;

    /* 192 then 221: three arguments popped, then two, and the release really ran (a mutant
     * nobody owns answers STATUS_MUTANT_NOT_OWNED, counted). */
    const uint32_t create_arguments[3] = {stack + 0x900u, 0u, 0u};
    g_eax = 0xDEADBEEFu;
    okay = call_thunk(192u, stack, create_arguments, 3u) && okay;
    uint32_t handle = 0u;
    okay = g_eax == STATUS_SUCCESS && kernel_guest_read_u32(stack + 0x900u, &handle) &&
           handle != 0u && okay;
    const uint32_t release_arguments[2] = {handle, 0u};
    g_eax = 0xDEADBEEFu;
    okay = call_thunk(221u, stack, release_arguments, 2u) && okay;
    okay = g_eax == STATUS_MUTANT_NOT_OWNED &&
           kernel_object_mutant_unowned_release_count() == 1u && okay;
    kernel_object_reset();

    /* 197: three arguments popped (16 bytes in all), and the duplicate really exists: a new
     * FILE handle that remembers the original it shares. */
    okay = kernel_object_register() == 7u && okay;
    const uint32_t original = kernel_object_create(KERNEL_OBJECT_FILE, 7u);
    okay = original != 0u && okay;
    const uint32_t duplicate_arguments[3] = {original, stack + 0x940u, 2u};
    g_eax = 0xDEADBEEFu;
    okay = call_thunk(197u, stack, duplicate_arguments, 3u) && okay;
    uint32_t duplicate = 0u;
    okay = g_eax == STATUS_SUCCESS && kernel_guest_read_u32(stack + 0x940u, &duplicate) &&
           duplicate != 0u && duplicate != original && okay;
    okay = kernel_object_file_identity(duplicate) == original && okay;
    kernel_object_reset();

    guest_mem_reset();
    puts(okay ? "kernel t8e thunk: passed" : "kernel t8e thunk: FAILED");
    return okay ? 0 : 1;
}
