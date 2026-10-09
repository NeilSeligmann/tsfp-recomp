/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_RECOMP_SECOND_VBLANK_H
#define TSFP_RECOMP_SECOND_VBLANK_H
#include "d3d8_first_vblank.h"
/* Trusted pure host query: verify this retained producer's complete launch,
 * control/FS identity and genuine confirmed termination. Called without locks.
 * Configuration, guest mappings/content and teardown are quiescent. This policy
 * admits only startup plus ONE credited second callback; it is not IRQ timing.
 * Notes accept trusted host identities, never guest pointer lists. Successful
 * pre-first waits are discarded; false waits and unsupported notes poison the
 * enabled epoch. A reserved second attempt is consumed even on refusal.
 * Same-byte permission probes ARE guest writes. Mappings/content must remain
 * quiescent through callback cleanup; unexpected faults can retain count/callback
 * guest writes, and no retry/rollback is claimed. No locks span guest execution.
 * Error notes/polls require an armed host stop target on the calling thread. */
typedef bool (*recomp_second_vblank_producer_confirmed)(uint32_t handle, uint32_t fs);
bool recomp_second_vblank_configure(bool enabled, d3d8_first_vblank_invoker invoker,
                                    uint32_t low, uint32_t high,
                                    recomp_second_vblank_producer_confirmed confirmed);
bool recomp_second_vblank_reset(void);
void recomp_second_vblank_bind_owner(uint32_t handle, uint32_t fs, uint32_t start, uint32_t system);
void recomp_second_vblank_note_registration(uint32_t callback, uint32_t handle, uint32_t fs);
void recomp_second_vblank_note_wait_completed(bool success, uint32_t handle, uint32_t fs);
void recomp_second_vblank_poll(uint32_t callee, uint32_t handle, uint32_t fs, uint32_t esp, uint32_t irql);
typedef struct recomp_second_vblank_snapshot {
    uint64_t epoch, credits;
    uint32_t owner_handle, owner_fs, producer_handle, producer_fs;
    uint32_t owner_budget, owner_delivered; /* T460 */
    uint32_t poll_delivered; /* T696: blanks delivered from a frame wait poll (counted in owner_delivered too) */
    uint32_t frames_admitted; /* T549: later frame waits that ran through with nothing to wait for */
    uint32_t worker_budget, worker_delivered; /* T592 */
    uint32_t worker_held, worker_final_held; /* T592: waits held for the owner to reach the gate, held after the last blank */
    uint32_t owner_gate_entries; /* T592: entries of the loading bar gate seen on the owner */
    bool enabled, bound, registered, first_delivered, second_attempted, second_delivered, inflight, refused;
    bool second_via_worker; /* Interactive terminal startup-worker credit delivery, not a new blank. */
} recomp_second_vblank_snapshot;
/* T460, default off: after the second event the OWNER's own completed waits each deliver one callback
 * from inside the wait (docs/vblank-delivery.md, "T460 results"), up to `budget` of them, then a named
 * stop. 0 turns it off. Needs an enabled policy, the coupled vblank effects and the quiescence check
 * (refused at the first wait otherwise), and refuses before any write on every unmeasured state.
 * Configure first (configure clears it).
 *
 * T549: with the budget on and the second event delivered, a LATER entry of the frame wait 0x1538C0 is
 * admitted without a delivery when its own exit test already holds (counter 0x563918 minus the counter
 * its previous exit stored at 0x7A58B0 is at least 1), counted in `frames_admitted`. One that would
 * spin refuses with a named stop. Without the budget a third entry is still refused as before. */
#define RECOMP_SECOND_VBLANK_OWNER_WAITS_MAX 1000000u
bool recomp_second_vblank_set_owner_waits(uint32_t budget);
/* T908: opt-in continuation; window owns wall pacing. Interactive startup also consumes
 * an existing second-event credit at producer GETTER156CB6 when the owner is blocked
 * on that exact worker's termination, counter/previous1 and stop1/gate1/running0.
 * The registered callback advances the counter; no new blank or clock advance.
 * FABRICATED delivery sequencing, identity/PCR/device/parked-reader checks retained. */
bool recomp_second_vblank_set_interactive(bool enabled);
void recomp_second_vblank_set_shutdown_query(bool (*query)(void));
/* T592, default off: the loading bar worker (start 0x156CB0, any thread after the credited producer) gets
 * one blank per completed wait of its own, up to `budget` of them, then a named stop. The blank is the T460
 * delivery (the coupled helper effects of that wait, then the callback from inside the wait) with a
 * different deliverer, made only while the owner is PROVEN not to read the counter: parked by location in
 * the loading bar gate (see `recomp_second_vblank_note_call`) or blocked on the worker's termination. A
 * wait with the owner elsewhere HOLDS the worker idle in the wait (bounded, `set_worker_hold_ms`) until the
 * owner arrives, then a wait whose frame satisfied the title's own gate (1.0f > progress is false) is held
 * until the owner has blocked on the worker, so the worker's stop test is deterministic. Needs the owner
 * waits (refused otherwise), configure first (configure clears it). */
#define RECOMP_SECOND_VBLANK_WORKER_BLANKS_MAX 1000000u
bool recomp_second_vblank_set_worker_blanks(uint32_t budget);
/* T696, default off, owner decision (option a): with the owner waits on, a frame wait poll whose exit test fails
 * with the counter EQUAL to the last exit (one blank owed) delivers that one blank from inside the poll through
 * the owner-wait delivery (same budget, same refusals). Any other failing state stays the named stop. FABRICATED
 * timing. Needs the owner waits (refused otherwise), configure first (configure clears it). */
bool recomp_second_vblank_set_poll_blank(bool enabled);
void recomp_second_vblank_set_worker_hold_ms(uint32_t milliseconds);
/* T592: called at every call boundary of every guest thread with the callee and the thread's handle,
 * before the callee runs. A no-op unless the worker budget is on. On the owner it tracks the loading bar
 * gate: the call of 0x156840 entered with [0x4E7A94] == 0 and [0x74A9AC] != 0 puts the owner in the gate (the proof in
 * `python -m tools.tracegaps gateregion`: the only call before its next blocking wait is 0x3800BF and no
 * path reads the counter), the call of 0x3800BF from the gate and any later call leave it. With a worker
 * epoch running (a blank delivered, the worker not terminated) an owner call to the counter getter or an
 * unproven call out of the gate is a named stop, so a wrong proof fails by name. */
void recomp_second_vblank_note_call(uint32_t callee, uint32_t handle);
void recomp_second_vblank_get_snapshot(recomp_second_vblank_snapshot *out);
#endif
