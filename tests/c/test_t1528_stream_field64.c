/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent ordinary native expectations for the actual explicit-input adapter.
 * No original-code execution or frame/data alias experiment.
 */
#include "game_replace.h"
#include <stdio.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x01000000];
static unsigned checks, failures;

#include "../../tools/harness/candidates/t1528_stream_field64.c"

#define CHECK(e) do { checks++; if (!(e)) { failures++; \
    fprintf(stderr, "line%d:%s\n", __LINE__, #e); } } while (0)

int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    CHECK(game_entry_00029E40.va == 0x00029E40u);
    CHECK(game_entry_00029E40.convention == GAME_CC_stdcall);
    CHECK(game_entry_00029E40.stack_args == 1u);
    CHECK(game_entry_00029E40.returns_value == 1u);
    CHECK(game_entry_00029E40.scratch_mask == 0u);
    CHECK(game_entry_00029E40.input_abi == GAME_INPUT_ecx);
    const uint32_t values[] = {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x12345678u};
    for (unsigned frame = 0u; frame < 2u; frame++) {
        for (unsigned target = 0u; target < 2u; target++) {
            for (unsigned value = 0u; value < 6u; value++) {
                for (unsigned decoy = 0u; decoy < 2u; decoy++) {
                    uint32_t sp = 0x10000u + frame * 0x400u;
                    uint32_t object = 0xD90000u + target * 0x10000u;
                    uint32_t saved[] = {0x55AA55AAu, object, 0x11223344u ^ decoy,
                        0x55667788u, sp, 0x99AABBCCu, 0xDDEEFF00u, 0x31415926u};
                    g_eax = saved[0]; g_ecx = saved[1]; g_edx = saved[2]; g_ebx = saved[3];
                    g_esp = saved[4]; g_ebp = saved[5]; g_esi = saved[6]; g_edi = saved[7];
                    guest_write32(sp, 0x00123456u);
                    guest_write32(sp + 4u, values[value]);
                    guest_write32(object + 0x60u, 0xBAD00BADu);
                    guest_write32(object + 0x64u, ~values[value]);
                    guest_write32(object + 0x68u, 0xC0FFEE00u);
                    uint64_t prior = game_entry_00029E40.calls;
                    sub_00029E40();
                    uint32_t actual[] = {g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi};
                    saved[0] = values[value]; saved[4] = sp + 8u;
                    for (unsigned reg = 0u; reg < 8u; reg++) CHECK(actual[reg] == saved[reg]);
                    CHECK(guest_read32(object + 0x64u) == values[value]);
                    CHECK(guest_read32(object + 0x60u) == 0xBAD00BADu);
                    CHECK(guest_read32(object + 0x68u) == 0xC0FFEE00u);
                    CHECK(guest_read32(sp) == 0x00123456u);
                    CHECK(guest_read32(sp + 4u) == values[value]);
                    CHECK(game_entry_00029E40.calls == prior + 1u);
                }
            }
        }
    }
    printf("T1528 actual ECX field64 adapter: 48 cases, %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
