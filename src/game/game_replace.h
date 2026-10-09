/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Registering a hand-written function as the replacement for a lifted one.
 *
 * WHAT ONE FILE DOES
 * ------------------
 * A file in src/game/ holds a readable C function and one registration line:
 *
 *     static uint32_t game_pick_larger(uint32_t first, uint32_t second) { ... }
 *     GAME_REPLACE(00012345, cdecl, 2, u32, game_pick_larger)
 *
 * The line expands to three things, all in the same translation unit:
 *
 *   1. A strong definition of `sub_00012345`, the symbol the lifter gave the original.
 *      It is the ADAPTER: it reads the guest's stack arguments, calls the readable
 *      function, writes the result to eax, and pops what the original's `ret` popped.
 *   2. A registry record (virtual address, name, convention, argument count, source)
 *      placed in the `game_replacements` linker section. The record is the ONLY thing
 *      the coverage metric and the differential harness believe: a replacement that is
 *      not in the registry of a linked binary does not exist.
 *   3. A call counter, so a run can say how often the replacement actually executed.
 *
 * HOW IT DISPLACES THE LIFTED BODY (no re-lift, no runtime redirect)
 * ------------------------------------------------------------------
 * The lifted chunks call each other by symbol (`sub_00012345()`), so a runtime lookup
 * hook is never consulted for a direct call. The lifted definition is instead made WEAK
 * at compile time by `#pragma weak sub_00012345` in a force-included header that
 * tools/replace generates, and this file's strong definition wins at link time for every
 * caller, including callers in the same chunk, and for the dispatch table. See
 * docs/decompilation-workflow.md.
 *
 * THE FOUR CONVENTIONS, and what the adapter does for each. `N` is the number of 32-bit
 * stack arguments the original takes, which MUST be the number its own `ret` pops.
 *
 *   cdecl     args on the stack, caller pops:  esp += 4
 *   stdcall   args on the stack, callee pops:  esp += 4 + 4N
 *   thiscall  `this` in ecx, N on the stack, callee pops:  esp += 4 + 4N
 *   fastcall  ecx and edx, N on the stack, callee pops:    esp += 4 + 4N
 *
 * The readable function receives register arguments first (ecx, then edx) and then the
 * stack arguments in order, all as uint32_t. It returns uint32_t (the adapter stores it
 * in eax) or nothing (`void`, eax is then scratch).
 *
 * WHAT IS NOT PRESERVED, stated here because the harness depends on it: ecx and edx are
 * caller-saved under all four conventions and eax is too when the function returns
 * nothing. The adapter leaves them alone. Equivalence is therefore judged modulo those
 * registers, and a function whose original leaves a value in one of them that a caller
 * reads is NOT eligible for this generic adapter. tools/replace audits direct call sites.
 * GAME_REPLACE_EXACT supplies a register-exact body and excludes no registers instead.
 */
#ifndef TSFP_GAME_REPLACE_H
#define TSFP_GAME_REPLACE_H

#include <stddef.h>
#include <stdint.h>

#include "game_guest.h"

/* The lifter's architectural registers (src/host/recomp_runtime.c, or the harness
 * runtime). Thread-local: each guest thread has its own. */
extern __thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_esi;
/* The fs segment base (`XBOX_FS_BASE` in the lifted code). */
extern __thread uint32_t g_fs_base;

typedef enum game_convention {
    GAME_CC_cdecl = 0,
    GAME_CC_stdcall = 1,
    GAME_CC_thiscall = 2,
    GAME_CC_fastcall = 3
} game_convention;

/* A replacement may need to invoke another lifted guest function. This is a real
 * nested guest call: arguments live on the guest stack, register arguments use
 * ECX/EDX, and the target's adapter (if registered) is reached through the same
 * dispatch table as an ordinary lifted call. The caller supplies the retail
 * return VA and the number of non-argument dwords already reserved below its
 * current ESP at that call site; stack_args themselves are placed by the helper. */
typedef enum game_guest_call_status {
    GAME_GUEST_CALL_OK = 0,
    GAME_GUEST_CALL_NOT_FOUND,
    GAME_GUEST_CALL_BAD_FRAME,
    GAME_GUEST_CALL_BAD_STACK
} game_guest_call_status;

typedef void (*game_guest_function)(void);

#if defined(__GNUC__) || defined(__clang__)
extern game_guest_function recomp_lookup(uint32_t xbox_va) __attribute__((weak));
#else
extern game_guest_function recomp_lookup(uint32_t xbox_va);
#endif

#define GAME_GUEST_CALL_MAX_STACK_ARGS 8u

/* Invoke a guest function from replacement C. `caller_stack_bytes` counts only
 * local/temporary dwords below the current ESP, not the stack arguments below.
 * `register_ecx` is for thiscall/fastcall and `register_edx` is for fastcall.
 * Those guest registers are left untouched for cdecl/stdcall. On success the callee's
 * register and memory effects remain visible and ESP is restored to its entry
 * value. A cdecl caller cleanup is performed here. On a stack-contract error,
 * the mismatched ESP is deliberately retained so the enclosing replacement
 * cannot hide the defect by continuing with a repaired frame. */
static inline game_guest_call_status game_guest_call(
    uint32_t target_va, game_convention convention, uint32_t return_va,
    uint32_t caller_stack_bytes, uint32_t register_ecx, uint32_t register_edx,
    const uint32_t *stack_args, unsigned stack_arg_count)
{
    if (stack_arg_count > GAME_GUEST_CALL_MAX_STACK_ARGS ||
        (stack_arg_count != 0u && stack_args == NULL) ||
        (caller_stack_bytes & 3u) != 0u || (int)convention < (int)GAME_CC_cdecl ||
        convention > GAME_CC_fastcall) {
        return GAME_GUEST_CALL_BAD_FRAME;
    }
    if (recomp_lookup == NULL) {
        return GAME_GUEST_CALL_NOT_FOUND;
    }
    game_guest_function function = recomp_lookup(target_va);
    if (function == NULL) {
        return GAME_GUEST_CALL_NOT_FOUND;
    }

    const uint32_t entry_esp = g_esp;
    const uint32_t frame_bytes = 4u * (stack_arg_count + 1u);
    if (entry_esp < caller_stack_bytes || entry_esp - caller_stack_bytes < frame_bytes) {
        return GAME_GUEST_CALL_BAD_FRAME;
    }
    const uint32_t call_top = entry_esp - caller_stack_bytes;
    const uint32_t callee_esp = call_top - frame_bytes;
    guest_write32(callee_esp, return_va);
    for (unsigned i = 0u; i < stack_arg_count; i++) {
        guest_write32(callee_esp + 4u + 4u * i, stack_args[i]);
    }

    if (convention == GAME_CC_thiscall || convention == GAME_CC_fastcall) {
        g_ecx = register_ecx;
    }
    if (convention == GAME_CC_fastcall) {
        g_edx = register_edx;
    }
    g_esp = callee_esp;
    function();

    uint32_t expected_esp = callee_esp + 4u;
    if (convention != GAME_CC_cdecl) {
        expected_esp += 4u * stack_arg_count;
    }
    if (g_esp != expected_esp) {
        return GAME_GUEST_CALL_BAD_STACK;
    }
    if (convention == GAME_CC_cdecl) {
        g_esp += 4u * stack_arg_count;
    }
    if (g_esp != call_top) {
        return GAME_GUEST_CALL_BAD_STACK;
    }
    g_esp = entry_esp;
    return GAME_GUEST_CALL_OK;
}

typedef struct game_replacement {
    uint32_t va;              /* guest address of the original */
    const char *name;         /* the readable function */
    game_convention convention;
    uint8_t stack_args;       /* 32-bit stack arguments the original pops or the caller pushes */
    uint8_t returns_value;    /* 1 when the function returns a value in eax */
    uint8_t scratch_mask;     /* registers the adapter does not reproduce */
    uint8_t input_abi;        /* 0: legacy; 0x11/12/13: exact ECX/ESI/ECX+ESI */
    const char *source;       /* __FILE__ of the registration */
    void (*adapter)(void);    /* the strong sub_XXXXXXXX this record belongs to */
    uint64_t calls;           /* times the adapter ran in this process */
} game_replacement;

/* The versioned byte occupies the old padding, preserving section record stride. */
enum { GAME_INPUT_ecx = 0x11, GAME_INPUT_esi = 0x12, GAME_INPUT_ecx_esi = 0x13 };

static inline int game_replacement_valid(const game_replacement *entry)
{
    if (entry == NULL || (unsigned)entry->convention > GAME_CC_fastcall ||
        entry->returns_value > 1u || (entry->scratch_mask & ~7u) != 0u ||
        entry->name == NULL || entry->source == NULL || entry->adapter == NULL) {
        return 0;
    }
    if (entry->input_abi == 0u) return 1;
    return (entry->input_abi == GAME_INPUT_ecx || entry->input_abi == GAME_INPUT_esi ||
            entry->input_abi == GAME_INPUT_ecx_esi) && entry->scratch_mask == 0u &&
           entry->stack_args <= 8u &&
           (entry->convention == GAME_CC_cdecl || entry->convention == GAME_CC_stdcall);
}

static inline const char *game_replacement_input_names(const game_replacement *entry)
{
    switch (entry->input_abi) {
    case 0: return "";
    case GAME_INPUT_ecx: return "ecx";
    case GAME_INPUT_esi: return "esi";
    case GAME_INPUT_ecx_esi: return "ecx,esi";
    default: return NULL;
    }
}

/* Every registered replacement linked into this binary, in link order. */
const game_replacement *game_replacement_table(size_t *count);

/* Bits for the registers a replacement does NOT promise to match the original on. */
enum { GAME_SCRATCH_EAX = 1, GAME_SCRATCH_ECX = 2, GAME_SCRATCH_EDX = 4 };
unsigned game_replacement_scratch(const game_replacement *entry);

/* The i-th 32-bit stack argument. The return address is at [esp], so argument 0 is at
 * esp + 4. Uses the adapter's entry esp, which is untouched until the call returns. */
#define game_stack_arg(index) guest_read32(g_esp + 4u + 4u * (unsigned)(index))

#define GAME_STK_0
#define GAME_STK_1 game_stack_arg(0)
#define GAME_STK_2 GAME_STK_1, game_stack_arg(1)
#define GAME_STK_3 GAME_STK_2, game_stack_arg(2)
#define GAME_STK_4 GAME_STK_3, game_stack_arg(3)
#define GAME_STK_5 GAME_STK_4, game_stack_arg(4)
#define GAME_STK_6 GAME_STK_5, game_stack_arg(5)
#define GAME_STK_7 GAME_STK_6, game_stack_arg(6)
#define GAME_STK_8 GAME_STK_7, game_stack_arg(7)

/* The same list with a leading comma, for conventions that put registers first. */
#define GAME_STKC_0
#define GAME_STKC_1 , game_stack_arg(0)
#define GAME_STKC_2 GAME_STKC_1, game_stack_arg(1)
#define GAME_STKC_3 GAME_STKC_2, game_stack_arg(2)
#define GAME_STKC_4 GAME_STKC_3, game_stack_arg(3)
#define GAME_STKC_5 GAME_STKC_4, game_stack_arg(4)
#define GAME_STKC_6 GAME_STKC_5, game_stack_arg(5)
#define GAME_STKC_7 GAME_STKC_6, game_stack_arg(6)
#define GAME_STKC_8 GAME_STKC_7, game_stack_arg(7)

#define GAME_ARGS_cdecl(N) GAME_STK_##N
#define GAME_ARGS_stdcall(N) GAME_STK_##N
#define GAME_ARGS_thiscall(N) g_ecx GAME_STKC_##N
#define GAME_ARGS_fastcall(N) g_ecx, g_edx GAME_STKC_##N

/* Stack arguments the callee pops. cdecl leaves them for the caller. */
#define GAME_POPS_cdecl(N) 0u
#define GAME_POPS_stdcall(N) (N##u)
#define GAME_POPS_thiscall(N) (N##u)
#define GAME_POPS_fastcall(N) (N##u)

#define GAME_RESULT_u32(call) (g_eax = (uint32_t)(call))
#define GAME_RESULT_void(call) ((void)(call))
#define GAME_RETURNS_u32 1
#define GAME_RETURNS_void 0

#if defined(__has_attribute)
#if __has_attribute(retain)
#define GAME_RETAIN , retain
#endif
#endif
#ifndef GAME_RETAIN
#define GAME_RETAIN
#endif

/* `VA` is the eight hex digits of the original's address, UPPER CASE and without 0x,
 * because it is pasted into the symbol name the lifter used (`sub_0003C330`). A
 * lower-case digit would define a symbol nothing calls, which the harness catches by
 * checking that the dispatch table reaches this adapter. */
#define GAME_REPLACE(VA, CC, N, RET, FN)                                            \
    void sub_##VA(void);                                                            \
    static game_replacement game_entry_##VA                                         \
        __attribute__((section("game_replacements"), used GAME_RETAIN)) = {         \
            0x##VA, #FN, GAME_CC_##CC, N, GAME_RETURNS_##RET,                         \
            GAME_SCRATCH_ECX | GAME_SCRATCH_EDX |                                    \
                (GAME_RETURNS_##RET ? 0 : GAME_SCRATCH_EAX),                         \
            0u, __FILE__, sub_##VA, 0u}; \
    void sub_##VA(void)                                                             \
    {                                                                               \
        __atomic_fetch_add(&game_entry_##VA.calls, 1u, __ATOMIC_RELAXED);           \
        GAME_RESULT_##RET(FN(GAME_ARGS_##CC(N)));                                   \
        g_esp += 4u + 4u * GAME_POPS_##CC(N);                                       \
    }

/* A register-exact adapter supplies its body after this macro. The readable helper is
 * still the registry's name, while the body reproduces the original guest register
 * outputs as well as calling that helper. No registers are excluded by the harness.
 * The wrapper owns accounting and the return-address/argument pop for both variants. */
#define GAME_REPLACE_EXACT_IMPL(VA, CC, N, RET, FN, INPUT_ABI)                                      \
    static void game_body_##VA(void);                                               \
    void sub_##VA(void);                                                            \
    static game_replacement game_entry_##VA                                         \
        __attribute__((section("game_replacements"), used GAME_RETAIN)) = {          \
            0x##VA, #FN, GAME_CC_##CC, N, GAME_RETURNS_##RET, 0u, INPUT_ABI,          \
            __FILE__, sub_##VA, 0u};                                                \
    void sub_##VA(void)                                                             \
    {                                                                              \
        __atomic_fetch_add(&game_entry_##VA.calls, 1u, __ATOMIC_RELAXED);             \
        game_body_##VA();                                                           \
        g_esp += 4u + 4u * GAME_POPS_##CC(N);                                        \
    }                                                                              \
    static void game_body_##VA(void)

#define GAME_REPLACE_EXACT(VA, CC, N, RET, FN) \
    GAME_REPLACE_EXACT_IMPL(VA, CC, N, RET, FN, 0u)

/* Explicit inputs do not imply output preservation; the exact body owns all GPRs.
 * An original thiscall uses stdcall cleanup plus explicit ecx input here. */
#define GAME_REPLACE_EXACT_INPUTS(VA, CC, N, RET, INPUTS, FN) \
    _Static_assert(GAME_CC_##CC == GAME_CC_cdecl || GAME_CC_##CC == GAME_CC_stdcall, \
                   "explicit inputs require cdecl or stdcall cleanup"); \
    _Static_assert((N) >= 0 && (N) <= 8, "explicit input stack count must be 0..8"); \
    GAME_REPLACE_EXACT_IMPL(VA, CC, N, RET, FN, GAME_INPUT_##INPUTS)

#endif /* TSFP_GAME_REPLACE_H */
