/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Prcb debug-monitor notify: a callable no-op for `monitor+0x14`.
 *
 * WHAT THE GUEST DOES, MEASURED. Every storage-failure arm of this title's startup
 * funnels into the same three instructions:
 *
 *     mov  eax, fs:[0x20]            ; PrcbData
 *     mov  eax, [eax+0x250]          ; the debug-monitor block
 *     test eax, eax / je <skip>      ; absent on retail, so the skip is the norm
 *     push <ptr> ; push <code>
 *     call [eax+0x14]                ; __stdcall notify(code, ptr)
 *
 * `src/xbox/kernel_thread.h` answers `+0x250` non-null, because a zero there routes
 * the guest into `0x00381D63`, which walks the REAL Xbox kernel's PE headers at
 * 0x80010000 and builds a GDT descriptor -- an `sgdt`/`cli`/`ljmp` path this host has
 * no way to emulate and no business emulating. That decision arms this call, and the
 * block is zeroed, so the target was 0 and the run stopped at a NULL indirect call.
 * This module is the other half of that decision: the no-op the notify needed.
 *
 * MEASURED ARITY: 2 STACK ARGUMENTS, UNANIMOUS ACROSS ALL 8 DISTINCT SITES.
 *
 *     0x0037CBAE  sub_0037CB7D   push 0    / push 2     code 2,   ptr 0
 *     0x0037FDF5  sub_0037FDE1   push 0    / push 0xc   code 0xC, ptr 0
 *     0x003809E0  sub_0038087D   push ebx  / push 2     code 2,   ptr 0 (ebx xored)
 *     0x00383DCA  sub_00383678   push ecx  / push 0xa   code 0xA, ptr stack buffer
 *     0x00383E3E  sub_00383DF3   push ecx  / push 0xb   code 0xB, ptr stack buffer
 *     0x003846C0  sub_00383FE7   push ecx  / push 0xb   code 0xB, ptr stack buffer
 *     0x003846EF  sub_00383FE7   push ecx  / push 0xa   code 0xA, ptr stack buffer
 *     0x00414801  sub_00414511   push ebx  / push 2     code 2,   ptr 0 (ebx xored)
 *
 * No site is followed by caller cleanup -- no `add esp, 8` anywhere -- so the callee
 * pops, which is `__stdcall`. THE PROOF IS SITE 2 AND IT IS A STACK BALANCE, not a
 * count of pushes: `sub_0037FDE1` is the one FPO function in the set, so it never
 * restores `esp` from a frame pointer. Its two branches converge at 0x0037FDFC, and
 * 0x0037FE09 then reads its OWN incoming argument at `[esp+4]` before `ret 4`. That
 * is correct only if the callee popped exactly 8 bytes. Counting pushes can be
 * fooled by a register save; a function reading its own argument afterwards cannot.
 *
 * WHY POPPING THE WRONG NUMBER IS THE ONE THING THAT MUST NOT HAPPEN. `__stdcall` is
 * callee cleanup, so the pop is OURS. Pop too few and 8 bytes of the caller's
 * arguments stay on its stack; pop too many and we eat the caller's locals. Neither
 * crashes. `esp` simply never recovers and every later observation is fiction, with
 * the damage surfacing arbitrarily far from the cause.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS IS A LIBRARY MODULE AND NOT THREE LINES IN kernel_thunk.c
 * ---------------------------------------------------------------------------
 *
 * The obvious home is `recomp_lookup_kernel` in `src/host/kernel_thunk.c`, with a
 * special case in its `stack_args_for()` and `ordinal_name()`. That was the proposed
 * shape and it is REJECTED for one measured reason: `kernel_thunk.c` is compiled
 * into `tsfp_host` ALONE, and `tsfp_host` is not a ctest binary. Nothing in it can
 * be covered by a C suite and no mutation can reach it -- `tools/mutate/c_suites.py`
 * records exactly that trap in its own header. So the prescribed shape would have
 * put the pop count, the arity special case and the trace call in the one place in
 * this project where "a test would have caught it" is not available.
 *
 * `recomp_lookup_manual` is consulted FIRST by every dispatch macro the lifter emits
 * (`recomp_lookup_manual` -> `recomp_lookup` -> `recomp_lookup_kernel`, see
 * RECOMP_ICALL_SAFE in the generated header), and it lives in `xdk_thunk.c`, which is
 * in the `tsfp_thunk` LIBRARY precisely so a suite can link it without the 2.56 M
 * lines of lifted C. Answering there costs one extra integer compare on the icall
 * path and buys a production code path a suite can execute and a mutant can be
 * injected into. Nothing in `kernel_thunk.c` changes at all, and `stack_args_for()`
 * and `ordinal_name()` stay purely ordinal-keyed with no special case to drift.
 *
 * THE VA STILL LIVES IN THE SYNTHETIC KERNEL WINDOW, above XBOX_KERNEL_ORDINAL_MAX.
 * Two reasons, both cheap. `RECOMP_ICALL_IS_CODE` admits any target at or above
 * 0xFE000000 and nothing else outside the title's own sections, so the window is the
 * only range a synthetic callable address can occupy. And the window is real mapped
 * memory, so a guest that ever READS the VA instead of calling it gets a zero rather
 * than a SIGSEGV. Slot 379 is the first past the 379 ordinals (0..378) in a page that
 * holds 1024, so it cannot collide with an ordinal; `kernel_thunk_is_va` caps at
 * XBOX_KERNEL_ORDINAL_MAX and would refuse it, which is belt and braces.
 */

#ifndef TSFP_HOST_MONITOR_THUNK_H
#define TSFP_HOST_MONITOR_THUNK_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#include "kernel_ordinals.h"
#include "kernel_thunk.h"
#include "recomp_abi.h"

/**
 * The synthetic slot the notify is dispatched from.
 *
 * First slot past the ordinals, so it is adjacent to them rather than at an
 * arbitrary offset: the next synthetic callable gets 380 and the relationship stays
 * readable. The static assertions below are what keep that true.
 */
#define MONITOR_THUNK_NOTIFY_SLOT (XBOX_KERNEL_ORDINAL_MAX + 1u)

/** The guest VA the guest calls. MEASURED 0xFE0005EC: base + 379 * 4, i.e. + 0x5EC. */
#define MONITOR_THUNK_NOTIFY_VA KERNEL_THUNK_VA(MONITOR_THUNK_NOTIFY_SLOT)

/**
 * Stack arguments the caller pushes, which is therefore what the callee pops.
 *
 * MEASURED 2, unanimous across all 8 sites and proven by the stack balance in
 * `sub_0037FDE1`. See the header comment.
 */
#define MONITOR_THUNK_NOTIFY_STACK_ARGS 2u

/** How this appears in the trace and the report. */
#define MONITOR_THUNK_NOTIFY_NAME "<Prcb debug-monitor notify>"

/* The slot MUST be above every ordinal, or it would shadow a real kernel export and
 * a kernel call would silently become a notification. */
_Static_assert(MONITOR_THUNK_NOTIFY_SLOT > XBOX_KERNEL_ORDINAL_MAX,
               "the synthetic notify slot would collide with a kernel ordinal");
/* And INSIDE the mapped window, or the VA is an unmapped address that happens to
 * pass RECOMP_ICALL_IS_CODE -- which would turn a read of it into a SIGSEGV and,
 * worse, would put it outside the range anything else reasons about. */
_Static_assert(MONITOR_THUNK_NOTIFY_SLOT < KERNEL_THUNK_WINDOW_SLOTS,
               "the synthetic notify slot falls outside the mapped thunk window");
_Static_assert(MONITOR_THUNK_NOTIFY_VA >= KERNEL_THUNK_VA_BASE
                   && MONITOR_THUNK_NOTIFY_VA
                          < KERNEL_THUNK_VA_BASE + KERNEL_THUNK_WINDOW_BYTES,
               "the synthetic notify VA falls outside the mapped thunk window");

/** Where this module's log lines go. Defaults to stderr. */
typedef int (*monitor_log_fn)(const char *format, ...);

/** Redirect the log. NULL restores the default. For tests and for the host. */
void monitor_thunk_set_log(monitor_log_fn printer);

/**
 * Arm the notify: tell `src/xbox/kernel_thread.c` to write our VA to `monitor+0x14`.
 *
 * Must be called BEFORE any control page is built, because `kernel_thread_control_init`
 * reads the injected VA at the moment it initialises a block and never revisits it.
 * A thread whose page was built before this returns keeps a NULL notify.
 *
 * Returns false only if the injection did not take, which is a programming error
 * rather than a condition to handle.
 */
bool monitor_thunk_arm(void);

/** True once `monitor_thunk_arm` has taken effect. */
bool monitor_thunk_is_armed(void);

/**
 * Resolve `va` to the notify dispatcher, or NULL.
 *
 * `recomp_lookup_manual` calls this first on every indirect call in the program, so
 * the negative answer is one compare. It answers for the notify VA whether or not
 * the block has been armed: a guest that reaches this VA got it from somewhere, and
 * dispatching it is strictly better than the "not-code" stop that is the only other
 * outcome.
 */
recomp_func_t monitor_thunk_lookup(uint32_t va);

/**
 * Run the dispatcher directly, as if the guest had called the notify VA.
 *
 * The same body the lookup path returns, for the reason `xdk_thunk_dispatch_at`
 * exists: one dispatch body means the tested route cannot drift from the production
 * route. A suite sets up `g_esp` over a synthetic frame and calls this.
 */
void monitor_thunk_dispatch_at(void);

/** How many notifications have been answered. */
uint64_t monitor_thunk_notify_count(void);

/** How many arrived on a frame whose two arguments could not be read. */
uint64_t monitor_thunk_unreadable_count(void);

/**
 * The `code` and `ptr` of the most recent notification.
 *
 * Reported rather than merely counted because the code is the only thing that says
 * WHICH notification the guest thought it was making: 2 is the one on the reboot
 * path at 0x0037CBAE, 0xA/0xB carry a stack buffer, 0xC comes from thread startup.
 * False before the first notification.
 */
bool monitor_thunk_last_notification(uint32_t *code, uint32_t *pointer);

/** Zero the counters. For tests. */
void monitor_thunk_reset_counts(void);

/**
 * Print what this boundary did, for the run report.
 *
 * ALWAYS PRINTS, including when the count is zero and when nothing was armed. A
 * deliberate divergence from a retail console that answered zero calls is a
 * different fact from one that was never wired up, and a silent boundary cannot tell
 * the two apart.
 */
void monitor_thunk_report(void);

#endif /* TSFP_HOST_MONITOR_THUNK_H */
