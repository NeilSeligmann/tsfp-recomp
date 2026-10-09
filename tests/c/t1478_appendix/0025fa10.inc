/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent control for 0x0025FA10 (derived from the original instructions only):
 * edx = [0x78B520]; push esi; eax = 0; edx <= 0 (signed) -> exit with ecx untouched.
 * Else esi = arg0 (read after the push), ecx = 0x78B2E0; loop: [ecx] == esi -> return
 * eax (index); eax++, ecx += 0x20 while eax < edx (signed); fall-through -> eax = 0.
 * Pops esi from the pushed slot. The only memory write is the push at (esp - 4). */

#define C25FA10_COUNT 0x78b520u
#define C25FA10_TABLE 0x78b2e0u

static uint8_t c25fa10_before[sizeof memory];

static void c25fa10_run(uint32_t esp, uint32_t key, uint32_t count, uint32_t expected_eax,
                        uint32_t expected_ecx)
{
    prepare();
    g_esp = esp;
    guest_write32(g_esp, 0x12345678u);
    g_ebx = 0xB4B4B4B4u;
    g_esi = 0x54545454u;
    g_edi = 0xD4D4D4D4u;
    g_ebp = 0xBDBDBDBDu;
    g_df = 0;
    guest_write32(C25FA10_COUNT, count);
    guest_write32(g_esp + 4u, key);
    memcpy(c25fa10_before, memory, sizeof memory);
    invoke(0x0025FA10u, 4u);
    CHECK(g_eax == expected_eax);
    CHECK(g_ecx == expected_ecx);
    CHECK(g_edx == count);
    CHECK(g_ebx == 0xB4B4B4B4u && g_esi == 0x54545454u);
    CHECK(g_edi == 0xD4D4D4D4u && g_ebp == 0xBDBDBDBDu);
    CHECK(g_esp == esp + 4u);
    /* only the pushed slot may change, and it holds the entry esi */
    CHECK(guest_read32(esp - 4u) == 0x54545454u);
    for (size_t a = 0; a < sizeof memory; a++) {
        if (a >= esp - 4u && a < esp) {
            continue;
        }
        if (c25fa10_before[a] != memory[a]) {
            CHECK(!"stray write");
            break;
        }
    }
}

static void test_t1478_appendix_0025fa10(void)
{
    memset(&memory[C25FA10_TABLE - 0x100u], 0, 0x1400u);
    for (uint32_t i = 0; i < 100u; i++) {
        uint32_t entry = C25FA10_TABLE + i * 0x20u;
        guest_write32(entry, 0x4000u + i);
        for (uint32_t offset = 4u; offset < 0x20u; offset += 4u) {
            guest_write32(entry + offset, 0x4005u); /* only dword 0 is compared */
        }
    }
    /* count <= 0 (signed): nothing scanned, ecx is the entry value, edx = count */
    c25fa10_run(0x100u, 0x4000u, 0u, 0u, 0x22222222u);
    c25fa10_run(0x100u, 0x4000u, 0xFFFFFFFFu, 0u, 0x22222222u);
    c25fa10_run(0x100u, 0x4000u, 0x80000000u, 0u, 0x22222222u);
    /* count 1: hit at index 0 returns 0 (ecx shows where it stopped), miss returns 0 too */
    c25fa10_run(0x100u, 0x4000u, 1u, 0u, C25FA10_TABLE);
    c25fa10_run(0x100u, 0x4001u, 1u, 0u, C25FA10_TABLE + 0x20u);
    /* count 5: hits at every index, miss at the count bound (entry 5 is not scanned) */
    for (uint32_t i = 0; i < 5u; i++) {
        c25fa10_run(0x100u, 0x4000u + i, 5u, i, C25FA10_TABLE + 0x20u * i);
    }
    c25fa10_run(0x100u, 0x4005u, 5u, 0u, C25FA10_TABLE + 0x20u * 5u);
    c25fa10_run(0x100u, 0x4006u, 5u, 0u, C25FA10_TABLE + 0x20u * 5u);
    c25fa10_run(0x100u, 0x9999u, 5u, 0u, C25FA10_TABLE + 0x20u * 5u);
    /* later dwords of an entry equal to the key do not match */
    c25fa10_run(0x100u, 0x4005u, 4u, 0u, C25FA10_TABLE + 0x20u * 4u);
    /* larger counts */
    c25fa10_run(0x100u, 0x4000u + 99u, 100u, 99u, C25FA10_TABLE + 0x20u * 99u);
    c25fa10_run(0x100u, 0x7777u, 100u, 0u, C25FA10_TABLE + 0x20u * 100u);
    /* duplicates: first match wins */
    guest_write32(C25FA10_TABLE + 0x20u * 4u, 0x4002u);
    c25fa10_run(0x100u, 0x4002u, 10u, 2u, C25FA10_TABLE + 0x40u);
    /* high-bit key equality */
    guest_write32(C25FA10_TABLE + 0x20u * 3u, 0x80000000u);
    c25fa10_run(0x100u, 0x80000000u, 10u, 3u, C25FA10_TABLE + 0x60u);
    c25fa10_run(0x100u, 0x00000000u, 10u, 0u, C25FA10_TABLE + 0x20u * 10u);

    /* push/scan alias: the stack slot of the pushed esi is entry 2 dword 0 of the table,
     * so the scan sees the pushed esi value (0x54545454) there. The key equals it. */
    {
        uint32_t esp = C25FA10_TABLE + 0x40u + 4u;
        prepare();
        g_esp = esp;
        guest_write32(g_esp, 0x12345678u);
        g_ebx = 0xB4B4B4B4u;
        g_esi = 0x54545454u;
        g_edi = 0xD4D4D4D4u;
        g_ebp = 0xBDBDBDBDu;
        guest_write32(C25FA10_COUNT, 4u);
        guest_write32(g_esp + 4u, 0x54545454u);
        invoke(0x0025FA10u, 4u);
        CHECK(g_eax == 2u);
        CHECK(g_ecx == C25FA10_TABLE + 0x40u);
        CHECK(g_edx == 4u);
        CHECK(g_esi == 0x54545454u);
        CHECK(g_ebx == 0xB4B4B4B4u && g_edi == 0xD4D4D4D4u && g_ebp == 0xBDBDBDBDu);
        CHECK(guest_read32(C25FA10_TABLE + 0x40u) == 0x54545454u);
        /* a pop alias: the key matches nothing, esi comes back from the same slot */
        g_esp = esp;
        g_esi = 0x54545454u;
        guest_write32(g_esp + 4u, 0x1u);
        invoke(0x0025FA10u, 4u);
        CHECK(g_eax == 0u && g_ecx == C25FA10_TABLE + 0x20u * 4u && g_esi == 0x54545454u);
    }
}
