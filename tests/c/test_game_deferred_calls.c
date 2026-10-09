/* SPDX-License-Identifier: GPL-3.0-or-later
 * Explicit native frame controls, not original differential execution.
 */
#include "game_replace.h"
#include <stdio.h>
#include <string.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base;
__thread uint32_t g_esi;
ptrdiff_t g_xbox_mem_offset;
static unsigned checks, failures, phase;
static uint32_t expected_mask, expected_returns[3];
static uint8_t memory[0x800000];
#define STACK 0x10000u
#define CHECK(expr) do { checks++; if (!(expr)) { failures++; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); } } while (0)
void sub_0009D740(void);
void sub_0009D780(void);

static void id_control(void)
{
    CHECK(phase < 2u);
    CHECK(g_esp == STACK - (phase == 0u ? 20u : 24u));
    CHECK(guest_read32(g_esp) == expected_returns[phase]);
    CHECK(game_stack_arg(0u) == (phase == 0u ? 0x22000u : 0x11000u));
    if (phase == 0u) {
        CHECK(game_stack_arg(1u) == expected_mask);
        CHECK(game_stack_arg(2u) == 0u);
        CHECK(g_eax == 0x22000u);
        g_eax = 4u;
    } else {
        CHECK(game_stack_arg(1u) == 4u);
        CHECK(game_stack_arg(2u) == expected_mask);
        CHECK(game_stack_arg(3u) == 0u);
        CHECK(g_eax == 4u);
        g_eax = 3u;
    }
    CHECK(g_esi == 0x11000u);
    g_ecx = 0xAB00u + phase;
    g_edx = 0xCD00u + phase;
    phase++;
    g_esp += 4u;
}

static void link_control(void)
{
    CHECK(phase == 2u);
    CHECK(g_esp == STACK - 24u);
    CHECK(guest_read32(g_esp) == expected_returns[2]);
    CHECK(game_stack_arg(0u) == 3u);
    CHECK(game_stack_arg(1u) == 4u);
    CHECK(game_stack_arg(2u) == expected_mask);
    CHECK(game_stack_arg(3u) == 0u);
    CHECK(g_eax == 3u && g_esi == 0x11000u);
    g_eax = 0x33000u;
    g_ecx = 0xAB02u;
    g_edx = 0xCD02u;
    g_esp += 4u;
    phase++;
}

game_guest_function recomp_lookup(uint32_t va)
{
    if (va == 0x9A510u) return id_control;
    if (va == 0x9D5E0u) return link_control;
    return NULL;
}

static void initialize(void)
{
    memset(memory, 0xA5, sizeof(memory));
    g_esp = STACK;
    g_eax = 0x1234u;
    g_ecx = 0x2345u;
    g_edx = 0x3456u;
    g_esi = 0x4567u;
    guest_write32(STACK, 0x9988u);
    guest_write32(STACK + 4u, 0x11000u);
    guest_write32(STACK + 8u, 0x22000u);
    phase = 0u;
}

int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)memory;
    for (unsigned variant = 0u; variant < 2u; variant++) {
        initialize();
        expected_mask = variant == 0u ? 0x10000u : 0u;
        expected_returns[0] = variant == 0u ? 0x9D75Eu : 0x9D79Bu;
        expected_returns[1] = variant == 0u ? 0x9D768u : 0x9D7A5u;
        expected_returns[2] = variant == 0u ? 0x9D771u : 0x9D7AEu;
        if (variant == 0u) sub_0009D740(); else sub_0009D780();
        CHECK(phase == 3u && g_esp == STACK + 4u);
        CHECK(g_eax == 0x33000u && g_ecx == 0xAB02u && g_edx == 0xCD02u);
        CHECK(g_esi == 0x4567u && guest_read32(STACK) == 0x9988u);
        CHECK(guest_read32(STACK - 4u) == 0x4567u);
        for (unsigned missing = 0u; missing < 2u; missing++) {
            initialize();
            guest_write32(STACK + 4u + 4u * missing, 0u);
            if (variant == 0u) sub_0009D740(); else sub_0009D780();
            CHECK(phase == 0u && g_eax == 0u && g_esp == STACK + 4u);
            CHECK(g_esi == 0x4567u && g_ecx == 0x2345u && g_edx == 0x3456u);
        }
    }
    printf("deferred frame controls: %u checks, %u failures\n", checks, failures);
    return failures != 0u;
}
