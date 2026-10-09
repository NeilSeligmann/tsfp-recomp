/* SPDX-License-Identifier: GPL-3.0-or-later
 * Original 0x0003E440: aligned allocation from the title's block ledger.
 * Reuse a released row only when its size exactly matches the aligned request;
 * otherwise append a new row and advance the recorded byte total.
 */
#include "game_replace.h"

extern __thread uint32_t g_ebp, g_ebx, g_esi, g_edi;

#define LEDGER_ROWS 0x006B7B68u
#define LEDGER_BASE 0x006B836Cu
#define LEDGER_BYTES 0x006B8394u
#define LEDGER_COUNT 0x006B8398u
#define LEDGER_ENABLED 0x004C81B4u

static void save_guest_word(uint32_t value)
{
    g_esp -= 4u;
    guest_write32(g_esp, value);
}

static uint32_t restore_guest_word(void)
{
    uint32_t value = guest_read32(g_esp);
    g_esp += 4u;
    return value;
}

static uint32_t game_allocate_ledger(uint32_t requested_bytes)
{
    uint32_t initial_bytes = guest_read32(LEDGER_BYTES);
    uint32_t enabled = guest_read32(LEDGER_ENABLED);
    save_guest_word(g_ebp);
    uint32_t base = guest_read32(LEDGER_BASE);
    uint32_t size = (requested_bytes + 15u) & UINT32_C(0xfffffff0);
    uint32_t appended_block = initial_bytes + base;
    if (enabled == 0u) {
        g_ebp = restore_guest_word();
        return 0u;
    }

    save_guest_word(g_ebx);
    uint32_t count = guest_read32(LEDGER_COUNT);
    save_guest_word(g_esi);
    save_guest_word(g_edi);
    uint32_t last = count - 1u;
    uint32_t offset = 0u;
    if (last != 0u && (last & UINT32_C(0x80000000)) == 0u) {
        for (uint32_t row = 0u; row < last; ++row) {
            uint32_t address = LEDGER_ROWS + row * 8u;
            if (guest_read32(address) == 0u && guest_read32(address + 4u) == size) {
                g_edi = restore_guest_word();
                uint32_t block = offset + base;
                g_esi = restore_guest_word();
                g_ebx = restore_guest_word();
                /* Keep even the redundant size write, and its ordering before
                 * the pointer publication and final saved-EBP read. */
                guest_write32(address + 4u, size);
                guest_write32(address, block);
                g_ebp = restore_guest_word();
                return block;
            }
            offset += guest_read32(address + 4u);
        }
    }

    /* Reload the recorded total after the scan, as in the original. The newest
     * existing row is excluded from exact-size reuse. There is no capacity test. */
    uint32_t next_bytes = guest_read32(LEDGER_BYTES) + size;
    guest_write32(LEDGER_ROWS + count * 8u + 4u, size);
    g_edi = restore_guest_word();
    guest_write32(LEDGER_ROWS + count * 8u, appended_block);
    guest_write32(LEDGER_BYTES, next_bytes);
    g_esi = restore_guest_word();
    guest_write32(LEDGER_COUNT, count + 1u);
    g_ebx = restore_guest_word();
    g_ebp = restore_guest_word();
    return appended_block;
}

/* Current caller audit proves ECX/EDX dead at all21direct sites. Preserved
 * registers, guest saved-frame writes and EAX/ESP remain part of the contract. */
GAME_REPLACE(0003E440, cdecl, 1, u32, game_allocate_ledger)
