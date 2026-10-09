/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Console identity DATA exports: XboxHardwareInfo (322) and XboxKrnlVersion (324).
 *
 * ORDINAL NUMBERS, RESOLVED NOT RECALLED. 322 and 324 are read out of
 * tools/kernel_ordinals.py and neither is in its SUSPECT_ON_XDK_5849 list. Both are
 * DATA exports (kernel_arity_oracle.c carries KERNEL_ARITY_ORACLE_DATA rows), so
 * there is no handler and no arity. The guest reads the import slot to get the
 * variable's address, then dereferences it.
 *
 * WHY THESE TWO GET REAL VALUES while the other data exports still read as zero.
 * They are the only two the CRT start reads UNCONDITIONALLY (docs/init-sequence.md
 * section 7): sub_00381DC7 reads XboxKrnlVersion.Build and XboxHardwareInfo.Flags on
 * every boot. Until now both read fabricated zeros through the thunk window, which is
 * an identity no console ever had (kernel build 0). This module publishes one
 * explicit, documented identity instead.
 *
 * WHY THE STRUCTS CANNOT LIVE AT THEIR OWN SLOT VAs. A data ordinal's window slot is
 * 4 bytes, and both structs are 8. Spilling into the neighbour slots would hand
 * nonzero bytes to XboxHDKey (323) and XboxSignatureKey (325), whose own reads today
 * consistently see zero. So the structs live in the window's data annex
 * (KERNEL_THUNK_ANNEX_VA in kernel_thunk.h) and kernel_thunk_patch_table points the
 * two import slots there.
 *
 * THE PUBLISHED IDENTITY, FIELD BY FIELD, WITH THE MEASURED CONSUMERS.
 *
 * XboxKrnlVersion = 1.0.5849.1 (USHORT Major, Minor, Build, Qfe).
 *   - Build is the only field a visible caller reads. sub_00381DC7 (the CRT start
 *     kernel-patch gate, read at 0x00381DC7 every boot) compares Build against
 *     0xF68, 0xFC2, 0xFC7 and the range 0x12D0..0x12D2: a matching build routes into
 *     a kernel-code byte-patch path this host cannot honour, and EVERY build above
 *     0x12D2 (4818) takes the skip arm. 5849 takes the same skip arm a real 2005
 *     retail kernel takes, and it is the XDK generation this repo already resolves
 *     its ordinal names on (tools/kernel_ordinals.py RESOLVED_ON_XDK_5849).
 *   - Major.Minor.Qfe = 1.0.1 is the documented retail shape (INFERRED, no visible
 *     caller reads them; sub_004366B0 passes the pointer to a loaded blob whose
 *     reads are not visible in the lift).
 *
 * XboxHardwareInfo = { Flags 0x00000000, GpuRevision 0xA2, McpRevision 0xD1, 0, 0 }.
 *   - Flags bit 1 (devkit): clear. MEASURED consumers sub_00381DC7 and sub_00415C60
 *     take their retail arms on clear.
 *   - Flags bit 3 (arcade/chihiro): clear. MEASURED consumers sub_0038126B and
 *     sub_00384742 run their retail init on clear.
 *   - Flags bit 9: clear. MEASURED consumer sub_003DD0FD skips an NV2A MMIO write
 *     path only when SET, so clear is the acting retail arm.
 *   - Flags bit 0 (internal USB hub): clear. sub_0046D190 and sub_00470447 branch on
 *     it; the retail value is UNVERIFIED, and clear preserves the arms every boot
 *     has taken under the zero window to date.
 *   - McpRevision: the one MEASURED constraint is sub_0046D14E skipping OHCI init
 *     when it equals 0xA1. 0xD1 is a late retail MCPX revision (INFERRED, plausible
 *     and distinct from the early-silicon 0xA1).
 *   - GpuRevision: no visible reader. 0xA2 is the common retail NV2A revision
 *     (INFERRED).
 */

#ifndef TSFP_XBOX_KERNEL_IDENTITY_H
#define TSFP_XBOX_KERNEL_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>

/* The published values, named so tests pin policy rather than copies of numbers. */
#define KERNEL_IDENTITY_HARDWARE_FLAGS 0x00000000u
#define KERNEL_IDENTITY_GPU_REVISION 0xA2u
#define KERNEL_IDENTITY_MCP_REVISION 0xD1u
#define KERNEL_IDENTITY_KRNL_MAJOR 1u
#define KERNEL_IDENTITY_KRNL_MINOR 0u
#define KERNEL_IDENTITY_KRNL_BUILD 5849u
#define KERNEL_IDENTITY_KRNL_QFE 1u

/* Each struct is exactly 8 guest bytes. */
#define KERNEL_IDENTITY_HARDWARE_INFO_BYTES 8u
#define KERNEL_IDENTITY_KRNL_VERSION_BYTES 8u

/**
 * Write both identity structs into guest-visible memory.
 *
 * hardware_info_va and krnl_version_va are the guest addresses the import slots for
 * ordinals 322 and 324 publish (the window annex in a real boot, any readable guest
 * memory in a test). Returns false and writes nothing further if either address
 * cannot back its 8 bytes.
 */
bool kernel_identity_publish(uint32_t hardware_info_va, uint32_t krnl_version_va);

#endif /* TSFP_XBOX_KERNEL_IDENTITY_H */
