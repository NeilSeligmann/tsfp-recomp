/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>
#include "recomp_x87_local.h"
int main(void) {
    const uint16_t words[] = {0x37f,0x27f,0x7f,0x47f,0xc7f,0x87f};
    unsigned errors = 0;
    const uint16_t invalid[] = {0x037e,0x017f,0x137f,0x83ff,0x033f};
    for (unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) errors += recomp_x87_local_available(invalid[i]);
    for (unsigned i=0;i<6;i++) errors += !recomp_x87_local_available(words[i]);
    RecompX87HostState original, before, after;
    __asm__ volatile("fxsave64 %0; fninit; fld1; fldpi; fxsave64 %1" : "=m"(original), "=m"(before) : : "memory");
    RecompX87Value probe=recomp_x87_load32(0x3f800000,0x37f);
    unsigned relation=recomp_x87_comip(probe,probe,0x37f);
    __asm__ volatile("fxsave64 %0" : "=m"(after) : : "memory");
    /* x87 environment and eight 16-byte physical-register slots only.
       XMM caller-saved registers may legitimately change through C calls. */
    unsigned host_changed=memcmp(before.bytes,after.bytes,160)!=0;
    __asm__ volatile("fxrstor64 %0" : : "m"(original) : "memory");
    printf("host-x87-state unchanged=%u equality=%02x\n",!host_changed,relation);
    errors += host_changed || relation != 0x40;
    for (unsigned i=0;i<6;i++) {
        uint16_t cw=words[i];
        RecompX87Value one=recomp_x87_load32(0x3f800000,cw), small=recomp_x87_load32(0x39800000,cw);
        RecompX87Value value=recomp_x87_sqrt(recomp_x87_addp(recomp_x87_mul(one,one,cw),recomp_x87_mul(small,small,cw),cw),cw);
        unsigned flags=recomp_x87_comip(value,one,cw), expected=(i==2||i==3||i==4)?0x40:0;
        printf("%04x flags=%02x expected=%02x\n",cw,flags,expected);
        errors += flags != expected;
    }
    /* This f32-input cone distinguishes genuine PC64 from binary64.
       sqrt(1 + 2^-62) rounds above one in PC64 and to one in PC53. */
    RecompX87Value one64=recomp_x87_load32(0x3f800000,0x37f);
    RecompX87Value tiny64=recomp_x87_load32(0x30000000,0x37f);
    RecompX87Value norm64=recomp_x87_sqrt(recomp_x87_addp(
        recomp_x87_mul(one64,one64,0x37f),
        recomp_x87_mul(tiny64,tiny64,0x37f),0x37f),0x37f);
    RecompX87Value norm53=recomp_x87_sqrt(recomp_x87_addp(
        recomp_x87_mul(one64,one64,0x27f),
        recomp_x87_mul(tiny64,tiny64,0x27f),0x27f),0x27f);
    unsigned flags64=recomp_x87_comip(norm64,one64,0x37f);
    unsigned flags53=recomp_x87_comip(norm53,one64,0x27f);
    printf("binary80 distinction pc64=%02x pc53=%02x\n",flags64,flags53);
    errors += flags64 != 0 || flags53 != 0x40;
    return errors!=0;
}
