/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ordinary mock call controls, not original execution.
 */
#include "game_replace.h"
#include <stdio.h>
#include <string.h>
__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x800000];
static unsigned checks, failures, ancestor_calls, identity_calls, mode_calls;
static uint32_t profile_flags, first_null, mode_value;
#define STACK 0x10000u
#define PROFILE 0x30000u
#define ACTOR 0x20000u
#define CHECK(expr) do { checks++; if (!(expr)) { failures++; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); } } while (0)
void sub_002FBF70(void);

static void write_word(uint32_t address, uint16_t value)
{
    memcpy(game_host_ptr(address), &value, sizeof value);
}

static void ancestor(void)
{
    CHECK(g_esp == STACK - 4u && ancestor_calls < 2u);
    CHECK(guest_read32(g_esp) == (ancestor_calls == 0u ? 0x2FBF8Au : 0x2FBF93u));
    CHECK(g_eax == (ancestor_calls == 0u ? PROFILE : ACTOR));
    g_eax = ancestor_calls == 0u && first_null ? 0u : ACTOR;
    g_ecx = 0xEC111111u; g_edx = 0xED111111u;
    ancestor_calls++; g_esp += 4u;
}
static void identity(void)
{
    CHECK(g_esp == STACK - 8u && guest_read32(g_esp) == 0x2FBFC8u);
    CHECK(game_stack_arg(0u) == 0x40000u && g_eax == 0x40000u);
    CHECK(g_ecx == PROFILE && g_edx == 0u);
    g_eax = ACTOR; g_ecx = 0xEC222222u; g_edx = 0xED222222u;
    identity_calls++; g_esp += 4u;
}
static void mode(void)
{
    CHECK(g_esp == STACK - 4u && guest_read32(g_esp) == 0x2FBFE9u);
    CHECK(g_ecx == PROFILE);
    CHECK(g_eax == ((profile_flags & 0x80u) != 0u
        ? (first_null ? 0u : ACTOR) | profile_flags : ACTOR));
    g_eax = mode_value; g_ecx = 0xEC333333u; g_edx = 0xED333333u;
    mode_calls++; g_esp += 4u;
}
game_guest_function recomp_lookup(uint32_t va)
{
    CHECK(va == 0x774B0u || va == 0x77470u || va == 0x314D60u);
    return va == 0x774B0u ? ancestor : va == 0x77470u ? identity : va == 0x314D60u ? mode : NULL;
}
static void reset(void)
{
    memset(memory, 0, sizeof(memory)); ancestor_calls = identity_calls = mode_calls = 0u;
    g_esp = STACK; g_ecx = 0x11111111u; g_edx = 0x22222222u;
    guest_write32(STACK, 0x10101010u);
    guest_write32(0x7844A8u, PROFILE);
    guest_write32(PROFILE + 0x25Cu, profile_flags);
    guest_write32(0x7BA26Cu, 0x40000u);
    guest_write32(0x7842A8u, ACTOR);
    guest_write32(PROFILE + 0x2F34u, 0xFFFFFFFFu);
    write_word(PROFILE + 0x2F10u, 0xFFFFu);
    guest_write32(PROFILE + 0x1185Cu, 0xFFFFFFFFu);
}
int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    const uint32_t flags[] = {0u, 0x80u, 0x20u, 8u};
    for (unsigned f = 0u; f < 4u; f++)
        for (first_null = 0u; first_null < 2u; first_null++)
            for (unsigned actor_flag = 0u; actor_flag < 2u; actor_flag++)
                for (mode_value = 0u; mode_value < 2u; mode_value++)
                    for (unsigned equal = 0u; equal < 2u; equal++) {
                        profile_flags = flags[f]; reset();
                        guest_write8(ACTOR + 0x218u, actor_flag ? 4u : 0u);
                        guest_write32(0x7842A8u, equal ? ACTOR : ACTOR + 4u);
                        sub_002FBF70();
                        const unsigned early = f == 3u || (!first_null && actor_flag);
                        const unsigned identity_ok = f == 1u || equal;
                        CHECK(g_eax == (!early && f != 2u && identity_ok && mode_value == 0u ? 1u : 0u));
                        CHECK(ancestor_calls == (f == 3u ? 0u : first_null ? 1u : 2u));
                        CHECK(identity_calls == (!early && f != 1u ? 1u : 0u));
                        CHECK(mode_calls == (!early && f != 2u && identity_ok ? 1u : 0u));
                        CHECK(g_esp == STACK + 4u && guest_read32(STACK) == 0x10101010u);
                    }
    for (unsigned sign = 0u; sign < 2u; sign++)
        for (unsigned broken = 0u; broken < 5u; broken++) {
            profile_flags = sign ? 0x80u : 0u; first_null = 0u; mode_value = 0u; reset();
            if (broken == 0u) guest_write32(PROFILE + 0x2F34u, 0u);
            if (broken == 1u) write_word(PROFILE + 0x2F10u, 0u);
            if (broken == 2u) guest_write32(PROFILE + 0x2F7Cu, 1u);
            if (broken == 3u) guest_write32(PROFILE + 0x1185Cu, 0u);
            sub_002FBF70();
            CHECK(g_eax == (broken == 4u ? 1u : 0u));
            CHECK(g_ecx == 0xFFFFFFFFu);
            CHECK(g_edx == (broken == 0u || broken == 3u || broken == 4u ? 0u
                : broken == 1u ? 0xFFFFFFFFu : 1u));
            CHECK(g_esp == STACK + 4u);
        }
    printf("T1479 profile guard ordinary controls: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
