/* SPDX-License-Identifier: GPL-3.0-or-later
 * MD5 update and audio-decoder Huffman symbol walk (T1480 eighth batch). Keep retail live
 * stack frames, access widths and nested-call register handoffs. */
#include "game_replace.h"
extern __thread uint32_t g_ebx, g_ebp, g_esi, g_edi;

static uint16_t read16(uint32_t address)
{
    uint16_t value;
    memcpy(&value, game_host_ptr(address), sizeof value);
    return value;
}
static void write16(uint32_t address, uint16_t value)
{
    memcpy(game_host_ptr(address), &value, sizeof value);
}

static void call(uint32_t target, uint32_t pc, uint32_t live,
                 const uint32_t *args, unsigned count)
{
    if (game_guest_call(target, GAME_CC_cdecl, pc, live, 0u, 0u, args, count) != GAME_GUEST_CALL_OK)
        __builtin_trap();
}

GAME_REPLACE_EXACT(003BC2A0, cdecl, 3, u32, game_fesl_md5_update_with_data_pointer_length_or_zero_terminated_when_negative)
{
    g_ecx = game_stack_arg(2u);
    g_eax = game_stack_arg(1u);
    guest_write32(g_esp - 4u, g_esi);
    guest_write32(g_esp - 8u, g_edi);
    g_edi = g_eax;
    if ((int32_t)g_ecx < 0) {
        g_edx = (g_edx & 0xFFFFFF00u) | guest_read8(g_eax);
        g_ecx = 0u;
        while ((g_edx & 255u) != 0u) {
            g_edx = (g_edx & 0xFFFFFF00u) | guest_read8(g_eax + g_ecx + 1u);
            ++g_ecx;
        }
    }
    g_esi = game_stack_arg(0u);
    g_eax = guest_read32(g_esi) & 63u;
    if ((int32_t)g_ecx > 0) {
        guest_write32(g_esp - 12u, g_ebx);
        g_ebx = g_ecx;
        do {
            g_ecx = (g_ecx & 0xFFFFFF00u) | guest_read8(g_edi);
            guest_write8(g_esi + g_eax + 20u, (uint8_t)g_ecx);
            g_edx = guest_read32(g_esi);
            ++g_eax;
            ++g_edi;
            ++g_edx;
            guest_write32(g_esi, g_edx);
            if (g_eax == 64u) {
                const uint32_t args[] = {g_esi};
                call(0x003BB9C0u, 0x003BC2E9u, 12u, args, 1u);
                g_eax = 0u;
            }
            --g_ebx;
        } while (g_ebx != 0u);
        g_ebx = guest_read32(g_esp - 12u);
    }
    g_edi = guest_read32(g_esp - 8u);
    g_esi = guest_read32(g_esp - 4u);
}

GAME_REPLACE_EXACT(0038F565, cdecl, 6, u32, game_audio_decoder_huffman_decode_symbol_from_bitstream)
{
    guest_write32(g_esp - 4u, g_ebp);
    g_ebp = g_esp - 4u;
    g_eax = game_stack_arg(1u);
    guest_write32(g_esp - 8u, g_ebx);
    g_ebx = game_stack_arg(4u);
    guest_write32(g_esp - 12u, g_esi);
    guest_write32(g_esp - 16u, g_edi);
    g_edi = game_stack_arg(0u);
    g_edx = 2u;
    guest_write32(g_esp + 4u, g_eax);
    for (;;) {
        g_ecx = guest_read32(g_ebx);
        g_eax = (uint32_t)(int32_t)(int16_t)g_edx;
        g_esi = g_ecx + g_eax;
        if ((int32_t)game_stack_arg(5u) < (int32_t)g_esi) {
            g_eax = 0x80040004u;
            break;
        }
        const uint32_t args[] = {game_stack_arg(3u), g_ecx, g_eax};
        /* The registered bit-reader preserves registers but omits its dead saved
         * stack words. Data may alias those words, so materialize the original
         * callee prologue before its adapter executes. Width is in [0,7]. */
        guest_write32(g_esp - 36u, g_ebp);
        guest_write32(g_esp - 40u, g_esi);
        if (g_eax != 0u) {
            guest_write32(g_esp - 44u, g_ebx);
            guest_write32(g_esp - 48u, g_edi);
        }
        call(0x0038F510u, 0x0038F591u, 16u, args, 3u);
        g_eax = (uint32_t)(int32_t)(int16_t)g_eax;
        g_eax = g_edi + g_eax * 2u;
        g_ecx = 0u;
        guest_write32(g_ebx, g_esi);
        g_ecx = read16(g_eax);
        if ((g_ecx & 0x8000u) == 0u) {
            g_edx = (g_ecx >> 12u) & 7u;
            g_esi = g_ecx & 4095u;
            const uint32_t levels = game_stack_arg(0u) - 1u;
            guest_write32(g_esp + 4u, levels);
            g_edi = g_eax + g_esi * 2u;
            if ((int16_t)levels > 0)
                continue;
        }
        g_eax = game_stack_arg(2u);
        g_ecx &= 0xFFFF0FFFu;
        write16(g_eax, (uint16_t)g_ecx);
        g_eax = 0u;
        break;
    }
    g_edi = guest_read32(g_esp - 16u);
    g_esi = guest_read32(g_esp - 12u);
    g_ebx = guest_read32(g_esp - 8u);
    g_ebp = guest_read32(g_esp - 4u);
}
