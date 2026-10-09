/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Subject-side callee stub. See call_stub.h for the contract and
 * call_stub_shim.h for how the interception is wired.
 *
 * The table is populated per case from the STUB protocol lines, which carry the
 * SAME (callee VA -> pop_bytes) pairs the oracle computed. Both sides therefore
 * stub the same callees with the same stack discipline by construction, rather
 * than by each independently deciding what a callee does.
 *
 * A call to a VA that is NOT in the table during a strict run is the one
 * failure that must never be quiet: the oracle would have stubbed it and the
 * subject would have run the real callee, so any comparison afterwards is
 * meaningless. It is recorded and the driver turns it into `FAULT STUB-MISS`,
 * which voids the case. Voiding a case is honest; comparing it is not.
 */
#define _GNU_SOURCE
#include "call_stub.h"
#include "stackprobe_contract.h"
#include <stddef.h>

#include <stdint.h>
#include <string.h>

/* Register mirrors owned by the lifted runtime. `esp`, `eax`, `ecx` and `edx`
 * are macro aliases for these globals in the generated code, so writing them
 * here is immediately visible to the calling lifted function -- unlike ebx/esi/
 * edi/ebp, which the generated code keeps in C locals and which the stub must
 * therefore leave untouched to count as callee-saved. */
extern __thread uint32_t g_eax, g_ecx, g_edx, g_esp;

/* Opt-in EFLAGS publication (T16). A stubbed callee publishes nothing, so a
 * caller that EXITS through a stubbed tail call must read as unpublished
 * rather than as whatever an earlier ret in this case published. Clearing the
 * mask here is overwritten again by any later publishing ret in the caller,
 * so an ordinary mid-body stubbed call costs nothing. */
extern __thread uint32_t g_harness_eflags_mask;

/* Power of two: the index wrap below is a mask, not a modulo. 1024 slots is far
 * more than the distinct callees of any single function under test. */
#define STUB_SLOTS 1024u
#define STUB_MASK (STUB_SLOTS - 1u)

typedef struct {
    uint32_t va;
    uint32_t pop_bytes;
    uint32_t caller_va;
    uint8_t used;
    uint8_t passthrough;
} stub_slot_t;

static stub_slot_t g_slots[STUB_SLOTS];
static uint32_t g_slot_count;

/* 0 = stubbing off, and every call runs the real callee exactly as before. The
 * harness only turns this on for functions it selected as call-bearing, so the
 * pre-existing call-free population is byte-for-byte unaffected. */
int g_stub_strict;

/* Counted and reported, never inferred. `applied` doubles as a reachability
 * signal: a stubbed function whose call was never applied did not reach its
 * call site on that case. */
uint32_t g_stub_applied;
uint32_t g_stub_misses;
uint32_t g_stub_miss_va;
uint32_t g_passthrough_applied, g_passthrough_fault;

static uint32_t slot_for(uint32_t va)
{
    uint32_t i = (va * 2654435761u) & STUB_MASK;
    for (uint32_t probe = 0; probe < STUB_SLOTS; probe++) {
        if (!g_slots[i].used || g_slots[i].va == va) return i;
        i = (i + 1u) & STUB_MASK;
    }
    return STUB_SLOTS; /* full; caller treats as not-found */
}

void harness_stub_reset(void)
{
    memset(g_slots, 0, sizeof g_slots);
    g_slot_count = 0;
    g_stub_strict = 0;
    g_stub_applied = 0;
    g_stub_misses = 0;
    g_stub_miss_va = 0;
    g_passthrough_applied = g_passthrough_fault = 0;
}

/* Returns 0 on success, -1 if the table is full (reported, never ignored). */
int harness_stub_add(uint32_t va, uint32_t pop_bytes)
{
    if (va == HARNESS_STACKPROBE_VA) return -1;
    uint32_t i = slot_for(va);
    if (i >= STUB_SLOTS) return -1;
    if (!g_slots[i].used) {
        g_slots[i].used = 1;
        g_slots[i].va = va;
        g_slot_count++;
    }
    g_slots[i].pop_bytes = pop_bytes;
    g_slots[i].passthrough = 0;
    return 0;
}

int harness_stackprobe_supported(void) { return HARNESS_STACKPROBE_APPROVED; }

int harness_passthrough_add(uint32_t va, uint32_t caller_va)
{
    if (!HARNESS_STACKPROBE_APPROVED || va != HARNESS_STACKPROBE_VA ||
        (caller_va != 0x00337BD0u && caller_va != 0x0038FE80u)) return -1;
    uint32_t i = slot_for(va);
    if (i >= STUB_SLOTS) return -1;
    if (!g_slots[i].used) {
        g_slots[i].used = 1;
        g_slots[i].va = va;
        g_slot_count++;
    }
    g_slots[i].passthrough = 1;
    g_slots[i].caller_va = caller_va;
    return 0;
}

uint32_t harness_stub_count(void) { return g_slot_count; }

void harness_stub_call(uint32_t va, void (*fn)(void))
{
    if (!g_stub_strict) {
        fn();
        return;
    }

    uint32_t i = slot_for(va);
    if (i >= STUB_SLOTS || !g_slots[i].used) {
        /* Deliberately does NOT fall back to fn(): running the real callee here
         * is precisely the asymmetry that would make the verdict a lie. Record
         * it and let the driver void the case. */
        if (!g_stub_misses) g_stub_miss_va = va;
        g_stub_misses++;
        return;
    }

    if (g_slots[i].passthrough) {
#if HARNESS_STACKPROBE_APPROVED
        extern ptrdiff_t g_xbox_mem_offset;
        extern void (*recomp_lookup(uint32_t))(void);
        const uint8_t *code = (const uint8_t *)(g_xbox_mem_offset + (uintptr_t)va);
        /* Only a planned direct call to this proved entry can execute it. SAFE
         * calls carrying NULL, a different native target, or mutated bytes fail. */
        const uint8_t *caller = (const uint8_t *)(g_xbox_mem_offset + (uintptr_t)g_slots[i].caller_va);
        if (fn && fn == recomp_lookup(va) && harness_stackprobe_original_proven(code) &&
            harness_stackprobe_caller_proven(g_slots[i].caller_va, caller)) {
            g_passthrough_applied++;
            fn();
            return;
        }
#endif
        g_passthrough_fault = 1;
        return;
    }

    /* The contract, in full. The caller already pushed the 4-byte guest return
     * address into guest memory and that store stays there, matching the store
     * the hardware performs; what the callee's `ret` would have done is undo the
     * stack adjustment and additionally pop its stdcall arguments. */
    g_esp += 4u + g_slots[i].pop_bytes;
    g_eax = HARNESS_STUB_EAX;
    g_ecx = HARNESS_STUB_ECX;
    g_edx = HARNESS_STUB_EDX;
    g_harness_eflags_mask = 0; /* a stub publishes no flags; see above */
    g_stub_applied++;
}
