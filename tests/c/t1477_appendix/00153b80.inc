/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent native control for 0x00153B80, derived from the original listing only:
 *   ecx=[esp+4]; xor eax,eax; xor edx,edx; add ecx,1; adc edx,eax;
 *   [0x7497C8]=eax; [0x4E7950]=ecx; [0x4E7954]=edx; [0x7497CC]=eax; [0x7497D0]=eax; ret
 * Exit (single path): EAX = 0, ECX = arg + 1 (mod 2^32), EDX = carry of that add (1 only for
 * arg == 0xFFFFFFFF, EDX was zeroed first), EBX/ESI/EDI/EBP untouched, ESP + 4 (plain cdecl
 * ret). Memory: the five dword stores 0, arg+1, carry, 0, 0 at the five addresses above, nothing
 * else (checked as a full-image diff). The argument is read FIRST, so a store on the argument
 * slot cannot change an exit register. There is NO PUSH or POP in the original, so
 * saved-register slot aliasing does not exist here. Alias controls: the stack ON the stores:
 * return slot and argument slot under two of the stored dwords, the argument slot under the
 * ECX store and the EDX store. The register values for the alias runs were also observed from
 * the ORIGINAL bytes in Unicorn 2.1.4 (original-only, tmp/c96/t153b80.py, untracked). */

#define C00153b80_A 0x007497C8u
#define C00153b80_B 0x004E7950u
#define C00153b80_C 0x004E7954u
#define C00153b80_D 0x007497CCu
#define C00153b80_E 0x007497D0u

static uint8_t c00153b80_expect[sizeof memory];

static void c00153b80_wr(uint32_t address, uint32_t value)
{
    for (unsigned b = 0; b < 4u; b++) {
        c00153b80_expect[address + b] = (uint8_t)(value >> (8u * b));
    }
}

/* Sentinels go into all five targets and their neighbours first, then the stack slots (so an
 * aliased slot holds the return value / argument at entry), then the call. */
static void c00153b80_run(uint32_t esp, uint32_t arg, uint32_t expected_ecx, uint32_t expected_edx)
{
    prepare();
    g_esp = esp;
    g_ebx = 0xB4B4B4B4u;
    g_esi = 0x54545454u;
    g_edi = 0xD4D4D4D4u;
    g_ebp = 0xBDBDBDBDu;
    g_df = 0;
    const uint32_t targets[] = {C00153b80_A, C00153b80_B, C00153b80_C, C00153b80_D, C00153b80_E};
    for (unsigned i = 0; i < 5u; i++) {
        guest_write32(targets[i] - 4u, 0xA0A0A0A0u + i);
        guest_write32(targets[i], 0xF0F0F0F0u + i);
        guest_write32(targets[i] + 4u, 0xC0C0C0C0u + i);
    }
    guest_write32(g_esp, 0x12345678u);
    guest_write32(g_esp + 4u, arg);
    memcpy(c00153b80_expect, memory, sizeof memory);
    invoke(0x00153B80u, 4u);
    CHECK(g_eax == 0u);
    CHECK(g_ecx == expected_ecx);
    CHECK(g_edx == expected_edx);
    CHECK(g_ebx == 0xB4B4B4B4u && g_esi == 0x54545454u);
    CHECK(g_edi == 0xD4D4D4D4u && g_ebp == 0xBDBDBDBDu);
    CHECK(g_esp == esp + 4u && g_df == 0);
    /* expected image: the five stores in listing order on the pre-call image */
    c00153b80_wr(C00153b80_A, 0u);
    c00153b80_wr(C00153b80_B, expected_ecx);
    c00153b80_wr(C00153b80_C, expected_edx);
    c00153b80_wr(C00153b80_D, 0u);
    c00153b80_wr(C00153b80_E, 0u);
    CHECK(memcmp(c00153b80_expect, memory, sizeof memory) == 0);
    CHECK(guest_read32(C00153b80_B) == expected_ecx && guest_read32(C00153b80_C) == expected_edx);
}

static void test_t1477_appendix_00153b80(void)
{
    /* arg + 1 boundaries, signed and unsigned: only 0xFFFFFFFF carries */
    c00153b80_run(0x100u, 0u, 1u, 0u);
    c00153b80_run(0x100u, 1u, 2u, 0u);
    c00153b80_run(0x100u, 2u, 3u, 0u);
    c00153b80_run(0x100u, 0x7FFFFFFEu, 0x7FFFFFFFu, 0u);
    c00153b80_run(0x100u, 0x7FFFFFFFu, 0x80000000u, 0u); /* signed overflow, no carry */
    c00153b80_run(0x100u, 0x80000000u, 0x80000001u, 0u);
    c00153b80_run(0x100u, 0xFFFFFFFDu, 0xFFFFFFFEu, 0u);
    c00153b80_run(0x100u, 0xFFFFFFFEu, 0xFFFFFFFFu, 0u);
    c00153b80_run(0x100u, 0xFFFFFFFFu, 0u, 1u); /* wrap: ecx 0, carry 1 */
    c00153b80_run(0x100u, 0x12345678u, 0x12345679u, 0u);
    c00153b80_run(0x100u, 0x000000FFu, 0x00000100u, 0u);  /* byte carry, not a 32-bit carry */
    c00153b80_run(0x100u, 0x0000FFFFu, 0x00010000u, 0u);
    c00153b80_run(0x100u, 0x00FFFFFFu, 0x01000000u, 0u);
    c00153b80_run(0x2000u, 0xFFFFFFFFu, 0u, 1u);           /* other stack position */

    /* (a) return slot AND argument slot are the stored dwords 0x7497C8 / 0x7497CC: the argument
     * is read first, so arg + 1 / carry are still exact; the slots end up zero. */
    c00153b80_run(C00153b80_A, 0xFFFFFFFFu, 0u, 1u);
    CHECK(guest_read32(C00153b80_A) == 0u && guest_read32(C00153b80_D) == 0u);
    c00153b80_run(C00153b80_A, 0x41u, 0x42u, 0u);
    /* (a) return slot == the ECX store, argument slot == the EDX store (0x4E7950 / 0x4E7954):
     * the return slot receives arg + 1, the argument slot receives the carry */
    c00153b80_run(C00153b80_B, 0x41u, 0x42u, 0u);
    CHECK(guest_read32(C00153b80_B) == 0x42u && guest_read32(C00153b80_C) == 0u);
    c00153b80_run(C00153b80_B, 0xFFFFFFFFu, 0u, 1u);
    CHECK(guest_read32(C00153b80_B) == 0u && guest_read32(C00153b80_C) == 1u);
    /* (a) argument slot == the ECX store (esp + 4 == 0x4E7950) and == the last store dword */
    c00153b80_run(C00153b80_B - 4u, 0x55u, 0x56u, 0u);
    CHECK(guest_read32(C00153b80_B) == 0x56u);
    c00153b80_run(C00153b80_E - 4u, 0xFFFFFFFFu, 0u, 1u);
    CHECK(guest_read32(C00153b80_E) == 0u);
    /* return slot straddles two stores: esp = 0x7497CA covers bytes of A+2.. and D */
    c00153b80_run(C00153b80_A + 2u, 0x77u, 0x78u, 0u);
}
