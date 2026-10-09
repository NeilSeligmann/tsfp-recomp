/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The seam between lifted guest code and our kernel HLE.
 *
 * HOW A GUEST KERNEL CALL ACTUALLY ARRIVES. MSVC compiles a call to an imported
 * kernel function as `call dword ptr [__imp__Fn@N]`, an indirect call through a
 * slot in the XBE's kernel thunk table. On a real console the loader overwrites
 * each slot with the kernel's own function address before the title runs; in the
 * file each slot holds `0x80000000 | ordinal`. TimeSplitters has 151 slots at
 * guest VA 0x00475780.
 *
 * The lifted code lowers that `call [slot]` to an indirect dispatch on whatever
 * value the slot holds. So the thunk table is the hook, and we are the loader:
 * `kernel_thunk_patch_table` writes a synthetic VA into every slot and
 * `recomp_lookup_kernel` turns that VA back into an ordinal.
 *
 * WHY SYNTHETIC VAs AND NOT THE RAW `0x80000000 | ordinal`. Tempting, because it
 * needs no patching at all, and wrong: the lifted code drops any indirect target
 * that is neither inside an executable section nor at or above 0xFE000000, so
 * every kernel call would be silently skipped with eax = 0. The synthetic window
 * is the one address range it is guaranteed to let through. We use
 * `0xFE000000 + ordinal * 4`, keyed on the ordinal rather than on the slot index,
 * so the mapping is a subtraction and a shift with no side table.
 *
 * THE STACK CONTRACT. At the instant our dispatcher is entered the lifted caller
 * has already pushed the guest return address, so `g_esp` points at it and
 * stdcall argument N sits at `g_esp + 4 + 4*N` -- exactly the layout
 * `kernel_call_frame` describes. Because WE are the callee, WE perform the
 * callee cleanup the real `ret N` would have done. That is why this file needs an
 * ABI table: pop the wrong number of bytes and `esp` desyncs, which does not
 * crash, it just makes every later observation fiction.
 */

#ifndef TSFP_HOST_KERNEL_THUNK_H
#define TSFP_HOST_KERNEL_THUNK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Base of the synthetic address window the lifted code reserves for kernel
 * thunks. Chosen by the lifter, not by us; RECOMP_ICALL_IS_CODE hardcodes it. */
#define KERNEL_THUNK_VA_BASE 0xFE000000u

/** Synthetic VA for an ordinal. */
#define KERNEL_THUNK_VA(ordinal) (KERNEL_THUNK_VA_BASE + (uint32_t)(ordinal) * 4u)

/** Ordinal for a synthetic VA. Only meaningful if kernel_thunk_is_va(). */
#define KERNEL_THUNK_ORDINAL(va) (unsigned)(((va) - KERNEL_THUNK_VA_BASE) / 4u)

/* How much of the window is real memory: one page covers it, since 379 ordinals at
 * 4 bytes is 1516. In the header rather than kernel_thunk.c so that anything else
 * placing a synthetic VA here can CHECK it lands inside the mapping at compile
 * time -- see monitor_thunk.h, which puts a dispatchable slot above the ordinals. */
#define KERNEL_THUNK_WINDOW_BYTES 4096u

/** How many 4-byte slots the mapped window holds. Slots past the ordinals are free. */
#define KERNEL_THUNK_WINDOW_SLOTS (KERNEL_THUNK_WINDOW_BYTES / 4u)

/*
 * The window's DATA ANNEX. A data ordinal's own slot is 4 bytes, but the guest reads
 * a data export THROUGH the published pointer, and some variables are wider than one
 * dword: spilling past the slot would hand nonzero bytes to the NEXT ordinal's
 * variable (XboxHardwareInfo would leak into XboxHDKey, XboxKrnlVersion into
 * XboxSignatureKey). Wide variables therefore live in the upper half of the same
 * mapped page, far above both the 371 ordinal slots and the monitor slot at 379,
 * and kernel_thunk_patch_table points their import slots here instead.
 */
#define KERNEL_THUNK_ANNEX_OFFSET 2048u
#define KERNEL_THUNK_ANNEX_VA(offset) \
    (KERNEL_THUNK_VA_BASE + KERNEL_THUNK_ANNEX_OFFSET + (uint32_t)(offset))

/* XboxHardwareInfo (322): ULONG Flags, UCHAR GpuRevision, UCHAR McpRevision,
 * UCHAR Unknown[2]. 8 bytes. */
#define KERNEL_THUNK_VA_XBOX_HARDWARE_INFO KERNEL_THUNK_ANNEX_VA(0u)

/* XboxKrnlVersion (324): USHORT Major, Minor, Build, Qfe. 8 bytes. */
#define KERNEL_THUNK_VA_XBOX_KRNL_VERSION KERNEL_THUNK_ANNEX_VA(8u)

/* Total annex bytes in use, so the window-fit check tracks additions. */
#define KERNEL_THUNK_VA_DISK_MODEL KERNEL_THUNK_ANNEX_VA(16u)
#define KERNEL_THUNK_VA_DISK_SERIAL KERNEL_THUNK_ANNEX_VA(24u)
#define KERNEL_THUNK_VA_DISK_STORAGE KERNEL_THUNK_ANNEX_VA(32u)
/* Actual EEPROM/HD source keys321/323: sixteen bytes each, after disk storage. */
#define KERNEL_THUNK_VA_EEPROM_KEY KERNEL_THUNK_ANNEX_VA(96u)
#define KERNEL_THUNK_VA_HD_KEY KERNEL_THUNK_ANNEX_VA(112u)
#define KERNEL_THUNK_ANNEX_BYTES_USED 128u

/* Startup-only binding. Call true only after actual device bytes and both
 * counted descriptors were completely published. Unavailable DATA imports41/42
 * resolve to NULL, never a manufactured empty STRING. Bind before patch_table
 * and guest threads; unmap clears readiness. No rebinding while guests run. */
void kernel_thunk_set_disk_identity_available(bool available);
bool kernel_thunk_disk_identity_available(void);
/* Startup-only publication of both authenticated source keys. Absent keys
 * patch321/323 to NULL; unmap clears readiness. No caller-supplied success flag. */
bool kernel_thunk_publish_eeprom_keys(void);
bool kernel_thunk_eeprom_keys_available(void);

/*
 * THE ORDERED TRACE IS NOT HERE ANY MORE. It is `thunk_trace.h`, shared with the
 * address-keyed XDK boundary in `xdk_thunk.h`, because one ordered sequence covering
 * both is recoverable and two separate ones are not. `thunk_trace_entries()` replaces
 * `kernel_thunk_trace()`, and each entry now says which KIND of boundary it crossed.
 */

/**
 * Map the synthetic thunk window as readable, zeroed guest memory.
 *
 * WHY A FUNCTION WINDOW NEEDS TO BE READABLE. A kernel export is either a
 * function or a variable, and the thunk slot is used differently for each:
 * `call dword ptr [__imp__Fn]` CALLS through the slot, while `__imp__Var` holds
 * the ADDRESS of a kernel variable that the guest then dereferences. Patch every
 * slot to a synthetic VA and the function case works and the variable case
 * faults; this was found the hard way on `LaunchDataPage` (ordinal 164), whose
 * fourteen references are all `eax = MEM32(slot)` and none a call.
 *
 * Making the window itself real memory serves both without needing to know which
 * is which, and gives the RIGHT ANSWER for the variable case rather than merely
 * not crashing: the synthetic VA is the variable's address, the page is zero, so
 * the guest reads zero -- which is what `LaunchDataPage` genuinely is unless the
 * dashboard handed the title launch data.
 *
 * That matters because classifying the slots is NOT reliable: an ordinal reached
 * via `mov esi, [__imp__Fn]` followed by `call esi` reads exactly like a
 * variable. Measurement flagged `NtQueryVirtualMemory` as data on that basis and
 * it is plainly a function. Mapping the window removes the need to decide.
 *
 * Returns false if the window cannot be mapped, which is not fatal: the function
 * case still works and the variable case faults with a diagnosis.
 */
bool kernel_thunk_map_window(void);

/** Release the mapping made by kernel_thunk_map_window. */
void kernel_thunk_unmap_window(void);

/**
 * Overwrite every kernel thunk slot with its synthetic VA.
 *
 * `slots` is the guest address of the table and `count` its length; both come
 * from the parsed XBE, never from a constant, because every title puts the table
 * somewhere different. Returns how many slots were patched, or 0 if the table is
 * not readable. Slots whose contents are not `0x80000000 | ordinal` are left
 * alone and counted in `*skipped`, because overwriting something we have not
 * understood is how a wrong-but-plausible run starts.
 */
size_t kernel_thunk_patch_table(uint32_t slots, size_t count, size_t *skipped);

/**
 * True when every reference to this ordinal's thunk slot in the lifted code is a
 * read rather than a call, i.e. it looks like a kernel variable.
 *
 * Diagnostic only, and deliberately not acted on: the measurement cannot tell a
 * variable from a function pointer loaded into a register before being called.
 * It is worth reporting at a stop, because a *call* to an ordinal that measured
 * this way means one of the two readings is wrong.
 */
bool kernel_thunk_measured_as_data(unsigned ordinal);

/** True when `va` falls in the synthetic kernel window. */
bool kernel_thunk_is_va(uint32_t va);

/**
 * Stop the run on the first ordinal with no implementation.
 *
 * On by default, and that is the point of a Phase 1.4 run: the first missing
 * ordinal is the answer. Turning it off lets a run continue through stubs to
 * collect a longer ordered trace, at the cost of every observation after the
 * first stub being made on a guest that was lied to.
 */
void kernel_thunk_set_stop_on_missing(bool stop);

#endif /* TSFP_HOST_KERNEL_THUNK_H */
