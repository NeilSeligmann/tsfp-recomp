/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Thread creation, and the thread model that makes the guest's real code run.
 *
 * `PsCreateSystemThreadEx` (ordinal 255) is the FIRST kernel call the guest makes,
 * measured by running it (`context.md` §6q). It is also a hard blocker that cannot
 * be stubbed, and that was established rather than assumed: returning
 * STATUS_SUCCESS without writing the out-parameter reaches three ordinals and then
 * faults dereferencing `0x20`, and returning a failure status faults at `0x24`
 * immediately. The guest dereferences what we hand back either way.
 *
 * Those two addresses are not arbitrary and are worth reading twice: they are
 * `fs:[0x20]` and `fs:[0x24]` with a zero `fs` base. That earlier, independent
 * measurement is the corroboration for the KPCR layout recorded further down.
 *
 * ARITY IS MEASURED, NOT RECALLED. `tools/lift/callsites.py` reads the guest's own
 * argument pushes at each call site and reports **10** for this ordinal, which
 * independently agrees with the published signature. `context.md` §6m records four
 * ordinal numbers being misremembered in a single earlier task, one of them pointing
 * at `NtReadFile`, so nothing here is taken from memory.
 *
 *     NTSTATUS __stdcall PsCreateSystemThreadEx(
 *         OUT PHANDLE ThreadHandle,        // 0
 *         IN  SIZE_T  ThreadExtensionSize, // 1
 *         IN  SIZE_T  KernelStackSize,     // 2
 *         IN  SIZE_T  TlsDataSize,         // 3
 *         OUT PHANDLE ThreadId,            // 4, optional -- may be NULL
 *         IN  PVOID   StartRoutine,        // 5
 *         IN  PVOID   StartContext,        // 6
 *         IN  BOOLEAN CreateSuspended,     // 7
 *         IN  BOOLEAN DebuggerThread,      // 8
 *         IN  PVOID   SystemRoutine);      // 9
 *
 * ---------------------------------------------------------------------------
 * HOW A GUEST THREAD IS ENTERED. DERIVED FROM THE GUEST, NOT RECALLED.
 * ---------------------------------------------------------------------------
 *
 * The entry convention was read out of the guest's own lifted code, by following
 * the chain from the entry point to the thunk slot and then into the routine the
 * guest hands us. Four links, every one checkable:
 *
 *  1. The XBE entry point pushes six arguments and calls one function, with the
 *     guest VA `0x003801D9` third from the left. Six arguments with the start
 *     address third is the XAPI `CreateThread` shape
 *     (Attributes, StackSize, StartAddress, Parameter, CreationFlags, ThreadId).
 *
 *  2. That function pushes TEN arguments and makes an indirect call through the
 *     thunk slot at guest VA `0x475894`. The thunk table is at `0x475780`, so that
 *     is slot 69, and slot 69 of this image holds `0x800000FF` -- **ordinal 255,
 *     `PsCreateSystemThreadEx`**. Read out of the XBE file, not assumed. Ten
 *     arguments independently re-confirms the measured arity above.
 *
 *     Its first pushed argument -- therefore the LAST one, `SystemRoutine` -- is
 *     the constant `0x0037FE1D`. Its `CreateSuspended` is the caller's creation
 *     flags shifted right by two and masked to one bit, i.e. `CREATE_SUSPENDED`.
 *     So the guest supplies its own system routine, and the kernel's job is to
 *     enter *that*, not the start routine.
 *
 *  3. `0x0037FE1D` -- XAPI's thread startup shim -- establishes a frame through
 *     the MSVC `__SEH_prolog` helper and then does exactly:
 *
 *         push [ebp+0x0C]        ; its own argument 1
 *         call [ebp+0x08]        ; its own argument 0
 *
 *     That is `SystemRoutine(StartRoutine, StartContext)` in __stdcall order,
 *     calling `StartRoutine(StartContext)`. Nothing is inferred from a name: the
 *     routine calls its first argument and passes its second.
 *
 *  4. It never returns. It finishes by pushing the start routine's `eax` and
 *     calling through the thunk slot at `0x475890` -- slot 68, which this image
 *     holds as `0x80000102`, **ordinal 258, `PsTerminateSystemThread`** -- and the
 *     byte after that is an `int3`. So the exit path is an ordinal, not a return,
 *     which is why `PsTerminateSystemThread` must actually end the thread here.
 *
 * THEREFORE the initial guest stack for a new thread is, at the instant the entry
 * routine's first instruction runs:
 *
 *     esp+0x00   return address   (a sentinel: nothing is expected to return here)
 *     esp+0x04   StartRoutine     (the entry routine's argument 0)
 *     esp+0x08   StartContext     (the entry routine's argument 1)
 *
 * and when `SystemRoutine` is zero -- which this title never does, but the ordinal
 * permits -- we fall back to entering `StartRoutine` directly with `StartContext`
 * as its single argument. That fallback is what the shim itself does, so it is the
 * same convention one link further in rather than an invention.
 *
 * ---------------------------------------------------------------------------
 * THE PER-THREAD KPCR, AND WHY IT IS HERE
 * ---------------------------------------------------------------------------
 *
 * The startup shim's first act after its prologue is to read `fs:[0x28]` and then
 * dereference what it finds. A thread entered with a zero `fs` base faults at
 * guest address `0x28` about ten instructions in, so "allocate a stack and jump"
 * is not a runnable thread. The `fs` base is part of the register state the lifted
 * code models (`g_fs_base`, thread-local like every other register), so setting it
 * up is thread creation's job.
 *
 * WHAT IS MODELLED IS EXACTLY WHAT THE GUEST TOUCHES, MEASURED. Across all 2.56 M
 * lines of lifted code there are only five distinct `fs:` accesses:
 *
 *     fs:[0x00]  114 sites, read and written   SEH chain head (NtTib.ExceptionList)
 *     fs:[0x20]   22 sites, read               Prcb pointer. 19 then read
 *                                              Prcb+0x250, 2 read Prcb+0x24C and
 *                                              1 WRITES Prcb+0x1C; see below
 *     fs:[0x24]    6 sites, read as a BYTE     Irql
 *     fs:[0x28]    6 sites, read               Prcb.CurrentThread
 *     fs:[0x58]    6 sites, read, compared to 0
 *
 * Those offsets, and the byte-sized access at `0x24`, are the NT/Xbox KPCR layout:
 * a 0x1C-byte NtTib, SelfPcr at 0x1C, Prcb at 0x20, Irql at 0x24, PrcbData from
 * 0x28 -- so `fs:[0x28]` is `PrcbData.CurrentThread` and `fs:[0x58]` is
 * `PrcbData+0x30`. The agreement between four independently-measured offsets, the
 * access width at `0x24`, and the two addresses an earlier task recorded faulting
 * at is the evidence. It is not taken from a header nobody can check.
 *
 * So each thread gets a zeroed control page with `Prcb` pointing at its own
 * `PrcbData`, `Irql` at PASSIVE, the SEH chain head set to the NT terminator
 * `0xFFFFFFFF`, and `CurrentThread` pointing at a KTHREAD block whose `TlsData`
 * field is a guest allocation of the `TlsDataSize` the guest asked for. A zeroed
 * `PrcbData` answers 0 to all 6 `fs:[0x58]` tests, which is the not-active branch
 * -- the right answer for a title that is booting.
 *
 * `Prcb+0x250` is the ONE exception, and the rest of this comment is why.
 *
 * ---------------------------------------------------------------------------
 * THE OBJECT AT `Prcb+0x250`, AND WHY IT MUST NOT BE NULL HERE
 * ---------------------------------------------------------------------------
 *
 * MEASURED, by decoding every one of the 22 lifted `fs:[0x20]` sites against the
 * real bytes of the image. An earlier note in this file said all 22 read `+0x250`;
 * that was wrong and is corrected here. The true tally is:
 *
 *     Prcb+0x250   19 sites, READ ONLY, always compared against 0
 *     Prcb+0x24C    2 sites, READ ONLY, a different object (see below)
 *     Prcb+0x1C     1 site, WRITTEN (a zero), at guest VA 0x0044127D
 *
 * and the 22 lifted sites are only 14 distinct guest instructions: the lifter
 * recovered five overlapping function entries that share one tail block, so two
 * real instructions appear ten times. So the Prcb must be at least `0x254` bytes
 * and byte `0x1C` of it must be writable.
 *
 * WHY THE FIELD DECIDES WHETHER THE TITLE RUNS AT ALL. The thread startup shim at
 * `0x0037FE1D` enters `0x003801D9`, whose first act is `call 0x381DC7`. Every path
 * through `sub_00381DC7` falls into its unconditional tail at `0x00381E76`, which
 * is exactly:
 *
 *     mov  eax, fs:[0x20]
 *     cmp  dword ptr [eax+0x250], 0
 *     jne  <return>                  ; non-null: the function simply returns
 *     jmp  0x00381D63                ; null:     the GDT path
 *
 * and `0x00381D63` is the only code in the entire 2.56 M lifted lines that reads
 * an absolute address in the kernel's window: `mov eax, [0x8001003C]`. It walks the
 * real Xbox kernel's PE headers at `0x80010000`, finds the discardable `"INIT"`
 * section in the section table, builds a code-segment descriptor from it
 * (`or ecx, 0xC09B00`) and hands it to `sub_00384834`, which issues `sgdt`, `cli`,
 * two `xchg`es into the live GDT, `sti` and an `ljmp` to reload CS.
 *
 * NONE OF THAT CAN WORK HERE AND NONE OF IT SHOULD. We have no kernel image, no
 * GDT and no segmentation; `sub_00384834` lifts to `RECOMP_UNIMPL("sgdt")` and then
 * stores through an uninitialised stack slot. `0x00381D63` has exactly ONE
 * reference in the image -- that gated `jmp` -- and `sub_00384834` has exactly two
 * callers, the second of which is gated on two globals that only `0x00381D63`
 * writes. So a non-null `Prcb+0x250` does not merely move the fault: it makes the
 * entire GDT path, both functions and the `sgdt` stub, unreachable code.
 *
 * THIS IS A DELIBERATE DIVERGENCE FROM THE CONSOLE, RECORDED AS ONE. A retail
 * console almost certainly has 0 here and really does shrink its CS limit. We
 * answer non-null because the alternative is emulating the GDT, and the field's
 * own semantics make the lie a cheap one: see what the guest does with it.
 *
 * WHAT THE OBJECT IS, MEASURED. Every offset the guest touches off it, and there
 * are only three:
 *
 *     +0x14   READ then CALLED, 8 distinct sites (16 lifted occurrences, because 5
 *             tail-jump aliases re-emit the same two calls). `__stdcall fn(code, ptr)`
 *             with `code` one of 2, 0xA, 0xB, 0xC and `ptr` either 0 or a stack
 *             buffer. CORRECTED: this said 13, which no metric reproduces. The
 *             per-site table, and the stack-balance proof of the arity, are in
 *             `src/host/monitor_thunk.h`.
 *     +0x20   READ as a pointer, 1 site. If non-null the title writes 0x24 bytes
 *             through it, ending `[p+0x20] = 0xABCDEF00` -- a handshake.
 *     +0x24   READ as a pointer, 1 site. If non-null, `[p] = 1` and `[p+4] = delta`.
 *
 * NOTHING IS EVER WRITTEN TO THE OBJECT ITSELF, at any offset, anywhere in the
 * image. A notification callback plus two optional shared blocks, all read-only to
 * the guest, is an instrumentation or debug-monitor interface -- present on a
 * devkit, absent on retail. We call it the Prcb MONITOR block, which names the
 * measured behaviour rather than a symbol nobody can check.
 *
 * So the useful consequence: a ZEROED monitor block is self-consistent. Both
 * second-level pointers read 0, so both of those branches skip and nothing is
 * written anywhere. The highest offset touched is `0x24`, so the block must be at
 * least `0x28` bytes.
 *
 * ITS OWN ALLOCATION, OVER-SIZED, NOT AN OVERLAY. Upstream's maintainer rejected
 * an equivalent shortcut -- overlaying `KTHREAD` on the TIB -- because the TIB is
 * 0x40 bytes with TLS data behind it, so a guest write through a KTHREAD offset
 * would corrupt TLS (`docs/upstream-issues-review-2026-10-01.md` §2). The guest
 * does not write this block today, but `+0x14` is a *called* function pointer and a
 * callee we eventually supply will, so the block gets a whole page of its own with
 * nothing else in it. 0x1000 against a measured requirement of 0x28 is 146x slack.
 *
 * PER THREAD, and that is a choice rather than a requirement. The guest never
 * writes the block, so one shared block would be observationally identical to the
 * guest as the code stands today. It is per-thread anyway because the Prcb that
 * points at it is per-thread, so one block per Prcb keeps the structure
 * one-to-one, and because it is then allocated and released with its thread
 * instead of living for the life of the process.
 *
 * `Prcb+0x24C` IS LEFT AT ZERO, on the record. It is a different object: its 2
 * sites read `[obj+0]` as a vtable and `call [vtable+4]`, and check `[obj+4]`
 * against the signature `0x58424436`. Both sites skip on zero, neither is on the
 * boot path, and inventing a vtable to satisfy a check nothing has reached yet
 * would be fidelity bought with guesses.
 *
 * THE COST OF THE SKIP, STATED UP FRONT. Answering non-null arms the 8 `+0x14`
 * call sites that a retail console would never reach. With a zeroed block the
 * target is 0, `RECOMP_ICALL_IS_CODE` rejects it and the run stops at
 * `recomp_icall_unresolved_trap` -- loudly, at a named site, which is a strictly
 * better stop than a SIGSEGV on a kernel address. Whether the boot path reaches
 * one of those sites is a measurement, not a guess, and it is recorded in
 * `context.md` §6q rather than predicted here.
 *
 * THAT MEASUREMENT CAME BACK: IT DOES REACH ONE. Every storage-failure arm funnels
 * into 0x0037CBAE, so the cost above was paid rather than merely risked. The notify
 * now has a callable no-op, injected through `kernel_thread_set_monitor_callback`
 * below and implemented in `src/host/monitor_thunk.h`. A host that does not inject
 * one still gets the NULL and the named stop, which is why the default is 0.
 *
 * NOT MODELLED, on the record: no scheduling, no affinity, no APCs, and the KTHREAD is a
 * bounded mapped view: TLS data plus the measured exit-query signal/status fields.
 * Other KTHREAD offsets remain zero and have no general kernel-layout claim. A `CreateSuspended`
 * thread is recorded with suspend count 1 and NOT started until NtResumeThread (224) takes
 * the count to 0. NtSuspendThread (231) is honoured only for a thread that has not started,
 * because a running host thread has no safe point to be paused: it is REFUSED and counted.
 * KeSetBasePriorityThread (143) and KeSetDisableBoostThread (144) are RECORDED and have no
 * effect on host scheduling, announced on every call.
 *
 * ORDINALS 231 224 143 144, STDCALL, ARITIES 2 2 2 2 (see the ARITY-OK notes in the .c).
 * Each has ONE call site, in the XAPI wrappers: 0x37FCC6 (231), 0x37FCEC (224), 0x37FC56
 * (143) and 0x37FC9C (144), MEASURED by decoding the XBE. The object argument of 143/144 comes from ObReferenceObjectByHandle, which
 * this host answers with the HANDLE, so all four look the handle up in this table.
 *
 * ---------------------------------------------------------------------------
 * ONE GUEST THREAD PER HOST THREAD, AND NO SCHEDULER
 * ---------------------------------------------------------------------------
 *
 * The lifted code keeps the whole guest register file in `__thread` globals, so a
 * host thread already has its own guest registers. Mapping one guest thread onto
 * one host thread is therefore the grain the lifter was built with, and the host
 * kernel does the scheduling. There is deliberately no scheduler here.
 *
 * The actual run is injected rather than called: `kernel_thread_set_host_ops`
 * takes the two operations only the host can perform -- resolving a guest VA to
 * lifted code, and installing guest register state before entering it. With no
 * ops registered this module behaves exactly as it did before a thread model
 * existed: it records the request and says, loudly, that nothing was started.
 * That is what lets this file and its tests build and run in a fresh clone, where
 * there is no lifted code at all.
 */

#ifndef TSFP_XBOX_KERNEL_THREAD_H
#define TSFP_XBOX_KERNEL_THREAD_H

#include <stdbool.h>
#include <stdint.h>

/** How many guest threads we will track before refusing to create more. */
#define KERNEL_THREAD_MAX 64

/* --- stack sizing ---------------------------------------------------------
 *
 * The guest's own request is honoured. This title asks for `PeStackCommit` from
 * its XBE header, which is 0x10000, so the floor below is deliberately equal to
 * it: the common case needs no fudging and a test can tell the two apart only by
 * a value that is NOT the floor. The cap exists because `KernelStackSize` is a
 * guest-supplied 32-bit number and a garbage one must fail to allocate rather
 * than succeed at 3 GB.
 */

/** Smallest stack any guest thread gets, and what a request of 0 becomes. */
#define KERNEL_THREAD_STACK_MIN 0x00010000u

/** Largest stack an honoured request can reach. */
#define KERNEL_THREAD_STACK_MAX 0x00800000u

/**
 * Unmapped band placed on BOTH sides of every guest stack.
 *
 * 64 KiB, not one page, and that is the point. A single 4 KiB guard is jumpable:
 * guest code that does `sub esp, 0x8000` and then writes steps clean over it and
 * corrupts whatever is below, which is the silent-corruption outcome a guard
 * exists to prevent. These bands are `mprotect`ed PROT_NONE, so an overflow is a
 * SIGSEGV at a known address that the run's fault handler reports, rather than a
 * neighbour's memory changing for no visible reason.
 */
#define KERNEL_THREAD_STACK_GUARD 0x00010000u

/* --- the per-thread control block ----------------------------------------
 *
 * Offsets MEASURED from the guest's own `fs:` accesses; see the header comment.
 */

/** Size of the control page holding the KPCR and the KTHREAD stub. */
#define KERNEL_THREAD_CONTROL_BYTES 0x1000u

/** Where the KTHREAD stub sits inside the control page. Clear of Prcb+0x250. */
#define KERNEL_THREAD_KTHREAD_OFFSET 0x0800u

/** KPCR fields the guest actually touches. */
#define KERNEL_PCR_EXCEPTION_LIST 0x00u
#define KERNEL_PCR_PRCB 0x20u
/* NtTib.StackBase's slot, which this kernel uses as the END of the thread's TLS block:
 * the title's `XapiSetLastError` reads `[fs:[4] + tlsindex*4]` with a NEGATIVE index (-5
 * for the 0x14-byte TLS block), so fs:[4] must be `TlsData + TlsDataSize`. MEASURED with
 * gdb: left at 0, the first SetLastError the boot makes faults at 0xFFFFFFEC. That fault
 * was misattributed to the DirectSound NULL device for a while. */
#define KERNEL_PCR_TLS_END 0x04u
#define KERNEL_PCR_IRQL 0x24u
#define KERNEL_PCR_PRCB_DATA 0x28u

/** KTHREAD field the guest actually touches: the XAPI TLS block. */
#define KERNEL_KTHREAD_TLS_DATA 0x28u
/* Measured GetExitCodeThread (0x37FD83): BYTE+4 then DWORD+0x120.
 * This bounded mapped view is not a general ETHREAD layout. */
#define KERNEL_THREAD_BODY_SIGNAL 0x04u
#define KERNEL_THREAD_BODY_EXIT_STATUS 0x120u

/* --- the Prcb fields past 0x240, and the monitor block --------------------
 *
 * All three offsets are measured from the guest's own code; the header comment has
 * the decode and the evidence. These are offsets within PrcbData, i.e. relative to
 * whatever `fs:[0x20]` holds -- NOT relative to the control page.
 */

/** The one Prcb field the guest WRITES (a zero, at guest VA 0x0044127D). */
#define KERNEL_PRCB_ZEROED_FIELD 0x1Cu

/** Read at 2 sites as a vtable'd object. Deliberately left null; see the header. */
#define KERNEL_PRCB_VTABLE_OBJECT 0x24Cu

/** Read at 19 sites and compared to 0. Non-null here, which skips the GDT path. */
#define KERNEL_PRCB_MONITOR 0x250u

/** One past the highest Prcb byte the guest touches. The Prcb must span this. */
#define KERNEL_PRCB_BYTES_TOUCHED 0x254u

/* --- the monitor block itself ---------------------------------------------- */

/** Called as `__stdcall fn(code, ptr)` from 8 distinct sites, all pushing 2 args. */
#define KERNEL_MONITOR_CALLBACK 0x14u

/** Read as a pointer; non-null invites a 0x24-byte write and a 0xABCDEF00 handshake. */
#define KERNEL_MONITOR_BLOCK_A 0x20u

/** Read as a pointer; non-null invites `[p] = 1` and `[p+4] = delta`. */
#define KERNEL_MONITOR_BLOCK_B 0x24u

/**
 * One past the highest monitor-block byte the guest touches.
 *
 * A block smaller than this is REFUSED rather than handed over, because the
 * dereference at `+0x24` would otherwise run off the end of the allocation and
 * read whatever the allocator placed behind it -- which is the silent-wrong-answer
 * failure this whole block exists to avoid.
 */
#define KERNEL_MONITOR_BYTES_TOUCHED 0x28u

/**
 * What we actually allocate for one monitor block: a page of its own.
 *
 * Its OWN allocation, never carved out of the control page or anything else. The
 * guest calls through `+0x14`, and whatever eventually answers that call will write
 * somewhere; a block overlaid on a neighbour turns that into corruption 146x
 * further away than the measured 0x28 bytes it needs.
 */
#define KERNEL_THREAD_MONITOR_BYTES 0x1000u

/** NT's end-of-SEH-chain sentinel. A zero here would walk the chain into 0. */
#define KERNEL_SEH_CHAIN_END 0xFFFFFFFFu

/** Smallest TLS block allocated, so a TlsDataSize of 0 still has the self-pointer. */
#define KERNEL_THREAD_TLS_MIN 0x40u

/** T371: what a started guest thread is blocked in, RECORDED ONLY, it changes no wait behaviour.
 * NONE covers runnable code and also a wait that is satisfied at once (a pre-signalled event,
 * a zero timeout poll). THREAD is an untimed wait that only the target's termination ends.
 * HOST_TIMER is a sleep that ends at a host CLOCK_MONOTONIC deadline, so its wake moment is host
 * timing and never guest state. */
typedef enum {
    KERNEL_THREAD_BLOCK_NONE = 0,
    KERNEL_THREAD_BLOCK_THREAD,
    KERNEL_THREAD_BLOCK_HOST_TIMER,
} kernel_thread_block_state;

/** One requested guest thread, started or not. */
typedef struct {
    uint32_t handle;
    uint32_t thread_id;
    uint32_t start_routine;
    uint32_t start_context;
    uint32_t system_routine;
    uint32_t kernel_stack_size;
    uint32_t tls_data_size;
    /* Base of the whole guard+stack+guard allocation, not of the usable stack. */
    uint32_t stack_region;
    uint32_t stack_region_bytes;
    /* First usable stack byte, and one past the last. `high` is 16-byte aligned. */
    uint32_t stack_low;
    uint32_t stack_high;
    /* Guest esp at the instant `entry_va`'s first instruction runs. */
    uint32_t initial_esp;
    /* What we actually enter: SystemRoutine, or StartRoutine when none was given. */
    uint32_t entry_va;
    uint32_t control_base;
    uint32_t tls_base;
    /* The Prcb monitor block, its own allocation. See the header comment. */
    uint32_t monitor_base;
    /* Requested exit status, valid only while termination_requested is true;
     * not the register value of an arbitrary host stop. */
    uint32_t exit_eax;
    bool created_suspended;
    /* NT suspend count. A CreateSuspended thread starts at 1, and the thread is started
     * when NtResumeThread takes it to 0. */
    uint32_t suspend_count;
    /* The last increment KeSetBasePriorityThread was given, and whether it was ever called.
     * RECORDED ONLY: host scheduling is not changed. */
    int32_t base_priority_increment;
    bool base_priority_set;
    /* KeSetDisableBoostThread's last flag. RECORDED ONLY. */
    bool boost_disabled;
    bool guards_armed;
    bool started;
    /* Host enter completed, including a diagnostic stop/fault; NOT wait signal. */
    bool finished;
    /* Valid request on this exact executing thread with a host terminate hook. */
    bool termination_requested;
    /* Request AND host-confirmed termination outcome; independent of finished. */
    bool terminated;
    bool in_use;
    /* T371: set under the table lock for the duration of a blocking wait, see the enum. */
    kernel_thread_block_state block_state;
    /* The thread handle waited on while block_state is THREAD, else 0. */
    uint32_t block_target;
} kernel_thread_record;

/** Where one thread's stack lives. Pure arithmetic; no allocation implied. */
typedef struct {
    uint32_t region_base;
    uint32_t region_bytes;
    uint32_t guard_bytes;
    /* First usable stack byte: region_base + guard_bytes. */
    uint32_t low;
    /* One past the last usable stack byte, 16-byte aligned. Stacks grow DOWN, so
     * this is the end a fresh esp starts from. */
    uint32_t high;
    /* Guest esp once an entry frame has been written. 0 before that. */
    uint32_t esp;
    /* How many 4-byte slots the entry frame occupies. 0 before it is written. */
    uint32_t frame_slots;
} kernel_thread_stack;

/** Everything the host needs in order to enter a guest thread. */
typedef struct {
    uint32_t handle;
    /* The routine to enter: SystemRoutine, or StartRoutine as a fallback. */
    uint32_t entry_va;
    uint32_t esp;
    uint32_t fs_base;
    /* Carried for diagnostics; they are already on the stack where the guest
     * expects them. */
    uint32_t start_routine;
    uint32_t start_context;
    uint32_t stack_low;
    uint32_t stack_high;
} kernel_thread_launch;

typedef enum {
    KERNEL_THREAD_WAIT_UNSUPPORTED_SCOPE = 0,
    KERNEL_THREAD_WAIT_TERMINAL_HOST_FAILURE,
    KERNEL_THREAD_WAIT_CONDITION_ERROR,
    /* An EVENT or MUTANT wait that cannot be satisfied now and is not a zero-timeout poll.
     * No scheduler is modelled to wake it, so it stops the run instead of hanging (T8g). */
    KERNEL_THREAD_WAIT_WOULD_BLOCK,
} kernel_thread_wait_refusal;

/** The identity a mutant records as its owner (T8g). A guest thread made through
 * PsCreateSystemThreadEx is its own thread handle, anything else (the boot thread, which has no
 * thread record, and host helper threads) is KERNEL_THREAD_IDENTITY_BOOT. 0 is never a handle.
 * INFERRED: NT owns a mutant by KTHREAD, this is the nearest identity the host keeps. */
#define KERNEL_THREAD_IDENTITY_BOOT 0u
uint32_t kernel_thread_current_identity(void);

/**
 * The operations only the host can perform.
 *
 * `has_code` answers whether a guest VA has runnable code; it is consulted on the
 * CREATING thread, so "the lift produced nothing there" is reported at creation
 * rather than discovered on a thread nobody is watching.
 *
 * `enter` installs guest register state on the calling host thread and runs the
 * guest until it stops. It is called on the NEW host thread and must return when
 * the guest thread is finished -- including by catching whatever non-local exit
 * the host uses for a fault.
 *
 * `terminate` ends the CALLING guest thread and must NOT return. It backs
 * `PsTerminateSystemThread`, which the guest's own startup shim reaches as its
 * exit path. Missing/returning hooks are refused with STATUS_NOT_IMPLEMENTED;
 * they never publish a termination request or confirmed guest termination.
 */
typedef struct {
    bool (*has_code)(uint32_t guest_va);
    void (*enter)(const kernel_thread_launch *launch);
    void (*terminate)(uint32_t exit_status);
    /* Optional same-thread outcome query, called after enter returns and without
     * the thread-table lock. True only for a real guest-termination stop. Missing
     * query never confirms termination. Must not reenter/reset the thread model. */
    bool (*termination_confirmed)(const kernel_thread_launch *launch);
    /* MUST NOT RETURN. Called after releasing all thread/object locks and wait
     * pins. Terminal failure must propagate the target's stable host diagnostic;
     * other refusals stop unsupported ordinal 234. Missing/returning hook aborts. */
    void (*wait_refused)(uint32_t handle, kernel_thread_wait_refusal reason);
} kernel_thread_host_ops;

/** Register the ten thread ordinals (adds 233 wait and 99 delay). NtYieldExecution implements only the
 * PASSIVE no-handoff policy: returns STATUS_NO_YIELD_PERFORMED without host
 * yield, guest scheduling, clock advancement, callbacks or IRQL changes.
 * Other IRQLs return STATUS_NOT_IMPLEMENTED with a refusal diagnostic.
 * Returns how many bound. */
unsigned kernel_thread_register(void);

/** T8g: the same PASSIVE, mode 1, nonalertable scope also accepts EVENT and MUTANT handles. A
 * signaled event (consumed by a type 1 auto-reset event) or a free or caller-owned mutant (owner
 * kernel_thread_current_identity, recursion counted) returns STATUS_SUCCESS at once whatever the
 * timeout, a zero-timeout poll that cannot be satisfied is STATUS_TIMEOUT (0x102), and a wait that
 * would genuinely block (NULL or finite timeout) invokes wait_refused with
 * KERNEL_THREAD_WAIT_WOULD_BLOCK, because no scheduler exists to wake it. Past
 * KERNEL_OBJECT_MUTANT_RECURSION_MAX a mutant wait returns STATUS_MUTANT_LIMIT_EXCEEDED.
 *
 * NtWaitForSingleObjectEx supports PASSIVE, mode 1, nonalertable THREAD waits
 * with NULL (infinite) or an explicit zero timeout. Success requires confirmed
 * guest termination; arbitrary host completion invokes wait_refused. Thread
 * records admitted under table->object locks remain retained across NtClose.
 * Reset and host-op changes refuse while a wait is admitted. Object reset and
 * external pthread cancellation remain outside the supported lifecycle contract.
 * The measured current-thread pseudo handle 0xFFFFFFFE with exactly -80000
 * relative 100ns units instead pins its current retained TLS record and sleeps
 * to a CLOCK_MONOTONIC absolute deadline, retrying EINTR, then returns TIMEOUT.
 * It changes no modeled clock/event/GPU/callback state. Other finite/absolute
 * timeouts, self/unstarted/suspended waits and other objects remain unsupported,
 * never fabricated as satisfied. */
unsigned kernel_thread_active_wait_count(void);

/**
 * Install the host operations, by value. NULL restores record-only behaviour.
 *
 * Must be called before any guest code runs. Changing it while a guest thread is
 * running is refused and reported, because a thread already in flight captured
 * the old pointers.
 */
bool kernel_thread_set_host_ops(const kernel_thread_host_ops *ops);

/**
 * Reset the table, releasing every stack and control block.
 *
 * For tests, so one case cannot leak into the next. Refused and reported if any
 * guest thread is still running, because freeing a running thread's stack is a
 * use-after-free with a very confusing crash site.
 */
bool kernel_thread_reset(void);

/**
 * NtSuspendThread requests REFUSED because the thread was already running. Each one is a
 * place the guest believes a thread is paused while it is not.
 */
unsigned kernel_thread_unhonoured_suspend_count(void);

/** Base-priority and boost requests recorded without any effect on host scheduling. */
unsigned kernel_thread_priority_ignored_count(void);

/** How many threads were created but never started. */
unsigned kernel_thread_unstarted_count(void);

/** How many threads were started. Includes ones that have since finished. */
unsigned kernel_thread_started_count(void);

/** How many started threads have not finished yet. */
unsigned kernel_thread_running_count(void);

/**
 * Wait for every started thread to finish, or for `timeout_ms` to elapse.
 *
 * Returns how many are STILL RUNNING, so 0 means everything finished. A non-zero
 * return is a hang, and the caller must treat it as a diagnosable stop rather
 * than waiting longer: this project's first guest threads are expected to hang,
 * and a hang with no diagnosis is the worst available outcome.
 *
 * Threads that have finished are reaped before returning, so this never leaves a
 * joinable corpse behind and never blocks past the deadline.
 */
unsigned kernel_thread_join_all(unsigned timeout_ms);

/**
 * Copy out one thread's record. False for a handle we never issued.
 *
 * Copies under the table lock, which `kernel_thread_find` cannot do -- prefer
 * this whenever a guest thread might be running.
 */
bool kernel_thread_get(uint32_t handle, kernel_thread_record *out);

/**
 * The record for `handle`, or NULL.
 *
 * Returns a pointer INTO the live table and therefore does no locking. Safe only
 * when no guest thread is running; use `kernel_thread_get` otherwise.
 */
const kernel_thread_record *kernel_thread_find(uint32_t handle);

/** T371: copy every in-use record under one hold of the table lock, so the set is a single
 * consistent instant of the model. Returns how many were written (at most `max`). */
unsigned kernel_thread_snapshot(kernel_thread_record *out, unsigned max);

/* --- the pieces, exposed so they can be tested without any lifted code -----
 *
 * These are the whole of the stack and entry-frame logic. They are pure functions
 * over guest addresses plus, for the last two, guest memory -- so the thread
 * model is testable in a fresh clone that has no lifted code and no XBE.
 */

/** Round a guest's requested stack size into the size it will actually get. */
uint32_t kernel_thread_stack_size_for(uint32_t requested);

/** Total bytes to allocate for a stack of `requested` size, guards included. */
bool kernel_thread_stack_region_bytes(uint32_t requested, uint32_t *out_bytes);

/**
 * Lay out one stack inside an already-allocated region.
 *
 * `region_base` must be the base of a region of at least
 * `kernel_thread_stack_region_bytes(requested)` bytes. Sets `low`/`high` with a
 * guard band on both sides and leaves `esp` at 0.
 */
bool kernel_thread_stack_layout(kernel_thread_stack *out, uint32_t region_base,
                                uint32_t requested);

/** Make both guard bands unreadable, so an overflow faults. */
bool kernel_thread_stack_arm_guards(kernel_thread_stack *stack);

/**
 * Write the entry frame and set `stack->esp`.
 *
 * `slots[0]` lands at `esp`, `slots[1]` at `esp+4`, and so on -- i.e. slot 0 is
 * the return address and slot N is the entry routine's argument N-1. See the
 * header comment for how that convention was derived.
 */
bool kernel_thread_stack_write_frame(kernel_thread_stack *stack, const uint32_t *slots,
                                     unsigned count);

/**
 * Initialise a control page: KPCR, KTHREAD stub, TLS pointer and monitor block.
 *
 * `control_base` must address `KERNEL_THREAD_CONTROL_BYTES` of guest memory and
 * `tls_base` a TLS block. `monitor_base` must be a SEPARATE allocation of
 * `monitor_bytes`, and nothing else may live in it.
 *
 * The monitor block is zero-filled here rather than being assumed zero. A fresh
 * region from `guest_region_alloc` happens to be zero today because it comes
 * straight from `mmap`, but "the allocator's current behaviour" is not a property
 * this structure can rest on: the guest *calls* `monitor+0x14` and *dereferences*
 * `monitor+0x20` and `+0x24`, so a stale non-zero byte there is an indirect call or
 * a store to an address we invented.
 *
 * REFUSED, each returning false rather than degrading quietly:
 *   - a zero `control_base`, `tls_base` or `monitor_base`. A null monitor is the
 *     exact condition that routes the guest into the GDT path, so accepting one
 *     would silently reinstate the blocker this field exists to remove.
 *   - `monitor_bytes` below `KERNEL_MONITOR_BYTES_TOUCHED`, because the guest reads
 *     `monitor+0x24` and a shorter block would answer from a neighbour.
 *   - a control page too short to hold the whole Prcb the guest touches.
 *   - any guest write that fails.
 */
bool kernel_thread_control_init(uint32_t control_base, uint32_t tls_base,
                                uint32_t monitor_base, uint32_t monitor_bytes);

/**
 * Guest VA written to `monitor+0x14`, the debug-monitor notify the guest CALLS.
 *
 * INJECTED, exactly as `kernel_thread_set_host_ops` injects the two operations only
 * the host can perform, and for the same reason: the only callable address that is
 * not guest code lives in the host's synthetic dispatch window, and `src/xbox/`
 * deliberately does not depend on `src/host/`. A setter keeps the dependency
 * one-way and keeps this module buildable in a clone with no lifted code.
 *
 * NOT A PARAMETER OF `kernel_thread_control_init`, deliberately. Every call site of
 * that function -- the host's main thread, every guest thread, and four suites --
 * would have to pass the same value, and the one that forgot would hand back a
 * control page with a NULL notify and no symptom until the guest called it. One
 * writer, set once at startup, cannot be forgotten per thread.
 *
 * DEFAULT 0, WHICH IS THE OLD BEHAVIOUR. A zero leaves `monitor+0x14` at the zero
 * the block is filled with, so a host that never arms this gets exactly the NULL
 * indirect call it got before -- loudly, at a named site. Arming is a decision
 * somebody makes, not something that happens by including a header.
 */
void kernel_thread_set_monitor_callback(uint32_t guest_va);

/**
 * Publish `fs:[4]`, the end of the thread's TLS data, into its control page.
 *
 * A separate function rather than a parameter of `kernel_thread_control_init`, which has
 * five test call sites that have no use for it. `tls_data_size` is the size the image
 * REQUESTED, not the page-rounded allocation: the guest indexes backwards from the end of
 * the requested block. False when the control page is not writable.
 */
bool kernel_thread_publish_tls_end(uint32_t control_base, uint32_t tls_base,
                                   uint32_t tls_data_size);

/** The VA `kernel_thread_control_init` will install, or 0 when unarmed. */
uint32_t kernel_thread_monitor_callback(void);

/**
 * Write `KPCR.Irql` in the control page at `control_base`.
 *
 * THE ONLY WRITER OF THAT BYTE, by construction. `kernel_thread_control_init`
 * calls it for the initial PASSIVE_LEVEL, and the host's IRQL publisher (see
 * `kernel_sync_set_irql_publisher`) calls it on every subsequent change with the
 * CALLING thread's own `fs` base. Before that, the byte was written once at thread
 * creation and never again, so all 6 `MEM8(XBOX_FS_BASE + 0x24)` sites compared a
 * permanently-zero copy against DISPATCH_LEVEL and took the PASSIVE arm forever.
 *
 * Lives here rather than in `kernel_sync.c` because this file owns the KPCR layout
 * and the evidence for it, and in `src/xbox/` rather than `src/host/` so the offset
 * has exactly one definition. The host supplies `g_fs_base`; it does not need to
 * know where in the page the field sits.
 *
 * `level` is truncated to a byte, which is what KIRQL is. False when `control_base`
 * is zero or the write is rejected.
 */
bool kernel_thread_set_irql(uint32_t control_base, uint32_t level);

#endif /* TSFP_XBOX_KERNEL_THREAD_H */
