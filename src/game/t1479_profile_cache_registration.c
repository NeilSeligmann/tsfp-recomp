#include "game_replace.h"
void t1479_candidate_2fe520(void);
void t1479_candidate_350dc0(void);
GAME_REPLACE_EXACT(002FE520, cdecl, 1, u32, t1479_profile_key_present) { t1479_candidate_2fe520(); }
GAME_REPLACE_EXACT(00350DC0, cdecl, 1, u32, t1479_cached_option_value) { t1479_candidate_350dc0(); }
