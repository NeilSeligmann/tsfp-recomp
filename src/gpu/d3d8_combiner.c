/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See d3d8_combiner.h. A transliteration of 0x003E11D0 and its helper 0x003E1130, the original's
 * registers and stack slots as locals and its jumps as gotos, so every line maps to one instruction
 * run. The guest labels are in the comments.
 */

#include "d3d8_combiner.h"

#include <stdbool.h>

#include "d3d8_guest.h"
#include "d3d8_hle.h"

#define ENTRY 0x003E11D0u
#define TEXTURE_STATE 0x003E3AC0u
#define STAGE_BYTES 0x80u
#define COLOR_OP 0x30u
#define ALPHA_OP 0x40u
#define RESULT_ARG 0x50u
#define SOURCE_TABLE 0x003E4EE8u
#define RS_FIRST_STAGE 0x003E3E98u /* render state 0x76: non-zero starts the walk at stage 3 */
#define RS_FOG_PAIR 0x003E3E5Cu /* render state 0x67 */
#define OPERATIONS 26u
#define PACKET_DWORDS 9u
#define PACKETS 4u
#define SLOTS 8u

#define FLAGS_CLEARED 0xFFFEFFBFu
#define FLAG_FOG 0x40u
#define FLAG_PROGRAM 0x10000u

typedef struct {
    uint32_t words[2u + PACKETS * PACKET_DWORDS + 2u];
    uint32_t count;
    uint32_t flags_after;
    uint32_t dirty_after;
} combiner_plan;

/* 0x003E1130: one argument selector (`source` low nibble) into a combiner input word. `flags` is
 * device+8, which selector 4 writes. */
static uint32_t combiner_input(uint32_t ecx, uint32_t edx, uint32_t edi, uint32_t *flags)
{
    uint32_t esi;
    switch (edi & 0xFu) {
    case 0u:
        esi = 4u;
        break;
    case 1u:
        esi = (edx & 0x10u) != 0u ? 4u : 0xCu;
        break;
    case 2u: {
        const uint32_t index = edx & 3u;
        esi = d3d8_guest_load32(SOURCE_TABLE + index * 4u) == 0u ? 0xFFFFFFFFu : index + 8u;
        break;
    }
    case 3u:
        esi = 1u;
        break;
    case 4u:
        *flags |= FLAG_FOG;
        esi = 5u;
        break;
    case 5u:
        esi = 0xDu;
        break;
    default:
        d3d8_hle_fatal(0x003E1130u,
                       "combiner argument selector %u indexes past the 6 entry jump table",
                       (unsigned)(edi & 0xFu));
        return 0u;
    }
    uint32_t eax = ecx | edx | edi;
    uint32_t xored = ecx ^ edi;
    eax >>= 1;
    xored &= 0x10u;
    eax &= 0x10u;
    xored <<= 1;
    eax |= xored;
    eax |= ecx & 0x40u;
    const uint32_t shift = ((ecx >> 13) & 0x78u) & 0x1Fu;
    eax |= esi;
    return eax << shift;
}

static void put(uint32_t buffer[PACKETS * PACKET_DWORDS], uint32_t packet, uint32_t slot,
                uint32_t value)
{
    if (packet >= PACKETS || slot >= SLOTS) {
        d3d8_hle_fatal(ENTRY, "combiner slot %u of packet %u is outside the staging area",
                       (unsigned)slot, (unsigned)packet);
        return;
    }
    buffer[packet * PACKET_DWORDS + 1u + slot] = value;
}

static void put_slot(uint32_t buffer[PACKETS * PACKET_DWORDS], uint32_t offset, uint32_t slot,
                     uint32_t value)
{
    /* [ecx + edi] with ecx the colour (0) or alpha (0x48) base, +0x24 the paired output word. */
    put(buffer, offset == 0x48u ? 2u : 0u, slot, value);
}

static void combiner_build(uint32_t dirty, combiner_plan *plan)
{
    uint32_t buffer[PACKETS * PACKET_DWORDS] = {0};
    const uint32_t saved_flags = d3d8_device_load32(D3D8_DEV_FLAGS);
    uint32_t flags = saved_flags & FLAGS_CLEARED;
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp;
    uint32_t s10, s14, s18, s20, s24, s28, s2c;

    const uint32_t first = d3d8_guest_load32(RS_FIRST_STAGE) != 0u ? 3u : 0u;
    const uint32_t start = first;
    ebx = first | 0x10u;
    s20 = first;
    s18 = 0u; /* the slot index, the original's byte offset divided by four */
    s24 = TEXTURE_STATE + first * STAGE_BYTES;
    s28 = s24;
    eax = d3d8_guest_load32(s24 + COLOR_OP);
    edi = esi = ebp = edx = s10 = s14 = s2c = ecx = 0u;

loop_top: /* 0x003E1250 */
    ecx = s24;
    edi = d3d8_guest_load32(ecx + 0x34u);
    esi = d3d8_guest_load32(ecx + 0x38u);
    ebp = d3d8_guest_load32(ecx + 0x3Cu);
    edx = d3d8_guest_load32(ecx + RESULT_ARG) != 5u ? 0xC00u : 0xD00u;
    s10 = edi;
    s2c = edx;

dispatch: /* 0x003E1280 */
    s14 = edx;
    if (eax - 1u >= OPERATIONS) {
        d3d8_hle_fatal(ENTRY, "texture stage operation %u indexes outside the jump table",
                       (unsigned)eax);
        return;
    }
    switch (eax - 1u) {
    case 0u:
        goto op_disable;
    case 1u:
        goto op_select_arg1;
    case 2u:
        goto op_select_arg2;
    case 3u:
        goto op_modulate;
    case 4u:
        goto op_modulate_2x;
    case 5u:
        goto op_modulate_4x;
    case 6u:
        goto op_add;
    case 7u:
        goto op_add_signed;
    case 8u:
        goto op_add_signed_2x;
    case 9u:
        goto op_subtract;
    case 10u:
        goto op_add_smooth;
    case 11u:
    case 12u:
    case 13u:
    case 14u:
        goto op_blend;
    case 15u:
        goto op_premodulate;
    case 16u:
        goto op_modulate_alpha_add_color;
    case 17u:
        goto op_modulate_color_add_alpha;
    case 18u:
        goto op_modulate_inv_alpha_add_color;
    case 19u:
        goto op_modulate_inv_color_add_alpha;
    case 20u:
        goto op_bump_env_map;
    case 21u:
        goto op_bump_env_map_luminance;
    case 22u:
        goto op_dot_product3;
    case 23u:
        goto op_multiply_add;
    default:
        goto op_lerp;
    }

op_disable: /* 0x003E128E */
    if ((ebx & 0x10u) == 0u) {
        s14 = 0u;
        eax = 0u;
        goto finish;
    }
    ecx = ebx & 0x20u;
    eax = (ecx << 0x17) | 0x4200000u;
    ebp = 0u;
    if (ecx != ebp) {
        goto finish;
    }
    edi = s18;
    esi = s20;
    put(buffer, 0u, edi, eax);
    eax |= 0x10000000u;
    put(buffer, 1u, edi, edx);
    put(buffer, 2u, edi, eax);
    put(buffer, 3u, edi, edx);
    edi += 1u;
    esi += 1u;
    goto fill;

op_select_arg1: /* 0x003E12D8 */
    edi = esi;
    eax = combiner_input(0x30000u, ebx, edi, &flags);
    eax |= 0x200000u;
    goto finish;

op_select_arg2: /* 0x003E12F0 */
    edi = ebp;
    eax = combiner_input(0u, ebx, edi, &flags);
    eax |= 0x2000u;
    goto finish;

op_modulate_4x: /* 0x003E1305 */
    s14 = edx | 0x20000u;
    goto op_modulate;

op_modulate_2x: /* 0x003E130E */
    s14 = edx | 0x10000u;
    /* fall through */
op_modulate: /* 0x003E1319 */
    edi = esi;
    eax = combiner_input(0x30000u, ebx, edi, &flags);
    esi = eax;
    ecx = 0x20000u;
    goto tail_second_argument;

op_add_signed_2x: /* 0x003E1333 */
    s14 = edx | 0x18000u;
    /* fall through */
op_add_signed: /* 0x003E133E */
    s14 |= 0x8000u;
    /* fall through */
op_add: /* 0x003E1346 */
    edi = esi;
    eax = combiner_input(0x30000u, ebx, edi, &flags);
    esi = eax | 0x202000u;
    goto tail_zero_ecx;

op_subtract: /* 0x003E1361 */
    edi = esi;
    eax = combiner_input(0x30000u, ebx, edi, &flags);
    esi = eax | 0x204000u;
    goto tail_zero_ecx;

op_add_smooth: /* 0x003E137C */
    edi = esi;
    eax = combiner_input(0x30000u, ebx, edi, &flags);
    eax |= 0x200000u;
    s10 = eax;
    eax = combiner_input(0x10010u, ebx, edi, &flags);
    esi = s10;
    goto tail_or_esi;

op_blend: /* 0x003E13A8 */
    eax -= 0xCu;
    edi = esi;
    s10 = eax;
    eax = combiner_input(0x30000u, ebx, edi, &flags);
    edi = s10;
    esi = eax;
    eax = combiner_input(0x20020u, ebx, edi, &flags);
    ecx = 0x10030u;
    goto tail_blend;

op_premodulate: /* 0x003E13D9 */
    edi = esi;
    eax = combiner_input(0x30000u, ebx, edi, &flags);
    esi = eax | 0x200000u;
    edi = 2u;
    ecx = 0x10030u;
    goto tail_call;

op_modulate_alpha_add_color: /* 0x003E13FE */
    edi = esi;
    eax = combiner_input(0x30000u, ebx, edi, &flags);
    esi = eax;
    if ((ebx & 0x10u) == 0u) {
        esi |= 0x200000u;
        eax = esi;
        goto finish;
    }
    edi = 2u;
    eax = combiner_input(0x20000u, ebx, edi, &flags);
    esi |= eax;
    eax = esi;
    goto finish;

op_modulate_color_add_alpha: /* 0x003E143A */
    edi = esi;
    eax = combiner_input(0x30000u, ebx, edi, &flags);
    eax |= 0x200000u;
    s10 = eax;
    eax = combiner_input(0x10020u, ebx, edi, &flags);
    esi = s10;
    goto tail_or_esi;

op_modulate_inv_alpha_add_color: /* 0x003E1466 */
    ecx = 0x30000u;
    goto op_bump_shared;

op_modulate_inv_color_add_alpha: /* 0x003E146D */
    edi = esi;
    eax = combiner_input(0x30000u, ebx, edi, &flags);
    eax |= 0x200000u;
    s10 = eax;
    eax = combiner_input(0x10030u, ebx, edi, &flags);
    esi = s10;
    goto tail_or_esi;

op_bump_env_map: /* 0x003E1499 */
    ecx = 0x30010u;
    /* fall through */
op_bump_shared: /* 0x003E149E */
    edi = esi;
    eax = combiner_input(ecx, ebx, edi, &flags);
    edi = ebp;
    s10 = eax;
    eax = combiner_input(0x20000u, ebx, edi, &flags);
    ebp = s10;
    edi = esi;
    ebp |= eax;
    eax = combiner_input(0x10020u, ebx, edi, &flags);
    eax |= ebp;
    eax |= 0x20u;
    goto finish;

op_bump_env_map_luminance: /* 0x003E1505 */
    edi = esi;
    eax = combiner_input(0x30040u, ebx, edi, &flags);
    edi = ebp;
    esi = eax;
    eax = combiner_input(0x20040u, ebx, edi, &flags);
    ecx = s2c;
    ecx |= 0x820000u;
    ecx >>= 4;
    edx = 0u;
    s14 = ecx;
    put(buffer, 2u, s18, edx);
    put(buffer, 3u, s18, edx);
    eax |= esi;
    ebx |= 0x80u;
    goto finish;

op_dot_product3: /* 0x003E154A */
    eax = combiner_input(0x30000u, ebx, edi, &flags);
    eax |= 0x200000u;
    edi = esi;
    s10 = eax;
    eax = combiner_input(0x10000u, ebx, edi, &flags);
    esi = s10;
    goto tail_or_esi;

op_multiply_add: /* 0x003E1573 */
    eax = combiner_input(0x30000u, ebx, edi, &flags);
    edi = esi;
    esi = eax;
    eax = combiner_input(0x20000u, ebx, edi, &flags);
    edi = s10;
    ecx = 0x10010u;
    goto tail_blend;

op_lerp: /* 0x003E14D7 */
    edi = 1u;
    eax = combiner_input(0x30000u, ebx, edi, &flags);
    flags |= FLAG_PROGRAM;
    eax |= 0x200000u;
    goto finish;

tail_blend: /* 0x003E159E */
    esi |= eax;
tail_call: /* 0x003E15A0 */
    eax = combiner_input(ecx, ebx, edi, &flags);
tail_or_esi: /* 0x003E15A7 */
    esi |= eax;
tail_zero_ecx: /* 0x003E15A9 */
    ecx = 0u;
tail_second_argument: /* 0x003E15AB */
    edi = ebp;
    eax = combiner_input(ecx, ebx, edi, &flags);
    eax |= esi;

finish: /* 0x003E15B6 */
    edx = eax & 0xFF000000u;
    if (edx == 0xFF000000u) {
        eax = ((~ebx) & 0x10u) << 0x17;
        ecx = ebx & 0x20u;
        eax |= 0x4200000u;
        ecx <<= 0x17;
        eax |= ecx;
    }
    edi = s18;
    edx = s14;
    ecx = ebx & 0x48u;
    put_slot(buffer, ecx, edi, eax);
    put(buffer, (ecx == 0x48u ? 2u : 0u) + 1u, edi, edx);
    if ((ebx & 0x80u) == 0u) {
        /* The colour pass is done, run the alpha pass from states 0x10 to 0x13. */
        eax = s24;
        ecx = d3d8_guest_load32(eax + 0x44u);
        esi = d3d8_guest_load32(eax + 0x48u);
        ebp = d3d8_guest_load32(eax + 0x4Cu);
        eax = d3d8_guest_load32(eax + ALPHA_OP);
        edx = s2c;
        ebx |= 0xE8u;
        s10 = ecx;
        edi = ecx;
        goto dispatch;
    }
    /* 0x003E161F: the stage is done. */
    esi = s20;
    eax = s28;
    edi += 1u;
    esi += 1u;
    eax += STAGE_BYTES;
    s18 = edi;
    s20 = esi;
    s28 = eax;
    if (esi != 4u) {
        s24 = eax;
        eax = d3d8_guest_load32(eax + COLOR_OP);
        ebx = esi;
        if (eax != 1u) {
            goto loop_top;
        }
    }
    ebp = 0u;

fill: /* 0x003E1655 */
    esi -= start;
    eax = SLOTS - esi;
    if (eax == 0u || eax > SLOTS) {
        d3d8_hle_fatal(ENTRY, "combiner stage count %u would run the original's fill loop away",
                       (unsigned)esi);
        return;
    }
    do {
        put(buffer, 0u, edi, ebp);
        put(buffer, 1u, edi, ebp);
        put(buffer, 2u, edi, ebp);
        put(buffer, 3u, edi, ebp);
        edi += 1u;
        eax -= 1u;
    } while (eax != 0u);

    plan->words[0] = 0x00041E60u;
    plan->words[1] = esi;
    static const uint32_t headers[PACKETS] = {0x00200AC0u, 0x00201E40u, 0x00200260u, 0x00200AA0u};
    for (uint32_t packet = 0u; packet < PACKETS; packet++) {
        buffer[packet * PACKET_DWORDS] = headers[packet];
    }
    for (uint32_t index = 0u; index < PACKETS * PACKET_DWORDS; index++) {
        plan->words[2u + index] = buffer[index];
    }
    plan->count = 2u + PACKETS * PACKET_DWORDS;
    if (((flags ^ saved_flags) & FLAG_FOG) != 0u && d3d8_guest_load32(RS_FOG_PAIR) == 0u) {
        plan->words[plan->count++] = 0x000403B8u;
        plan->words[plan->count++] = (flags >> 6) & 1u;
    }
    plan->flags_after = flags;
    plan->dirty_after = dirty;
    if (((flags | saved_flags) & FLAG_PROGRAM) != 0u) {
        plan->dirty_after |= 0x400Fu;
    }
}

uint32_t d3d8_plan_fixed_function_combiner(uint32_t dirty, d3d8_pushbuffer_sim *sim)
{
    combiner_plan plan;
    combiner_build(dirty, &plan);
    d3d8_pushbuffer_sim_site(sim, ENTRY, plan.count * 4u);
    return plan.dirty_after;
}

uint32_t d3d8_emit_fixed_function_combiner(uint32_t dirty)
{
    combiner_plan plan;
    combiner_build(dirty, &plan);
    d3d8_device_store32(D3D8_DEV_FLAGS, plan.flags_after);
    const uint32_t cursor = d3d8_pushbuffer_begin();
    for (uint32_t index = 0u; index < plan.count; index++) {
        d3d8_guest_store32(cursor + index * 4u, plan.words[index]);
    }
    d3d8_pushbuffer_end(cursor + plan.count * 4u);
    return plan.dirty_after;
}
