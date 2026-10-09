/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent ordinary controls for the actual scratch adapter; no fault probes.
 */
#include "game_replace.h"
#include <stdio.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x01000000];
static unsigned checks, failures, cases;

#include "../../tools/harness/candidates/t1535_menu_index_flags.c"

#define CHECK(e) do { checks++; if (!(e)) { failures++; \
    fprintf(stderr, "line%d:%s\n", __LINE__, #e); } } while (0)

int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    CHECK(game_entry_002D2BE0.va == 0x002D2BE0u);
    CHECK(game_entry_002D2BE0.convention == GAME_CC_cdecl);
    CHECK(game_entry_002D2BE0.stack_args == 2u);
    CHECK(game_entry_002D2BE0.returns_value == 1u);
    CHECK(game_entry_002D2BE0.scratch_mask == 0u);
    CHECK(game_entry_002D2BE0.input_abi == 0u);
    const uint8_t modes[] = {0u, 9u, 10u, 11u, 255u};
    const uint32_t indices[] = {0u, 7u, 255u, 0x08000000u, 0x80000000u, 0xFFFFFFFFu};
    const uint32_t values[] = {0u, 1u, 0x80000000u, 0xFFFFFFFFu};
    const uint32_t decoys[] = {0u, 0xDEADBEEFu};
    /* Independent truth table: row mode==10, column loaded word==0. */
    const uint32_t masks[2][2] = {{0x00020000u, 0x02020000u}, {0u, 0x02000000u}};
    for (unsigned frame = 0u; frame < 2u; frame++)
    for (unsigned mode = 0u; mode < 5u; mode++)
    for (unsigned index = 0u; index < 6u; index++)
    for (unsigned value = 0u; value < 4u; value++)
    for (unsigned decoy = 0u; decoy < 2u; decoy++) {
        uint32_t sp = 0x10000u + frame * 0x400u;
        uint32_t offset = (indices[index] & 0x07FFFFFFu) * 32u;
        uint32_t address = 0x004D1F88u + offset;
        uint32_t before[] = {0x55AA55AAu, 0xCC998877u, 0x11335577u,
            0x12345678u, sp, 0x87654321u, 0xA0B0C0D0u, 0xFEDCBA98u};
        g_eax = before[0]; g_ecx = before[1]; g_edx = before[2]; g_ebx = before[3];
        g_esp = before[4]; g_ebp = before[5]; g_esi = before[6]; g_edi = before[7];
        guest_write32(sp, 0x00123456u);
        guest_write32(sp + 4u, decoys[decoy]);
        guest_write32(sp + 8u, indices[index]);
        guest_write8(0x007DE455u, modes[mode]);
        guest_write32(address, values[value]);
        guest_write32(address - 4u, 0xBADC0FFEu);
        guest_write32(address + 4u, 0xCAFEBABEu);
        uint64_t counter = game_entry_002D2BE0.calls;
        sub_002D2BE0();
        uint32_t actual[] = {g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi};
        before[0] = masks[modes[mode] == 10u][values[value] == 0u];
        before[1] = offset; before[2] = values[value]; before[4] = sp + 4u;
        for (unsigned reg = 0u; reg < 8u; reg++) CHECK(actual[reg] == before[reg]);
        CHECK(guest_read32(address) == values[value]);
        CHECK(guest_read32(address - 4u) == 0xBADC0FFEu);
        CHECK(guest_read32(address + 4u) == 0xCAFEBABEu);
        CHECK(guest_read8(0x007DE455u) == modes[mode]);
        CHECK(guest_read32(sp) == 0x00123456u);
        CHECK(guest_read32(sp + 4u) == decoys[decoy]);
        CHECK(guest_read32(sp + 8u) == indices[index]);
        CHECK(game_entry_002D2BE0.calls == counter + 1u);
        cases++;
    }
    printf("T1535 actual adapter: %u cases, %u checks, %u failures\n", cases, checks, failures);
    return failures ? 1 : 0;
}
