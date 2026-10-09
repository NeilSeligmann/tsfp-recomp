/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1597: hand C drafts for the 13 roots of docs/data/t1595-vector-lists/list-002.json (the
 * T1594 vector whitelist roots). Only the 8 roots that passed the batch gate are registered here (00309080,
 * 003090D0, 00046210, 000425E0, 00362CC0, 0012C130, 00119AB0, 00164960); the
 * file proved with all 13 bodies is kept verbatim under
 * docs/data/t1597-vec-list003/proved-all-13-drafts.c.txt. Drafted from the retail disassembly
 * of the pinned XBE (sha256 3cfd001a...). Every body is register-exact: GPRs are only changed
 * where the original writes them, memory is read and written in the original order, and the
 * XMM registers are modelled through the runtime's raw 128-bit TLS slots (g_xmm0).
 * `movss xmm, m32` zeroes bits 32..127, `xorps xmm, xmm` clears all 128 bits, `movss m32, xmm`
 * writes the low lane. Names describe memory effects only.
 * Record: docs/t-vec-draft-list003.md. */
#include "game_replace.h"

extern __thread uint32_t g_edi;

typedef union GameVecXmm3 {
    float f[4];
    double d[2];
    uint32_t u[4];
    int32_t i[4];
    uint64_t q[2];
} GameVecXmm3;

/* Weak storage so registry-only links resolve the symbols (see game_vec_list001.c). */
__thread GameVecXmm3 g_xmm0 __attribute__((weak, aligned(8)));

/* movss xmm0, dword ptr [mem]: low lane loaded, lanes 1..3 zeroed. */
static void vec3_load0(uint32_t address)
{
    g_xmm0.u[0] = guest_read32(address);
    g_xmm0.u[1] = 0u;
    g_xmm0.u[2] = 0u;
    g_xmm0.u[3] = 0u;
}

/* xorps xmm0, xmm0. */
static void vec3_clear0(void)
{
    g_xmm0.q[0] = 0u;
    g_xmm0.q[1] = 0u;
}

/* 0x00309080: cdecl(). Copies words 783F2C, 783F28, 783DD4, 783F20 to 762D58, 762D5C, 762D60,
 * 762D64 in that order. XMM0 ends with the last word. */
GAME_REPLACE_EXACT(00309080, cdecl, 0, u32, game_copy_four_floats_783f_to_762d)
{
    vec3_load0(0x00783F2Cu);
    guest_write32(0x00762D58u, g_xmm0.u[0]);
    vec3_load0(0x00783F28u);
    guest_write32(0x00762D5Cu, g_xmm0.u[0]);
    vec3_load0(0x00783DD4u);
    guest_write32(0x00762D60u, g_xmm0.u[0]);
    vec3_load0(0x00783F20u);
    guest_write32(0x00762D64u, g_xmm0.u[0]);
}

/* 0x003090D0: cdecl(). Inverse of 00309080: 762D58, 762D5C, 762D60, 762D64 back to 783F2C,
 * 783F28, 783DD4, 783F20. */
GAME_REPLACE_EXACT(003090D0, cdecl, 0, u32, game_copy_four_floats_762d_to_783f)
{
    vec3_load0(0x00762D58u);
    guest_write32(0x00783F2Cu, g_xmm0.u[0]);
    vec3_load0(0x00762D5Cu);
    guest_write32(0x00783F28u, g_xmm0.u[0]);
    vec3_load0(0x00762D60u);
    guest_write32(0x00783DD4u, g_xmm0.u[0]);
    vec3_load0(0x00762D64u);
    guest_write32(0x00783F20u, g_xmm0.u[0]);
}

/* 0x00046210: cdecl(). 4D1838 = [4785D4]; 4D1844 = [4785C8]; x = [475C78] stored to 4D183C,
 * 4D1840, 4D1848; [6D5EC0] = 0. XMM0 ends with x. */
GAME_REPLACE_EXACT(00046210, cdecl, 0, u32, game_init_float_block_4d1838)
{
    vec3_load0(0x004785D4u);
    guest_write32(0x004D1838u, g_xmm0.u[0]);
    vec3_load0(0x004785C8u);
    guest_write32(0x004D1844u, g_xmm0.u[0]);
    vec3_load0(0x00475C78u);
    guest_write32(0x004D183Cu, g_xmm0.u[0]);
    guest_write32(0x004D1840u, g_xmm0.u[0]);
    guest_write32(0x004D1848u, g_xmm0.u[0]);
    guest_write32(0x006D5EC0u, 0u);
}

/* 0x000425E0: cdecl(). If [7DE458] bit 23 is clear: EAX = [6B98A4]; when EAX is 3 or 5,
 * [6B98B0] = [475D18] (XMM0 loaded) and [6B98A4] = 5. EAX keeps the value read. */
GAME_REPLACE_EXACT(000425E0, cdecl, 0, u32, game_promote_mode_3_or_5_to_5)
{
    if ((guest_read32(0x007DE458u) & 0x00800000u) != 0u)
        return;
    g_eax = guest_read32(0x006B98A4u);
    if (g_eax != 3u && g_eax != 5u)
        return;
    vec3_load0(0x00475D18u);
    guest_write32(0x006B98B0u, g_xmm0.u[0]);
    guest_write32(0x006B98A4u, 5u);
}

/* 0x00362CC0: cdecl(). XMM0 = 0; copies 0xFB words from 0x546304 to 0x7738A0 (rep movsd,
 * ESI/EDI saved and restored through the stack, ECX ends 0); EAX = 0; [7736E0] = [7736E4] =
 * [773700] = 0; [76C0B4] = 1; [77388C] = 0. */
GAME_REPLACE_EXACT(00362CC0, cdecl, 0, u32, game_load_table_546304_to_7738a0)
{
    const uint32_t entry = g_esp;
    vec3_clear0();
    guest_write32(entry - 4u, g_esi);
    guest_write32(entry - 8u, g_edi);
    for (uint32_t index = 0u; index < 0xFBu; index++)
        guest_write32(0x007738A0u + 4u * index, guest_read32(0x00546304u + 4u * index));
    g_ecx = 0u;
    g_eax = 0u;
    guest_write32(0x007736E0u, g_eax);
    guest_write32(0x007736E4u, g_eax);
    guest_write32(0x00773700u, g_eax);
    guest_write32(0x0076C0B4u, 1u);
    guest_write32(0x0077388Cu, g_xmm0.u[0]);
}

/* 0x0012C130: cdecl(). Loads floats from 475C78, 4E6680..4E6694 into 7ABF7C, 7ABF64..7ABF78;
 * [7ABF60] = 0x4E6678 is written before the last float store. XMM0 ends with [4E6694]. */
GAME_REPLACE_EXACT(0012C130, cdecl, 0, u32, game_init_float_block_7abf60)
{
    vec3_load0(0x00475C78u);
    guest_write32(0x007ABF7Cu, g_xmm0.u[0]);
    vec3_load0(0x004E6680u);
    guest_write32(0x007ABF64u, g_xmm0.u[0]);
    vec3_load0(0x004E6684u);
    guest_write32(0x007ABF68u, g_xmm0.u[0]);
    vec3_load0(0x004E6688u);
    guest_write32(0x007ABF6Cu, g_xmm0.u[0]);
    vec3_load0(0x004E668Cu);
    guest_write32(0x007ABF70u, g_xmm0.u[0]);
    vec3_load0(0x004E6690u);
    guest_write32(0x007ABF74u, g_xmm0.u[0]);
    vec3_load0(0x004E6694u);
    guest_write32(0x007ABF60u, 0x004E6678u);
    guest_write32(0x007ABF78u, g_xmm0.u[0]);
}

/* 0x00119AB0: cdecl(a, b). a == 0: [7AC800] = 1, [7AC804] = 0x4E63F8, EAX = 0; else
 * [7AC800] = a, EAX = b, [7AC804] = b. Then x = [47D580] to 7AC814, 7AC818, 7AC81C; XMM0 = 0;
 * [7AC820] = 0; [7AC808] = [7AC80C] = ECX = 0; [7AC810] = 0. */
GAME_REPLACE_EXACT(00119AB0, cdecl, 2, u32, game_init_state_block_7ac800)
{
    g_eax = game_stack_arg(0u);
    g_ecx = 0u;
    if (g_eax == 0u) {
        guest_write32(0x007AC800u, 1u);
        guest_write32(0x007AC804u, 0x004E63F8u);
    } else {
        guest_write32(0x007AC800u, g_eax);
        g_eax = game_stack_arg(1u);
        guest_write32(0x007AC804u, g_eax);
    }
    vec3_load0(0x0047D580u);
    guest_write32(0x007AC814u, g_xmm0.u[0]);
    guest_write32(0x007AC818u, g_xmm0.u[0]);
    guest_write32(0x007AC81Cu, g_xmm0.u[0]);
    vec3_clear0();
    guest_write32(0x007AC820u, g_xmm0.u[0]);
    guest_write32(0x007AC808u, g_ecx);
    guest_write32(0x007AC80Cu, g_ecx);
    guest_write32(0x007AC810u, g_xmm0.u[0]);
}

/* 0x00164960: cdecl(). Clears 74C3F0, 74C3EC, 74C400, 74C404, 74C3E4; sets 74C400 and 74C3E4
 * to -1; then if [79094C] != 4: 74C3E8 = 0, [74C3CC] = [47CFF4], [74C408] = [475D28]; else
 * [74C3CC] = [475D28], [74C408] = [47D1F4]. Leaves EAX = -1, ECX = 0. */
GAME_REPLACE_EXACT(00164960, cdecl, 0, u32, game_reset_state_block_74c3cc)
{
    g_eax = 0u;
    guest_write32(0x0074C3F0u, g_eax);
    guest_write32(0x0074C3ECu, g_eax);
    guest_write32(0x0074C400u, g_eax);
    guest_write32(0x0074C404u, g_eax);
    g_eax = 0xFFFFFFFFu;
    g_ecx = 0u;
    guest_write32(0x0074C3E4u, g_ecx);
    guest_write32(0x0074C400u, g_eax);
    guest_write32(0x0074C3E4u, g_eax);
    const uint32_t mode = guest_read32(0x0079094Cu);
    guest_write32(0x0074C3E8u, g_ecx);
    if (mode != 4u) {
        vec3_load0(0x0047CFF4u);
        guest_write32(0x0074C3CCu, g_xmm0.u[0]);
        vec3_load0(0x00475D28u);
        guest_write32(0x0074C408u, g_xmm0.u[0]);
    } else {
        vec3_load0(0x00475D28u);
        guest_write32(0x0074C3CCu, g_xmm0.u[0]);
        vec3_load0(0x0047D1F4u);
        guest_write32(0x0074C408u, g_xmm0.u[0]);
    }
}
