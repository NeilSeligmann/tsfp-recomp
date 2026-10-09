/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Calls the MMX and SSE helper macros of the lift runtime header recomp_types.h exactly the way the
 * generated XMV code calls them (tools/recomp/lifter.py emits the same expressions), one request
 * at a time, so tests/test_xmv_mmx_lifted.py can compare each against the real instruction run in
 * Unicorn (T394).
 *
 *   runner --list            prints "NAME id" for every op, the single source of the ids.
 *   runner                   stdin: uint32 count, then count Case records.
 *                            stdout: count results of 16 bytes (mm results zero extended).
 *
 * Case: uint32 op, form, imm, off, then a[16] (the destination operand before the instruction, or
 * the value stored) and b[16] (the source operand, or the memory contents before a store).
 * Forms: REG source register, MEM source from guest memory at GUEST_DATA + off, IMM immediate
 * `imm`, SAME the destination register is also the source. Guest memory is mapped through
 * g_xbox_mem_offset as production does. Stores report the 16 guest bytes afterwards, so a store of
 * the wrong width shows as a changed or unchanged byte. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECOMP_GENERATED_CODE
#include "recomp_types.h"

ptrdiff_t g_xbox_mem_offset;

#define GUEST_BASE 0x3000u
#define GUEST_DATA (GUEST_BASE + 0x100u)

enum { FORM_REG = 0, FORM_MEM = 1, FORM_IMM = 2, FORM_SAME = 3 };

#define MMX_BINARY_OPS(X)                                                                         \
    X(PADDB) X(PADDW) X(PADDD) X(PSUBB) X(PSUBW) X(PSUBD) X(PAND) X(PANDN) X(POR) X(PXOR)         \
    X(PACKSSDW) X(PACKUSWB) X(PMADDWD) X(PMULLW) X(PAVGB) X(PCMPEQB) X(PCMPEQW) X(PCMPGTB)        \
    X(PCMPGTW) X(PCMPGTD) X(PUNPCKLBW) X(PUNPCKLWD) X(PUNPCKLDQ) X(PUNPCKHBW) X(PUNPCKHWD)        \
    X(PUNPCKHDQ)
#define MMX_SHIFT_OPS(X) X(PSLLW) X(PSLLD) X(PSLLQ) X(PSRLW) X(PSRLD) X(PSRLQ) X(PSRAW) X(PSRAD)
#define XMM_BINARY_OPS(X) X(ADDPS, XMM_ADD) X(MULPS, XMM_MUL) X(RSQRTPS, XMM_RSQRT)
#define OTHER_OPS(X)                                                                              \
    X(PSHUFW) X(MOVQ_RR) X(MOVQ_LOAD) X(MOVQ_STORE) X(MOVD_LOAD) X(MOVD_FROM_GPR)                 \
    X(MOVD_TO_GPR) X(MOVD_STORE) X(EMMS) X(SHUFPS) X(MOVAPS_RR) X(MOVAPS_LOAD) X(MOVAPS_STORE)    \
    X(MOVUPS_LOAD) X(MOVUPS_STORE) X(MOVHPS_LOAD) X(MOVHPS_STORE) X(CVTPS2PI) X(CVTTPS2PI)        \
    X(CVTPI2PS)

#define AS_ENUM(NAME) OP_##NAME,
#define AS_ENUM2(NAME, MACRO) OP_##NAME,
enum {
    MMX_BINARY_OPS(AS_ENUM) MMX_SHIFT_OPS(AS_ENUM) XMM_BINARY_OPS(AS_ENUM2) OTHER_OPS(AS_ENUM)
    OP_COUNT
};

#define AS_NAME(NAME) #NAME,
#define AS_NAME2(NAME, MACRO) #NAME,
static const char *const NAMES[OP_COUNT] = {
    MMX_BINARY_OPS(AS_NAME) MMX_SHIFT_OPS(AS_NAME) XMM_BINARY_OPS(AS_NAME2) OTHER_OPS(AS_NAME)
};

typedef struct {
    uint32_t op, form, imm, off;
    uint8_t a[16], b[16];
} Case;

static uint8_t guest[0x1000];

static void fail(const char *what)
{
    fprintf(stderr, "runner: %s\n", what);
    exit(2);
}

static void put_mm(uint8_t out[16], RecompMmx value)
{
    memset(out, 0, 16);
    memcpy(out, &value, 8);
}

static void put_xmm(uint8_t out[16], RecompXmm value) { memcpy(out, &value, 16); }

static void run(const Case *c, uint8_t out[16])
{
    RecompMmx ma, mb;
    RecompXmm xa, xb;
    uint32_t addr = GUEST_DATA + c->off;
    uint32_t imm = c->imm;
    uint8_t *data = guest + (GUEST_DATA - GUEST_BASE) + c->off;
    uint32_t gpr;

    if (c->op >= OP_COUNT) fail("bad op");
    memcpy(&ma, c->a, 8);
    memcpy(&mb, c->b, 8);
    memcpy(&xa, c->a, 16);
    memcpy(&xb, c->b, 16);
    memset(guest, 0xCC, sizeof guest);
    memcpy(data, c->b, 16);

    switch (c->op) {
#define MMX_BIN_CASE(NAME)                                                                        \
    case OP_##NAME:                                                                               \
        put_mm(out, c->form == FORM_MEM    ? MMX_##NAME(ma, MMX_MEM(addr))                        \
                    : c->form == FORM_SAME ? MMX_##NAME(ma, ma)                                   \
                                           : MMX_##NAME(ma, mb));                                 \
        return;
        MMX_BINARY_OPS(MMX_BIN_CASE)
#define MMX_SHIFT_CASE(NAME)                                                                      \
    case OP_##NAME:                                                                               \
        put_mm(out, c->form == FORM_MEM    ? MMX_##NAME(ma, MMX_MEM(addr).q)                      \
                    : c->form == FORM_SAME ? MMX_##NAME(ma, ma.q)                                 \
                    : c->form == FORM_IMM  ? MMX_##NAME(ma, (imm & 0xFFu))                        \
                                           : MMX_##NAME(ma, mb.q));                               \
        return;
        MMX_SHIFT_OPS(MMX_SHIFT_CASE)
#define XMM_BIN_CASE(NAME, MACRO)                                                                 \
    case OP_##NAME:                                                                               \
        put_xmm(out, c->form == FORM_MEM    ? MACRO(xa, XMM_MEM(addr))                            \
                     : c->form == FORM_SAME ? MACRO(xa, xa)                                       \
                                            : MACRO(xa, xb));                                     \
        return;
        XMM_BINARY_OPS(XMM_BIN_CASE)
    case OP_PSHUFW:
        put_mm(out, MMX_PSHUFW(c->form == FORM_MEM ? MMX_MEM(addr) : mb, imm & 0xFFu));
        return;
    case OP_MOVQ_RR: put_mm(out, mb); return;
    case OP_MOVQ_LOAD: put_mm(out, MMX_MEM(addr)); return;
    case OP_MOVQ_STORE: MMX_STORE(addr, ma); memcpy(out, data, 16); return;
    case OP_MOVD_LOAD: put_mm(out, MMX_FROM32(MEM32(addr))); return;
    case OP_MOVD_FROM_GPR:
        memcpy(&gpr, c->b, 4);
        put_mm(out, MMX_FROM32(gpr));
        return;
    case OP_MOVD_TO_GPR:
        gpr = ma.ud[0];
        memset(out, 0, 16);
        memcpy(out, &gpr, 4);
        return;
    case OP_MOVD_STORE: MEM32(addr) = ma.ud[0]; memcpy(out, data, 16); return;
    case OP_EMMS: (void)0; put_mm(out, ma); return;
    case OP_SHUFPS:
        put_xmm(out, c->form == FORM_MEM    ? XMM_SHUFFLE(xa, XMM_MEM(addr), imm)
                     : c->form == FORM_SAME ? XMM_SHUFFLE(xa, xa, imm)
                                            : XMM_SHUFFLE(xa, xb, imm));
        return;
    case OP_MOVAPS_RR: put_xmm(out, xb); return;
    case OP_MOVAPS_LOAD:
    case OP_MOVUPS_LOAD: put_xmm(out, XMM_MEM(addr)); return;
    case OP_MOVAPS_STORE:
    case OP_MOVUPS_STORE: XMM_STORE(addr, xa); memcpy(out, data, 16); return;
    case OP_MOVHPS_LOAD: XMM_LOAD_HIGH(xa, addr); put_xmm(out, xa); return;
    case OP_MOVHPS_STORE: XMM_STORE_HIGH(addr, xa); memcpy(out, data, 16); return;
    case OP_CVTPS2PI:
    case OP_CVTTPS2PI: {
        int truncate = c->op == OP_CVTTPS2PI;
        put_mm(out, c->form == FORM_MEM
                        ? MMX_FROM_PS(MEMF(addr), MEMF(addr + 4), truncate)
                        : MMX_FROM_PS(xb.f[0], xb.f[1], truncate));
        return;
    }
    case OP_CVTPI2PS:
        put_xmm(out, c->form == FORM_MEM ? XMM_FROM_PI(xa, MMX_MEM(addr)) : XMM_FROM_PI(xa, mb));
        return;
    default: fail("unhandled op");
    }
}

int main(int argc, char **argv)
{
    uint32_t count, index;
    Case *cases;
    uint8_t *results;

    if (argc > 1 && strcmp(argv[1], "--list") == 0) {
        for (index = 0; index < OP_COUNT; index++) printf("%s %u\n", NAMES[index], index);
        return 0;
    }
    g_xbox_mem_offset = (ptrdiff_t)guest - (ptrdiff_t)GUEST_BASE;
    if (fread(&count, 4, 1, stdin) != 1) fail("short input");
    cases = malloc((size_t)count * sizeof *cases + 1);
    results = malloc((size_t)count * 16 + 1);
    if (!cases || !results) fail("out of memory");
    if (fread(cases, sizeof *cases, count, stdin) != count) fail("short input");
    for (index = 0; index < count; index++) run(&cases[index], results + (size_t)index * 16);
    if (fwrite(results, 16, count, stdout) != count) fail("short output");
    return 0;
}
