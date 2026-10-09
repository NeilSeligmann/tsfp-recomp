/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent actual-adapter ordinary checks, no native fault probes.
 */
#include "game_replace.h"
#include <stdio.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x01000000];
static unsigned checks, failures, cases;
#include "../../tools/harness/candidates/t1538_hud_slot.c"
#define CHECK(e) do { checks++; if (!(e)) { failures++; \
    fprintf(stderr, "line%d:%s\n", __LINE__, #e); } } while (0)

int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    CHECK(game_entry_00139160.va == 0x00139160u);
    CHECK(game_entry_00139160.convention == GAME_CC_cdecl);
    CHECK(game_entry_00139160.stack_args == 0u);
    CHECK(game_entry_00139160.returns_value == 1u);
    CHECK(game_entry_00139160.scratch_mask == 0u);
    CHECK(game_entry_00139160.input_abi == 0u);
    const uint32_t slots[] = {0xFFFFFFFEu, 0xFFFFFFFFu, 0u, 1u, 3u, 0x40000000u, 0x7FFFFFFFu};
    /* Expected mapped addresses independently enumerated, including wrap. */
    const uint32_t addresses[] = {0x7A3268u, 0x7A49E4u, 0x7A6160u,
        0x7A78DCu, 0x7AA7D4u, 0x7A6160u, 0x7A49E4u};
    const uint8_t flags[] = {0u, 1u, 0x40u, 0x41u, 0x80u, 0xFFu};
    const uint32_t states[] = {0u, 0xCu, 0x11u, 1u, 0xFFFFFFFFu};
    const uint32_t state_results[] = {1u, 0u, 0u, 1u, 1u};
    for (unsigned frame = 0u; frame < 2u; frame++)
    for (unsigned slot = 0u; slot < 7u; slot++)
    for (unsigned flag = 0u; flag < 6u; flag++)
    for (unsigned state = 0u; state < 5u; state++) {
        uint32_t sp = 0x10000u + frame * 0x400u;
        uint32_t player = 0xDC0000u + frame * 0x100u;
        uint32_t at = addresses[slot];
        uint32_t before[] = {0x55AA55AAu, 0xCC998877u, 0x11335577u,
            0x12345678u, sp, 0x87654321u, 0xA0B0C0D0u, 0xFEDCBA98u};
        g_eax = before[0]; g_ecx = before[1]; g_edx = before[2]; g_ebx = before[3];
        g_esp = before[4]; g_ebp = before[5]; g_esi = before[6]; g_edi = before[7];
        guest_write32(sp, 0x00123456u);
        guest_write32(0x7B0C7Cu, player);
        guest_write32(player + 4u, slots[slot]);
        guest_write8(player + 12u, flags[flag]);
        guest_write32(at, states[state]);
        guest_write32(at - 4u, 0xBADC0FFEu);
        guest_write32(at + 4u, 0xCAFEBABEu);
        uint64_t counter = game_entry_00139160.calls;
        sub_00139160();
        uint32_t actual[] = {g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi};
        before[0] = slot < 2u || (flags[flag] & 0x40u) != 0u ? 0u : state_results[state];
        before[1] = player;
        if (slot >= 2u) before[2] = 0x11335500u | flags[flag];
        before[4] = sp + 4u;
        for (unsigned reg = 0u; reg < 8u; reg++) CHECK(actual[reg] == before[reg]);
        CHECK(guest_read32(0x7B0C7Cu) == player);
        CHECK(guest_read32(player + 4u) == slots[slot]);
        CHECK(guest_read8(player + 12u) == flags[flag]);
        CHECK(guest_read32(at) == states[state]);
        CHECK(guest_read32(at - 4u) == 0xBADC0FFEu);
        CHECK(guest_read32(at + 4u) == 0xCAFEBABEu);
        CHECK(guest_read32(sp) == 0x00123456u);
        CHECK(game_entry_00139160.calls == counter + 1u);
        cases++;
    }
    printf("T1538 actual adapter: %u cases, %u checks, %u failures\n", cases, checks, failures);
    return failures ? 1 : 0;
}
