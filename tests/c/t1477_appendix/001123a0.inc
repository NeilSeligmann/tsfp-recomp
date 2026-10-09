/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent native control for 0x001123A0, derived from the original listing only:
 *   mov eax,[0x6B7AA8]; test eax,eax; jne skip; mov dword [0x0073D950u],0; skip: ret
 * Exit on every path: EAX = [0x6B7AA8] (zero when the store ran), ECX/EDX/EBX/ESI/EDI/EBP
 * untouched, ESP + 4 (plain cdecl ret). The only possible memory change is the dword at
 * 0x0073D950u becoming zero, and only when the 32-bit gate is zero. There is NO PUSH or POP in the
 * original, so saved-register slot aliasing does not exist here: the alias controls cover the
 * return slot and the argument slot under the store target, and the stack placed on the gate. */

#define C001123a0_GATE 0x006B7AA8u
#define C001123a0_TARGET 0x0073D950u
#define C001123a0_SENTINEL 0xA5A5A5A5u

static uint8_t c001123a0_before[sizeof memory];

/* Run with the stack at esp, the gate and the target preset; check everything. */
static void c001123a0_run(uint32_t esp, uint32_t gate, uint32_t target_before)
{
    prepare();
    g_esp = esp;
    guest_write32(g_esp, 0x12345678u); /* return slot */
    guest_write32(g_esp + 4u, 0xCAFEBABEu); /* a would-be argument, unused by the original */
    g_ebx = 0xB4B4B4B4u;
    g_esi = 0x54545454u;
    g_edi = 0xD4D4D4D4u;
    g_ebp = 0xBDBDBDBDu;
    g_df = 0;
    guest_write32(C001123a0_TARGET, target_before);
    guest_write32(C001123a0_GATE, gate); /* last, so a stack placed over the gate keeps it */
    const uint32_t return_slot = guest_read32(esp);
    const uint32_t arg_slot = guest_read32(esp + 4u);
    const uint32_t target_seen = guest_read32(C001123a0_TARGET);
    memcpy(c001123a0_before, memory, sizeof memory);
    invoke(0x001123A0, 4u);
    const int store = gate == 0u;
    CHECK(g_eax == gate);
    CHECK(g_ecx == 0x22222222u && g_edx == 0x33333333u);
    CHECK(g_ebx == 0xB4B4B4B4u && g_esi == 0x54545454u);
    CHECK(g_edi == 0xD4D4D4D4u && g_ebp == 0xBDBDBDBDu);
    CHECK(g_esp == esp + 4u && g_df == 0);
    CHECK(guest_read32(C001123a0_TARGET) == (store ? 0u : target_seen));
    /* memory diff: nothing but the target dword may differ */
    for (size_t a = 0; a < sizeof memory; a++) {
        if (a >= C001123a0_TARGET && a < C001123a0_TARGET + 4u) {
            continue;
        }
        if (c001123a0_before[a] != memory[a]) {
            CHECK(!"stray write");
            break;
        }
    }
    (void)return_slot;
    (void)arg_slot;
}

static void test_t1477_appendix_001123a0(void)
{
    /* gate zero: store runs, over a sentinel and over zero (zero written over zero) */
    c001123a0_run(0x100u, 0u, C001123a0_SENTINEL);
    c001123a0_run(0x100u, 0u, 0u);
    c001123a0_run(0x100u, 0u, 0xFFFFFFFFu);
    /* gate nonzero in every bit position and byte lane: 32-bit test, no store */
    const uint32_t gates[] = {1u, 2u, 0x80u, 0xFFu, 0x100u, 0x8000u, 0xFFFFu, 0x10000u, 0x00800000u,
                              0x00FF0000u, 0x01000000u, 0x7FFFFFFFu, 0x80000000u, 0xFF000000u,
                              0xFFFFFFFFu, 0xFFFFFFFEu, 0x12345678u};
    for (unsigned i = 0; i < sizeof gates / sizeof gates[0]; i++) {
        c001123a0_run(0x100u, gates[i], C001123a0_SENTINEL);
        c001123a0_run(0x2000u, gates[i], 0u);
    }
    /* neighbours of the target dword are untouched when it is cleared */
    guest_write32(C001123a0_TARGET - 4u, 0x11AA11AAu);
    guest_write32(C001123a0_TARGET + 4u, 0x22BB22BBu);
    c001123a0_run(0x100u, 0u, C001123a0_SENTINEL);
    CHECK(guest_read32(C001123a0_TARGET - 4u) == 0x11AA11AAu);
    CHECK(guest_read32(C001123a0_TARGET + 4u) == 0x22BB22BBu);
    CHECK(guest_read32(C001123a0_TARGET) == 0u);

    /* (a) return-slot alias: the stack sits ON the target dword, the store zeroes the
     * return slot, the wrapper still pops 4 and no register is affected. */
    c001123a0_run(C001123a0_TARGET, 0u, C001123a0_SENTINEL);
    CHECK(guest_read32(C001123a0_TARGET) == 0u);
    CHECK(guest_read32(C001123a0_TARGET + 4u) == 0xCAFEBABEu); /* arg slot untouched */
    /* gate nonzero: the return slot keeps its value */
    c001123a0_run(C001123a0_TARGET, 5u, C001123a0_SENTINEL);
    CHECK(guest_read32(C001123a0_TARGET) == C001123a0_SENTINEL /* preset target value survives */);
    /* (a) argument-slot alias: esp+4 is the target, the zero lands on the argument slot */
    c001123a0_run(C001123a0_TARGET - 4u, 0u, C001123a0_SENTINEL);
    CHECK(guest_read32(C001123a0_TARGET) == 0u);
    CHECK(guest_read32(C001123a0_TARGET - 4u) == 0x12345678u); /* return slot kept */
    c001123a0_run(C001123a0_TARGET - 4u, 7u, C001123a0_SENTINEL);
    CHECK(guest_read32(C001123a0_TARGET) == C001123a0_SENTINEL);
    /* stack placed on the gate: the return slot IS the gate, so the gate is the dword
     * written last by the helper (set to 0 here: store runs and EAX = 0, ESP + 4) */
    c001123a0_run(C001123a0_GATE, 0u, C001123a0_SENTINEL);
    CHECK(guest_read32(C001123a0_TARGET) == 0u);
    c001123a0_run(C001123a0_GATE, 0x12345678u, C001123a0_SENTINEL);
    CHECK(guest_read32(C001123a0_TARGET) == C001123a0_SENTINEL);
    /* the argument slot over the gate: gate = esp+4 */
    c001123a0_run(C001123a0_GATE - 4u, 0u, C001123a0_SENTINEL);
    CHECK(guest_read32(C001123a0_GATE - 4u) == 0x12345678u);
}
