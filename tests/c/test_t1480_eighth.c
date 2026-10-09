/* SPDX-License-Identifier: GPL-3.0-or-later
 * Synthetic native callee controls for retail frames and scratch handoffs.
 * Authentic nested callees are checked separately by selected differential proofs. */
#include "game_replace.h"
#include <stdio.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base;
__thread uint32_t g_ebx, g_ebp, g_esi, g_edi;
ptrdiff_t g_xbox_mem_offset;
static unsigned char memory[0x800000];
static unsigned checks, failures, calls;
static const uint32_t entry = 0x10000u, context = 0x20000u, data = 0x21000u;
static unsigned mode;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; fprintf(stderr, "%d: %s\n", __LINE__, #x); } } while (0)
void sub_003BC2A0(void);
void sub_0038F565(void);

static void put16(uint32_t p, uint16_t v) { memcpy(game_host_ptr(p), &v, 2u); }
static uint16_t get16(uint32_t p) { uint16_t v; memcpy(&v, game_host_ptr(p), 2u); return v; }
static void block(void)
{
    ++calls;
    CHECK(g_esp == entry - 20u && guest_read32(g_esp) == 0x3BC2E9u);
    CHECK(game_stack_arg(0u) == context);
    CHECK(g_eax == 64u && g_esi == context);
    CHECK(guest_read32(context) % 64u == 0u);
    g_eax = 0xDEADBEEFu; g_ecx = 0xABCD1234u; g_edx = 0x789u;
    if (mode == 1u) guest_write32(entry - 8u, 0x76543210u);
    g_esp += 4u;
}
static void bits(void)
{
    ++calls;
    CHECK(g_esp == entry - 32u && guest_read32(g_esp) == 0x38F591u);
    CHECK(game_stack_arg(0u) == data);
    CHECK(game_stack_arg(2u) == 2u);
    CHECK(g_ebp == entry - 4u);
    g_eax = 1u; g_ecx = 0xAABBu; g_edx = 2u;
    g_esp += 4u;
}
game_guest_function recomp_lookup(uint32_t va)
{
    switch (va) {
    case 0x3BB9C0u: return block;
    case 0x38F510u: return bits;
    default: CHECK(0); return NULL;
    }
}
static void reset(void)
{
    memset(memory, 0, sizeof memory);
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    g_esp = entry; g_eax = 0x1111u; g_ecx = 0x22334455u; g_edx = 0x33445566u;
    g_ebp = 0x4444u; g_ebx = 0x5555u; g_esi = 0x6666u; g_edi = 0x7777u;
    calls = 0u; mode = 0u;
}
static void args(uint32_t a, uint32_t b, uint32_t c)
{
    guest_write32(entry + 4u, a); guest_write32(entry + 8u, b); guest_write32(entry + 12u, c);
}
int main(void)
{
    for (unsigned length = 0u; length <= 65u; ++length) {
        reset(); args(context, data, length);
        for (unsigned i = 0u; i < length; ++i) guest_write8(data + i, (uint8_t)(i + 1u));
        sub_003BC2A0();
        CHECK(guest_read32(context) == length && g_eax == length % 64u);
        CHECK(calls == length / 64u);
        CHECK(g_esi == 0x6666u && g_edi == 0x7777u && g_ebx == 0x5555u);
        CHECK(g_esp == entry + 4u);
        if (length != 0u) CHECK(guest_read8(context + 20u + (length - 1u) % 64u) == length);
    }
    reset(); args(context, data, 0xFFFFFFFFu); guest_write8(data, 42u); sub_003BC2A0();
    CHECK(guest_read32(context) == 1u && g_eax == 1u && g_ecx == 42u && g_edx == 1u);
    reset(); args(context, data, 1u); guest_write32(context, 63u); mode = 1u; sub_003BC2A0();
    CHECK(g_edi == 0x76543210u && g_eax == 0u && g_ecx == 0xABCD1234u && g_edx == 0x789u);

    reset(); args(context, 8u, 0x22000u);
    guest_write32(entry + 16u, data); guest_write32(entry + 20u, 0x23000u);
    guest_write32(entry + 24u, 2u); put16(context + 2u, 0x8ABCu);
    sub_0038F565();
    CHECK(calls == 1u && g_eax == 0u && g_ecx == 0xABCu && g_edx == 2u);
    CHECK(get16(0x22000u) == 0xABCu && guest_read32(0x23000u) == 2u);
    CHECK(guest_read32(entry + 4u) == 8u && g_ebp == 0x4444u);
    reset(); args(context, 8u, 0x22000u);
    guest_write32(entry + 16u, data); guest_write32(entry + 20u, 0x23000u);
    guest_write32(entry + 24u, 1u); sub_0038F565();
    CHECK(calls == 0u && g_eax == 0x80040004u && g_ecx == 0u && g_edx == 2u);

    printf("T1480 eighth native: %u checks, %u failures\n", checks, failures);
    return failures != 0u;
}
