/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The EFLAGS an x86 `cmp left, right` leaves, as the flag bridge publishes them (T554).
 * Header only so the oracle test can compile exactly this function and judge it against the
 * original's real EFLAGS under Unicorn (tests/test_flag_bridge_oracle.py).
 */
#ifndef TSFP_GAME_X87_FLAGS_H
#define TSFP_GAME_X87_FLAGS_H

#include <stdint.h>

/* Bits the lifter's bridge can read: CF 0x001, PF 0x004, ZF 0x040, SF 0x080, OF 0x800. */
#define FLAG_BRIDGE_CF 0x001u
#define FLAG_BRIDGE_PF 0x004u
#define FLAG_BRIDGE_ZF 0x040u
#define FLAG_BRIDGE_SF 0x080u
#define FLAG_BRIDGE_OF 0x800u
#define FLAG_BRIDGE_ALL (FLAG_BRIDGE_CF | FLAG_BRIDGE_PF | FLAG_BRIDGE_ZF | FLAG_BRIDGE_SF | FLAG_BRIDGE_OF)

static inline uint32_t x87_cmp_flags(uint32_t left, uint32_t right)
{
    uint32_t difference = left - right;
    uint32_t low = difference & 0xFFu;
    low ^= low >> 4;
    low ^= low >> 2;
    low ^= low >> 1;
    uint32_t flags = 0u;
    flags |= left < right ? FLAG_BRIDGE_CF : 0u;
    flags |= (low & 1u) == 0u ? FLAG_BRIDGE_PF : 0u;
    flags |= difference == 0u ? FLAG_BRIDGE_ZF : 0u;
    flags |= (difference >> 31) != 0u ? FLAG_BRIDGE_SF : 0u;
    flags |= (((left ^ right) & (left ^ difference)) >> 31) != 0u ? FLAG_BRIDGE_OF : 0u;
    return flags;
}

#endif /* TSFP_GAME_X87_FLAGS_H */
