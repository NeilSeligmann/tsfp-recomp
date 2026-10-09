/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent native control for 0x0036CC10 (cdecl, 4 stack args). Derived from the original
 * instructions only:
 *   eax = [esp+4] (obj) ; ecx = [esp+8] (a) ; edx = [esp+0xC] (b) ; [eax+0x24] = ecx ;
 *   ecx = [esp+0x10] (c, read AFTER the first store) ; [eax+0x28] = edx ; [eax+0x2C] = ecx ; ret.
 *   Exit: EAX = obj, ECX = c as read after the +0x24 store, EDX = b. EBX/ESI/EDI/EBP untouched.
 *   Guest writes in order: +0x24 = a, +0x28 = b, +0x2C = c. The obj + offset sums wrap at 32 bits.
 * The original has NO push and NO pop, so the saved-register POP aliases (b) and (c) do not
 * exist. Alias (a) is covered: the object placed so its +0x24/+0x28/+0x2C stores land on the
 * return slot, on the argument slots (before and after they are read) and on the arg3 slot read
 * between the first and second store, and an object address that wraps to low memory.
 * MUTANT: g_ecx = guest_read32(g_esp + 16u); -> g_ecx = guest_read32(g_esp + 12u);
 * MUTANT: guest_write32(g_eax + 0x2cu, g_ecx); -> guest_write32(g_eax + 0x2cu, g_edx);
 * MUTANT: guest_write32(g_eax + 0x28u, g_edx); -> guest_write32(g_eax + 0x28u, g_ecx);
 */

#define T1479_36CC10_OBJ 0x300000u

static uint8_t t1479_36cc10_exp[sizeof memory];

static void t1479_36cc10_put(uint32_t address, uint32_t value)
{
    memcpy(&t1479_36cc10_exp[address], &value, sizeof value);
}

/* esp given, obj/a/b/c on the stack; exp image = the pre-call image plus the listed stores */
static void t1479_36cc10_setup(uint32_t esp, uint32_t obj, uint32_t a, uint32_t b, uint32_t c)
{
    prepare();
    g_esp = esp;
    guest_write32(esp, 0x12345678u);
    guest_write32(esp + 4u, obj);
    guest_write32(esp + 8u, a);
    guest_write32(esp + 12u, b);
    guest_write32(esp + 16u, c);
    guest_write32(esp + 20u, 0xA5A5A5A5u); /* a fifth word: not an argument */
    memcpy(t1479_36cc10_exp, memory, sizeof memory);
}

static void t1479_36cc10_finish(uint32_t exp_eax, uint32_t exp_ecx, uint32_t exp_edx)
{
    g_ebx = 0xB1B1B1B1u;
    g_esi = 0xB2B2B2B2u;
    g_edi = 0xB3B3B3B3u;
    g_ebp = 0xB4B4B4B4u;
    g_df = 0;
    invoke(0x0036CC10u, 4u);
    CHECK(g_eax == exp_eax);
    CHECK(g_ecx == exp_ecx);
    CHECK(g_edx == exp_edx);
    CHECK(g_ebx == 0xB1B1B1B1u && g_esi == 0xB2B2B2B2u);
    CHECK(g_edi == 0xB3B3B3B3u && g_ebp == 0xB4B4B4B4u);
    CHECK(memcmp(t1479_36cc10_exp, memory, sizeof memory) == 0);
}

/* plain object away from the stack: three stores, everything else untouched */
static void t1479_36cc10_plain(uint32_t obj, uint32_t a, uint32_t b, uint32_t c)
{
    t1479_36cc10_setup(0x100u, obj, a, b, c);
    t1479_36cc10_put(obj + 0x24u, a);
    t1479_36cc10_put(obj + 0x28u, b);
    t1479_36cc10_put(obj + 0x2Cu, c);
    t1479_36cc10_finish(obj, c, b);
}

static void test_t1479_appendix_0036cc10(void)
{
    static const uint32_t values[] = {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x12345678u};
    unsigned i;
    unsigned j;

    /* sentinels around the three stored dwords: +0x20 and +0x30 must stay */
    memset(&memory[T1479_36CC10_OBJ], 0x5A, 0x80u);
    t1479_36cc10_plain(T1479_36CC10_OBJ, 0x11111111u, 0x22222222u, 0x33333333u);
    CHECK(guest_read32(T1479_36CC10_OBJ + 0x20u) == 0x5A5A5A5Au);
    CHECK(guest_read32(T1479_36CC10_OBJ + 0x30u) == 0x5A5A5A5Au);
    CHECK(guest_read32(T1479_36CC10_OBJ + 0x24u) == 0x11111111u);
    CHECK(guest_read32(T1479_36CC10_OBJ + 0x28u) == 0x22222222u);
    CHECK(guest_read32(T1479_36CC10_OBJ + 0x2Cu) == 0x33333333u);
    /* every value class in every argument position */
    for (i = 0; i < 6u; i++) {
        for (j = 0; j < 6u; j++) {
            t1479_36cc10_plain(T1479_36CC10_OBJ + 0x100u, values[i], values[j], values[(i + j) % 6u]);
            t1479_36cc10_plain(T1479_36CC10_OBJ + 0x200u, values[j], values[(i + 3u * j) % 6u], values[i]);
        }
    }
    /* object at odd alignments and in low memory */
    t1479_36cc10_plain(T1479_36CC10_OBJ + 0x301u, 0xA1A1A1A1u, 0xB2B2B2B2u, 0xC3C3C3C3u);
    t1479_36cc10_plain(0x2000u, 0xA1A1A1A1u, 0xB2B2B2B2u, 0xC3C3C3C3u);
    t1479_36cc10_plain(0x7F0000u, 0xA1A1A1A1u, 0xB2B2B2B2u, 0xC3C3C3C3u);
    /* obj + 0x24 wraps at 32 bits: obj = 0xFFFFFFE0 stores to 4, 8 and 0xC */
    t1479_36cc10_plain(0xFFFFFFE0u, 0xA1A1A1A1u, 0xB2B2B2B2u, 0xC3C3C3C3u);
    CHECK(guest_read32(4u) == 0xA1A1A1A1u && guest_read32(8u) == 0xB2B2B2B2u);
    CHECK(guest_read32(12u) == 0xC3C3C3C3u);
    /* obj = 0xFFFFFFDC: +0x24 wraps to 0, +0x28 to 4, +0x2C to 8 */
    t1479_36cc10_plain(0xFFFFFFDCu, 0x01010101u, 0x02020202u, 0x03030303u);

    /* alias (a): the stores land on the return slot (obj = esp - 0x24): +0x24 = esp (return
     * slot), +0x28 = arg0 slot, +0x2C = arg1 slot. Arguments were read before. */
    t1479_36cc10_setup(0x100u, 0x100u - 0x24u, 0x0A0A0A0Au, 0x0B0B0B0Bu, 0x0C0C0C0Cu);
    t1479_36cc10_put(0x100u, 0x0A0A0A0Au);
    t1479_36cc10_put(0x104u, 0x0B0B0B0Bu);
    t1479_36cc10_put(0x108u, 0x0C0C0C0Cu);
    t1479_36cc10_finish(0x100u - 0x24u, 0x0C0C0C0Cu, 0x0B0B0B0Bu);
    /* obj = esp - 0x20: stores on arg0, arg1, arg2 slots */
    t1479_36cc10_setup(0x100u, 0x100u - 0x20u, 0x0A0A0A0Au, 0x0B0B0B0Bu, 0x0C0C0C0Cu);
    t1479_36cc10_put(0x104u, 0x0A0A0A0Au);
    t1479_36cc10_put(0x108u, 0x0B0B0B0Bu);
    t1479_36cc10_put(0x10Cu, 0x0C0C0C0Cu);
    t1479_36cc10_finish(0x100u - 0x20u, 0x0C0C0C0Cu, 0x0B0B0B0Bu);
    /* obj = esp - 0x14: the first store (+0x24) lands on the arg3 slot, which is read AFTER it,
     * so c becomes a: ECX = a, [esp+0x14] = b, [esp+0x18] = a */
    t1479_36cc10_setup(0x100u, 0x100u - 0x14u, 0x0A0A0A0Au, 0x0B0B0B0Bu, 0x0C0C0C0Cu);
    t1479_36cc10_put(0x110u, 0x0A0A0A0Au);
    t1479_36cc10_put(0x114u, 0x0B0B0B0Bu);
    t1479_36cc10_put(0x118u, 0x0A0A0A0Au);
    t1479_36cc10_finish(0x100u - 0x14u, 0x0A0A0A0Au, 0x0B0B0B0Bu);
    /* obj = esp - 0x18: the second store (+0x28 = arg3 slot) comes after the c read, ECX = c */
    t1479_36cc10_setup(0x100u, 0x100u - 0x18u, 0x0A0A0A0Au, 0x0B0B0B0Bu, 0x0C0C0C0Cu);
    t1479_36cc10_put(0x10Cu, 0x0A0A0A0Au);
    t1479_36cc10_put(0x110u, 0x0B0B0B0Bu);
    t1479_36cc10_put(0x114u, 0x0C0C0C0Cu);
    t1479_36cc10_finish(0x100u - 0x18u, 0x0C0C0C0Cu, 0x0B0B0B0Bu);
    /* obj = esp - 0x1C: the third store (+0x2C) lands on the arg3 slot after the read */
    t1479_36cc10_setup(0x100u, 0x100u - 0x1Cu, 0x0A0A0A0Au, 0x0B0B0B0Bu, 0x0C0C0C0Cu);
    t1479_36cc10_put(0x108u, 0x0A0A0A0Au);
    t1479_36cc10_put(0x10Cu, 0x0B0B0B0Bu);
    t1479_36cc10_put(0x110u, 0x0C0C0C0Cu);
    t1479_36cc10_finish(0x100u - 0x1Cu, 0x0C0C0C0Cu, 0x0B0B0B0Bu);
    /* sweep of every object position from esp - 0x30 to esp + 0x1C (stores land on the
     * return slot, each argument slot, the word after the args and the dead area) */
    for (i = 0; i < 12u; i++) {
        const uint32_t obj = 0x100u - 0x30u + 4u * i;
        uint32_t c_seen = 0x0C0C0C0Cu;
        t1479_36cc10_setup(0x100u, obj, 0x0A0A0A0Au, 0x0B0B0B0Bu, 0x0C0C0C0Cu);
        /* first store: a at obj+0x24, the read of c happens right after it */
        if (obj + 0x24u == 0x110u) {
            c_seen = 0x0A0A0A0Au;
        }
        t1479_36cc10_put(obj + 0x24u, 0x0A0A0A0Au);
        t1479_36cc10_put(obj + 0x28u, 0x0B0B0B0Bu);
        t1479_36cc10_put(obj + 0x2Cu, c_seen);
        t1479_36cc10_finish(obj, c_seen, 0x0B0B0B0Bu);
    }
}
