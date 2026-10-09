/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Console identity exports (T94): the published XboxHardwareInfo and
 * XboxKrnlVersion bytes, and the import-slot redirect that keeps their 8-byte
 * structs out of the neighbouring key exports' slots.
 *
 * The one always-reached consumer is the CRT start's kernel-patch gate
 * sub_00381DC7 (read site 0x00381DC7, docs/init-sequence.md section 7). The gate
 * test below replays its exact field reads and comparisons, so a published
 * identity that would route the title into the unhonourable kernel-patch arm
 * fails here rather than in a boot.
 */
#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "kernel_identity.h"
#include "kernel_thunk.h"
#include "recomp_abi.h"

#include <stdio.h>
#include <stdlib.h>

TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp,
    g_fs_base, g_seh_ebp;

static unsigned checks;
static unsigned failures;

#define CHECK(cond)                                                     \
    do {                                                                \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            failures++;                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

/* Published bytes, read back through the same guest accessors the host uses.
 * Mutation kill: swapping Build and Qfe in kernel_identity.c, or publishing a
 * devkit flag, fails the exact-field checks. */
static void test_published_struct_bytes(void)
{
    uint32_t hardware_flags = 0xA5A5A5A5u;
    uint8_t gpu = 0xA5u, mcp = 0xA5u, tail0 = 0xA5u, tail1 = 0xA5u;
    CHECK(kernel_guest_read_u32(KERNEL_THUNK_VA_XBOX_HARDWARE_INFO, &hardware_flags));
    CHECK(kernel_guest_read_u8(KERNEL_THUNK_VA_XBOX_HARDWARE_INFO + 4u, &gpu));
    CHECK(kernel_guest_read_u8(KERNEL_THUNK_VA_XBOX_HARDWARE_INFO + 5u, &mcp));
    CHECK(kernel_guest_read_u8(KERNEL_THUNK_VA_XBOX_HARDWARE_INFO + 6u, &tail0));
    CHECK(kernel_guest_read_u8(KERNEL_THUNK_VA_XBOX_HARDWARE_INFO + 7u, &tail1));
    CHECK(hardware_flags == 0x00000000u); /* retail: bits 0,1,3,9 all clear */
    CHECK(gpu == 0xA2u);
    CHECK(mcp == 0xD1u);
    CHECK(tail0 == 0u && tail1 == 0u);

    uint32_t version_lo = 0xA5A5A5A5u, version_hi = 0xA5A5A5A5u;
    CHECK(kernel_guest_read_u32(KERNEL_THUNK_VA_XBOX_KRNL_VERSION, &version_lo));
    CHECK(kernel_guest_read_u32(KERNEL_THUNK_VA_XBOX_KRNL_VERSION + 4u, &version_hi));
    /* USHORT Major 1, Minor 0, then Build 5849, Qfe 1, little-endian. */
    CHECK(version_lo == 0x00000001u);
    CHECK(version_hi == ((1u << 16) | 5849u));
}

/* Replay of the gate's reads at 0x00381DC7: ecx = Build (16-bit read at +4),
 * eax = Flags bit 1 after the shift. The published Build must dodge every patch
 * constant the gate compares against: 0xF68, 0xFC2, 0xFC7 and 0x12D0..0x12D2
 * (literals from the lifted sub_00381DC7, pinned here), and the devkit bit must
 * be clear so the gate reads a retail console. */
static void test_crt_patch_gate_takes_the_skip_arm(void)
{
    uint32_t flags = 0u;
    uint32_t build_qfe = 0u;
    CHECK(kernel_guest_read_u32(KERNEL_THUNK_VA_XBOX_HARDWARE_INFO, &flags));
    CHECK(kernel_guest_read_u32(KERNEL_THUNK_VA_XBOX_KRNL_VERSION + 4u, &build_qfe));
    const uint32_t build = build_qfe & 0xFFFFu;
    const uint32_t not_devkit = (~(flags >> 1)) & 1u;
    CHECK(not_devkit == 1u);
    CHECK(build != 0xF68u);
    CHECK(build != 0xFC2u);
    CHECK(build != 0xFC7u);
    CHECK((int32_t)build > (int32_t)0x12D2u); /* the jg skip arm, like real 2005 retail */
}

/* The patcher must point 322 and 324 at the annex and every other ordinal at its
 * own slot. Mutation kill: slot_published_va returning the formula VA for 322. */
static void test_patch_table_redirects_the_identity_slots(void)
{
    const guest_region_request request = {
        .bytes = 4096u,
        .alignment = 4096u,
        .protect = PAGE_READWRITE,
        .state = MEM_COMMIT,
    };
    nt_status status = STATUS_SUCCESS;
    const uint32_t table = guest_region_alloc(&request, &status);
    CHECK(table != 0u);
    if (table == 0u) {
        return;
    }
    CHECK(kernel_guest_write_u32(table + 0u, 0x80000000u | 322u));
    CHECK(kernel_guest_write_u32(table + 4u, 0x80000000u | 324u));
    CHECK(kernel_guest_write_u32(table + 8u, 0x80000000u | 156u));
    CHECK(kernel_guest_write_u32(table + 12u, 0x80000000u | 95u));
    CHECK(kernel_guest_write_u32(table + 16u, 0u));
    size_t skipped = 99u;
    const size_t patched = kernel_thunk_patch_table(table, 5u, &skipped);
    CHECK(patched == 4u);
    CHECK(skipped == 0u);
    uint32_t published = 0u;
    CHECK(kernel_guest_read_u32(table + 0u, &published));
    CHECK(published == KERNEL_THUNK_VA_XBOX_HARDWARE_INFO);
    CHECK(kernel_guest_read_u32(table + 4u, &published));
    CHECK(published == KERNEL_THUNK_VA_XBOX_KRNL_VERSION);
    CHECK(kernel_guest_read_u32(table + 8u, &published));
    CHECK(published == KERNEL_THUNK_VA(156u));
    CHECK(kernel_guest_read_u32(table + 12u, &published));
    CHECK(published == KERNEL_THUNK_VA(95u));
    guest_region_free(table);
}

/* The annex must not spill into any dispatchable slot's dword, and the two
 * structs must not overlap each other. */
static void test_annex_layout_is_disjoint(void)
{
    CHECK(KERNEL_THUNK_VA_XBOX_HARDWARE_INFO >=
          KERNEL_THUNK_VA(XBOX_KERNEL_ORDINAL_MAX + 2u));
    CHECK(KERNEL_THUNK_VA_XBOX_HARDWARE_INFO + 8u <=
          KERNEL_THUNK_VA_XBOX_KRNL_VERSION);
    CHECK(KERNEL_THUNK_VA_XBOX_KRNL_VERSION + 8u <=
          KERNEL_THUNK_VA_BASE + KERNEL_THUNK_WINDOW_BYTES);
}

/* An unmapped target refuses the publish without touching the other struct. */
static void test_publish_refuses_unmapped_addresses(void)
{
    CHECK(!kernel_identity_publish(0u, KERNEL_THUNK_VA_XBOX_KRNL_VERSION));
    CHECK(!kernel_identity_publish(KERNEL_THUNK_VA_XBOX_HARDWARE_INFO, 0u));
}

int main(void)
{
    if (!kernel_thunk_map_window()) {
        fprintf(stderr, "could not map the thunk window\n");
        return 1;
    }
    CHECK(kernel_identity_publish(KERNEL_THUNK_VA_XBOX_HARDWARE_INFO,
                                  KERNEL_THUNK_VA_XBOX_KRNL_VERSION));
    test_published_struct_bytes();
    test_crt_patch_gate_takes_the_skip_arm();
    test_patch_table_redirects_the_identity_slots();
    test_annex_layout_is_disjoint();
    test_publish_refuses_unmapped_addresses();
    guest_mem_reset();
    kernel_thunk_unmap_window();
    printf("kernel identity: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
