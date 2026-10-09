/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RECOMP_X87_LOCAL_H
#define RECOMP_X87_LOCAL_H
#include <stdint.h>
#include <stdlib.h>

/* Opaque binary80 storage: C never evaluates these values as double. */
typedef struct { unsigned char bytes[16]; } RecompX87Value;

static inline int recomp_x87_local_available(uint16_t cw) {
#if defined(__x86_64__) && defined(__GNUC__)
    /* Mask all exceptions; reject reserved PC, reserved bits and IC.
       Guest precision/rounding bits are used, not a fixture control word. */
    return (cw & 0x003fu) == 0x003fu && (cw & 0x0300u) != 0x0100u &&
           (cw & 0xf080u) == 0 && (cw & 0x0040u) != 0;
#else
    (void)cw;
    return 0;
#endif
}

#if defined(__x86_64__) && defined(__GNUC__)
typedef struct { unsigned char bytes[512]; }
    __attribute__((aligned(16))) RecompX87HostState;

static inline RecompX87Value recomp_x87_load32(uint32_t bits, uint16_t cw) {
    RecompX87HostState saved;
    RecompX87Value result = {{0}};
    __asm__ volatile("fxsave64 %0; fninit; fldcw %2; flds %3; fstpt %1; fxrstor64 %0"
        : "=m"(saved), "+m"(result) : "m"(cw), "m"(bits) : "memory");
    return result;
}

#define RECOMP_X87_MEMORY_HELPER(name, opcode) \
static inline RecompX87Value name(RecompX87Value a, uint32_t bits, uint16_t cw) { \
    RecompX87HostState saved; RecompX87Value result = {{0}}; \
    __asm__ volatile("fxsave64 %0; fninit; fldcw %2; fldt %3; " opcode " %4; fstpt %1; fxrstor64 %0" \
        : "=m"(saved), "+m"(result) : "m"(cw), "m"(a), "m"(bits) : "memory"); \
    return result; \
}
RECOMP_X87_MEMORY_HELPER(recomp_x87_sub32, "fsubs")
RECOMP_X87_MEMORY_HELPER(recomp_x87_subr32, "fsubrs")
#undef RECOMP_X87_MEMORY_HELPER

/* Load operands in original ST0/ST1 order. FMUL keeps ST0;
   FADDP writes ST1 and pops ST0, exactly like the admitted cone. */
#define RECOMP_X87_BINARY_HELPER(name, opcode) \
static inline RecompX87Value name(RecompX87Value top, RecompX87Value next, uint16_t cw) { \
    RecompX87HostState saved; RecompX87Value result = {{0}}; \
    __asm__ volatile("fxsave64 %0; fninit; fldcw %2; fldt %4; fldt %3; " opcode "; fstpt %1; fxrstor64 %0" \
        : "=m"(saved), "+m"(result) : "m"(cw), "m"(top), "m"(next) : "memory"); \
    return result; \
}
RECOMP_X87_BINARY_HELPER(recomp_x87_mul, ".byte 0xd8,0xc9")
RECOMP_X87_BINARY_HELPER(recomp_x87_addp, ".byte 0xde,0xc1")
#undef RECOMP_X87_BINARY_HELPER

static inline RecompX87Value recomp_x87_sqrt(RecompX87Value a, uint16_t cw) {
    RecompX87HostState saved; RecompX87Value result = {{0}};
    __asm__ volatile("fxsave64 %0; fninit; fldcw %2; fldt %3; fsqrt; fstpt %1; fxrstor64 %0"
        : "=m"(saved), "+m"(result) : "m"(cw), "m"(a) : "memory");
    return result;
}
static inline unsigned recomp_x87_comip(RecompX87Value a, RecompX87Value b, uint16_t cw) {
    RecompX87HostState saved;
    unsigned result;
    __asm__ volatile("fxsave64 %1; fninit; fldcw %2; fldt %4; fldt %3; .byte 0xdf,0xf1; lahf; movzbl %%ah,%%eax; andl $0x45,%%eax; fxrstor64 %1"
        : "=&a"(result), "=m"(saved) : "m"(cw), "m"(a), "m"(b) : "memory", "cc");
    return result;
}
#else
/* The emitter checks availability before entering its local cone.
   Accidental direct use on an unsupported host also fails closed. */
static inline RecompX87Value recomp_x87_load32(uint32_t bits, uint16_t cw) {
    (void)bits; (void)cw; abort();
}
#define RECOMP_X87_UNAVAILABLE_MEMORY(name) \
static inline RecompX87Value name(RecompX87Value a, uint32_t bits, uint16_t cw) { \
    (void)a; (void)bits; (void)cw; abort(); \
}
RECOMP_X87_UNAVAILABLE_MEMORY(recomp_x87_sub32)
RECOMP_X87_UNAVAILABLE_MEMORY(recomp_x87_subr32)
#undef RECOMP_X87_UNAVAILABLE_MEMORY
#define RECOMP_X87_UNAVAILABLE_BINARY(name) \
static inline RecompX87Value name(RecompX87Value a, RecompX87Value b, uint16_t cw) { \
    (void)a; (void)b; (void)cw; abort(); \
}
RECOMP_X87_UNAVAILABLE_BINARY(recomp_x87_mul)
RECOMP_X87_UNAVAILABLE_BINARY(recomp_x87_addp)
#undef RECOMP_X87_UNAVAILABLE_BINARY
static inline RecompX87Value recomp_x87_sqrt(RecompX87Value a, uint16_t cw) {
    (void)a; (void)cw; abort();
}
static inline unsigned recomp_x87_comip(RecompX87Value a, RecompX87Value b, uint16_t cw) {
    (void)a; (void)b; (void)cw; abort();
}
#endif
#endif
