/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The memory-management and heap group of the Xbox kernel HLE: the Mm* contiguous
 * allocators and the Nt*VirtualMemory calls.
 *
 * ORDINALS. Every number below was checked against tools/kernel_ordinals.py, which
 * is the single source of truth. They are not in the order a reader might guess --
 * MmQueryAllocationSize is 180, not 178 -- so take them from here rather than from
 * memory.
 *
 * NOT IMPLEMENTED HERE, AND WHY: the Rtl heap functions. There are none. The Xbox
 * kernel export table contains no RtlCreateHeap, RtlAllocateHeap, RtlFreeHeap,
 * RtlSizeHeap, RtlReAllocateHeap or RtlDestroyHeap at any ordinal; grep the table
 * for "Heap" and it returns nothing. Those live in the title's statically linked
 * XAPI library, so they are recompiled guest code rather than an HLE boundary. The
 * heap engine they will eventually bind to is implemented and tested in
 * guest_mem.h, reachable by C API but deliberately registered against no ordinal.
 *
 * ALSO NOT MODELLED, beyond the list in guest_mem.h:
 *   - MmIsAddressValid (174), NtProtectVirtualMemory (204) and MmMapIoSpace (177) are in
 *     this area but out of this group's scope, and remain stubs. (MmQueryAddressProtect
 *     179 and MmLockUnlockBufferPages 175 are implemented below.)
 *   - The `void`-returning exports still return a value through the dispatcher,
 *     because kernel_fn has one return type for all 371 ordinals. They return
 *     STATUS_SUCCESS by convention; a recompiled caller of a void export must
 *     ignore EAX, which it would anyway.
 */

#ifndef TSFP_XBOX_KERNEL_MEMORY_H
#define TSFP_XBOX_KERNEL_MEMORY_H

#include <stddef.h>
#include <stdint.h>

/* Verified against tools/kernel_ordinals.py. */
#define ORD_MmAllocateContiguousMemory 165u
#define ORD_MmAllocateContiguousMemoryEx 166u
#define ORD_MmCreateKernelStack 169u
#define ORD_MmDeleteKernelStack 170u
#define ORD_MmFreeContiguousMemory 171u
#define ORD_MmGetPhysicalAddress 173u
#define ORD_MmLockUnlockBufferPages 175u
#define ORD_MmLockUnlockPhysicalPage 176u
#define ORD_MmPersistContiguousMemory 178u
#define ORD_MmQueryAddressProtect 179u
#define ORD_MmQueryAllocationSize 180u
#define ORD_MmQueryStatistics 181u
#define GUEST_MM_STATISTICS_SIZE 36u
#define ORD_MmSetAddressProtect 182u
#define ORD_NtAllocateVirtualMemory 184u
#define ORD_NtFreeVirtualMemory 199u
#define ORD_NtQueryVirtualMemory 217u

/** Size of the guest MEMORY_BASIC_INFORMATION that NtQueryVirtualMemory fills. */
#define GUEST_MEMORY_BASIC_INFORMATION_SIZE 28u

/**
 * Register this group's handlers with the HLE dispatcher.
 *
 * Returns how many ordinals were registered, which must equal
 * kernel_memory_ordinal_count(). A short count means an ordinal in this file is
 * absent from the generated table, i.e. the two have drifted apart, and that is
 * worth failing a test over rather than discovering later as a stub report.
 *
 * Call after kernel_hle_init(), which resets every entry back to a stub.
 */
size_t kernel_memory_register(void);

/** The ordinals this module implements, for tests and for the backlog report. */
const unsigned *kernel_memory_ordinals(size_t *count);

/** How many ordinals this module implements. */
size_t kernel_memory_ordinal_count(void);

/*
 * KERNEL STACKS (169, 170). The pair is one contract, and the guest's single static
 * user is the lazily created 24 KiB stack held in the dword at 0x7717DC.
 *
 * MmCreateKernelStack(NumberOfBytes, DebuggerThread) returns the TOP of the stack,
 * that is one past the end of a real writable region of NumberOfBytes bytes, because
 * the guest switches esp to the returned value and pushes downward from it. It
 * returns 0 when the allocation fails, which the measured caller maps to
 * E_OUTOFMEMORY. DebuggerThread is not modelled.
 *
 * MmDeleteKernelStack(StackBase, StackLimit) takes the value 169 returned and
 * StackBase - NumberOfBytes. It frees only a tracked region that starts exactly at
 * StackLimit and spans exactly StackBase - StackLimit. Anything else is logged and
 * left alone, because the model never frees memory it cannot prove it allocated.
 * The export returns void, so the handler returns STATUS_SUCCESS by convention.
 */

/*
 * BUFFER PAGE LOCKS (175).
 *
 *     VOID __stdcall MmLockUnlockBufferPages(PVOID BaseAddress, ULONG NumberOfBytes,
 *                                            BOOLEAN UnlockPages);
 *
 * ARITY AND CONVENTION, MEASURED. THREE stack arguments, callee pops 12 (`ret 12`, nxdk
 * MmLockUnlockBufferPages@12). The measured table says 1 over 14 sites, non-unanimous, and is
 * WRONG: the bracket loses pushes that sit above an intervening call. The count is pinned by
 * the hand row in src/host/kernel_thunk.c. At all 14 sites the push order is UnlockPages
 * first, NumberOfBytes second, BaseAddress last, so right to left as __stdcall requires:
 *
 *     DSOUND 0x0040B715  push 1; push [esi+0xc]; push [edi]        (unlock the buffer's pages)
 *     DSOUND 0x0040F489  push 1; push eax; push ecx                (unlock, in teardown)
 *     DSOUND 0x0040F638  push 0; push [ebp+8]; push eax; call edi  (lock a fresh allocation)
 *     XNET   0x0043A765  push 1; push eax; call 0x439cff; push eax (two pushes held across a
 *                                                                   call that pops nothing)
 *     XPP    0x0046D491  push 0; push edi; push esi, esi = MmAllocateContiguousMemory result
 *
 * UnlockPages is the literal 0 (lock) or 1 (unlock) at EVERY site, and a lock is always paired
 * with the buffer it was just handed by MmAllocateContiguousMemory, so 0 = lock and 1 = unlock
 * is MEASURED from the pairing with the allocator and the matching free paths. No caller reads
 * EAX afterwards (VOID, nxdk).
 *
 * WHAT IT MODELS. A per-page lock depth, keyed by the page containing each byte of
 * [BaseAddress, BaseAddress + NumberOfBytes): lock adds one to every page the range touches,
 * unlock subtracts one. INFERRED: the real kernel pins the pages for DMA (the title locks
 * exactly the buffers it then hands to MMIO-programmed devices: audio and network). Nothing
 * on a flat host mapping can fault, so the lock has no effect on the guest's memory itself
 * and what is recorded is the guest's own bookkeeping, so that a test or a later change can
 * see which pages the title believes are pinned and whether its locks balance.
 *
 * REPORTED, NEVER SILENT:
 *   - UnlockPages other than 0 or 1: REFUSED (unmeasured mode), no state change.
 *   - A range that wraps the 4 GB space: REFUSED, no state change.
 *   - A lock whose range is not wholly inside one tracked region, or whose region is only
 *     RESERVEd: reported, and the lock IS recorded, so the title's later unlock balances.
 *   - An unlock of a page with no lock: reported, the depth stays at 0.
 * A zero NumberOfBytes touches no page and is silent (0x0040C2D5 computes its length as
 * `pages << 12`, and 0 is an ordinary value there).
 *
 * NOT MODELLED: freeing a region does not release its locks (a title that frees a locked
 * buffer leaves a stale depth, which is its own bug), and nothing is flushed or written back.
 */

/*
 * PHYSICAL PAGE UNLOCK (176).
 *
 *     VOID __stdcall MmLockUnlockPhysicalPage(ULONG_PTR PhysicalAddress, BOOLEAN UnlockPage);
 *
 * ARITY AND ROLES, MEASURED. TWO stack arguments, callee pops 8 (nxdk MmLockUnlockPhysicalPage@8,
 * measured {176,2,2,1}). All FOUR sites are in the XPP USB driver's transfer-descriptor
 * completion and every one is `push 1; push <physical>; call`: 0x00473E5F (`[edi+4]`),
 * 0x00473E78 (`[edi+0xC]`, only when it is on a different page than `[edi+4]`) and the
 * register-dispatched twins 0x00474240 and 0x00474253 (the slot is loaded into `edi` at
 * 0x00474233). UnlockPage is therefore the literal 1 everywhere. The matching submit path is
 * measured too (0x00473F81): MmLockUnlockBufferPages(va, len, 0), then MmGetPhysicalAddress(va)
 * stored at TD+4 and MmGetPhysicalAddress(va + len - 1) stored at TD+0xC. So the physical
 * address the title passes is a value THIS module's synthetic physical map handed out, and
 * releasing it must release the page lock the virtual lock took.
 *
 * WHAT IT MODELS. The physical address is translated back to its guest address
 * (`guest_virtual_from_physical`) and ONE lock is removed from the page containing it, in the
 * same table 175 uses, so the two doors balance each other. INFERRED, as for 175: the lock is
 * the title's own bookkeeping with no effect on a flat host mapping.
 *
 * REPORTED, NEVER SILENT:
 *   - UnlockPage other than 1, INCLUDING 0 (lock): REFUSED, no state change, counted. Lock mode
 *     is not measured at any site, so nothing here invents it.
 *   - A physical address no live region owns (0, never handed out, or freed): REFUSED, no
 *     state change, counted.
 *   - An unlock of a page with no lock: reported, the depth stays at 0 (not a refusal).
 */

/** How many 176 calls were refused (unmeasured UnlockPage, or an unmappable physical address). */
uint32_t kernel_memory_physical_refused_count(void);

/** How many distinct pages currently have a lock depth above zero. */
uint32_t kernel_memory_locked_page_total(void);

/** The lock depth of the page containing `address`, 0 when it is not locked. */
uint32_t kernel_memory_page_lock_count(uint32_t address);

#endif /* TSFP_XBOX_KERNEL_MEMORY_H */
