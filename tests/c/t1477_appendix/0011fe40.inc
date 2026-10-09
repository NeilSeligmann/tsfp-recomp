/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native control for 0x0011FE40, derived from the ORIGINAL instruction listing (Capstone of the
 * XBE), independent of the draft. Original listing:
 *   0011fe40 mov eax,[esp+4] ; mov edx,[eax+4] ; xorps xmm0,xmm0 ; mov ecx,2 ; or edx,ecx
 *   0011fe51 movss [eax+0x90],xmm0 ; movss xmm0,[esp+8] ; mov [eax+4],edx ; mov [eax+0x88],ecx
 *   0011fe68 movss [eax+0x94],xmm0 ; mov dword [eax+0x8c],0x3f ; ret
 * No PUSH/POP. Stack pop: cdecl plain ret (4). Aliases covered: stores landing on the float
 * argument slot (before its read: +0x90), on consumed argument slots (+0x88, +0x8C), on the return
 * slot (same value, and a clobbering wild-return case).
 * Table cases below: expected registers, XMM lanes and every memory word of the checked windows were
 * produced by EXECUTING THE ORIGINAL BYTES in Unicorn 2.1.4 (original only, never the draft) from
 * the named entry state (stack at 0x5000, return address 0x10000, sentinels EAX=0x11111111,
 * ECX=0x22222222, EDX=0x33333333, EBX=0xB1B1B1B1, ESI=0x51515151, EDI=0xD1D1D1D1,
 * EBP=0xBBBB0001, XMM n lane l = 0xA0000000|n<<8|l, windows pre-filled with an address canary).
 * Every write of the original was verified to fall inside the checked windows. The hand-derived
 * check function at the end is written from the listing without the oracle.
 */

/* Weak definitions of all eight XMM slots so the control can check untouched registers
 * (the draft or the runtime owns the strong/real slot, identical layout 4 x uint32). */
extern __thread uint32_t c0011fe40_xmm0[4] __asm__("g_xmm0");
extern __thread uint32_t c0011fe40_xmm1[4] __asm__("g_xmm1");
extern __thread uint32_t c0011fe40_xmm2[4] __asm__("g_xmm2");
extern __thread uint32_t c0011fe40_xmm3[4] __asm__("g_xmm3");
extern __thread uint32_t c0011fe40_xmm4[4] __asm__("g_xmm4");
extern __thread uint32_t c0011fe40_xmm5[4] __asm__("g_xmm5");
extern __thread uint32_t c0011fe40_xmm6[4] __asm__("g_xmm6");
extern __thread uint32_t c0011fe40_xmm7[4] __asm__("g_xmm7");

typedef struct { uint32_t address, value; } c0011fe40_word;
typedef struct { uint32_t low, high; } c0011fe40_window;
typedef struct { unsigned reg; uint32_t lane[4]; } c0011fe40_xmmvalue;
typedef struct {
    const char *name;
    uint32_t esp;
    int df;
    const c0011fe40_word *setup; unsigned setup_count;
    const c0011fe40_window *windows; unsigned window_count;
    uint32_t eax, ecx, edx, ebx, esi, edi, ebp;
    const c0011fe40_xmmvalue *xmm; unsigned xmm_count;
    const c0011fe40_word *diff; unsigned diff_count;
} c0011fe40_case;

static uint32_t c0011fe40_canary(uint32_t address)
{
    return 0xC0DE0000u ^ (address * 2654435761u);
}

/* ordinary */
static const c0011fe40_word c0011fe40_c0_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x00020000u}, {0x00005008u, 0x40490FDBu},
    {0x00020004u, 0x00000010u},
};
static const c0011fe40_window c0011fe40_c0_win[] = {
    {0x00004F00u, 0x00005100u}, {0x0001FFF0u, 0x000200B0u},
};
static const c0011fe40_xmmvalue c0011fe40_c0_xmm[] = {
    {0, {0x40490FDBu, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0011fe40_word c0011fe40_c0_diff[] = {
    {0x00020004u, 0x00000012u}, {0x00020088u, 0x00000002u}, {0x0002008Cu, 0x0000003Fu},
    {0x00020090u, 0x00000000u}, {0x00020094u, 0x40490FDBu},
};
/* flags zero */
static const c0011fe40_word c0011fe40_c1_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x00020000u}, {0x00005008u, 0x3F800000u},
    {0x00020004u, 0x00000000u},
};
static const c0011fe40_window c0011fe40_c1_win[] = {
    {0x00004F00u, 0x00005100u}, {0x0001FFF0u, 0x000200B0u},
};
static const c0011fe40_xmmvalue c0011fe40_c1_xmm[] = {
    {0, {0x3F800000u, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0011fe40_word c0011fe40_c1_diff[] = {
    {0x00020004u, 0x00000002u}, {0x00020088u, 0x00000002u}, {0x0002008Cu, 0x0000003Fu},
    {0x00020090u, 0x00000000u}, {0x00020094u, 0x3F800000u},
};
/* flags already 2 */
static const c0011fe40_word c0011fe40_c2_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x00020000u}, {0x00005008u, 0x3F800000u},
    {0x00020004u, 0x00000002u},
};
static const c0011fe40_window c0011fe40_c2_win[] = {
    {0x00004F00u, 0x00005100u}, {0x0001FFF0u, 0x000200B0u},
};
static const c0011fe40_xmmvalue c0011fe40_c2_xmm[] = {
    {0, {0x3F800000u, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0011fe40_word c0011fe40_c2_diff[] = {
    {0x00020088u, 0x00000002u}, {0x0002008Cu, 0x0000003Fu}, {0x00020090u, 0x00000000u},
    {0x00020094u, 0x3F800000u},
};
/* flags all ones */
static const c0011fe40_word c0011fe40_c3_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x00020000u}, {0x00005008u, 0x3F800000u},
    {0x00020004u, 0xFFFFFFFFu},
};
static const c0011fe40_window c0011fe40_c3_win[] = {
    {0x00004F00u, 0x00005100u}, {0x0001FFF0u, 0x000200B0u},
};
static const c0011fe40_xmmvalue c0011fe40_c3_xmm[] = {
    {0, {0x3F800000u, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0011fe40_word c0011fe40_c3_diff[] = {
    {0x00020088u, 0x00000002u}, {0x0002008Cu, 0x0000003Fu}, {0x00020090u, 0x00000000u},
    {0x00020094u, 0x3F800000u},
};
/* flags sign bit: denormal float bits pass through raw */
static const c0011fe40_word c0011fe40_c4_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x00020000u}, {0x00005008u, 0x00000001u},
    {0x00020004u, 0x80000000u},
};
static const c0011fe40_window c0011fe40_c4_win[] = {
    {0x00004F00u, 0x00005100u}, {0x0001FFF0u, 0x000200B0u},
};
static const c0011fe40_xmmvalue c0011fe40_c4_xmm[] = {
    {0, {0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0011fe40_word c0011fe40_c4_diff[] = {
    {0x00020004u, 0x80000002u}, {0x00020088u, 0x00000002u}, {0x0002008Cu, 0x0000003Fu},
    {0x00020090u, 0x00000000u}, {0x00020094u, 0x00000001u},
};
/* flags 0x7FFFFFFD: negative zero bits */
static const c0011fe40_word c0011fe40_c5_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x00020000u}, {0x00005008u, 0x80000000u},
    {0x00020004u, 0x7FFFFFFDu},
};
static const c0011fe40_window c0011fe40_c5_win[] = {
    {0x00004F00u, 0x00005100u}, {0x0001FFF0u, 0x000200B0u},
};
static const c0011fe40_xmmvalue c0011fe40_c5_xmm[] = {
    {0, {0x80000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0011fe40_word c0011fe40_c5_diff[] = {
    {0x00020004u, 0x7FFFFFFFu}, {0x00020088u, 0x00000002u}, {0x0002008Cu, 0x0000003Fu},
    {0x00020090u, 0x00000000u}, {0x00020094u, 0x80000000u},
};
/* flags 1: NaN bits */
static const c0011fe40_word c0011fe40_c6_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x00020000u}, {0x00005008u, 0x7FC00001u},
    {0x00020004u, 0x00000001u},
};
static const c0011fe40_window c0011fe40_c6_win[] = {
    {0x00004F00u, 0x00005100u}, {0x0001FFF0u, 0x000200B0u},
};
static const c0011fe40_xmmvalue c0011fe40_c6_xmm[] = {
    {0, {0x7FC00001u, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0011fe40_word c0011fe40_c6_diff[] = {
    {0x00020004u, 0x00000003u}, {0x00020088u, 0x00000002u}, {0x0002008Cu, 0x0000003Fu},
    {0x00020090u, 0x00000000u}, {0x00020094u, 0x7FC00001u},
};
/* float all ones */
static const c0011fe40_word c0011fe40_c7_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x00020000u}, {0x00005008u, 0xFFFFFFFFu},
    {0x00020004u, 0x0F0F0F0Fu},
};
static const c0011fe40_window c0011fe40_c7_win[] = {
    {0x00004F00u, 0x00005100u}, {0x0001FFF0u, 0x000200B0u},
};
static const c0011fe40_xmmvalue c0011fe40_c7_xmm[] = {
    {0, {0xFFFFFFFFu, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0011fe40_word c0011fe40_c7_diff[] = {
    {0x00020088u, 0x00000002u}, {0x0002008Cu, 0x0000003Fu}, {0x00020090u, 0x00000000u},
    {0x00020094u, 0xFFFFFFFFu},
};
/* DF set has no effect */
static const c0011fe40_word c0011fe40_c8_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x00020000u}, {0x00005008u, 0x12345678u},
    {0x00020004u, 0x00000044u},
};
static const c0011fe40_window c0011fe40_c8_win[] = {
    {0x00004F00u, 0x00005100u}, {0x0001FFF0u, 0x000200B0u},
};
static const c0011fe40_xmmvalue c0011fe40_c8_xmm[] = {
    {0, {0x12345678u, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0011fe40_word c0011fe40_c8_diff[] = {
    {0x00020004u, 0x00000046u}, {0x00020088u, 0x00000002u}, {0x0002008Cu, 0x0000003Fu},
    {0x00020090u, 0x00000000u}, {0x00020094u, 0x12345678u},
};
/* alias +0x90 on float argument slot: the +0x90 zero store precedes the argument read, so XMM0 and +0x94 become 0 [original returns to a clobbered return slot: state at the ret instruction only] */
static const c0011fe40_word c0011fe40_c9_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x00004F78u}, {0x00005008u, 0xCAFEF00Du},
    {0x00004F7Cu, 0x00000021u},
};
static const c0011fe40_window c0011fe40_c9_win[] = {
    {0x00004F00u, 0x00005100u},
};
static const c0011fe40_xmmvalue c0011fe40_c9_xmm[] = {
    {0, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0011fe40_word c0011fe40_c9_diff[] = {
    {0x00004F7Cu, 0x00000023u}, {0x00005000u, 0x00000002u}, {0x00005004u, 0x0000003Fu},
    {0x00005008u, 0x00000000u}, {0x0000500Cu, 0x00000000u},
};
/* alias +4 on float argument slot (record = arg0 slot): record pointer is the arg0 slot, +4 is the float slot: flags = float bits */
static const c0011fe40_word c0011fe40_c10_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x00005004u}, {0x00005008u, 0xCAFEF00Du},
};
static const c0011fe40_window c0011fe40_c10_win[] = {
    {0x00004F00u, 0x00005100u},
};
static const c0011fe40_xmmvalue c0011fe40_c10_xmm[] = {
    {0, {0xCAFEF00Du, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0011fe40_word c0011fe40_c10_diff[] = {
    {0x00005008u, 0xCAFEF00Fu}, {0x0000508Cu, 0x00000002u}, {0x00005090u, 0x0000003Fu},
    {0x00005094u, 0x00000000u}, {0x00005098u, 0xCAFEF00Du},
};
/* alias +0x8C on arg0 slot: 0x3F stored over the consumed record argument [original returns to a clobbered return slot: state at the ret instruction only] */
static const c0011fe40_word c0011fe40_c11_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x00004F78u}, {0x00005008u, 0x3F000000u},
    {0x00004F7Cu, 0x00000100u},
};
static const c0011fe40_window c0011fe40_c11_win[] = {
    {0x00004F00u, 0x00005100u},
};
static const c0011fe40_xmmvalue c0011fe40_c11_xmm[] = {
    {0, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0011fe40_word c0011fe40_c11_diff[] = {
    {0x00004F7Cu, 0x00000102u}, {0x00005000u, 0x00000002u}, {0x00005004u, 0x0000003Fu},
    {0x00005008u, 0x00000000u}, {0x0000500Cu, 0x00000000u},
};
/* alias +0x88 on arg1 slot: 2 stored over the consumed float argument */
static const c0011fe40_word c0011fe40_c12_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x00004F80u}, {0x00005008u, 0x3F000000u},
    {0x00004F84u, 0x00000100u},
};
static const c0011fe40_window c0011fe40_c12_win[] = {
    {0x00004F00u, 0x00005100u},
};
static const c0011fe40_xmmvalue c0011fe40_c12_xmm[] = {
    {0, {0x3F000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0011fe40_word c0011fe40_c12_diff[] = {
    {0x00004F84u, 0x00000102u}, {0x00005008u, 0x00000002u}, {0x0000500Cu, 0x0000003Fu},
    {0x00005010u, 0x00000000u}, {0x00005014u, 0x3F000000u},
};
/* alias +0x94 rewrites return slot with the same value: float bits equal the return address so the original still returns normally */
static const c0011fe40_word c0011fe40_c13_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x00004F6Cu}, {0x00005008u, 0x00010000u},
    {0x00004F70u, 0x00000008u},
};
static const c0011fe40_window c0011fe40_c13_win[] = {
    {0x00004F00u, 0x00005100u},
};
static const c0011fe40_xmmvalue c0011fe40_c13_xmm[] = {
    {0, {0x00010000u, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0011fe40_word c0011fe40_c13_diff[] = {
    {0x00004F70u, 0x0000000Au}, {0x00004FF4u, 0x00000002u}, {0x00004FF8u, 0x0000003Fu},
    {0x00004FFCu, 0x00000000u},
};
/* alias +4 clobbers return slot: flags are read from the return slot (0x10000) and 0x10002 is stored back over it [original returns to a clobbered return slot: state at the ret instruction only] */
static const c0011fe40_word c0011fe40_c14_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x00004FFCu}, {0x00005008u, 0x3F800000u},
};
static const c0011fe40_window c0011fe40_c14_win[] = {
    {0x00004F00u, 0x00005100u},
};
static const c0011fe40_xmmvalue c0011fe40_c14_xmm[] = {
    {0, {0x3F800000u, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0011fe40_word c0011fe40_c14_diff[] = {
    {0x00005000u, 0x00010002u}, {0x00005084u, 0x00000002u}, {0x00005088u, 0x0000003Fu},
    {0x0000508Cu, 0x00000000u}, {0x00005090u, 0x3F800000u},
};
static const c0011fe40_case c0011fe40_cases[] = {
    {"ordinary", 0x00005000u, 0,
     c0011fe40_c0_setup, 4u, c0011fe40_c0_win, 2u,
     0x00020000u, 0x00000002u, 0x00000012u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0011fe40_c0_xmm, 1u, c0011fe40_c0_diff, 5u},
    {"flags zero", 0x00005000u, 0,
     c0011fe40_c1_setup, 4u, c0011fe40_c1_win, 2u,
     0x00020000u, 0x00000002u, 0x00000002u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0011fe40_c1_xmm, 1u, c0011fe40_c1_diff, 5u},
    {"flags already 2", 0x00005000u, 0,
     c0011fe40_c2_setup, 4u, c0011fe40_c2_win, 2u,
     0x00020000u, 0x00000002u, 0x00000002u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0011fe40_c2_xmm, 1u, c0011fe40_c2_diff, 4u},
    {"flags all ones", 0x00005000u, 0,
     c0011fe40_c3_setup, 4u, c0011fe40_c3_win, 2u,
     0x00020000u, 0x00000002u, 0xFFFFFFFFu, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0011fe40_c3_xmm, 1u, c0011fe40_c3_diff, 4u},
    {"flags sign bit", 0x00005000u, 0,
     c0011fe40_c4_setup, 4u, c0011fe40_c4_win, 2u,
     0x00020000u, 0x00000002u, 0x80000002u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0011fe40_c4_xmm, 1u, c0011fe40_c4_diff, 5u},
    {"flags 0x7FFFFFFD", 0x00005000u, 0,
     c0011fe40_c5_setup, 4u, c0011fe40_c5_win, 2u,
     0x00020000u, 0x00000002u, 0x7FFFFFFFu, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0011fe40_c5_xmm, 1u, c0011fe40_c5_diff, 5u},
    {"flags 1", 0x00005000u, 0,
     c0011fe40_c6_setup, 4u, c0011fe40_c6_win, 2u,
     0x00020000u, 0x00000002u, 0x00000003u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0011fe40_c6_xmm, 1u, c0011fe40_c6_diff, 5u},
    {"float all ones", 0x00005000u, 0,
     c0011fe40_c7_setup, 4u, c0011fe40_c7_win, 2u,
     0x00020000u, 0x00000002u, 0x0F0F0F0Fu, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0011fe40_c7_xmm, 1u, c0011fe40_c7_diff, 4u},
    {"DF set has no effect", 0x00005000u, 1,
     c0011fe40_c8_setup, 4u, c0011fe40_c8_win, 2u,
     0x00020000u, 0x00000002u, 0x00000046u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0011fe40_c8_xmm, 1u, c0011fe40_c8_diff, 5u},
    {"alias +0x90 on float argument slot", 0x00005000u, 0,
     c0011fe40_c9_setup, 4u, c0011fe40_c9_win, 1u,
     0x00004F78u, 0x00000002u, 0x00000023u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0011fe40_c9_xmm, 1u, c0011fe40_c9_diff, 5u},
    {"alias +4 on float argument slot (record = arg0 slot)", 0x00005000u, 0,
     c0011fe40_c10_setup, 3u, c0011fe40_c10_win, 1u,
     0x00005004u, 0x00000002u, 0xCAFEF00Fu, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0011fe40_c10_xmm, 1u, c0011fe40_c10_diff, 5u},
    {"alias +0x8C on arg0 slot", 0x00005000u, 0,
     c0011fe40_c11_setup, 4u, c0011fe40_c11_win, 1u,
     0x00004F78u, 0x00000002u, 0x00000102u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0011fe40_c11_xmm, 1u, c0011fe40_c11_diff, 5u},
    {"alias +0x88 on arg1 slot", 0x00005000u, 0,
     c0011fe40_c12_setup, 4u, c0011fe40_c12_win, 1u,
     0x00004F80u, 0x00000002u, 0x00000102u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0011fe40_c12_xmm, 1u, c0011fe40_c12_diff, 5u},
    {"alias +0x94 rewrites return slot with the same value", 0x00005000u, 0,
     c0011fe40_c13_setup, 4u, c0011fe40_c13_win, 1u,
     0x00004F6Cu, 0x00000002u, 0x0000000Au, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0011fe40_c13_xmm, 1u, c0011fe40_c13_diff, 4u},
    {"alias +4 clobbers return slot", 0x00005000u, 0,
     c0011fe40_c14_setup, 3u, c0011fe40_c14_win, 1u,
     0x00004FFCu, 0x00000002u, 0x00010002u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0011fe40_c14_xmm, 1u, c0011fe40_c14_diff, 5u},
};

static void c0011fe40_run(const c0011fe40_case *c)
{
    static uint32_t before[8192];
    unsigned count = 0;
    prepare();
    for (unsigned w = 0; w < c->window_count; ++w)
        for (uint32_t a = c->windows[w].low; a < c->windows[w].high; a += 4u)
            guest_write32(a, c0011fe40_canary(a));
    for (unsigned k = 0; k < c->setup_count; ++k)
        guest_write32(c->setup[k].address, c->setup[k].value);
    for (unsigned w = 0; w < c->window_count; ++w)
        for (uint32_t a = c->windows[w].low; a < c->windows[w].high; a += 4u)
            before[count++] = guest_read32(a);
    g_esp = c->esp;
    g_eax = 0x11111111u; g_ecx = 0x22222222u; g_edx = 0x33333333u;
    g_ebx = 0xB1B1B1B1u; g_esi = 0x51515151u; g_edi = 0xD1D1D1D1u; g_ebp = 0xBBBB0001u;
    g_df = c->df;
    uint32_t *const slots[8] = {c0011fe40_xmm0, c0011fe40_xmm1, c0011fe40_xmm2, c0011fe40_xmm3, c0011fe40_xmm4, c0011fe40_xmm5, c0011fe40_xmm6, c0011fe40_xmm7};
    for (unsigned reg = 0; reg < 8; ++reg)
        for (unsigned lane = 0; lane < 4; ++lane)
            slots[reg][lane] = 0xA0000000u | (reg << 8) | lane;
    invoke(0x0011FE40u, 4u);
    CHECK(g_eax == c->eax);
    CHECK(g_ecx == c->ecx);
    CHECK(g_edx == c->edx);
    CHECK(g_ebx == c->ebx);
    CHECK(g_esi == c->esi);
    CHECK(g_edi == c->edi);
    CHECK(g_ebp == c->ebp);
    CHECK(g_esp == c->esp + 4u);
    CHECK(g_df == c->df);
    for (unsigned reg = 0; reg < 8; ++reg) {
        uint32_t expected[4];
        for (unsigned lane = 0; lane < 4; ++lane) expected[lane] = 0xA0000000u | (reg << 8) | lane;
        for (unsigned k = 0; k < c->xmm_count; ++k)
            if (c->xmm[k].reg == reg)
                for (unsigned lane = 0; lane < 4; ++lane) expected[lane] = c->xmm[k].lane[lane];
        for (unsigned lane = 0; lane < 4; ++lane) CHECK(slots[reg][lane] == expected[lane]);
    }
    count = 0;
    for (unsigned w = 0; w < c->window_count; ++w) {
        for (uint32_t a = c->windows[w].low; a < c->windows[w].high; a += 4u) {
            uint32_t expected = before[count++];
            for (unsigned k = 0; k < c->diff_count; ++k)
                if (c->diff[k].address == a) expected = c->diff[k].value;
            CHECK(guest_read32(a) == expected);
        }
    }
}

static void c0011fe40_hand(void)
{
    /* hand-derived from the listing: record 0x20000, [rec+4]=0x10, float 0x40490FDB */
    prepare();
    g_esp = 0x6000u;
    guest_write32(0x6000u, 0x00010000u);
    guest_write32(0x6004u, 0x20000u);
    guest_write32(0x6008u, 0x40490FDBu);
    guest_write32(0x20004u, 0x10u);
    g_ebx = 1u; g_esi = 2u; g_edi = 3u; g_ebp = 4u;
    for (unsigned lane = 0; lane < 4; ++lane) c0011fe40_xmm0[lane] = 0x77u + lane;
    invoke(0x0011FE40u, 4u);
    CHECK(g_eax == 0x20000u);
    CHECK(g_ecx == 2u);
    CHECK(g_edx == 0x12u);                       /* 0x10 | 2 */
    CHECK(g_ebx == 1u && g_esi == 2u && g_edi == 3u && g_ebp == 4u);
    CHECK(guest_read32(0x20004u) == 0x12u);
    CHECK(guest_read32(0x20088u) == 2u);
    CHECK(guest_read32(0x2008Cu) == 0x3Fu);
    CHECK(guest_read32(0x20090u) == 0u);
    CHECK(guest_read32(0x20094u) == 0x40490FDBu);
    CHECK(c0011fe40_xmm0[0] == 0x40490FDBu && c0011fe40_xmm0[1] == 0u);
    CHECK(c0011fe40_xmm0[2] == 0u && c0011fe40_xmm0[3] == 0u);
}

static void test_t1477_appendix_0011fe40(void)
{
    for (unsigned i = 0; i < sizeof c0011fe40_cases / sizeof c0011fe40_cases[0]; ++i) c0011fe40_run(&c0011fe40_cases[i]);
    c0011fe40_hand();
}
