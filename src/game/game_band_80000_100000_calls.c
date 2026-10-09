/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1476 real guest-callee state snapshot; measured four-run live-closure proof.
 * Receipt: docs/t77-band-80000-100000-calls.md.
 */
#include "game_replace.h"

static void call_band_leaf(uint32_t target, uint32_t return_va,
                           uint32_t local_bytes, const uint32_t *args, unsigned count)
{
    if (game_guest_call(target, GAME_CC_cdecl, return_va, local_bytes,
                        0u, 0u, args, count) != GAME_GUEST_CALL_OK) {
        __builtin_trap();
    }
}

GAME_REPLACE_EXACT(0008F470, cdecl, 0, u32,
                   game_d3d_save_current_state_group_blend_mode_and_alpha)
{
    call_band_leaf(0x00019640u, 0x0008F475u, 0u, NULL, 0u);
    guest_write32(0x004D9708u, g_eax);
    call_band_leaf(0x00019D50u, 0x0008F47Fu, 0u, NULL, 0u);
    guest_write32(0x004D970Cu, g_eax);
    call_band_leaf(0x00019D60u, 0x0008F489u, 0u, NULL, 0u);
    guest_write32(0x004D9710u, g_eax);
}

/* Earlier pushed arguments remain live until the final caller cleanup.
 * Reserve their original depth for each subsequent real guest call. */
GAME_REPLACE_EXACT(000AE8B0, cdecl, 0, u32, game_register_callbacks_mask_0x80)
{
    const uint32_t args0[3] = {0x00000080u, 0x00000000u, 0x000AC940u};
    call_band_leaf(0x0009E2D0u, 0x000AE8C1u, 0u, args0, 3u);
    const uint32_t args1[3] = {0x00000080u, 0x00000001u, 0x000A92F0u};
    call_band_leaf(0x0009E2D0u, 0x000AE8D2u, 12u, args1, 3u);
}

GAME_REPLACE_EXACT(000AEA20, cdecl, 0, u32, game_register_callbacks_mask_0x4000000)
{
    const uint32_t args0[3] = {0x04000000u, 0x00000000u, 0x000AE940u};
    call_band_leaf(0x0009E2D0u, 0x000AEA31u, 0u, args0, 3u);
    const uint32_t args1[3] = {0x04000000u, 0x00000003u, 0x000AE9B0u};
    call_band_leaf(0x0009E2D0u, 0x000AEA42u, 12u, args1, 3u);
    const uint32_t args2[3] = {0x04000000u, 0x00000001u, 0x000AE980u};
    call_band_leaf(0x0009E2D0u, 0x000AEA53u, 24u, args2, 3u);
}

GAME_REPLACE_EXACT(000B2FA0, cdecl, 0, u32, game_register_callbacks_masks_0x1000_0x100000)
{
    const uint32_t args0[3] = {0x00001000u, 0x00000001u, 0x000B2D70u};
    call_band_leaf(0x0009E2D0u, 0x000B2FB1u, 0u, args0, 3u);
    const uint32_t args1[3] = {0x00100000u, 0x00000001u, 0x000B2DB0u};
    call_band_leaf(0x0009E2D0u, 0x000B2FC2u, 12u, args1, 3u);
}

GAME_REPLACE_EXACT(000C3510, cdecl, 0, u32, game_register_callbacks_mask_0x800)
{
    const uint32_t args0[3] = {0x00000800u, 0x00000001u, 0x000C2E70u};
    call_band_leaf(0x0009E2D0u, 0x000C3521u, 0u, args0, 3u);
    const uint32_t args1[3] = {0x00000800u, 0x00000004u, 0x000C30A0u};
    call_band_leaf(0x0009E2D0u, 0x000C3532u, 12u, args1, 3u);
}

GAME_REPLACE_EXACT(000D9210, cdecl, 0, u32, game_register_callbacks_mask_0x8)
{
    const uint32_t args0[3] = {0x00000008u, 0x00000003u, 0x000D8E30u};
    call_band_leaf(0x0009E2D0u, 0x000D921Eu, 0u, args0, 3u);
    const uint32_t args1[3] = {0x00000008u, 0x00000004u, 0x000D9030u};
    call_band_leaf(0x0009E2D0u, 0x000D922Cu, 12u, args1, 3u);
}

/* Original constant free-list initializer; its unregistered call-free callee
 * runs through live guest dispatch. Original PUSH order is count/storage/
 * stride/pool, so the cdecl argument array is pool/stride/storage/count. */
GAME_REPLACE_EXACT(000AE910, cdecl, 0, u32, game_pool_7329f0_init)
{
    const uint32_t args[4] = {0x007329F0u, 0x24u, 0x007327B0u, 0x10u};
    call_band_leaf(0x00156DB0u, 0x000AE923u, 0u, args, 4u);
}

/* Signed selection gate and independent modulo32 table index. The original
 * text-bank leaf executes through live guest dispatch; no range is invented. */
GAME_REPLACE_EXACT(00082860, cdecl, 0, u32,
                   game_text_string_by_selected_table_entry)
{
    g_eax = guest_read32(0x007B9798u);
    if ((int32_t)g_eax < 0 || (int32_t)g_eax >= 5) {
        g_eax = 0x004766D8u;
        return;
    }
    g_eax = guest_read32(0x007B9794u);
    g_eax = g_eax + g_eax * 4u;
    g_ecx = guest_read32(0x004D9690u + g_eax * 4u);
    const uint32_t args[1] = {g_ecx};
    call_band_leaf(0x0003D7B0u, 0x00082883u, 0u, args, 1u);
}
