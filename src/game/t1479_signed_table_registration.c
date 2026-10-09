/* SPDX-License-Identifier: GPL-3.0-or-later
 * Validated original/default and namespace79 fixture contracts in docs/evidence/t1479/.
 */
#include "game_replace.h"
void t1479_candidate_2d2c10(void);
void t1479_candidate_2d2c40(void);
void t1479_candidate_338150(void);
GAME_REPLACE_EXACT(002D2C10, cdecl, 0, u32, t1479_unsigned_profile_value) { t1479_candidate_2d2c10(); }
GAME_REPLACE_EXACT(002D2C40, cdecl, 0, u32, t1479_signed_profile_value) { t1479_candidate_2d2c40(); }
GAME_REPLACE_EXACT(00338150, cdecl, 1, u32, t1479_clear_indexed_slot) { t1479_candidate_338150(); }
