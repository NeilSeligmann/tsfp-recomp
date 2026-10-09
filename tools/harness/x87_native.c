/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Isolated masked x87 operations. This is not a guest memory/fault adapter. */
#include <stdint.h>
#include <string.h>

#if !defined(__x86_64__) || !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "x87 helper requires little-endian x86-64"
#endif

typedef struct { unsigned char bytes[108]; } environment;
_Static_assert(sizeof(environment) == 108, "32-bit operand-size FNSAVE image");
_Static_assert(sizeof(uint32_t) == 4 && sizeof(uint16_t) == 2, "integer widths");

static uint16_t read16(const unsigned char *p) {
    uint16_t value; memcpy(&value, p, 2); return value;
}
static void write16(unsigned char *p, uint16_t value) { memcpy(p, &value, 2); }

/* Public wire image: eight physical raw80 slots, then tags/CW/SW (all LE). */
static void unpack(environment *env, const unsigned char *state) {
    memset(env, 0, sizeof(*env));
    const uint16_t status = read16(state + 84);
    const unsigned top = (status >> 11) & 7;
    write16(env->bytes, read16(state + 82));
    write16(env->bytes + 4, status);
    write16(env->bytes + 8, read16(state + 80));
    for (unsigned logical = 0; logical < 8; ++logical)
        memcpy(env->bytes + 28 + 10 * logical, state + 10 * ((top + logical) & 7), 10);
}
static void pack(unsigned char *state, const environment *env) {
    const uint16_t status = read16(env->bytes + 4);
    const unsigned top = (status >> 11) & 7;
    for (unsigned logical = 0; logical < 8; ++logical)
        memcpy(state + 10 * ((top + logical) & 7), env->bytes + 28 + 10 * logical, 10);
    write16(state + 80, read16(env->bytes + 8));
    write16(state + 82, read16(env->bytes));
    write16(state + 84, status);
}

/* 0=roundtrip layout canary, 1=FLD m32, 2=FMUL m32, 3=FSTP m32.
 * Returns 0 only after real execution and host-environment preservation. */
int x87_operation(const unsigned char *input, unsigned char *output,
                  unsigned operation, uint32_t operand, uint32_t *stored) {
    const uint16_t control = read16(input + 82);
    if ((control & 0x3f) != 0x3f || !(control & 0x40) ||
        (control & 0xf080) || (control & 0x300) == 0x100) return 1;
    if (operation > 3) return 2;
    environment host, guest, result, after;
    unpack(&guest, input);
    uint32_t value = operand;
    /* FRSTOR may reclassify nonempty tags. Refuse any input the host cannot
     * restore exactly; never repair the supplied tags or other state. */
    __asm__ volatile("fnsave %0; frstor %1; fnsave %2; frstor %0"
                     : "=m"(host), "+m"(guest), "=m"(result) : : "memory");
    pack(output, &result);
    if (memcmp(input, output, 86)) return 4;
    /* C performs no floating-point arithmetic between restore and capture. */
    if (operation == 0) {
        __asm__ volatile("fnsave %0; frstor %1; fnsave %2; frstor %0"
                         : "=m"(host), "+m"(guest), "=m"(result) : : "memory");
    } else if (operation == 1) {
        __asm__ volatile("fnsave %0; frstor %1; flds %3; fnsave %2; frstor %0"
                         : "=m"(host), "+m"(guest), "=m"(result) : "m"(value) : "memory");
    } else if (operation == 2) {
        __asm__ volatile("fnsave %0; frstor %1; fmuls %3; fnsave %2; frstor %0"
                         : "=m"(host), "+m"(guest), "=m"(result) : "m"(value) : "memory");
    } else {
        __asm__ volatile("fnsave %0; frstor %1; fstps %3; fnsave %2; frstor %0"
                         : "=m"(host), "+m"(guest), "=m"(result), "=m"(value) : : "memory");
    }
    __asm__ volatile("fnsave %0; frstor %0" : "=m"(after) : : "memory");
    if (memcmp(&host, &after, sizeof(host))) return 3;
    pack(output, &result);
    *stored = value;
    return 0;
}

/* Test entry: install a nontrivial host canary, then restore the caller's state. */
int x87_operation_canary(const unsigned char *canary, const unsigned char *input,
                         unsigned char *output, unsigned operation,
                         uint32_t operand, uint32_t *stored) {
    unsigned char checked[86];
    uint32_t ignored;
    const int admitted = x87_operation(canary, checked, 0, 0, &ignored);
    if (admitted) return admitted;
    environment outer, seeded;
    unpack(&seeded, canary);
    __asm__ volatile("fnsave %0; frstor %1" : "=m"(outer) : "m"(seeded) : "memory");
    const int result = x87_operation(input, output, operation, operand, stored);
    __asm__ volatile("frstor %0" : : "m"(outer) : "memory");
    return result;
}
