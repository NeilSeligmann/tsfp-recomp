/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Transactional fast paths for code-write roots with pointer-chasing bodies (T1508, second
 * admission pass: 003C97AB, 0040F720, 003C2260 and the bit packers). The constructor-family
 * roots of codewrite_replace.h write a fixed, precomputable set of stores, so a plan can be
 * validated before anything is written. These roots read and write through caller pointers whose
 * values steer later accesses (a bit reader's cursor lives in memory the same call stores to),
 * so a plan cannot be built in advance. A transaction does the work instead:
 *
 *   1. The body is a plain C transliteration of the original that performs every guest load and
 *      store, in the original's order, through cw_tx_read/cw_tx_write. Aliasing between the
 *      caller's buffers is therefore exact for free.
 *   2. Every access is checked. A load or store that leaves the mapped window, a store that
 *      overlaps the live bytes of the root or of any callee, a load or store that touches the
 *      incoming stack frame region (the dead stack the original itself pushes to, and the
 *      argument slots it re-reads) or an iteration/undo budget overrun marks the transaction
 *      BAD. Stores are logged byte by byte before they are applied.
 *   3. A BAD transaction is rolled back (undo log, newest first) and the root runs the original's
 *      LIVE bytes through the exact interpreter (codewrite_x86.h) from the pristine entry state.
 *      Nothing about a faulting, self-modifying or stack-aliasing input is guessed.
 *   4. A clean transaction replays the original's own dead-stack stores (kept in a local shadow
 *      of the frame, see cw_frame) and sets the exact register outputs.
 *
 * The original bytes (root plus every callee) are authenticated on every entry: a changed byte is
 * a refusal, not a different function.
 *
 * Not reproduced: EFLAGS (callers audited), see codewrite_x86.h for the fallback.
 */
#ifndef TSFP_GAME_CODEWRITE_TX_H
#define TSFP_GAME_CODEWRITE_TX_H

#include "codewrite_x86.h"

extern __thread int g_df;

/* An authenticated byte range of the original (root or a callee). */
typedef struct codewrite_span {
    uint32_t va;
    uint32_t size;
    const uint8_t *bytes;
} codewrite_span;

typedef struct codewrite_set {
    const codewrite_span *span;
    unsigned count;
    uint32_t root;
    unsigned frame_below; /* bytes below the entry esp the original may push to */
    unsigned arg_bytes;   /* stack argument bytes above the return slot (protected from data access) */
    unsigned pop_bytes;   /* bytes the original's own `ret` pops beyond the return address */
    uint32_t max_steps;
} codewrite_set;

static inline int codewrite_set_intact(const codewrite_set *set)
{
    for (unsigned i = 0u; i < set->count; i++) {
        for (uint32_t k = 0u; k < set->span[i].size; k++) {
            const uint32_t address = set->span[i].va + k;
            if (!codewrite_inside(address, 1u) || guest_read8(address) != set->span[i].bytes[k]) {
                return 0;
            }
        }
    }
    return 1;
}

static inline void codewrite_set_require_intact(const codewrite_set *set)
{
    if (!codewrite_set_intact(set)) {
        codewrite_stop(CODEWRITE_UNSUPPORTED, set->root, 0u);
    }
}

/* The exact fallback of a transactional root: runs the live original from its entry to the
 * return address. `pop_bytes` is what the adapter pops after the body (4 + 4N). */
static inline void codewrite_run_general(const codewrite_set *set, uint32_t sentinel)
{
    uint32_t reg[8];
    uint32_t eflags = 0x202u | (g_df ? CW86_DF : 0u);
    codewrite_load_registers(reg);
    codewrite_outcome outcome = cw86_run(reg, &eflags, set->root, sentinel, set->max_steps);
    codewrite_store_registers(reg);
    if (outcome.kind != CODEWRITE_DONE) {
        codewrite_stop(outcome.kind, outcome.eip, outcome.steps);
    }
    g_esp -= 4u + set->pop_bytes; /* the adapter pops the return slot and the popped arguments */
}

/* ---- the transaction ---- */

#define CW_TX_UNDO 2048u

typedef struct cw_undo {
    uint32_t address;
    uint8_t old;
} cw_undo;

typedef struct cw_tx {
    const codewrite_set *set;
    uint32_t entry_esp;
    int bad;
    unsigned used;
    uint64_t steps; /* upper bound of the original instructions executed so far */
    cw_undo undo[CW_TX_UNDO];
} cw_tx;

static inline void cw_tx_begin(cw_tx *tx, const codewrite_set *set)
{
    tx->set = set;
    tx->entry_esp = g_esp;
    tx->bad = 0;
    tx->used = 0u;
    tx->steps = 0u;
}

/* Accounts an upper bound of original instructions. A body that would exceed the step budget of
 * the exact interpreter is left to it, so the budget stop (kind B) is the interpreter's. */
static inline void cw_tx_charge(cw_tx *tx, uint32_t instructions)
{
    tx->steps += instructions;
    if (tx->steps > tx->set->max_steps) {
        tx->bad = 1;
    }
}

static inline int cw_tx_in_frame(const cw_tx *tx, uint32_t address, uint32_t width)
{
    return codewrite_overlap(address, width, tx->entry_esp - tx->set->frame_below,
                             tx->set->frame_below + 4u + tx->set->arg_bytes);
}

static inline int cw_tx_in_code(const cw_tx *tx, uint32_t address, uint32_t width)
{
    for (unsigned i = 0u; i < tx->set->count; i++) {
        if (codewrite_overlap(address, width, tx->set->span[i].va, tx->set->span[i].size)) {
            return 1;
        }
    }
    return 0;
}

/* A data load through a caller-influenced pointer. */
static inline uint32_t cw_tx_read(cw_tx *tx, uint32_t address, uint32_t width)
{
    uint32_t value = 0u;
    if (tx->bad) {
        return 0u;
    }
    if (cw_tx_in_frame(tx, address, width) || codewrite_read(address, width, &value)) {
        tx->bad = 1;
        return 0u;
    }
    return value;
}

/* A stack argument (index from 0), read before any store. */
static inline uint32_t cw_tx_arg(cw_tx *tx, unsigned index)
{
    uint32_t value = 0u;
    if (tx->bad || codewrite_read(tx->entry_esp + 4u + 4u * index, 4u, &value)) {
        tx->bad = 1;
        return 0u;
    }
    return value;
}

/* A data store through a caller-influenced pointer, logged for rollback. */
static inline void cw_tx_write(cw_tx *tx, uint32_t address, uint32_t value, uint32_t width)
{
    if (tx->bad) {
        return;
    }
    if (!codewrite_inside(address, width) || cw_tx_in_frame(tx, address, width) ||
        cw_tx_in_code(tx, address, width) || tx->used + width > CW_TX_UNDO) {
        tx->bad = 1;
        return;
    }
    for (uint32_t i = 0u; i < width; i++) {
        tx->undo[tx->used].address = address + i;
        tx->undo[tx->used].old = guest_read8(address + i);
        tx->used++;
    }
    (void)codewrite_write(address, value, width);
}

static inline void cw_tx_rollback(cw_tx *tx)
{
    while (tx->used > 0u) {
        tx->used--;
        guest_write8(tx->undo[tx->used].address, tx->undo[tx->used].old);
    }
}

/* The original's own stack stores: the dead stack below the entry esp and the return slot and
 * argument area above it (some roots write a byte into one of their own argument slots). The
 * shadow holds the bytes of [entry_esp - frame_below, entry_esp + 4 + arg_bytes). Stores are
 * stated relative to the entry esp: cw_frame_store takes the distance BELOW it, as the pushes
 * do, and cw_frame_store_rel a signed offset. */
#define CW_FRAME_MAX 320u

typedef struct cw_frame {
    uint32_t base; /* entry_esp - frame_below */
    uint32_t entry_esp;
    unsigned size; /* frame_below + 4 + arg_bytes */
    uint8_t byte[CW_FRAME_MAX];
} cw_frame;

static inline void cw_frame_begin(cw_frame *frame, const cw_tx *tx)
{
    frame->base = tx->entry_esp - tx->set->frame_below;
    frame->entry_esp = tx->entry_esp;
    frame->size = tx->set->frame_below + 4u + tx->set->arg_bytes;
    if (frame->size > CW_FRAME_MAX || !codewrite_inside(frame->base, frame->size)) {
        frame->size = 0u;
        return;
    }
    for (unsigned i = 0u; i < frame->size; i++) {
        frame->byte[i] = guest_read8(frame->base + i);
    }
}

static inline void cw_frame_store_rel(cw_frame *frame, int32_t rel, uint32_t value, unsigned width)
{
    const int64_t offset = (int64_t)frame->entry_esp + rel - (int64_t)frame->base;
    if (frame->size == 0u) {
        return; /* the commit refuses an unusable frame */
    }
    if (offset < 0 || offset + width > frame->size) {
        __builtin_trap(); /* a transliteration bug, never reachable from a guest input */
    }
    for (unsigned i = 0u; i < width; i++) {
        frame->byte[offset + i] = (uint8_t)(value >> (8u * i));
    }
}

static inline void cw_frame_store(cw_frame *frame, uint32_t below, uint32_t value, unsigned width)
{
    cw_frame_store_rel(frame, -(int32_t)below, value, width);
}

/* Commits the shadow to guest memory. Returns 0 when the frame cannot be written cleanly
 * (outside the window or over live code), in which case nothing was written. */
static inline int cw_frame_commit(const cw_tx *tx, const cw_frame *frame)
{
    if (frame->size == 0u || !codewrite_inside(frame->base, frame->size) ||
        cw_tx_in_code(tx, frame->base, frame->size)) {
        return 0;
    }
    for (unsigned i = 0u; i < frame->size; i++) {
        guest_write8(frame->base + i, frame->byte[i]);
    }
    return 1;
}

/* Ends a body: a clean transaction commits the frame; anything else rolls back. Returns 1 when
 * the fast path stands. The caller sets registers only after a 1. */
#if defined(TSFP_CODEWRITE_COUNT)
extern unsigned cw_tx_accepted_count; /* native tests only: transactions that stood */
#endif

static inline int cw_tx_finish(cw_tx *tx, const cw_frame *frame)
{
    if (tx->bad || !cw_frame_commit(tx, frame)) {
        cw_tx_rollback(tx);
        return 0;
    }
#if defined(TSFP_CODEWRITE_COUNT)
    cw_tx_accepted_count++;
#endif
    return 1;
}

#endif /* TSFP_GAME_CODEWRITE_TX_H */
