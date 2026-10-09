/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent ABI spy, not an implementation or equivalence proof of retail CRC.
 */
#include "game_replace.h"
#include <stdio.h>
#include <string.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base;
__thread uint32_t g_ebp, g_ebx, g_esi, g_edi;
__thread int g_df;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x10000];
static unsigned checks, failures, calls;
static uint32_t entry_sp, pointer, length, incoming_esi;
static int alias_slot;

#define CHECK(expression) do { checks++; if (!(expression)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); failures++; } } while (0)

void sub_00150530(void);

static void crc_leaf_spy(void)
{
    calls++;
    CHECK(g_esp == entry_sp - 16u);
    CHECK(guest_read32(g_esp) == 0x00150550u);
    CHECK(game_stack_arg(0u) == pointer);
    CHECK(game_stack_arg(1u) == length);
    CHECK(g_eax == length && g_edx == pointer);
    CHECK(g_ecx == 0xACDC1200u);
    CHECK(g_esi == pointer + 1u);
    CHECK(guest_read32(entry_sp - 4u) == incoming_esi);
    CHECK(guest_read32(entry_sp) == 0x9ABCDEF0u);
    if (alias_slot == 0) CHECK(guest_read32(pointer) == incoming_esi);
    if (alias_slot == 1) CHECK(guest_read32(pointer) == length);
    if (alias_slot == 2) CHECK(guest_read32(pointer) == pointer);
    if (alias_slot == 3) CHECK(guest_read32(pointer) == 0x00150550u);
    g_eax = 0xC0123456u;
    g_ecx = 0xEC123456u;
    g_edx = 0xED123456u;
    g_esp += 4u;
}

game_guest_function recomp_lookup(uint32_t va)
{
    CHECK(va == 0x001504F0u);
    return va == 0x001504F0u ? crc_leaf_spy : NULL;
}

static uint8_t pattern(unsigned kind, unsigned at)
{
    if (kind == 0u) return 0x41u;
    if (kind == 1u) return 0xFFu;
    if (kind == 2u) return at % 2u == 0u ? 0x80u : 1u;
    return (uint8_t)(1u + (7u * at % 255u));
}

static void prepare(void)
{
    memset(memory, 0xA5, sizeof memory);
    entry_sp = 0x8000u;
    g_esp = entry_sp;
    g_eax = 0xEA123456u;
    g_ecx = 0xACDC12EFu;
    g_edx = 0xDD123456u;
    g_esi = incoming_esi;
    g_ebx = 0xBB123456u;
    g_edi = 0xDD987654u;
    g_ebp = 0xB0123456u;
    guest_write32(entry_sp, 0x9ABCDEF0u);
    guest_write32(entry_sp + 4u, pointer);
}

static void run(void)
{
    const unsigned before = calls;
    sub_00150530();
    CHECK(calls == before + 1u);
    CHECK(g_esp == entry_sp + 4u);
    CHECK(g_eax == 0xC0123456u && g_ecx == 0xEC123456u && g_edx == 0xED123456u);
    CHECK(g_esi == incoming_esi);
    CHECK(g_ebx == 0xBB123456u && g_edi == 0xDD987654u && g_ebp == 0xB0123456u);
    CHECK(guest_read32(entry_sp) == 0x9ABCDEF0u);
}

int main(void)
{
    static const unsigned lengths[] = {0u, 1u, 2u, 3u, 7u, 15u, 31u, 63u, 127u};
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    for (unsigned n = 0u; n < sizeof lengths / sizeof lengths[0]; n++) {
        for (unsigned kind = 0u; kind < 4u; kind++) {
            for (unsigned align = 0u; align < 4u; align++) {
                pointer = 0x1000u + align;
                length = lengths[n];
                incoming_esi = 0xEE123456u;
                alias_slot = -1;
                prepare();
                for (unsigned i = 0u; i < length; i++) guest_write8(pointer + i, pattern(kind, i));
                guest_write8(pointer + length, 0u);
                run();
            }
        }
    }
    for (unsigned kind = 0u; kind < 4u; kind++) {
        for (alias_slot = 0; alias_slot < 4; alias_slot++) {
            pointer = 0x8000u - 4u * ((unsigned)alias_slot + 1u);
            length = 4u * (unsigned)alias_slot + 3u;
            incoming_esi = 0x00434241u;
            prepare();
            for (unsigned i = 0u; i < length - 3u; i++) guest_write8(pointer + i, pattern(kind, i));
            run();
        }
    }
    printf("T1477 CRC ABI: %u checks, %u calls, %u failures\n", checks, calls, failures);
    return failures ? 1 : 0;
}
