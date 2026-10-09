/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native state-contract controls for the frozen T1786 drafts, including rejected roots.
 * These controls are not equivalence admissions; original-vs-subject tuples decide those. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "game_replace.h"
ptrdiff_t g_xbox_mem_offset;
__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_esi, g_edi;
__thread int g_df;
#include "../../docs/data/t1786-data/all-12-drafts.c.txt"
static unsigned checks;
#define CHECK(x) do { assert(x); ++checks; } while (0)
static void reset(void)
{
    g_eax=0x11223344; g_ecx=0x55667788; g_edx=0x99AABBCC;
    g_esi=0x12345678; g_edi=0x87654321; g_esp=0x100000; g_df=0;
}
static void common(uint32_t ecx, uint32_t edx)
{
    CHECK(g_esp==0x100004); CHECK(g_ecx==ecx); CHECK(g_edx==edx);
    CHECK(g_esi==0x12345678); CHECK(g_edi==0x87654321);
}
int main(void)
{
    void *memory=calloc(1,0x1000000); assert(memory);
    g_xbox_mem_offset=(ptrdiff_t)(uintptr_t)memory;
    const uint32_t edges[]={0,1,0xFFFFFFFFu,0x80000000u,0x76DB78u};
    for(unsigned i=0;i<5;++i) {
        uint32_t v=edges[i];
        reset(); guest_write32(0x6F11BC,v); sub_000665C0();
        CHECK(g_eax==(v==0)); common(v,0x99AABBCC);
        reset(); guest_write32(0x75EAF8,v); sub_00246D10();
        CHECK(g_eax==(v==0)); common(v,0x99AABBCC);
        reset(); guest_write32(g_esp+4,0x200000); guest_write32(0x20000C,v);
        sub_00378510(); CHECK(g_eax==(v==0x76DB78)); common(0x200000,v);
        reset(); guest_write32(0x6B7AA8,v); guest_write32(0x732E60,0xCAFE);
        sub_000BDB80(); CHECK(g_eax==v); CHECK(guest_read32(0x732E60)==(v?0xCAFEu:0));
        common(0x55667788,0x99AABBCC);
        reset(); guest_write32(0x6B7AA8,v); guest_write32(0x7356D8,0xBEEF);
        sub_000D6A50(); CHECK(g_eax==v); CHECK(guest_read32(0x7356D8)==(v?0xBEEFu:0));
        common(0x55667788,0x99AABBCC);
        reset(); guest_write32(g_esp+4,v); guest_write32(0x4D9658,0x42);
        sub_000827D0(); CHECK(g_eax==v); CHECK(guest_read32(0x7B979C)==v);
        CHECK(guest_read32(0x4D9658)==(v==0xFFFFFFFFu?1u:0x42u));
        common(0x55667788,0x99AABBCC);
    }
    for(unsigned i=0;i<128;++i) {
        uint32_t slot=0x200000+i*0x4C;
        reset(); guest_write32(0x78CED0,i); guest_write32(0x78CECC,0x200000);
        guest_write32(slot+0x48,0xA1B2C3D4); sub_00227A30();
        CHECK(guest_read32(slot+0x48)==0xA1B2C300); CHECK(g_eax==i*0x4C);
        common(0x200000,0x99AABBCC);
        reset(); guest_write32(g_esp+4,0xCAFEBABEu + i); sub_00227A50();
        CHECK(guest_read32(slot+0x3C)==0xCAFEBABEu + i); CHECK(g_eax==i*0x4C);
        common(0xCAFEBABEu + i,0x200000);
        reset(); guest_write32(g_esp+4,i); guest_write32(0x765E44,0x300000);
        guest_write32(0x3000B0,0x400000); sub_003397F0();
        CHECK(g_eax==0x400000+i*0x58); common(0x300000,0x99AABBCC);
    }
    /* Saved EDI slot deliberately overlaps cleared memory; pop must read zero. */
    for(int df=0;df<=1;++df) {
        reset(); g_df=df; g_esp=0x7A2D84;
        guest_write32(0x7A2D80,0xBAD); sub_00162060();
        CHECK(g_esp==0x7A2D88); CHECK(g_edi==0); CHECK(g_ecx==0); CHECK(g_eax==0);
        CHECK(guest_read32(0x7A2D80+(df?0xFFFFF884u:0x77Cu))==0);
        reset(); g_df=df; g_esp=0x7A3584;
        guest_write32(0x74C37C,0xBAD); sub_00158D90();
        CHECK(g_esp==0x7A3588); CHECK(g_edi==0); CHECK(g_ecx==0); CHECK(g_eax==0);
        CHECK(guest_read32(0x74C37C)==0);
        /* rep movsd preserves EAX and reads source before each destination store. */
        reset(); g_df=df; guest_write32(0x789824,0x400000); guest_write32(0x789820,0x500000);
        uint32_t step=df?0xFFFFFFFCu:4u;
        for(uint32_t j=0;j<0xEE0A;++j) guest_write32(0x400000+j*step,j^0x13579BDF);
        sub_002DB4D0(); CHECK(g_eax==0x11223344); common(0,0x99AABBCC);
        for(uint32_t j=0;j<0xEE0A;++j) CHECK(guest_read32(0x500000+j*step)==(j^0x13579BDF));
    }
    /* Overlap is sequential x86 copying, not memmove's snapshot semantics. */
    reset(); guest_write32(0x789824,0x400000); guest_write32(0x789820,0x400004);
    guest_write32(0x400000,0xF00D); sub_002DB4D0();
    CHECK(guest_read32(0x400000+0xEE0A*4)==0xF00D);
    /* Copy writes the saved EDI/ESI slots: pop reloads their overwritten words. */
    reset(); guest_write32(0x789824,0x400000); guest_write32(0x789820,0x500000);
    guest_write32(0x400000,0xDEADC0DE); guest_write32(0x400004,0xBAADF00D);
    g_esp=0x500008; sub_002DB4D0();
    CHECK(g_edi==0xDEADC0DE); CHECK(g_esi==0xBAADF00D); CHECK(g_esp==0x50000C);
    printf("T1786 native: %u checks passed\n",checks); free(memory); return 0;
}
