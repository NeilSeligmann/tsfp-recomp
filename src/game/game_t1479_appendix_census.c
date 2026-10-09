/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1479 appendix census: roots that earlier records named (docs/t1479-appendix-census.md).
 * Each body passed the fixed matrix (seeds 20261001/20261006, O0/O2/O3, 600 random cases,
 * verdicts >= 100, coverage >= 0.9, null-agree <= 0.9, 0 DISAGREE) and is the frozen draft
 * of docs/data/t1479-appendix-census/drafts/ except for the include, the shared XMM0 slot
 * and the SPDX line. Original guest reads, writes, register exits and saved-register
 * stack slots are preserved; see each header comment. */
#include "game_replace.h"

/* Shared XMM0 slot of the legacy vector bodies. The runtime owns the strong slot, the
 * weak definition lets registry-only links resolve it. */
typedef union T1479AppendixXmm {
    uint32_t u[4];
    uint64_t q[2];
} T1479AppendixXmm;
__thread T1479AppendixXmm g_xmm0 __attribute__((weak, aligned(8)));

/* T1479 appendix draft 0x002D2BE0 (plain, cdecl, 2 stack args, only arg1 is used, no push or pop).
 * Original behavior: builds a flag mask. EAX starts at 0 and becomes 0x20000 unless the byte at
 * 0x7DE455 equals 0x0A. Then ECX = arg1 << 5 (arg0 at [esp+4] is never read), EDX = the dword at
 * [0x4D1F88 + (arg1 << 5)] (32-bit wrapping address), and EAX |= 0x2000000 when that dword is zero.
 * Reads in order: byte [0x7DE455], dword [esp+8], dword [(arg1<<5) + 0x4D1F88]. No guest writes.
 * Register exit contract (single path): EAX = flag mask, ECX = arg1 << 5 (the whole register
 * is replaced, the entry ECX is dead), EDX = the dword read, EBX/ESI/EDI/EBP untouched, ESP
 * unchanged, plain `ret` (caller pops both arguments). The original has no PUSH or POP. */

GAME_REPLACE_EXACT(002D2BE0, cdecl, 2, u32,
                   game_menu_flag_mask_mode_not_0xa_and_entry_4d1f88_stride_20_zero)
{
    const uint8_t mode = guest_read8(0x007DE455u);
    g_eax = (mode == 0x0Au) ? 0u : 0x00020000u;
    g_ecx = guest_read32(g_esp + 8u) << 5;
    g_edx = guest_read32(g_ecx + 0x004D1F88u);
    if (g_edx == 0u) {
        g_eax |= 0x02000000u;
    }
}

/* T1479 appendix draft 0x0033EBF0 (plain, cdecl, 2 stack args, no register input).
 * Arguments: dest, source. Copies a counted list of 16-bit words from a packed source list into a
 * stride-8 destination list. The source starts with a count byte at +0 and its words sit at
 * source+2+4*i; the destination gets the count as a dword at +0 and the words at dest+4+8*i.
 * Original behavior, in order: dest is read from the stack, ESI is pushed, source is read from
 * the stack (so at esp+0xC after the push), ECX = zero-extended byte [source], [dest] = ECX (dword
 * store of the count), then the count byte is RE-READ into CL (so a dword store that overlapped
 * the source can change it). EDX = 0. If CL == 0 (test cl,cl / jbe is the zero test) the function
 * ends with ESI popped. Otherwise ECX = dest+4, EAX = source+2, EDI is pushed and a do-while loop
 * runs: word [ECX] = word [EAX], EDI = zero-extended byte [source] (RE-READ every iteration, so a
 * word store that lands on the source count byte changes the loop bound), EDX++, EAX += 4,
 * ECX += 8, repeat while EDX < EDI (signed jl). Then EDI and ESI are popped.
 * Guest globals touched: none. Guest writes in order: ESI save, dword [dest] (count), (nonzero count
 * only) EDI save and the 16-bit stores. Both saves are real guest stack pushes and the pops read
 * the slots back AFTER the function's own stores, so a store that lands on a save slot changes the
 * restored register (the T1479 POP-alias lesson). The EDI save is written after the count store.
 * Register exit contract: zero count (as re-read): EAX = dest, ECX = the re-read byte (0), EDX = 0.
 * Non-zero count: EAX = source+2+4*n, ECX = dest+4+8*n, EDX = n where n is the number of executed
 * iterations (at least 1). ESI is popped from the slot at entry esp-4 and EDI (loop path only)
 * from the slot at entry esp-8, EBX and EBP are untouched, ESP returns to entry. */

extern __thread uint32_t g_edi;

static uint32_t counted_words_read_u16(uint32_t address)
{
    uint16_t value;
    memcpy(&value, game_host_ptr(address), sizeof value);
    return value;
}

static void counted_words_write_u16(uint32_t address, uint32_t value)
{
    const uint16_t word = (uint16_t)value;
    memcpy(game_host_ptr(address), &word, sizeof word);
}

static void counted_words_push(uint32_t value)
{
    guest_write32(g_esp - 4u, value);
    g_esp -= 4u;
}

static uint32_t counted_words_pop(void)
{
    const uint32_t value = guest_read32(g_esp);
    g_esp += 4u;
    return value;
}

GAME_REPLACE_EXACT(0033EBF0, cdecl, 2, u32, game_copy_counted_u16_list_to_stride8_list)
{
    g_eax = game_stack_arg(0u);                 /* dest */
    counted_words_push(g_esi);
    g_esi = guest_read32(g_esp + 0xCu);         /* source (second stack argument) */
    g_ecx = guest_read8(g_esi);                 /* movzx ecx, byte [source] */
    guest_write32(g_eax, g_ecx);                /* count dword */
    g_ecx = (g_ecx & 0xFFFFFF00u) | guest_read8(g_esi); /* mov cl, [source]: re-read after the store */
    g_edx = 0u;
    if ((uint8_t)g_ecx != 0u) {
        g_ecx = g_eax + 4u;
        g_eax = g_esi + 2u;
        counted_words_push(g_edi);
        do {
            counted_words_write_u16(g_ecx, counted_words_read_u16(g_eax));
            g_edi = guest_read8(g_esi);         /* count byte re-read each iteration */
            g_edx += 1u;
            g_eax += 4u;
            g_ecx += 8u;
        } while ((int32_t)g_edx < (int32_t)g_edi);
        g_edi = counted_words_pop();
    }
    g_esi = counted_words_pop();
}

/* T1479 appendix draft 0x00341A10 (legacy vector mode, cdecl, 2 stack args, no register input).
 * Arguments: object, value (a float passed as a raw 32-bit stack word).
 * Original behavior: a null object returns 0 at once (EAX = 0, ECX and EDX untouched, XMM0
 * untouched, no memory access). Otherwise EDX = old [object+4] (state), ECX = 3, and when the old
 * state is not 3 the dword [object+8] is set to zero (xorps xmm0, xmm0 then movss store, so XMM0
 * is cleared in all four lanes on this path). Then movss xmm0, [esp+8] loads the value argument AFTER
 * that store (the load zero-extends: XMM0 = value, 0, 0, 0), [object+4] = 3 and [object+0xC] = the
 * low dword of XMM0. An object that overlaps the argument slots therefore loads the zero just
 * stored when [object+8] is the value argument slot.
 * Guest globals touched: none. No push or pop. Writes in order: [object+8] = 0 (state != 3 only),
 * [object+4] = 3, [object+0xC] = value.
 * Register exit contract: EAX = object (0 for null), ECX = 3 and EDX = old state (not for null),
 * XMM0 = {value as loaded, 0, 0, 0} (not for null), ESP back at entry; EBX/ESI/EDI/EBP untouched. */


GAME_REPLACE_EXACT(00341A10, cdecl, 2, u32, game_object_set_state_3_store_float_0xc_clearing_0x8_on_change)
{
    g_eax = game_stack_arg(0u); /* object */
    if (g_eax == 0u) {
        return;
    }
    g_edx = guest_read32(g_eax + 4u); /* old state */
    g_ecx = 3u;
    if (g_edx != g_ecx) {
        g_xmm0.q[0] = 0u; /* xorps xmm0, xmm0: all lanes cleared */
        g_xmm0.q[1] = 0u;
        guest_write32(g_eax + 8u, g_xmm0.u[0]); /* movss [object+8], xmm0 */
    }
    g_xmm0.u[0] = game_stack_arg(1u); /* movss xmm0, [esp+8]: load zeroes the upper lanes */
    g_xmm0.u[1] = 0u;
    g_xmm0.q[1] = 0u;
    guest_write32(g_eax + 4u, g_ecx);
    guest_write32(g_eax + 0xCu, g_xmm0.u[0]);
}

/* T1479 appendix draft 0x00347D70 (legacy vector mode, cdecl, 3 stack args, no register input).
 * Arguments: arg0 (dword), value (a float as a raw stack word), source (pointer).
 * Appends one 0x2C-byte record to a 32-entry table at 0x767100 whose count is the dword at 0x7670F8.
 * Original behavior: EBX is pushed and the count is read (signed). Count >= 0x20 pops EBX and
 * returns (EAX, ECX, EDX, XMM0 untouched). Otherwise EBP is pushed and source is read from the
 * stack; a null source pops EBP and EBX and returns (same untouched registers, no writes). Else XMM0 =
 * movss [value argument] (lanes 1..3 zeroed), EDX = count*0x2C (32-bit), ESI and EDI are pushed,
 * ESI = EDX+0x767100 (the record), the 11 dwords of the record are zeroed by `rep stosd` (direction
 * flag clear, ECX ends 0), then EAX = arg0 is read from the stack AFTER the fill, [record+0x24] =
 * EAX, [record+0x20] = low dword of XMM0 (the value as loaded BEFORE the fill), [record+0x28] = 0,
 * ECX = [source+0x2C], EBX = count+1, EDI is popped, [record+0] = ECX, [0x7670F8] = EBX, then ESI, EBP and
 * EBX are popped.
 * Guest globals touched: [0x7670F8] (read, then written count+1) and the table. All four saved
 * registers are real guest stack pushes (EBX, EBP, ESI, EDI in that order). Every pop reads its slot
 * back after the function's own stores, so a stack that overlaps the table or the count global
 * (the fill, the three field stores, [record+0], the count store) changes the restored registers
 * (the T1479 POP-alias lesson): EDI is popped before the [record+0] and count stores, ESI/EBP/EBX after.
 * Register exit contract, full path: EAX = arg0 as read after the fill, ECX = [source+0x2C],
 * EDX = count*0x2C, XMM0 = {value,0,0,0}, EBX/EBP/ESI/EDI popped from their slots, ESP back at entry.
 * Early exits: EAX, ECX, EDX and XMM0 unchanged, EBX (and EBP) popped. */

extern __thread uint32_t g_ebx, g_ebp, g_edi;


#define ENTRY_COUNT_ADDR 0x007670F8u
#define ENTRY_TABLE_ADDR 0x00767100u
#define ENTRY_SIZE 0x2Cu
#define ENTRY_LIMIT 0x20
#define ENTRY_DWORDS 0xBu

static void entry_push(uint32_t value)
{
    guest_write32(g_esp - 4u, value);
    g_esp -= 4u;
}

static uint32_t entry_pop(void)
{
    const uint32_t value = guest_read32(g_esp);
    g_esp += 4u;
    return value;
}

GAME_REPLACE_EXACT(00347D70, cdecl, 3, u32, game_append_0x2c_byte_record_value_0x20_arg0_0x24_source_field_0x2c)
{
    entry_push(g_ebx);
    g_ebx = guest_read32(ENTRY_COUNT_ADDR);
    if ((int32_t)g_ebx >= ENTRY_LIMIT) {
        g_ebx = entry_pop();
        return;
    }
    entry_push(g_ebp);
    g_ebp = guest_read32(g_esp + 0x14u); /* source (third stack argument) */
    if (g_ebp == 0u) {
        g_ebp = entry_pop();
        g_ebx = entry_pop();
        return;
    }
    g_xmm0.u[0] = guest_read32(g_esp + 0x10u); /* movss xmm0, [value argument] */
    g_xmm0.u[1] = 0u;
    g_xmm0.q[1] = 0u;
    g_edx = g_ebx * ENTRY_SIZE;
    entry_push(g_esi);
    entry_push(g_edi);
    g_esi = g_edx + ENTRY_TABLE_ADDR;
    g_eax = 0u;
    g_edi = g_esi;
    for (g_ecx = ENTRY_DWORDS; g_ecx != 0u; g_ecx--) { /* rep stosd, ascending addresses */
        guest_write32(g_edi, g_eax);
        g_edi += 4u;
    }
    g_eax = guest_read32(g_esp + 0x14u); /* arg0, read after the fill */
    guest_write32(g_edx + ENTRY_TABLE_ADDR + 0x24u, g_eax);
    guest_write32(g_edx + ENTRY_TABLE_ADDR + 0x20u, g_xmm0.u[0]); /* movss [record+0x20], xmm0 */
    guest_write32(g_edx + ENTRY_TABLE_ADDR + 0x28u, 0u);
    g_ecx = guest_read32(g_ebp + 0x2Cu);
    g_ebx += 1u;
    g_edi = entry_pop();
    guest_write32(g_esi, g_ecx);
    guest_write32(ENTRY_COUNT_ADDR, g_ebx);
    g_esi = entry_pop();
    g_ebp = entry_pop();
    g_ebx = entry_pop();
}

/* T1479 appendix draft 0x00349390 (legacy vector mode, cdecl, 3 stack args, no register input).
 * Original behavior: copies two 3-float vectors (arguments 1 and 2, read as raw dwords) into the
 * destination record (argument 0) at +0x0C..+0x14 and +0x18..+0x20, stores a zero float at +0x38
 * (movss from a cleared XMM0) and sets the dword at +0x08 to 1. INFERRED: a screen distortion
 * beam entry initializer (docs/t1261-naming-band-300000-370000.md names 349390 as the update/add of
 * a distortion entry).
 * Guest globals touched: none.
 * Read order: dest = arg0, srcA = arg1, [srcA], (write +0x0C), [srcA+4], (write +0x10), [srcA+8],
 * (write +0x14), then srcB = arg2 (the argument slot is read AFTER the first three stores, so a
 * destination that overlaps the argument slots changes what is read), [srcB], (write +0x18),
 * [srcB+4], (write +0x1C), [srcB+8], (write +0x20), then [dest+0x38] = 0 and [dest+0x08] = 1.
 * Register exit contract (single path): EAX = dest, ECX = [srcB+8] (the last loaded dword),
 * EDX = [srcB+4] (the value read before the +0x1C store). XMM0 is cleared in all four lanes
 * (xorps xmm0, xmm0), XMM1..7 untouched. No push or pop, so no saved-register slots. */


GAME_REPLACE_EXACT(00349390, cdecl, 3, u32,
                   game_init_entry_copy_two_vec3_to_0xc_0x18_clear_float_0x38_set_dword_8)
{
    g_eax = game_stack_arg(0u);                       /* destination record */
    g_ecx = game_stack_arg(1u);                       /* first source vector */
    g_edx = guest_read32(g_ecx);
    g_xmm0.q[0] = 0u;                                 /* xorps xmm0, xmm0 */
    g_xmm0.q[1] = 0u;
    guest_write32(g_eax + 0x0Cu, g_edx);
    g_edx = guest_read32(g_ecx + 4u);
    guest_write32(g_eax + 0x10u, g_edx);
    g_ecx = guest_read32(g_ecx + 8u);
    guest_write32(g_eax + 0x14u, g_ecx);
    g_ecx = game_stack_arg(2u);                       /* second source vector, read after the stores */
    g_edx = guest_read32(g_ecx);
    guest_write32(g_eax + 0x18u, g_edx);
    g_edx = guest_read32(g_ecx + 4u);
    guest_write32(g_eax + 0x1Cu, g_edx);
    g_ecx = guest_read32(g_ecx + 8u);
    guest_write32(g_eax + 0x20u, g_ecx);
    guest_write32(g_eax + 0x38u, g_xmm0.u[0]);        /* movss [dest+0x38], xmm0 */
    guest_write32(g_eax + 0x08u, 1u);
}

/* T1479 appendix draft 0x00350C60 (plain, cdecl, 0 stack args, no register input).
 * Original behavior: reads the signed dword at 0x4C0454 and returns it clamped below at zero
 * (a negative value yields 0, anything else is returned unchanged). The original builds the mask
 * with `setl cl ; dec ecx` (ECX = 0 for a negative value, 0xFFFFFFFF otherwise) and ANDs EAX.
 * INFERRED from other records: [0x4C0454] is the language/locale selector, -1 meaning none loaded.
 * Guest globals touched: [0x4C0454] (dword, read, the only memory access). No writes, no stack
 * use beyond the return address, no push or pop.
 * Register exit contract (single path): EAX = clamped value, ECX = 0 when the value is negative
 * (signed, jl semantics: bit 31 set) else 0xFFFFFFFF, EDX unchanged. */

#define T1479A_00350C60_SELECTOR 0x004C0454u

GAME_REPLACE_EXACT(00350C60, cdecl, 0, u32, game_clamp_global_4c0454_negative_to_zero)
{
    g_eax = guest_read32(T1479A_00350C60_SELECTOR);
    g_ecx = ((int32_t)g_eax < 0) ? 0u : 0xFFFFFFFFu;  /* xor ecx,ecx ; test ; setl cl ; dec ecx */
    g_eax &= g_ecx;
}

/* T1479 appendix draft 0x003562E0 (plain, cdecl, 0 stack args, no register input).
 * Original behavior: counts how many of 16 byte flags have bit 0 set. The flags are the first
 * bytes of 16 records of stride 0x30C starting at 0x7741B4 (0x7741B4 + 0x30C * i, i = 0..15). The
 * original walks four records per loop pass (ECX at 0x7744C0 + 0xC30 * pass, bytes at ECX - 0x30C,
 * ECX, ECX + 0x30C, ECX + 0x618), ECX < 0x777580 as a signed compare, exactly four passes. Only
 * bit 0 matters (test byte, dl with dl = 1). Every one of the 16 bytes is read, in ascending
 * record order, even when the count is already known.
 * Guest globals touched: the 16 bytes above (read only). No writes, no push or pop.
 * Register exit contract (single path): EAX = count (0..16), ECX = 0x777580, EDX = (EDX & ~0xFF)
 * | 1 (`mov dl, 1` changes only the low byte), no other register is changed. */

#define T1479A_003562E0_FIRST_BYTE 0x007741B4u  /* record 0 flag byte */
#define T1479A_003562E0_STRIDE 0x30Cu
#define T1479A_003562E0_RECORDS 16u
#define T1479A_003562E0_FINAL_ECX 0x00777580u

GAME_REPLACE_EXACT(003562E0, cdecl, 0, u32, game_count_bit0_of_16_flag_bytes_at_7741b4_stride_30c)
{
    uint32_t count = 0u;
    for (uint32_t index = 0u; index < T1479A_003562E0_RECORDS; index++) {
        const uint32_t flags = guest_read8(T1479A_003562E0_FIRST_BYTE + T1479A_003562E0_STRIDE * index);
        if ((flags & 1u) != 0u) {
            count += 1u;
        }
    }
    g_eax = count;
    g_ecx = T1479A_003562E0_FINAL_ECX;
    g_edx = (g_edx & 0xFFFFFF00u) | 1u;
}

/* T1479 appendix draft 0x00357DD0 (legacy vector mode, cdecl, 4 stack args, no register input).
 * Original behavior: arguments owner, record, tag, vector. A null record returns at once (EAX = 0,
 * ECX, EDX and XMM0 unchanged, nothing read or written). Otherwise: [record+0x158] = owner,
 * [owner+0x28] |= 0x10000 (read-modify-write), [record+0x15C] = tag (argument read AFTER those two
 * stores), the three floats [record+0x64], +0x68, +0x6C are cleared to 0.0f (movss from a cleared
 * XMM0), then the 3-dword vector at the vector argument is copied to [record+0x1C], +0x20, +0x24
 * (the vector pointer argument is read before the movss stores and the copy). Finally the pointer P
 * at [record+0xA8] is loaded: when it is null the function ends, otherwise the copied vector is
 * mirrored into P+0x64, P+0x68, P+0x6C, re-reading [record+0xA8] before each of the last two
 * mirror stores and re-reading the source dwords [record+0x1C], +0x20, +0x24 from guest memory
 * (so stores that alias the record change what is mirrored).
 * Guest globals touched: none.
 * Register exit contract:
 *   record == 0 : EAX = 0, ECX, EDX unchanged, XMM0 unchanged.
 *   [record+0xA8] == 0 : EAX = record, ECX = 0, EDX = the vector dword [vector+4] read before the
 *                 +0x20 store, XMM0 = 0 in all four lanes.
 *   mirrored    : EAX = record, ECX = [record+0xA8] as re-read for the last mirror store,
 *                 EDX = [record+0x24] as re-read for the last mirror store, XMM0 cleared.
 * XMM1..7 untouched. No push or pop, so no saved-register slots. */


GAME_REPLACE_EXACT(00357DD0, cdecl, 4, u32,
                   game_bind_record_to_owner_copy_vec3_to_0x1c_clear_0x64_mirror_to_pointer_0xa8)
{
    g_eax = game_stack_arg(1u);                       /* record */
    if (g_eax == 0u) {
        return;
    }
    g_ecx = game_stack_arg(0u);                       /* owner */
    g_xmm0.q[0] = 0u;                                 /* xorps xmm0, xmm0 */
    g_xmm0.q[1] = 0u;
    guest_write32(g_eax + 0x158u, g_ecx);
    guest_write32(g_ecx + 0x28u, guest_read32(g_ecx + 0x28u) | 0x10000u);
    g_ecx = game_stack_arg(2u);                       /* tag, read after the two stores */
    guest_write32(g_eax + 0x15Cu, g_ecx);
    g_ecx = game_stack_arg(3u);                       /* source vector */
    guest_write32(g_eax + 0x64u, g_xmm0.u[0]);        /* movss [record+0x64], xmm0 */
    guest_write32(g_eax + 0x68u, g_xmm0.u[0]);
    guest_write32(g_eax + 0x6Cu, g_xmm0.u[0]);
    g_edx = guest_read32(g_ecx);
    guest_write32(g_eax + 0x1Cu, g_edx);
    g_edx = guest_read32(g_ecx + 4u);
    guest_write32(g_eax + 0x20u, g_edx);
    g_ecx = guest_read32(g_ecx + 8u);
    guest_write32(g_eax + 0x24u, g_ecx);
    g_ecx = guest_read32(g_eax + 0xA8u);              /* linked record P */
    if (g_ecx == 0u) {
        return;
    }
    g_edx = guest_read32(g_eax + 0x1Cu);
    guest_write32(g_ecx + 0x64u, g_edx);
    g_ecx = guest_read32(g_eax + 0xA8u);              /* re-read, the store may have aliased it */
    g_edx = guest_read32(g_eax + 0x20u);
    guest_write32(g_ecx + 0x68u, g_edx);
    g_ecx = guest_read32(g_eax + 0xA8u);
    g_edx = guest_read32(g_eax + 0x24u);
    guest_write32(g_ecx + 0x6Cu, g_edx);
}

/* T1479 appendix draft 0x0035E290 (plain, cdecl, 0 stack args, no register input).
 * Original behavior: returns 1 when the dword at 0x76BBB0 is non-zero, else 0.
 * Guest globals touched: [0x76BBB0] (dword, read, the only memory access). No writes, no push or
 * pop, no stack use beyond the return address.
 * Register exit contract (single path): EAX = (value != 0) as 0 or 1, ECX = the value read,
 * EDX unchanged. */

#define T1479A_0035E290_FLAG 0x0076BBB0u

GAME_REPLACE_EXACT(0035E290, cdecl, 0, u32, game_is_global_76bbb0_nonzero)
{
    g_ecx = guest_read32(T1479A_0035E290_FLAG);
    g_eax = (g_ecx != 0u) ? 1u : 0u;                  /* xor eax,eax ; test ecx,ecx ; setne al */
}

/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * T1479 appendix draft 0x0035E960 (plain, cdecl, 0 stack args, no register inputs, plain `ret`).
 *
 * Original: ECX = dword [0x774024]; EAX = 1 when that dword equals 1 exactly (cmp ecx,1 sete al
 * after xor eax,eax, so EAX is 0 or 1 with all upper bits clear), else 0.
 *
 * Guest reads: [0x774024]. Guest writes: none. No push or pop, no stack argument.
 * Register exit contract:
 *   EAX = (dword == 1) ? 1 : 0.   ECX = the dword read.   EDX unchanged.
 *   EBX, ESI, EDI, EBP untouched. ESP unchanged.
 */
GAME_REPLACE_EXACT(0035E960, cdecl, 0, u32, game_is_dword_774024_equal_to_one)
{
    g_ecx = guest_read32(0x774024u);
    g_eax = (g_ecx == 1u) ? 1u : 0u;
}

/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * T1479 appendix draft 0x0036CC10 (plain, cdecl, 4 stack args, no register inputs, plain `ret`).
 *
 * Original: object = arg0, then arg1 -> ECX, arg2 -> EDX; stores [object+0x24] = arg1;
 * ONLY THEN reads arg3 (ECX = [esp+0x10]); stores [object+0x28] = arg2 and [object+0x2C] = arg3.
 * Because arg3 is read after the first store, an object whose +0x24 field lies on the arg3
 * slot (object = esp - 0x14 at entry) makes the third store write arg1 instead. The stores to
 * +0x28 and +0x2C happen after all argument reads, so they cannot change what is read.
 *
 * Guest reads: arg0, arg1, arg2, (store +0x24), arg3. Guest writes in order: +0x24, +0x28, +0x2C.
 * No push or pop.
 * Register exit contract:
 *   EAX = object (arg0).   ECX = arg3 as read after the +0x24 store.   EDX = arg2.
 *   EBX, ESI, EDI, EBP untouched. ESP unchanged (cdecl, caller pops).
 */
GAME_REPLACE_EXACT(0036CC10, cdecl, 4, u32, game_object_store_three_args_into_fields_0x24_0x28_0x2c)
{
    g_eax = guest_read32(g_esp + 4u);
    g_ecx = guest_read32(g_esp + 8u);
    g_edx = guest_read32(g_esp + 12u);
    guest_write32(g_eax + 0x24u, g_ecx);
    g_ecx = guest_read32(g_esp + 16u);
    guest_write32(g_eax + 0x28u, g_edx);
    guest_write32(g_eax + 0x2cu, g_ecx);
}
