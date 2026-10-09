/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Isolated raw-state/guest-memory adapter. Not linked by the default driver. */
#include "x87_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int x87_operation(const unsigned char *, unsigned char *, unsigned,
                         uint32_t, uint32_t *);
static __thread unsigned char raw_state[86];
static __thread int initialized;

static void unsupported(int reason) {
    printf("FATAL isolated-x87-producer-refusal:%d\n", reason);
    fflush(stdout);
    _Exit(2); /* Infrastructure refusal, never a simulated guest trap. */
}

int harness_x87_reset(const unsigned char state[86]) {
    unsigned char candidate[86];
    uint32_t ignored;
    initialized = 0;
    memset(raw_state, 0, sizeof(raw_state));
    if (sysconf(_SC_PAGESIZE) != 4096) return 5;
    const int error = x87_operation(state, candidate, 0, 0, &ignored);
    if (error) return error;
    memcpy(raw_state, candidate, sizeof(raw_state));
    initialized = 1;
    return 0;
}

void harness_x87_copy(unsigned char state[86]) {
    if (!initialized) unsupported(6);
    memcpy(state, raw_state, sizeof(raw_state));
}

static uint32_t read32(uint32_t address) {
    uint32_t value;
    /* Actual unaligned m32 access, with no C floating conversion/alignment UB. */
    __asm__ volatile("movl (%1), %0" : "=r"(value) : "r"((uintptr_t)address) : "memory");
    return value;
}

static void candidate(unsigned operation, uint32_t operand,
                      unsigned char output[86], uint32_t *stored) {
    if (!initialized) unsupported(6);
    const int error = x87_operation(raw_state, output, operation, operand, stored);
    if (error) unsupported(error);
}

void harness_x87_fld32(uint32_t address) {
    if (!initialized) unsupported(6);
    const uint32_t value = read32(address);
    unsigned char output[86];
    uint32_t ignored;
    candidate(1, value, output, &ignored);
    memcpy(raw_state, output, sizeof(raw_state));
}

void harness_x87_fmul32(uint32_t address) {
    if (!initialized) unsupported(6);
    const uint32_t value = read32(address);
    unsigned char output[86];
    uint32_t ignored;
    candidate(2, value, output, &ignored);
    memcpy(raw_state, output, sizeof(raw_state));
}

void harness_x87_fstp32(uint32_t address) {
    unsigned char output[86];
    uint32_t stored;
    candidate(3, 0, output, &stored); /* Host x87 restored before guest access. */
    if ((address & 4095u) <= 4092u) {
        __asm__ volatile("movl %1, (%0)" : : "r"((uintptr_t)address), "r"(stored) : "memory");
    } else {
        /* Reference crosses pages in increasing address order and retains the
         * writable prefix on a subsequent page fault. Volatile bytes prevent
         * coalescing under optimization. Never pre-read destination bytes. */
        volatile unsigned char *destination = (volatile unsigned char *)(uintptr_t)address;
        for (unsigned i = 0; i < 4; ++i)
            destination[i] = (unsigned char)(stored >> (8 * i));
    }
    /* Prevent TLS-state commit moving before any faulting guest store. */
    __asm__ volatile("" : : : "memory");
    memcpy(raw_state, output, sizeof(raw_state));
}

/* T1510 (3C9DE8): register-only operations of x87_native_ext.c (4=FXCH st(1), 5=FPREM, 6=FSTP
 * st(1)). One FPREM is ONE iteration: the replacement loops on C2 itself, as the original does. */
static void register_only(unsigned operation) {
    unsigned char output[86];
    uint32_t ignored;
    candidate(operation, 0, output, &ignored);
    memcpy(raw_state, output, sizeof(raw_state));
}

void harness_x87_fxch1(void) { register_only(4); }
void harness_x87_fprem(void) { register_only(5); }
void harness_x87_fstp1(void) { register_only(6); }
