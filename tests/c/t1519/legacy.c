/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
static uint32_t legacy(uint32_t value) { return value + 1; }
GAME_REPLACE(00010070, cdecl, 1, u32, legacy)
GAME_REPLACE_EXACT(00010080, stdcall, 0, u32, legacy_exact) { g_eax = 42; }
