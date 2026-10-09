/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Kernel thunk dispatch: lifted guest code -> kernel_hle_call().
 *
 * See kernel_thunk.h for how a guest kernel call arrives and why the thunk table
 * is patched to synthetic VAs. This file is the other half: the ABI table that
 * says how to unwind each call, and the dispatcher the lifted code lands in.
 */

#include "kernel_thunk.h"
#include "guest_frame_trace.h"

#include <stdio.h>
#include <sys/mman.h>

#include "host_runtime.h"
#include "kernel_arity_oracle.h"
#include "kernel_call.h"
#include "kernel_config.h"
#include "kernel_hle.h"
#include "kernel_memory.h"
#include "kernel_ordinals.h"
#include "recomp_abi.h"
#include "thunk_trace.h"

/* How a kernel export takes its arguments and who cleans up after it. */
typedef enum {
    /* Arguments on the stack, callee pops them. The default for xboxkrnl. */
    THUNK_CC_STDCALL,
    /* First argument in ecx, second in edx, remainder on the stack. The callee
     * pops only the stack remainder. Every `Kf*` export is one of these. */
    THUNK_CC_FASTCALL,
    /* Arguments on the stack, CALLER pops them. The variadic printf-family exports
     * (DbgPrint, RtlSnprintf...) are these: the callee cannot know what to pop, so
     * it pops NOTHING however many arguments were pushed. For a cdecl row,
     * `stack_args` is therefore the CALLEE POP -- always 0 -- and never the
     * argument count, which varies per call site. The arity oracle encodes its
     * cdecl rows the same way (kernel_arity_oracle.h), so the cross-check in
     * tests/test_arity_oracle.py compares like with like. */
    THUNK_CC_CDECL,
} thunk_cc;

typedef struct {
    unsigned ordinal;
    thunk_cc cc;
    /* Arguments passed on the stack, in 4-byte slots. For fastcall this EXCLUDES
     * the one or two that travel in registers. For cdecl it is the callee pop,
     * which is 0 by definition, NOT the per-site argument count. */
    unsigned stack_args;
    /* Arguments passed in ecx/edx: 0, 1 or 2. Always 0 for stdcall. */
    unsigned register_args;
} thunk_abi;

/*
 * WHY THIS TABLE IS SHORT, AND WHY IT IS NOT A GUESS.
 *
 * Getting an entry wrong does not crash. It moves the modelled `esp` by the
 * wrong amount, the caller then reads every subsequent stack slot off by one,
 * and the run continues producing a plausible, wrong trace -- the exact failure
 * mode this project has been bitten by before. So an ordinal we cannot account
 * for STOPS THE RUN rather than being assumed to take zero arguments.
 *
 * Every entry below is cross-checked against an implementation in `src/xbox/`
 * that independently reads that many arguments: the memory-ordinal counts match
 * the `frame_args(frame, N, ...)` call in `kernel_memory.c` one for one, and the
 * IRQL entries match `kernel_sync.c`. A table nobody can check is worth less
 * than no table.
 *
 * `Kf*` ARE FASTCALL, NOT STDCALL. Measured at real call sites in this binary:
 * `KfLowerIrql` is reached by `mov cl, al` with nothing pushed. Treating one as
 * stdcall would both read a bogus argument off the stack and pop four bytes that
 * were never pushed.
 */
static const thunk_abi ABI_TABLE[] = {
    /* kernel_sync.c -- IRQL. The highest-traffic ordinals in the image. */
    {103u, THUNK_CC_STDCALL, 0u, 0u},  /* KeGetCurrentIrql */
    {129u, THUNK_CC_STDCALL, 0u, 0u},  /* KeRaiseIrqlToDpcLevel */
    {130u, THUNK_CC_STDCALL, 0u, 0u},  /* KeRaiseIrqlToSynchLevel */
    {160u, THUNK_CC_FASTCALL, 0u, 1u}, /* KfRaiseIrql(KIRQL) */
    {161u, THUNK_CC_FASTCALL, 0u, 1u}, /* KfLowerIrql(KIRQL) */

    /* kernel_memory.c -- counts verified against its frame_args() calls. */
    {ORD_MmAllocateContiguousMemory, THUNK_CC_STDCALL, 1u, 0u},
    {ORD_MmAllocateContiguousMemoryEx, THUNK_CC_STDCALL, 5u, 0u},
    /*
     * ARITY-OK(169)/ARITY-OK(170): TWO stack arguments each, counted by hand at all
     * three sites because the measured table cannot carry either (169 has ONE site, a
     * tautologous unanimity, and 170's two sites miss the 3-site quorum). Create site
     * 0x00387A67 in sub_00387A55 pushes the literals 0 then 0x6000 and nothing else
     * between prologue and call. Delete sites 0x00387A4E (sub_00387A28) and 0x00387A9B
     * (sub_00387A55) each push exactly StackLimit then StackBase, with the containing
     * functions ending in a bare `ret` whose balance only closes if the callee pops 8.
     * The nxdk oracle agrees on both (@8).
     */
    {ORD_MmCreateKernelStack, THUNK_CC_STDCALL, 2u, 0u},
    {ORD_MmDeleteKernelStack, THUNK_CC_STDCALL, 2u, 0u},
    {ORD_MmFreeContiguousMemory, THUNK_CC_STDCALL, 1u, 0u},
    {ORD_MmGetPhysicalAddress, THUNK_CC_STDCALL, 1u, 0u},
    {ORD_MmPersistContiguousMemory, THUNK_CC_STDCALL, 3u, 0u},
    {ORD_MmQueryAddressProtect, THUNK_CC_STDCALL, 1u, 0u},
    {ORD_MmQueryAllocationSize, THUNK_CC_STDCALL, 1u, 0u},
    {ORD_MmSetAddressProtect, THUNK_CC_STDCALL, 3u, 0u},
    {ORD_NtAllocateVirtualMemory, THUNK_CC_STDCALL, 5u, 0u},
    {ORD_NtFreeVirtualMemory, THUNK_CC_STDCALL, 3u, 0u},
    {ORD_NtQueryVirtualMemory, THUNK_CC_STDCALL, 2u, 0u},
    /* kernel_rtl.c, kernel_pool.c, kernel_event.c. Every count here was verified
     * against the guest's own call sites, because the measured table is wrong for
     * six of these thirteen -- see the refusal in stack_args_for() above. */
    {15u, THUNK_CC_STDCALL, 2u, 0u},  /* ExAllocatePoolWithTag */
    {17u, THUNK_CC_STDCALL, 1u, 0u},  /* ExFreePool */
    {97u, THUNK_CC_STDCALL, 1u, 0u},  /* KeCancelTimer */
    {107u, THUNK_CC_STDCALL, 3u, 0u}, /* KeInitializeDpc */
    {113u, THUNK_CC_STDCALL, 2u, 0u}, /* KeInitializeTimerEx */
    {119u, THUNK_CC_STDCALL, 3u, 0u}, /* KeInsertQueueDpc */
    {128u, THUNK_CC_STDCALL, 1u, 0u}, /* KeQuerySystemTime */
    {137u, THUNK_CC_STDCALL, 1u, 0u}, /* KeRemoveQueueDpc */
    {145u, THUNK_CC_STDCALL, 3u, 0u}, /* KeSetEvent */
    {149u, THUNK_CC_STDCALL, 4u, 0u}, /* KeSetTimer */
    {159u, THUNK_CC_STDCALL, 5u, 0u}, /* KeWaitForSingleObject */
    {289u, THUNK_CC_STDCALL, 2u, 0u}, /* RtlInitAnsiString */
    {301u, THUNK_CC_STDCALL, 1u, 0u}, /* RtlNtStatusToDosError */
    /* kernel_critsec.c. All three take one argument, the critical section pointer.
     * 294 is unanimous across 114 sites and 291 across 4; 277 measures 1 over 10
     * sites but non-unanimously, so it is listed here rather than relying on the
     * measured table -- and it was verified at the call sites, where no site has an
     * `add esp, 4` after the call, which a cdecl caller would need. */
    /* kernel_hal.c. 2 stack arguments, counted by hand at all 12 call sites rather
     * than taken from the measured table, which has 2 over 10 sites but flags it
     * non-unanimous (so stack_args_for() refuses it). Six sites push a 0/1 literal
     * as the second argument, which pins both the count and which one is the
     * BOOLEAN; the 2 sites the measured table cannot see call through a register
     * holding the thunk slot. Site-by-site table in src/xbox/kernel_hal.h. */
    {47u, THUNK_CC_STDCALL, 2u, 0u}, /* HalRegisterShutdownNotification */

    /* kernel_config.c. 5 stack arguments. The measured table has NO row for this
     * ordinal and `ordinal_callsites.json` reports 1 site, because every one of the
     * 12 real sites is a direct `call` to the import jump stub whose body is
     * `jmp [0x4757EC]` -- which callsites.py cannot see through, so it classifies
     * ordinal 24 as a DATA export. Counted by hand at all 12. Site-by-site table,
     * and the three push idioms that have to be discounted to arrive at 5, in
     * src/xbox/kernel_config.h. */
    {24u, THUNK_CC_STDCALL, 5u, 0u},

    /*
     * T34: DbgPrint, the one __cdecl export this title imports. The row exists so the
     * pop is HAND tier with this-image evidence, rather than oracle tier with the
     * "different kernel build" caveat -- the oracle row {8, CDECL, 0, 0} agrees.
     *
     * CDECL MEANS THE CALLEE POPS ZERO, and this image proves the zero at its one
     * real site. Ordinal 8 is reached only through the import stub sub_00384888
     * (`jmp [0x47594C]`), whose single caller is D3D's debug formatter sub_003DF990
     * (14 callers across the D3D section): it vsprintfs into a 0x104-byte stack
     * buffer via sub_003C862C, pushes that ONE buffer pointer, calls the stub, and
     * then cleans up EVERYTHING ITSELF with a single `add esp, 0x118` at 0x003DF9BC
     * = 0x104 locals + 0x10 for the four pushes to sub_003C862C + 0x4 for the one
     * push to DbgPrint. With esp = E at entry the balance only closes if BOTH
     * callees pop nothing, which is caller cleanup -- cdecl -- measured, not
     * assumed. A stdcall reading of the same site would leave esp 4 bytes high on
     * every debug print, permanently.
     *
     * The measured table cannot see the site (the stub is a direct call, so no
     * `_icall_esp` bracket opens -- the ordinal 24 mechanism above), which is why
     * this row and the oracle are the only two sources, and why they must agree.
     */
    {8u, THUNK_CC_CDECL, 0u, 0u}, /* DbgPrint -- caller cleans up, pop ZERO */

    /* ZERO arguments at 0x4414E8 (126) and 0x4414FC (127). Both return EDX:EAX.
     * The two pushes before 127 are saved ESI/EDI, restored after its arithmetic;
     * the measured arity 2 is a register-save overcount. nxdk decorates both @0. */
    {126u, THUNK_CC_STDCALL, 0u, 0u},
    {127u, THUNK_CC_STDCALL, 0u, 0u}, /* KeQueryPerformanceFrequency */

    /* ONE argument (MicroSeconds), VOID. T8a: the measured row {151,1,15,unanimous} already
     * agrees, but 6 of the 15 sites are `call esi/ebx` through a register loaded from the
     * slot (XNET 0x0043A394, 0x0043A3B0, 0x0043A3E3, 0x0043A3EF, 0x0043AF7D, 0x0043AF99), so
     * the row is hand-pinned here: at every one exactly one value (a literal 0xa or 0x32, or
     * a register) is pushed for the call, no `add esp` follows, and the nxdk .def row is
     * KeStallExecutionProcessor@4. */
    {151u, THUNK_CC_STDCALL, 1u, 0u}, /* KeStallExecutionProcessor */

    /* ONE argument each, T8c (src/xbox/kernel_fpstate.h has the site table). 142 is
     * KeSaveFloatingPointState(PKFLOATING_SAVE), 139 is KeRestoreFloatingPointState(same). All
     * 12 sites (6 + 6) are `push <area pointer>; call dword ptr [slot]` with no `add esp`
     * after and no register-dispatched or import-stub site, so the measured bracket is
     * complete. Hand-pinned so the pop is HAND tier with this-image evidence in a build with
     * no lifted tree; the nxdk rows are KeSaveFloatingPointState@4 and
     * KeRestoreFloatingPointState@4. */
    {139u, THUNK_CC_STDCALL, 1u, 0u}, /* KeRestoreFloatingPointState */
    {142u, THUNK_CC_STDCALL, 1u, 0u}, /* KeSaveFloatingPointState */

    /* TWO arguments, T8f: NtSetSystemTime(PLARGE_INTEGER NewTime, PLARGE_INTEGER OldTime OPTIONAL). The one
     * site is the XONLINE wrapper 0x00425813, `push 0; lea eax,[esp+8]; push eax; call dword ptr
     * [0x4759C0]; ret 8` at 0x0042581A, with no `add esp` after. The slot is referenced once. The
     * measured row {228,2,1,1} has one voter, below MEASURED_ARITY_MIN_SITES, so stack_args_for()
     * would refuse it; the oracle and nxdk (NtSetSystemTime@8) agree. */
    {228u, THUNK_CC_STDCALL, 2u, 0u}, /* NtSetSystemTime */

    /* ONE argument, T8f: NtDeleteFile(POBJECT_ATTRIBUTES). The one site is `push eax` (the address of
     * the OBJECT_ATTRIBUTES built at ebp-0x14) then `call dword ptr [0x475990]` at 0x004228EA
     * inside the XONLINE cache closer 0x0042287C, with no `add esp` after and the result read
     * from eax. The slot is referenced once. The measured row {195,1,1,1} has one voter, below
     * MEASURED_ARITY_MIN_SITES, so stack_args_for() would refuse it; the oracle and nxdk
     * (NtDeleteFile@4) agree. */
    {195u, THUNK_CC_STDCALL, 1u, 0u}, /* NtDeleteFile */

    /* TWO arguments, T8d: MmLockUnlockPhysicalPage(ULONG_PTR PhysicalAddress, BOOLEAN UnlockPage). All
     * four sites are in the XPP USB driver, `push 1; push <physical>; call`, two as `call [0x475930]`
     * (0x00473E5F, 0x00473E78) and two through `edi` (0x00474240, 0x00474253), with no `add esp`
     * after. The measured row {176,2,2,1} has two voters and the register sites are invisible to
     * the bracket, so this hand row pins the pop in a build with no lifted tree; nxdk @8. */
    {176u, THUNK_CC_STDCALL, 2u, 0u}, /* MmLockUnlockPhysicalPage */

    /* TWO arguments, T8c: NtSetEvent(HANDLE, PLONG PreviousState). All four sites (one XAPI
     * SetEvent wrapper at 0x0037FF97, three in DSOUND) are `push 0; push <handle>; call
     * [0x4758A0]` with no `add esp` after, and the nxdk row is NtSetEvent@8. */
    {225u, THUNK_CC_STDCALL, 2u, 0u}, /* NtSetEvent */

    /* TWO arguments, T8e: NtReleaseMutant(HANDLE, PLONG PreviousCount). The one site is the
     * title's ReleaseMutex at 0x00380009: `push 0; push [esp+8]; call [0x4758A8]` with no
     * `add esp` after, `ret 4` in the wrapper. The slot is referenced once, as `call dword
     * ptr`. The measured table has one voter, below MEASURED_ARITY_MIN_SITES, so
     * stack_args_for() would refuse it; the oracle and nxdk (NtReleaseMutant@8) agree. */
    {221u, THUNK_CC_STDCALL, 2u, 0u}, /* NtReleaseMutant */

    /* THREE arguments, T8e: NtDuplicateObject(HANDLE Source, PHANDLE Target, ULONG Options). The
     * one site is the title's DuplicateHandle wrapper at 0x0037CBE2 (seven wrapper arguments,
     * `ret 0x1c`), which forwards three: `push [esp+0x1c]; push [esp+0x14]; push [esp+0x10];
     * call [0x4757AC]` at 0x0037CBEE with no `add esp` after. The slot is referenced once, as
     * `call dword ptr`. The measured table has one voter, below MEASURED_ARITY_MIN_SITES, so
     * stack_args_for() would refuse it; the oracle and nxdk (NtDuplicateObject@12) agree. */
    {197u, THUNK_CC_STDCALL, 3u, 0u}, /* NtDuplicateObject */

    /* THREE arguments each, T8c: RtlAnsiStringToUnicodeString(260) and
     * RtlUnicodeStringToAnsiString(308), (Destination, Source, AllocateDestinationString). One
     * site each, both complete (the slots 0x4759D4 and 0x4759D8 are referenced once, as
     * `call dword ptr`): `push 0; push &source; push &destination; call [slot]` at 0x003D15A2
     * and 0x003D166A with no `add esp` after. The measured table has one voter for each, below
     * MEASURED_ARITY_MIN_SITES, so stack_args_for() would refuse it; nxdk has both as @12. */
    {260u, THUNK_CC_STDCALL, 3u, 0u}, /* RtlAnsiStringToUnicodeString */
    {308u, THUNK_CC_STDCALL, 3u, 0u}, /* RtlUnicodeStringToAnsiString */

    /* kernel_file.c. 6 stack arguments. The measured table says 6 over 12 sites but
     * flags it non-unanimous, so stack_args_for() refuses it. Confirmed by hand at
     * six sites; the one disagreeing site's extra push is a register save, settled by
     * another site with the same code shape and argument literals and no such push.
     * Site table in src/xbox/kernel_file.h. */
    {202u, THUNK_CC_STDCALL, 6u, 0u}, /* NtOpenFile */

    /*
     * kernel_io.c and kernel_file.c -- the file-I/O family.
     *
     * EVERY COUNT HERE WAS READ OFF THE GUEST'S OWN CALL SITES, not taken from the
     * measured table, and the reason is ordinal 219: the measured table says 6 and
     * the guest pushes 8. That is not a near miss. Our handler is the __stdcall
     * callee and must pop what the caller pushed, so trusting the 6 would leave 8
     * bytes of the caller's arguments on its stack and desync `esp` permanently,
     * with the damage surfacing arbitrarily far from the cause. The measured row is
     * flagged non-unanimous, so `stack_args_for()` was already refusing it -- but a
     * refusal stops the run, and these rows are what let it proceed CORRECTLY.
     *
     * The scanner under-counted 219 because it takes the MINIMUM across sites on the
     * premise that over-counting is its only dangerous error. That premise is false:
     * at site 0x003DF10B the lifter's `_icall_esp` bracket opens two pushes late, so
     * `push edi` (Length) and `push 0x3E6428` (ByteOffset) fall outside it and the
     * site tallies 6 where it pushes 8. Per-site tallies were {8,8,8,8,6,10}; the
     * minimum is the one wrong number in the set.
     *
     * Argument order for each is pinned by literals at a named site, and the
     * per-ordinal site tables live beside the handlers in src/xbox/kernel_io.h and
     * src/xbox/kernel_file.h.
     */
    {190u, THUNK_CC_STDCALL, 9u, 0u}, /* NtCreateFile -- 9, NOT 11; see kernel_file.h */
    {207u, THUNK_CC_STDCALL, 10u, 0u}, /* NtQueryDirectoryFile */
    {210u, THUNK_CC_STDCALL, 2u, 0u},  /* NtQueryFullAttributesFile */
    {211u, THUNK_CC_STDCALL, 5u, 0u},  /* NtQueryInformationFile */
    {218u, THUNK_CC_STDCALL, 5u, 0u},  /* NtQueryVolumeInformationFile */
    {219u, THUNK_CC_STDCALL, 8u, 0u},  /* NtReadFile -- measured table says 6, WRONG */
    {226u, THUNK_CC_STDCALL, 5u, 0u},  /* NtSetInformationFile */

    /*
     * ARITY-OK(255): TEN stack arguments. Needed because of the minimum-site refusal
     * below, and it is the case that justifies having one.
     *
     * PsCreateSystemThreadEx is the FIRST kernel call the guest makes, and the measured
     * table carries `{255u, 10u, 1u, 1}` -- ten arguments, ONE site, flagged unanimous.
     * That flag is worthless here: a single voter always agrees with itself. So the boot
     * path's very first call has been running on an uncorroborated measurement all along,
     * and the site-count refusal surfaced that rather than causing it.
     *
     * COUNTED BY HAND at the one site, 0x0037FEEE in `sub_0037FEB5`. Ten pushes, and
     * there is nothing to discount: the prologue is `push ebp; mov ebp, esp` with NO
     * callee-saved register saves at all, and no `pop` occurs before the call, so every
     * push between the prologue and the call is an argument.
     *
     *     push 0x37fe1d           ; pushed first, so the LAST argument
     *     push 0
     *     push ecx                ; [ebp+0x18] >> 2, masked
     *     push dword ptr [ebp+0x14]
     *     push dword ptr [ebp+0x10]
     *     push dword ptr [ebp+0x1c]
     *     push dword ptr [0x7eb4c8]
     *     push eax
     *     push 0
     *     lea eax,[ebp+0xc]; push eax   ; the FIRST argument, a handle out-parameter
     *     call dword ptr [0x475894]
     *
     * Two independent confirmations that this reading is right rather than merely
     * self-consistent. The literal 0x0037FE1D is a CODE address, and it is exactly the
     * address the run reports the new guest thread entering -- so that argument is the
     * start routine and the count reaches it. And `lea eax,[ebp+0xc]` makes argument 0 a
     * pointer to the caller's own parameter slot, which 0x0037FEB8 reads and tests before
     * the call: the same in/out aliasing NtOpenFile uses at 0x00380D32.
     */
    {255u, THUNK_CC_STDCALL, 10u, 0u}, /* PsCreateSystemThreadEx */

    /*
     * ARITY-OK(87): FASTCALL, and this row exists for the CONVENTION rather than the
     * count. The measured row `{87u, 0u, 9u, 1}` is unanimous over 9 sites and would be
     * accepted as it stands -- but a measured row carries no convention, so the zero
     * would be believed for the wrong reason and the next handler written against it
     * would reach for `kernel_frame_arg(0)` and get the return address.
     *
     * MEASURED at all 9 sites: zero pushes, ECX a PIRP, and ALL NINE set EDX with an
     * 8-bit `xor dl, dl`. The byte width is proved rather than assumed at 0x0046ED96,
     * where the compiler had a LIVE 32-bit NTSTATUS in EDX and destroyed exactly its low
     * byte immediately before the call. So a handler MUST MASK `frame->edx` TO 8 BITS;
     * the upper 24 are the caller's leftovers. No handler yet -- there is no IRP model.
     * Full evidence in src/xbox/kernel_io.h ARITY-OK(87).
     */
    {87u, THUNK_CC_FASTCALL, 0u, 2u}, /* IofCompleteRequest(PIRP ecx, CCHAR dl) */

    /*
     * ARITY-OK(236): EIGHT stack arguments, NOT the SIX the measured table publishes.
     * The measured row `{236u, 6u, 11u, 0}` is non-unanimous so stack_args_for() already
     * refuses it. NtWriteFile has NO handler, so this row's job is to let a continuation
     * run (stop-on-missing off) get past the ordinal with esp intact instead of stopping
     * again on ABI_UNKNOWN.
     *
     * STRONGER THAN THE 219 ROW IT MIRRORS. All ELEVEN sites hand-count to 8
     * UNANIMOUSLY, where 219's hand tallies were {8,8,8,8,6,10} with outliers in both
     * directions. The site set is proved exhaustive: slot 0x004757B8's literal occurs
     * exactly 11 times in the image and every occurrence is a `call dword ptr`. An esp
     * balance swept over arities 5..10 across all seven containing functions is
     * consistent only at 8, and `FUN_003df0a0` has NO FRAME POINTER -- no `leave` to
     * absorb an error -- so its five returns land at exactly zero depth only at 8.
     * Decisively, that same function calls NtReadFile at 0x003DF10B and NtWriteFile at
     * 0x003DF375 with a byte-for-byte identical argument shape (same ByteOffset global
     * 0x3E6428, same IoStatusBlock global 0x3E6420, same round-up-to-512 Length), which
     * pins 236 to 219's hand-verified 8 directly.
     *
     * A SECOND SCANNER UNDER-COUNT MECHANISM, distinct from 219's late bracket and
     * recorded because it will recur: at 0x00380E3E and 0x003810BF an argument is pushed
     * BEFORE AN INTERVENING STDCALL. `push ebx` (ByteOffset) then
     * `push [ebp+8]; call XGetSectionSize` -- that callee is `ret 4` and pops only its
     * own argument, so `push ebx` is still live at entry and no basic-block-aligned
     * bracket can see it. Those sites tally 7. Per-site set {8,8,8,7,7,8,8,8,8,6,8}, and
     * the published 6 is the minimum. Full evidence in src/xbox/kernel_io.h.
     */
    {236u, THUNK_CC_STDCALL, 8u, 0u}, /* NtWriteFile -- measured table says 6, WRONG */

    /*
     * ARITY-OK(279): THREE stack arguments. The second case the minimum-site refusal
     * exists for, and the ordinal that was the live boot blocker at 30 kernel calls.
     *
     * The measured table carries `{279u, 3u, 1u, 1}` -- three arguments, ONE site,
     * flagged unanimous, which over a single voter is a tautology. Refused, correctly.
     *
     * COUNTED BY HAND at the one site, 0x0037C952 in sub_0037C914 (FLIRT:
     * `_XGetSectionHandleA@4`). Three pushes with nothing to discount among them --
     * the basic block opens at 0x0037C948 and the four prologue pushes (ebp and the
     * three callee-saved registers) sit above `sub esp, 0x10`:
     *
     *     push 1                        ; pushed first, so the LAST argument (BOOLEAN)
     *     lea eax,[ebp-8];    push eax  ; String2 -- the XBE section's name
     *     lea eax,[ebp-0x10]; push eax  ; String1 -- the caller's name
     *     call dword ptr [0x475780]
     *     test al, al                   ; the result is a BYTE
     *
     * Thunk slot 0x00475780 appears exactly ONCE in the whole disassembly, as this
     * `call dword ptr`. No register-indirect site and no `jmp [slot]` import stub, so
     * for once the scanner has no blind spot and "1 site" is complete.
     *
     * TWO independent confirmations, so this is not merely self-consistent.
     *
     * A FORCED STACK BALANCE derives the 3 rather than checking it. The call sits in a
     * loop (0x0037C93F..0x0037C961) containing NO `add esp`, and the match exit
     * (`jne 0x37C977`, `mov eax,edi`, `jmp 0x37C970`, `pop edi; pop esi; pop ebx`)
     * contains none either, so esp at 0x0037C970 must equal esp after `push edi` for
     * EVERY trip count. The trip count genuinely varies -- 0x00380FB7, 0x00380FC4 and
     * 0x00380FD1 ask for "$$XTINFO", "$$XTIMAGE" and "$$XSIMAGE", sections 13, 14 and
     * 15 of this XBE -- so the per-iteration delta must be zero, giving
     * (pop of 289) + (pop of 279) = 20 bytes, and the entry balance pins 289 at 8.
     * Hence 279 pops 12, i.e. three dwords. That re-derives ordinal 289 as TWO
     * arguments from scratch and agrees with the hand verification already recorded
     * for it, while disagreeing with the measured table's 1 exactly as that record
     * says it should.
     *
     * AND THE OPERAND SHAPES. `sub esp, 0x10` is exactly two 8-byte OBJECT_STRINGs
     * with no third local, and both are initialised by ordinal 289 immediately before
     * the call -- MEASURED in the live run as calls 28 and 29 of the 30-call boot
     * trace, returning to 0x0037C92C and 0x0037C948. A one- or two-argument reading
     * has nowhere to put the second operand.
     */
    {279u, THUNK_CC_STDCALL, 3u, 0u}, /* RtlEqualString */

    {277u, THUNK_CC_STDCALL, 1u, 0u}, /* RtlEnterCriticalSection */
    {291u, THUNK_CC_STDCALL, 1u, 0u}, /* RtlInitializeCriticalSection */
    {294u, THUNK_CC_STDCALL, 1u, 0u}, /* RtlLeaveCriticalSection */

    /*
     * kernel_crypto.c. THE MEASURED TABLE HAS NO ROW FOR ANY OF THESE FIVE, and not
     * because the measurement disagreed with itself -- because it NEVER VOTED.
     * `ordinal_callsites.json` records all five as 100% stub-attributed (`bracket: 0`,
     * `register: 0`): every real site is a direct `call` to a one-instruction import
     * jump stub, and callsites.py opens an `_icall_esp` bracket only for INDIRECT
     * calls, so there is no push window to measure. `kernel_arity.inc` steps straight
     * from {328u,...} to {358u,...}. stack_args_for() therefore falls out of its loop
     * and returns false for all five, never reaching either refusal condition -- so
     * without these rows the host stops on HOST_STOP_KERNEL_ABI_UNKNOWN at the first
     * crypto call even with a correct handler registered. Ordinal 24 above is the same
     * situation and is the true precedent for this block.
     *
     * COUNTED TWO INDEPENDENT WAYS, AGREEING. (1) Hand-counted push runs in the raw
     * instruction stream, after resolving each stub and the secondary XcSHAUpdate
     * trampoline at 0x0037E656; noisy UPWARD, because a backward walk crosses an
     * unbroken prologue. (2) The decompiler's dataflow argument reconstruction at every
     * site, which does not share that failure mode and is UNANIMOUS for all five. The
     * mode of (1) equals the unanimous value of (2) in every case.
     *
     * __stdcall IS MEASURED, NOT INFERRED FROM THE `Xc` PREFIX: zero of the 144 crypto
     * call sites is followed by `add esp, N`, and a one-instruction `jmp` stub cannot
     * clean up either, so the kernel routine pops its own arguments. Corroborated by
     * `_XcspComputeSectionDigest@8` at 0x003851A1 ending `ret 8`.
     *
     * ORDINAL 340 IS WHY THIS BLOCK IS NOT OPTIONAL. Its push vote is
     * {6:1, 7:30, 8:3, 10:2, 12:4}, so the MINIMUM rule would have said SIX. The outlier
     * is 0x004149FC, where the compiler hoisted six shared argument pushes above the
     * `je` at 0x004149B5, leaving only `push 0x7f2ba4` in the block containing the call.
     * Both arms are seven-argument calls. This is the same trap that made ordinal 219's
     * measured row wrong, and it is the second time the minimum rule has been the one
     * wrong number in a set.
     *
     * Full site tables, the literals pinning each argument ORDER, and the five
     * independent measurements of the 116-byte SHA context are in src/xbox/kernel_crypto.h.
     */
    {335u, THUNK_CC_STDCALL, 1u, 0u}, /* XcSHAInit */
    {336u, THUNK_CC_STDCALL, 3u, 0u}, /* XcSHAUpdate */
    {337u, THUNK_CC_STDCALL, 2u, 0u}, /* XcSHAFinal */
    /* T1114: original 338/339 caller votes 4/7 sites unanimously have three
     * args; nxdk CC0 exports @12 and actual xemu kernel probe corroborate.
     * Crypt helper439CA3 raises IRQL, passes object+88/length/data, restores
     * IRQL and RET8. No inferred quorum exemption or return-preserve profile. */
    {338u, THUNK_CC_STDCALL, 3u, 0u}, /* XcRC4Key */
    {339u, THUNK_CC_STDCALL, 3u, 0u}, /* XcRC4Crypt */
    {340u, THUNK_CC_STDCALL, 7u, 0u}, /* XcHMAC -- SEVEN; the minimum rule says 6 */
    {346u, THUNK_CC_STDCALL, 2u, 0u}, /* XcDESKeyParity */

    /*
     * ARITY-OK(327) and ARITY-OK(328): ONE stack argument EACH. The measured table
     * publishes `{327u, 2u, 1u, 1}` and `{328u, 1u, 1u, 1}` -- one site each, so both
     * "unanimous" flags are tautologies and stack_args_for() refuses both. It is right
     * to: 327's published TWO IS WRONG, and this is the FIRST recorded case of the
     * scanner OVER-counting. Ordinals 219 and 340 both lose pushes; this one gains one.
     *
     *     NTSTATUS __stdcall XeLoadSection(PXBE_SECTION_HEADER Section);
     *     NTSTATUS __stdcall XeUnloadSection(PXBE_SECTION_HEADER Section);
     *
     * THE TWO SITES ARE ADJACENT FUNCTIONS DIFFERING IN EXACTLY ONE WAY, which is both
     * the evidence and the mechanism. sub_0037C97B (`_XLoadSectionByHandle@4`) needs its
     * handle again after the call, so it opens with a callee-saved `push esi`:
     *
     *     0x0037C97B  push esi                  ; CALLEE-SAVE, not an argument
     *     0x0037C97C  mov  esi, [esp + 8]       ; its OWN argument, at +8 because of it
     *     0x0037C980  push esi                  ; the ONE argument
     *     0x0037C981  call dword ptr [0x475788] ; -> XeLoadSection
     *     0x0037C998  pop  esi
     *     0x0037C999  ret  4
     *
     * sub_0037C99C (`_XUnloadSectionByHandle@4`) never needs its argument again, so it
     * forwards it in place and saves nothing:
     *
     *     0x0037C99C  push dword ptr [esp + 4]  ; the ONE argument
     *     0x0037C9A0  call dword ptr [0x47578C] ; -> XeUnloadSection
     *     0x0037C9B7  ret  4
     *
     * callsites.py brackets its `_icall_esp` window on basic-block boundaries and a
     * function entry IS a block head, so at 0x0037C97B the window opens on the prologue
     * and tallies BOTH pushes. At 0x0037C99C there is no prologue to swallow. Same
     * scanner, same rule, and the tally is right exactly where there is nothing to
     * miscount -- which is why a minimum-across-sites rule cannot help here and a
     * single-site over-count has no outlier to discard.
     *
     * A FORCED STACK BALANCE DERIVES THE 1 IN BOTH CASES, the ordinal 279 standard.
     * Neither function contains any `add esp` on any path. For 327, with esp = E at
     * entry: two pushes take it to E-8, the CALL to E-12, the callee pops the return
     * address and 4N argument bytes to E-8+4N, and `pop esi` at 0x0037C998 must restore
     * the register saved at [E-4], so E-8+4N = E-4 and N = 1. At N = 2 that `pop esi`
     * loads THE RETURN ADDRESS and `ret 4` jumps to the caller's argument. For 328: one
     * push to E-4, CALL to E-8, callee pops to E-4+4N, and `ret 4` at 0x0037C9B7 needs
     * esp = E, so N = 1. Both error arms are esp-neutral: the only thing on them is
     * `push eax; call 0x37E9FD`, and sub_0037E9FD ends `ret 4`.
     *
     * AND THE ARGUMENT FETCH PINS WHICH PUSH IS THE PROLOGUE. `mov esi, [esp+8]` reads
     * sub_0037C97B's own argument, which a stdcall callee finds at [esp+4] on entry.
     * Reaching it at +8 means EXACTLY ONE push has happened, so 0x0037C97B is the
     * callee-save and 0x0037C980 is the only argument push. The scanner's block-aligned
     * window cannot make that reading.
     *
     * CORROBORATED BY A COMMITTED DECORATION FROM A DIFFERENT KERNEL BUILD. nxdk's
     * `lib/xboxkrnl/xboxkrnl.exe.def` (CC0-1.0, USE in docs/provenance.md) lines 334-335
     * list `XeLoadSection@4` and `XeUnloadSection@4`: four argument BYTES each, so one
     * dword, and no leading `@`, so __stdcall not __fastcall. Read for corroboration
     * only -- that file is kernel 3944/4039 and this title is XDK 5849 -- but it agrees
     * with both readings of this binary and DISAGREES with the measured table.
     *
     * Thunk slots 0x475788 and 0x47578C each occur exactly ONCE in the disassembly, as
     * the `call dword ptr` above. No register-indirect site and no `jmp [slot]` stub, so
     * unlike ordinal 47 (real site count 12, reported 10) "1 site" really is complete.
     *
     * Full evidence, plus the measured XBE_SECTION_HEADER layout the handlers use, is in
     * src/xbox/kernel_xe.h.
     */
    {327u, THUNK_CC_STDCALL, 1u, 0u}, /* XeLoadSection -- measured table says 2, WRONG */
    {328u, THUNK_CC_STDCALL, 1u, 0u}, /* XeUnloadSection */
    /* kernel_io.c, XapiFormatFATVolumeEx. 10 each, hand-verified at 0x0038143D, 0x00381479
     * (196) and 0x003816D4 (200), and forced by the frame's `pop esi; pop ebx; pop edi`. */
    {196u, THUNK_CC_STDCALL, 10u, 0u}, /* NtDeviceIoControlFile -- measured table says 12, WRONG */
    {200u, THUNK_CC_STDCALL, 10u, 0u}, /* NtFsControlFile */

    /*
     * kernel_file.c / kernel_object.c: the symbolic-link and mutant ordinals.
     *
     * 203 NtOpenSymbolicLinkObject takes TWO stack arguments, (PHANDLE, POBJECT_ATTRIBUTES),
     * where desktop NT takes three. The measured row {203,2,3,0} is NON-UNANIMOUS because the
     * site at 0x0038047F has a leading `push esi`, a callee-save restored by `pop esi` at
     * 0x00380508, so stack_args_for() refuses it. At 0x0037F8F2 exactly two pushes sit between
     * the prologue and the call, and the oracle (`NtOpenSymbolicLinkObject@8`) agrees. The row
     * is here, though the oracle would supply it, so the stack-balance evidence lives with the
     * number rather than only in a log line.
     *
     * 192 NtCreateMutant takes THREE. One call site (0x0037FFD9), so the measured row
     * {192,3,1,1} is a single-voter tautology and is refused. INDEPENDENT EVIDENCE: 0x0037FFD1
     * is a control-flow merge, so no push can precede it on only one path, and exactly three
     * pushes follow. The oracle says 3.
     */
    {203u, THUNK_CC_STDCALL, 2u, 0u}, /* NtOpenSymbolicLinkObject -- desktop NT takes 3 */
    {192u, THUNK_CC_STDCALL, 3u, 0u}, /* NtCreateMutant */

    /*
     * T33: the six remaining oracle-versus-refused-measurement disagreements, each
     * HAND-VERIFIED at its call sites in the binary. None has a handler yet; like the
     * 236 row above, each row's job is to keep esp intact when a continuation run
     * crosses the ordinal. In every case the hand count AGREES WITH THE ORACLE and the
     * measured row is wrong, by one of the mechanisms already on record.
     *
     * ARITY-OK(76): FIVE. Measured {76,6,1,1} is a single-voter tautology. The one
     * site is 0x0037D188: `push edi` at 0x0037D178 is a CALLEE-SAVE the block-aligned
     * bracket swallowed (the 327 mechanism), then exactly five argument pushes --
     * &[ebp-4] (ReturnedLength), &[ebp-0x24] (FsInformation), 0x20 (Length),
     * 5 (FileFsSizeInformation), [ebp+8] (FileObject). Oracle: 5.
     *
     * ARITY-OK(150): FIVE, FORCED. Measured {150,6,1,1}, same tautology. The one site
     * is 0x00439FD6 in a frameless XNET helper: `push esi` at 0x00439FB8 is the
     * callee-save, then five argument pushes (Dpc, Period 0xC8, DueTime as a BY-VALUE
     * LARGE_INTEGER = TWO dwords -1:0xFFE17B80, Timer). The balance is forced: entry E,
     * six pushes to E-24, callee pops 4N to E-24+4N, and `pop esi; ret` at 0x00439FDC
     * needs E-4, so N = 5. Oracle: 5.
     *
     * ARITY-OK(175): THREE. Measured {175,1,14,0}, non-unanimous, and the published 1
     * is the minimum rule losing pushes again (the 219/340 mechanism: at 0x0043A765 two
     * of the three pushes sit above an intervening `call 0x439cff`). Hand-counted 3 at
     * sites in THREE sections: 0x0040B715 (DSOUND), 0x0043A765 (XNET), 0x0046D491
     * (XPP), and at the register-dispatch site 0x0040F638 the bracket cannot see at
     * all. Always (BaseAddress, NumberOfBytes, UnlockPages). Oracle: 3.
     *
     * ARITY-OK(198): TWO. Measured {198,4,1,1}, tautology. The one site is 0x0037D0F6
     * and the over-count is a NEW MECHANISM worth naming: the frame allocates its
     * 8-byte local with `push ecx; push ecx` at 0x0037D0ED, and both land in the
     * bracket. The two real arguments are &[ebp-8] (IoStatusBlock) and [ebp+8]
     * (FileHandle). Oracle: 2.
     *
     * ARITY-OK(234): FOUR, FORCED. Measured {234,3,2,0}, non-unanimous. At 0x00380046
     * the four pushes are Timeout (esi), Alertable ([ebp+0x10]), WaitMode (1),
     * Handle ([ebp+8]); the under-count is the back-edge target 0x0038003D opening a
     * block so `push esi` falls outside the bracket. The balance is forced: on
     * STATUS_ALERTED (0x101) the code loops to 0x0038003D and re-pushes all four with
     * NO `add esp` anywhere in the loop, so the callee must pop exactly 16 bytes for
     * esp to hold over any trip count. Oracle: 4.
     *
     * ARITY-OK(358): ZERO. Measured {358,1,1,1}, tautology. The one site is 0x0046F086
     * and every push above it (ebx, esi, edi at 0x0046F074..7C) is a prologue
     * callee-save; the bracket counted one. Nothing is popped by the callee and the
     * result is a BOOLEAN in al (`test al, al` at 0x0046F08C). Oracle: 0.
     */
    {76u, THUNK_CC_STDCALL, 5u, 0u},  /* IoQueryVolumeInformation -- measured says 6 */
    {150u, THUNK_CC_STDCALL, 5u, 0u}, /* KeSetTimerEx -- measured says 6 */
    {175u, THUNK_CC_STDCALL, 3u, 0u}, /* MmLockUnlockBufferPages -- measured says 1 */
    {198u, THUNK_CC_STDCALL, 2u, 0u}, /* NtFlushBuffersFile -- measured says 4 */
    {234u, THUNK_CC_STDCALL, 4u, 0u}, /* NtWaitForSingleObjectEx -- measured says 3 */
    {358u, THUNK_CC_STDCALL, 0u, 0u}, /* HalIsResetOrShutdownPending -- measured says 1 */

    /*
     * kernel_av.c: AvGetSavedDataAddress 0, AvSendTVEncoderOption 4, AvSetDisplayMode 6,
     * AvSetSavedDataAddress 1 stack arguments.
     *
     * Ordinals 3 and 4 are PINNED BY A FORCED STACK BALANCE, not a push count: a walker over
     * the caller at 0x003D8450 with the pop counts brute-forced finds EXACTLY ONE balanced
     * assignment, {3 = 6, 2 = 4, 4 = 1, 1 = 0}. The nxdk .def agrees on all four, but it is a
     * different kernel build, so it corroborates rather than decides. 2 has nine call sites
     * and the measured row {2,4,9,1} is accepted on its own; its row is here for the evidence.
     */
    {1u, THUNK_CC_STDCALL, 0u, 0u}, /* AvGetSavedDataAddress */
    {2u, THUNK_CC_STDCALL, 4u, 0u}, /* AvSendTVEncoderOption */
    {3u, THUNK_CC_STDCALL, 6u, 0u}, /* AvSetDisplayMode */
    {4u, THUNK_CC_STDCALL, 1u, 0u}, /* AvSetSavedDataAddress */
};

#define ABI_TABLE_COUNT (sizeof(ABI_TABLE) / sizeof(ABI_TABLE[0]))

/*
 * MEASURED arities for everything the hand table does not cover.
 *
 * Generated by tools/lift/callsites.py from the guest's own call sites, and only
 * written at all once it has reproduced the arities our own handlers establish
 * independently (9 of them on this image, spanning 1 to 5 arguments, 0
 * mismatches). Without it the run stops at the first ordinal outside the hand
 * table, which is correct but gets nowhere -- 10 of 151 ordinals are implemented,
 * so the whole value of a Phase 1.4 run is the ORDER in which the guest asks for
 * the other 141, and that order cannot be observed without unwinding each call.
 *
 * The hand table above still wins. A measurement is evidence; an arity we can
 * point at an implementation for is better evidence.
 */
#if defined(TSFP_HAVE_MEASURED_ARITIES)
#include "kernel_arity.inc"
#else
typedef struct {
    unsigned ordinal;
    unsigned stack_args;
    unsigned sites;
    unsigned char unanimous;
} measured_arity;
static const measured_arity MEASURED_ARITIES[1] = {{0u, 0u, 0u, 0}};
#define MEASURED_ARITY_COUNT 0u
static const unsigned MEASURED_DATA_ORDINALS[1] = {0u};
#define MEASURED_DATA_ORDINAL_COUNT 0u
#endif

/*
 * Fewest call sites a measured arity needs before it is believed at all.
 *
 * The unanimity flag cannot catch a row with one voter, because one voter always agrees
 * with itself. Reasoning for the value, and the three ordinals that made it necessary,
 * are at the refusal in `stack_args_for()`.
 */
#define MEASURED_ARITY_MIN_SITES 3u

/* Thread-local, like every guest register. See the long note above
 * `kernel_thunk_dispatch` for why a lock cannot substitute for this. */
#if defined(__GNUC__) || defined(__clang__)
#define THUNK_TLS __thread
#else
#define THUNK_TLS _Thread_local
#endif

static bool g_stop_on_missing = true;

/*
 * THE TRACE NOW LIVES IN `thunk_trace.c`, SHARED WITH THE XDK ADDRESS BOUNDARY.
 *
 * It was here, with its own lock and its own per-thread ids, and the extraction is a
 * move rather than a rewrite -- same append-then-patch shape, same rule that the lock
 * is never held across anything that can `host_run_stop`. The reason it had to move is
 * that an address-keyed XDK dispatcher now appends to the same ordered sequence, and
 * two separate arrays cannot be interleaved after the fact: "DirectSoundCreate came
 * after RtlEqualString" stops being recoverable, and that sentence is the deliverable
 * of a bring-up run.
 */

bool kernel_thunk_is_va(uint32_t va)
{
    return va >= KERNEL_THUNK_VA_BASE
           && KERNEL_THUNK_ORDINAL(va) <= XBOX_KERNEL_ORDINAL_MAX;
}

void kernel_thunk_set_stop_on_missing(bool stop)
{
    g_stop_on_missing = stop;
}

/*
 * Which source answered. A bool no longer suffices now that there are three, and the
 * distinction is not cosmetic: a diagnostic that attributed an oracle-derived pop to a
 * measurement would misstate how well-founded the number is.
 */
typedef enum {
    ARITY_SOURCE_NONE = 0,
    /* ABI_TABLE: hand-verified at this image's own call sites. */
    ARITY_SOURCE_HAND,
    /* MEASURED_ARITIES: this image's call sites, past the quorum and unanimity gates. */
    ARITY_SOURCE_MEASURED,
    /* kernel_arity_oracle.c: an MSVC name decoration from a DIFFERENT kernel build. */
    ARITY_SOURCE_ORACLE,
} arity_source;

/* Resolve an ordinal's stack-argument count. Hand table first, measured table second,
 * the derived oracle third, failure fourth. `*src` says which answered, so a diagnostic
 * can be honest about how well-founded the number is. */
static bool stack_args_for(unsigned ordinal, unsigned *out, arity_source *src)
{
    for (size_t i = 0; i < ABI_TABLE_COUNT; i++) {
        if (ABI_TABLE[i].ordinal == ordinal) {
            *out = ABI_TABLE[i].stack_args;
            *src = ARITY_SOURCE_HAND;
            return true;
        }
    }
    for (size_t i = 0; i < (size_t)MEASURED_ARITY_COUNT; i++) {
        if (MEASURED_ARITIES[i].ordinal != ordinal) {
            continue;
        }
        /* A NON-UNANIMOUS measurement is refused, not used.
         *
         * MEASURED on this image: of 13 ordinals whose arity was independently
         * verified against their call sites, the measured table is WRONG for 6 --
         * ExAllocatePoolWithTag and ExFreePool and KeInsertQueueDpc and KeSetEvent
         * measured 0 where they take 2, 1, 3 and 3; KeInitializeDpc measured 2 where
         * it takes 3; RtlInitAnsiString measured 1 where all 22 sites push 2.
         *
         * Every one of those six is flagged non-unanimous. The flag was correct and
         * this function was ignoring it. `callsites.py` takes the minimum across
         * sites on the premise that over-counting is its only dangerous error, but
         * that premise is false here: a late-opening `_icall_esp` bracket makes a
         * site under-count, so the minimum is an under-estimate.
         *
         * Refusing is the safe direction. Our handler is the __stdcall callee and
         * must pop the arguments, so a count that is too low leaves bytes on the
         * guest's stack and desyncs `esp` permanently, with the damage surfacing
         * arbitrarily far away. An ordinal that reaches here with no usable count
         * stops with a diagnostic naming it, which is a bug report; a silent wrong
         * pop is not.
         */
        if (!MEASURED_ARITIES[i].unanimous) {
            break;
        }
        /*
         * AND A ROW WITH TOO FEW VOTERS IS ALSO REFUSED, because "unanimous" over one
         * site is a tautology rather than a measurement.
         *
         * This became load-bearing when the call-site scanner learned to see calls made
         * through a register and through `jmp [slot]` import stubs. That was a real
         * improvement -- it took the table from 112 rows to 116 -- but it also promoted
         * three ordinals to "unanimous" on the strength of one or two sites with nothing
         * to corroborate them:
         *
         *     46  HalReadWritePCISpace                  6 args, 2 sites
         *     84  IoSynchronousDeviceIoControlRequest   8 args, 2 sites
         *     196 NtDeviceIoControlFile                12 args, 1 site
         *
         * Ordinal 196 is the one that would have bitten. It used to reach the refusal
         * below and stop with a diagnostic naming it; with the flag alone it would
         * instead run straight past, popping TWELVE dwords inferred from a single call
         * site -- and 12 is more than that export actually takes. Over-popping is the
         * worse direction: it eats the caller's own locals and `esp` never recovers.
         *
         * THREE, not two, and the choice is deliberate. Three means a number has been
         * agreed by at least two sites beyond the first, which is corroboration rather
         * than repetition. Two would admit a row where both voters are register-indirect
         * sites -- and those are structurally prone to the late-`_icall_esp` undercount
         * described above, because a load sitting in an earlier basic block correlates
         * with the argument pushes sitting there too, so the bracket opens after them and
         * the site votes low. Two correlated voters are closer to one than to three.
         *
         * The asymmetry settles it. Refusing a row that was actually right costs a
         * reported stop naming the exact ordinal, which is a bug report somebody can act
         * on in minutes. Accepting a row that was wrong costs a permanently desynced
         * `esp` and a plausible, wrong trace that cannot be falsified from the inside.
         * So this errs toward refusing, and an ordinal that genuinely needs its number
         * gets a hand-verified ABI_TABLE row above -- which is the mechanism that is
         * supposed to carry a count this process cannot establish on its own.
         */
        if (MEASURED_ARITIES[i].sites < MEASURED_ARITY_MIN_SITES) {
            break;
        }
        *out = MEASURED_ARITIES[i].stack_args;
        *src = ARITY_SOURCE_MEASURED;
        return true;
    }
    /*
     * THIRD: the derived arity oracle, and `break` above rather than `return false` is
     * what lets a refused measurement reach it.
     *
     * src/xbox/kernel_arity_oracle.c derives every ordinal's argument count from the
     * MSVC name decorations in nxdk's CC0-1.0 `xboxkrnl.exe.def`. It is a NAME, not a
     * measurement, so it shares no failure mode with the scanner above: it cannot be
     * fooled by a late `_icall_esp` bracket, by a callee-saved push inside an argument
     * window, or by a `jmp [slot]` import stub, which are the three ways every measured
     * row here has actually gone wrong.
     *
     * IT IS THIRD DESPITE BEING STRUCTURALLY STRONGER THAN A GATED MEASUREMENT, and that
     * ordering is the design decision. It describes a kernel build that is not ours. A
     * hand row carries reasoning -- a forced stack balance, a literal pinning an argument
     * order -- that a decoration does not have, so where they disagree the hand row may
     * be the right one. Nothing here overrides either table.
     *
     * MEASURED, and the reason this is purely additive: of 151 imported ordinals, the
     * oracle disagrees with the measured table for 18 and NONE OF THOSE 18 IS A ROW THIS
     * FUNCTION ACCEPTS. Every one is already refused by the unanimity or quorum gate. So
     * adding this branch changes no answer the host gives today; it only converts
     * HOST_STOP_KERNEL_ABI_UNKNOWN into a correct continuation for 59 ordinals. Pinned by
     * tests/test_arity_oracle.py, which fails loudly if that ever stops being true.
     *
     * DATA exports are refused here, not answered with zero: 18 of the imported ordinals
     * are variables the guest dereferences and have no arity at all.
     */
    unsigned oracle_dwords = 0;
    if (kernel_arity_oracle_callee_pop(ordinal, &oracle_dwords)) {
        *out = oracle_dwords;
        *src = ARITY_SOURCE_ORACLE;
        return true;
    }
    return false;
}

static const char *ordinal_name(unsigned ordinal)
{
    const char *name = xbox_kernel_ordinal_name(ordinal);
    return name ? name : "<unknown ordinal>";
}

/*
 * SAID OUT LOUD, ONCE PER ORDINAL. An oracle-derived pop is well-founded enough to run on
 * and not well-founded enough to go unmentioned: it comes from a different kernel build,
 * so the ordinal numbering itself is an assumption rather than a measurement. Once per
 * ordinal rather than once per call, because the ordered call trace is the deliverable of
 * a bring-up run and must not be buried under repetition. The bitmap is racy across guest
 * threads and the only consequence of losing the race is saying it twice.
 */
static unsigned char g_oracle_announced[XBOX_KERNEL_ORDINAL_MAX + 1u];

static void announce_oracle_arity(unsigned ordinal, unsigned stack_args)
{
    if (ordinal > XBOX_KERNEL_ORDINAL_MAX || g_oracle_announced[ordinal]) {
        return;
    }
    g_oracle_announced[ordinal] = 1u;
    fprintf(stderr,
            "kernel: ordinal %u (%s) unwound with %u stack dwords DERIVED FROM THE nxdk "
            ".def ORACLE, not measured in this image\n",
            ordinal, ordinal_name(ordinal), stack_args);
}

/* KERNEL_THUNK_WINDOW_BYTES is in the header now, so a synthetic slot placed above
 * the ordinals can be checked against the mapping at compile time. */
static void *g_window;
static bool g_disk_identity_available;
static bool g_eeprom_keys_available;

void kernel_thunk_set_disk_identity_available(bool available)
{
    g_disk_identity_available = available && g_window != NULL;
}

bool kernel_thunk_disk_identity_available(void)
{
    return g_disk_identity_available;
}

bool kernel_thunk_eeprom_keys_available(void)
{
    return g_eeprom_keys_available;
}
bool kernel_thunk_publish_eeprom_keys(void)
{
    uint8_t eeprom_key[16], hd_key[16];
    g_eeprom_keys_available = false;
    if (!g_window || !kernel_config_eeprom_key(321u, eeprom_key) ||
        !kernel_config_eeprom_key(323u, hd_key) ||
        !kernel_guest_write_bytes(KERNEL_THUNK_VA_EEPROM_KEY, eeprom_key, 16u) ||
        !kernel_guest_write_bytes(KERNEL_THUNK_VA_HD_KEY, hd_key, 16u)) return false;
    g_eeprom_keys_available = true;
    return true;
}

bool kernel_thunk_map_window(void)
{
    if (g_window) {
        return true;
    }
    void *want = (void *)(uintptr_t)KERNEL_THUNK_VA_BASE;
    /* MAP_FIXED_NOREPLACE, not MAP_FIXED: if something already owns this address
     * we must hear about it rather than silently unmap it. */
    void *got = mmap(want, KERNEL_THUNK_WINDOW_BYTES, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (got == MAP_FAILED) {
        return false;
    }
    if (got != want) {
        (void)munmap(got, KERNEL_THUNK_WINDOW_BYTES);
        return false;
    }
    g_window = got;
    return true;
}

void kernel_thunk_unmap_window(void)
{
    g_disk_identity_available = false;
    g_eeprom_keys_available = false;
    if (g_window) {
        (void)munmap(g_window, KERNEL_THUNK_WINDOW_BYTES);
        g_window = NULL;
    }
}

/* The annex must sit inside the mapped window, above every dispatchable slot. */
_Static_assert(KERNEL_THUNK_ANNEX_OFFSET + KERNEL_THUNK_ANNEX_BYTES_USED <=
                   KERNEL_THUNK_WINDOW_BYTES,
               "the data annex must fit inside the mapped thunk window");
_Static_assert(KERNEL_THUNK_ANNEX_OFFSET / 4u > XBOX_KERNEL_ORDINAL_MAX + 1u,
               "the data annex must sit above the ordinal and monitor slots");

/*
 * Where a data ordinal's import slot points. Almost every ordinal publishes its own
 * 4-byte window slot, which reads as zero until something gives it a value. The two
 * 8-byte console-identity variables (322 XboxHardwareInfo, 324 XboxKrnlVersion) are
 * redirected into the annex, because at their own slots the second dword of each
 * would overwrite the neighbouring key export (323 XboxHDKey, 325 XboxSignatureKey).
 * kernel_identity_publish writes the annex bytes; see src/xbox/kernel_identity.h
 * for the published identity and its measured consumers.
 */
static uint32_t slot_published_va(unsigned ordinal)
{
    switch (ordinal) {
    case 41u:
        return g_disk_identity_available ? KERNEL_THUNK_VA_DISK_MODEL : 0u;
    case 42u:
        return g_disk_identity_available ? KERNEL_THUNK_VA_DISK_SERIAL : 0u;
    case 321u:
        return g_eeprom_keys_available ? KERNEL_THUNK_VA_EEPROM_KEY : 0u;
    case 323u:
        return g_eeprom_keys_available ? KERNEL_THUNK_VA_HD_KEY : 0u;
    case 322u:
        return KERNEL_THUNK_VA_XBOX_HARDWARE_INFO;
    case 324u:
        return KERNEL_THUNK_VA_XBOX_KRNL_VERSION;
    default:
        return KERNEL_THUNK_VA(ordinal);
    }
}

size_t kernel_thunk_patch_table(uint32_t slots, size_t count, size_t *skipped)
{
    size_t patched = 0;
    size_t left_alone = 0;

    for (size_t i = 0; i < count; i++) {
        kernel_guest_ptr slot = (kernel_guest_ptr)(slots + (uint32_t)(i * 4u));
        uint32_t encoded = 0;
        if (!kernel_guest_read_u32(slot, &encoded)) {
            break;
        }
        /* A terminating zero ends the table on real hardware. */
        if (encoded == 0u) {
            break;
        }
        /* Anything without the import flag is already an address, or is something
         * we have not understood. Either way it is not ours to overwrite. */
        if ((encoded & 0x80000000u) == 0u) {
            left_alone++;
            continue;
        }
        unsigned ordinal = (unsigned)(encoded & 0x7FFFFFFFu);
        if (ordinal > XBOX_KERNEL_ORDINAL_MAX) {
            left_alone++;
            continue;
        }
        if (!kernel_guest_write_u32(slot, slot_published_va(ordinal))) {
            break;
        }
        patched++;
    }

    if (skipped) {
        *skipped = left_alone;
    }
    return patched;
}

/* True when the guest's own code only ever READS this ordinal's thunk slot. Not
 * used to decide anything -- the mapped window makes the decision unnecessary --
 * but a stop at an ordinal measured this way is worth saying out loud, because it
 * means the guest is treating a variable as a function or vice versa. */
bool kernel_thunk_measured_as_data(unsigned ordinal)
{
    for (size_t i = 0; i < (size_t)MEASURED_DATA_ORDINAL_COUNT; i++) {
        if (MEASURED_DATA_ORDINALS[i] == ordinal) {
            return true;
        }
    }
    return false;
}

/*
 * The one function every kernel call lands in.
 *
 * `g_esp` points at the guest return address the lifted caller pushed, so the
 * frame is simply `stack_ptr = g_esp`. The ordinal comes from a file-scope
 * variable rather than a parameter because the lifted code can only call
 * `void(void)`: `recomp_lookup_kernel` sets it immediately before returning this
 * function.
 *
 * THREAD-LOCAL, AND A LOCK WOULD NOT DO. The generated `RECOMP_ICALL` macro does
 * `_fn = recomp_lookup_kernel(_va); ... RECOMP_ABI_CALL(_va, _fn);` -- the store
 * and the call are separated by statements in code we do not own. A second guest
 * thread overwriting this between them would make us look up the wrong ABI entry,
 * call the wrong handler with this thread's frame, and then advance `g_esp` by the
 * wrong number of bytes: a PERMANENT esp desync, non-deterministic, producing
 * exactly the plausible-but-wrong trace the hand-written ABI table exists to
 * prevent. A mutex cannot fix it because the critical section would have to span
 * generated code with no place to release it. Thread-local storage can, and does.
 */
static THUNK_TLS unsigned g_pending_ordinal;

static void kernel_thunk_dispatch_body(void);

/* T1289: the guest frame trace times every kernel call (exclusive of the phases it enters). */
static void kernel_thunk_dispatch(void)
{
    gft_enter(GFT_KERNEL, g_pending_ordinal);
    kernel_thunk_dispatch_body();
    gft_leave();
}

static void kernel_thunk_dispatch_body(void)
{
    const unsigned ordinal = g_pending_ordinal;
    (void)thunk_trace_thread_id();
    unsigned stack_args = 0;
    arity_source arity_from = ARITY_SOURCE_NONE;
    const bool have_abi = stack_args_for(ordinal, &stack_args, &arity_from);
    const kernel_entry *entry = kernel_hle_entry(ordinal);
    uint32_t return_address = 0;
    /* The slot THIS call owns. Patching "the last entry" instead would let one
     * thread write its result into another thread's record. */
    size_t slot = THUNK_TRACE_NO_SLOT;

    (void)kernel_guest_read_u32(g_esp, &return_address);

    const bool implemented = entry && entry->state == KERNEL_ENTRY_IMPLEMENTED;

    /*
     * RECORDED BEFORE THE CALL, FOR EVERY ORDINAL AND NOT ONLY THE MISSING ONES.
     *
     * It used to pre-append only on the unimplemented path, on the premise that an
     * implemented handler always returns and can therefore be appended afterwards
     * with its result. MEASURED: that premise is false, and ordinal 49 is the
     * counter-example. `HalReturnToFirmware` is implemented and deliberately never
     * comes back -- it calls the host's firmware sink, which `siglongjmp`s out --
     * so the append after the call never ran. The first time this host ever reached
     * ordinal 49, the stop record named it and the ordered trace did not, and
     * `thunk_trace_total_of_kind(ORDINAL)` reported 16 for a run that had made 17
     * kernel calls. A total that under-reports is the one failure a total must not
     * have: it makes a complete trace look truncated and a truncated one complete.
     *
     * The result is not known yet and is patched below on every path that gets that
     * far, which is the append-then-patch shape this file already used. The lock is
     * released before `host_run_stop`, which never returns.
     */
    slot = thunk_trace_append_pending(THUNK_KIND_ORDINAL, ordinal, 0u, return_address,
                                      implemented);

    if (!implemented) {
        /* The expected end of a bring-up run. */
        if (g_stop_on_missing) {
            host_run_stop(HOST_STOP_KERNEL_UNIMPLEMENTED, return_address, ordinal,
                          ordinal_name(ordinal));
        }
        /* Continuing: still need the stack discipline, so fall through. */
    }

    if (!have_abi) {
        /* We know what it is called and nothing about its signature. Guessing
         * zero arguments would desync esp for every later call, and a desynced
         * run does not crash -- it keeps going and lies. */
        host_run_stop(HOST_STOP_KERNEL_ABI_UNKNOWN, return_address, ordinal,
                      ordinal_name(ordinal));
    }
    if (arity_from == ARITY_SOURCE_ORACLE) {
        announce_oracle_arity(ordinal, stack_args);
    }

    kernel_call_frame frame = {
        .stack_ptr = (kernel_guest_ptr)g_esp,
        .stack_limit = 0u,
        .ecx = 0u,
        .edx = 0u,
        .has_registers = false,
    };
    /* Registers are attached unconditionally. A stdcall handler never asks for
     * them, a fastcall one refuses the call without them, and we do not know the
     * convention for an ordinal that came from the measured table -- which only
     * recovers stack arguments, that being all `esp` cleanup needs. Supplying
     * them always is therefore strictly more informative and never wrong. */
    kernel_frame_set_registers(&frame, g_ecx, g_edx);

    uint32_t result = 0;
    if (implemented) {
        result = kernel_hle_call(ordinal, &frame);
    } else {
        result = entry ? entry->default_return : 0u;
        (void)kernel_hle_call(ordinal, &frame);
    }

    /* Patch OUR slot by index; "the last entry" is another thread's by now.
     * THUNK_TRACE_NO_SLOT passes straight through, so a full trace needs no branch. */
    thunk_trace_patch_result(slot, result);

    /* Callee cleanup: the return address, plus the stack arguments a real
     * `ret N` would have popped. Register arguments are not on the stack and
     * must not be counted. */
    g_eax = result;
    if (frame.has_result_high) {
        g_edx = frame.result_high;
    }
    g_esp += 4u + 4u * stack_args;
}

recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

recomp_func_t recomp_lookup_kernel(uint32_t xbox_va)
{
    if (!kernel_thunk_is_va(xbox_va)) {
        return NULL;
    }
    g_pending_ordinal = KERNEL_THUNK_ORDINAL(xbox_va);
    return kernel_thunk_dispatch;
}

/* `recomp_lookup_manual` -- the override seam a VA answered here takes priority over
 * the lifted body with -- IS NOW IN `xdk_thunk.c`. It used to sit here returning NULL.
 * It moved rather than being duplicated because it has exactly one owner and the first
 * thing to need it is the address-keyed XDK boundary; leaving a second definition here
 * would be a link error at best and a silent override of the override at worst. */
