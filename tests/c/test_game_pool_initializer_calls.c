/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1476 AE910 caller controls. The leaf below is an explicit native control
 * transcribed from original156DB0 instructions, not differential evidence.
 * Production live proof must use the original lifted unregistered leaf.
 */
#include "game_replace.h"
#include <stdio.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base;
__thread uint32_t g_ebx, g_ebp, g_esi, g_edi;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x00800000];
static unsigned checks, failures, calls;
#define CHECK(expr) do { checks++; if (!(expr)) { failures++; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); } } while (0)
#define ROOT_STACK 0x00010000u
#define POOL 0x007329F0u
#define STORAGE 0x007327B0u
#define STRIDE 0x24u

static void push_word(uint32_t value)
{
    g_esp -= 4u;
    guest_write32(g_esp, value);
}
static uint32_t pop_word(void)
{
    const uint32_t value = guest_read32(g_esp);
    g_esp += 4u;
    return value;
}
static void pool_leaf_native_control(void)
{
    calls++;
    CHECK(g_esp == ROOT_STACK - 20u);
    CHECK(guest_read32(g_esp) == 0x000AE923u);
    CHECK(game_stack_arg(0u) == POOL && game_stack_arg(1u) == STRIDE);
    CHECK(game_stack_arg(2u) == STORAGE && game_stack_arg(3u) == 16u);
    CHECK(g_eax == 0x11223344u && g_ecx == 0x22334455u && g_edx == 0x33445566u);
    g_eax = game_stack_arg(3u);
    g_edx = game_stack_arg(2u);
    push_word(g_esi);
    g_esi = guest_read32(g_esp + 8u);
    g_ecx = 0u;
    guest_write32(g_esi, g_edx);
    guest_write32(g_esi + 8u, g_eax);
    guest_write32(g_esi + 12u, g_eax);
    if (g_eax != 0u) {
        push_word(g_ebx);
        g_ebx = guest_read32(g_esp + 16u);
        push_word(g_edi);
        g_edi = g_eax;
        do {
            g_eax = g_edx;
            g_edx += g_ebx;
            g_edi--;
            guest_write32(g_eax, g_ecx);
            g_ecx = g_eax;
        } while (g_edi != 0u);
        g_edi = pop_word();
        g_ebx = pop_word();
    }
    guest_write32(g_esi + 4u, g_ecx);
    g_esi = pop_word();
    CHECK(pop_word() == 0x000AE923u);
}

game_guest_function recomp_lookup(uint32_t va)
{
    return va == 0x00156DB0u ? pool_leaf_native_control : NULL;
}
extern void sub_000AE910(void);
static uint32_t prestate(unsigned mode, uint32_t expected)
{
    const uint32_t values[4] = {0u, 0xFFFFFFFFu, 0x80000000u, expected};
    return values[mode];
}
static void test_root(unsigned mode)
{
    g_esp = ROOT_STACK;
    g_eax = 0x11223344u; g_ecx = 0x22334455u; g_edx = 0x33445566u;
    g_ebx = 0x44556677u; g_ebp = 0x55667788u;
    g_esi = 0x66778899u; g_edi = 0x778899AAu;
    guest_write32(ROOT_STACK, 0x12345678u);
    for (unsigned j = 1u; j <= 8u; j++) {
        guest_write32(ROOT_STACK - 4u * j, (0xC0DE0000u | j) ^ 0x000AE910u);
    }
    for (unsigned index = 0u; index < 16u; index++) {
        const uint32_t node = STORAGE + STRIDE * index;
        guest_write32(node, prestate(mode, index == 0u ? 0u : node - STRIDE));
        for (uint32_t offset = 4u; offset < STRIDE; offset += 4u) {
            guest_write32(node + offset, 0x13579BDFu ^ (node + offset));
        }
    }
    const uint32_t pool_final[4] = {STORAGE, 0x007329CCu, 16u, 16u};
    for (unsigned j = 0u; j < 4u; j++) guest_write32(POOL + 4u * j, prestate(mode, pool_final[j]));
    guest_write32(STORAGE - 4u, 0x13579BDFu ^ (STORAGE - 4u));
    guest_write32(POOL + 16u, 0x13579BDFu ^ (POOL + 16u));
    sub_000AE910();
    CHECK(g_esp == ROOT_STACK + 4u && guest_read32(ROOT_STACK) == 0x12345678u);
    CHECK(g_eax == 0x007329CCu && g_ecx == 0x007329CCu && g_edx == POOL);
    CHECK(g_ebx == 0x44556677u && g_ebp == 0x55667788u);
    CHECK(g_esi == 0x66778899u && g_edi == 0x778899AAu);
    for (unsigned j = 0u; j < 4u; j++) CHECK(guest_read32(POOL + 4u * j) == pool_final[j]);
    for (unsigned index = 0u; index < 16u; index++) {
        const uint32_t node = STORAGE + STRIDE * index;
        CHECK(guest_read32(node) == (index == 0u ? 0u : node - STRIDE));
        for (uint32_t offset = 4u; offset < STRIDE; offset += 4u) {
            CHECK(guest_read32(node + offset) == (0x13579BDFu ^ (node + offset)));
        }
    }
    CHECK(guest_read32(STORAGE - 4u) == (0x13579BDFu ^ (STORAGE - 4u)));
    CHECK(guest_read32(POOL + 16u) == (0x13579BDFu ^ (POOL + 16u)));
    const uint32_t frame[8] = {16u, STORAGE, STRIDE, POOL, 0x000AE923u,
                               0x66778899u, 0x44556677u, 0x778899AAu};
    for (unsigned j = 0u; j < 8u; j++) CHECK(guest_read32(ROOT_STACK - 4u * (j + 1u)) == frame[j]);
}
int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    for (unsigned mode = 0u; mode < 4u; mode++) test_root(mode);
    CHECK(calls == 4u);
    printf("%u checks, %u failures\n", checks, failures);
    return failures != 0u;
}
