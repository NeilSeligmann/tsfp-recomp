/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1510 cross-backend runner for the registry x87 roots (src/game/x87_roots.inc). The SAME
 * adapters `sub_XXXXXXXX` run over an identity-mapped guest window, built twice:
 *   - X87_RUNNER_RAW=1: raw-state backend (-DTSFP_X87_BACKEND_RAW, x87_runtime.c + x87_native.c),
 *     the backend the model-arbitrated-x87-v2 proof measures;
 *   - X87_RUNNER_RAW=0: the PRODUCTION backend (the lifter's double model g_fp_*), the code that
 *     would ship. Its entry state is loaded from the same raw record when every occupied value is
 *     exactly a double and the tags are contiguous from TOP, else the case is REFUSED, never
 *     approximated.
 * Protocol (stdin): IMAGE <path> | REGS eax ecx edx ebx esp ebp esi edi | X87 <172 hex = 86
 * bytes: 8 raw80 slots LE, tags, control, status> | PATCH <addr> <bytes> | RUN <root va>.
 * Reply: REGS .. / X87LOG <control> <depth> <double bits top first> / W <addr> <byte> / END,
 * or REFUSED <reason> / END. The X87LOG line is derived identically on both backends.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "x87_replace.h"

#if X87_RUNNER_RAW
#include "x87_runtime.h"
#endif

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base, g_esi;
ptrdiff_t g_xbox_mem_offset;
__thread double g_fp_stack[8];
__thread int g_fp_top;
__thread uint16_t g_fp_control_word;

void sub_000CB3B0(void);
void sub_001B7D00(void);
void sub_001B7D70(void);
void sub_00259DF0(void);
void sub_003C9DE8(void);

#define GUEST_LO 0x00010000u
#define GUEST_HI 0x01000000u
#define GUEST_SPAN (GUEST_HI - GUEST_LO)

static int hex(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int decode(const char *text, unsigned char *out, size_t bytes)
{
    for (size_t i = 0; i < bytes; ++i) {
        int hi = hex(text[2 * i]), lo = hex(text[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (unsigned char)(hi << 4 | lo);
    }
    return 0;
}

/* The logical stack (top first) of a raw record as doubles, or a refusal reason. */
static const char *logical_from_raw(const unsigned char state[86], double values[8],
                                    unsigned *depth, uint16_t *control, unsigned *top)
{
    uint16_t tags = (uint16_t)(state[80] | state[81] << 8);
    uint16_t status = (uint16_t)(state[84] | state[85] << 8);
    *control = (uint16_t)(state[82] | state[83] << 8);
    *top = (status >> 11) & 7u;
    unsigned count = 0;
    int gap = 0;
    for (unsigned logical = 0; logical < 8; ++logical) {
        unsigned physical = (*top + logical) & 7u;
        unsigned tag = (tags >> (physical * 2)) & 3u;
        if (tag == 3u) { gap = 1; continue; }
        if (gap) return "tags-not-contiguous-from-top";
        long double extended = 0.0L;
        memset(&extended, 0, sizeof extended);
        memcpy(&extended, state + 10 * physical, 10);
        double narrowed = (double)extended;
        if ((long double)narrowed != extended && extended == extended)
            return "value-not-a-double";
        values[count++] = narrowed;
    }
    *depth = count;
    return NULL;
}

int main(void)
{
    static char line[8192];
    unsigned char state[86];
    uint32_t regs[8] = {0};
    void *want = (void *)(uintptr_t)GUEST_LO;
    uint8_t *guest = mmap(want, GUEST_SPAN, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (guest != want) { printf("FATAL identity-map-failed\n"); return 3; }
    g_xbox_mem_offset = 0;
    uint8_t *pristine = malloc(GUEST_SPAN), *snapshot = malloc(GUEST_SPAN);
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
            printf("READY raw=%d\n", X87_RUNNER_RAW); fflush(stdout);
        } else if (!strncmp(line, "REGS ", 5)) {
            char *cursor = line + 5;
            for (int i = 0; i < 8; ++i) regs[i] = (uint32_t)strtoul(cursor, &cursor, 16);
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
            double values[8];
            unsigned depth = 0, top = 0;
            uint16_t control = 0;
            const char *refusal = logical_from_raw(state, values, &depth, &control, &top);
#if X87_RUNNER_RAW
            (void)refusal; /* the raw backend takes any state the runtime accepts */
            {
                int error = harness_x87_reset(state);
                if (error) { printf("REFUSED reset-%d\nEND\n", error); fflush(stdout); return 4; }
            }
#else
            if (refusal != NULL) {
                printf("REFUSED %s\nEND\n", refusal); fflush(stdout);
                memcpy(guest, pristine, GUEST_SPAN);
                have_regs = have_state = 0;
                continue;
            }
            memset(g_fp_stack, 0, sizeof g_fp_stack);
            g_fp_top = (int)top;
            for (unsigned i = 0; i < depth; ++i) g_fp_stack[(top + i) & 7u] = values[i];
            g_fp_control_word = control;
#endif
            g_eax = regs[0]; g_ecx = regs[1]; g_edx = regs[2]; g_esp = regs[4]; g_esi = regs[6];
            switch (root) {
            case 0xCB3B0: sub_000CB3B0(); break;
            case 0x1B7D00: sub_001B7D00(); break;
            case 0x1B7D70: sub_001B7D70(); break;
            case 0x259DF0: sub_00259DF0(); break;
            case 0x3C9DE8: sub_003C9DE8(); break;
            default: printf("FATAL unknown-root\n"); return 2;
            }
            double out_values[8];
            unsigned out_depth = 0, out_top = 0;
            uint16_t out_control = 0;
            const char *out_refusal = NULL;
#if X87_RUNNER_RAW
            unsigned char after[86];
            harness_x87_copy(after);
            out_refusal = logical_from_raw(after, out_values, &out_depth, &out_control, &out_top);
#else
            out_top = (unsigned)g_fp_top & 7u;
            out_control = g_fp_control_word;
            /* depth changes by the TOP movement: a push lowers TOP by one */
            {
                int moved = (int)((top + 8u - out_top) & 7u);
                if (moved > 4) moved -= 8;
                out_depth = (unsigned)((int)depth + moved);
                for (unsigned i = 0; i < out_depth && i < 8u; ++i)
                    out_values[i] = g_fp_stack[(out_top + i) & 7u];
            }
#endif
            printf("REGS %08x %08x %08x %08x %08x %08x %08x %08x\n", g_eax, g_ecx, g_edx, regs[3],
                   g_esp, regs[5], regs[6], regs[7]);
            if (out_refusal != NULL) {
                printf("REFUSED out-%s\n", out_refusal);
            } else {
                printf("X87LOG %04x %u", out_control, out_depth);
                for (unsigned i = 0; i < out_depth; ++i) {
                    uint64_t bits;
                    memcpy(&bits, &out_values[i], 8);
                    printf(" %016llx", (unsigned long long)bits);
                }
                printf("\n");
            }
            for (uint32_t i = 0; i < GUEST_SPAN; ++i)
                if (guest[i] != snapshot[i]) printf("W %08x %02x\n", GUEST_LO + i, guest[i]);
            printf("END\n"); fflush(stdout);
            memcpy(guest, pristine, GUEST_SPAN);
            have_regs = have_state = 0;
        }
    }
    return 0;
}
