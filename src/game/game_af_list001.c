/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1588: hand C drafts for the 16 roots of docs/data/t1587-af-kill-lists/list-001.json (the T1585
 * AF-kill newly caller-eligible roots). Drafted from the retail disassembly of the pinned XBE
 * (sha256 3cfd001a...). Every body is register-exact: pushes and pops of saved registers and
 * locals go through real guest memory, callees stay live through game_guest_call (their leftovers
 * in EAX/ECX/EDX stay visible), and registers are assigned in the original order. Names describe
 * memory effects only. Record: docs/t-af-draft-list001.md. */
#include "game_replace.h"
extern __thread uint32_t g_ebx, g_ebp, g_esi, g_edi;

static void push32(uint32_t value)
{
    g_esp -= 4u;
    guest_write32(g_esp, value);
}

static uint32_t pop32(void)
{
    const uint32_t value = guest_read32(g_esp);
    g_esp += 4u;
    return value;
}

/* One nested guest call at the current (really modelled) ESP. Arguments are placed by the helper. */
static void call(uint32_t target, game_convention convention, uint32_t return_va,
                 const uint32_t *args, unsigned count)
{
    const uint32_t return_slot = g_esp - 4u * (count + 1u);
    if (game_guest_call(target, convention, return_va, 0u, g_ecx, 0u, args, count) !=
        GAME_GUEST_CALL_OK)
        __builtin_trap();
    if (guest_read32(return_slot) != return_va)
        __builtin_trap();
}

/* 0x003ACB50: stdcall(a, b), ECX = object. [obj+0x28] = a, [obj+0x2C] = b. Returns a, EDX = b. */
GAME_REPLACE_EXACT_INPUTS(003ACB50, stdcall, 2, u32, ecx, game_store_pair_at_offset_0x28)
{
    g_eax = game_stack_arg(0u);
    g_edx = game_stack_arg(1u);
    guest_write32(g_ecx + 0x28u, g_eax);
    guest_write32(g_ecx + 0x2Cu, g_edx);
}

/* 0x003BEAC0: ECX = object. Zeroes [obj+4], [obj+0xC], [obj+8]. Returns 0. */
GAME_REPLACE_EXACT_INPUTS(003BEAC0, cdecl, 0, u32, ecx, game_zero_three_fields)
{
    g_eax = 0u;
    guest_write32(g_ecx + 4u, g_eax);
    guest_write32(g_ecx + 0xCu, g_eax);
    guest_write32(g_ecx + 8u, g_eax);
}

/* 0x00386FCF: ECX = object. Returns [[obj+0x20]+0xC], or 0 when [obj+0x20] is 0. */
GAME_REPLACE_EXACT_INPUTS(00386FCF, cdecl, 0, u32, ecx, game_field_0xc_of_pointer_at_0x20_or_zero)
{
    g_eax = guest_read32(g_ecx + 0x20u);
    if (g_eax == 0u)
        return;
    g_eax = guest_read32(g_eax + 0xCu);
}

/* 0x0035E670: 0x0035E960() decides. Returns [0x76BBBC] when it returned non-zero, else 0x10. */
GAME_REPLACE_EXACT(0035E670, cdecl, 0, u32, game_global_76bbbc_or_16_by_predicate)
{
    call(0x0035E960u, GAME_CC_cdecl, 0x0035E675u, NULL, 0u);
    const uint32_t predicate = g_eax;
    g_eax = guest_read32(0x0076BBBCu);
    if (predicate == 0u)
        g_eax = 0x10u;
}

/* 0x003BFBE0: stdcall(value), ECX = object. [obj+4] = value, [obj] = vtable 0x4B6374, [obj+8],
 * [obj+0xC], [obj+0x10] = 0. Returns the object, ECX = 0. */
GAME_REPLACE_EXACT_INPUTS(003BFBE0, stdcall, 1, u32, ecx, game_construct_vtable_4b6374_with_value)
{
    g_eax = g_ecx;
    g_ecx = game_stack_arg(0u);
    guest_write32(g_eax + 4u, g_ecx);
    g_ecx = 0u;
    guest_write32(g_eax, 0x004B6374u);
    guest_write32(g_eax + 8u, g_ecx);
    guest_write32(g_eax + 0xCu, g_ecx);
    guest_write32(g_eax + 0x10u, g_ecx);
}

/* 0x003B3120: stdcall(a, b), ECX = table object. Scans the 0xAC byte records from obj+0x60 up to
 * obj+0x310 for one with a non-zero [rec+8], [rec+0x24] == a and [rec+0x20] == b. Returns its
 * address or 0. ESI and EDI are saved and restored. */
GAME_REPLACE_EXACT_INPUTS(003B3120, stdcall, 2, u32, ecx, game_find_record_by_key_pair_at_0x20_0x24)
{
    g_eax = g_ecx + 0x60u;
    g_ecx += 0x310u;
    push32(g_esi);
    push32(g_edi);
    if (g_eax < g_ecx) {
        g_edx = guest_read32(g_esp + 0x10u);
        g_esi = guest_read32(g_esp + 0xCu);
        for (;;) {
            g_edi = guest_read32(g_eax + 8u);
            if (g_edi != 0u && guest_read32(g_eax + 0x24u) == g_esi &&
                guest_read32(g_eax + 0x20u) == g_edx)
                goto done;
            g_eax += 0xACu;
            if (!(g_eax < g_ecx))
                break;
        }
    }
    g_eax = 0u;
done:
    g_edi = pop32();
    g_esi = pop32();
}

/* 0x003C44C0: ECX = object. Clears the fields of a 0x178 byte record. Returns 0, EDX = 0. */
GAME_REPLACE_EXACT_INPUTS(003C44C0, cdecl, 0, u32, ecx, game_clear_record_fields)
{
    g_edx = 0u;
    g_eax = 0u;
    guest_write32(g_ecx + 8u, g_eax);
    guest_write32(g_ecx + 4u, g_eax);
    guest_write32(g_ecx + 0xCu, g_eax);
    guest_write32(g_ecx + 0x10u, g_eax);
    guest_write32(g_ecx + 0x14u, g_eax);
    guest_write32(g_ecx + 0x58u, g_eax);
    guest_write32(g_ecx + 0x5Cu, g_eax);
    guest_write32(g_ecx + 0x64u, g_eax);
    guest_write8(g_ecx + 0x168u, (uint8_t)g_eax);
    guest_write32(g_ecx + 0x16Cu, g_eax);
    guest_write32(g_ecx + 0x170u, g_eax);
    for (uint32_t offset = 0x18u; offset <= 0x54u; offset += 4u)
        guest_write32(g_ecx + offset, g_edx);
    guest_write8(g_ecx + 0x174u, (uint8_t)g_eax);
}
