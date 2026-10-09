/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent ordinary native expectations for the actual explicit-ECX adapter 00286F10.
 * Expected values are computed from the table contents, not by the candidate logic.
 * No original-code execution or frame/data alias experiment.
 */
#include "game_replace.h"
#include <stdio.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x01000000];
static unsigned checks, failures;

#include "../../tools/harness/candidates/t1528_ecx_table_index.c"

#define CHECK(e) do { checks++; if (!(e)) { failures++; \
    fprintf(stderr, "line%d:%s\n", __LINE__, #e); } } while (0)

#define TABLE 0x005208B8u

int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    CHECK(game_entry_00286F10.va == 0x00286F10u);
    CHECK(game_entry_00286F10.convention == GAME_CC_cdecl);
    CHECK(game_entry_00286F10.stack_args == 0u);
    CHECK(game_entry_00286F10.returns_value == 1u);
    CHECK(game_entry_00286F10.scratch_mask == 0u);
    CHECK(game_entry_00286F10.input_abi == GAME_INPUT_ecx);
    /* Table with a duplicate (value 0x40 at 2 and 5) and a value 0 entry at slot 6. */
    const uint32_t table[8] = {0x10u, 0x20u, 0x40u, 0x80u, 0x100u, 0x40u, 0u, 0xFFFFFFFFu};
    /* Probe values and independently hand-derived indices (first match, else 0). */
    const struct { uint32_t probe; uint32_t expect; } probes[] = {
        {0x10u, 0u}, {0x20u, 1u}, {0x40u, 2u}, {0x80u, 3u}, {0x100u, 4u},
        {0u, 6u}, {0xFFFFFFFFu, 7u}, {0x12345678u, 0u}, {0x7FFFFFFFu, 0u},
        {0x80000000u, 0u}, {1u, 0u}, {0x200u, 0u},
    };
    for (unsigned frame = 0u; frame < 2u; frame++) {
        for (unsigned probe = 0u; probe < 12u; probe++) {
            for (unsigned decoy = 0u; decoy < 2u; decoy++) {
                uint32_t sp = 0x10000u + frame * 0x400u;
                uint32_t saved[] = {0x55AA55AAu, probes[probe].probe, 0x11223344u ^ decoy,
                    0x55667788u, sp, 0x99AABBCCu, 0xDDEEFF00u, 0x31415926u};
                g_eax = saved[0]; g_ecx = saved[1]; g_edx = saved[2]; g_ebx = saved[3];
                g_esp = saved[4]; g_ebp = saved[5]; g_esi = saved[6]; g_edi = saved[7];
                for (unsigned slot = 0u; slot < 8u; slot++) {
                    guest_write32(TABLE + slot * 4u, table[slot]);
                }
                guest_write32(TABLE + 32u, probes[probe].probe);   /* guard: slot 8 must be ignored */
                guest_write32(sp, 0x00123456u);
                uint64_t prior = game_entry_00286F10.calls;
                sub_00286F10();
                uint32_t actual[] = {g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi};
                saved[0] = probes[probe].expect; saved[4] = sp + 4u;
                for (unsigned reg = 0u; reg < 8u; reg++) CHECK(actual[reg] == saved[reg]);
                for (unsigned slot = 0u; slot < 8u; slot++) {
                    CHECK(guest_read32(TABLE + slot * 4u) == table[slot]);
                }
                CHECK(guest_read32(TABLE + 32u) == probes[probe].probe);
                CHECK(guest_read32(sp) == 0x00123456u);
                CHECK(game_entry_00286F10.calls == prior + 1u);
            }
        }
    }
    /* Slot-8-only match must return 0 (the scan stops at 8 entries). */
    {
        uint32_t sp = 0x10000u;
        for (unsigned slot = 0u; slot < 8u; slot++) guest_write32(TABLE + slot * 4u, 1u + slot);
        guest_write32(TABLE + 32u, 0xCAFEBABEu);
        g_eax = 0xFFFFFFFFu; g_ecx = 0xCAFEBABEu; g_edx = 7u; g_esp = sp;
        guest_write32(sp, 0x00123456u);
        sub_00286F10();
        CHECK(g_eax == 0u && g_ecx == 0xCAFEBABEu && g_edx == 7u && g_esp == sp + 4u);
    }
    printf("T1528 actual ECX table-index adapter: 24 cases+1, %u checks, %u failures\n",
        checks, failures);
    return failures ? 1 : 0;
}
