/* SPDX-License-Identifier: GPL-3.0-or-later
 * Unregistered draft; native-profile-guard-predecl.md.
 */
#include "game_replace.h"
#include <string.h>

static uint16_t guard_read_word(uint32_t address)
{
    uint16_t value;
    memcpy(&value, game_host_ptr(address), sizeof value);
    return value;
}

static void guard_call(uint32_t target, uint32_t return_pc,
                       const uint32_t *args, unsigned count)
{
    if (game_guest_call(target, GAME_CC_cdecl, return_pc, 0u,
                        0u, 0u, args, count) != GAME_GUEST_CALL_OK)
        __builtin_trap();
}

void t1479_candidate_2fbf70(void)
{
    g_eax = guest_read32(0x7844A8u);
    if ((guest_read32(g_eax + 0x25Cu) & 0x16004008u) != 0u) goto fail;
    guard_call(0x774B0u, 0x2FBF8Au, NULL, 0u);
    if (g_eax != 0u) {
        guard_call(0x774B0u, 0x2FBF93u, NULL, 0u);
        if ((guest_read8(g_eax + 0x218u) & 4u) != 0u) goto fail;
    }
    g_ecx = guest_read32(0x7844A8u);
    g_eax = (g_eax & 0xFFFFFF00u) | guest_read8(g_ecx + 0x25Cu);
    if ((g_eax & 0x80u) == 0u) {
        g_edx = guest_read32(0x7BA9E0u) * 0x1E0u;
        g_eax = guest_read32(g_edx + 0x7BA26Cu);
        const uint32_t argument[1] = {g_eax};
        guard_call(0x77470u, 0x2FBFC8u, argument, 1u);
        g_ecx = guest_read32(0x7842A8u);
        if (g_eax != g_ecx) goto fail;
    }
    g_ecx = guest_read32(0x7844A8u);
    if ((guest_read8(g_ecx + 0x25Cu) & 0x20u) != 0u) goto fail;
    guard_call(0x314D60u, 0x2FBFE9u, NULL, 0u);
    if (g_eax != 0u) goto fail;
    g_eax = guest_read32(0x7844A8u);
    g_edx = guest_read32(g_eax + 0x2F34u);
    g_ecx = 0xFFFFFFFFu;
    if (g_edx != g_ecx) goto fail;
    if (guard_read_word(g_eax + 0x2F10u) != 0xFFFFu) goto fail;
    g_edx = guest_read32(g_eax + 0x2F7Cu);
    if (g_edx != 0u) goto fail;
    if (guest_read32(g_eax + 0x1185Cu) != g_ecx) goto fail;
    g_eax = 1u;
    return;
fail:
    g_eax = 0u;
}
