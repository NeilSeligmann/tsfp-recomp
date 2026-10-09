/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Small call-free cdecl leaves of retail XBE range 0x00150000-0x002C0000 (T77).
 * Title-level purposes are unknown, so the names describe only the measured behavior.
 * See docs/t77-range-150000-2c0000-leaves.md for the proof and the rejected candidates.
 */
#include "game_replace.h"

/* 0x00156E70: span of a [begin,end) pair, `[p+0xC] - [p+8]` modulo 2^32. */
static uint32_t game_pair_span(uint32_t record)
{
    return guest_read32(record + 0xCu) - guest_read32(record + 8u);
}
GAME_REPLACE(00156E70, cdecl, 1, u32, game_pair_span)

/* 0x00177910: address of an embedded sub-record (offset 0x5B0) inside the object that
 * `[p+0x7C]` points at. */
static uint32_t game_subrecord_5b0(uint32_t object)
{
    return guest_read32(object + 0x7Cu) + 0x5B0u;
}
GAME_REPLACE(00177910, cdecl, 1, u32, game_subrecord_5b0)
