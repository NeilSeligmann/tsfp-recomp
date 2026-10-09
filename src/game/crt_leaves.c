/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Five small leaf routines of the title's statically linked runtime. Each one is written
 * from our own reading of the retail bytes (default.xbe, build 5849). No name here comes
 * from a library or header: the functions are named by what they do and where they are.
 *
 *   003C8960  copy a NUL-terminated byte string, at most `count` bytes, zero-filling the rest
 *   003C94F0  compare two byte strings over at most `count` bytes, result -1 / 0 / +1
 *   003C9C4E  look a signed char value up in a 16-bit class table and mask it
 *   003CA6C0  length of a NUL-terminated string of 16-bit units
 *   003C9F80  append at most `count` bytes of a source string, then a NUL
 *
 * Every one is cdecl, a leaf (no call), touches no hardware or kernel state and has no
 * floating point. 003C9C4E reads one global, the class-table pointer at 0x0054D7F0. The
 * pointer is only ever read in the image (41 references, no store) and starts out pointing
 * at a table in .rdata, so the replacement reads it from guest memory every call.
 *
 * Both copy routines move 32-bit words once the source is aligned and detect a NUL inside a
 * word with a carry trick. The result is the same as a byte loop except for three details
 * that the bodies below reproduce on purpose: the ecx/edx values left behind, which depend
 * on the path taken, the exact set of bytes written, and the order of the stores when source
 * and destination overlap. The bodies are therefore transliterations of the original
 * control flow with its labels kept as comments, not simplified loops. EFLAGS is not
 * reproduced, and the direction flag is assumed clear as the original assumes.
 */
#include "game_replace.h"

#define CLASS_TABLE_POINTER_ADDRESS 0x0054D7F0u
#define CLASS_INDEX_LIMIT 0x100u
#define WORD_ZERO_TEST 0x81010100u
#define WORD_ZERO_BIAS 0x7EFEFEFFu

static uint32_t read_unit16(guest_addr address)
{
    return (uint32_t)guest_read8(address) | ((uint32_t)guest_read8(address + 1u) << 8);
}

static void write_unit16(guest_addr address, uint32_t value)
{
    guest_write8(address, (uint8_t)value);
    guest_write8(address + 1u, (uint8_t)(value >> 8));
}

typedef struct leaf_registers {
    uint32_t ecx;
    uint32_t edx;
} leaf_registers;

/* ------------------------------------------------------------------------------------ */
/* 003C8960: copy at most `count` bytes of `source`, stopping after its NUL and filling   */
/* the remainder of the `count` bytes with zeros. Returns `destination`.                  */
/* ------------------------------------------------------------------------------------ */
static uint32_t crt_leaf_bounded_copy_003c8960(guest_addr destination, guest_addr source,
                                               uint32_t count, leaf_registers *regs)
{
    uint32_t ecx = count;
    uint32_t edx = regs->edx;
    uint32_t ebx = 0u;
    uint32_t eax = 0u; /* only al is used before the final result is loaded */
    guest_addr esi = source;
    guest_addr edi = destination;

    if (ecx == 0u) {
        goto finish;
    }
    ebx = ecx;
    if ((esi & 3u) != 0u) {
        goto head_byte;
    }
    ecx >>= 2;
    if (ecx != 0u) {
        goto word_loop;
    }
    goto tail_byte;

head_byte: /* 003C898C: copy bytes until the source is word aligned */
    eax = guest_read8(esi);
    esi += 1u;
    guest_write8(edi, (uint8_t)eax);
    edi += 1u;
    ecx -= 1u;
    if (ecx == 0u) {
        goto finish;
    }
    if ((uint8_t)eax == 0u) {
        goto pad_start;
    }
    if ((esi & 3u) != 0u) {
        goto head_byte;
    }
    ebx = ecx;
    ecx >>= 2;
    if (ecx != 0u) {
        goto word_loop;
    }
    ebx &= 3u;
    if (ebx == 0u) {
        goto finish;
    }

tail_byte: /* 003C89B3: the last 1..3 bytes */
    eax = guest_read8(esi);
    esi += 1u;
    guest_write8(edi, (uint8_t)eax);
    edi += 1u;
    if ((uint8_t)eax == 0u) {
        goto pad_tail_decrement;
    }
    ebx -= 1u;
    if (ebx != 0u) {
        goto tail_byte;
    }
    goto finish;

pad_start: /* 003C89CE: source ended, zero-fill; al is zero */
    if ((edi & 3u) != 0u) {
        do {
            guest_write8(edi, (uint8_t)eax);
            edi += 1u;
            ecx -= 1u;
            if (ecx == 0u) {
                goto finish;
            }
        } while ((edi & 3u) != 0u);
    }
    ebx = ecx;
    ecx >>= 2;
    if (ecx != 0u) {
        goto pad_words;
    }
pad_tail_byte: /* 003C89F3 */
    guest_write8(edi, (uint8_t)eax);
    edi += 1u;
pad_tail_decrement: /* 003C89F8 */
    ebx -= 1u;
    if (ebx != 0u) {
        goto pad_tail_byte;
    }
    goto finish;

word_store: /* 003C8A05: the word held no NUL, store it as it is */
    guest_write32(edi, edx);
    edi += 4u;
    ecx -= 1u;
    if (ecx == 0u) {
        ebx &= 3u;
        if (ebx == 0u) {
            goto finish;
        }
        goto tail_byte;
    }
word_loop: /* 003C8A0F */
    eax = guest_read32(esi);
    edx = WORD_ZERO_BIAS + eax;
    eax = ~eax ^ edx;
    edx = guest_read32(esi);
    esi += 4u;
    if ((eax & WORD_ZERO_TEST) == 0u) {
        goto word_store;
    }
    if ((edx & 0xFFu) == 0u) {
        edx = 0u;
        guest_write32(edi, edx);
    } else if ((edx & 0xFF00u) == 0u) {
        edx &= 0xFFu;
        guest_write32(edi, edx);
    } else if ((edx & 0xFF0000u) == 0u) {
        edx &= 0xFFFFu;
        guest_write32(edi, edx);
    } else if ((edx & 0xFF000000u) != 0u) {
        goto word_store;
    } else {
        guest_write32(edi, edx);
    }
    edi += 4u; /* 003C8A5D: the NUL word is stored, zero-fill what remains */
    eax = 0u;
    ecx -= 1u;
    if (ecx == 0u) {
        goto pad_after_words;
    }
pad_words: /* 003C8A67 */
    eax = 0u;
    do {
        guest_write32(edi, eax);
        edi += 4u;
        ecx -= 1u;
    } while (ecx != 0u);
pad_after_words: /* 003C8A73 */
    ebx &= 3u;
    if (ebx != 0u) {
        goto pad_tail_byte;
    }

finish:
    regs->ecx = ecx;
    regs->edx = edx;
    return destination;
}

GAME_REPLACE_EXACT(003C8960, cdecl, 3, u32, crt_leaf_bounded_copy_003c8960)
{
    leaf_registers regs = {g_ecx, g_edx};
    g_eax = crt_leaf_bounded_copy_003c8960(game_stack_arg(0), game_stack_arg(1),
                                           game_stack_arg(2), &regs);
    g_ecx = regs.ecx;
    g_edx = regs.edx;
}

/* ------------------------------------------------------------------------------------ */
/* 003C94F0: compare `first` and `second` over at most `count` bytes, ending at the first */
/* difference or after the NUL of `first`. Returns -1 when the second byte is larger,     */
/* +1 when it is smaller (an unsigned byte comparison), else 0.                           */
/* ------------------------------------------------------------------------------------ */
static uint32_t crt_leaf_bounded_compare_003c94f0(guest_addr first, guest_addr second,
                                                  uint32_t count)
{
    if (count == 0u) {
        return 0u;
    }
    /* The original first scans `first` for a NUL within `count` bytes (the NUL counts) and
     * then compares exactly that many bytes of the two strings. */
    uint32_t scanned = 0u;
    while (scanned < count) {
        uint8_t byte = guest_read8(first + scanned);
        scanned += 1u;
        if (byte == 0u) {
            break;
        }
    }
    uint8_t first_byte = 0u;
    uint8_t second_byte = 0u;
    for (uint32_t index = 0u; index < scanned; index++) {
        first_byte = guest_read8(first + index);
        second_byte = guest_read8(second + index);
        if (first_byte != second_byte) {
            break;
        }
    }
    if (second_byte > first_byte) {
        return UINT32_MAX;
    }
    return second_byte == first_byte ? 0u : 1u;
}

GAME_REPLACE_EXACT(003C94F0, cdecl, 3, u32, crt_leaf_bounded_compare_003c94f0)
{
    g_eax = crt_leaf_bounded_compare_003c94f0(game_stack_arg(0), game_stack_arg(1),
                                              game_stack_arg(2));
    /* The result is copied into ecx before the return. edx is not touched. */
    g_ecx = g_eax;
}

/* ------------------------------------------------------------------------------------ */
/* 003C9C4E: class bits of one character. `value` is a signed byte-range value: -1 .. 255 */
/* index a table of 16-bit entries whose pointer lives in a global, anything else gives 0. */
/* ------------------------------------------------------------------------------------ */
static uint32_t crt_leaf_class_lookup_003c9c4e(uint32_t value, uint32_t mask, uint32_t *ecx)
{
    *ecx = value + 1u;
    if (*ecx > CLASS_INDEX_LIMIT) {
        return 0u;
    }
    *ecx = guest_read32(CLASS_TABLE_POINTER_ADDRESS);
    return read_unit16(*ecx + value * 2u) & mask;
}

GAME_REPLACE_EXACT(003C9C4E, cdecl, 2, u32, crt_leaf_class_lookup_003c9c4e)
{
    uint32_t ecx;
    g_eax = crt_leaf_class_lookup_003c9c4e(game_stack_arg(0), game_stack_arg(1), &ecx);
    g_ecx = ecx;
}

/* ------------------------------------------------------------------------------------ */
/* 003CA6C0: number of 16-bit units before the first zero unit.                           */
/* ------------------------------------------------------------------------------------ */
static uint32_t crt_leaf_unit16_length_003ca6c0(guest_addr text)
{
    guest_addr cursor = text;
    uint32_t unit;
    do {
        unit = read_unit16(cursor);
        cursor += 2u;
    } while (unit != 0u);
    /* sub, sar 1, dec: the byte distance is halved with an arithmetic shift. */
    return (uint32_t)((int32_t)(cursor - text) >> 1) - 1u;
}

GAME_REPLACE_EXACT(003CA6C0, cdecl, 1, u32, crt_leaf_unit16_length_003ca6c0)
{
    g_eax = crt_leaf_unit16_length_003ca6c0(game_stack_arg(0));
    /* The loop reads the unit into cx, so only the low half of ecx ends up zero. */
    g_ecx &= 0xFFFF0000u;
}

/* ------------------------------------------------------------------------------------ */
/* 003C9F80: append up to `count` bytes of `source` after the NUL of `destination`, then  */
/* a NUL. Nothing is zero-filled. Returns `destination`.                                  */
/* ------------------------------------------------------------------------------------ */
static uint32_t crt_leaf_bounded_append_003c9f80(guest_addr destination, guest_addr source,
                                                 uint32_t count, leaf_registers *regs)
{
    uint32_t ecx = count;
    uint32_t edx = regs->edx;
    uint32_t ebx = 0u;
    uint32_t eax;
    guest_addr esi;
    guest_addr edi = destination;

    if (ecx == 0u) {
        goto finish;
    }
    if ((edi & 3u) != 0u) {
        do {
            eax = guest_read8(edi);
            edi += 1u;
            if ((uint8_t)eax == 0u) {
                goto end_found_1;
            }
        } while ((edi & 3u) != 0u);
    }
scan_word: /* 003C9FAC: find the end of the destination a word at a time */
    eax = guest_read32(edi);
    edx = WORD_ZERO_BIAS + eax;
    eax = ~eax ^ edx;
    edi += 4u;
    if ((eax & WORD_ZERO_TEST) == 0u) {
        goto scan_word;
    }
    eax = guest_read32(edi - 4u);
    if ((eax & 0xFFu) == 0u) {
        edi -= 4u;
        goto end_found;
    }
    if ((eax & 0xFF00u) == 0u) {
        edi -= 3u;
        goto end_found;
    }
    if ((eax & 0xFF0000u) == 0u) {
        edi -= 2u;
        goto end_found;
    }
    if ((eax & 0xFF000000u) != 0u) {
        goto scan_word;
    }
end_found_1:
    edi -= 1u;
end_found: /* 003C9FEF: edi is the address of the destination's NUL */
    esi = source;
    if ((esi & 3u) != 0u) {
        goto head_byte;
    }
    ebx = ecx;
    ecx >>= 2;
    if (ecx != 0u) {
        goto word_loop;
    }
    goto tail_start;

head_byte: /* 003CA004 */
    edx = (edx & 0xFFFFFF00u) | guest_read8(esi);
    esi += 1u;
    if ((edx & 0xFFu) == 0u) {
        goto store_terminator;
    }
    guest_write8(edi, (uint8_t)edx);
    edi += 1u;
    ecx -= 1u;
    if (ecx == 0u) {
        goto terminate_count;
    }
    if ((esi & 3u) != 0u) {
        goto head_byte;
    }
    ebx = ecx;
    ecx >>= 2;
    if (ecx != 0u) {
        goto word_loop;
    }
tail_start: /* 003CA026 */
    ecx = ebx & 3u;
    if (ecx == 0u) {
        goto terminate_count;
    }
tail_byte: /* 003CA02D */
    edx = (edx & 0xFFFFFF00u) | guest_read8(esi);
    esi += 1u;
    guest_write8(edi, (uint8_t)edx);
    edi += 1u;
    if ((edx & 0xFFu) == 0u) {
        goto finish;
    }
    ecx -= 1u;
    if (ecx != 0u) {
        goto tail_byte;
    }
terminate_count: /* 003CA040: count ran out, ecx is zero, cl is the NUL */
    guest_write8(edi, (uint8_t)ecx);
    goto finish;

store_terminator: /* 003CA04A: the source ended, dl is zero */
    guest_write8(edi, (uint8_t)edx);
    goto finish;

word_store: /* 003CA054 */
    guest_write32(edi, edx);
    edi += 4u;
    ecx -= 1u;
    if (ecx == 0u) {
        goto tail_start;
    }
word_loop: /* 003CA05E */
    eax = guest_read32(esi);
    edx = WORD_ZERO_BIAS + eax;
    eax = ~eax ^ edx;
    edx = guest_read32(esi);
    esi += 4u;
    if ((eax & WORD_ZERO_TEST) == 0u) {
        goto word_store;
    }
    if ((edx & 0xFFu) == 0u) {
        goto store_terminator;
    }
    if ((edx & 0xFF00u) == 0u) {
        write_unit16(edi, edx); /* 003CA0AA: one byte, then the NUL */
        goto finish;
    }
    if ((edx & 0xFF0000u) == 0u) {
        write_unit16(edi, edx); /* 003CA09A: two bytes, then the NUL */
        edx = 0u;
        guest_write8(edi + 2u, 0u);
        goto finish;
    }
    if ((edx & 0xFF000000u) != 0u) {
        goto word_store;
    }
    guest_write32(edi, edx); /* 003CA090: three bytes plus the NUL fill the word */

finish:
    regs->ecx = ecx;
    regs->edx = edx;
    return destination;
}

GAME_REPLACE_EXACT(003C9F80, cdecl, 3, u32, crt_leaf_bounded_append_003c9f80)
{
    leaf_registers regs = {g_ecx, g_edx};
    g_eax = crt_leaf_bounded_append_003c9f80(game_stack_arg(0), game_stack_arg(1),
                                             game_stack_arg(2), &regs);
    g_ecx = regs.ecx;
    g_edx = regs.edx;
}

/* ------------------------------------------------------------------------------------ */
/* 0037EB7C: GetTickCount. eax = [[0x475868]], the dword the KeTickCount import thunk     */
/* slot points at. Plain ret, no arguments, only eax is written.                          */
/* ------------------------------------------------------------------------------------ */
#define KE_TICK_COUNT_THUNK_SLOT 0x00475868u

GAME_REPLACE_EXACT(0037EB7C, cdecl, 0, u32, crt_leaf_tick_count_0037eb7c)
{
    g_eax = guest_read32(guest_read32(KE_TICK_COUNT_THUNK_SLOT));
}

/* ------------------------------------------------------------------------------------ */
/* 0037E9A7: GetLastError, the per-thread TLS read. eax = [[fs:[4] + [0x771368] * 4] + 4], */
/* with fs:[4] the thread's TLS pointer array and [0x771368] the slot index the CRT       */
/* allocated. The original also reads byte fs:[0x24] and, below 2, dword fs:[0x28], both   */
/* dead (both arms reach the same load), so they are not reproduced. ecx ends as fs:[4].   */
/* Plain ret, no arguments, eflags are not reproduced.                                    */
/* ------------------------------------------------------------------------------------ */
#define TLS_SLOT_INDEX_ADDRESS 0x00771368u
#define FS_TLS_ARRAY_OFFSET 4u
#define TLS_VALUE_OFFSET 4u

GAME_REPLACE_EXACT(0037E9A7, cdecl, 0, u32, crt_leaf_last_error_0037e9a7)
{
    uint32_t index = guest_read32(TLS_SLOT_INDEX_ADDRESS);
    uint32_t array = guest_read32(g_fs_base + FS_TLS_ARRAY_OFFSET);
    g_ecx = array;
    g_eax = guest_read32(guest_read32(array + index * 4u) + TLS_VALUE_OFFSET);
}

/* ------------------------------------------------------------------------------------ */
/* 003C9546: find the first byte of `text` equal to the low byte of `wanted` or to NUL.   */
/* The entry is the middle of a longer routine whose first lines load `wanted` into eax;   */
/* 003C8690 tail-jumps here with it already in eax. Returns the address of a match, or 0   */
/* when the NUL comes first. The word loop reads aligned words, as the original does.     */
/* ------------------------------------------------------------------------------------ */
static uint32_t crt_leaf_find_byte_003c9546(guest_addr text, uint32_t wanted,
                                            leaf_registers *regs)
{
    uint32_t ecx = regs->ecx;
    uint32_t edx = text;
    uint32_t ebx = wanted;
    uint32_t eax = wanted << 8;
    uint32_t esi;
    uint32_t edi;

    if ((edx & 3u) == 0u) {
        goto aligned;
    }
head_byte: /* 003C9558 */
    ecx = (ecx & 0xFFFFFF00u) | guest_read8(edx);
    edx += 1u;
    if ((ecx & 0xFFu) == (ebx & 0xFFu)) {
        eax = edx - 1u; /* 003C9530 */
        goto finish;
    }
    if ((ecx & 0xFFu) == 0u) {
        goto not_found;
    }
    if ((edx & 3u) != 0u) {
        goto head_byte;
    }
aligned: /* 003C956D: replicate the wanted byte into all four lanes */
    ebx |= eax;
    eax = ebx;
    ebx <<= 16;
    ebx |= eax;
scan_word: /* 003C9578 */
    ecx = guest_read32(edx);
    edi = WORD_ZERO_BIAS;
    eax = ecx;
    esi = edi;
    ecx ^= ebx;
    esi += eax;
    edi += ecx;
    ecx = ~ecx;
    eax = ~eax;
    ecx ^= edi;
    eax ^= esi;
    edx += 4u;
    ecx &= WORD_ZERO_TEST;
    if (ecx != 0u) {
        goto word_hit;
    }
    eax &= WORD_ZERO_TEST;
    if (eax == 0u) {
        goto scan_word;
    }
    if ((eax & 0x01010100u) != 0u) {
        goto not_found;
    }
    if ((esi & 0x80000000u) != 0u) {
        goto scan_word;
    }
    goto not_found;

word_hit: /* 003C95BA: locate the byte inside the word */
    eax = guest_read32(edx - 4u);
    if ((eax & 0xFFu) == (ebx & 0xFFu)) {
        eax = edx - 4u;
        goto finish;
    }
    if ((eax & 0xFFu) == 0u) {
        goto not_found;
    }
    if (((eax >> 8) & 0xFFu) == (ebx & 0xFFu)) {
        eax = edx - 3u;
        goto finish;
    }
    if (((eax >> 8) & 0xFFu) == 0u) {
        goto not_found;
    }
    eax >>= 16;
    if ((eax & 0xFFu) == (ebx & 0xFFu)) {
        eax = edx - 2u;
        goto finish;
    }
    if ((eax & 0xFFu) == 0u) {
        goto not_found;
    }
    if (((eax >> 8) & 0xFFu) == (ebx & 0xFFu)) {
        eax = edx - 1u;
        goto finish;
    }
    if (((eax >> 8) & 0xFFu) == 0u) {
        goto not_found;
    }
    goto scan_word;

not_found: /* 003C95B4 / 003C95B6 */
    eax = 0u;
finish:
    regs->ecx = ecx;
    regs->edx = edx;
    return eax;
}

GAME_REPLACE_EXACT(003C9546, cdecl, 1, u32, crt_leaf_find_byte_003c9546)
{
    leaf_registers regs = {g_ecx, g_edx};
    g_eax = crt_leaf_find_byte_003c9546(game_stack_arg(0), g_eax, &regs);
    g_ecx = regs.ecx;
    g_edx = regs.edx;
}

/* ------------------------------------------------------------------------------------ */
/* 003C8690: find the first occurrence of `needle` inside `haystack`. An empty needle      */
/* returns `haystack`, a miss returns 0. Two-byte needle prefix scan, then the rest is     */
/* compared two bytes at a time. A one-byte needle is handed to 003C9546, as the original  */
/* tail-jumps there.                                                                       */
/* ------------------------------------------------------------------------------------ */
static uint32_t crt_leaf_find_substring_003c8690(guest_addr haystack, guest_addr needle,
                                                 uint32_t eax_in, leaf_registers *regs)
{
    uint32_t ecx = needle;
    uint32_t edx = regs->edx;
    uint32_t eax = eax_in;
    guest_addr edi = haystack;
    guest_addr esi;

    edx = (edx & 0xFFFFFF00u) | guest_read8(ecx);
    if ((edx & 0xFFu) == 0u) {
        regs->ecx = ecx;
        regs->edx = edx;
        return edi; /* 003C8710: empty needle */
    }
    edx = (edx & 0xFFFF00FFu) | ((uint32_t)guest_read8(ecx + 1u) << 8);
    if ((edx & 0xFF00u) == 0u) {
        leaf_registers tail = {ecx, edx};
        uint32_t result = crt_leaf_find_byte_003c9546(haystack, edx & 0xFFu, &tail);
        regs->ecx = tail.ecx;
        regs->edx = tail.edx;
        return result; /* 003C86FD: one-byte needle */
    }

restart: /* 003C86A8 */
    esi = edi;
    ecx = needle;
    eax = (eax & 0xFFFFFF00u) | guest_read8(edi);
    esi += 1u;
    if ((eax & 0xFFu) == (edx & 0xFFu)) {
        goto first_matched;
    }
/* 003C86B7 */
    if ((eax & 0xFFu) == 0u) {
        goto miss;
    }
next_start: /* 003C86BB */
    eax = (eax & 0xFFFFFF00u) | guest_read8(esi);
    esi += 1u;
compare_first: /* 003C86C0 */
    if ((eax & 0xFFu) == (edx & 0xFFu)) {
        goto first_matched;
    }
    if ((eax & 0xFFu) != 0u) {
        goto next_start;
    }
miss: /* 003C86C8 */
    regs->ecx = ecx;
    regs->edx = edx;
    return 0u;

first_matched: /* 003C86CE */
    eax = (eax & 0xFFFFFF00u) | guest_read8(esi);
    esi += 1u;
    if ((eax & 0xFFu) != ((edx >> 8) & 0xFFu)) {
        goto compare_first;
    }
    edi = esi - 1u;
pair_check: /* 003C86DA */
    eax = (eax & 0xFFFF00FFu) | ((uint32_t)guest_read8(ecx + 2u) << 8);
    if ((eax & 0xFF00u) == 0u) {
        goto found;
    }
    eax = (eax & 0xFFFFFF00u) | guest_read8(esi);
    esi += 2u;
    if ((eax & 0xFFu) != ((eax >> 8) & 0xFFu)) {
        goto restart;
    }
    eax = (eax & 0xFFFFFF00u) | guest_read8(ecx + 3u);
    if ((eax & 0xFFu) == 0u) {
        goto found;
    }
    eax = (eax & 0xFFFF00FFu) | ((uint32_t)guest_read8(esi - 1u) << 8);
    ecx += 2u;
    if ((eax & 0xFFu) == ((eax >> 8) & 0xFFu)) {
        goto pair_check;
    }
    goto restart;

found: /* 003C8709 */
    regs->ecx = ecx;
    regs->edx = edx;
    return edi - 1u;
}

GAME_REPLACE_EXACT(003C8690, cdecl, 2, u32, crt_leaf_find_substring_003c8690)
{
    leaf_registers regs = {g_ecx, g_edx};
    g_eax = crt_leaf_find_substring_003c8690(game_stack_arg(0), game_stack_arg(1), g_eax,
                                             &regs);
    g_ecx = regs.ecx;
    g_edx = regs.edx;
}
