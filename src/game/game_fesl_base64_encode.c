/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
extern __thread uint32_t g_ebx, g_edi, g_ebp;
/* The encoder has no semantic return value. Callers do not consume scratch
 * registers; original ordered table reads/writes and caller count slot retained. */
static void game_fesl_base64_encode_bytes_into_text_for_content_field(uint32_t n, uint32_t src, uint32_t dst)
{
    guest_write32(g_esp-4u,g_ebx);
    guest_write32(g_esp-8u,g_esi);
    guest_write32(g_esp-12u,g_edi);
    uint32_t in=0,out=0;
    if ((int32_t)n>=3) {
        guest_write32(g_esp-16u,g_ebp);
        uint32_t groups=n/3;
        n-=groups*3;
        guest_write32(g_esp+4u,n);
        do {
            uint32_t a=guest_read8(src+in);
            guest_write8(dst+out,guest_read8(0x4b5de8u+(a>>2)));
            uint32_t b=guest_read8(src+in+1u);
            a=guest_read8(src+in);
            guest_write8(dst+out+1u,guest_read8(0x4b5de8u+(b>>4)+((a&3u)<<4)));
            uint32_t c=guest_read8(src+in+2u);
            b=guest_read8(src+in+1u);
            guest_write8(dst+out+2u,guest_read8(0x4b5de8u+((b&15u)<<2)+(c>>6)));
            c=guest_read8(src+in+2u);
            guest_write8(dst+out+3u,guest_read8(0x4b5de8u+(c&63u)));
            in+=3;out+=4;
        } while (--groups);
        n=guest_read32(g_esp+4u);
        g_ebp=guest_read32(g_esp-16u);
    }
    if (n==2) {
        uint32_t a=guest_read8(src+in);
        guest_write8(dst+out,guest_read8(0x4b5de8u+(a>>2)));
        uint32_t b=guest_read8(src+in+1u);
        a=guest_read8(src+in);
        guest_write8(dst+out+1u,guest_read8(0x4b5de8u+(b>>4)+((a&3u)<<4)));
        b=guest_read8(src+in+1u);
        guest_write8(dst+out+2u,guest_read8(0x4b5de8u+((b&15u)<<2)));
        guest_write8(dst+out+3u,'=');out+=4;
    } else if(n==1) {
        uint32_t a=guest_read8(src+in);
        guest_write8(dst+out,guest_read8(0x4b5de8u+(a>>2)));
        a=guest_read8(src+in);
        guest_write8(dst+out+1u,guest_read8(0x4b5de8u+((a&3u)<<4)));
        guest_write8(dst+out+2u,'=');guest_write8(dst+out+3u,'=');out+=4;
    }
    guest_write8(dst+out,0);
    g_edi=guest_read32(g_esp-12u);g_esi=guest_read32(g_esp-8u);g_ebx=guest_read32(g_esp-4u);
}
GAME_REPLACE(003BA270,cdecl,3,void,game_fesl_base64_encode_bytes_into_text_for_content_field)
