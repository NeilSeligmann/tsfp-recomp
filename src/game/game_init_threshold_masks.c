/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"

static uint32_t game_init_threshold_masks(guest_addr destination, uint32_t first,
                                          uint32_t second, uint32_t third)
{
    guest_write32(destination + 4u, second);
    guest_write32(destination, first);
    guest_write32(destination + 8u, third);
    guest_write32(destination + 0x0Cu, 0u);
    guest_write32(destination + 0x10u, 0u);
    guest_write32(destination + 0x14u, 0u);
    guest_write32(destination + 0x18u, 0u);
    guest_write32(destination + 0x1Cu, 0u);
    guest_write32(destination + 0x20u, 0u);

    uint32_t index = 1u;
    uint32_t bit = 1u;
    for (;;) {
        uint32_t next_bit = 0u;
        if (index < first) {
            guest_write32(destination + 0x0Cu,
                          guest_read32(destination + 0x0Cu) | bit);
            bit <<= 1;
            next_bit = bit;
        }
        if (index < guest_read32(g_esp + 8u)) {
            guest_write32(destination + 0x10u,
                          guest_read32(destination + 0x10u) | bit);
            bit <<= 1;
            next_bit = bit;
        }
        if (index < guest_read32(g_esp + 12u)) {
            guest_write32(destination + 0x14u,
                          guest_read32(destination + 0x14u) | bit);
            bit <<= 1;
            next_bit = bit;
        }

        index <<= 1;
        if (next_bit == 0u) {
            break;
        }
    }
    return bit;
}

GAME_REPLACE(0002A200, thiscall, 3, u32, game_init_threshold_masks)
