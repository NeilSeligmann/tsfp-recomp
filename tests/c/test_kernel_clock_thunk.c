/* SPDX-License-Identifier: GPL-3.0-or-later
 * Real kernel dispatch regression: 64-bit returns and zero argument cleanup.
 * 127's measured two pushes are saved registers, not arguments (0x004414FA).
 * 151 pops its one argument and advances the shared clock.
 */
#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_clock.h"
#include "kernel_hle.h"
#include "kernel_thunk.h"
#include "recomp_abi.h"

#include <stdio.h>

recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;

static uint32_t narrow_result(void *context)
{
    (void)context;
    return 7u;
}

static bool invoke(unsigned ordinal, uint32_t stack)
{
    recomp_func_t fn = recomp_lookup_kernel(KERNEL_THUNK_VA(ordinal));
    if (fn == NULL || !kernel_guest_write_u32(stack, 0x004414EEu)) {
        return false;
    }
    g_esp = stack;
    fn();
    return g_esp == stack + 4u;
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
    kernel_clock_reset();
    bool okay = kernel_clock_register() == 3u;
    okay = kernel_clock_bind_tick_count(stack + 16u) && okay;
    /* Cross 2^32 so dropping the high half cannot pass. */
    for (unsigned i = 0u; i < 360u; i++) {
        okay = kernel_clock_frame(60u) && okay;
    }
    const uint64_t expected = kernel_clock_peek();
    uint32_t published = 0u;
    okay = kernel_guest_read_u32(stack + 16u, &published) && okay;
    okay = published == 6000u && okay;
    g_edx = 0xDEADBEEFu;
    okay = invoke(126u, stack) && okay;
    okay = (((uint64_t)g_edx << 32) | g_eax) == expected && okay;
    /* Frequency must explicitly zero EDX, even after a high counter result. */
    okay = invoke(127u, stack) && okay;
    okay = g_edx == 0u && g_eax == KERNEL_CLOCK_FREQUENCY_HZ && okay;
    /* 151 KeStallExecutionProcessor(us): ONE stdcall argument, so the thunk pops the
     * return address plus 4 bytes (the hand row), and the clock advances by exactly the
     * stalled time. A thunk row of 0 would leave esp 4 low. */
    kernel_clock_reset();
    okay = kernel_guest_write_u32(stack + 4u, 10u) && okay;
    okay = recomp_lookup_kernel(KERNEL_THUNK_VA(151u)) != NULL && okay;
    g_esp = stack;
    okay = kernel_guest_write_u32(stack, 0x0043A396u) && okay;
    recomp_lookup_kernel(KERNEL_THUNK_VA(151u))();
    okay = g_esp == stack + 8u && okay;
    okay = kernel_clock_peek() == 7333u && okay;
    okay = kernel_hle_register(103u, narrow_result) && okay;
    g_edx = 0xCAFEBABEu;
    okay = invoke(103u, stack) && okay;
    okay = g_eax == 7u && g_edx == 0xCAFEBABEu && okay;
    okay = kernel_clock_bind_tick_count(0u) && okay;
    guest_mem_reset();
    puts(okay ? "kernel clock thunk: passed" : "kernel clock thunk: FAILED");
    return okay ? 0 : 1;
}
