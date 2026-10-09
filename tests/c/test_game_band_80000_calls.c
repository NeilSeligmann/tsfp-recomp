/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native call-frame and snapshot checks; live proof uses original getter bytes.
 */
#include "game_replace.h"
#include <stdio.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x00800000];
static unsigned checks, failures, calls;
#define CHECK(expr) do { checks++; if (!(expr)) { failures++; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); } } while (0)

static void state_group_get(void)
{
    CHECK(guest_read32(g_esp) == 0x0008F475u);
    g_eax = guest_read32(0x0055FAF8u);
    g_esp += 4u;
    calls++;
}
static void blend_mode_get(void)
{
    CHECK(guest_read32(g_esp) == 0x0008F47Fu);
    g_eax = guest_read32(0x0055FAFCu);
    g_esp += 4u;
    calls++;
}
static void blend_alpha_get(void)
{
    CHECK(guest_read32(g_esp) == 0x0008F489u);
    g_eax = guest_read32(0x0055FB04u);
    g_esp += 4u;
    calls++;
}

game_guest_function recomp_lookup(uint32_t va)
{
    switch (va) {
    case 0x00019640u: return state_group_get;
    case 0x00019D50u: return blend_mode_get;
    case 0x00019D60u: return blend_alpha_get;
    default: return NULL;
    }
}
extern void sub_0008F470(void);
int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    for (unsigned i = 0u; i < 4u; i++) {
        const uint32_t value = i == 0u ? 0u : i == 1u ? 0xFFFFFFFFu :
                               i == 2u ? 0x80000000u : 0x10203040u;
        g_esp = 0x10000u;
        g_ecx = 0x12345678u;
        g_edx = 0x87654321u;
        guest_write32(0x0055FAF8u, value);
        guest_write32(0x0055FAFCu, value ^ 0xABCDEF01u);
        guest_write32(0x0055FB04u, value ^ 0x12345678u);
        sub_0008F470();
        CHECK(g_esp == 0x10004u && g_eax == (value ^ 0x12345678u));
        CHECK(g_ecx == 0x12345678u && g_edx == 0x87654321u);
        CHECK(guest_read32(0x004D9708u) == value);
        CHECK(guest_read32(0x004D970Cu) == (value ^ 0xABCDEF01u));
        CHECK(guest_read32(0x004D9710u) == (value ^ 0x12345678u));
    }
    CHECK(calls == 12u);
    printf("%u checks, %u failures\n", checks, failures);
    return failures != 0u;
}
