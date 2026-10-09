/* SPDX-License-Identifier: GPL-3.0-or-later
 * T8c thunk pops. Each hand ABI row in src/host/kernel_thunk.c is checked through the real
 * dispatcher: the thunk must pop the return address plus exactly the measured argument
 * bytes, and the handler must have run (observed through its own state).
 *   142 KeSaveFloatingPointState(area)      ONE stdcall argument, pop 4
 *   139 KeRestoreFloatingPointState(area)   ONE stdcall argument, pop 4
 *   225 NtSetEvent(handle, previous)        TWO stdcall arguments, pop 8
 *   260 RtlAnsiStringToUnicodeString        THREE stdcall arguments, pop 12
 *   308 RtlUnicodeStringToAnsiString        THREE stdcall arguments, pop 12
 */
#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_fpstate.h"
#include "kernel_event_handle.h"
#include "kernel_hle.h"
#include "kernel_object.h"
#include "kernel_rtl_string.h"
#include "guest_structs.h"
#include "kernel_thunk.h"
#include "recomp_abi.h"

#include <stdio.h>

recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;

#define RETURN_ADDRESS 0x00387308u

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
    bool okay = true;

    okay = kernel_fpstate_register() == 2u && okay;
    const uint32_t area = stack + 0x800u;
    okay = call_thunk(142u, stack, &area, 1u) && okay;
    okay = g_eax == STATUS_SUCCESS && kernel_fpstate_depth() == 1u && okay;
    okay = call_thunk(139u, stack, &area, 1u) && okay;
    okay = kernel_fpstate_depth() == 0u && okay;

    /* 225: two arguments popped (12 bytes in all), the event really signaled. */
    kernel_object_reset();
    okay = kernel_event_handle_register() == 2u && okay;
    okay = kernel_object_create_event(1u, 0u, stack + 0x900u) == STATUS_SUCCESS && okay;
    uint32_t handle = 0u;
    okay = kernel_guest_read_u32(stack + 0x900u, &handle) && handle != 0u && okay;
    bool signaled = true;
    okay = kernel_object_event_signaled(handle, &signaled) && !signaled && okay;
    const uint32_t set_arguments[2] = {handle, 0u};
    g_eax = 0xDEADBEEFu;
    okay = call_thunk(225u, stack, set_arguments, 2u) && okay;
    okay = g_eax == STATUS_SUCCESS && okay;
    okay = kernel_object_event_signaled(handle, &signaled) && signaled && okay;
    kernel_object_reset();

    /* 260 then 308 over one buffer pair: three arguments popped each (16 bytes in all) and the
     * conversion really happened ("ab" -> 61 00 62 00 00 00, then back). */
    okay = kernel_rtl_string_register() == 2u && okay;
    const uint32_t ansi_text = stack + 0xA00u;
    const uint32_t wide_text = stack + 0xA40u;
    const uint32_t ansi_descriptor = stack + 0xA80u;
    const uint32_t wide_descriptor = stack + 0xA90u;
    okay = kernel_guest_write_u32(ansi_text, 0x6261u) && okay;
    const guest_object_string ansi = {2u, 3u, ansi_text};
    const guest_object_string wide = {0u, 6u, wide_text};
    okay = kernel_guest_write_bytes(ansi_descriptor, &ansi, sizeof(ansi)) && okay;
    okay = kernel_guest_write_bytes(wide_descriptor, &wide, sizeof(wide)) && okay;
    const uint32_t widen_arguments[3] = {wide_descriptor, ansi_descriptor, 0u};
    g_eax = 0xDEADBEEFu;
    okay = call_thunk(260u, stack, widen_arguments, 3u) && okay;
    uint32_t widened = 0u;
    okay = g_eax == STATUS_SUCCESS && kernel_guest_read_u32(wide_text, &widened) &&
           widened == 0x00620061u && okay;
    uint32_t terminator = 0xFFFFFFFFu;
    okay = kernel_guest_read_u32(wide_text + 4u, &terminator) && (terminator & 0xFFFFu) == 0u &&
           okay;
    const guest_object_string narrow = {0u, 3u, ansi_text + 0x20u};
    okay = kernel_guest_write_bytes(ansi_descriptor, &narrow, sizeof(narrow)) && okay;
    const guest_object_string wide_source = {4u, 6u, wide_text};
    okay = kernel_guest_write_bytes(wide_descriptor, &wide_source, sizeof(wide_source)) && okay;
    const uint32_t narrow_arguments[3] = {ansi_descriptor, wide_descriptor, 0u};
    g_eax = 0xDEADBEEFu;
    okay = call_thunk(308u, stack, narrow_arguments, 3u) && okay;
    uint32_t narrowed = 0u;
    okay = g_eax == STATUS_SUCCESS && kernel_guest_read_u32(ansi_text + 0x20u, &narrowed) &&
           (narrowed & 0xFFFFFFu) == 0x006261u && okay;

    guest_mem_reset();
    puts(okay ? "kernel t8c thunk: passed" : "kernel t8c thunk: FAILED");
    return okay ? 0 : 1;
}
