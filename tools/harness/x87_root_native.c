/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * T1510 isolated native root runner. Not the differential driver: it links ONLY the four
 * authenticated lowered x87 roots (included from roots_lifted.inc, produced by
 * docs/evidence/t1510/prepare_x87_lift.py) plus the raw-state x87 runtime, over an
 * identity-mapped copy of the guest window. Protocol (stdin):
 *   IMAGE <path>          once, first line (GUEST_SPAN bytes of the shared guest image)
 *   REGS  eax ecx edx ebx esp ebp esi edi   (hex)
 *   X87   <86 bytes as 172 hex chars: 8 raw80 slots LE, tags, control, status>
 *   PATCH <addr hex> <bytes hex>            zero or more, applied in order
 *   RUN   <root va hex>
 * Reply: REGS .. / X87OUT .. / W <addr> <byte> (bytes changed versus the patched baseline,
 * increasing address) / END. A refusal in the x87 runtime exits the process (FATAL).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "x87_runtime.h"

#define GUEST_LO 0x00010000u
#define GUEST_HI 0x01000000u
#define GUEST_SPAN (GUEST_HI - GUEST_LO)

static uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
#define MEM32(a) (*(volatile uint32_t *)(uintptr_t)(uint32_t)(a))
#define MEM8(a) (*(volatile uint8_t *)(uintptr_t)(uint32_t)(a))
#define PUSH32(sp, v) do { uint32_t _v = (v); (sp) -= 4; MEM32(sp) = _v; } while (0)
#define POP32(sp, r) do { (r) = MEM32(sp); (sp) += 4; } while (0)
#define CMP_EQ(a, b) ((a) == (b))
#define CMP_NE(a, b) ((a) != (b))
#define TEST_Z(a, b) (((a) & (b)) == 0)
#define RECOMP_ABI_CALL(va, fn) fn()

void sub_000CB3B0(void);
void sub_001B7D00(void);
void sub_001B7D70(void);
void sub_00259DF0(void);
#include "roots_lifted.inc"

static uint8_t *pristine, *snapshot;

static int hex(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int decode(const char *text, unsigned char *out, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) {
        int hi = hex(text[2 * i]), lo = hex(text[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (unsigned char)(hi << 4 | lo);
    }
    return 0;
}

int main(void) {
    static char line[8192];
    unsigned char state[86];
    void *want = (void *)(uintptr_t)GUEST_LO;
    uint8_t *guest = mmap(want, GUEST_SPAN, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (guest != want) { printf("FATAL identity-map-failed\n"); return 3; }
    pristine = malloc(GUEST_SPAN);
    snapshot = malloc(GUEST_SPAN);
    if (!pristine || !snapshot) return 2;
    int have_regs = 0, have_state = 0;
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "IMAGE ", 6)) {
            FILE *file = fopen(line + 6, "rb");
            if (!file || fread(pristine, 1, GUEST_SPAN, file) != GUEST_SPAN) {
                printf("FATAL bad-image\n"); return 2;
            }
            fclose(file);
            memcpy(guest, pristine, GUEST_SPAN);
            printf("READY\n"); fflush(stdout);
        } else if (!strncmp(line, "REGS ", 5)) {
            uint32_t *slots[8] = {&eax, &ecx, &edx, &ebx, &esp, &ebp, &esi, &edi};
            char *cursor = line + 5;
            for (int i = 0; i < 8; ++i) *slots[i] = (uint32_t)strtoul(cursor, &cursor, 16);
            have_regs = 1;
        } else if (!strncmp(line, "X87 ", 4)) {
            if (strlen(line + 4) != 172 || decode(line + 4, state, 86)) {
                printf("FATAL bad-x87\n"); return 2;
            }
            have_state = 1;
        } else if (!strncmp(line, "PATCH ", 6)) {
            char *rest;
            unsigned long address = strtoul(line + 6, &rest, 16);
            while (*rest == ' ') ++rest;
            size_t bytes = strlen(rest) / 2;
            if (address < GUEST_LO || address + bytes > GUEST_HI) { printf("FATAL bad-patch\n"); return 2; }
            if (decode(rest, guest + (address - GUEST_LO), bytes)) { printf("FATAL bad-patch\n"); return 2; }
        } else if (!strncmp(line, "RUN ", 4)) {
            unsigned long root = strtoul(line + 4, NULL, 16);
            if (!have_regs || !have_state) { printf("FATAL missing-input\n"); return 2; }
            memcpy(snapshot, guest, GUEST_SPAN);
            int error = harness_x87_reset(state);
            if (error) { printf("REFUSED reset %d\nEND\n", error); fflush(stdout); return 4; }
            switch (root) {
            case 0xCB3B0: sub_000CB3B0(); break;
            case 0x1B7D00: sub_001B7D00(); break;
            case 0x1B7D70: sub_001B7D70(); break;
            case 0x259DF0: sub_00259DF0(); break;
            default: printf("FATAL unknown-root\n"); return 2;
            }
            harness_x87_copy(state);
            printf("REGS %08x %08x %08x %08x %08x %08x %08x %08x\nX87OUT ",
                   eax, ecx, edx, ebx, esp, ebp, esi, edi);
            for (int i = 0; i < 86; ++i) printf("%02x", state[i]);
            printf("\n");
            for (uint32_t i = 0; i < GUEST_SPAN; ++i)
                if (guest[i] != snapshot[i]) printf("W %08x %02x\n", GUEST_LO + i, guest[i]);
            printf("END\n"); fflush(stdout);
            memcpy(guest, pristine, GUEST_SPAN);
            have_regs = have_state = 0;
        }
    }
    return 0;
}
