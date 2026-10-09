/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Floating-point state save and restore: ordinals 142 and 139.
 *
 *     NTSTATUS __stdcall KeSaveFloatingPointState(PKFLOATING_SAVE FloatSave);   // 142
 *     VOID     __stdcall KeRestoreFloatingPointState(PKFLOATING_SAVE FloatSave); // 139
 *
 * ARITY AND CONVENTION, MEASURED. ONE stack argument each, callee pops 4 (nxdk
 * KeSaveFloatingPointState@4 and KeRestoreFloatingPointState@4). 142 has 6 sites and 139
 * has 6, every one `lea eax,[ebp-N]; push eax; call dword ptr [slot]` or a literal pointer
 * push, so the count is unambiguous (no register-dispatched site, no import stub). Pairing,
 * save site -> restore site, each pair sharing one save area:
 *
 *     0x00387302 -> 0x0038735B   area [ebp-0x3c]
 *     0x00387EEB -> 0x0038808F   area [ebp-0x2c]
 *     0x003888FB -> 0x00388917   area [ebp-0x38]
 *     0x0038905D -> 0x00389081   area [ebp-0x38]
 *     0x003891D7 -> 0x003891FA   area [ebp-0x38]
 *     0x00406847 -> 0x00406865   DSOUND: the STATIC area 0x00412B34, guarded by a use counter
 *                                at 0x00412460 (save on 0 -> 1, restore on 1 -> 0)
 *
 * Five of the six pairs are in the title's own code (0x0038xxxx), the sixth is in DSOUND. At
 * every site both calls are guarded by a read of `fs:[0x58]` (PrcbData+0x30, the not-active
 * zero a fresh KPCR gives, see kernel_thread.h), so the guest decides whether the kernel is
 * asked at all and nothing here has to. NO SITE READS EAX after 142 (the next instruction is a
 * push or a load of an unrelated field), and the save area is never read back by the guest:
 * it is an opaque cookie the title only hands back to 139.
 *
 * WHAT IS MODELLED: the bracket. A per-thread stack of live save areas, so that the title's
 * own pairing is checked and a mismatch is reported instead of silently accepted. The real
 * kernel snapshots the x87 state into the 0x20-byte area (INFERRED size: 8 dwords, the
 * NT KFLOATING_SAVE, and the smallest local at the first site is exactly 0x20 bytes before the
 * next one) and reloads it on restore.
 *
 * NOT MODELLED, and said plainly: the x87 control word, condition flags and register stack
 * are the lifted runtime's thread-local C variables (`g_fp_control_word`, `g_fp_cmp`,
 * `g_fp_cc`), which src/xbox/ does not link and which the title does not need preserved
 * across these brackets (no measured site changes them between save and restore). So the area
 * is NOT written: the guest never reads it, and fabricating a state image would be an answer
 * to a question nobody asks.
 *
 * REPORTED, NEVER SILENT (each through kernel_hle_log(), each counted):
 *   - a save area that is not readable for 0x20 bytes: REFUSED, STATUS_ACCESS_VIOLATION (the
 *     real kernel would fault on its write), nothing recorded;
 *   - a restore with no save of that area on this thread: ignored;
 *   - a restore of an area that is not the most recent save: honoured (removed) and reported;
 *   - a save of an area that already holds a live save: recorded again and reported;
 *   - more than KERNEL_FPSTATE_MAX_ACTIVE live saves: REFUSED, STATUS_INSUFFICIENT_RESOURCES.
 * INFERRED: that status is what the real export returns when it cannot save (the DDK
 * documents it), no site tests the result, so the choice is only observable here.
 *
 * `KeRestoreFloatingPointState` is VOID, so its handler returns STATUS_SUCCESS by convention
 * (the dispatcher has one return type), as the other VOID handlers do.
 */
#ifndef TSFP_XBOX_KERNEL_FPSTATE_H
#define TSFP_XBOX_KERNEL_FPSTATE_H

#include <stddef.h>
#include <stdint.h>

#define ORD_KeRestoreFloatingPointState 139u
#define ORD_KeSaveFloatingPointState 142u

/** Bytes of the guest save area the handler requires to be readable (INFERRED, see above). */
#define KERNEL_FPSTATE_AREA_BYTES 0x20u

/** Live saves, all threads together, before a save is refused. INFERRED bound: the measured
 * nesting is one per function, and the deepest static call chain through these is small. */
#define KERNEL_FPSTATE_MAX_ACTIVE 256u

/** Bind 142 and 139 and clear every recorded save. Returns how many ordinals bound. */
size_t kernel_fpstate_register(void);

/** Drop every recorded save and the anomaly count. */
void kernel_fpstate_reset(void);

/** Live saves on the calling thread. */
uint32_t kernel_fpstate_depth(void);

/** Live saves on all threads together. */
uint32_t kernel_fpstate_total_depth(void);

/** How many calls were reported as refused or mismatched since the last reset. */
uint32_t kernel_fpstate_anomaly_count(void);

#endif /* TSFP_XBOX_KERNEL_FPSTATE_H */
