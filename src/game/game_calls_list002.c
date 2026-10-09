/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1541 (T1534 list-002): hand C for the 7 admitted roots of the 25-root list. The other 18
 * drafts failed the gates and are kept only under docs/data/t1541-list002.
 * Drafted from the retail disassembly of the pinned XBE (sha256 3cfd001a...1816bc), one body
 * per root. Every body is register-exact: it reproduces the original guest register outputs,
 * the saved-register stack slots, the pushed call arguments and the memory access order. Names
 * describe visible memory effects only. Calls to other guest functions go through
 * game_guest_call with the retail return VA and the live stack bytes of the original frame.
 * Proof record: docs/t-call-draft-list002.md.
 */
#include "game_replace.h"
extern __thread uint32_t g_ebx, g_ebp, g_esi, g_edi;

/* One nested guest call. `live` is the count of bytes the original already holds below the
 * entry ESP (saved registers and uncleaned earlier arguments). ECX is passed for thiscall.
 * The original `ret` reads its return address from memory, so a callee that overwrote its own
 * return slot sends the original to a garbage address. The helper reproduces that as a trap. */
static void call(uint32_t target, game_convention convention, uint32_t return_va, uint32_t live,
                 const uint32_t *args, unsigned count)
{
    const uint32_t return_slot = g_esp - live - 4u * (count + 1u);
    if (game_guest_call(target, convention, return_va, live, g_ecx, 0u, args, count) !=
        GAME_GUEST_CALL_OK)
        __builtin_trap();
    if (guest_read32(return_slot) != return_va)
        __builtin_trap();
}

/* The final `ret` of a body that stores through caller pointers reads [entry ESP]. When a
 * store overwrote that slot the original returns to the stored garbage, modelled as a trap. */
static void check_return_slot(uint32_t expected_return)
{
    if (guest_read32(g_esp) != expected_return)
        __builtin_trap();
}

/* 0x00025240: maps bit 3 and bits 30 and 2 of the argument to a selector, then returns the
 * stdcall 0x0037E89E result for it. */
GAME_REPLACE_EXACT(00025240, cdecl, 1, u32, game_l2_flags_to_selector_size)
{
    g_ecx = game_stack_arg(0u);
    g_eax = 0u;
    if ((g_ecx & 8u) != 0u)
        g_eax = 1u;
    if ((g_ecx & 0x40000000u) != 0u) {
        g_eax |= 4u;
        const uint32_t args[] = {g_eax};
        call(0x0037E89Eu, GAME_CC_stdcall, 0x00025261u, 0u, args, 1u);
        return;
    }
    if ((g_ecx & 4u) != 0u)
        g_eax |= 8u;
    const uint32_t args[] = {g_eax};
    call(0x0037E89Eu, GAME_CC_stdcall, 0x0002526Bu, 0u, args, 1u);
}

/* 0x001B5260: runs 0x00302A20, stores into the 0x0009E2C0 table twice more, registers one
 * 0x0009E2D0 entry and sets the 0x0009E380 global. The original keeps all arguments on its
 * stack until the final add, so each call sees the earlier arguments live below it. */
GAME_REPLACE_EXACT(001B5260, cdecl, 0, u32, game_l2_init_indexed_entries_and_stream)
{
    call(0x00302A20u, GAME_CC_cdecl, 0x001B5265u, 0u, NULL, 0u);
    const uint32_t first[] = {0u, 0x00242680u};
    call(0x0009E2C0u, GAME_CC_cdecl, 0x001B5271u, 0u, first, 2u);
    const uint32_t second[] = {1u, 0x00242BA0u};
    call(0x0009E2C0u, GAME_CC_cdecl, 0x001B527Du, 8u, second, 2u);
    const uint32_t third[] = {2u, 0x002426B0u};
    call(0x0009E2C0u, GAME_CC_cdecl, 0x001B5289u, 16u, third, 2u);
    const uint32_t fourth[] = {0x2000u, 0u, 0x002426D0u};
    call(0x0009E2D0u, GAME_CC_cdecl, 0x001B529Au, 24u, fourth, 3u);
    const uint32_t fifth[] = {0x0035DE60u};
    call(0x0009E380u, GAME_CC_cdecl, 0x001B52A4u, 36u, fifth, 1u);
}

/* 0x00195770: copies six dwords of the source (stack arg 0) to 0x0074FA78..0x0074FA8C, then
 * calls 0x003C8960(0x00752344, stack arg 1, 0x37). */
GAME_REPLACE_EXACT(00195770, cdecl, 2, u32, game_l2_copy_six_dwords_then_bounded_copy)
{
    g_eax = game_stack_arg(0u);
    g_ecx = guest_read32(g_eax);
    guest_write32(0x0074FA78u, g_ecx);
    g_edx = guest_read32(g_eax + 4u);
    guest_write32(0x0074FA7Cu, g_edx);
    g_ecx = guest_read32(g_eax + 8u);
    guest_write32(0x0074FA80u, g_ecx);
    g_edx = guest_read32(g_eax + 0xCu);
    guest_write32(0x0074FA84u, g_edx);
    g_ecx = guest_read32(g_eax + 0x10u);
    guest_write32(0x0074FA88u, g_ecx);
    g_edx = guest_read32(g_eax + 0x14u);
    g_eax = game_stack_arg(1u);
    guest_write32(0x0074FA8Cu, g_edx);
    const uint32_t args[] = {0x00752344u, g_eax, 0x37u};
    call(0x003C8960u, GAME_CC_cdecl, 0x001957BAu, 0u, args, 3u);
}

/* 0x0038864E: ECX is the object. Zeroes a fixed field set, stores 0xFFFF at +0x30 and 1 at
 * +0x98 and returns the object. */
GAME_REPLACE_EXACT_INPUTS(0038864E, cdecl, 0, u32, ecx, game_l2_reset_object_fields)
{
    const uint32_t return_address = guest_read32(g_esp);
    g_eax = g_ecx;
    g_ecx = 0u;
    g_edx = g_eax + 0x9Cu;
    guest_write32(g_eax, g_ecx);
    guest_write32(g_eax + 4u, g_ecx);
    guest_write32(g_eax + 0x14u, g_ecx);
    guest_write32(g_eax + 0x18u, g_ecx);
    guest_write32(g_eax + 0x1Cu, g_ecx);
    guest_write32(g_eax + 0x20u, g_ecx);
    guest_write32(g_eax + 0x24u, g_ecx);
    guest_write32(g_eax + 0x28u, g_ecx);
    guest_write32(g_eax + 0x2Cu, g_ecx);
    guest_write32(g_eax + 0x30u, 0xFFFFu);
    guest_write32(g_eax + 0x54u, g_ecx);
    guest_write32(g_eax + 0x58u, g_ecx);
    guest_write32(g_eax + 0x5Cu, g_ecx);
    guest_write32(g_eax + 0x98u, 1u);
    guest_write32(g_edx, g_ecx);
    guest_write32(g_edx + 4u, g_ecx);
    guest_write32(g_eax + 0xA4u, g_ecx);
    guest_write32(g_eax + 0xA8u, g_ecx);
    guest_write32(g_eax + 0x90u, g_ecx);
    guest_write32(g_eax + 0x94u, g_ecx);
    check_return_slot(return_address);
}

/* 0x00025BE0: sums the stdcall 0x0037E89E results for selectors 8 and 0, adds the argument
 * and converts to a count of 0x4000 units plus a fixed base (5 for a zero argument, which
 * also doubles the sum and adds 0x9F54, else 2), plus one when not an exact multiple. */
GAME_REPLACE_EXACT(00025BE0, cdecl, 1, u32, game_l2_block_count_for_size)
{
    guest_write32(g_esp - 4u, g_esi);
    const uint32_t eight[] = {8u};
    call(0x0037E89Eu, GAME_CC_stdcall, 0x00025BE8u, 4u, eight, 1u);
    g_esi = g_eax;
    const uint32_t zero[] = {0u};
    call(0x0037E89Eu, GAME_CC_stdcall, 0x00025BF1u, 4u, zero, 1u);
    g_edx = game_stack_arg(0u);
    g_ecx = g_eax + g_esi;
    if (g_edx == 0u) {
        g_ecx = g_ecx + g_ecx + 0x9F54u;
        g_eax = g_ecx;
        g_eax >>= 14u;
        g_eax += 5u;
    } else {
        g_ecx += g_edx;
        g_eax = g_ecx;
        g_eax >>= 14u;
        g_eax += 2u;
    }
    g_esi = guest_read32(g_esp - 4u);
    if ((g_ecx & 0x3FFFu) != 0u)
        g_eax++;
}

/* 0x003BA560: ECX is a signed year. Returns AL = 1 for a leap year (divisible by 4 and not
 * by 100 unless by 400). EAX upper bits, EDX and ECX follow the original idiv sequence. */
GAME_REPLACE_EXACT_INPUTS(003BA560, cdecl, 0, u32, ecx, game_l2_year_is_leap)
{
    g_eax = g_ecx;
    g_eax &= 0x80000003u;
    if ((int32_t)g_eax < 0) {
        g_eax--;
        g_eax |= 0xFFFFFFFCu;
        g_eax++;
    }
    if (g_eax == 0u) {
        g_eax = g_ecx;
        guest_write32(g_esp - 4u, g_esi);
        g_edx = (uint32_t)((int32_t)g_eax >> 31);
        g_esi = 100u;
        {
            const int32_t dividend = (int32_t)g_eax;
            g_eax = (uint32_t)(dividend / 100);
            g_edx = (uint32_t)(dividend % 100);
        }
        g_esi = guest_read32(g_esp - 4u);
        if (g_edx != 0u) {
            g_eax = (g_eax & 0xFFFFFF00u) | 1u;
            return;
        }
    }
    g_eax = g_ecx;
    g_edx = (uint32_t)((int32_t)g_eax >> 31);
    g_ecx = 0x190u;
    {
        const int32_t dividend = (int32_t)g_eax;
        g_eax = (uint32_t)(dividend / 400);
        g_edx = (uint32_t)(dividend % 400);
    }
    if (g_edx != 0u) {
        g_eax &= 0xFFFFFF00u;
        return;
    }
    g_eax = (g_eax & 0xFFFFFF00u) | 1u;
}

/* 0x000B3C10: ESI is the key. Scans 12 entries of stride 0x2C from 0x004DAA50 (four per loop
 * pass, loop base 0x004DAA7C) and returns the address of the LAST whose first dword equals the
 * key, else 0. */
GAME_REPLACE_EXACT_INPUTS(000B3C10, cdecl, 0, u32, esi, game_l2_last_entry_with_key)
{
    int equal;
    g_eax = 0u;
    g_ecx = 0x004DAA7Cu;
    guest_write32(g_esp - 4u, g_edi);
    do {
        g_edi = guest_read32(g_ecx - 0x2Cu);
        equal = g_edi == g_esi;
        g_edi = guest_read32(g_ecx + 0x2Cu);
        g_edx = g_ecx - 0x2Cu;
        if (equal)
            g_eax = g_edx;
        equal = guest_read32(g_ecx) == g_esi;
        g_edx = g_ecx + 0x2Cu;
        if (equal)
            g_eax = g_ecx;
        equal = g_edi == g_esi;
        g_edi = guest_read32(g_ecx + 0x58u);
        if (equal)
            g_eax = g_edx;
        g_edx = g_ecx + 0x58u;
        equal = g_edi == g_esi;
        g_edi = guest_read32(g_ecx + 0x84u);
        if (equal)
            g_eax = g_edx;
        g_edx = g_ecx + 0x84u;
        equal = g_edi == g_esi;
        if (equal)
            g_eax = g_edx;
        g_ecx += 0xDCu;
    } while ((int32_t)g_ecx < 0x004DAD10);
    g_edi = guest_read32(g_esp - 4u);
}
