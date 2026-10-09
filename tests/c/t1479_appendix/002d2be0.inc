/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent control for 0x002D2BE0, derived from the original instructions only:
 *   cl = byte [0x7DE455]; eax = 0; if (cl != 0x0A) eax = 0x20000; ecx = [esp+8] << 5 (the
 *   whole register, the first argument at [esp+4] is never read); edx = [ecx + 0x4D1F88];
 *   if (edx == 0) eax |= 0x2000000; ret. There is no PUSH or POP and no guest store, so
 *   POP-slot aliasing does not exist here: the aliases are the table entry placed on the
 *   return-address slot, the first argument slot and the second argument slot (read
 *   aliases), and the whole guest memory is diffed after every call (no write at all).
 *   The shift and the address add wrap at 32 bits. Neighbouring bytes of the mode byte
 *   hold 0x0A to catch a wider read, entries hold bytes that are zero only in part.
 * MUTANT: g_eax = (mode == 0x0Au) ? 0u : 0x00020000u; -> g_eax = (mode == 0x0Bu) ? 0u : 0x00020000u;
 * MUTANT: g_ecx = guest_read32(g_esp + 8u) << 5; -> g_ecx = guest_read32(g_esp + 4u) << 5;
 * MUTANT: g_eax |= 0x02000000u; -> g_eax |= 0x00020000u;
 * MUTANT: if (g_edx == 0u) { -> if ((g_edx & 0xFFu) == 0u) {
 */

#define C2D2BE0_MODE 0x7de455u
#define C2D2BE0_TABLE 0x4d1f88u

static uint8_t t1479_2d2be0_before[sizeof memory];
static uint32_t t1479_2d2be0_ret_word = 0x12345678u;

static uint32_t t1479_2d2be0_entry(uint32_t index)
{
    return (index % 4u == 3u) ? 0u : 0x1000u + index;
}

static void t1479_2d2be0_tables(void)
{
    for (uint32_t index = 0; index < 64u; index++) {
        const uint32_t address = C2D2BE0_TABLE + 32u * index;
        guest_write32(address, t1479_2d2be0_entry(index));
        for (uint32_t offset = 4u; offset < 32u; offset += 4u) {
            guest_write32(address + offset, 0xABCD0000u + offset);  /* only dword 0 is read */
        }
    }
    /* large index region and the wrapped addresses just below the table */
    guest_write32(C2D2BE0_TABLE - 0x20u, 0x77770000u);
    guest_write32(C2D2BE0_TABLE + 0x320000u, 0u);
    guest_write32(C2D2BE0_TABLE + 0x31FFE0u, 0x66660000u);
}

static void t1479_2d2be0_run(uint32_t esp, uint8_t mode, uint32_t arg0, uint32_t arg1,
                             uint32_t exp_eax, uint32_t exp_ecx, uint32_t exp_edx)
{
    prepare();
    g_esp = esp;
    guest_write32(esp, t1479_2d2be0_ret_word);
    guest_write32(esp + 4u, arg0);
    guest_write32(esp + 8u, arg1);
    memory[C2D2BE0_MODE - 1u] = 0x0Au;
    memory[C2D2BE0_MODE] = mode;
    memory[C2D2BE0_MODE + 1u] = 0x0Au;
    g_ebx = 0xB5B5B5B5u;
    g_esi = 0x55555555u;
    g_edi = 0xD5D5D5D5u;
    g_ebp = 0xBEBEBEBEu;
    g_df = 0;
    memcpy(t1479_2d2be0_before, memory, sizeof memory);
    invoke(0x002D2BE0u, 4u);
    CHECK(g_eax == exp_eax);
    CHECK(g_ecx == exp_ecx);
    CHECK(g_edx == exp_edx);
    CHECK(g_ebx == 0xB5B5B5B5u && g_esi == 0x55555555u);
    CHECK(g_edi == 0xD5D5D5D5u && g_ebp == 0xBEBEBEBEu);
    CHECK(g_esp == esp + 4u);
    CHECK(memcmp(t1479_2d2be0_before, memory, sizeof memory) == 0);
}

static void test_t1479_appendix_002d2be0(void)
{
    t1479_2d2be0_tables();
    /* literal anchors: mode 0x0A drops 0x20000, an empty entry adds 0x2000000 */
    t1479_2d2be0_run(0x7000u, 0x0Au, 0xAAAAAAAAu, 0u, 0x00000000u, 0x00000000u, 0x00001000u);
    t1479_2d2be0_run(0x7000u, 0x0Au, 0xAAAAAAAAu, 1u, 0x00000000u, 0x00000020u, 0x00001001u);
    t1479_2d2be0_run(0x7000u, 0x09u, 0xAAAAAAAAu, 0u, 0x00020000u, 0x00000000u, 0x00001000u);
    t1479_2d2be0_run(0x7000u, 0x09u, 0xAAAAAAAAu, 3u, 0x02020000u, 0x00000060u, 0x00000000u);
    t1479_2d2be0_run(0x7000u, 0x0Au, 0xAAAAAAAAu, 3u, 0x02000000u, 0x00000060u, 0x00000000u);
    t1479_2d2be0_run(0x7000u, 0x0Au, 0xAAAAAAAAu, 2u, 0x00000000u, 0x00000040u, 0x00001002u);
    /* the mode byte only: neighbours hold 0x0A, other bit patterns are not 0x0A */
    {
        static const uint8_t modes[] = {0x00u, 0x01u, 0x09u, 0x0Bu, 0x1Au, 0x2Au, 0x4Au,
                                        0x8Au, 0xFAu, 0xFFu, 0x0Au};
        for (size_t i = 0; i < sizeof modes; i++) {
            t1479_2d2be0_run(0x7000u, modes[i], 0x11111111u, 4u,
                             (modes[i] == 0x0Au ? 0u : 0x20000u), 0x80u, 0x1004u);
            t1479_2d2be0_run(0x7000u, modes[i], 0x11111111u, 3u,
                             (modes[i] == 0x0Au ? 0u : 0x20000u) | 0x2000000u, 0x60u, 0u);
        }
    }
    /* index sweep (stride 0x20 = shift 5), the first argument must not matter */
    for (uint32_t index = 0; index < 64u; index++) {
        const uint32_t value = (index % 4u == 3u) ? 0u : 0x1000u + index;
        t1479_2d2be0_run(0x7000u, 0x30u, 0x1000u + index * 7u, index,
                         0x20000u | (value == 0u ? 0x2000000u : 0u), index * 32u, value);
        t1479_2d2be0_run(0x7000u, 0x0Au, index ^ 0x5Au, index,
                         (value == 0u ? 0x2000000u : 0u), index * 32u, value);
    }
    /* the whole dword decides zero, not a byte or a half: set bytes of each lane */
    {
        static const uint32_t patterns[] = {0x00000001u, 0x00000100u, 0x00010000u,
                                            0x01000000u, 0x80000000u, 0xFFFFFFFFu,
                                            0x0000FF00u, 0x00FF0000u};
        for (size_t i = 0; i < sizeof patterns / sizeof patterns[0]; i++) {
            guest_write32(C2D2BE0_TABLE + 32u * 10u, patterns[i]);
            t1479_2d2be0_run(0x7000u, 0x09u, 0u, 10u, 0x20000u, 0x140u, patterns[i]);
            t1479_2d2be0_run(0x7000u, 0x0Au, 0u, 10u, 0u, 0x140u, patterns[i]);
        }
        guest_write32(C2D2BE0_TABLE + 32u * 10u, 0u);
        t1479_2d2be0_run(0x7000u, 0x09u, 0u, 10u, 0x02020000u, 0x140u, 0u);
        t1479_2d2be0_run(0x7000u, 0x0Au, 0u, 10u, 0x02000000u, 0x140u, 0u);
        guest_write32(C2D2BE0_TABLE + 32u * 10u, 0x100Au);
    }
    /* 32-bit wrap of the shift and of the address add */
    t1479_2d2be0_run(0x7000u, 0x09u, 0u, 0xFFFFFFFFu, 0x20000u, 0xFFFFFFE0u, 0x77770000u);
    t1479_2d2be0_run(0x7000u, 0x0Au, 0u, 0x08000000u, 0x00000000u, 0x00000000u, 0x00001000u);
    t1479_2d2be0_run(0x7000u, 0x0Au, 0u, 0x08000001u, 0x00000000u, 0x00000020u, 0x00001001u);
    t1479_2d2be0_run(0x7000u, 0x09u, 0u, 0x80000000u, 0x00020000u, 0x00000000u, 0x00001000u);
    t1479_2d2be0_run(0x7000u, 0x09u, 0u, 0x80000001u, 0x00020000u, 0x00000020u, 0x00001001u);
    t1479_2d2be0_run(0x7000u, 0x09u, 0u, 0xF8000002u, 0x00020000u, 0x00000040u, 0x00001002u);
    t1479_2d2be0_run(0x7000u, 0x09u, 0u, 0x10000003u, 0x02020000u, 0x00000060u, 0x00000000u);
    t1479_2d2be0_run(0x7000u, 0x09u, 0u, 0x07FFFFFFu, 0x00020000u, 0xFFFFFFE0u, 0x77770000u);
    /* large in-range indexes: 0x19000 << 5 = 0x320000, and one entry below it */
    t1479_2d2be0_run(0x7000u, 0x09u, 0u, 0x19000u, 0x02020000u, 0x320000u, 0u);
    t1479_2d2be0_run(0x7000u, 0x0Au, 0u, 0x18FFFu, 0x00000000u, 0x31FFE0u, 0x66660000u);

    /* aliases: no store exists, the reads must still see the stack words itself. */
    t1479_2d2be0_tables();
    /* entry address == the second argument slot (esp + 8): index 5 holds the argument 5, and
     * index 0 holds the argument 0 (an empty entry) */
    t1479_2d2be0_run(C2D2BE0_TABLE + 5u * 32u - 8u, 0x09u, 0x30u, 5u, 0x00020000u, 0xA0u, 5u);
    t1479_2d2be0_run(C2D2BE0_TABLE - 8u, 0x09u, 0x30u, 0u, 0x02020000u, 0u, 0u);
    /* entry address == the first argument slot (esp + 4), which is otherwise never read */
    t1479_2d2be0_run(C2D2BE0_TABLE + 5u * 32u - 4u, 0x0Au, 0x7777u, 5u, 0x00000000u, 0xA0u,
                     0x7777u);
    t1479_2d2be0_run(C2D2BE0_TABLE + 5u * 32u - 4u, 0x0Au, 0u, 5u, 0x02000000u, 0xA0u, 0u);
    /* entry address == the return-address slot */
    t1479_2d2be0_run(C2D2BE0_TABLE + 5u * 32u, 0x0Au, 0x7777u, 5u, 0x00000000u, 0xA0u,
                     0x12345678u);
    t1479_2d2be0_ret_word = 0u;
    t1479_2d2be0_run(C2D2BE0_TABLE + 5u * 32u, 0x09u, 0x7777u, 5u, 0x02020000u, 0xA0u, 0u);
    t1479_2d2be0_ret_word = 0x12345678u;
}
