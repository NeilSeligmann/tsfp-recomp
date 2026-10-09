/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1510 (3CAD8F): the CRT locale-aware character fold, an integer-only root whose only
 * callee (3CF514, a word-table lookup) is inlined. Contract and evidence:
 * docs/replace-x87-t1510.md, section "3CAD8F".
 */
#include "game_replace.h"

#define TOWLOWER_LOCALE_FLAG 0x772A34u
#define TOWLOWER_TABLE_POINTER 0x54D7F4u

/* The original's lookup (3CF514) reads a 16-bit table word whose value is then discarded by the
 * caller, but the read itself can fault, so it is performed through volatile bytes. */
static void locale_table_word_touch(uint32_t table, uint32_t index)
{
    volatile const uint8_t *word = (volatile const uint8_t *)game_host_ptr(table + index * 2u);
    (void)word[0];
    (void)word[1];
}

/* 003CAD8F: cdecl(wchar). Only the low 16 bits of the argument are examined.
 * 0xFFFF returns 0xFFFF. With the locale flag [772A34] clear, 'A'..'Z' (0x41..0x5A) gain 0x20
 * and everything else is returned zero-extended. With the flag set, a character below 0x100
 * goes through the table lookup of 3CF514 (reads [54D7F4] and the word at table + 2*c, whose
 * result the original overwrites) and leaves ECX = 1 from the callee's argument pops; either
 * way the result is the zero-extended character. EDX is never written. */
GAME_REPLACE_EXACT(003CAD8F, cdecl, 1, u32, game_wchar_fold_ascii_upper_or_locale_table_touch)
{
    const uint32_t character = game_stack_arg(0u) & 0xFFFFu;
    if (character == 0xFFFFu) {
        g_eax = 0xFFFFu;
        return;
    }
    if (guest_read32(TOWLOWER_LOCALE_FLAG) == 0u) {
        g_eax = character + ((character >= 0x41u && character <= 0x5Au) ? 0x20u : 0u);
        return;
    }
    if (character < 0x100u) {
        locale_table_word_touch(guest_read32(TOWLOWER_TABLE_POINTER), character);
        g_ecx = 1u;
    }
    g_eax = character;
}
