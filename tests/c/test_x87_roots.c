/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1510 native tests of the four x87 roots (src/game/x87_roots.inc) on the PRODUCTION
 * (double-model) backend, plus the checked-return traps. Built by
 * tests/test_t1510_x87_roots_native.py. The same cases also run inside
 * tests/c/test_game_replacements.c (registry count 489).
 */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "x87_replace.h"

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base;
__thread uint32_t g_esi;
ptrdiff_t g_xbox_mem_offset;
__thread double g_fp_stack[8];
__thread int g_fp_top;
__thread uint16_t g_fp_control_word;
static uint8_t memory[0x00810000];
static unsigned checks, failures;

#define CHECK(expression) do { checks++; if (!(expression)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); failures++; } } while (0)

void sub_000CB3B0(void);
void sub_001B7D00(void);
void sub_001B7D70(void);
void sub_00259DF0(void);
void sub_003C9DE8(void);

static float as_float(uint32_t bits) { float value; memcpy(&value, &bits, 4); return value; }
static uint32_t bits_of(float value) { uint32_t bits; memcpy(&bits, &value, 4); return bits; }

#define STACK 0x00200000u
#define RETURN_SITE 0x00056F19u
#define TABLE 0x00300000u
#define RECORD 0x00310000u
#define OBJECT 0x00320000u
#define OTHER 0x00330000u

static void reset(void)
{
    memset(memory, 0, sizeof memory);
    g_fp_top = 0;
    g_fp_control_word = 0x027Fu;
    memset(g_fp_stack, 0, sizeof g_fp_stack);
    g_esp = STACK;
    guest_write32(STACK, RETURN_SITE);
    guest_write32(0x478900u, bits_of(2.0f));
    guest_write32(0x4785CCu, bits_of(4.0f));
    guest_write32(0x4F9BACu, TABLE);
}

static void call(void (*root)(void), uint32_t first, uint32_t second)
{
    guest_write32(STACK + 4u, first);
    guest_write32(STACK + 8u, second);
    root();
}

static void test_cb3b0(void)
{
    reset();
    guest_write32(OBJECT + 0x254u, 0u);
    guest_write32(OBJECT + 0x258u, 1u);
    g_edx = 0xD0D0D0D0u;
    call(sub_000CB3B0, OBJECT, 0u);
    CHECK(g_fp_top == 7 && g_fp_stack[7] == 2.0);
    CHECK(g_esp == STACK + 4u);
    /* register contract callers read: eax = object, ecx = [object+0x254], edx untouched */
    CHECK(g_eax == OBJECT && g_ecx == 0u && g_edx == 0xD0D0D0D0u);
    reset();
    guest_write32(OBJECT + 0x254u, 0u);
    guest_write32(OBJECT + 0x258u, 2u);
    call(sub_000CB3B0, OBJECT, 0u);
    CHECK(g_fp_top == 7 && g_fp_stack[7] == 4.0);
    reset();
    guest_write32(OBJECT + 0x254u, 5u);
    guest_write32(OBJECT + 0x258u, 1u);
    call(sub_000CB3B0, OBJECT, 0u);
    CHECK(g_fp_stack[7] == 4.0);
}

static void test_1b7d00(void)
{
    reset();
    guest_write32(RECORD + 8u, OBJECT);
    guest_write32(TABLE + 3u * 0x29Cu + 0x5Cu, bits_of(1.5f));
    guest_write32(TABLE + 3u * 0x29Cu + 0x60u, bits_of(-2.25f));
    guest_write32(TABLE + 3u * 0x29Cu + 0x64u, bits_of(0.1f));
    guest_write32(OBJECT + 0x258u, 1u);
    call(sub_001B7D00, RECORD, 3u);
    CHECK(as_float(guest_read32(OBJECT + 0x38Cu)) == 3.0f);
    CHECK(as_float(guest_read32(OBJECT + 0x390u)) == -4.5f);
    CHECK(as_float(guest_read32(OBJECT + 0x394u)) == (float)(2.0 * (double)0.1f));
    CHECK(g_fp_top == 0 && g_esp == STACK + 4u);
    CHECK(g_eax == OBJECT && g_ecx == OBJECT && g_edx == OBJECT);
    reset();
    guest_write32(TABLE + 0x5Cu, bits_of(9.0f));
    call(sub_001B7D00, RECORD, 0u); /* null object pointer: nothing written, stack untouched */
    CHECK(g_fp_top == 0 && guest_read32(0x38Cu) == 0u && g_eax == 0u);
}

static void test_1b7d70(void)
{
    /* flag 0x40 clear: delegates to the scaler */
    reset();
    guest_write32(RECORD + 8u, OBJECT);
    guest_write32(OTHER + 0x30u, 1u);
    guest_write32(TABLE + 0x29Cu + 0xCu, 0u);
    guest_write32(TABLE + 0x29Cu + 0x5Cu, bits_of(3.0f));
    guest_write32(OBJECT + 0x258u, 1u);
    call(sub_001B7D70, RECORD, OTHER);
    CHECK(as_float(guest_read32(OBJECT + 0x38Cu)) == 6.0f);
    /* flag 0x40 set and other+0x38 != 1: one value to Y, copied to Z and X */
    reset();
    guest_write32(RECORD + 8u, OBJECT);
    guest_write32(OTHER + 0x30u, 1u);
    guest_write32(TABLE + 0x29Cu + 0xCu, 0x40u);
    guest_write32(OTHER + 0x38u, 0u);
    guest_write32(OBJECT + 0x258u, 1u);
    call(sub_001B7D70, RECORD, OTHER);
    CHECK(as_float(guest_read32(OBJECT + 0x390u)) == 2.0f);
    CHECK(guest_read32(OBJECT + 0x394u) == guest_read32(OBJECT + 0x390u));
    CHECK(guest_read32(OBJECT + 0x38Cu) == guest_read32(OBJECT + 0x390u));
    CHECK(g_fp_top == 0 && g_esp == STACK + 4u);
    /* index -1 and null object return untouched */
    reset();
    guest_write32(RECORD + 8u, OBJECT);
    guest_write32(OTHER + 0x30u, 0xFFFFFFFFu);
    call(sub_001B7D70, RECORD, OTHER);
    CHECK(guest_read32(OBJECT + 0x390u) == 0u && g_fp_top == 0);
}

static void test_259df0(void)
{
    reset();
    guest_write32(0x7B0CC0u, OBJECT);
    guest_write32(0x7B0C7Cu, RECORD);
    guest_write32(RECORD + 0xCu, 0xFFFFFFFFu);
    guest_write32(OBJECT + 0x388u, bits_of(7.0f));
    guest_write32(OBJECT + 0x258u, 1u);
    call(sub_00259DF0, 0u, 0u);
    CHECK(as_float(guest_read32(OBJECT + 0x38Cu)) == 7.0f);
    CHECK(as_float(guest_read32(OBJECT + 0x384u)) == 7.0f);
    CHECK(as_float(guest_read32(OBJECT + 0x388u)) == 2.0f);
    CHECK(guest_read32(RECORD + 0xCu) == 0xFFFFFF9Fu);
    CHECK(g_fp_top == 0 && g_esp == STACK + 4u);
    CHECK(g_eax == RECORD && g_ecx == OBJECT && g_edx == bits_of(7.0f));
}

static uint64_t xorshift(uint64_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

/* x87_fmod_exact (libm free) against libm fmod: random bit patterns (denormals, infinities, NaNs,
 * zeros included), shaped exponent differences, and directed edge pairs. */
static void test_fmod_exact(void)
{
    uint64_t state = 0x3C9DE8u;
    unsigned mismatches = 0u;
    for (unsigned index = 0u; index < 300000u; ++index) {
        uint64_t a = xorshift(&state), b = xorshift(&state);
        if (index % 3u == 1u) { /* near exponents so that long and short divisions both occur */
            b = (b & ~(0x7FFull << 52)) | (((a >> 52) - (xorshift(&state) % 80u)) & 0x7FFull) << 52;
        } else if (index % 3u == 2u) {
            b &= 0x800FFFFFFFFFFFFFull | ((xorshift(&state) % 64u) << 52);
        }
        double x, y;
        memcpy(&x, &a, 8);
        memcpy(&y, &b, 8);
        double got = x87_fmod_exact(x, y), want = fmod(x, y);
        uint64_t gb, wb;
        memcpy(&gb, &got, 8);
        memcpy(&wb, &want, 8);
        int both_nan = got != got && want != want;
        if (!both_nan && gb != wb) {
            if (mismatches++ < 5u) {
                fprintf(stderr, "fmod mismatch %016llx %016llx: %016llx vs %016llx\n",
                        (unsigned long long)a, (unsigned long long)b, (unsigned long long)gb,
                        (unsigned long long)wb);
            }
        }
    }
    CHECK(mismatches == 0u);
    const double edge[] = {0.0, -0.0, 5e-324, -5e-324, 2.2250738585072014e-308, 1.0, -1.0, 3.0,
                           1e300, -1e300, 1.7976931348623157e308, INFINITY, -INFINITY, NAN};
    for (unsigned i = 0u; i < sizeof edge / sizeof edge[0]; ++i) {
        for (unsigned j = 0u; j < sizeof edge / sizeof edge[0]; ++j) {
            double got = x87_fmod_exact(edge[i], edge[j]), want = fmod(edge[i], edge[j]);
            uint64_t gb, wb;
            memcpy(&gb, &got, 8);
            memcpy(&wb, &want, 8);
            CHECK((got != got && want != want) || gb == wb);
        }
    }
}

static void test_3c9de8(void)
{
    /* ST(0) = divisor 3, ST(1) = dividend 10 (TOP 6): ST(1) becomes 10 mod 3 and the divisor is
     * popped (TOP 7). The production status word is the TOP field only (the quotient bits and C2
     * of a real FPREM are not represented by the double model), loaded into AX of eax. */
    reset();
    g_fp_top = 6;
    g_fp_stack[6] = 3.0;
    g_fp_stack[7] = 10.0;
    g_eax = 0xAABB1234u;
    call(sub_003C9DE8, 0u, 0u);
    CHECK(g_fp_top == 7 && g_fp_stack[7] == 1.0);
    CHECK(g_eax == (0xAABB0000u | (6u << 11)) && g_esp == STACK + 4u);
    /* the sign of the dividend survives, a huge exponent difference needs one fmod */
    reset();
    g_fp_top = 0;
    g_fp_stack[0] = 7.0;
    g_fp_stack[1] = -1e300;
    call(sub_003C9DE8, 0u, 0u);
    CHECK(g_fp_top == 1 && g_fp_stack[1] == fmod(-1e300, 7.0) && g_fp_stack[1] <= 0.0);
    /* a zero dividend stays zero, an infinite divisor leaves the dividend */
    reset();
    g_fp_top = 3;
    g_fp_stack[3] = 5.0;
    g_fp_stack[4] = 0.0;
    call(sub_003C9DE8, 0u, 0u);
    CHECK(g_fp_top == 4 && g_fp_stack[4] == 0.0);
    reset();
    g_fp_top = 3;
    g_fp_stack[3] = INFINITY;
    g_fp_stack[4] = 9.5;
    call(sub_003C9DE8, 0u, 0u);
    CHECK(g_fp_top == 4 && g_fp_stack[4] == 9.5);
}

static int traps(x87_frame frame, int net_push, uint32_t esp_after)
{
    fflush(NULL);
    pid_t child = fork();
    if (child == 0) {
        g_esp = esp_after;
        x87_checked_return(frame, net_push);
        _exit(0);
    }
    int status = 0;
    waitpid(child, &status, 0);
    return WIFSIGNALED(status);
}

static void test_checked_return(void)
{
    reset();
    x87_frame frame = x87_frame_open();
    CHECK(!traps(frame, 0, STACK));
    CHECK(traps(frame, 1, STACK));              /* a missing push is refused */
    CHECK(traps(frame, 0, STACK + 4u));         /* a body that popped the return slot */
    CHECK(traps(frame, 0, STACK - 4u));         /* a body that left a stray push */
    g_fp_top = 7;                               /* an unaccounted x87 push */
    CHECK(traps(frame, 0, STACK));
    CHECK(!traps(frame, 1, STACK));
    reset();
    frame = x87_frame_open();
    guest_write32(STACK, 0u);                   /* a body that overwrote the return slot */
    CHECK(traps(frame, 0, STACK));
    reset();
    guest_write32(STACK, 0x12345678u);          /* any caller return address is accepted */
    CHECK(!traps(x87_frame_open(), 0, STACK));
}

int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    test_cb3b0();
    test_1b7d00();
    test_1b7d70();
    test_259df0();
    test_fmod_exact();
    test_3c9de8();
    test_checked_return();
    printf("x87 roots: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
