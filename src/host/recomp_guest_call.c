/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "recomp_guest_call.h"
#include "kernel_call.h"
#include "recomp_abi.h"
bool recomp_guest_call_stdcall(uint32_t address, const uint32_t *arguments, unsigned count)
{
    if (count > GUEST_CALL_MAX_ARGS || (count != 0u && arguments == NULL)) return false;
    const recomp_func_t function = recomp_lookup(address);
    if (function == NULL) return false;
    const uint32_t frame_bytes = 4u * (count + 1u), top = g_esp;
    if (top < frame_bytes) return false;
    const uint32_t base = top - frame_bytes;
    uint32_t frame[GUEST_CALL_MAX_ARGS + 1u] = {0u};
    for (unsigned i = 0u; i < count; i++) frame[1u + i] = arguments[i];
    /* The guarded write is all or nothing (the straddling case is pinned in the routes test), so a
     * frame that is not writable stack leaves every byte alone and is refused here. */
    if (!kernel_guest_write_bytes(base, frame, frame_bytes)) return false;
    const uint32_t saved[8] = {g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_ebp, top};
    g_esp = base;
    function();
    const bool balanced = g_esp == top;
    g_eax = saved[0];
    g_ecx = saved[1];
    g_edx = saved[2];
    g_ebx = saved[3];
    g_esi = saved[4];
    g_edi = saved[5];
    g_ebp = saved[6];
    g_esp = saved[7];
    return balanced;
}
