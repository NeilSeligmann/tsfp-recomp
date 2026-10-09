/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent literal call/write contract for an ignored generated retail seed body.
 * Stub callees stop before the existing tail dependency; this is a finite body test.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "recomp_funcs.h"

uint32_t eax, ebx, ecx, edx, esi, edi, esp, g_ebp, g_seh_ebp;
uint32_t memory[0x00d00000 / 4];
static int slot, errors, tail;
static uint32_t expected_flags;
#define CHECK(x) do { if (!(x)) errors++; } while (0)

void sub_0003D7B0(void) {
    CHECK(MEM32(esp + 4) == 0x198d);
    eax = 0x11223344;
    esp += 4;
}
void sub_00075B70(void) {
    CHECK(MEM32(esp + 4) == expected_flags);
    CHECK(MEM32(esp + 8) == 0x11223344);
    CHECK(MEM32(esp + 12) == 0 && MEM32(esp + 16) == 0 && MEM32(esp + 20) == 0);
    eax = 0;
    esp += 4;
}
void sub_00078E50(void) {
    CHECK(MEM32(esp + 4) == 0xa01000);
    CHECK(MEM32(esp + 8) == 0x4e8d54 && MEM32(esp + 12) == 3);
    CHECK(MEM32(0x7ba9e0) == (uint32_t)slot);
    eax = 0xa02000 + (uint32_t)slot * 0x300;
    slot++;
    MEM32(eax + 0x228) = 0x400 + (uint32_t)slot;
    esp += 4;
}
void sub_00075E70(void) {
    tail++;
    CHECK(esp == 0xbfff00);
    CHECK(esi == 0x12345678 && edi == 0x87654321);
}
int main(void) {
    const int counts[] = {0, 2, 3, -1};
    for (unsigned n = 0; n < sizeof counts / sizeof counts[0]; n++) {
        memset(memory, 0, sizeof memory);
        slot = tail = 0;
        esi = 0x12345678; edi = 0x87654321; esp = 0xbfff00;
        MEM32(esp) = 0x980000; MEM32(esp + 4) = 0xa01000;
        MEM32(0x74e934) = (uint32_t)counts[n]; MEM32(0x7ba9e0) = 0x76543210;
        expected_flags = counts[n] > 2 ? 0x44200000 : 0x200000;
        sub_00191B10();
        CHECK(tail == 1);
        CHECK(slot == (counts[n] > 0 ? counts[n] : 0));
        CHECK(MEM32(0x7ba9e0) == (slot ? (uint32_t)(slot - 1) : 0x76543210));
        for (int i = 0; i < slot; i++) CHECK(MEM32(0xa02000 + i * 0x300 + 0x228) == (uint32_t)(0x411 + i));
    }
    printf("original-literal cases=4 errors=%d\n", errors);
    return errors != 0;
}
