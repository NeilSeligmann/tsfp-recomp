/* SPDX-License-Identifier: GPL-3.0-or-later
 * The measured integer transform at 0x004734CC.
 */
#include "game_replace.h"

static void game_state_word_transform(guest_addr object)
{
    guest_addr source = guest_read32(object + 8u);
    uint32_t state = guest_read32(object + 0x418u);
    uint32_t word = (uint32_t)guest_read8(source + 0x80u) |
                    ((uint32_t)guest_read8(source + 0x81u) << 8);
    uint32_t result = (word ^ state) & 0x8000u;
    uint32_t combined = (word & 0x7FFFu) | state;

    g_eax = result + combined;
    g_ecx = combined;
    g_edx = state;
}

/* Original register effects: EAX=(low16([([ECX+8])+0x80]) XOR [ECX+0x418])&0x8000,
 * then add that bit to ((low16(...)&0x7fff)|[ECX+0x418]); ECX is the OR result,
 * EDX is [ECX+0x418], and ret leaves the return-address pop to the caller.
 */
GAME_REPLACE_EXACT(004734CC, thiscall, 0, u32, game_state_word_transform)
{
    guest_addr object = g_ecx;
    game_state_word_transform(object);
}
