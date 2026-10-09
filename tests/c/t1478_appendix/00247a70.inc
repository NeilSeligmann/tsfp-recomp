/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent control for 0x00247A70 (derived from the original instructions only).
 * ecx = arg0 (obj); cmp [obj+0x20], 0x10000000; eax = [obj+0x7C] (always read);
 * mismatch or eax == 0 -> fail with ecx = obj. ecx = [eax+4]: zero -> fail (ecx = 0).
 * eax = [eax+0x10] zero -> fail; eax = [eax+0x150] zero -> fail; eax = [eax+4],
 * negative (signed) -> fail. Fail: eax = 0 with ecx as last assigned (see cases).
 * Success: ecx = arg1; if nonzero [ecx] = eax; eax = 1. edx is never touched. */

#define C247A70_OBJ 0x300000u
#define C247A70_P 0x301000u
#define C247A70_Q 0x302000u
#define C247A70_R 0x303000u
#define C247A70_OUT 0x304000u

static uint8_t c247a70_before[sizeof memory];

/* expected_write: nonzero when [OUT] must hold `value` after the call */
static void c247a70_call(uint32_t obj, uint32_t out, uint32_t expected_eax, uint32_t expected_ecx,
                         int expects_write, uint32_t value)
{
    prepare();
    g_ebx = 0xB5B5B5B5u;
    g_esi = 0x55555555u;
    g_edi = 0xD5D5D5D5u;
    g_ebp = 0xBEBEBEBEu;
    g_df = 0;
    guest_write32(g_esp + 4u, obj);
    guest_write32(g_esp + 8u, out);
    guest_write32(C247A70_OUT, 0xEEEEEEEEu);
    memcpy(c247a70_before, memory, sizeof memory);
    invoke(0x00247A70u, 4u);
    CHECK(g_eax == expected_eax);
    CHECK(g_ecx == expected_ecx);
    CHECK(g_edx == 0x33333333u);
    CHECK(g_ebx == 0xB5B5B5B5u && g_esi == 0x55555555u);
    CHECK(g_edi == 0xD5D5D5D5u && g_ebp == 0xBEBEBEBEu);
    if (expects_write) {
        CHECK(guest_read32(C247A70_OUT) == value);
        memcpy(&c247a70_before[C247A70_OUT], &memory[C247A70_OUT], 4u);
    }
    CHECK(memcmp(c247a70_before, memory, sizeof memory) == 0);
}

static void c247a70_chain(uint32_t cls, uint32_t p, uint32_t p4, uint32_t p10, uint32_t q150,
                          uint32_t r4)
{
    guest_write32(C247A70_OBJ + 0x20u, cls);
    guest_write32(C247A70_OBJ + 0x7cu, p);
    guest_write32(C247A70_P + 4u, p4);
    guest_write32(C247A70_P + 0x10u, p10);
    guest_write32(C247A70_Q + 0x150u, q150);
    guest_write32(C247A70_R + 4u, r4);
}

static void test_t1478_appendix_00247a70(void)
{
    memset(&memory[C247A70_OBJ - 0x100u], 0, 0x6000u);
    /* full chain: success, value stored through the out pointer */
    c247a70_chain(0x10000000u, C247A70_P, 0x1234u, C247A70_Q, C247A70_R, 0x2468u);
    c247a70_call(C247A70_OBJ, C247A70_OUT, 1u, C247A70_OUT, 1, 0x2468u);
    /* null out pointer: success without a store, ecx = 0 */
    c247a70_call(C247A70_OBJ, 0u, 1u, 0u, 0, 0u);
    /* value 0 and 0x7FFFFFFF are non-negative: stored */
    c247a70_chain(0x10000000u, C247A70_P, 0x80000000u, C247A70_Q, C247A70_R, 0u);
    c247a70_call(C247A70_OBJ, C247A70_OUT, 1u, C247A70_OUT, 1, 0u);
    c247a70_chain(0x10000000u, C247A70_P, 0x1u, C247A70_Q, C247A70_R, 0x7FFFFFFFu);
    c247a70_call(C247A70_OBJ, C247A70_OUT, 1u, C247A70_OUT, 1, 0x7FFFFFFFu);
    /* negative value: fail with ecx = [P+4], no store */
    c247a70_chain(0x10000000u, C247A70_P, 0x1357u, C247A70_Q, C247A70_R, 0x80000000u);
    c247a70_call(C247A70_OBJ, C247A70_OUT, 0u, 0x1357u, 0, 0u);
    c247a70_chain(0x10000000u, C247A70_P, 0x1357u, C247A70_Q, C247A70_R, 0xFFFFFFFFu);
    c247a70_call(C247A70_OBJ, C247A70_OUT, 0u, 0x1357u, 0, 0u);
    /* [R+0x150-slot] zero: fail, ecx = [P+4] (the value itself is never read) */
    c247a70_chain(0x10000000u, C247A70_P, 0x1357u, C247A70_Q, 0u, 0x5u);
    c247a70_call(C247A70_OBJ, C247A70_OUT, 0u, 0x1357u, 0, 0u);
    /* [P+0x10] zero: fail, ecx = [P+4] */
    c247a70_chain(0x10000000u, C247A70_P, 0x80000001u, 0u, C247A70_R, 0x5u);
    c247a70_call(C247A70_OBJ, C247A70_OUT, 0u, 0x80000001u, 0, 0u);
    /* [P+4] zero: fail with ecx = 0 */
    c247a70_chain(0x10000000u, C247A70_P, 0u, C247A70_Q, C247A70_R, 0x5u);
    c247a70_call(C247A70_OBJ, C247A70_OUT, 0u, 0u, 0, 0u);
    /* [obj+0x7C] zero: fail with ecx = obj */
    c247a70_chain(0x10000000u, 0u, 0x1357u, C247A70_Q, C247A70_R, 0x5u);
    c247a70_call(C247A70_OBJ, C247A70_OUT, 0u, C247A70_OBJ, 0, 0u);
    /* class mismatch with an otherwise valid chain: fail with ecx = obj */
    c247a70_chain(0x10000001u, C247A70_P, 0x1357u, C247A70_Q, C247A70_R, 0x5u);
    c247a70_call(C247A70_OBJ, C247A70_OUT, 0u, C247A70_OBJ, 0, 0u);
    c247a70_chain(0x0FFFFFFFu, C247A70_P, 0x1357u, C247A70_Q, C247A70_R, 0x5u);
    c247a70_call(C247A70_OBJ, C247A70_OUT, 0u, C247A70_OBJ, 0, 0u);
    c247a70_chain(0x00000000u, C247A70_P, 0x1357u, C247A70_Q, C247A70_R, 0x5u);
    c247a70_call(C247A70_OBJ, C247A70_OUT, 0u, C247A70_OBJ, 0, 0u);
    c247a70_chain(0x90000000u, C247A70_P, 0x1357u, C247A70_Q, C247A70_R, 0x5u);
    c247a70_call(C247A70_OBJ, C247A70_OUT, 0u, C247A70_OBJ, 0, 0u);
    /* class mismatch and null chain pointer together: still ecx = obj */
    c247a70_chain(0x20000000u, 0u, 0u, 0u, 0u, 0u);
    c247a70_call(C247A70_OBJ, C247A70_OUT, 0u, C247A70_OBJ, 0, 0u);
    /* store aliasing the source value: out = &[R+4]; eax = value, memory unchanged */
    c247a70_chain(0x10000000u, C247A70_P, 0x1357u, C247A70_Q, C247A70_R, 0x2468u);
    prepare();
    guest_write32(g_esp + 4u, C247A70_OBJ);
    guest_write32(g_esp + 8u, C247A70_R + 4u);
    invoke(0x00247A70u, 4u);
    CHECK(g_eax == 1u && g_ecx == C247A70_R + 4u && guest_read32(C247A70_R + 4u) == 0x2468u);
    /* out pointer inside the object chain: the store lands, a second call sees it */
    guest_write32(C247A70_OBJ + 0x20u, 0x10000000u);
    prepare();
    guest_write32(g_esp + 4u, C247A70_OBJ);
    guest_write32(g_esp + 8u, C247A70_P + 4u); /* overwrite [P+4] with the value */
    invoke(0x00247A70u, 4u);
    CHECK(g_eax == 1u && guest_read32(C247A70_P + 4u) == 0x2468u);
}
