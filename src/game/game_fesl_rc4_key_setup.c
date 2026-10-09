/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
extern __thread uint32_t g_ebx,g_ebp,g_edi;
#define AL(v) (g_eax=(g_eax&0xffffff00u)|(uint8_t)(v))
#define DL(v) (g_edx=(g_edx&0xffffff00u)|(uint8_t)(v))
#define BL(v) (g_ebx=(g_ebx&0xffffff00u)|(uint8_t)(v))
GAME_REPLACE_EXACT(003C6730,cdecl,4,u32,game_fesl_rc4_key_setup_two_index_bytes_and_256_byte_state_with_key_pointer_and_length)
{
    const uint32_t sp=g_esp;
    g_ecx=game_stack_arg(3u);
    if((int32_t)g_ecx<1)g_ecx=1;
    guest_write32(sp-16,g_ebx);guest_write32(sp-20,g_ebp);
    g_ebx=0;guest_write32(sp-24,g_esi);
    g_esi=game_stack_arg(0u);
    guest_write8(g_esi,0);guest_write8(g_esi+1,0);
    g_eax=0;
    do {guest_write8(g_esi+g_eax+2,(uint8_t)g_eax);++g_eax;}while(g_eax<256);
    g_ebp=game_stack_arg(2u);
    if((int32_t)g_ebp>0 && (int32_t)g_ecx>0){
        g_eax=0xffffffffu-g_esi;guest_write32(sp-12,g_eax);
        g_eax=1-g_esi;guest_write32(sp-8,g_eax);
        g_eax=0xfffffffeu-g_esi;
        guest_write32(sp-28,g_edi);g_edi=game_stack_arg(1u);
        guest_write32(sp-4,g_eax);guest_write32(sp+12,g_ecx);
        do {
            guest_write32(sp+4,2);g_ecx=g_esi+2;
            do {
                DL(guest_read8(g_ecx));guest_write8(sp+16,(uint8_t)g_edx);
                g_eax+=g_ecx;g_edx=g_eax%g_ebp;g_eax/=g_ebp;
                AL(guest_read8(g_edx+g_edi));AL(g_eax+guest_read8(sp+16));BL(g_ebx+g_eax);
                g_edx=(uint8_t)g_ebx;g_eax=g_edx+g_esi+2;
                DL(guest_read8(g_eax));guest_write8(g_ecx,(uint8_t)g_edx);
                DL(guest_read8(sp+16));guest_write8(g_eax,(uint8_t)g_edx);
                AL(guest_read8(g_ecx+1));g_edx=guest_read32(sp-12);guest_write8(sp+16,(uint8_t)g_eax);
                g_eax=g_edx+g_ecx;g_edx=g_eax%g_ebp;g_eax/=g_ebp;
                AL(guest_read8(g_edx+g_edi));AL(g_eax+guest_read8(sp+16));BL(g_ebx+g_eax);
                g_edx=(uint8_t)g_ebx;g_eax=g_edx+g_esi+2;
                DL(guest_read8(g_eax));guest_write8(g_ecx+1,(uint8_t)g_edx);
                DL(guest_read8(sp+16));guest_write8(g_eax,(uint8_t)g_edx);
                AL(guest_read8(g_ecx+2));guest_write8(sp+16,(uint8_t)g_eax);g_eax=guest_read32(sp+4);
                g_edx=g_eax%g_ebp;g_eax/=g_ebp;
                AL(guest_read8(sp+16));DL(guest_read8(g_edx+g_edi));DL(g_edx+g_eax);BL(g_ebx+g_edx);
                g_eax=(uint8_t)g_ebx;DL(guest_read8(g_eax+g_esi+2));g_eax+=g_esi+2;
                guest_write8(g_ecx+2,(uint8_t)g_edx);DL(guest_read8(sp+16));guest_write8(g_eax,(uint8_t)g_edx);
                AL(guest_read8(g_ecx+3));g_edx=guest_read32(sp-8);guest_write8(sp+16,(uint8_t)g_eax);
                g_eax=g_edx+g_ecx;g_edx=g_eax%g_ebp;g_eax/=g_ebp;g_ecx+=4;
                AL(guest_read8(g_edx+g_edi));AL(g_eax+guest_read8(sp+16));BL(g_ebx+g_eax);
                g_edx=(uint8_t)g_ebx;g_eax=g_edx+g_esi+2;DL(guest_read8(g_eax));
                guest_write8(g_ecx-1,(uint8_t)g_edx);DL(guest_read8(sp+16));guest_write8(g_eax,(uint8_t)g_edx);
                guest_write32(sp+4,guest_read32(sp+4)+4);g_eax=guest_read32(sp-4);g_edx=g_eax+g_ecx;
            }while(g_edx<256);
            guest_write32(sp+12,guest_read32(sp+12)-1);
        }while(guest_read32(sp+12)!=0);
        g_edi=guest_read32(sp-28);
    }
    g_esi=guest_read32(sp-24);g_ebp=guest_read32(sp-20);g_ebx=guest_read32(sp-16);
}
