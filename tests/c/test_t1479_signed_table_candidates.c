/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ordinary mocked call-frame controls, not original equivalence evidence.
 */
#include "game_replace.h"
#include <stdio.h>
#include <string.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base, g_esi;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x800000];
static unsigned checks, failures, calls;
static uint32_t target, argument, return_pc, frame, incoming_eax, incoming_esi;
#define STACK 0x10000u
#define CHECK(expr) do { checks++; if (!(expr)) { failures++; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); } } while (0)
void t1479_candidate_338150(void);
void t1479_candidate_2d2c10(void);
void t1479_candidate_2d2c40(void);

static void ordinary_callee(void)
{
    calls++;
    CHECK(g_esp == frame);
    CHECK(guest_read32(g_esp) == return_pc);
    CHECK(game_stack_arg(0u) == argument);
    CHECK(g_eax == incoming_eax && g_esi == incoming_esi);
    g_eax = target == 0x45590u ? 0xABCDEF01u : 0x20000u;
    g_ecx = 0xEC123456u;
    g_edx = 0xED123456u;
    g_esp += 4u;
}

game_guest_function recomp_lookup(uint32_t va)
{
    CHECK(va == target);
    return va == target ? ordinary_callee : NULL;
}

static void reset(void)
{
    memset(memory, 0, sizeof(memory));
    g_esp = STACK;
    g_eax = 0x12345678u;
    g_esi = 0x76543210u;
    g_ecx = 0x11111111u;
    g_edx = 0x22222222u;
    guest_write32(STACK, 0x10101010u);
    calls = 0u;
}

int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    const uint32_t indexes[] = {0xFFFFFFFFu, 0u, 7u, 0x40000000u};
    for (unsigned i = 0u; i < 4u; i++) {
        reset();
        argument = indexes[i]; target = 0x45590u; return_pc = 0x33815Bu;
        frame = STACK - 12u; incoming_eax = g_eax; incoming_esi = argument;
        guest_write32(STACK + 4u, argument);
        t1479_candidate_338150();
        CHECK(calls == 1u && g_esp == STACK);
        CHECK(g_eax == 0xABCDEF01u && g_esi == 0x76543210u);
        CHECK(guest_read32(0x765E4Cu) == argument);
        CHECK(g_ecx == 0xEC123456u && g_edx == 0xED123456u);
    }
    const uint32_t modes[] = {0u, 6u, 7u, 0xFFFFFFFFu};
    const uint32_t bytes[] = {0u, 0x7Fu, 0x80u, 0xFFu};
    const uint32_t table_indexes[] = {0xFFFFFFFFu, 20u, 21u};
    for (unsigned root = 0u; root < 2u; root++)
        for (unsigned m = 0u; m < 4u; m++)
            for (unsigned b = 0u; b < 4u; b++)
                for (unsigned j = 0u; j < 3u; j++) {
                    reset();
                    target = 0x22E050u; argument = table_indexes[j];
                    return_pc = root == 0u ? 0x2D2C32u : 0x2D2C62u;
                    frame = STACK - 8u; incoming_eax = argument; incoming_esi = g_esi;
                    guest_write32(0x78AD2Cu, modes[m]);
                    guest_write32(0x78AD40u, argument);
                    guest_write8(0x78ADAEu, (uint8_t)bytes[b]);
                    guest_write8(0x78ADACu, (uint8_t)bytes[b]);
                    guest_write32(0x20004u, 0x44556677u);
                    guest_write32(0x20008u, 0x8899AABBu);
                    if (root == 0u) t1479_candidate_2d2c10();
                    else t1479_candidate_2d2c40();
                    const unsigned active = modes[m] == 6u || modes[m] == 7u;
                    const uint32_t fallback = root == 0u || bytes[b] < 128u
                        ? bytes[b] : bytes[b] | 0xFFFFFF00u;
                    CHECK(g_eax == (active ? (root == 0u ? 0x44556677u : 0x8899AABBu) : fallback));
                    CHECK(calls == active && g_esp == STACK && g_esi == 0x76543210u);
                    CHECK(g_ecx == (active ? 0xEC123456u : 0x11111111u));
                    CHECK(g_edx == (active ? 0xED123456u : 0x22222222u));
                    CHECK(guest_read32(STACK) == 0x10101010u);
                }
    printf("T1479 ordinary frame controls: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
