/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1788 default-domain admissions. Fixed proof receipts: docs/t-prove-campaign-gameplay.md.
 * GPR-exact, no scratch exclusions; original load and register write order. */
#include "game_replace.h"
GAME_REPLACE_EXACT(000C9BB0,cdecl,0,u32,game_camera_733108_present) {
    g_ecx=guest_read32(0x733108); g_eax=g_ecx!=0;
}
GAME_REPLACE_EXACT(001B58C0,cdecl,1,u32,game_weapon_entry_flag_80) {
    g_eax=guest_read32(g_esp+4); g_ecx=guest_read32(g_eax+0x30); g_edx=guest_read32(0x4F9BAC);
    g_ecx*=0x29C; g_eax=guest_read32(g_ecx+g_edx+0xC)&0x80;
}
