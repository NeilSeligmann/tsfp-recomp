/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "recomp_ret_pilot.h"
#include <stdatomic.h>
#include <string.h>
#include "ret_pilot_profile_generated.h"

#if !defined(__x86_64__) && !defined(__i386__)
#error "guest-ret-pilot-v2 requires x86 word-access fault semantics"
#endif

static atomic_uint_fast64_t sequence = 1;
static _Thread_local uint64_t thread_owner;
static uint64_t owner(void)
{
    if (!thread_owner) thread_owner = atomic_fetch_add(&sequence, 1);
    return thread_owner;
}

/* One actual unaligned host word access, including cross-page protection
 * faults. A byte loop/memcpy store may partially commit unlike x86 MOV/PUSH.
 * memory clobbers also preserve ordered guest/TLS publication at O3. */
static uint32_t read_word(const ret_pilot_context *ctx, uint32_t address)
{
    uint32_t value;
    const void *pointer = (const void *)(ctx->memory_offset + (uintptr_t)address);
    __asm__ volatile("movl (%1), %0" : "=r"(value) : "r"(pointer) : "memory");
    return value;
}
static void write_word(const ret_pilot_context *ctx, uint32_t address, uint32_t value)
{
    void *pointer = (void *)(ctx->memory_offset + (uintptr_t)address);
    __asm__ volatile("movl %0, (%1)" : : "r"(value), "r"(pointer) : "memory");
}

static int valid(const ret_pilot_context *ctx)
{
    if (!ctx || !ctx->live || ctx->owner != owner() || !ctx->generation ||
        !ctx->frames || !ctx->capacity || ctx->depth > ctx->capacity) return 0;
    for (size_t i = 0; i < ctx->depth; ++i) {
        const ret_pilot_frame *frame = &ctx->frames[i];
        if (!frame->active || frame->owner != ctx->owner ||
            frame->generation != ctx->generation) return 0;
    }
    return 1;
}

static int profile_valid(const ret_pilot_profile *profile)
{
    /* Reject historical layouts before reading the larger v2 profile tail. */
    return profile && profile->abi == RET_PILOT_ABI && profile->layout == RET_PILOT_LAYOUT &&
        memcmp(profile, &generated_profile, sizeof *profile) == 0;
}

const ret_pilot_profile *ret_pilot_compiled_profile(void) { return &generated_profile; }

int ret_pilot_open(ret_pilot_context *ctx, const ret_pilot_profile *profile,
                   uintptr_t offset, ret_pilot_frame *frames, size_t capacity,
                   uint32_t stop_va)
{
    if (!ctx || ctx->live || !profile_valid(profile) || !frames || !capacity || capacity > 1024u) return RP_REJECT;
    for (size_t i = 0; i < capacity; ++i)
        if (frames[i].owner || frames[i].generation || frames[i].active) return RP_REJECT;
    /* Zero-initialized or previously closed objects only. Caller owns object
     * and frame storage lifetime, exclusive access and mapping permissions. */
    memset(ctx, 0, sizeof *ctx);
    memset(frames, 0, capacity * sizeof *frames);
    ctx->owner = owner();
    ctx->generation = atomic_fetch_add(&sequence, 1);
    ctx->frames = frames; ctx->capacity = capacity; ctx->memory_offset = offset;
    ctx->live = 1; ctx->stop_va = stop_va;
    for (size_t i = 0; i < capacity; ++i) {
        frames[i].owner = ctx->owner; frames[i].generation = ctx->generation;
    }
    return 0;
}

static int view_valid(const ret_pilot_view *view)
{
    if (!view || view->abi != RET_PILOT_VIEW_ABI || view->size != sizeof *view ||
        !view->generation || view->reserved || !view->pages ||
        memcmp(view->image_sha256, generated_profile.image_sha256, 65) ||
        !view->page_count || view->page_count > 1048576u) return 0;
    for (size_t i = 0; i < view->page_count; ++i) {
        const ret_pilot_page *p = &view->pages[i];
        if ((p->address & 4095u) || (p->permissions & ~7u) || !p->generation ||
            (i && view->pages[i - 1].address >= p->address)) return 0;
    }
    return 1;
}

int ret_pilot_bind_view(ret_pilot_context *ctx, const ret_pilot_view *view)
{
    if (!valid(ctx) || !view_valid(view)) return RP_REJECT;
    ctx->view = view; ctx->view_generation = view->generation;
    return 0;
}

static const ret_pilot_page *page_at(const ret_pilot_view *view, uint32_t address)
{
    uint32_t base = address & ~4095u;
    size_t first = 0, end = view->page_count;
    while (first < end) {
        size_t mid = first + (end - first) / 2;
        if (view->pages[mid].address < base) first = mid + 1;
        else end = mid;
    }
    return first < view->page_count && view->pages[first].address == base
        ? &view->pages[first] : NULL;
}

static int permitted(const ret_pilot_view *view, uint32_t address, unsigned size,
                      unsigned permission)
{
    /* Crossing 4GiB is not silently converted to a wrapped host word access. */
    if (address > UINT32_MAX - (size - 1u)) return 0;
    for (unsigned i = 0; i < size; ++i) {
        const ret_pilot_page *page = page_at(view, address + i);
        if (!page || (page->permissions & permission) != permission) return 0;
    }
    return 1;
}

static unsigned qualify(const ret_pilot_context *ctx)
{
    const ret_pilot_machine *m = &ctx->machine;
    const ret_pilot_view *view = ctx->view;
    if (!view_valid(view) || view->generation != ctx->view_generation) return RP_REASON_VIEW;
    if (view->required_state != RP_STATE_INTEGER || !(m->eflags & 2u) ||
        (m->eflags & ~RP_INTEGER_EFLAGS)) return RP_REASON_STATE;
    size_t index;
    for (index = 0; index < sizeof approved_code / sizeof approved_code[0]; ++index)
        if (approved_code[index].va == m->eip) break;
    if (index == sizeof approved_code / sizeof approved_code[0]) return RP_REASON_UNKNOWN;
    unsigned size = approved_code[index].size;
    if (!permitted(view, m->eip, size, RP_PERM_READ | RP_PERM_EXEC)) return RP_REASON_FETCH;
    for (unsigned i = 0; i < size; ++i) {
        const ret_pilot_page *page = page_at(view, m->eip + i);
        if (page->code_generation != page->generation) return RP_REASON_STALE_CODE;
        const volatile unsigned char *byte = (const volatile unsigned char *)
            (ctx->memory_offset + (uintptr_t)(m->eip + i));
        if (*byte != approved_code[index].bytes[i]) return RP_REASON_CHANGED_CODE;
    }
    uint32_t address = 0;
    unsigned permission = 0;
    switch (m->eip) {
    case 0x3e240: address = 0x6b8380; permission = RP_PERM_READ; break;
    case 0x3e245: address = 0x6b8388; permission = RP_PERM_READ; break;
    case 0x3e24f: address = 0x6b8384; permission = RP_PERM_READ | RP_PERM_WRITE; break;
    case 0x3e255: address = 0x6b8388; permission = RP_PERM_WRITE; break;
    case 0x3e7cc: address = 0x6b83ac; permission = RP_PERM_READ; break;
    case 0x3e7c7: case 0x12e79: case 0x3e7d5: case 0x3e7d7:
        address = m->r[RP_ESP] - 4u; permission = RP_PERM_WRITE; break;
    case 0x3e25a: case 0x29b15:
        address = m->r[RP_ESP]; permission = RP_PERM_READ; break;
    default: break;
    }
    if (permission && !permitted(view, address, 4, permission)) return RP_REASON_MEMORY;
    return RP_REASON_NONE;
}

static void retire(ret_pilot_context *ctx)
{
    while (ctx->depth) ctx->frames[--ctx->depth].active = 0;
}

int ret_pilot_close(ret_pilot_context *ctx)
{
    if (!valid(ctx)) return RP_REJECT;
    retire(ctx);
    memset(ctx->frames, 0, ctx->capacity * sizeof *ctx->frames);
    ctx->live = 0; ctx->generation = 0;
    ctx->view = NULL; ctx->view_generation = 0;
    return 0;
}

int ret_pilot_call(ret_pilot_context *ctx, uint32_t target, uint32_t next_va)
{
    if (!valid(ctx)) return RP_REJECT;
    if (ctx->depth == ctx->capacity) return RP_FRAME_LIMIT;
    uint32_t slot = ctx->machine.r[RP_ESP] - 4u;
    write_word(ctx, slot, next_va);
    ctx->machine.r[RP_ESP] = slot;
    ret_pilot_frame *frame = &ctx->frames[ctx->depth++];
    *frame = (ret_pilot_frame){ctx->owner, ctx->generation, slot, next_va, 1u};
    ctx->machine.eip = target;
    return 0;
}

int ret_pilot_ret(ret_pilot_context *ctx, uint16_t immediate)
{
    if (!valid(ctx)) return RP_REJECT;
    uint32_t slot = ctx->machine.r[RP_ESP];
    uint32_t target = read_word(ctx, slot);
    /* The read must complete before pop/EIP/frame changes. Target execution
     * belongs to the loop or unexecuted handoff, never a speculative check. */
    ctx->machine.r[RP_ESP] = slot + 4u + (uint32_t)immediate;
    ctx->machine.eip = target;
    if (ctx->depth && ctx->frames[ctx->depth - 1].return_slot == slot) {
        ret_pilot_frame *frame = &ctx->frames[--ctx->depth];
        if (frame->expected_resume == target) ++ctx->fast_returns;
        else ++ctx->redirected_returns;
        frame->active = 0;
    } else {
        /* A foreign stack invalidates tracking, not guest control flow. No
         * guest state is restored from a frame and targets are unrestricted. */
        retire(ctx); ++ctx->redirected_returns;
    }
    return 0;
}

static uint32_t arithmetic_flags(uint32_t flags, uint32_t a, uint32_t b,
                                 uint32_t result, int subtract)
{
    uint32_t bits = 0, low = result & 255u;
    if (subtract ? a < b : result < a) bits |= 1u;
    low ^= low >> 4; low ^= low >> 2; low ^= low >> 1;
    if (!(low & 1u)) bits |= 4u;
    if ((a ^ b ^ result) & 16u) bits |= 16u;
    if (!result) bits |= 64u;
    if (result & 0x80000000u) bits |= 128u;
    uint32_t overflow = subtract ? (a ^ b) & (a ^ result) : ~(a ^ b) & (a ^ result);
    if (overflow & 0x80000000u) bits |= 2048u;
    return (flags & ~0x8d5u) | bits;
}

static void push(ret_pilot_context *ctx, uint32_t value)
{
    uint32_t next = ctx->machine.r[RP_ESP] - 4u;
    write_word(ctx, next, value);
    ctx->machine.r[RP_ESP] = next;
}

int ret_pilot_run(ret_pilot_context *ctx, const ret_pilot_profile *profile, uint32_t budget)
{
    if (!valid(ctx) || !profile_valid(profile))
        return RP_REJECT;
    ret_pilot_machine *m = &ctx->machine;
    for (uint32_t step = 0; step < budget; ++step) {
        if (m->eip == ctx->stop_va) return RP_STOP;
        ctx->handoff_reason = qualify(ctx);
        if (ctx->handoff_reason) return RP_HANDOFF;
        uint32_t value, result;
        int status;
        switch (m->eip) {
        case 0x3e7c7:
            status = ret_pilot_call(ctx, 0x3e240u, 0x3e7ccu);
            if (status) return status;
            break;
        case 0x3e240: m->r[RP_EAX] = read_word(ctx, 0x6b8380u); m->eip = 0x3e245; break;
        case 0x3e245: m->r[RP_EDX] = read_word(ctx, 0x6b8388u); m->eip = 0x3e24b; break;
        case 0x3e24b: m->r[RP_ECX] = m->r[RP_EAX]; m->eip = 0x3e24d; break;
        case 0x3e24d:
            value = m->r[RP_ECX]; result = value - m->r[RP_EDX];
            m->eflags = arithmetic_flags(m->eflags, value, m->r[RP_EDX], result, 1);
            m->r[RP_ECX] = result; m->eip = 0x3e24f; break;
        case 0x3e24f:
            value = read_word(ctx, 0x6b8384u); result = value + m->r[RP_ECX];
            write_word(ctx, 0x6b8384u, result);
            m->eflags = arithmetic_flags(m->eflags, value, m->r[RP_ECX], result, 0);
            m->eip = 0x3e255; break;
        case 0x3e255: write_word(ctx, 0x6b8388u, m->r[RP_EAX]); m->eip = 0x3e25a; break;
        case 0x3e25a:
            status = ret_pilot_ret(ctx, 0);
            if (status) return status;
            break;
        case 0x3e7cc: m->r[RP_EDX] = read_word(ctx, 0x6b83acu); m->eip = 0x3e7d2; break;
        case 0x3e7d2: m->r[RP_EAX] = m->r[RP_EDX] + m->r[RP_EDX]; m->eip = 0x3e7d5; break;
        case 0x3e7d5: push(ctx, 0); m->eip = 0x3e7d7; break;
        case 0x3e7d7: push(ctx, m->r[RP_EAX]); m->eip = 0x3e7d8; break;
        case 0x12e79:
            status = ret_pilot_call(ctx, m->r[RP_EAX], 0x12e7bu);
            if (status) return status;
            break;
        case 0x29b15:
            status = ret_pilot_ret(ctx, 8);
            if (status) return status;
            break;
        default: return RP_HANDOFF;
        }
        ++ctx->completed;
    }
    return m->eip == ctx->stop_va ? RP_STOP : RP_BUDGET;
}
