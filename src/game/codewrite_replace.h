/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The code-write replacement ABI (T1508). A replacement of a root whose ORIGINAL rewrites its own
 * (or its callee's) code bytes when it is handed a hostile object pointer cannot be a plain
 * transcription: the original's observable behaviour then includes executing the rewritten bytes
 * (an AAM chain, a read fault, an INT3 or ICEBP stop). This header gives such a root the same
 * shape as the opt-in `faithful-codewrite-arbiter-v2` proof contract (tools/replace) names:
 *
 *   1. A STORE PLAN. The readable body lists the stores the original performs, in program order
 *      (the object fields, the pushes and the call return slots), without touching memory.
 *   2. THE GUARD. `codewrite_plan_clean` proves, before anything is written, that no store leaves
 *      the mapped window, overlaps the live bytes of the root or its callee, or aliases the
 *      incoming stack frame (the return slot the callee comes back through). Only then the plan
 *      is committed (`codewrite_plan_commit`) and the root returns its register outputs.
 *   3. THE EXACT FALLBACK. Any other input runs `codewrite_run_original`: a byte-accurate
 *      interpreter that FETCHES THE LIVE BYTES of the original from guest memory and executes
 *      them, so a store that rewrites not-yet-executed code is executed as rewritten (the x86
 *      self-modifying code rule: a store never changes the instruction already being executed).
 *      A fault ends the call with `codewrite_stop`, never with a repaired state.
 *
 * The interpreter is deliberately small: exactly the instruction subset the covered roots and
 * their callees use, plus ADD r/m8,r8 (what zero bytes decode to after the root zeroes its own
 * continuation), AAM, DEC, INT3 and ICEBP (what the rewritten bytes decode to). An opcode outside
 * the subset is a refusal (`codewrite_stop` kind CODEWRITE_UNSUPPORTED), never a guess.
 *
 * WHAT IS NOT REPRODUCED, stated because the proof depends on it: EFLAGS. No instruction of the
 * subset reads a flag, so the interpreter does not compute them; callers are audited for flag
 * use after the call (tools.replace audit). The registers are exact on every exit path.
 *
 * FAULTS. The original would raise #PF, #BP or #UD. The recompiled runtime has no guest
 * exception delivery, so the replacement stops (trap) with the state the original had when the
 * exception was raised. Proof builds (TSFP_CODEWRITE_PROOF) publish that state through
 * `harness_codewrite_stop` so the proof compares it with the arbiter's.
 *
 * WINDOW. The mapped guest range. Proof builds use the harness window (16 MiB); production uses
 * the 64 MiB retail RAM range (INFERRED, the interpreter only reads and writes inside it).
 * A store is atomic: an access that is not entirely inside the window faults and commits nothing
 * (the architectural page-crossing rule; Unicorn commits ascending bytes, a measured deviation).
 *
 * Touches no shared header: it only includes game_replace.h.
 */
#ifndef TSFP_GAME_CODEWRITE_REPLACE_H
#define TSFP_GAME_CODEWRITE_REPLACE_H

#include <stdint.h>

#include "game_replace.h"

extern __thread uint32_t g_ebx, g_ebp, g_edi;

#define CODEWRITE_WINDOW_LO 0x00010000u
#if defined(TSFP_CODEWRITE_PROOF)
#define CODEWRITE_WINDOW_HI 0x01000000u
#else
#define CODEWRITE_WINDOW_HI 0x04000000u
#endif
/* An original that has not returned after this many instructions is refused. */
#define CODEWRITE_MAX_STEPS 256u
/* Bytes below the entry esp and at it that belong to the stack frame the callee returns through. */
#define CODEWRITE_FRAME_BELOW 16u
#define CODEWRITE_FRAME_BYTES 20u
#define CODEWRITE_PLAN_MAX 16u

/* Stop kinds: the letters the proof publishes. */
enum {
    CODEWRITE_DONE = 0,
    CODEWRITE_FETCH_FAULT = 'F',
    CODEWRITE_READ_FAULT = 'R',
    CODEWRITE_WRITE_FAULT = 'W',
    CODEWRITE_BREAKPOINT = 'X',
    CODEWRITE_ICEBP = 'I',
    CODEWRITE_UNSUPPORTED = 'U'
};

#if defined(TSFP_CODEWRITE_PROOF)
/* tools/harness/driver.c (HARNESS_CODEWRITE): publishes kind, eip, step count and the state the
 * registers and memory are in right now, then ends the case. Does not return. */
void harness_codewrite_stop(unsigned kind, uint32_t eip, uint32_t steps) __attribute__((noreturn));
#endif

static inline void codewrite_stop(unsigned kind, uint32_t eip, uint32_t steps)
{
#if defined(TSFP_CODEWRITE_PROOF)
    harness_codewrite_stop(kind, eip, steps);
#else
    (void)kind;
    (void)eip;
    (void)steps;
    __builtin_trap();
#endif
    __builtin_trap();
}

/* The authenticated bytes of one original function. */
typedef struct codewrite_code {
    uint32_t va;
    uint32_t size;
    const uint8_t *bytes;
} codewrite_code;

/* A root and the one callee it calls. */
typedef struct codewrite_original {
    codewrite_code root;
    codewrite_code callee;
} codewrite_original;

enum { CW_EAX, CW_ECX, CW_EDX, CW_EBX, CW_ESP, CW_EBP, CW_ESI, CW_EDI };

static inline void codewrite_load_registers(uint32_t reg[8])
{
    reg[CW_EAX] = g_eax;
    reg[CW_ECX] = g_ecx;
    reg[CW_EDX] = g_edx;
    reg[CW_EBX] = g_ebx;
    reg[CW_ESP] = g_esp;
    reg[CW_EBP] = g_ebp;
    reg[CW_ESI] = g_esi;
    reg[CW_EDI] = g_edi;
}

static inline void codewrite_store_registers(const uint32_t reg[8])
{
    g_eax = reg[CW_EAX];
    g_ecx = reg[CW_ECX];
    g_edx = reg[CW_EDX];
    g_ebx = reg[CW_EBX];
    g_esp = reg[CW_ESP];
    g_ebp = reg[CW_EBP];
    g_esi = reg[CW_ESI];
    g_edi = reg[CW_EDI];
}

/* ---- bounds-checked guest memory: 0 on success, 1 when not entirely inside the window ---- */

static inline int codewrite_inside(uint32_t address, uint32_t width)
{
    return address >= CODEWRITE_WINDOW_LO && (uint64_t)address + width <= CODEWRITE_WINDOW_HI;
}

static inline int codewrite_read(uint32_t address, uint32_t width, uint32_t *value)
{
    if (!codewrite_inside(address, width)) {
        return 1;
    }
    uint32_t result = 0u;
    for (uint32_t i = 0u; i < width; i++) {
        result |= (uint32_t)guest_read8(address + i) << (8u * i);
    }
    *value = result;
    return 0;
}

static inline int codewrite_write(uint32_t address, uint32_t value, uint32_t width)
{
    if (!codewrite_inside(address, width)) {
        return 1;
    }
    for (uint32_t i = 0u; i < width; i++) {
        guest_write8(address + i, (uint8_t)(value >> (8u * i)));
    }
    return 0;
}

/* The live bytes of the original are still the authenticated ones. */
static inline int codewrite_intact(const codewrite_code *code)
{
    for (uint32_t i = 0u; i < code->size; i++) {
        if (!codewrite_inside(code->va + i, 1u) || guest_read8(code->va + i) != code->bytes[i]) {
            return 0;
        }
    }
    return 1;
}

/* ---- the store plan and its guard ---- */

typedef struct codewrite_store {
    uint32_t address;
    uint32_t value;
    uint32_t width;
    int on_stack; /* a push or call return slot: part of the frame, exempt from the alias guard */
} codewrite_store;

typedef struct codewrite_plan {
    codewrite_store store[CODEWRITE_PLAN_MAX];
    unsigned count;
} codewrite_plan;

static inline void codewrite_plan_add(codewrite_plan *plan, uint32_t address, uint32_t value,
                                      uint32_t width, int on_stack)
{
    if (plan->count >= CODEWRITE_PLAN_MAX) {
        __builtin_trap();
    }
    plan->store[plan->count].address = address;
    plan->store[plan->count].value = value;
    plan->store[plan->count].width = width;
    plan->store[plan->count].on_stack = on_stack;
    plan->count++;
}

static inline void codewrite_plan_push(codewrite_plan *plan, uint32_t address, uint32_t value)
{
    codewrite_plan_add(plan, address, value, 4u, 1);
}

static inline void codewrite_plan_field(codewrite_plan *plan, uint32_t address, uint32_t value,
                                        uint32_t width)
{
    codewrite_plan_add(plan, address, value, width, 0);
}

static inline int codewrite_overlap(uint32_t address, uint32_t width, uint32_t low, uint32_t size)
{
    return (uint64_t)address < (uint64_t)low + size && (uint64_t)low < (uint64_t)address + width;
}

/* True when committing the plan cannot reach the window edge, the live bytes of the root or
 * callee, or (for object stores) the frame [esp-16, esp+4) the callee returns through. */
static inline int codewrite_plan_clean(const codewrite_plan *plan, const codewrite_original *original,
                                       uint32_t entry_esp)
{
    if (!codewrite_inside(entry_esp, 4u)) { /* the return slot must be readable to take the fast path */
        return 0;
    }
    for (unsigned i = 0u; i < plan->count; i++) {
        const codewrite_store *item = &plan->store[i];
        if (!codewrite_inside(item->address, item->width)) {
            return 0;
        }
        if (codewrite_overlap(item->address, item->width, original->root.va, original->root.size) ||
            codewrite_overlap(item->address, item->width, original->callee.va,
                              original->callee.size)) {
            return 0;
        }
        if (!item->on_stack &&
            codewrite_overlap(item->address, item->width, entry_esp - CODEWRITE_FRAME_BELOW,
                              CODEWRITE_FRAME_BYTES)) {
            return 0;
        }
    }
    return 1;
}

static inline void codewrite_plan_commit(const codewrite_plan *plan)
{
    for (unsigned i = 0u; i < plan->count; i++) {
        if (codewrite_write(plan->store[i].address, plan->store[i].value, plan->store[i].width)) {
            __builtin_trap(); /* unreachable after a clean guard */
        }
    }
}

/* ---- the exact interpreter ---- */

typedef struct codewrite_outcome {
    unsigned kind;
    uint32_t eip;
    uint32_t steps;
} codewrite_outcome;

static inline uint8_t codewrite_get8(const uint32_t reg[8], unsigned index)
{
    return index < 4u ? (uint8_t)reg[index] : (uint8_t)(reg[index - 4u] >> 8);
}

static inline void codewrite_set8(uint32_t reg[8], unsigned index, uint8_t value)
{
    if (index < 4u) {
        reg[index] = (reg[index] & ~0xFFu) | value;
    } else {
        reg[index - 4u] = (reg[index - 4u] & ~0xFF00u) | ((uint32_t)value << 8);
    }
}

/* Effective address of a ModRM operand without SIB: [reg], [disp32], [reg+disp8], [reg+disp32].
 * `length` grows by the displacement. 0 ok, 1 unsupported form, 2 fetch fault. */
static inline int codewrite_effective_address(const uint32_t reg[8], uint32_t eip, unsigned *length,
                                              uint8_t modrm, uint32_t *address)
{
    unsigned mod = (unsigned)modrm >> 6, rm = (unsigned)modrm & 7u;
    unsigned count = mod == 1u ? 1u : ((mod == 2u || (mod == 0u && rm == 5u)) ? 4u : 0u);
    uint32_t displacement = 0u;
    if (rm == 4u || mod == 3u) {
        return 1;
    }
    for (unsigned i = 0u; i < count; i++) {
        uint32_t byte;
        if (codewrite_read(eip + *length + i, 1u, &byte)) {
            return 2;
        }
        displacement |= byte << (8u * i);
    }
    *length += count;
    if (count == 1u) {
        displacement = (uint32_t)(int32_t)(int8_t)(uint8_t)displacement;
    }
    *address = (mod == 0u && rm == 5u) ? displacement : reg[rm] + displacement;
    return 0;
}

static inline int codewrite_fetch_imm32(uint32_t eip, unsigned at, uint32_t *value)
{
    return codewrite_read(eip + at, 4u, value);
}

/* Executes the live bytes from `entry` until control reaches `sentinel`. Registers are read from
 * and written back to `reg`; memory effects stay committed up to the stop. */
static inline codewrite_outcome codewrite_interpret(uint32_t reg[8], uint32_t entry,
                                                    uint32_t sentinel)
{
    uint32_t eip = entry;
    uint32_t steps = 0u;
#define CW_STOP(code) \
    do { codewrite_outcome stopped = {(code), eip, steps}; return stopped; } while (0)
#define CW_UNSUPPORTED() CW_STOP(CODEWRITE_UNSUPPORTED)
#define CW_FAULT_IF(failed, code) do { if (failed) { CW_STOP(code); } } while (0)
    while (eip != sentinel) {
        uint32_t opcode, modrm_word, address, value;
        if (steps >= CODEWRITE_MAX_STEPS) {
            CW_UNSUPPORTED();
        }
        CW_FAULT_IF(codewrite_read(eip, 1u, &opcode), CODEWRITE_FETCH_FAULT);
        if (opcode >= 0x50u && opcode <= 0x57u) { /* push r32: value read before esp changes */
            CW_FAULT_IF(codewrite_write(reg[CW_ESP] - 4u, reg[opcode - 0x50u], 4u),
                        CODEWRITE_WRITE_FAULT);
            reg[CW_ESP] -= 4u;
            eip += 1u;
        } else if (opcode >= 0x58u && opcode <= 0x5Fu) { /* pop r32: esp first, so pop esp loads */
            CW_FAULT_IF(codewrite_read(reg[CW_ESP], 4u, &value), CODEWRITE_READ_FAULT);
            reg[CW_ESP] += 4u;
            reg[opcode - 0x58u] = value;
            eip += 1u;
        } else if (opcode == 0x8Bu || opcode == 0x8Du || opcode == 0xC7u || opcode == 0x00u ||
                   opcode == 0x33u || opcode == 0x88u || opcode == 0x89u) {
            unsigned length = 2u;
            unsigned mod, field, rm;
            int status;
            CW_FAULT_IF(codewrite_read(eip + 1u, 1u, &modrm_word), CODEWRITE_FETCH_FAULT);
            mod = (unsigned)(modrm_word >> 6);
            field = (unsigned)(modrm_word >> 3) & 7u;
            rm = (unsigned)modrm_word & 7u;
            if (opcode == 0x8Bu) { /* mov r32, r/m32 */
                if (mod == 3u) {
                    reg[field] = reg[rm];
                } else {
                    status = codewrite_effective_address(reg, eip, &length, (uint8_t)modrm_word,
                                                         &address);
                    if (status == 1) { CW_UNSUPPORTED(); }
                    CW_FAULT_IF(status, CODEWRITE_FETCH_FAULT);
                    CW_FAULT_IF(codewrite_read(address, 4u, &value), CODEWRITE_READ_FAULT);
                    reg[field] = value;
                }
            } else if (opcode == 0x89u) { /* mov r/m32, r32 */
                if (mod == 3u) {
                    reg[rm] = reg[field];
                } else {
                    status = codewrite_effective_address(reg, eip, &length, (uint8_t)modrm_word,
                                                         &address);
                    if (status == 1) { CW_UNSUPPORTED(); }
                    CW_FAULT_IF(status, CODEWRITE_FETCH_FAULT);
                    CW_FAULT_IF(codewrite_write(address, reg[field], 4u), CODEWRITE_WRITE_FAULT);
                }
            } else if (opcode == 0x88u) { /* mov r/m8, r8 */
                if (mod == 3u) {
                    codewrite_set8(reg, rm, codewrite_get8(reg, field));
                } else {
                    status = codewrite_effective_address(reg, eip, &length, (uint8_t)modrm_word,
                                                         &address);
                    if (status == 1) { CW_UNSUPPORTED(); }
                    CW_FAULT_IF(status, CODEWRITE_FETCH_FAULT);
                    CW_FAULT_IF(codewrite_write(address, codewrite_get8(reg, field), 1u),
                                CODEWRITE_WRITE_FAULT);
                }
            } else if (opcode == 0x33u) { /* xor r32, r/m32 (flags not reproduced) */
                uint32_t source;
                if (mod == 3u) {
                    source = reg[rm];
                } else {
                    status = codewrite_effective_address(reg, eip, &length, (uint8_t)modrm_word,
                                                         &address);
                    if (status == 1) { CW_UNSUPPORTED(); }
                    CW_FAULT_IF(status, CODEWRITE_FETCH_FAULT);
                    CW_FAULT_IF(codewrite_read(address, 4u, &source), CODEWRITE_READ_FAULT);
                }
                reg[field] ^= source;
            } else if (opcode == 0x8Du) { /* lea r32, m: any form this subset cannot form is refused */
                if (mod == 3u || codewrite_effective_address(reg, eip, &length, (uint8_t)modrm_word,
                                                             &address) != 0) {
                    CW_UNSUPPORTED();
                }
                reg[field] = address;
            } else if (opcode == 0xC7u) { /* mov r/m32, imm32 */
                if (field != 0u) { CW_UNSUPPORTED(); }
                address = 0u;
                if (mod != 3u) {
                    status = codewrite_effective_address(reg, eip, &length, (uint8_t)modrm_word,
                                                         &address);
                    if (status == 1) { CW_UNSUPPORTED(); }
                    CW_FAULT_IF(status, CODEWRITE_FETCH_FAULT);
                }
                CW_FAULT_IF(codewrite_fetch_imm32(eip, length, &value), CODEWRITE_FETCH_FAULT);
                length += 4u;
                if (mod == 3u) {
                    reg[rm] = value;
                } else {
                    CW_FAULT_IF(codewrite_write(address, value, 4u), CODEWRITE_WRITE_FAULT);
                }
            } else { /* 00: add r/m8, r8 (flags not reproduced) */
                uint32_t left;
                uint32_t right = codewrite_get8(reg, field);
                if (mod == 3u) {
                    left = codewrite_get8(reg, rm);
                } else {
                    status = codewrite_effective_address(reg, eip, &length, (uint8_t)modrm_word,
                                                         &address);
                    if (status == 1) { CW_UNSUPPORTED(); }
                    CW_FAULT_IF(status, CODEWRITE_FETCH_FAULT);
                    CW_FAULT_IF(codewrite_read(address, 1u, &left), CODEWRITE_READ_FAULT);
                }
                value = (left + right) & 0xFFu;
                if (mod == 3u) {
                    codewrite_set8(reg, rm, (uint8_t)value);
                } else {
                    CW_FAULT_IF(codewrite_write(address, value, 1u), CODEWRITE_WRITE_FAULT);
                }
            }
            eip += length;
        } else if (opcode == 0xE8u) { /* call rel32 */
            CW_FAULT_IF(codewrite_fetch_imm32(eip, 1u, &value), CODEWRITE_FETCH_FAULT);
            CW_FAULT_IF(codewrite_write(reg[CW_ESP] - 4u, eip + 5u, 4u), CODEWRITE_WRITE_FAULT);
            reg[CW_ESP] -= 4u;
            eip = eip + 5u + value;
        } else if (opcode == 0xC3u) { /* ret */
            CW_FAULT_IF(codewrite_read(reg[CW_ESP], 4u, &value), CODEWRITE_READ_FAULT);
            reg[CW_ESP] += 4u;
            eip = value;
        } else if (opcode >= 0xB8u && opcode <= 0xBFu) { /* mov r32, imm32 */
            CW_FAULT_IF(codewrite_fetch_imm32(eip, 1u, &value), CODEWRITE_FETCH_FAULT);
            reg[opcode - 0xB8u] = value;
            eip += 5u;
        } else if (opcode >= 0x48u && opcode <= 0x4Fu) { /* dec r32 (flags not reproduced) */
            reg[opcode - 0x48u] -= 1u;
            eip += 1u;
        } else if (opcode == 0xD4u) { /* aam imm8: ah = al / imm8, al = al % imm8 */
            CW_FAULT_IF(codewrite_read(eip + 1u, 1u, &modrm_word), CODEWRITE_FETCH_FAULT);
            if (modrm_word == 0u) { CW_UNSUPPORTED(); } /* #DE is not modelled */
            value = (uint8_t)reg[CW_EAX];
            codewrite_set8(reg, 4u, (uint8_t)(value / modrm_word));
            codewrite_set8(reg, 0u, (uint8_t)(value % modrm_word));
            eip += 2u;
        } else if (opcode == 0xF1u) { /* icebp: stops at the instruction (the arbiter's rule) */
            CW_STOP(CODEWRITE_ICEBP);
        } else if (opcode == 0xCCu) { /* int3: the trap saves the NEXT instruction */
            eip += 1u;
            CW_STOP(CODEWRITE_BREAKPOINT);
        } else {
            CW_UNSUPPORTED();
        }
        steps++;
    }
    {
        codewrite_outcome done = {CODEWRITE_DONE, eip, steps};
        return done;
    }
#undef CW_FAULT_IF
#undef CW_UNSUPPORTED
#undef CW_STOP
}

/* The exact fallback of a root. Runs the live original from its entry to its return address and
 * leaves the registers as the original would, with esp ONE SLOT LOW so the adapter's own pop of
 * the return slot lands where the original's `ret` did. A fault or refusal ends the call. */
static inline void codewrite_run_original(const codewrite_original *original, uint32_t sentinel)
{
    uint32_t reg[8];
    codewrite_load_registers(reg);
    codewrite_outcome outcome = codewrite_interpret(reg, original->root.va, sentinel);
    codewrite_store_registers(reg);
    if (outcome.kind != CODEWRITE_DONE) {
        codewrite_stop(outcome.kind, outcome.eip, outcome.steps);
    }
    g_esp -= 4u;
}

/* Entry check shared by every covered root: the original bytes are the authenticated ones. A
 * changed byte is a refusal, not a different function. */
static inline void codewrite_require_intact(const codewrite_original *original)
{
    if (!codewrite_intact(&original->root) || !codewrite_intact(&original->callee)) {
        codewrite_stop(CODEWRITE_UNSUPPORTED, original->root.va, 0u);
    }
}

/* The return address the original's `ret` returns to (the fallback runs until it is reached). */
static inline uint32_t codewrite_return_address(void)
{
    uint32_t value = 0u;
    return codewrite_read(g_esp, 4u, &value) ? 0u : value;
}

#endif /* TSFP_GAME_CODEWRITE_REPLACE_H */
