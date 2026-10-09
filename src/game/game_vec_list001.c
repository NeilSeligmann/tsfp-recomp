/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1595: hand C drafts for the first 6 roots of docs/data/t1595-vector-lists/list-001.json (the
 * T1594 vector whitelist roots). Only the 2 roots that passed the batch gate are registered here
 * (00196170, 000C9F60). The exact file proved with all 6 drafts is kept verbatim under
 * docs/data/t1595-vec-list001/proved-all-6-drafts.c.txt. Drafted from the retail disassembly of the pinned XBE (sha256
 * 3cfd001a...). Every body is register-exact: all eight GPRs and EFLAGS are untouched unless
 * the original writes them, memory is read and written in the original order, and the XMM0
 * result is modelled through the runtime's raw 128-bit TLS slot (g_xmm0). `movss xmm, m32`
 * zeroes bits 32..127, `xorps xmm0, xmm0` clears all 128 bits. Only the whitelist forms
 * movss/xorps occur here. Names describe memory effects only.
 * Record: docs/t-vec-draft-list001.md. */
#include "game_replace.h"

typedef union GameVecXmm {
    float f[4];
    double d[2];
    uint32_t u[4];
    int32_t i[4];
    uint64_t q[2];
} GameVecXmm;

/* The runtime (src/host/recomp_runtime.c, tools/harness/runtime_min.c) owns the strong slot. This
 * weak definition only lets registry-only links (game_manifest, test_game_replacements) resolve
 * the symbol, exactly as tools/replace/manifest_vector_state.c does for the metadata build. */
__thread GameVecXmm g_xmm0 __attribute__((weak, aligned(8)));

/* movss xmm0, dword ptr [mem]: low lane loaded, lanes 1..3 zeroed. */
static void load_xmm0_scalar(uint32_t value)
{
    g_xmm0.u[0] = value;
    g_xmm0.u[1] = 0u;
    g_xmm0.u[2] = 0u;
    g_xmm0.u[3] = 0u;
}

/* 0x00196170: cdecl(). EAX = [0x4E8CEC]; xorps xmm0, xmm0; [0x756E7C] = EAX;
 * movss [0x74FE24], xmm0 (writes the zero low lane). */
GAME_REPLACE_EXACT(00196170, cdecl, 0, u32, game_copy_global_4e8cec_to_756e7c_and_clear_float_74fe24)
{
    g_eax = guest_read32(0x004E8CECu);
    g_xmm0.q[0] = 0u;
    g_xmm0.q[1] = 0u;
    guest_write32(0x00756E7Cu, g_eax);
    guest_write32(0x0074FE24u, g_xmm0.u[0]);
}

/* 0x000C9F60: cdecl(float a, float b). movss xmm0, [esp+4]; movss [0x7330AC], xmm0;
 * movss xmm0, [esp+8]; movss [0x7330B0], xmm0. XMM0 ends holding b zero-extended. */
GAME_REPLACE_EXACT(000C9F60, cdecl, 2, u32, game_store_float_arg_pair_to_7330ac_7330b0)
{
    load_xmm0_scalar(game_stack_arg(0u));
    guest_write32(0x007330ACu, g_xmm0.u[0]);
    load_xmm0_scalar(game_stack_arg(1u));
    guest_write32(0x007330B0u, g_xmm0.u[0]);
}
