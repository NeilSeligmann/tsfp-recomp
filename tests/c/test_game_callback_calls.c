/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1476 original literal call frames, real registered indexed-store leaf,
 * and callback table overwrite/guard behavior. No disc or lifted input.
 */
#include "game_replace.h"
#include <stdio.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base;
__thread uint32_t g_ebx, g_ebp, g_esi, g_edi;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x00800000];
static unsigned checks, failures;
#define CHECK(expr) do { checks++; if (!(expr)) { failures++; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); } } while (0)

extern void sub_0009E2D0(void);
extern void sub_000AE8B0(void), sub_000AEA20(void), sub_000B2FA0(void);
extern void sub_000C3510(void), sub_000D9210(void);

struct expected_call {
    uint32_t mask, slot, callback, return_pc, destination;
};
struct expected_root {
    uint32_t va;
    void (*function)(void);
    unsigned count;
    struct expected_call calls[3];
};
static const struct expected_root roots[] = {
    {0x000AE8B0u, sub_000AE8B0, 2u, {
        {0x80u, 0u, 0x000AC940u, 0x000AE8C1u, 0x0072F388u},
        {0x80u, 1u, 0x000A92F0u, 0x000AE8D2u, 0x0072F38Cu}}},
    {0x000AEA20u, sub_000AEA20, 3u, {
        {0x4000000u, 0u, 0x000AE940u, 0x000AEA31u, 0x0072F550u},
        {0x4000000u, 3u, 0x000AE9B0u, 0x000AEA42u, 0x0072F55Cu},
        {0x4000000u, 1u, 0x000AE980u, 0x000AEA53u, 0x0072F554u}}},
    {0x000B2FA0u, sub_000B2FA0, 2u, {
        {0x1000u, 1u, 0x000B2D70u, 0x000B2FB1u, 0x0072F404u},
        {0x100000u, 1u, 0x000B2DB0u, 0x000B2FC2u, 0x0072F4C4u}}},
    {0x000C3510u, sub_000C3510, 2u, {
        {0x800u, 1u, 0x000C2E70u, 0x000C3521u, 0x0072F3ECu},
        {0x800u, 4u, 0x000C30A0u, 0x000C3532u, 0x0072F3F8u}}},
    {0x000D9210u, sub_000D9210, 2u, {
        {8u, 3u, 0x000D8E30u, 0x000D921Eu, 0x0072F334u},
        {8u, 4u, 0x000D9030u, 0x000D922Cu, 0x0072F338u}}},
};
static const struct expected_root *active_root;
static unsigned call_index;
#define ROOT_STACK 0x00010000u

static void checked_indexed_store(void)
{
    CHECK(call_index < active_root->count);
    const struct expected_call *call = &active_root->calls[call_index];
    const uint32_t frame = ROOT_STACK - 16u - 12u * call_index;
    CHECK(g_esp == frame);
    CHECK(guest_read32(frame) == call->return_pc);
    CHECK(game_stack_arg(0u) == call->mask);
    CHECK(game_stack_arg(1u) == call->slot);
    CHECK(game_stack_arg(2u) == call->callback);
    if (call_index == 0u) {
        CHECK(g_eax == 0x11223344u && g_ecx == 0x22334455u && g_edx == 0x33445566u);
    } else {
        const struct expected_call *previous = &active_root->calls[call_index - 1u];
        CHECK(g_eax == previous->callback && g_ecx == previous->slot);
        CHECK(g_edx == (previous->destination - 0x0072F2E0u) / 4u);
    }
    sub_0009E2D0();
    CHECK(g_esp == frame + 4u);
    CHECK(g_eax == call->callback && g_ecx == call->slot);
    CHECK(g_edx == (call->destination - 0x0072F2E0u) / 4u);
    call_index++;
}

game_guest_function recomp_lookup(uint32_t va)
{
    return va == 0x0009E2D0u ? checked_indexed_store : NULL;
}

static int is_target(uint32_t address)
{
    for (unsigned i = 0u; i < active_root->count; i++) {
        if (active_root->calls[i].destination == address) return 1;
    }
    return 0;
}

static void test_root(const struct expected_root *root, unsigned mode)
{
    active_root = root;
    call_index = 0u;
    g_esp = ROOT_STACK;
    g_eax = 0x11223344u;
    g_ecx = 0x22334455u;
    g_edx = 0x33445566u;
    g_ebx = 0x44556677u;
    g_ebp = 0x55667788u;
    g_esi = 0x66778899u;
    g_edi = 0x778899AAu;
    guest_write32(ROOT_STACK, 0x12345678u);
    for (unsigned j = 1u; j <= 3u * root->count + 1u; j++) {
        guest_write32(ROOT_STACK - 4u * j, (0xC0DE0000u | j) ^ root->va);
    }
    for (unsigned i = 0u; i < root->count; i++) {
        const struct expected_call *call = &root->calls[i];
        const uint32_t values[4] = {0u, 0xFFFFFFFFu, 0x80000000u, call->callback};
        guest_write32(call->destination, values[mode]);
        const uint32_t neighbors[2] = {call->destination - 4u, call->destination + 4u};
        for (unsigned n = 0u; n < 2u; n++) {
            if (!is_target(neighbors[n])) {
                guest_write32(neighbors[n], 0x13579BDFu ^ neighbors[n]);
            }
        }
    }
    root->function();
    const struct expected_call *last = &root->calls[root->count - 1u];
    CHECK(call_index == root->count && g_esp == ROOT_STACK + 4u);
    CHECK(g_eax == last->callback && g_ecx == last->slot);
    CHECK(g_edx == (last->destination - 0x0072F2E0u) / 4u);
    CHECK(g_ebx == 0x44556677u && g_ebp == 0x55667788u);
    CHECK(g_esi == 0x66778899u && g_edi == 0x778899AAu);
    CHECK(guest_read32(ROOT_STACK) == 0x12345678u);
    CHECK(guest_read32(ROOT_STACK - 12u * root->count - 4u) == last->return_pc);
    for (unsigned i = 0u; i < root->count; i++) {
        const struct expected_call *call = &root->calls[i];
        CHECK(guest_read32(call->destination) == call->callback);
        CHECK(guest_read32(ROOT_STACK - 12u * i - 4u) == call->callback);
        CHECK(guest_read32(ROOT_STACK - 12u * i - 8u) == call->slot);
        CHECK(guest_read32(ROOT_STACK - 12u * i - 12u) == call->mask);
        const uint32_t neighbors[2] = {call->destination - 4u, call->destination + 4u};
        for (unsigned n = 0u; n < 2u; n++) {
            if (!is_target(neighbors[n])) {
                CHECK(guest_read32(neighbors[n]) == (0x13579BDFu ^ neighbors[n]));
            }
        }
    }
}

int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    for (unsigned root = 0u; root < sizeof(roots) / sizeof(roots[0]); root++) {
        for (unsigned mode = 0u; mode < 4u; mode++) test_root(&roots[root], mode);
    }
    printf("%u checks, %u failures\n", checks, failures);
    return failures != 0u;
}
