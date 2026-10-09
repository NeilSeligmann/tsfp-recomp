/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See d3d8_matrix_inverse.h. Every function carries the address of the original it ports.
 */

#include "d3d8_matrix_inverse.h"

#include <float.h>
#include <string.h>

#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "d3d8_scissor.h"
#include "kernel_call.h"

#define ENTRY 0x003D9DB0u
#define SQUARE_ROOT_HELPER 0x003D9D00u

#define CONSTANT_ZERO 0x00475CACu
#define CONSTANT_HALF 0x00475CD4u
#define CONSTANT_THREE 0x00475D18u
#define CONSTANT_ESTIMATE_SCALE 0x00549720u
#define CONSTANT_ESTIMATE_OFFSET 0x00549724u

/* The frame words (byte offsets of the original's stack after its `sub esp, 0x54`) the cofactors are stored at, in
 * the order of the original's output stores (`[eax + 0]`, `[eax + 0x10]`, `[eax + 4]`, `[eax + 0x20]` ...), and the
 * output word each one lands at. */
#define FRAME_WORDS 0x68u
#define FRAME_DETERMINANT 0x5Cu
static const struct {
    uint32_t frame;
    uint32_t output;
} COFACTOR_STORES[16] = {
    {0x1Cu, 0u},  {0x18u, 4u},  {0x0Cu, 1u},  {0x34u, 8u},  {0x10u, 2u},  {0x38u, 12u}, {0x14u, 3u},  {0x04u, 5u},
    {0x3Cu, 9u},  {0x08u, 6u},  {0x40u, 13u}, {0x00u, 7u},  {0x44u, 10u}, {0x48u, 14u}, {0x4Cu, 11u}, {0x50u, 15u},
};

static bool nan_class(uint32_t word)
{
    return (word & 0x7FFFFFFFu) > 0x7F800000u;
}

static float word_float(uint32_t word)
{
    float value;
    memcpy(&value, &word, sizeof(value));
    return value;
}

static uint32_t float_word(float value)
{
    uint32_t word;
    memcpy(&word, &value, sizeof(word));
    return word;
}

static uint32_t constant(uint32_t address)
{
    const uint32_t word = d3d8_guest_load32(address);
    if (nan_class(word)) {
        d3d8_hle_fatal(ENTRY, "float constant %#x is NaN: payload handling differs between the oracle and physical "
                              "hardware", (unsigned)address);
    }
    return word;
}

/* 0x003D9DB7 to 0x003DA0FB: the sixteen cofactors and the determinant, the original's instructions one for one.
 * `in` stands for eax, `frame` for the original's esp (rbx here), so [esp + 0x28] is `[rbx + 0x28]`. The x87
 * register forms are the original's own bytes (their Intel and AT&T operand orders disagree for fsub and fsubr). */
/* The asm text is one 6.5 KB string literal (267 lines), past the 4095 characters ISO C requires compilers to accept. Both
 * GCC and clang take it. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
static void cofactors(const uint32_t in[16], uint32_t frame[FRAME_WORDS / 4u])
{
#if defined(__x86_64__)
    const uint32_t *in_pointer = in;
    __asm__ volatile(
        ".intel_syntax noprefix\n\t"
        "mov edx, dword ptr [rax + 0x14]\n\t" /* 3D9DB7 */
        "fld dword ptr [rax]\n\t" /* 3D9DBA */
        "fld dword ptr [rax + 4]\n\t" /* 3D9DBC */
        "mov ecx, dword ptr [rax + 0x10]\n\t" /* 3D9DBF */
        "mov dword ptr [rbx + 4], edx\n\t" /* 3D9DC2 */
        "fld dword ptr [rbx + 4]\n\t" /* 3D9DC6 */
        "mov edx, dword ptr [rax + 0x24]\n\t" /* 3D9DCA */
        ".byte 0xd8,0xca\n\t" /* 3D9DCD fmul st(2) */
        "mov dword ptr [rbx + 0x24], ecx\n\t" /* 3D9DCF */
        "fld dword ptr [rbx + 0x24]\n\t" /* 3D9DD3 */
        "mov ecx, dword ptr [rax + 0x20]\n\t" /* 3D9DD7 */
        ".byte 0xd8,0xca\n\t" /* 3D9DDA fmul st(2) */
        "mov dword ptr [rbx + 8], edx\n\t" /* 3D9DDC */
        "mov edx, dword ptr [rax + 0x34]\n\t" /* 3D9DE0 */
        "mov dword ptr [rbx + 0x20], ecx\n\t" /* 3D9DE3 */
        ".byte 0xde,0xe9\n\t" /* 3D9DE7 fsubp st(1) */
        "mov ecx, dword ptr [rax + 0x30]\n\t" /* 3D9DE9 */
        "mov dword ptr [rbx], edx\n\t" /* 3D9DEC */
        "mov dword ptr [rbx + 0x5c], ecx\n\t" /* 3D9DEF */
        "fstp dword ptr [rbx + 0x28]\n\t" /* 3D9DF3 */
        "mov ecx, dword ptr [rax + 8]\n\t" /* 3D9DF7 */
        "fld dword ptr [rbx + 8]\n\t" /* 3D9DFA */
        "mov edx, dword ptr [rax + 0xc]\n\t" /* 3D9DFE */
        ".byte 0xd8,0xca\n\t" /* 3D9E01 fmul st(2) */
        "mov dword ptr [rbx + 0x1c], ecx\n\t" /* 3D9E03 */
        "fld dword ptr [rbx + 0x20]\n\t" /* 3D9E07 */
        "mov ecx, dword ptr [rax + 0x18]\n\t" /* 3D9E0B */
        ".byte 0xd8,0xca\n\t" /* 3D9E0E fmul st(2) */
        "mov dword ptr [rbx + 0xc], ecx\n\t" /* 3D9E10 */
        "mov ecx, dword ptr [rax + 0x28]\n\t" /* 3D9E14 */
        "mov dword ptr [rbx + 0x10], ecx\n\t" /* 3D9E17 */
        ".byte 0xde,0xe9\n\t" /* 3D9E1B fsubp st(1) */
        "mov ecx, dword ptr [rax + 0x38]\n\t" /* 3D9E1D */
        "mov dword ptr [rbx + 0x14], ecx\n\t" /* 3D9E20 */
        "mov dword ptr [rbx + 0x18], edx\n\t" /* 3D9E24 */
        "fstp dword ptr [rbx + 0x2c]\n\t" /* 3D9E28 */
        "mov edx, dword ptr [rax + 0x1c]\n\t" /* 3D9E2C */
        "fld dword ptr [rbx]\n\t" /* 3D9E2F */
        ".byte 0xde,0xca\n\t" /* 3D9E32 fmulp st(2) */
        "fld dword ptr [rbx + 0x5c]\n\t" /* 3D9E34 */
        ".byte 0xd8,0xc9\n\t" /* 3D9E38 fmul st(1) */
        ".byte 0xde,0xea\n\t" /* 3D9E3A fsubp st(2) */
        ".byte 0xdd,0xd8\n\t" /* 3D9E3C fstp st(0) */
        "fld dword ptr [rbx + 8]\n\t" /* 3D9E3E */
        "fmul dword ptr [rbx + 0x24]\n\t" /* 3D9E42 */
        "fld dword ptr [rbx + 0x20]\n\t" /* 3D9E46 */
        "fmul dword ptr [rbx + 4]\n\t" /* 3D9E4A */
        ".byte 0xde,0xe9\n\t" /* 3D9E4E fsubp st(1) */
        "fld dword ptr [rbx]\n\t" /* 3D9E50 */
        "fmul dword ptr [rbx + 0x24]\n\t" /* 3D9E53 */
        "fld dword ptr [rbx + 0x5c]\n\t" /* 3D9E57 */
        "fmul dword ptr [rbx + 4]\n\t" /* 3D9E5B */
        "mov dword ptr [rbx + 4], edx\n\t" /* 3D9E5F */
        "mov edx, dword ptr [rax + 0x2c]\n\t" /* 3D9E63 */
        ".byte 0xde,0xe9\n\t" /* 3D9E66 fsubp st(1) */
        "fld dword ptr [rbx]\n\t" /* 3D9E68 */
        "fmul dword ptr [rbx + 0x20]\n\t" /* 3D9E6B */
        "fld dword ptr [rbx + 0x5c]\n\t" /* 3D9E6F */
        "fmul dword ptr [rbx + 8]\n\t" /* 3D9E73 */
        "mov dword ptr [rbx + 8], edx\n\t" /* 3D9E77 */
        "mov edx, dword ptr [rax + 0x3c]\n\t" /* 3D9E7B */
        "mov dword ptr [rbx], edx\n\t" /* 3D9E7E */
        ".byte 0xde,0xe9\n\t" /* 3D9E81 fsubp st(1) */
        "fld dword ptr [rbx + 0x1c]\n\t" /* 3D9E83 */
        ".byte 0xd8,0xcb\n\t" /* 3D9E87 fmul st(3) */
        "fld dword ptr [rbx + 0xc]\n\t" /* 3D9E89 */
        "fmul dword ptr [rbx + 0x2c]\n\t" /* 3D9E8D */
        ".byte 0xde,0xe9\n\t" /* 3D9E91 fsubp st(1) */
        "fld dword ptr [rbx + 0x10]\n\t" /* 3D9E93 */
        "fmul dword ptr [rbx + 0x28]\n\t" /* 3D9E97 */
        ".byte 0xde,0xc1\n\t" /* 3D9E9B faddp st(1) */
        "fstp dword ptr [rbx + 0x50]\n\t" /* 3D9E9D */
        "fld dword ptr [rbx + 0xc]\n\t" /* 3D9EA1 */
        ".byte 0xd8,0xcc\n\t" /* 3D9EA5 fmul st(4) */
        "fld dword ptr [rbx + 0x14]\n\t" /* 3D9EA7 */
        "fmul dword ptr [rbx + 0x28]\n\t" /* 3D9EAB */
        ".byte 0xde,0xe9\n\t" /* 3D9EAF fsubp st(1) */
        "fld dword ptr [rbx + 0x1c]\n\t" /* 3D9EB1 */
        ".byte 0xd8,0xcb\n\t" /* 3D9EB5 fmul st(3) */
        ".byte 0xde,0xe9\n\t" /* 3D9EB7 fsubp st(1) */
        "fstp dword ptr [rbx + 0x48]\n\t" /* 3D9EB9 */
        "fld dword ptr [rbx + 0x1c]\n\t" /* 3D9EBD */
        ".byte 0xd8,0xc9\n\t" /* 3D9EC1 fmul st(1) */
        "fld dword ptr [rbx + 0x10]\n\t" /* 3D9EC3 */
        ".byte 0xd8,0xcd\n\t" /* 3D9EC7 fmul st(5) */
        ".byte 0xde,0xe9\n\t" /* 3D9EC9 fsubp st(1) */
        "fld dword ptr [rbx + 0x14]\n\t" /* 3D9ECB */
        "fmul dword ptr [rbx + 0x2c]\n\t" /* 3D9ECF */
        ".byte 0xde,0xc1\n\t" /* 3D9ED3 faddp st(1) */
        "fstp dword ptr [rbx + 0x40]\n\t" /* 3D9ED5 */
        "fld dword ptr [rbx + 0x10]\n\t" /* 3D9ED9 */
        ".byte 0xd8,0xca\n\t" /* 3D9EDD fmul st(2) */
        "fld dword ptr [rbx + 0x14]\n\t" /* 3D9EDF */
        ".byte 0xd8,0xcc\n\t" /* 3D9EE3 fmul st(4) */
        ".byte 0xde,0xe9\n\t" /* 3D9EE5 fsubp st(1) */
        "fld dword ptr [rbx + 0xc]\n\t" /* 3D9EE7 */
        ".byte 0xd8,0xca\n\t" /* 3D9EEB fmul st(2) */
        ".byte 0xde,0xe9\n\t" /* 3D9EED fsubp st(1) */
        "fstp dword ptr [rbx + 0x38]\n\t" /* 3D9EEF */
        "fld dword ptr [rbx + 0x2c]\n\t" /* 3D9EF3 */
        "fmul dword ptr [rbx + 4]\n\t" /* 3D9EF7 */
        "fld dword ptr [rbx + 0x28]\n\t" /* 3D9EFB */
        "fmul dword ptr [rbx + 8]\n\t" /* 3D9EFF */
        ".byte 0xde,0xe9\n\t" /* 3D9F03 fsubp st(1) */
        ".byte 0xd9,0xc3\n\t" /* 3D9F05 fld st(3) */
        "fmul dword ptr [rbx + 0x18]\n\t" /* 3D9F07 */
        ".byte 0xde,0xe9\n\t" /* 3D9F0B fsubp st(1) */
        "fstp dword ptr [rbx + 0x4c]\n\t" /* 3D9F0D */
        ".byte 0xd9,0xc1\n\t" /* 3D9F11 fld st(1) */
        "fmul dword ptr [rbx + 0x18]\n\t" /* 3D9F13 */
        ".byte 0xd9,0xc4\n\t" /* 3D9F17 fld st(4) */
        "fmul dword ptr [rbx + 4]\n\t" /* 3D9F19 */
        ".byte 0xde,0xe9\n\t" /* 3D9F1D fsubp st(1) */
        "fld dword ptr [rbx + 0x28]\n\t" /* 3D9F1F */
        "fmul dword ptr [rbx]\n\t" /* 3D9F23 */
        ".byte 0xde,0xc1\n\t" /* 3D9F26 faddp st(1) */
        "fstp dword ptr [rbx + 0x44]\n\t" /* 3D9F28 */
        ".byte 0xd9,0xcb\n\t" /* 3D9F2C fxch st(3) */
        "fmul dword ptr [rbx + 8]\n\t" /* 3D9F2E */
        "fld dword ptr [rbx + 0x2c]\n\t" /* 3D9F32 */
        "fmul dword ptr [rbx]\n\t" /* 3D9F36 */
        ".byte 0xde,0xe9\n\t" /* 3D9F39 fsubp st(1) */
        ".byte 0xd9,0xc3\n\t" /* 3D9F3B fld st(3) */
        "fmul dword ptr [rbx + 0x18]\n\t" /* 3D9F3D */
        ".byte 0xde,0xe9\n\t" /* 3D9F41 fsubp st(1) */
        "fstp dword ptr [rbx + 0x3c]\n\t" /* 3D9F43 */
        ".byte 0xd9,0xca\n\t" /* 3D9F47 fxch st(2) */
        "fmul dword ptr [rbx + 4]\n\t" /* 3D9F49 */
        ".byte 0xd9,0xca\n\t" /* 3D9F4D fxch st(2) */
        "fmul dword ptr [rbx + 8]\n\t" /* 3D9F4F */
        ".byte 0xde,0xea\n\t" /* 3D9F53 fsubp st(2) */
        "fmul dword ptr [rbx]\n\t" /* 3D9F55 */
        ".byte 0xde,0xc1\n\t" /* 3D9F58 faddp st(1) */
        "fstp dword ptr [rbx + 0x34]\n\t" /* 3D9F5A */
        "fld dword ptr [rbx + 0x1c]\n\t" /* 3D9F5E */
        "fmul dword ptr [rbx + 4]\n\t" /* 3D9F62 */
        "fld dword ptr [rbx + 0xc]\n\t" /* 3D9F66 */
        "fmul dword ptr [rbx + 0x18]\n\t" /* 3D9F6A */
        ".byte 0xde,0xe9\n\t" /* 3D9F6E fsubp st(1) */
        "fld dword ptr [rbx + 0x1c]\n\t" /* 3D9F70 */
        "fmul dword ptr [rbx + 8]\n\t" /* 3D9F74 */
        "fld dword ptr [rbx + 0x10]\n\t" /* 3D9F78 */
        "fmul dword ptr [rbx + 0x18]\n\t" /* 3D9F7C */
        ".byte 0xde,0xe9\n\t" /* 3D9F80 fsubp st(1) */
        "fld dword ptr [rbx + 0x1c]\n\t" /* 3D9F82 */
        "fmul dword ptr [rbx]\n\t" /* 3D9F86 */
        "fld dword ptr [rbx + 0x14]\n\t" /* 3D9F89 */
        "fmul dword ptr [rbx + 0x18]\n\t" /* 3D9F8D */
        ".byte 0xde,0xe9\n\t" /* 3D9F91 fsubp st(1) */
        "fld dword ptr [rbx + 0xc]\n\t" /* 3D9F93 */
        "fmul dword ptr [rbx + 8]\n\t" /* 3D9F97 */
        "fld dword ptr [rbx + 0x10]\n\t" /* 3D9F9B */
        "fmul dword ptr [rbx + 4]\n\t" /* 3D9F9F */
        ".byte 0xde,0xe9\n\t" /* 3D9FA3 fsubp st(1) */
        "fstp dword ptr [rbx + 0x28]\n\t" /* 3D9FA5 */
        "fld dword ptr [rbx + 0xc]\n\t" /* 3D9FA9 */
        "fmul dword ptr [rbx]\n\t" /* 3D9FAD */
        "fld dword ptr [rbx + 0x14]\n\t" /* 3D9FB0 */
        "fmul dword ptr [rbx + 4]\n\t" /* 3D9FB4 */
        ".byte 0xde,0xe9\n\t" /* 3D9FB8 fsubp st(1) */
        "fstp dword ptr [rbx + 0x2c]\n\t" /* 3D9FBA */
        "fld dword ptr [rbx + 0x10]\n\t" /* 3D9FBE */
        "mov ecx, dword ptr [rax]\n\t" /* 3D9FC2 */
        "fmul dword ptr [rbx]\n\t" /* 3D9FC4 */
        "mov edx, dword ptr [rax + 0x10]\n\t" /* 3D9FC7 */
        "fld dword ptr [rbx + 0x14]\n\t" /* 3D9FCA */
        "mov dword ptr [rbx + 0x30], ecx\n\t" /* 3D9FCE */
        "fmul dword ptr [rbx + 8]\n\t" /* 3D9FD2 */
        "mov ecx, dword ptr [rax + 0x20]\n\t" /* 3D9FD6 */
        "mov dword ptr [rbx + 0x24], edx\n\t" /* 3D9FD9 */
        "mov edx, dword ptr [rax + 0x30]\n\t" /* 3D9FDD */
        ".byte 0xde,0xe9\n\t" /* 3D9FE0 fsubp st(1) */
        "mov dword ptr [rbx + 0x20], ecx\n\t" /* 3D9FE2 */
        "mov dword ptr [rbx + 0x5c], edx\n\t" /* 3D9FE6 */
        "fstp dword ptr [rbx + 0x18]\n\t" /* 3D9FEA */
        "fld dword ptr [rax + 4]\n\t" /* 3D9FEE */
        "fld dword ptr [rax + 0x14]\n\t" /* 3D9FF1 */
        "fld dword ptr [rax + 0x24]\n\t" /* 3D9FF4 */
        "mov eax, dword ptr [rax + 0x34]\n\t" /* 3D9FF7 */
        ".byte 0xd9,0xc4\n\t" /* 3D9FFA fld st(4) */
        "mov dword ptr [rbx], eax\n\t" /* 3D9FFC */
        ".byte 0xd8,0xca\n\t" /* 3D9FFF fmul st(2) */
        ".byte 0xd9,0xc6\n\t" /* 3DA001 fld st(6) */
        ".byte 0xd8,0xca\n\t" /* 3DA003 fmul st(2) */
        ".byte 0xde,0xe9\n\t" /* 3DA005 fsubp st(1) */
        "fld dword ptr [rbx + 0x28]\n\t" /* 3DA007 */
        ".byte 0xd8,0xcc\n\t" /* 3DA00B fmul st(4) */
        ".byte 0xde,0xe9\n\t" /* 3DA00D fsubp st(1) */
        "fstp dword ptr [rbx + 0x14]\n\t" /* 3DA00F */
        "fld dword ptr [rbx + 0x2c]\n\t" /* 3DA013 */
        ".byte 0xd8,0xcb\n\t" /* 3DA017 fmul st(3) */
        ".byte 0xd9,0xc4\n\t" /* 3DA019 fld st(4) */
        ".byte 0xd8,0xcb\n\t" /* 3DA01B fmul st(3) */
        ".byte 0xde,0xe9\n\t" /* 3DA01D fsubp st(1) */
        ".byte 0xd9,0xc6\n\t" /* 3DA01F fld st(6) */
        "fmul dword ptr [rbx]\n\t" /* 3DA021 */
        ".byte 0xde,0xc1\n\t" /* 3DA024 faddp st(1) */
        "fstp dword ptr [rbx + 0x10]\n\t" /* 3DA026 */
        ".byte 0xd9,0xc3\n\t" /* 3DA02A fld st(3) */
        ".byte 0xd8,0xc9\n\t" /* 3DA02C fmul st(1) */
        ".byte 0xd9,0xc5\n\t" /* 3DA02E fld st(5) */
        "fmul dword ptr [rbx]\n\t" /* 3DA030 */
        ".byte 0xde,0xe9\n\t" /* 3DA033 fsubp st(1) */
        "fld dword ptr [rbx + 0x18]\n\t" /* 3DA035 */
        ".byte 0xd8,0xcc\n\t" /* 3DA039 fmul st(4) */
        ".byte 0xde,0xe9\n\t" /* 3DA03B fsubp st(1) */
        "fstp dword ptr [rbx + 0xc]\n\t" /* 3DA03D */
        "fld dword ptr [rbx + 0x18]\n\t" /* 3DA041 */
        ".byte 0xd8,0xca\n\t" /* 3DA045 fmul st(2) */
        "fld dword ptr [rbx + 0x2c]\n\t" /* 3DA047 */
        ".byte 0xd8,0xca\n\t" /* 3DA04B fmul st(2) */
        ".byte 0xde,0xe9\n\t" /* 3DA04D fsubp st(1) */
        "fld dword ptr [rbx + 0x28]\n\t" /* 3DA04F */
        "fmul dword ptr [rbx]\n\t" /* 3DA053 */
        ".byte 0xde,0xc1\n\t" /* 3DA056 faddp st(1) */
        "fstp dword ptr [rbx + 0x1c]\n\t" /* 3DA058 */
        ".byte 0xdd,0xd8\n\t" /* 3DA05C fstp st(0) */
        ".byte 0xdd,0xd8\n\t" /* 3DA05E fstp st(0) */
        ".byte 0xdd,0xd8\n\t" /* 3DA060 fstp st(0) */
        "fld dword ptr [rbx + 0x28]\n\t" /* 3DA062 */
        "fmul dword ptr [rbx + 0x30]\n\t" /* 3DA066 */
        ".byte 0xd9,0xc2\n\t" /* 3DA06A fld st(2) */
        "fmul dword ptr [rbx + 0x24]\n\t" /* 3DA06C */
        ".byte 0xde,0xe9\n\t" /* 3DA070 fsubp st(1) */
        ".byte 0xd9,0xc3\n\t" /* 3DA072 fld st(3) */
        "fmul dword ptr [rbx + 0x20]\n\t" /* 3DA074 */
        ".byte 0xde,0xc1\n\t" /* 3DA078 faddp st(1) */
        "fstp dword ptr [rbx]\n\t" /* 3DA07A */
        "fld dword ptr [rbx + 0x24]\n\t" /* 3DA07D */
        ".byte 0xd8,0xc9\n\t" /* 3DA081 fmul st(1) */
        ".byte 0xd9,0xcb\n\t" /* 3DA083 fxch st(3) */
        "fmul dword ptr [rbx + 0x5c]\n\t" /* 3DA085 */
        ".byte 0xde,0xeb\n\t" /* 3DA089 fsubp st(3) */
        "fld dword ptr [rbx + 0x2c]\n\t" /* 3DA08B */
        "fmul dword ptr [rbx + 0x30]\n\t" /* 3DA08F */
        ".byte 0xde,0xeb\n\t" /* 3DA093 fsubp st(3) */
        ".byte 0xd9,0xca\n\t" /* 3DA095 fxch st(2) */
        "fstp dword ptr [rbx + 8]\n\t" /* 3DA097 */
        "fld dword ptr [rbx + 0x18]\n\t" /* 3DA09B */
        "fmul dword ptr [rbx + 0x30]\n\t" /* 3DA09F */
        ".byte 0xd9,0xca\n\t" /* 3DA0A3 fxch st(2) */
        "fmul dword ptr [rbx + 0x20]\n\t" /* 3DA0A5 */
        ".byte 0xde,0xea\n\t" /* 3DA0A9 fsubp st(2) */
        "fmul dword ptr [rbx + 0x5c]\n\t" /* 3DA0AB */
        ".byte 0xde,0xc1\n\t" /* 3DA0AF faddp st(1) */
        "fstp dword ptr [rbx + 4]\n\t" /* 3DA0B1 */
        "fld dword ptr [rbx + 0x2c]\n\t" /* 3DA0B5 */
        "fmul dword ptr [rbx + 0x20]\n\t" /* 3DA0B9 */
        "fld dword ptr [rbx + 0x28]\n\t" /* 3DA0BD */
        "fmul dword ptr [rbx + 0x5c]\n\t" /* 3DA0C1 */
        ".byte 0xde,0xe9\n\t" /* 3DA0C5 fsubp st(1) */
        "fld dword ptr [rbx + 0x18]\n\t" /* 3DA0C7 */
        "fmul dword ptr [rbx + 0x24]\n\t" /* 3DA0CB */
        ".byte 0xde,0xe9\n\t" /* 3DA0CF fsubp st(1) */
        "fstp dword ptr [rbx + 0x18]\n\t" /* 3DA0D1 */
        "fld dword ptr [rbx + 0x14]\n\t" /* 3DA0D5 */
        "fmul dword ptr [rbx + 0x5c]\n\t" /* 3DA0D9 */
        "fld dword ptr [rbx + 0x10]\n\t" /* 3DA0DD */
        "fmul dword ptr [rbx + 0x20]\n\t" /* 3DA0E1 */
        ".byte 0xde,0xc1\n\t" /* 3DA0E5 faddp st(1) */
        "fld dword ptr [rbx + 0xc]\n\t" /* 3DA0E7 */
        "fmul dword ptr [rbx + 0x24]\n\t" /* 3DA0EB */
        ".byte 0xde,0xc1\n\t" /* 3DA0EF faddp st(1) */
        "fld dword ptr [rbx + 0x1c]\n\t" /* 3DA0F1 */
        "fmul dword ptr [rbx + 0x30]\n\t" /* 3DA0F5 */
        ".byte 0xde,0xc1\n\t" /* 3DA0F9 faddp st(1) */
        "fstp dword ptr [rbx + 0x5c]\n\t" /* 3DA0FB */
        ".att_syntax prefix\n\t"
        : "+a"(in_pointer)
        : "b"(frame)
        : "ecx", "edx", "memory", "cc", "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)");
#else
    (void)in;
    (void)frame;
    d3d8_hle_fatal(ENTRY, "the matrix inverse requires x86-64 and the x87");
#endif
}
#pragma GCC diagnostic pop

/* 0x003D9D00: an estimate of 1 / sqrt(x). The integer seed (0xBE800000 - bits, halved with the sign kept), one
 * Newton step with the two .rdata constants 0.47 and 1.47, then a second refinement with the constant 3.0 and 0.5
 * on the magnitude. Returns what the original leaves in st0, in EXTENDED precision (the caller's `fstp` rounds). */
static long double fast_inverse_square_root(uint32_t x_word, uint32_t scale, uint32_t offset, uint32_t three,
                                            uint32_t half)
{
    const uint32_t seed = (uint32_t)((int32_t)(0xBE800000u - x_word) >> 1);
    const long double x = word_float(x_word);
    const long double y0 = word_float(seed);
    const long double scaled = (long double)word_float(scale) * x;
    const long double squared = y0 * y0;
    const long double refined = ((long double)word_float(offset) - squared * scaled) * y0;
    const float stored = (float)refined;
    const float magnitude = word_float(float_word(stored) & 0x7FFFFFFFu);
    const long double product = ((long double)magnitude * magnitude) * x;
    return (((long double)word_float(three) - product) * magnitude) * word_float(half);
}

bool d3d8_matrix_inverse_compute(const uint32_t in[16], bool normalize, uint32_t out[16])
{
    d3d8_check_default_fp(ENTRY);
    if (LDBL_MANT_DIG != 64) {
        d3d8_hle_fatal(ENTRY, "the matrix inverse requires x87-width long double");
    }
    for (uint32_t index = 0u; index < 16u; index++) {
        if (nan_class(in[index])) {
            d3d8_hle_fatal(ENTRY, "NaN input payload behavior differs in the original-byte oracle");
        }
    }
    const uint32_t zero = constant(CONSTANT_ZERO);
    const uint32_t half = constant(CONSTANT_HALF);
    const uint32_t three = constant(CONSTANT_THREE);
    const uint32_t estimate_scale = constant(CONSTANT_ESTIMATE_SCALE);
    const uint32_t estimate_offset = constant(CONSTANT_ESTIMATE_OFFSET);

    uint32_t frame[FRAME_WORDS / 4u];
    memset(frame, 0, sizeof(frame));
    cofactors(in, frame);
    const uint32_t determinant = frame[FRAME_DETERMINANT / 4u];
    if (nan_class(determinant)) {
        d3d8_hle_fatal(ENTRY, "NaN determinant: payload handling differs between the oracle and physical hardware");
    }
    /* FUCOMPP of the determinant against the constant at 0x475CAC: equal returns -1 before any store (unordered
     * would go on, and is refused above). */
    if (word_float(determinant) == word_float(zero)) {
        return false;
    }
    uint32_t result[16];
    if (normalize) {
        /* 0x003DA129: s = sign(det) | trunc(rsqrt(float(det * det))), every output float(s * cofactor). */
        const float squared = (float)((long double)word_float(determinant) * word_float(determinant));
        const long double root = fast_inverse_square_root(float_word(squared), estimate_scale, estimate_offset, three,
                                                          half);
        const uint32_t scale = (determinant & 0x80000000u) | float_word((float)root);
        for (uint32_t index = 0u; index < 16u; index++) {
            result[COFACTOR_STORES[index].output] =
                float_word((float)((long double)word_float(scale) * word_float(frame[COFACTOR_STORES[index].frame / 4u])));
        }
    } else {
        /* 0x003DA214: the cofactors with the sign of the determinant flipped in. */
        for (uint32_t index = 0u; index < 16u; index++) {
            result[COFACTOR_STORES[index].output] =
                frame[COFACTOR_STORES[index].frame / 4u] ^ (determinant & 0x80000000u);
        }
    }
    for (uint32_t index = 0u; index < 16u; index++) {
        if (nan_class(result[index])) {
            d3d8_hle_fatal(ENTRY, "NaN output: payload handling differs between the oracle and physical hardware");
        }
    }
    memcpy(out, result, sizeof(result));
    return true;
}

uint32_t d3d8_matrix_inverse(uint32_t out, uint32_t in, uint32_t flag)
{
    uint32_t matrix[16];
    uint32_t result[16];
    if (!kernel_guest_read_bytes(in, matrix, sizeof(matrix))) {
        d3d8_hle_fatal(ENTRY, "matrix inverse input is unreadable or overflows");
    }
    if (!d3d8_matrix_inverse_compute(matrix, flag != 0u, result)) {
        return 0xFFFFFFFFu;
    }
    if (!kernel_guest_write_bytes(out, result, sizeof(result))) {
        d3d8_hle_fatal(ENTRY, "matrix inverse output is unwritable");
    }
    return 0u;
}
