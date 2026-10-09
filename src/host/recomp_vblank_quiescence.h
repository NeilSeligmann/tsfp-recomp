/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_RECOMP_VBLANK_QUIESCENCE_H
#define TSFP_RECOMP_VBLANK_QUIESCENCE_H
#include "kernel_thread.h"
#include <stdbool.h>
#include <stdint.h>

/* T371: the two-consumer quiescence rule for vblank delivery. See docs/vblank-delivery.md
 * "T371 results" for the measurement behind it.
 *
 * The counter 0x563918 has exactly two guest readers, both through getter 0x22030: the owner's
 * frame wait loop (return address 0x153975) and the loading-bar worker loop (0x156CB6). A reader
 * that is running can sample the counter at host timing moments, so a delivery is deterministic
 * only while every OTHER guest thread cannot run until the delivering thread's next guest action.
 * The predicate is a pure function of the thread table (guest scheduler state), never wall time,
 * never host thread timing, and it only OBSERVES: it never pokes the counter, never advances the
 * getter and never infers a host thread's guest writes. */

#define RVQ_GETTER 0x22030u
#define RVQ_OWNER_RETURN 0x153975u
#define RVQ_WORKER_RETURN 0x156CB6u
#define RVQ_OWNER_START 0x3801D9u
#define RVQ_WORKER_START 0x156CB0u
/* The image has exactly four guest thread start routines (static scan of the CreateThread wrapper
 * 0x37FEB5, `tools/vblank_probe.py readers`): 0x3801D9 owner, 0x156CB0 worker, 0x3C0FE0 and 0x30160.
 * 0x3801D9 and 0x156CB0 reach a counter reader. 0x3C0FE0 (T424) is the title network library's
 * 50 ms polling thread, created at 0x3C108A, and 0x30160 (T593, lifted by T556) is the title's
 * loader thread, created at 0x301EB, which waits 16 ms on the current-thread pseudo handle in a loop.
 * `python -m tools.tracegaps threadreach [--start 0x30160]` proves from the retail image that each
 * can neither reach the getter 0x22030 nor create a reader thread: no function that reaches the
 * getter, or creates a reader, has its address taken anywhere except as the start routine passed
 * to the CreateThread wrapper. So both are entries of the DEFAULT non-reader table, which
 * `evaluate` skips in EVERY state (runnable, host timer, host stopped): a thread that cannot
 * sample the counter needs no parking, and sleeping to a host deadline is not "parked". Every
 * other thread must still be parked, and an unproven thread that sleeps to a host deadline is
 * still refused (RVQ_REFUSE_HOST_TIMER). */
#define RVQ_NON_READER_NET_POLL_START 0x3C0FE0u
#define RVQ_NON_READER_LOADER_START 0x30160u
typedef enum {
    RVQ_UNSTARTED = 0, /* created or suspended, cannot run until another thread resumes it */
    RVQ_TERMINATED,    /* guest termination confirmed (PsTerminateSystemThread) */
    RVQ_HOST_STOPPED,  /* finished by a host stop or fault, never confirmed as a guest exit */
    RVQ_BLOCKED,       /* untimed wait on a thread's termination */
    RVQ_HOST_TIMER,    /* sleeping to a host deadline, wakes by wall time */
    RVQ_RUNNABLE       /* started, not finished, in no recorded wait */
} rvq_thread_state;

typedef enum {
    RVQ_HOLDS = 0,
    RVQ_REFUSE_DELIVERER_UNKNOWN,   /* the delivering thread has no live started record */
    RVQ_REFUSE_RUNNABLE,            /* another thread runs and could sample the counter */
    RVQ_REFUSE_HOST_TIMER,          /* another thread wakes at a host deadline */
    RVQ_REFUSE_HOST_STOPPED,        /* another thread ended by a host stop, not a guest exit */
    RVQ_REFUSE_BLOCKED_ON_ACTIVE    /* blocked on a thread that is itself not quiescent */
} rvq_reason;

typedef struct {
    bool holds;
    rvq_reason reason;
    /* The first refusing thread (0 when holds) and what it was started at. */
    uint32_t handle;
    uint32_t start_routine;
    /* Threads examined besides the deliverer, a guard against an empty pass. */
    unsigned examined;
} rvq_verdict;

rvq_thread_state recomp_vblank_quiescence_classify(const kernel_thread_record *record);
const char *recomp_vblank_quiescence_state_name(rvq_thread_state state);
const char *recomp_vblank_quiescence_reason_name(rvq_reason reason);

/* The predicate over one consistent snapshot. Every thread other than `deliverer` and other than
 * a thread whose start routine is in `non_reader_starts` must be TERMINATED, UNSTARTED, or BLOCKED
 * on a thread that is `deliverer` or itself parked. A start routine belongs in that table only with
 * evidence that it cannot reach the readers and cannot create a reader thread: `classify_start` in
 * tools/vblank_probe.py reporting closed_non_reader (a closed direct-call closure), or the entry
 * barrier proof of `python -m tools.tracegaps threadreach` (T424). `evaluate` passes the default
 * table, `evaluate_with` an explicit one that replaces it. */
rvq_verdict recomp_vblank_quiescence_evaluate_with(const kernel_thread_record *records,
                                                   unsigned count, uint32_t deliverer,
                                                   const uint32_t *non_reader_starts,
                                                   unsigned non_reader_count);
rvq_verdict recomp_vblank_quiescence_evaluate(const kernel_thread_record *records, unsigned count,
                                              uint32_t deliverer);
/* T592: `evaluate_with` plus handles the CALLER proves parked while they are runnable: a thread that
 * is provably inside a stretch of code that reaches no counter reader until the deliverer ends or a
 * known call is made (the owner in the loading bar gate, docs/vblank-delivery.md "T592 results"), or
 * a thread this host holds idle. The proof is the caller's, this predicate only counts such a thread
 * as parked when its record says RUNNABLE. Every other thread keeps the rules above. */
rvq_verdict recomp_vblank_quiescence_evaluate_parked(const kernel_thread_record *records,
                                                     unsigned count, uint32_t deliverer,
                                                     const uint32_t *non_reader_starts,
                                                     unsigned non_reader_count,
                                                     const uint32_t *parked_handles,
                                                     unsigned parked_count);
/* The default non-reader table `evaluate` uses (T424 RVQ_NON_READER_NET_POLL_START, T593
 * RVQ_NON_READER_LOADER_START).
 * Returns the entries and stores their count in `*count`. */
const uint32_t *recomp_vblank_quiescence_default_non_readers(unsigned *count);

/* The checked form: snapshot the live thread table, evaluate, and on a failed predicate stop the
 * host with HOST_STOP_XDK_UNIMPLEMENTED naming the thread and state (never returns then). A
 * no-op returning the verdict while the check is not enabled. */
void recomp_vblank_quiescence_configure(bool enabled);
bool recomp_vblank_quiescence_enabled(void);
rvq_verdict recomp_vblank_quiescence_check(uint32_t deliverer, uint32_t site);
/* T592: `check` with caller-proven parked handles, see `evaluate_parked`. */
rvq_verdict recomp_vblank_quiescence_check_parked(uint32_t deliverer, uint32_t site,
                                                  const uint32_t *parked_handles,
                                                  unsigned parked_count);

/* Reader census: a thread reaching the getter must be a registered reader at its registered
 * return address (owner start routine at 0x153975, worker start routine at 0x156CB6). Returns
 * true for a registered reader. */
bool recomp_vblank_quiescence_is_registered_reader(uint32_t return_address, uint32_t start_routine);

/* The checked census: with the check enabled, a thread without a record or an unregistered reader
 * reaching the getter stops the host (HOST_STOP_XDK_UNIMPLEMENTED at the getter, never returns).
 * A no-op while the check is off. */
void recomp_vblank_quiescence_check_getter(uint32_t handle, uint32_t return_address);
/* How many getter polls the enabled check has examined (the trace prints it, so a boot proves the
 * census was wired). */
unsigned recomp_vblank_quiescence_getter_checks(void);

/* Text of one census line for tracing: "member=0x.. start=0x.. state=name". */
void recomp_vblank_quiescence_describe(const kernel_thread_record *record, char *out, unsigned size);
#endif
