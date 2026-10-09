/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1596: hand C drafts for functions 7-14 of docs/data/t1595-vector-lists/list-001.json (the
 * T1594 vector whitelist roots, after the 6 of game_vec_list001.c). Only the 3 roots that pass the batch
 * gate (00065230, 0024C920, 0010E970) stay registered here, the exact file proved with all 8 drafts is kept verbatim under
 * docs/data/t1596-vec-list001b/proved-all-8-drafts.c.txt. Drafted from the retail disassembly of
 * the pinned XBE. Register-exact: GPRs untouched unless the original writes them, memory is
 * read and written in the original order, XMM0 is modelled through the runtime raw 128-bit TLS
 * slot g_xmm0 (movss load zeroes lanes 1..3, xorps xmm0,xmm0 clears all 128 bits). Names
 * describe memory effects only. Record: docs/t-vec-draft-list001b.md. */
#include "game_replace.h"

typedef union GameVecXmm {
    float f[4];
    double d[2];
    uint32_t u[4];
    int32_t i[4];
    uint64_t q[2];
} GameVecXmm;

/* Strong slot lives in the runtime, this weak copy only lets registry-only links resolve. */
__thread GameVecXmm g_xmm0 __attribute__((weak, aligned(8)));

/* movss xmm0, dword ptr [mem]: low lane loaded, lanes 1..3 zeroed. */
static void vb_load_xmm0_scalar(uint32_t value)
{
    g_xmm0.u[0] = value;
    g_xmm0.u[1] = 0u;
    g_xmm0.u[2] = 0u;
    g_xmm0.u[3] = 0u;
}

/* xorps xmm0, xmm0 */
static void vb_clear_xmm0(void)
{
    g_xmm0.q[0] = 0u;
    g_xmm0.q[1] = 0u;
}

/* 0x00065230: cdecl(u32 v). EAX=v; if v != [0x7DE30C]: xmm0=0; [0x7DE30C]=v; [0x7DE300]=0. */
GAME_REPLACE_EXACT(00065230, cdecl, 1, u32, game_set_7de30c_clear_float_7de300)
{
    g_eax = game_stack_arg(0u);
    if (g_eax == guest_read32(0x007DE30Cu)) {
        return;
    }
    vb_clear_xmm0();
    guest_write32(0x007DE30Cu, g_eax);
    guest_write32(0x007DE300u, g_xmm0.u[0]);
}

/* 0x0024C920: cdecl(). xorps xmm0,xmm0; [78B600]=4; [78B664]=0xA; [78B668]=0; [78B66C]=0;
 * [78B670]=0. */
GAME_REPLACE_EXACT(0024C920, cdecl, 0, u32, game_reset_state_block_78b600)
{
    vb_clear_xmm0();
    guest_write32(0x0078B600u, 4u);
    guest_write32(0x0078B664u, 0x0Au);
    guest_write32(0x0078B668u, g_xmm0.u[0]);
    guest_write32(0x0078B66Cu, 0u);
    guest_write32(0x0078B670u, g_xmm0.u[0]);
}

/* 0x0010E970: cdecl(float a). EAX=[0x7AD28C]; if EAX: [EAX+0xAC]=a; EAX=[0x7AD28C];
 * [EAX+4] |= 0x20000. */
GAME_REPLACE_EXACT(0010E970, cdecl, 1, u32, game_set_obj_7ad28c_float_ac_flag_20000)
{
    g_eax = guest_read32(0x007AD28Cu);
    if (g_eax == 0u) {
        return;
    }
    vb_load_xmm0_scalar(game_stack_arg(0u));
    guest_write32(g_eax + 0xACu, g_xmm0.u[0]);
    g_eax = guest_read32(0x007AD28Cu);
    guest_write32(g_eax + 4u, guest_read32(g_eax + 4u) | 0x20000u);
}
