/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host-side control of a lifted-code run.
 *
 * The lifted C has no notion of stopping. Every guest function is `void(void)`
 * and the whole program is one deep host call stack, so there is no return value
 * to check and no place to put an error code. The only way out of a run is a
 * non-local jump, which is what this header exposes.
 *
 * WHY THAT IS THE RIGHT SHAPE, AND NOT A HACK. `docs/lifter-evaluation.md` §9
 * lists "make unresolved indirect dispatch trap instead of setting eax = 0" as
 * the single highest-risk adaptation, because upstream's own notes name
 * silent-zero as a top-three failure mode and its reference title was still
 * taking ~60 failed indirect calls per second during gameplay. Upstream puts the
 * silent-zero inside a macro in generated code, so the usual fix is to patch the
 * lifter. It is not needed: the macro calls `recomp_icall_fail_log(va)` on the
 * failure path *before* it zeroes `eax`, and nothing says that function has to
 * return. Making it jump out converts every silent-zero into a hard stop with a
 * diagnosis, at the cost of nothing, and without a single edit to 2.56 M lines of
 * generated code.
 *
 * ONE RUN PER HOST THREAD, AND WHY THAT IS NOT A DETAIL. Guest threads now exist:
 * each runs on its own host thread with its own guest register file. The jump
 * buffer, the stop record and the armed flag are therefore THREAD-LOCAL.
 *
 * That is not tidiness, it is correctness. `SIGSEGV`, `SIGBUS`, `SIGFPE` and
 * `SIGILL` are synchronous and thread-directed: the kernel delivers them to the
 * thread that executed the faulting instruction. A shared `sigjmp_buf` records one
 * thread's stack pointer and frame pointer, so a second thread faulting into it
 * would `siglongjmp` onto a stack it does not own -- undefined behaviour whose
 * most likely outcomes are two threads writing into each other's live frames, or
 * glibc's `__longjmp_chk` aborting. Neither produces a diagnosis.
 *
 * SIGNAL DISPOSITIONS, BY CONTRAST, ARE PROCESS-WIDE. POSIX has no per-thread
 * disposition, only a per-thread mask, so arming cannot be per-thread even though
 * everything else is. `host_run_arm` therefore installs the handlers on the first
 * call and `host_run_disarm` removes them only when the last armed thread leaves;
 * otherwise one thread finishing its run would silently remove the other thread's
 * only diagnostic path and a later fault would be a bare core dump. A fault on a
 * thread that never armed restores the default disposition and returns, so the
 * faulting instruction re-executes and kills the process for real rather than
 * being swallowed.
 */

#ifndef TSFP_HOST_RUNTIME_H
#define TSFP_HOST_RUNTIME_H

/* `sigjmp_buf`, `siginfo_t` and `struct sigaction` are POSIX, not ISO C, and the
 * project builds with CMAKE_C_EXTENSIONS OFF -- i.e. `-std=c11`, under which
 * glibc defines __STRICT_ANSI__ and hides all three. The build supplies
 * _POSIX_C_SOURCE on the command line, which is the only place it can go: by the
 * time any header is read the first <stdio.h> has already locked features.h in,
 * so defining it here would be too late and would fail with "unknown type name
 * sigjmp_buf" rather than with a reason. Say the reason. */
#if !defined(_POSIX_C_SOURCE) || _POSIX_C_SOURCE < 200112L
#error "compile src/host with -D_POSIX_C_SOURCE=200809L; sigsetjmp and sigaction need it"
#endif

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>

/** Why a run ended. */
typedef enum {
    /* The guest's entry point returned. Not expected: _mainCRTStartup does not. */
    HOST_STOP_RETURNED = 0,
    /* An indirect-call target resolved to nothing. Upstream would have returned
     * zero here and carried on. */
    HOST_STOP_ICALL_UNRESOLVED,
    /* An indirect-call target was outside every executable section and outside
     * the synthetic kernel window -- a garbage pointer reaching a call site. */
    HOST_STOP_ICALL_NOT_CODE,
    /* The lifter emitted no translation for an instruction and the guest reached
     * it. Measured at 917 sites, almost all data decoded as code, so arriving
     * here usually means control flow went somewhere it should not have. */
    HOST_STOP_UNIMPLEMENTED,
    /* A kernel ordinal with no implementation. THIS IS THE EXPECTED OUTCOME of
     * a Phase 1.4 run and is not a failure. */
    HOST_STOP_KERNEL_UNIMPLEMENTED,
    /* A kernel ordinal whose stack discipline we cannot reproduce. Continuing
     * would desync the modelled esp and make every later observation fiction. */
    HOST_STOP_KERNEL_ABI_UNKNOWN,
    /* A statically linked XDK function with no implementation. The address-keyed
     * counterpart of HOST_STOP_KERNEL_UNIMPLEMENTED, and like it, the EXPECTED
     * outcome of a bring-up run that reaches the boundary. `guest_address` is the
     * XDK function's VA. */
    HOST_STOP_XDK_UNIMPLEMENTED,
    /* An XDK address whose stack discipline we cannot reproduce. Same consequence
     * as the kernel case, and the same refusal: no convention is assumed. */
    HOST_STOP_XDK_ABI_UNKNOWN,
    /* A measured XDK address whose section has no HLE module -- XGRPH, XNET,
     * XONLINE and XMV today. NOT relaxed by --continue-on-missing: that flag means
     * "proceed past something nobody has written yet, using its announced default",
     * and there is no module here to have a default. */
    HOST_STOP_XDK_UNROUTED,
    /* A dispatch to an address the adopted surface does not contain. The table and
     * the binary disagree, which is a worse problem than unfinished work, so it is
     * also not relaxed by --continue-on-missing. */
    HOST_STOP_XDK_NOT_MEASURED,
    /* The host faulted. `fault_address` says where. */
    HOST_STOP_FAULT,
    /* The run exceeded its instruction-free step budget. */
    HOST_STOP_BUDGET,
    /* A guest thread reached `PsTerminateSystemThread`, which is the measured exit
     * path of this title's thread startup shim. An ORDERLY END, not a failure. */
    HOST_STOP_THREAD_EXITED,
    /* A guest thread was still running when the watchdog expired. The run is
     * abandoned deliberately: a hang that is reported is diagnosable, a hang that
     * is waited on forever is not. */
    HOST_STOP_THREAD_TIMEOUT,
    /* A guest thread's entry address had no runnable code. Reported on the
     * creating thread, so it names an address rather than vanishing. */
    HOST_STOP_THREAD_NO_CODE,
    /* The title called HalReturnToFirmware -- it asked to REBOOT the console.
     *
     * NOT an expected outcome, and the exit status says so. On hardware this call
     * never returns, so continuing past it would invent a trace no console could
     * produce. It is reached when the title has decided something is unrecoverable:
     * every measured path to it in this image is a storage failure (see context.md
     * §6w), so a run that ends here is almost always a host problem upstream rather
     * than the title genuinely wanting to launch a new image.
     *
     * `ordinal` is 49 and `detail` names the routine value the guest pushed. */
    HOST_STOP_FIRMWARE_RETURN,
    /* An implemented kernel handler reached a state the guest must never continue
     * from: KeBugCheck, or an exception raise nothing dispatches. The EXPECTED end
     * of a run that hits a guest fatal path, as loud as the unimplemented stops. */
    HOST_STOP_KERNEL_FATAL,
    /* T908: host window close/cooperative cancellation, not guest termination or a step budget. */
    HOST_STOP_HOST_SHUTDOWN,
} host_stop_reason;

/* T505: how much of a fault's call chain the handler records. Host frames are the `rbp`
 * chain (one per lifted call at -O0), guest frames the `g_ebp` chain. */
#define HOST_FAULT_HOST_FRAMES 32u
#define HOST_FAULT_GUEST_FRAMES 16u

/** Where a run ended, and with what evidence. */
typedef struct {
    host_stop_reason reason;
    /* Guest VA implicated, if any: the unresolved call target, the unimplemented
     * instruction's address, or 0. */
    uint32_t guest_address;
    /* Kernel ordinal implicated, or 0. */
    unsigned ordinal;
    /* Host address that faulted, for HOST_STOP_FAULT. */
    uintptr_t fault_address;
    /* Signal number, for HOST_STOP_FAULT. */
    int signal_number;
    /* Short free-text detail. Never NULL. */
    const char *detail;
    /* HOST_STOP_FAULT only (T505), all zero otherwise. Plain scalars stored by the signal
     * handler, resolved to names later by host_report (host_symbols). `fault_rip` is the
     * faulting host instruction, `fault_stack_top` the word at the host stack pointer (the
     * return address into the caller when RIP is in a frameless leaf, which the `rbp` chain
     * skips). `fault_host_frames` are return addresses up the `rbp` chain, innermost first.
     * `fault_guest_ebp` is the guest `ebp` at the fault and `fault_guest_frames` the guest
     * return addresses up its chain (the word above each saved ebp), innermost first. */
    uintptr_t fault_rip;
    uintptr_t fault_stack_top;
    uint32_t fault_guest_ebp;
    unsigned fault_host_frame_count;
    unsigned fault_guest_frame_count;
    uintptr_t fault_host_frames[HOST_FAULT_HOST_FRAMES];
    uint32_t fault_guest_frames[HOST_FAULT_GUEST_FRAMES];
} host_stop;

/** Guest `ebp` of the calling thread. Registered by the host (the lifted register file is
 *  not visible here). Called from the fault handler, so it must be async signal safe: a
 *  plain read of the thread local register. */
typedef uint32_t (*host_fault_guest_ebp_fn)(void);
void host_run_set_fault_guest_ebp(host_fault_guest_ebp_fn reader);

/* Nested targets are caller-owned and bounded; no jump buffers are copied.
 * Start with HOST_RUN_SCOPE_INITIALIZER, then init on the owner thread. Initialize
 * the target with sigsetjmp(*host_run_scope_jmp(&scope),0) IN THE CALLER (T827: 0, the mask is never changed, the handler is SA_NODEFER) before
 * push. The target frame/storage must remain live until pop, including cleanup
 * after a jump. Use static/heap storage or volatile automatic storage so C's
 * setjmp rules do not make modified automatic objects indeterminate. Metadata
 * is private: never copy/mutate a scope, move it while active, or race access.
 * Cross-thread attempts require externally synchronized scope storage/lifetime.
 * Pop invalidates the target; reuse requires init and fresh caller sigsetjmp.
 * Root arm/disarm with live scopes, double arm, or invalid rethrow aborts.
 * Push/pop publish with one aligned store of the depth (T827, no signal mask: handled
 * faults are synchronous, and a handler sees the old or the new stack, never a
 * half built one); a target is never published before its caller initializes sigsetjmp. No guest state is restored
 * automatically: caller cleanup/pop must precede rethrow. Detail string storage
 * in the canonical stop record must outlive cleanup and all propagated jumps. */
#define HOST_RUN_SCOPE_MAX 32u
#define HOST_RUN_SCOPE_INITIALIZER {0}
typedef struct host_run_scope {
    sigjmp_buf jump;
    uint64_t owner_token;
    bool initialized,active;
} host_run_scope;
bool host_run_scope_init(volatile host_run_scope *scope);
sigjmp_buf *host_run_scope_jmp(volatile host_run_scope *scope);
bool host_run_scope_push(volatile host_run_scope *scope);
bool host_run_scope_pop(volatile host_run_scope *scope);
unsigned host_run_scope_depth(void);
/* After popping/cleanup, propagate the original diagnostic to next scope/root.
 * Record may be host_run_result() itself. This never returns. */
_Noreturn void host_run_rethrow(const host_stop *record);

/** Human-readable form of a stop reason. Never NULL. */
const char *host_stop_reason_str(host_stop_reason reason);

/**
 * Arm the run: install fault handlers and record the jump target.
 *
 * Call as `if (sigsetjmp(*host_run_jmp(), 1) == 0) { host_run_arm(); ...guest...; }`
 * The buffer belongs to the CALLING THREAD, so every guest thread does its own
 * `sigsetjmp` at the top of its own run. See the header comment for why a shared
 * buffer is undefined behaviour rather than merely untidy.
 */
sigjmp_buf *host_run_jmp(void);

/**
 * Arm the calling thread's run.
 *
 * Installs the process-wide SIGSEGV/SIGBUS/SIGFPE/SIGILL handlers on the first
 * armed thread and resets this thread's stop record. Safe to call from several
 * threads; the installation is reference-counted.
 */
void host_run_arm(void);

/** T1741: an optional first-chance fault hook (the guest write watch). It runs in the signal handler before the fault is
 * turned into a stop and returns true when it handled the fault (the instruction is retried). NULL (default) costs one
 * pointer test on the fault path only. Set before the guest threads start. */
typedef bool (*host_fault_hook)(int signal_number, void *siginfo, void *context);
void host_run_set_fault_hook(host_fault_hook hook);

/**
 * Disarm the calling thread's run.
 *
 * The handlers are removed only when the LAST armed thread disarms. A thread that
 * removed them while another was still running guest code would turn that
 * thread's next fault into a bare core dump with no stop record.
 */
void host_run_disarm(void);

/**
 * End the CALLING THREAD's run now. Does not return.
 *
 * WARNING, and it is a real one: this is a `siglongjmp`, so it unwinds nothing and
 * releases nothing. No mutex may be held on any path that can reach here, or it
 * stays locked for the life of the process. Stopping at an unimplemented ordinal
 * is the EXPECTED end of a run, so this path is taken on essentially every run and
 * is not an edge case.
 */
void host_run_stop(host_stop_reason reason, uint32_t guest_address, unsigned ordinal,
                   const char *detail);

/** How the CALLING THREAD's run ended. Meaningful only after its jump was taken. */
const host_stop *host_run_result(void);

/** True once the calling thread armed and has not disarmed. */
bool host_run_armed(void);

#endif /* TSFP_HOST_RUNTIME_H */
