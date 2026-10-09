/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
void t1479_candidate_3158c0(void);
void t1479_candidate_2fbf70(void);
GAME_REPLACE_EXACT(003158C0, cdecl, 0, u32, t1479_actor_status) { t1479_candidate_3158c0(); }
GAME_REPLACE_EXACT(002FBF70, cdecl, 0, u32, t1479_profile_guard) { t1479_candidate_2fbf70(); }
