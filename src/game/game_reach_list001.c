/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1583: hand C drafts for the 13 roots of docs/data/t1578-newly-reachable-lists/list-001.json.
 * Only the 3 roots that passed the batch gate are registered here (002CBCE0, 00386155,
 * 003367A0). The exact file proved with all 13 drafts is kept verbatim under
 * docs/data/t1583-reach-list001/proved-all-13-drafts.c.txt. Record: docs/t-reach-draft-list001.md.
 * Drafted from the retail disassembly of the pinned XBE (sha256 3cfd001a...1816bc). Every body
 * is register-exact: the guest stack is modelled with real memory (push, pop, locals) so saved
 * registers, spilled counters and pushed call arguments are written and read in the original
 * order, and callees stay live through game_guest_call. Names describe memory effects only.
 */
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

/* One nested guest call at the current (really modelled) ESP. The caller pushed any argument
 * that must stay live after the call by hand, the others are passed in `args`. */
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

/* The final `ret` reads its address from [ESP]. A body that stored over that slot returns to
 * garbage in the original, modelled as a trap. */
static void check_return_slot(uint32_t expected_return)
{
    if (guest_read32(g_esp) != expected_return)
        __builtin_trap();
}

/* rep stosd with the direction flag clear. */
static void rep_stosd(void)
{
    while (g_ecx != 0u) {
        guest_write32(g_edi, g_eax);
        g_edi += 4u;
        g_ecx--;
    }
}

/* 0x002CBCE0: compares 0x1F dwords of the block 0x00069620(dword [0x0078AD34]) with the block
 * at 0x007BA364 + dword [0x007BA9E0] * 0x1E0 (repe cmpsd). AL = 1 when they differ. */
GAME_REPLACE_EXACT(002CBCE0, cdecl, 0, u32, game_player_block_differs_from_saved_profile_entry_full_compare)
{
    int equal;
    g_eax = guest_read32(0x0078AD34u);
    push32(g_esi);
    push32(g_edi);
    const uint32_t args[] = {g_eax};
    call(0x00069620u, GAME_CC_cdecl, 0x002CBCEDu, args, 1u);
    g_edi = guest_read32(0x007BA9E0u);
    g_edi *= 0x1E0u;
    g_edi += 0x007BA364u;
    g_esi = g_eax;
    g_edx = 0u;
    g_ecx = 0x1Fu;
    do {
        const uint32_t left = guest_read32(g_esi);
        const uint32_t right = guest_read32(g_edi);
        g_esi += 4u;
        g_edi += 4u;
        g_ecx--;
        equal = left == right;
    } while (g_ecx != 0u && equal);
    g_eax = g_edx;
    g_edi = pop32();
    g_eax = (g_eax & 0xFFFFFF00u) | (equal ? 0u : 1u);
    g_esi = pop32();
}

/* 0x00386155: ECX is the object. Over the stream list ([obj+0x20], count [obj+0x14]) it first
 * releases up to [obj+0x1C] active entries (entry +0x14 nonzero) through 0x0038608A(entry),
 * zeroing the 0x50 dword state of idle ones, then zeroes the state of every remaining entry. */
GAME_REPLACE_EXACT_INPUTS(00386155, cdecl, 0, u32, ecx, game_audio_iface_stream_list_release_active_or_zero_idle_state)
{
    const uint32_t return_address = guest_read32(g_esp);
    push32(g_ebx);
    push32(g_ebp);
    push32(g_esi);
    g_esi = g_ecx;
    g_ebp = guest_read32(g_esi + 0x1Cu);
    g_ebx = 0u;
    const int has_entries = g_ebx < guest_read32(g_esi + 0x14u);
    push32(g_edi);
    if (!has_entries)
        goto done;
    for (;;) {
        if (g_ebp == 0u)
            break;
        g_eax = guest_read32(g_esi + 0x20u);
        g_edx = guest_read32(g_eax + g_ebx * 4u);
        if (guest_read32(g_edx + 0x14u) != 0u) {
            g_ecx = g_esi;
            const uint32_t args[] = {g_edx};
            call(0x0038608Au, GAME_CC_stdcall, 0x0038617Du, args, 1u);
            g_ebp--;
        } else {
            g_edi = guest_read32(g_edx + 0x24u);
            push32(0x50u);
            g_eax = 0u;
            g_ecx = pop32();
            rep_stosd();
            guest_write32(g_edx + 0x4Cu, g_eax);
        }
        g_ebx++;
        if (!(g_ebx < guest_read32(g_esi + 0x14u)))
            break;
    }
    while (g_ebx < guest_read32(g_esi + 0x14u)) {
        g_eax = guest_read32(g_esi + 0x20u);
        g_edx = guest_read32(g_eax + g_ebx * 4u);
        g_edi = guest_read32(g_edx + 0x24u);
        push32(0x50u);
        g_eax = 0u;
        g_ecx = pop32();
        rep_stosd();
        guest_write32(g_edx + 0x4Cu, g_eax);
        g_ebx++;
    }
done:
    g_edi = pop32();
    g_esi = pop32();
    g_ebp = pop32();
    g_ebx = pop32();
    check_return_slot(return_address);
}

/* 0x003367A0: ECX unused (saved), ESI is the output array of (start, length) dword pairs
 * (-1 start means empty). Arg 0 and arg 1 are two byte buffers, arg 2 and arg 3 their
 * sizes. Records each run of differing bytes (a run is also split every 0x20 bytes) over
 * the common length, then a final range for the longer buffer's tail. Returns the number
 * of ranges. */
GAME_REPLACE_EXACT_INPUTS(003367A0, cdecl, 4, u32, ecx_esi, game_mapedit_compute_changed_byte_ranges_between_buffers)
{
    const uint32_t return_address = guest_read32(g_esp);
    push32(g_ecx);
    push32(g_ebx);
    g_ebx = guest_read32(g_esp + 0x14u);
    push32(g_edi);
    g_edi = guest_read32(g_esp + 0x1Cu);
    g_ecx = 0u;
    g_eax = 0u;
    g_edx = g_ebx;
    if ((int32_t)g_ebx >= (int32_t)g_edi)
        g_edx = g_edi;
    push32(g_ebp);
    g_ebp = 0u;
    if ((int32_t)g_edx <= 0)
        goto tail;
    if ((int32_t)g_ecx >= (int32_t)g_edx)
        goto next_outer;
scan_start:
    g_edi = guest_read32(g_esp + 0x18u);
    g_ebx = guest_read32(g_esp + 0x14u);
    g_edi += g_ecx;
    g_ebx -= guest_read32(g_esp + 0x18u);
    guest_write32(g_esp + 0xCu, g_ebx);
    goto compare;
reload:
    g_ebx = guest_read32(g_esp + 0xCu);
compare:
    {
        const uint32_t left = guest_read8(g_ebx + g_edi);
        g_ebx = (g_ebx & 0xFFFFFF00u) | left;
        const uint32_t right = guest_read8(g_edi);
        if (left == right && (int32_t)g_ebp >= 0x20)
            goto after_run;
    }
    if (guest_read32(g_esi + g_eax * 8u) == 0xFFFFFFFFu)
        guest_write32(g_esi + g_eax * 8u, g_ecx);
    g_ebx = guest_read32(g_esi + g_eax * 8u + 4u);
    g_ebx++;
    g_ecx++;
    g_edi++;
    g_ebp++;
    guest_write32(g_esi + g_eax * 8u + 4u, g_ebx);
    if ((int32_t)g_ecx < (int32_t)g_edx)
        goto reload;
after_run:
    g_ebx = guest_read32(g_esp + 0x1Cu);
    g_edi = guest_read32(g_esp + 0x20u);
next_outer:
    if (guest_read32(g_esi + g_eax * 8u) != 0xFFFFFFFFu)
        g_eax++;
    g_ecx++;
    g_ebp = 0u;
    if ((int32_t)g_ecx < (int32_t)g_edx)
        goto scan_start;
tail:
    {
        const int edi_greater = (int32_t)g_edi > (int32_t)g_ebx;
        g_ebp = pop32();
        if (edi_greater) {
            g_edi -= g_ebx;
            guest_write32(g_esi + g_eax * 8u + 4u, g_edi);
            guest_write32(g_esi + g_eax * 8u, g_ebx);
            g_edi = pop32();
            g_eax++;
            g_ebx = pop32();
            g_ecx = pop32();
            check_return_slot(return_address);
            return;
        }
    }
    if ((int32_t)g_ebx > (int32_t)g_edi) {
        g_ebx -= g_edi;
        guest_write32(g_esi + g_eax * 8u, g_edi);
        guest_write32(g_esi + g_eax * 8u + 4u, g_ebx);
        g_eax++;
    }
    g_edi = pop32();
    g_ebx = pop32();
    g_ecx = pop32();
    check_return_slot(return_address);
}
