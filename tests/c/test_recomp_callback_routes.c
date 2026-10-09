/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "host_runtime.h"
#include "recomp_abi.h"
#include "recomp_callback.h"
#include "recomp_cooperative.h"
#include "d3d8_first_vblank.h"
#include "kernel_clock.h"
#include "kernel_sync.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */

static unsigned checks;
#define CHECK(value) do { checks++; if (!(value)) { \
    fprintf(stderr, "callback route failed at %d\n", __LINE__); abort(); } } while (0)
static void map_page(uint32_t address)
{
    CHECK(mmap((void *)(uintptr_t)address, 4096u, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) ==
          (void *)(uintptr_t)address);
}
/* T370: the guest's own copy of the level, as the host's publisher writes it, and the peak seen. */
static uint32_t published_peak;
static void publish_pcr(uint32_t level)
{
    if (g_fs_base != 0u) {
        *(uint8_t *)(uintptr_t)(g_fs_base + 0x24u) = (uint8_t)level;
    }
    if (level > published_peak) {
        published_peak = level;
    }
}
static bool run_armed(const uint32_t payload[3])
{
    bool ok = false;
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        ok = recomp_callback_run(0x22020u, 0x21000000u, 0x21001000u, payload);
    } else {
        CHECK(false);
    }
    host_run_disarm();
    return ok;
}
static void check_irql_restored(void)
{
    CHECK(kernel_sync_current_irql() == KERNEL_IRQL_PASSIVE);
    CHECK(*(uint8_t *)(uintptr_t)(g_fs_base + 0x24u) == KERNEL_IRQL_PASSIVE);
}
static void check_registers(void)
{
    CHECK(g_eax == 11u && g_ecx == 12u && g_edx == 13u);
    CHECK(g_ebx == 14u && g_esi == 15u && g_edi == 16u && g_ebp == 17u);
    CHECK(g_esp == 0x22000000u && g_fs_base == 0x21002000u);
    CHECK(*(uint32_t *)(uintptr_t)g_fs_base == 0x12345678u);
    CHECK(host_run_scope_depth() == 0u);
}
static void startup_provider(uint32_t callee, void *userdata)
{
    (void)userdata;
    d3d8_first_vblank_poll(callee, g_fs_base, g_esp, 0u);
}
static void check_startup_callback(volatile uint32_t *counter)
{
    if (!recomp_cooperative_ready()) {
        puts("compiled cooperative startup route unavailable in this lift");
        return;
    }
    static bool mapped; /* the startup route runs twice since T370 */
    if (!mapped) {
        map_page(0x003E3000u); map_page(0x003E4000u);
        map_page(0x003E5000u); map_page(0x003E6000u);
        map_page(g_esp);
        mapped = true;
    }
    *(uint32_t *)(uintptr_t)g_esp = 0x3D455u;
    *(uint32_t *)(uintptr_t)0x3E3F58u = 0x3E3F60u;
    *(uint32_t *)(uintptr_t)(0x3E3F60u + 0x1DB8u) = 0x22020u;
    *(uint32_t *)(uintptr_t)(0x3E3F60u + 0x1DE8u) = 1u;
    *(uint32_t *)(uintptr_t)(0x3E3F60u + 0x1DDCu) = 0x02480104u;
    const uint32_t previous = *counter;
    const uint64_t clock = kernel_clock_peek();
    d3d8_first_vblank_configure(true, recomp_callback_run, 0x21000000u, 0x21001000u);
    d3d8_first_vblank_bind_owner(1u, g_fs_base, 0x3801D9u, 0x37FE1Du);
    CHECK(recomp_cooperative_configure(startup_provider, NULL));
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        recomp_call_safepoint(0x1538C0u);
        CHECK(*counter == previous + 1u);
        CHECK(*(uint32_t *)(uintptr_t)(0x3E3F60u + 0x1DE8u) == 2u);
        check_registers();
        d3d8_first_vblank_snapshot state;
        d3d8_first_vblank_get_snapshot(&state);
        CHECK(state.attempted && state.delivered);
        CHECK(kernel_clock_peek() == clock);
    } else { CHECK(false); }
    host_run_disarm();
    CHECK(recomp_cooperative_configure(NULL, NULL));
    d3d8_first_vblank_configure(false, NULL, 0u, 0u);
    d3d8_first_vblank_reset();
}
int main(void)
{
    map_page(0x00563000u);
    map_page(0x21000000u);
    map_page(0x21002000u);
    CHECK(recomp_lookup(0x22020u) != NULL);
    volatile uint32_t *counter = (void *)(uintptr_t)0x563918u;
    uint32_t *neighbors = (void *)(uintptr_t)0x563914u;
    const uint32_t payload[3] = {1u, 0u, 0u};
    g_eax = 11u; g_ecx = 12u; g_edx = 13u; g_ebx = 14u;
    g_esi = 15u; g_edi = 16u; g_ebp = 17u;
    g_esp = 0x22000000u; g_fs_base = 0x21002000u;
    *(uint32_t *)(uintptr_t)g_fs_base = 0x12345678u;
    neighbors[0] = 0xAABBCCDDu; neighbors[2] = 0x11223344u;
    *counter = UINT32_MAX;
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        CHECK(!recomp_callback_run(0x22030u, 0x21000000u, 0x21001000u, payload));
        CHECK(*counter == UINT32_MAX);
        CHECK(recomp_callback_run(0x22020u, 0x21000000u, 0x21001000u, payload));
        CHECK(*counter == 0u); check_registers();
        CHECK(recomp_callback_run(0x22020u, 0x21000000u, 0x21001000u, payload));
        CHECK(*counter == 1u); check_registers();
        CHECK(neighbors[0] == 0xAABBCCDDu && neighbors[2] == 0x11223344u);
        CHECK(mprotect((void *)(uintptr_t)0x563000u, 4096u, PROT_READ) == 0);
        (void)recomp_callback_run(0x22020u, 0x21000000u, 0x21001000u, payload);
        CHECK(false);
    } else {
        CHECK(host_run_result()->reason == HOST_STOP_FAULT);
        CHECK(host_run_result()->signal_number == SIGSEGV);
        CHECK(host_run_result()->fault_address == 0x563918u);
        check_registers(); CHECK(*counter == 1u);
    }
    host_run_disarm();
    CHECK(mprotect((void *)(uintptr_t)0x563000u, 4096u, PROT_READ | PROT_WRITE) == 0);
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        CHECK(recomp_callback_run(0x22020u, 0x21000000u, 0x21001000u, payload));
        CHECK(*counter == 2u); check_registers();
    } else { CHECK(false); }
    host_run_disarm();
    check_startup_callback(counter);

    /* T370: at DISPATCH_LEVEL the real leaf's result is unchanged (counter +1, registers, count)
     * and the level is restored after a normal return, after the leaf faults, and after the
     * startup route. Default off the level never leaves PASSIVE. */
    kernel_sync_set_irql_publisher(publish_pcr);
    kernel_sync_reset();
    published_peak = 0u;
    uint32_t base = *counter;
    CHECK(run_armed(payload));
    CHECK(*counter == base + 1u); check_registers(); check_irql_restored();
    CHECK(published_peak == KERNEL_IRQL_PASSIVE);
    recomp_callback_set_dispatch_level(true);
    CHECK(run_armed(payload));
    CHECK(*counter == base + 2u); check_registers(); check_irql_restored();
    CHECK(published_peak == KERNEL_IRQL_DISPATCH);
    CHECK(mprotect((void *)(uintptr_t)0x563000u, 4096u, PROT_READ) == 0);
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        (void)recomp_callback_run(0x22020u, 0x21000000u, 0x21001000u, payload);
        CHECK(false);
    } else {
        CHECK(host_run_result()->reason == HOST_STOP_FAULT);
        CHECK(host_run_result()->fault_address == 0x563918u);
        check_registers(); check_irql_restored();
    }
    host_run_disarm();
    CHECK(mprotect((void *)(uintptr_t)0x563000u, 4096u, PROT_READ | PROT_WRITE) == 0);
    published_peak = 0u;
    const uint32_t before_startup = *counter;
    check_startup_callback(counter);
    if (recomp_cooperative_ready()) {
        CHECK(*counter == before_startup + 1u);
        CHECK(published_peak == KERNEL_IRQL_DISPATCH);
    }
    check_irql_restored();
    recomp_callback_set_dispatch_level(false);
    kernel_sync_set_irql_publisher(NULL);
    printf("compiled callback routes: %u checks passed\n", checks);
    return 0;
}
