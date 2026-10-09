/* SPDX-License-Identifier: GPL-3.0-or-later
 * Caller controls with an explicitly transcribed original3D7B0 leaf.
 * This is independent native ABI checking, not original-byte execution.
 */
#include "game_replace.h"
#include <stdio.h>
#include <string.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base;
__thread uint32_t g_ebx, g_ebp, g_esi, g_edi;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x01000000];
static unsigned checks, failures, calls;
#define CHECK(expr) do { checks++; if (!(expr)) { failures++; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); } } while (0)
#define STACK 0x00020000u
#define TABLE 0x004D9690u
#define BANK0 0x00D10000u
#define BANK1 0x00D11000u

static void original_leaf_native_control(void)
{
    calls++;
    CHECK(g_esp == STACK - 8u);
    CHECK(guest_read32(g_esp) == 0x00082883u);
    CHECK(g_ecx == game_stack_arg(0u));
    g_ecx = guest_read32(0x006B7B08u);
    if (g_ecx == 0u) {
        g_eax = guest_read32(0x004C0A44u);
    } else {
        g_eax = game_stack_arg(0u);
        if ((int32_t)g_eax >= 0xD87) {
            g_eax = guest_read32(g_ecx + g_eax * 4u - 0x361Cu);
        } else if (g_eax == 0u) {
            g_eax = 0x004766D8u;
        } else {
            g_ecx = guest_read32(0x006B7B10u);
            g_eax = g_ecx != 0u ? guest_read32(g_ecx + g_eax * 4u)
                                : guest_read32(0x004C0A44u);
        }
    }
    g_esp += 4u;
}
game_guest_function recomp_lookup(uint32_t va)
{
    return va == 0x0003D7B0u ? original_leaf_native_control : NULL;
}
extern void sub_00082860(void);

static void one_case(unsigned ordinal)
{
    memset(memory, 0xA5, sizeof(memory));
    g_eax=0x11111111u; g_ecx=0x22222222u; g_edx=0x33333333u;
    g_ebx=0x44444444u; g_ebp=0x55555555u;
    g_esi=0x66666666u; g_edi=0x77777777u; g_esp=STACK;
    guest_write32(STACK, 0x12345678u);
    int32_t gate=0;
    uint32_t index=0u, key=0xD87u, bank0=BANK0, bank1=BANK1;
    uint32_t expected=0x00D12000u+ordinal*4u, expected_ecx=BANK0;
    if (ordinal < 7u) {
        const int32_t gates[7]={-1,0,1,2,3,4,5}; gate=gates[ordinal];
        if (ordinal==0u || ordinal==6u) { expected=0x004766D8u; expected_ecx=0x22222222u; }
    } else if (ordinal < 14u) {
        const uint32_t keys[7]={0xD87u,0u,1u,1u,0xFFFFFFFFu,0xD86u,0xD88u};
        const uint32_t indices[3]={0u,1u,0xFFFFFFFFu};
        key=keys[ordinal-7u]; index=indices[(ordinal-7u)%3u];
        if (ordinal==7u) { bank0=0u; expected=0x004766D8u; expected_ecx=0u; }
        if (ordinal==8u) expected=0x004766D8u;
        if (ordinal>=9u && ordinal<=12u) expected_ecx=BANK1;
        if (ordinal==10u) { bank1=0u; expected=0x004766D8u; expected_ecx=0u; }
    } else if (ordinal==14u) { bank0=STACK-8u; expected=0x00082883u; expected_ecx=bank0; }
    else if (ordinal==15u) { key=1u; bank1=STACK-12u; expected=0x00082883u; expected_ecx=bank1; }
    else if (ordinal==16u) { key=1u; bank1=STACK-4u; expected=0x12345678u; expected_ecx=bank1; }
    else {
        /* Inverse5 modulo2^30 is0x0CCCCCCD; checked slot geometry below. */
        index=((STACK-TABLE)/4u)*0x0CCCCCCDu;
        bank0=0u; expected=0x004766D8u; expected_ecx=0u;
    }
    guest_write32(0x007B9798u,(uint32_t)gate);
    guest_write32(0x007B9794u,index);
    guest_write32(0x006B7B08u,bank0);
    guest_write32(0x006B7B10u,bank1);
    guest_write32(0x004C0A44u,0x004766D8u);
    const uint32_t slot=TABLE+20u*index;
    if (ordinal==17u) { CHECK(slot==STACK); key=0x12345678u; }
    else guest_write32(slot,key);
    if (ordinal<14u && bank0!=0u && key!=0u && ((int32_t)key>=0xD87 || bank1!=0u)) {
        const uint32_t result=(int32_t)key>=0xD87 ? bank0+key*4u-0x361Cu : bank1+key*4u;
        guest_write32(result,expected);
    }
    const unsigned before=calls;
    sub_00082860();
    const unsigned expected_calls=(gate>=0 && gate<5)?1u:0u;
    CHECK(calls-before==expected_calls);
    CHECK(g_eax==expected && g_ecx==expected_ecx);
    CHECK(g_edx==0x33333333u && g_ebx==0x44444444u);
    CHECK(g_ebp==0x55555555u && g_esi==0x66666666u && g_edi==0x77777777u);
    CHECK(g_esp==STACK+4u && guest_read32(STACK)==0x12345678u);
    CHECK(guest_read32(STACK-12u)==0xA5A5A5A5u);
    if (expected_calls!=0u) {
        CHECK(guest_read32(STACK-4u)==key);
        CHECK(guest_read32(STACK-8u)==0x00082883u);
    } else {
        CHECK(guest_read32(STACK-4u)==0xA5A5A5A5u);
        CHECK(guest_read32(STACK-8u)==0xA5A5A5A5u);
    }
}
int main(void)
{
    g_xbox_mem_offset=(ptrdiff_t)(uintptr_t)memory;
    for (unsigned ordinal=0u; ordinal<18u; ordinal++) one_case(ordinal);
    printf("%u checks, %u failures\n",checks,failures);
    return failures!=0u;
}
