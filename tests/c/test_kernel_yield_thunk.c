/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "guest_mem.h"
#include "host_runtime.h"
#include "kernel_clock.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "kernel_sync.h"
#include "kernel_thread.h"
#include "kernel_thunk.h"
#include "recomp_abi.h"
#include "thunk_trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */

TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
TSFP_RECOMP_TLS uint32_t g_ebx, g_esi, g_edi, g_ebp, g_fs_base, g_seh_ebp;
recomp_func_t recomp_lookup_kernel(uint32_t address);
static unsigned checks;
#define CHECK(value) do { checks++; if (!(value)) { \
    fprintf(stderr, "yield thunk failed at %d: %s\n", __LINE__, #value); abort(); } } while (0)
int main(void)
{
    guest_region_request request = {.bytes = 8192u, .alignment = 4096u,
        .protect = PAGE_READWRITE, .state = MEM_COMMIT};
    nt_status status;
    const uint32_t allocation = guest_region_alloc(&request, &status);
    CHECK(allocation != 0u);
    const uint32_t stack = allocation + 4092u;
    CHECK(kernel_guest_write_u32(stack, 0x37FD64u));
    CHECK(mprotect((void *)(uintptr_t)(allocation + 4096u), 4096u, PROT_NONE) == 0);
    kernel_hle_init();
    CHECK(kernel_thread_register() == 10u);
    const uint64_t clock = kernel_clock_peek();
    thunk_trace_reset();
    g_eax = 1u; g_ecx = 2u; g_edx = 3u; g_ebx = 4u;
    g_esi = 5u; g_edi = 6u; g_ebp = 7u;
    g_fs_base = 8u; g_seh_ebp = 9u; g_esp = stack;
    recomp_func_t function = recomp_lookup_kernel(KERNEL_THUNK_VA(238u));
    CHECK(function != NULL);
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        function();
        CHECK(g_eax == STATUS_NO_YIELD_PERFORMED);
        CHECK(g_esp == stack + 4u);
        CHECK(g_ecx == 2u && g_edx == 3u && g_ebx == 4u);
        CHECK(g_esi == 5u && g_edi == 6u && g_ebp == 7u);
        CHECK(g_fs_base == 8u && g_seh_ebp == 9u);
    } else { CHECK(false); }
    host_run_disarm();
    CHECK(kernel_sync_current_irql() == KERNEL_IRQL_PASSIVE);
    CHECK(kernel_clock_peek() == clock);
    uint32_t caller;
    CHECK(kernel_guest_read_u32(stack, &caller) && caller == 0x37FD64u);
    size_t count;
    const thunk_trace_entry *trace = thunk_trace_entries(&count);
    CHECK(count == 1u && trace[0].ordinal == 238u && trace[0].implemented);
    CHECK(trace[0].return_address == caller && trace[0].result_known);
    CHECK(trace[0].result == STATUS_NO_YIELD_PERFORMED);
    CHECK(mprotect((void *)(uintptr_t)(allocation + 4096u), 4096u,
                   PROT_READ | PROT_WRITE) == 0);
    CHECK(guest_region_free(allocation));
    kernel_thread_reset();
    printf("kernel yield thunk: %u checks passed\n", checks);
    return 0;
}
