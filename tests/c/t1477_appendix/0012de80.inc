/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native control for 0x0012DE80, derived from the ORIGINAL instruction listing (Capstone of the
 * XBE), independent of the draft. Original listing:
 *   0012de80 mov eax,[esp+4] ; test eax,eax ; jl 0012deb9 ; mov edx,[esp+8]
 *   0012de8c imul eax,eax,0x177c ; add eax,0x7a78c8 ; mov ecx,[eax] ; cmp ecx,edx ; jne 0012deb9
 *   0012de9d xorps xmm0,xmm0 ; movss [eax+4],xmm0 ; movss xmm0,[0x475d10] ; mov dword [eax+0xc],2
 *   0012deb4 movss [eax+8],xmm0 ; 0012deb9 ret
 * No PUSH/POP. cdecl plain ret (4). Aliases covered: group +4/+0/+0xC/+8 on the constant at 0x475D10
 * (read-after-store versus read-before-store ordering), the whole stack inside the group (return
 * slot, key slot and index slot overwritten after their reads), both arguments aliased to the head.
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
extern __thread uint32_t c0012de80_xmm0[4] __asm__("g_xmm0");
extern __thread uint32_t c0012de80_xmm1[4] __asm__("g_xmm1");
extern __thread uint32_t c0012de80_xmm2[4] __asm__("g_xmm2");
extern __thread uint32_t c0012de80_xmm3[4] __asm__("g_xmm3");
extern __thread uint32_t c0012de80_xmm4[4] __asm__("g_xmm4");
extern __thread uint32_t c0012de80_xmm5[4] __asm__("g_xmm5");
extern __thread uint32_t c0012de80_xmm6[4] __asm__("g_xmm6");
extern __thread uint32_t c0012de80_xmm7[4] __asm__("g_xmm7");

typedef struct { uint32_t address, value; } c0012de80_word;
typedef struct { uint32_t low, high; } c0012de80_window;
typedef struct { unsigned reg; uint32_t lane[4]; } c0012de80_xmmvalue;
typedef struct {
    const char *name;
    uint32_t esp;
    int df;
    const c0012de80_word *setup; unsigned setup_count;
    const c0012de80_window *windows; unsigned window_count;
    uint32_t eax, ecx, edx, ebx, esi, edi, ebp;
    const c0012de80_xmmvalue *xmm; unsigned xmm_count;
    const c0012de80_word *diff; unsigned diff_count;
} c0012de80_case;

static uint32_t c0012de80_canary(uint32_t address)
{
    return 0xC0DE0000u ^ (address * 2654435761u);
}

/* match idx 2 */
static const c0012de80_word c0012de80_c0_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x00000002u}, {0x00005008u, 0x12345678u},
    {0x00475D10u, 0x40490FDBu}, {0x007AA7C0u, 0x12345678u},
};
static const c0012de80_window c0012de80_c0_win[] = {
    {0x00004F00u, 0x00005100u}, {0x00475D08u, 0x00475D20u}, {0x007AA7B0u, 0x007AA7E0u},
};
static const c0012de80_xmmvalue c0012de80_c0_xmm[] = {
    {0, {0x40490FDBu, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0012de80_word c0012de80_c0_diff[] = {
    {0x007AA7C4u, 0x00000000u}, {0x007AA7C8u, 0x40490FDBu}, {0x007AA7CCu, 0x00000002u},
};
/* match idx 0 zero key */
static const c0012de80_word c0012de80_c1_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x00000000u}, {0x00005008u, 0x00000000u},
    {0x00475D10u, 0x40490FDBu}, {0x007A78C8u, 0x00000000u},
};
static const c0012de80_window c0012de80_c1_win[] = {
    {0x00004F00u, 0x00005100u}, {0x00475D08u, 0x00475D20u}, {0x007A78B8u, 0x007A78E8u},
};
static const c0012de80_xmmvalue c0012de80_c1_xmm[] = {
    {0, {0x40490FDBu, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0012de80_word c0012de80_c1_diff[] = {
    {0x007A78CCu, 0x00000000u}, {0x007A78D0u, 0x40490FDBu}, {0x007A78D4u, 0x00000002u},
};
/* match idx 0 key ones */
static const c0012de80_word c0012de80_c2_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x00000000u}, {0x00005008u, 0xFFFFFFFFu},
    {0x00475D10u, 0x40490FDBu}, {0x007A78C8u, 0xFFFFFFFFu},
};
static const c0012de80_window c0012de80_c2_win[] = {
    {0x00004F00u, 0x00005100u}, {0x00475D08u, 0x00475D20u}, {0x007A78B8u, 0x007A78E8u},
};
static const c0012de80_xmmvalue c0012de80_c2_xmm[] = {
    {0, {0x40490FDBu, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0012de80_word c0012de80_c2_diff[] = {
    {0x007A78CCu, 0x00000000u}, {0x007A78D0u, 0x40490FDBu}, {0x007A78D4u, 0x00000002u},
};
/* mismatch idx 0 by one */
static const c0012de80_word c0012de80_c3_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x00000000u}, {0x00005008u, 0x12345679u},
    {0x00475D10u, 0x40490FDBu}, {0x007A78C8u, 0x12345678u},
};
static const c0012de80_window c0012de80_c3_win[] = {
    {0x00004F00u, 0x00005100u}, {0x00475D08u, 0x00475D20u}, {0x007A78B8u, 0x007A78E8u},
};
/* mismatch idx 1 sign bit */
static const c0012de80_word c0012de80_c4_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x00000001u}, {0x00005008u, 0x80000000u},
    {0x00475D10u, 0x40490FDBu}, {0x007A9044u, 0x00000000u},
};
static const c0012de80_window c0012de80_c4_win[] = {
    {0x00004F00u, 0x00005100u}, {0x00475D08u, 0x00475D20u}, {0x007A9034u, 0x007A9064u},
};
/* mismatch idx 3 low halves equal */
static const c0012de80_word c0012de80_c5_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x00000003u}, {0x00005008u, 0xAAAA1234u},
    {0x00475D10u, 0x40490FDBu}, {0x007ABF3Cu, 0xBBBB1234u},
};
static const c0012de80_window c0012de80_c5_win[] = {
    {0x00004F00u, 0x00005100u}, {0x00475D08u, 0x00475D20u}, {0x007ABF2Cu, 0x007ABF5Cu},
};
/* mismatch idx 3 high halves equal */
static const c0012de80_word c0012de80_c6_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x00000003u}, {0x00005008u, 0x1234AAAAu},
    {0x00475D10u, 0x40490FDBu}, {0x007ABF3Cu, 0x1234BBBBu},
};
static const c0012de80_window c0012de80_c6_win[] = {
    {0x00004F00u, 0x00005100u}, {0x00475D08u, 0x00475D20u}, {0x007ABF2Cu, 0x007ABF5Cu},
};
/* match idx 17 (last in range) */
static const c0012de80_word c0012de80_c7_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x00000011u}, {0x00005008u, 0x600D600Du},
    {0x00475D10u, 0x40490FDBu}, {0x007C0804u, 0x600D600Du},
};
static const c0012de80_window c0012de80_c7_win[] = {
    {0x00004F00u, 0x00005100u}, {0x00475D08u, 0x00475D20u}, {0x007C07F4u, 0x007C0824u},
};
static const c0012de80_xmmvalue c0012de80_c7_xmm[] = {
    {0, {0x40490FDBu, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0012de80_word c0012de80_c7_diff[] = {
    {0x007C0808u, 0x00000000u}, {0x007C080Cu, 0x40490FDBu}, {0x007C0810u, 0x00000002u},
};
/* match const zero: float constant zero */
static const c0012de80_word c0012de80_c8_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x00000004u}, {0x00005008u, 0x00000007u},
    {0x00475D10u, 0x00000000u}, {0x007AD6B8u, 0x00000007u},
};
static const c0012de80_window c0012de80_c8_win[] = {
    {0x00004F00u, 0x00005100u}, {0x00475D08u, 0x00475D20u}, {0x007AD6A8u, 0x007AD6D8u},
};
static const c0012de80_xmmvalue c0012de80_c8_xmm[] = {
    {0, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0012de80_word c0012de80_c8_diff[] = {
    {0x007AD6BCu, 0x00000000u}, {0x007AD6C0u, 0x00000000u}, {0x007AD6C4u, 0x00000002u},
};
/* match const ones */
static const c0012de80_word c0012de80_c9_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x00000004u}, {0x00005008u, 0x00000007u},
    {0x00475D10u, 0xFFFFFFFFu}, {0x007AD6B8u, 0x00000007u},
};
static const c0012de80_window c0012de80_c9_win[] = {
    {0x00004F00u, 0x00005100u}, {0x00475D08u, 0x00475D20u}, {0x007AD6A8u, 0x007AD6D8u},
};
static const c0012de80_xmmvalue c0012de80_c9_xmm[] = {
    {0, {0xFFFFFFFFu, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0012de80_word c0012de80_c9_diff[] = {
    {0x007AD6BCu, 0x00000000u}, {0x007AD6C0u, 0xFFFFFFFFu}, {0x007AD6C4u, 0x00000002u},
};
/* negative index 0xffffffff returns at once */
static const c0012de80_word c0012de80_c10_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0xFFFFFFFFu}, {0x00005008u, 0x12345678u},
    {0x00475D10u, 0x40490FDBu},
};
static const c0012de80_window c0012de80_c10_win[] = {
    {0x00004F00u, 0x00005100u}, {0x00475D08u, 0x00475D20u}, {0x007A613Cu, 0x007A616Cu},
};
/* negative index 0x80000000 returns at once */
static const c0012de80_word c0012de80_c11_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x80000000u}, {0x00005008u, 0x12345678u},
    {0x00475D10u, 0x40490FDBu},
};
static const c0012de80_window c0012de80_c11_win[] = {
    {0x00004F00u, 0x00005100u}, {0x00475D08u, 0x00475D20u}, {0x007A78B8u, 0x007A78E8u},
};
/* negative index 0xfffffff0 returns at once */
static const c0012de80_word c0012de80_c12_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0xFFFFFFF0u}, {0x00005008u, 0x12345678u},
    {0x00475D10u, 0x40490FDBu},
};
static const c0012de80_window c0012de80_c12_win[] = {
    {0x00004F00u, 0x00005100u}, {0x00475D08u, 0x00475D20u}, {0x007900F8u, 0x00790128u},
};
/* negative index 0xc0000003 returns at once */
static const c0012de80_word c0012de80_c13_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0xC0000003u}, {0x00005008u, 0x12345678u},
    {0x00475D10u, 0x40490FDBu},
};
static const c0012de80_window c0012de80_c13_win[] = {
    {0x00004F00u, 0x00005100u}, {0x00475D08u, 0x00475D20u}, {0x007ABF2Cu, 0x007ABF5Cu},
};
/* index 0x7FFFFFFF wraps to 0x7A6150, match: imul wraps at 32 bits: group at 0x7A6150 */
static const c0012de80_word c0012de80_c14_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x7FFFFFFFu}, {0x00005008u, 0x55AA55AAu},
    {0x00475D10u, 0x40490FDBu}, {0x007A614Cu, 0x55AA55AAu},
};
static const c0012de80_window c0012de80_c14_win[] = {
    {0x00004F00u, 0x00005100u}, {0x00475D08u, 0x00475D20u}, {0x007A613Cu, 0x007A616Cu},
};
static const c0012de80_xmmvalue c0012de80_c14_xmm[] = {
    {0, {0x40490FDBu, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0012de80_word c0012de80_c14_diff[] = {
    {0x007A6150u, 0x00000000u}, {0x007A6154u, 0x40490FDBu}, {0x007A6158u, 0x00000002u},
};
/* index 0x7FFFFFFF wraps, mismatch */
static const c0012de80_word c0012de80_c15_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x7FFFFFFFu}, {0x00005008u, 0x55AA55AAu},
    {0x00475D10u, 0x40490FDBu}, {0x007A614Cu, 0x55AA55ABu},
};
static const c0012de80_window c0012de80_c15_win[] = {
    {0x00004F00u, 0x00005100u}, {0x00475D08u, 0x00475D20u}, {0x007A613Cu, 0x007A616Cu},
};
/* index 0x40000003 equals slot 3 (wrap), match */
static const c0012de80_word c0012de80_c16_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x40000003u}, {0x00005008u, 0x2468ACE0u},
    {0x00475D10u, 0x40490FDBu}, {0x007ABF3Cu, 0x2468ACE0u},
};
static const c0012de80_window c0012de80_c16_win[] = {
    {0x00004F00u, 0x00005100u}, {0x00475D08u, 0x00475D20u}, {0x007ABF2Cu, 0x007ABF5Cu},
};
static const c0012de80_xmmvalue c0012de80_c16_xmm[] = {
    {0, {0x40490FDBu, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0012de80_word c0012de80_c16_diff[] = {
    {0x007ABF40u, 0x00000000u}, {0x007ABF44u, 0x40490FDBu}, {0x007ABF48u, 0x00000002u},
};
/* index 0x40000003 equals slot 3, mismatch */
static const c0012de80_word c0012de80_c17_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x40000003u}, {0x00005008u, 0x2468ACE0u},
    {0x00475D10u, 0x40490FDBu}, {0x007ABF3Cu, 0x00000000u},
};
static const c0012de80_window c0012de80_c17_win[] = {
    {0x00004F00u, 0x00005100u}, {0x00475D08u, 0x00475D20u}, {0x007ABF2Cu, 0x007ABF5Cu},
};
/* DF set has no effect */
static const c0012de80_word c0012de80_c18_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x00000002u}, {0x00005008u, 0x00000009u},
    {0x00475D10u, 0x40490FDBu}, {0x007AA7C0u, 0x00000009u},
};
static const c0012de80_window c0012de80_c18_win[] = {
    {0x00004F00u, 0x00005100u}, {0x00475D08u, 0x00475D20u}, {0x007AA7B0u, 0x007AA7E0u},
};
static const c0012de80_xmmvalue c0012de80_c18_xmm[] = {
    {0, {0x40490FDBu, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0012de80_word c0012de80_c18_diff[] = {
    {0x007AA7C4u, 0x00000000u}, {0x007AA7C8u, 0x40490FDBu}, {0x007AA7CCu, 0x00000002u},
};
/* alias group+4 on the constant: read after the zero store: idx 0x79f3f0f */
static const c0012de80_word c0012de80_c19_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x079F3F0Fu}, {0x00005008u, 0x3C3C3C3Cu},
    {0x00475D10u, 0x40490FDBu}, {0x00475D0Cu, 0x3C3C3C3Cu},
};
static const c0012de80_window c0012de80_c19_win[] = {
    {0x00004F00u, 0x00005100u}, {0x00475CFCu, 0x00475D2Cu},
};
static const c0012de80_xmmvalue c0012de80_c19_xmm[] = {
    {0, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0012de80_word c0012de80_c19_diff[] = {
    {0x00475D10u, 0x00000000u}, {0x00475D14u, 0x00000000u}, {0x00475D18u, 0x00000002u},
};
/* alias group itself on the constant: idx 0xaa5352e */
static const c0012de80_word c0012de80_c20_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x0AA5352Eu}, {0x00005008u, 0x3F000000u},
    {0x00475D10u, 0x40490FDBu}, {0x00475D10u, 0x3F000000u},
};
static const c0012de80_window c0012de80_c20_win[] = {
    {0x00004F00u, 0x00005100u}, {0x00475D00u, 0x00475D30u},
};
static const c0012de80_xmmvalue c0012de80_c20_xmm[] = {
    {0, {0x3F000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0012de80_word c0012de80_c20_diff[] = {
    {0x00475D14u, 0x00000000u}, {0x00475D18u, 0x3F000000u}, {0x00475D1Cu, 0x00000002u},
};
/* alias group+0xC on the constant: read before the 2 store: idx 0x19352d1 */
static const c0012de80_word c0012de80_c21_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x019352D1u}, {0x00005008u, 0x3C3C3C3Cu},
    {0x00475D10u, 0x40490FDBu}, {0x00475D04u, 0x3C3C3C3Cu},
};
static const c0012de80_window c0012de80_c21_win[] = {
    {0x00004F00u, 0x00005100u}, {0x00475CF4u, 0x00475D24u},
};
static const c0012de80_xmmvalue c0012de80_c21_xmm[] = {
    {0, {0x40490FDBu, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0012de80_word c0012de80_c21_diff[] = {
    {0x00475D08u, 0x00000000u}, {0x00475D0Cu, 0x40490FDBu}, {0x00475D10u, 0x00000002u},
};
/* alias group+8 on the constant: idx 0x49948f0 */
static const c0012de80_word c0012de80_c22_setup[] = {
    {0x00005000u, 0x00010000u}, {0x00005004u, 0x049948F0u}, {0x00005008u, 0x3C3C3C3Cu},
    {0x00475D10u, 0x40490FDBu}, {0x00475D08u, 0x3C3C3C3Cu},
};
static const c0012de80_window c0012de80_c22_win[] = {
    {0x00004F00u, 0x00005100u}, {0x00475CF8u, 0x00475D28u},
};
static const c0012de80_xmmvalue c0012de80_c22_xmm[] = {
    {0, {0x40490FDBu, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0012de80_word c0012de80_c22_diff[] = {
    {0x00475D0Cu, 0x00000000u}, {0x00475D14u, 0x00000002u},
};
/* alias stack over the group (match): return slot = group head = key, arg slots are group+4 and +8, overwritten after their reads */
static const c0012de80_word c0012de80_c23_setup[] = {
    {0x007ABF3Cu, 0x00010000u}, {0x007ABF40u, 0x00000003u}, {0x007ABF44u, 0x00010000u},
    {0x00475D10u, 0x40490FDBu},
};
static const c0012de80_window c0012de80_c23_win[] = {
    {0x00004F00u, 0x00005100u}, {0x00475D08u, 0x00475D20u}, {0x007ABF2Cu, 0x007ABF6Cu},
};
static const c0012de80_xmmvalue c0012de80_c23_xmm[] = {
    {0, {0x40490FDBu, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0012de80_word c0012de80_c23_diff[] = {
    {0x007ABF40u, 0x00000000u}, {0x007ABF44u, 0x40490FDBu}, {0x007ABF48u, 0x00000002u},
};
/* alias stack over the group (mismatch): nothing is written */
static const c0012de80_word c0012de80_c24_setup[] = {
    {0x007ABF3Cu, 0x00010000u}, {0x007ABF40u, 0x00000003u}, {0x007ABF44u, 0x00010001u},
    {0x00475D10u, 0x40490FDBu},
};
static const c0012de80_window c0012de80_c24_win[] = {
    {0x00004F00u, 0x00005100u}, {0x00475D08u, 0x00475D20u}, {0x007ABF2Cu, 0x007ABF6Cu},
};
/* alias arg0 slot is the group head, arg1 slot is group+4: key = idx = head = [esp+4]; the +4 zero store lands on the already read key slot */
static const c0012de80_word c0012de80_c25_setup[] = {
    {0x007ABF38u, 0x00010000u}, {0x007ABF3Cu, 0x00000003u}, {0x007ABF40u, 0x00000003u},
    {0x00475D10u, 0x40490FDBu},
};
static const c0012de80_window c0012de80_c25_win[] = {
    {0x00004F00u, 0x00005100u}, {0x00475D08u, 0x00475D20u}, {0x007ABF2Cu, 0x007ABF6Cu},
};
static const c0012de80_xmmvalue c0012de80_c25_xmm[] = {
    {0, {0x40490FDBu, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static const c0012de80_word c0012de80_c25_diff[] = {
    {0x007ABF40u, 0x00000000u}, {0x007ABF44u, 0x40490FDBu}, {0x007ABF48u, 0x00000002u},
};
static const c0012de80_case c0012de80_cases[] = {
    {"match idx 2", 0x00005000u, 0,
     c0012de80_c0_setup, 5u, c0012de80_c0_win, 3u,
     0x007AA7C0u, 0x12345678u, 0x12345678u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0012de80_c0_xmm, 1u, c0012de80_c0_diff, 3u},
    {"match idx 0 zero key", 0x00005000u, 0,
     c0012de80_c1_setup, 5u, c0012de80_c1_win, 3u,
     0x007A78C8u, 0x00000000u, 0x00000000u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0012de80_c1_xmm, 1u, c0012de80_c1_diff, 3u},
    {"match idx 0 key ones", 0x00005000u, 0,
     c0012de80_c2_setup, 5u, c0012de80_c2_win, 3u,
     0x007A78C8u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0012de80_c2_xmm, 1u, c0012de80_c2_diff, 3u},
    {"mismatch idx 0 by one", 0x00005000u, 0,
     c0012de80_c3_setup, 5u, c0012de80_c3_win, 3u,
     0x007A78C8u, 0x12345678u, 0x12345679u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     0, 0u, 0, 0u},
    {"mismatch idx 1 sign bit", 0x00005000u, 0,
     c0012de80_c4_setup, 5u, c0012de80_c4_win, 3u,
     0x007A9044u, 0x00000000u, 0x80000000u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     0, 0u, 0, 0u},
    {"mismatch idx 3 low halves equal", 0x00005000u, 0,
     c0012de80_c5_setup, 5u, c0012de80_c5_win, 3u,
     0x007ABF3Cu, 0xBBBB1234u, 0xAAAA1234u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     0, 0u, 0, 0u},
    {"mismatch idx 3 high halves equal", 0x00005000u, 0,
     c0012de80_c6_setup, 5u, c0012de80_c6_win, 3u,
     0x007ABF3Cu, 0x1234BBBBu, 0x1234AAAAu, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     0, 0u, 0, 0u},
    {"match idx 17 (last in range)", 0x00005000u, 0,
     c0012de80_c7_setup, 5u, c0012de80_c7_win, 3u,
     0x007C0804u, 0x600D600Du, 0x600D600Du, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0012de80_c7_xmm, 1u, c0012de80_c7_diff, 3u},
    {"match const zero", 0x00005000u, 0,
     c0012de80_c8_setup, 5u, c0012de80_c8_win, 3u,
     0x007AD6B8u, 0x00000007u, 0x00000007u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0012de80_c8_xmm, 1u, c0012de80_c8_diff, 3u},
    {"match const ones", 0x00005000u, 0,
     c0012de80_c9_setup, 5u, c0012de80_c9_win, 3u,
     0x007AD6B8u, 0x00000007u, 0x00000007u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0012de80_c9_xmm, 1u, c0012de80_c9_diff, 3u},
    {"negative index 0xffffffff returns at once", 0x00005000u, 0,
     c0012de80_c10_setup, 4u, c0012de80_c10_win, 3u,
     0xFFFFFFFFu, 0x22222222u, 0x33333333u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     0, 0u, 0, 0u},
    {"negative index 0x80000000 returns at once", 0x00005000u, 0,
     c0012de80_c11_setup, 4u, c0012de80_c11_win, 3u,
     0x80000000u, 0x22222222u, 0x33333333u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     0, 0u, 0, 0u},
    {"negative index 0xfffffff0 returns at once", 0x00005000u, 0,
     c0012de80_c12_setup, 4u, c0012de80_c12_win, 3u,
     0xFFFFFFF0u, 0x22222222u, 0x33333333u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     0, 0u, 0, 0u},
    {"negative index 0xc0000003 returns at once", 0x00005000u, 0,
     c0012de80_c13_setup, 4u, c0012de80_c13_win, 3u,
     0xC0000003u, 0x22222222u, 0x33333333u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     0, 0u, 0, 0u},
    {"index 0x7FFFFFFF wraps to 0x7A6150, match", 0x00005000u, 0,
     c0012de80_c14_setup, 5u, c0012de80_c14_win, 3u,
     0x007A614Cu, 0x55AA55AAu, 0x55AA55AAu, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0012de80_c14_xmm, 1u, c0012de80_c14_diff, 3u},
    {"index 0x7FFFFFFF wraps, mismatch", 0x00005000u, 0,
     c0012de80_c15_setup, 5u, c0012de80_c15_win, 3u,
     0x007A614Cu, 0x55AA55ABu, 0x55AA55AAu, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     0, 0u, 0, 0u},
    {"index 0x40000003 equals slot 3 (wrap), match", 0x00005000u, 0,
     c0012de80_c16_setup, 5u, c0012de80_c16_win, 3u,
     0x007ABF3Cu, 0x2468ACE0u, 0x2468ACE0u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0012de80_c16_xmm, 1u, c0012de80_c16_diff, 3u},
    {"index 0x40000003 equals slot 3, mismatch", 0x00005000u, 0,
     c0012de80_c17_setup, 5u, c0012de80_c17_win, 3u,
     0x007ABF3Cu, 0x00000000u, 0x2468ACE0u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     0, 0u, 0, 0u},
    {"DF set has no effect", 0x00005000u, 1,
     c0012de80_c18_setup, 5u, c0012de80_c18_win, 3u,
     0x007AA7C0u, 0x00000009u, 0x00000009u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0012de80_c18_xmm, 1u, c0012de80_c18_diff, 3u},
    {"alias group+4 on the constant: read after the zero store", 0x00005000u, 0,
     c0012de80_c19_setup, 5u, c0012de80_c19_win, 2u,
     0x00475D0Cu, 0x3C3C3C3Cu, 0x3C3C3C3Cu, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0012de80_c19_xmm, 1u, c0012de80_c19_diff, 3u},
    {"alias group itself on the constant", 0x00005000u, 0,
     c0012de80_c20_setup, 5u, c0012de80_c20_win, 2u,
     0x00475D10u, 0x3F000000u, 0x3F000000u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0012de80_c20_xmm, 1u, c0012de80_c20_diff, 3u},
    {"alias group+0xC on the constant: read before the 2 store", 0x00005000u, 0,
     c0012de80_c21_setup, 5u, c0012de80_c21_win, 2u,
     0x00475D04u, 0x3C3C3C3Cu, 0x3C3C3C3Cu, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0012de80_c21_xmm, 1u, c0012de80_c21_diff, 3u},
    {"alias group+8 on the constant", 0x00005000u, 0,
     c0012de80_c22_setup, 5u, c0012de80_c22_win, 2u,
     0x00475D08u, 0x3C3C3C3Cu, 0x3C3C3C3Cu, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0012de80_c22_xmm, 1u, c0012de80_c22_diff, 2u},
    {"alias stack over the group (match)", 0x007ABF3Cu, 0,
     c0012de80_c23_setup, 4u, c0012de80_c23_win, 3u,
     0x007ABF3Cu, 0x00010000u, 0x00010000u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0012de80_c23_xmm, 1u, c0012de80_c23_diff, 3u},
    {"alias stack over the group (mismatch)", 0x007ABF3Cu, 0,
     c0012de80_c24_setup, 4u, c0012de80_c24_win, 3u,
     0x007ABF3Cu, 0x00010000u, 0x00010001u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     0, 0u, 0, 0u},
    {"alias arg0 slot is the group head, arg1 slot is group+4", 0x007ABF38u, 0,
     c0012de80_c25_setup, 4u, c0012de80_c25_win, 3u,
     0x007ABF3Cu, 0x00000003u, 0x00000003u, 0xB1B1B1B1u, 0x51515151u, 0xD1D1D1D1u, 0xBBBB0001u,
     c0012de80_c25_xmm, 1u, c0012de80_c25_diff, 3u},
};

static void c0012de80_run(const c0012de80_case *c)
{
    static uint32_t before[8192];
    unsigned count = 0;
    prepare();
    for (unsigned w = 0; w < c->window_count; ++w)
        for (uint32_t a = c->windows[w].low; a < c->windows[w].high; a += 4u)
            guest_write32(a, c0012de80_canary(a));
    for (unsigned k = 0; k < c->setup_count; ++k)
        guest_write32(c->setup[k].address, c->setup[k].value);
    for (unsigned w = 0; w < c->window_count; ++w)
        for (uint32_t a = c->windows[w].low; a < c->windows[w].high; a += 4u)
            before[count++] = guest_read32(a);
    g_esp = c->esp;
    g_eax = 0x11111111u; g_ecx = 0x22222222u; g_edx = 0x33333333u;
    g_ebx = 0xB1B1B1B1u; g_esi = 0x51515151u; g_edi = 0xD1D1D1D1u; g_ebp = 0xBBBB0001u;
    g_df = c->df;
    uint32_t *const slots[8] = {c0012de80_xmm0, c0012de80_xmm1, c0012de80_xmm2, c0012de80_xmm3, c0012de80_xmm4, c0012de80_xmm5, c0012de80_xmm6, c0012de80_xmm7};
    for (unsigned reg = 0; reg < 8; ++reg)
        for (unsigned lane = 0; lane < 4; ++lane)
            slots[reg][lane] = 0xA0000000u | (reg << 8) | lane;
    invoke(0x0012DE80u, 4u);
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

static void c0012de80_hand(void)
{
    /* hand-derived: index 2 -> group 0x7A78C8 + 2*0x177C = 0x7AA7C0 ; key 0x12345678 matches */
    prepare();
    g_esp = 0x6000u;
    guest_write32(0x6000u, 0x00010000u);
    guest_write32(0x6004u, 2u);
    guest_write32(0x6008u, 0x12345678u);
    guest_write32(0x7AA7C0u, 0x12345678u);
    guest_write32(0x475D10u, 0x40490FDBu);
    guest_write32(0x7AA7C4u, 0xFFFFFFFFu);
    g_ebx = 1u; g_esi = 2u; g_edi = 3u; g_ebp = 4u;
    for (unsigned lane = 0; lane < 4; ++lane) c0012de80_xmm0[lane] = 0x77u + lane;
    invoke(0x0012DE80u, 4u);
    CHECK(g_eax == 0x7AA7C0u && g_ecx == 0x12345678u && g_edx == 0x12345678u);
    CHECK(g_ebx == 1u && g_esi == 2u && g_edi == 3u && g_ebp == 4u);
    CHECK(guest_read32(0x7AA7C4u) == 0u);
    CHECK(guest_read32(0x7AA7C8u) == 0x40490FDBu);
    CHECK(guest_read32(0x7AA7CCu) == 2u);
    CHECK(c0012de80_xmm0[0] == 0x40490FDBu && c0012de80_xmm0[1] == 0u);
    CHECK(c0012de80_xmm0[2] == 0u && c0012de80_xmm0[3] == 0u);
    /* negative index (signed): EAX = index, everything else unchanged */
    prepare();
    g_esp = 0x6000u;
    guest_write32(0x6000u, 0x00010000u);
    guest_write32(0x6004u, 0x80000000u);
    guest_write32(0x6008u, 0x12345678u);
    for (unsigned lane = 0; lane < 4; ++lane) c0012de80_xmm0[lane] = 0x77u + lane;
    invoke(0x0012DE80u, 4u);
    CHECK(g_eax == 0x80000000u && g_ecx == 0x22222222u && g_edx == 0x33333333u);
    CHECK(c0012de80_xmm0[0] == 0x77u && c0012de80_xmm0[3] == 0x7Au);
    /* index 0 is NOT negative: group 0x7A78C8, mismatch leaves memory and XMM0 alone */
    prepare();
    g_esp = 0x6000u;
    guest_write32(0x6000u, 0x00010000u);
    guest_write32(0x6004u, 0u);
    guest_write32(0x6008u, 5u);
    guest_write32(0x7A78C8u, 6u);
    guest_write32(0x7A78CCu, 0x1234u);
    invoke(0x0012DE80u, 4u);
    CHECK(g_eax == 0x7A78C8u && g_ecx == 6u && g_edx == 5u);
    CHECK(guest_read32(0x7A78CCu) == 0x1234u);
}

static void test_t1477_appendix_0012de80(void)
{
    for (unsigned i = 0; i < sizeof c0012de80_cases / sizeof c0012de80_cases[0]; ++i) c0012de80_run(&c0012de80_cases[i]);
    c0012de80_hand();
}
