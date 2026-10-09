/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
/* Original 00336840: ECX loads global; EAX is its nonnull predicate. */
GAME_REPLACE_EXACT(00336840, cdecl, 0, u32, game_mapedit_has_undo_node)
{ g_ecx = guest_read32(0x00765C28u); g_eax = (g_ecx != 0u); }
/* Original 003443F0: ECX loads global; EAX is its nonnull predicate. */
GAME_REPLACE_EXACT(003443F0, cdecl, 0, u32, game_render_is_weather_pass_flag_set)
{ g_ecx = guest_read32(0x00765E58u); g_eax = (g_ecx != 0u); }
/* Original 00352670: ECX loads global; EAX is its zero predicate. */
GAME_REPLACE_EXACT(00352670, cdecl, 0, u32, game_is_global_76b128_zero)
{ g_ecx = guest_read32(0x0076B128u); g_eax = (g_ecx == 0u); }
