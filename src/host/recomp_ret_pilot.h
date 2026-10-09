/* SPDX-License-Identifier: GPL-3.0-or-later
 * Isolated guest-ret-pilot-v2. No production register or dispatch symbols.
 */
#ifndef TSFP_RECOMP_RET_PILOT_H
#define TSFP_RECOMP_RET_PILOT_H
#include <stddef.h>
#include <stdint.h>

#define RET_PILOT_ABI 2u
#define RET_PILOT_LAYOUT 2u
enum ret_pilot_register { RP_EAX, RP_ECX, RP_EDX, RP_EBX, RP_ESP, RP_EBP, RP_ESI, RP_EDI };
enum ret_pilot_result { RP_STOP = 1, RP_HANDOFF, RP_BUDGET, RP_REJECT, RP_FRAME_LIMIT };

/* All modeled machine state is explicit, including state untouched by this
 * integer-only graph. x87 uses the host runtime's double model, not raw80. */
typedef struct {
    uint32_t r[8], eip, eflags, fs_base, seh_ebp;
    uint32_t flag_bridge, flag_bridge_mask;
    uint64_t mmx[8];
    uint32_t xmm[8][4];
    double x87[8];
    uint32_t x87_top, x87_control, x87_compare, x87_condition;
} ret_pilot_machine;

/* Storage remains caller-owned and live until close. Tokens reject stale or
 * foreign-thread use, not arbitrary dangling C pointers (outside this API). */
typedef struct {
    uint64_t owner, generation;
    uint32_t return_slot, expected_resume, active;
} ret_pilot_frame;

typedef struct {
    uint32_t abi, layout, machine_size, frame_size, context_size;
    uint32_t view_abi, page_size, view_size;
    char identity[32], image_sha256[65], source_sha256[65], build_sha256[65];
} ret_pilot_profile;

/* Exclusive caller-owned immutable view while executing. Guest permissions are
 * independent of host backing permissions. Host readable code and accessible
 * data backing must agree with this view; dangling pointers/races are invalid.
 * generation changes require rebinding; code_generation must match generation.
 * Actual instruction bytes are also compared with the pinned image every step. */
#define RET_PILOT_VIEW_ABI 2u
#define RP_STATE_INTEGER 1u
#define RP_STATE_X87 2u
#define RP_STATE_MMX 4u
#define RP_STATE_SIMD 8u
#define RP_STATE_SEGMENT 16u
#define RP_STATE_DEBUG 32u
#define RP_PERM_READ 1u
#define RP_PERM_WRITE 2u
#define RP_PERM_EXEC 4u
#define RP_INTEGER_EFLAGS 0xed7u

typedef struct {
    uint32_t address, permissions;
    uint64_t generation, code_generation;
} ret_pilot_page;

typedef struct {
    uint32_t abi, size, required_state, reserved;
    uint64_t generation;
    const ret_pilot_page *pages;
    size_t page_count;
    char image_sha256[65];
} ret_pilot_view;

enum ret_pilot_handoff_reason {
    RP_REASON_NONE, RP_REASON_UNKNOWN, RP_REASON_VIEW, RP_REASON_STATE,
    RP_REASON_FETCH, RP_REASON_STALE_CODE, RP_REASON_CHANGED_CODE, RP_REASON_MEMORY
};

typedef struct {
    ret_pilot_machine machine;
    ret_pilot_frame *frames;
    size_t capacity, depth;
    uintptr_t memory_offset;
    uint64_t owner, generation, completed;
    uint32_t live, stop_va, fast_returns, redirected_returns;
    const ret_pilot_view *view;
    uint64_t view_generation;
    uint32_t handoff_reason;
} ret_pilot_context;

const ret_pilot_profile *ret_pilot_compiled_profile(void);
int ret_pilot_open(ret_pilot_context *ctx, const ret_pilot_profile *profile,
                   uintptr_t offset, ret_pilot_frame *frames, size_t capacity,
                   uint32_t stop_va);
/* Bind/rebind without executing guest instructions. Missing/unqualified views
 * cause run() HANDOFF; they never manufacture a guest exception. */
int ret_pilot_bind_view(ret_pilot_context *ctx, const ret_pilot_view *view);
int ret_pilot_close(ret_pilot_context *ctx);
int ret_pilot_run(ret_pilot_context *ctx, const ret_pilot_profile *profile, uint32_t budget);
/* Single architectural CALL/RET operations for independent boundary tests.
 * These low-level test operations do not qualify fetch/view capabilities.
 * run() qualifies each approved instruction before any architectural effect.
 * A genuine memory fault escapes normally to the caller's own crash scope. */
int ret_pilot_call(ret_pilot_context *ctx, uint32_t target, uint32_t next_va);
int ret_pilot_ret(ret_pilot_context *ctx, uint16_t immediate);
#endif
