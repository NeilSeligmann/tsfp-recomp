/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See d3d8_pushbuffer.h for what is modelled, what is replaced and why.
 */

#include "d3d8_pushbuffer.h"

#include "d3d8_gpu.h"
#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "kernel_call.h"

#include <string.h>

/* MmAllocateContiguousMemoryEx and the arguments the library's allocation wrapper (0x00380C53)
 * forms for the flag word 0xBC800000: 4 KiB alignment, protect 0x404 (read-write plus
 * write-combine), no physical window. MEASURED by running the original under the oracle, which
 * logged (size, alignment 0x1000, protect 0x404) for the pushbuffer. */
#define KERNEL_ORDINAL_MM_ALLOCATE_CONTIGUOUS_EX 166u
#define PUSHBUFFER_ALIGNMENT 0x1000u
#define PUSHBUFFER_PROTECT 0x404u

#define DEV_PUSHBUFFER_MODE 0x002Cu

static d3d8_pushbuffer_consumer consumer_fn;
static void *consumer_context;
static d3d8_pushbuffer_refill_tail refill_tail_fn;
static d3d8_pushbuffer_fence_reached fence_reached_fn;
static uint32_t put_address;
static uint32_t segment_start;
static uint64_t dwords_written;
static uint64_t rollover_count;

void d3d8_pushbuffer_set_consumer(d3d8_pushbuffer_consumer consumer, void *context)
{
    consumer_fn = consumer;
    consumer_context = context;
}

void d3d8_pushbuffer_set_refill_tail(d3d8_pushbuffer_refill_tail tail)
{
    refill_tail_fn = tail;
}

void d3d8_pushbuffer_set_fence_reached(d3d8_pushbuffer_fence_reached reached)
{
    fence_reached_fn = reached;
}

uint32_t d3d8_pushbuffer_put(void)
{
    return put_address;
}

void d3d8_pushbuffer_set_put(uint32_t put)
{
    put_address = put;
}

void d3d8_pushbuffer_reset(void)
{
    consumer_fn = NULL;
    consumer_context = NULL;
    refill_tail_fn = NULL;
    fence_reached_fn = NULL;
    put_address = 0u;
    segment_start = 0u;
    dwords_written = 0u;
    rollover_count = 0u;
}

uint64_t d3d8_pushbuffer_dwords_written(void)
{
    return dwords_written;
}

uint64_t d3d8_pushbuffer_rollovers(void)
{
    return rollover_count;
}

bool d3d8_pushbuffer_create(void)
{
    const uint32_t size = d3d8_guest_load32(D3D8_GLOBAL_PUSHBUFFER_SIZE);
    const uint32_t kickoff = d3d8_guest_load32(D3D8_GLOBAL_KICKOFF_SIZE);

    const uint32_t args[5] = {size, 0u, 0xFFFFFFFFu, PUSHBUFFER_ALIGNMENT, PUSHBUFFER_PROTECT};
    const uint32_t base = d3d8_kernel_call(KERNEL_ORDINAL_MM_ALLOCATE_CONTIGUOUS_EX, args, 5u);
    if (base == 0u) {
        return false;
    }

    /* 0x003D6375 to 0x003D63A5. The sizes are rounded down to a dword, as `shr ecx, 2` does. */
    d3d8_device_store32(D3D8_DEV_PB_BASE, base);
    d3d8_device_store32(D3D8_DEV_PB_END, base + (size & ~3u));
    d3d8_device_store32(D3D8_DEV_CURSOR, base);
    d3d8_device_store32(D3D8_DEV_LIMIT, base + (kickoff & ~3u) - D3D8_PUSHBUFFER_SLACK_BYTES);
    d3d8_device_store32(DEV_PUSHBUFFER_MODE, 5u);
    segment_start = base;
    put_address = base;
    return true;
}

/*
 * The no-contention path of 0x003D69E0(half, kickoff). 0x003D6B20 enters it with (kickoff/2,
 * kickoff) and the sized reservation 0x003D6B30 with larger values for a big request.
 *
 * The candidate end of the next segment is cursor + kickoff. If that comes within 0x4000 of
 * the ring's end it is clamped to the end when `half` still fits, and otherwise the ring wraps:
 * the library writes a jump to the ring's base at the cursor (the base's low 28 bits plus one,
 * an NV2A old-style jump), counts the wrap, remembers how far the cursor had got, and restarts
 * at the base. The limit is the candidate end less the slack.
 *
 * Dropped, because it is the GPU: the spin while the region about to be reused is still queued
 * (REFUSED by name instead, see below) and the DMA_PUT write that starts the GPU on what was just
 * filled (the kick, which hands the range to the consumer).
 *
 * T391 ported the rest of the refill, MEASURED against the original under the instantaneous-GPU
 * oracle (tests/test_d3d8_refill_oracle.py, equal on every device dword and every ring dword):
 *   - 0x003D6530 at the entry stores GET (the last kick's put, in the contiguous window) at
 *     device+0x5C and the wrap generation `wrap count - (cursor < GET)` at +0x60. A GET outside the
 *     ring takes a branch that reads the GPU's own register file, which nothing models, so it is fatal;
 *   - the end (0x003D6AE0) is the GPU module's `refill_tail`: a fence inserted with flags 3 plus the
 *     kick, or with device flag 0x800 the flag 0x1000 plus the kick. The fence writes its eight
 *     dwords at the new cursor, so a refill leaves the cursor 0x20 bytes further on, and the previews
 *     account for it;
 *   - when GET lies in the area about to be reused (cursor < GET <= segment end, after the wrap
 *     adjustments of 0x003D6A62) the original waits (0x003D6A8E to 0x003D6AD4, T486). It walks the second
 *     fence history backwards from its cursor for the fence whose predecessor's ring position (shifted by
 *     the wrap distance when it lies below the new cursor) is within HALF THE RING (the global at
 *     0x003E6404, not the kickoff size) of the new cursor, stopping at the history's start or at the
 *     first fence the GPU has already passed, and calls 0x003D6870(fence, 8). The instantaneous GPU has
 *     passed every fence it was kicked with, and a fence it has passed makes 0x003D6870 return before it
 *     writes anything (MEASURED), so the wait is the search alone and has no effect: the search is only
 *     run to refuse by name, before any write, the fence that has NOT been passed (see
 *     d3d8_pushbuffer_fence_reached). That is INFERRED unreachable outside a transplanted state, and
 *     there the original spins forever (fence near GET) or leaves stall patches in the ring (MEASURED).
 */

typedef struct {
    uint32_t cursor;
    uint32_t limit;
    bool wrapped;
    uint32_t get_shadow;
    uint32_t get_generation;
} refill_plan;

typedef d3d8_pushbuffer_refill_state refill_state;

/* What a refill reads that the refills before it moved (T526), as the device holds it now. */
static refill_state refill_state_now(void)
{
    refill_state state;
    memset(&state, 0, sizeof(state));
    state.loaded = true;
    state.put = put_address;
    state.wrap_count = d3d8_device_load32(D3D8_DEV_WRAP_COUNT);
    state.wrap_delta = d3d8_device_load32(D3D8_DEV_WRAP_DELTA);
    state.fence_counter = d3d8_device_load32(D3D8_DEV_FENCE);
    state.history_cursor = d3d8_device_load32(D3D8_DEV_HISTORY_CURSOR);
    return state;
}

static uint32_t state_semaphore(const refill_state *state)
{
    if (state->semaphore_known) return state->semaphore_value;
    return d3d8_guest_load32(d3d8_device_load32(D3D8_DEV_SEMAPHORE));
}

/* One word of the second history (0: the fence, 4: the position) as the plan sees it: the newest entry a refill of
 * the plan wrote at that index, else guest memory. */
static uint32_t state_history(const refill_state *state, uint32_t ring, uint32_t index, uint32_t offset)
{
    for (uint32_t entry = state->fences; entry != 0u; entry--) {
        const d3d8_pushbuffer_history_entry *written = &state->history[entry - 1u];
        if (written->index == index) return offset == 0u ? written->fence : written->position;
    }
    return d3d8_guest_load32(ring + index * 8u + offset);
}

/* 0x003D6A8E to 0x003D6AC8: the fence the wait names. `cursor` is the new cursor (the original's edi) and
 * `wrap_delta` the distance device+0x44 holds after the wrap adjustments. The history entry the walk is at
 * gives the fence and the entry before it the ring position, as the original pairs them. */
static uint32_t refill_wait_fence(const refill_state *state, uint32_t cursor, uint32_t wrap_delta)
{
    const uint32_t ring = d3d8_device_load32(D3D8_DEV_HISTORY_RING);
    const uint32_t mask = d3d8_device_load32(D3D8_DEV_HISTORY_MASK);
    const uint32_t head = state->history_cursor;
    const uint32_t completed = state_semaphore(state);
    const uint32_t reach = cursor + (d3d8_guest_load32(D3D8_GLOBAL_PUSHBUFFER_SIZE) >> 1);
    uint32_t index = head;
    for (;;) {
        const uint32_t fence = state_history(state, ring, index, 0u);
        index = (index - 1u) & mask;
        uint32_t position = state_history(state, ring, index, 4u);
        if (position < cursor) {
            position += wrap_delta;
        }
        if (position <= reach || index == head || state_history(state, ring, index, 0u) <= completed) {
            return fence;
        }
    }
}

/* The original's device flag 4 selects an alternate put pointer (device+0x764 and +0x770) that
 * nothing in this boot sets and nothing here models, so a refill under it is refused. */
#define DEV_FLAG_ALTERNATE_PUT 0x4u

/* The fence packet the refill's tail writes at the new cursor moves it on, unless flag 0x800 skips it. `flags` are the
 * device flags the refill sees (T546: the indexed draw sets 0x800 before its reservation, so a plan carries them). */
static uint32_t refill_fence_bytes_for(uint32_t flags)
{
    if (refill_tail_fn == NULL || (flags & D3D8_PUSHBUFFER_FLAG_REFILL_WITHOUT_FENCE) != 0u) {
        return 0u;
    }
    return D3D8_PUSHBUFFER_FENCE_PACKET_BYTES;
}

static refill_plan plan_refill(const refill_state *state, uint32_t cursor, uint32_t half, uint32_t kickoff,
                               uint32_t flags)
{
    if (d3d8_device_load32(D3D8_DEV_PB_BASE) == 0u) {
        d3d8_hle_fatal(0x003D6B20u,
                       "the pushbuffer was used before CreateDevice allocated it (device+0x24 "
                       "is 0)");
    }
    if ((d3d8_device_load32(D3D8_DEV_FLAGS) & DEV_FLAG_ALTERNATE_PUT) != 0u) {
        d3d8_hle_fatal(0x003D69E0u,
                       "a pushbuffer refill with device flag 4 uses the alternate put pointer "
                       "device+0x764, which nothing models");
    }
    const uint32_t base = d3d8_device_load32(D3D8_DEV_PB_BASE);
    const uint32_t end = d3d8_device_load32(D3D8_DEV_PB_END);
    /* A segment longer than the ring would run past its end, which the original lets happen and
     * this port refuses. */
    if (end < base || kickoff > end - base) {
        d3d8_hle_fatal(0x003D69E0u, "a pushbuffer refill of %#x bytes exceeds the %#x byte ring",
                       (unsigned)kickoff, (unsigned)(end - base));
    }
    /* 0x003D6530: GET, the last kick's put. The original forms it as the register's low 28 bits in the
     * contiguous window, which is the ring address itself, and so is `put_address`. Without the GPU
     * module (no refill tail) there is no DMA state at all and the refill is the arithmetic alone. */
    const bool gpu_attached = refill_tail_fn != NULL;
    const uint32_t get = state->put;
    if (gpu_attached && (get < base || get >= end)) {
        d3d8_hle_fatal(0x003D6530u,
                       "DMA_GET %#x is outside the ring [%#x, %#x): the original reads the GPU's "
                       "own registers on that path, which nothing models",
                       (unsigned)get, (unsigned)base, (unsigned)end);
    }
    refill_plan plan = {cursor, 0u, false, get,
                        state->wrap_count - (cursor < get ? 1u : 0u)};
    uint32_t candidate = cursor + kickoff;
    if (candidate + D3D8_PUSHBUFFER_WRAP_MARGIN >= end) {
        if (cursor + half > end) {
            plan.wrapped = true;
            plan.cursor = base;
            candidate = base + kickoff;
        } else {
            candidate = end;
        }
    }
    plan.limit = candidate - D3D8_PUSHBUFFER_SLACK_BYTES;
    /* 0x003D6A62 to 0x003D6A88: GET as the wait test sees it, then the wait itself. */
    uint32_t effective = get;
    if (plan.wrapped) {
        if (get > cursor) {
            effective = base + 4u;
        }
        if (effective == base) {
            effective = base + 4u;
        }
    }
    if (gpu_attached && plan.cursor < effective && effective <= candidate) {
        if (fence_reached_fn == NULL) {
            d3d8_hle_fatal(0x003D6A88u, "the refill would wait for the GPU: GET %#x lies in the area [%#x, %#x] "
                           "about to be reused, and no fence completion test is installed",
                           (unsigned)effective, (unsigned)plan.cursor, (unsigned)candidate);
        }
        const uint32_t wrap_delta = plan.wrapped ? cursor - base : state->wrap_delta;
        const uint32_t fence = refill_wait_fence(state, plan.cursor, wrap_delta);
        if (!fence_reached_fn(fence, state->fence_counter, state_semaphore(state))) {
            d3d8_hle_fatal(0x003D6870u,
                           "the refill waits for fence %u (GET %#x lies in the area [%#x, %#x] about to be "
                           "reused) but the GPU has not reached it: only a transplanted state has such a "
                           "fence under the instantaneous GPU, and the original then spins or stalls the "
                           "ring, which nothing models",
                           (unsigned)fence, (unsigned)effective, (unsigned)plan.cursor,
                           (unsigned)candidate);
        }
    }
    /* The fence the tail inserts goes through the same preamble (0x003D67B0 begins with `cursor >= limit`), so a
     * segment shorter than the slack and the fence would roll over inside the refill. Refused, not modelled. */
    if (gpu_attached && refill_fence_bytes_for(flags) != 0u && plan.cursor >= plan.limit) {
        d3d8_hle_fatal(0x003D67B0u,
                       "the refill's fence packet would itself refill: the new segment %#x..%#x is shorter "
                       "than the slack",
                       (unsigned)plan.cursor, (unsigned)plan.limit);
    }
    return plan;
}

/* What the refill moves, after its plan is made (T526): the wrap bookkeeping of roll_over_sized, then the tail's
 * fence packet (a history entry at the packet's position, the counter plus 2) and the kick (the put after the
 * packet, the semaphore at the last fence inserted). `cursor` is where the refill was called. */
static void refill_advance(refill_state *state, const refill_plan *plan, uint32_t cursor, uint32_t flags)
{
    if (plan->wrapped) {
        state->wrap_count++;
        state->wrap_delta = cursor - d3d8_device_load32(D3D8_DEV_PB_BASE);
    }
    if (refill_tail_fn == NULL) return;
    const uint32_t fence_bytes = refill_fence_bytes_for(flags);
    if (fence_bytes != 0u) {
        if (state->fences == D3D8_PUSHBUFFER_SIM_MAX_FENCES) {
            d3d8_hle_fatal(0x003D67B0u, "a plan with more than %u refill fences is not modelled",
                           (unsigned)D3D8_PUSHBUFFER_SIM_MAX_FENCES);
        }
        state->history_cursor = d3d8_device_load32(D3D8_DEV_HISTORY_MASK) & (state->history_cursor + 1u);
        const d3d8_pushbuffer_history_entry entry = {state->history_cursor, state->fence_counter, plan->cursor};
        state->history[state->fences++] = entry;
        state->fence_counter += 2u;
    }
    state->put = plan->cursor + fence_bytes;
    if (d3d8_device_load32(D3D8_DEV_SEMAPHORE) != 0u) {
        state->semaphore_known = true;
        state->semaphore_value = state->fence_counter - 2u;
    }
}

static void roll_over_sized(uint32_t half, uint32_t kickoff)
{
    const uint32_t base = d3d8_device_load32(D3D8_DEV_PB_BASE);
    uint32_t cursor = d3d8_device_load32(D3D8_DEV_CURSOR);
    const refill_state state = refill_state_now();
    const refill_plan plan = plan_refill(&state, cursor, half, kickoff, d3d8_device_load32(D3D8_DEV_FLAGS));

    rollover_count++;
    if (consumer_fn != NULL && cursor > segment_start) {
        consumer_fn(consumer_context, segment_start, cursor);
    }
    if (plan.wrapped) {
        d3d8_device_store32(D3D8_DEV_WRAP_COUNT, d3d8_device_load32(D3D8_DEV_WRAP_COUNT) + 1u);
        d3d8_device_store32(D3D8_DEV_WRAP_DELTA, cursor - base);
        d3d8_guest_store32(cursor, (base & 0x0FFFFFFFu) + 1u);
        cursor = plan.cursor;
    }
    if (refill_tail_fn != NULL) {
        d3d8_device_store32(D3D8_DEV_GET_SHADOW, plan.get_shadow);
        d3d8_device_store32(D3D8_DEV_GET_GENERATION, plan.get_generation);
    }
    d3d8_device_store32(D3D8_DEV_LIMIT, plan.limit);
    d3d8_device_store32(D3D8_DEV_CURSOR, cursor);
    segment_start = cursor;
    if (refill_tail_fn != NULL) {
        refill_tail_fn();
    }
}

static void roll_over(void)
{
    const uint32_t kickoff = d3d8_guest_load32(D3D8_GLOBAL_KICKOFF_SIZE);
    roll_over_sized(kickoff >> 1, kickoff);
}

/* 0x003D6B30(dwords): the sized reservation. Returns without refilling while the request still
 * ends below limit + 0x200, otherwise refills with both sizes raised to the request plus the
 * 0x204 slack when that is larger. The 32-bit arithmetic is the original's. */
static void sized_arguments(uint32_t dwords, uint32_t *half, uint32_t *kickoff)
{
    const uint32_t kick = d3d8_guest_load32(D3D8_GLOBAL_KICKOFF_SIZE);
    const uint32_t need = dwords * 4u + D3D8_PUSHBUFFER_SLACK_BYTES;
    *half = need > (kick >> 1) ? need : (kick >> 1);
    *kickoff = need > kick ? need : kick;
}

bool d3d8_pushbuffer_preview_begin(uint32_t *cursor, uint32_t *limit)
{
    if (*cursor < *limit) return false;
    const uint32_t kickoff = d3d8_guest_load32(D3D8_GLOBAL_KICKOFF_SIZE);
    const refill_state state = refill_state_now();
    const uint32_t flags = d3d8_device_load32(D3D8_DEV_FLAGS);
    const refill_plan plan = plan_refill(&state, *cursor, kickoff >> 1, kickoff, flags);
    *cursor = plan.cursor + refill_fence_bytes_for(flags);
    *limit = plan.limit;
    return true;
}

d3d8_pushbuffer_sim d3d8_pushbuffer_sim_start(void)
{
    d3d8_pushbuffer_sim sim;
    memset(&sim, 0, sizeof(sim));
    sim.cursor = d3d8_device_load32(D3D8_DEV_CURSOR);
    sim.limit = d3d8_device_load32(D3D8_DEV_LIMIT);
    return sim;
}

/* Record a span the plan writes (T546), merged with the one before it when they touch. The indexed draw checks every one
 * against what it reads. */
static void sim_note(d3d8_pushbuffer_sim *sim, uint32_t entry, uint32_t begin, uint32_t bytes)
{
    if (bytes == 0u) return;
    if (sim->span_count != 0u) {
        d3d8_pushbuffer_span *last = &sim->span[sim->span_count - 1u];
        if ((uint64_t)last->begin + last->bytes == begin) {
            last->bytes += bytes;
            return;
        }
    }
    if (sim->span_count == D3D8_PUSHBUFFER_SIM_MAX_SPANS)
        d3d8_hle_fatal(entry, "a plan with more than %u separate command spans is not modelled",
                       (unsigned)D3D8_PUSHBUFFER_SIM_MAX_SPANS);
    sim->span[sim->span_count].begin = begin;
    sim->span[sim->span_count].bytes = bytes;
    sim->span_count++;
}

uint32_t d3d8_pushbuffer_sim_flags(const d3d8_pushbuffer_sim *sim)
{
    return sim->flags_set ? sim->flags : d3d8_device_load32(D3D8_DEV_FLAGS);
}

void d3d8_pushbuffer_sim_set_flags(d3d8_pushbuffer_sim *sim, uint32_t flags)
{
    sim->flags_set = true;
    sim->flags = flags;
}

/* Plan one refill of the simulation from the state the ones before it left, then move that state on (T526). The refill
 * sees the flags the plan carries (T546), and a tail that runs under flag 0x800 leaves flag 0x1000 behind. */
static refill_plan sim_refill(d3d8_pushbuffer_sim *sim, uint32_t cursor, uint32_t half, uint32_t kickoff)
{
    if (!sim->state.loaded) sim->state = refill_state_now();
    const uint32_t flags = d3d8_pushbuffer_sim_flags(sim);
    const refill_plan plan = plan_refill(&sim->state, cursor, half, kickoff, flags);
    refill_advance(&sim->state, &plan, cursor, flags);
    sim->limit = plan.limit;
    sim->refills++;
    const d3d8_pushbuffer_refill_spans spans = {cursor, plan.wrapped, plan.cursor, refill_fence_bytes_for(flags)};
    sim->last_refill = spans;
    if (refill_tail_fn != NULL && (flags & D3D8_PUSHBUFFER_FLAG_REFILL_WITHOUT_FENCE) != 0u)
        d3d8_pushbuffer_sim_set_flags(sim, flags | 0x1000u);
    if (plan.wrapped) sim_note(sim, 0x003D6B20u, cursor, 4u);
    sim_note(sim, 0x003D6B20u, plan.cursor, spans.fence_bytes);
    return plan;
}

/* The preamble `if (cursor >= limit) roll over` of one site over the simulation. */
static bool sim_enter(d3d8_pushbuffer_sim *sim)
{
    if (sim->cursor < sim->limit) return false;
    const uint32_t kickoff = d3d8_guest_load32(D3D8_GLOBAL_KICKOFF_SIZE);
    const refill_plan plan = sim_refill(sim, sim->cursor, kickoff >> 1, kickoff);
    sim->cursor = plan.cursor + sim->last_refill.fence_bytes;
    return true;
}

uint32_t d3d8_pushbuffer_sim_write(d3d8_pushbuffer_sim *sim, uint32_t entry, uint32_t bytes)
{
    const uint32_t start = sim->cursor;
    if ((uint64_t)sim->cursor + bytes > UINT32_MAX || kernel_guest_at(sim->cursor, bytes) == NULL)
        d3d8_hle_fatal(entry, "command span %#x+%u is not mapped guest memory",
                       (unsigned)sim->cursor, (unsigned)bytes);
    sim_note(sim, entry, start, bytes);
    sim->cursor += bytes;
    sim->bytes += bytes;
    return start;
}

uint32_t d3d8_pushbuffer_sim_site(d3d8_pushbuffer_sim *sim, uint32_t entry, uint32_t bytes)
{
    (void)sim_enter(sim);
    return d3d8_pushbuffer_sim_write(sim, entry, bytes);
}

bool d3d8_pushbuffer_sim_reserve(d3d8_pushbuffer_sim *sim, uint32_t dwords)
{
    if (sim->cursor + dwords * 4u < sim->limit + 0x200u) return false;
    uint32_t half, kickoff;
    sized_arguments(dwords, &half, &kickoff);
    const refill_plan plan = sim_refill(sim, sim->cursor, half, kickoff);
    sim->cursor = plan.cursor + sim->last_refill.fence_bytes;
    return true;
}

/* T487: where the site's bytes land when it is written in units with a re-check between two of them. */
static uint32_t span_end(uint32_t entry, uint32_t begin, uint32_t bytes)
{
    const uint64_t end = (uint64_t)begin + bytes;
    if (end > UINT32_MAX || (bytes != 0u && kernel_guest_at(begin, bytes) == NULL))
        d3d8_hle_fatal(entry, "command span %#x+%u is not mapped guest memory", (unsigned)begin,
                       (unsigned)bytes);
    return (uint32_t)end;
}

void d3d8_pushbuffer_sim_split_site(d3d8_pushbuffer_sim *sim, uint32_t entry, uint32_t head_bytes,
                                    uint32_t unit_bytes, uint32_t units, uint32_t tail_bytes,
                                    d3d8_pushbuffer_split *plan)
{
    const bool entered = sim_enter(sim);
    const d3d8_pushbuffer_split none = {entered, false, sim->cursor, 1u, {{sim->cursor, 0u, false, 0u}}};
    *plan = none;
    uint64_t site_bytes = (uint64_t)head_bytes + (uint64_t)unit_bytes * units + tail_bytes;
    if (site_bytes > UINT32_MAX) d3d8_hle_fatal(entry, "command site of %llu bytes is too large",
                                                (unsigned long long)site_bytes);
    uint32_t position = span_end(entry, sim->cursor, head_bytes);
    for (uint32_t unit = 1u; unit <= units; unit++) {
        position = span_end(entry, position, unit_bytes);
        /* 0x003D5877: `cmp edi, [limit]` only when another FULL unit follows. */
        if (unit == units || position < sim->limit) continue;
        if (plan->runs == D3D8_PUSHBUFFER_MAX_RUNS)
            d3d8_hle_fatal(entry, "a command site refilling more than %u times is not modelled",
                           (unsigned)(D3D8_PUSHBUFFER_MAX_RUNS - 1u));
        const uint32_t kickoff = d3d8_guest_load32(D3D8_GLOBAL_KICKOFF_SIZE);
        /* Planned from the state every refill before this one left: the entry roll-over's and the earlier splits'. */
        const refill_plan refill = sim_refill(sim, position, kickoff >> 1, kickoff);
        d3d8_pushbuffer_run *ended = &plan->run[plan->runs - 1u];
        ended->bytes = position - ended->begin;
        ended->wraps = refill.wrapped;
        ended->fence_bytes = sim->last_refill.fence_bytes;
        if (refill.wrapped) (void)span_end(entry, position, 4u);
        position = span_end(entry, refill.cursor, ended->fence_bytes);
        const d3d8_pushbuffer_run next = {position, 0u, false, 0u};
        plan->run[plan->runs++] = next;
        plan->split = true;
    }
    position = span_end(entry, position, tail_bytes);
    d3d8_pushbuffer_run *last = &plan->run[plan->runs - 1u];
    last->bytes = position - last->begin;
    for (uint32_t run = 0u; run < plan->runs; run++) sim_note(sim, entry, plan->run[run].begin, plan->run[run].bytes);
    sim->bytes += (uint32_t)site_bytes;
    sim->cursor = position;
}

void d3d8_pushbuffer_sim_checked_site(d3d8_pushbuffer_sim *sim, uint32_t entry, uint32_t bytes,
                                      d3d8_pushbuffer_split *plan)
{
    const d3d8_pushbuffer_split none = {false, false, sim->cursor, 1u, {{sim->cursor, 0u, false, 0u}}};
    *plan = none;
    /* 0x003D5720 to 0x003D572E and 0x003D57B4 to 0x003D57BD: `cursor + bytes >= limit` (a 32-bit add, `jae`) calls the
     * refill 0x003D6B20 with the cursor already published, then starts again from the loads of the cursor and limit. */
    while ((uint32_t)(sim->cursor + bytes) >= sim->limit) {
        if (plan->runs >= D3D8_PUSHBUFFER_MAX_RUNS)
            d3d8_hle_fatal(entry, "a command site refilling more than %u times is not modelled",
                           (unsigned)(D3D8_PUSHBUFFER_MAX_RUNS - 1u));
        const uint32_t kickoff = d3d8_guest_load32(D3D8_GLOBAL_KICKOFF_SIZE);
        const uint32_t at = sim->cursor;
        const refill_plan refill = sim_refill(sim, at, kickoff >> 1, kickoff);
        d3d8_pushbuffer_run *ended = &plan->run[plan->runs - 1u];
        const uint32_t fence_bytes = sim->last_refill.fence_bytes;
        ended->wraps = sim->last_refill.wraps;
        ended->fence_bytes = fence_bytes;
        if (sim->last_refill.wraps) (void)span_end(entry, at, 4u);
        sim->cursor = span_end(entry, refill.cursor, fence_bytes);
        const d3d8_pushbuffer_run next = {sim->cursor, 0u, false, 0u};
        plan->run[plan->runs++] = next;
    }
    plan->split = plan->runs > 1u;
    const uint32_t end = span_end(entry, sim->cursor, bytes);
    plan->run[plan->runs - 1u].bytes = bytes;
    plan->begin = plan->run[plan->runs - 1u].begin;
    sim_note(sim, entry, plan->begin, bytes);
    sim->bytes += plan->run[plan->runs - 1u].bytes;
    sim->cursor = end;
}

uint32_t d3d8_pushbuffer_refill_at(uint32_t cursor)
{
    d3d8_pushbuffer_end(cursor);
    roll_over();
    return d3d8_device_load32(D3D8_DEV_CURSOR);
}

bool d3d8_pushbuffer_preview_reserve(uint32_t dwords, uint32_t *cursor, uint32_t *limit)
{
    if (*cursor + dwords * 4u < *limit + 0x200u) return false;
    uint32_t half, kickoff;
    sized_arguments(dwords, &half, &kickoff);
    const refill_state state = refill_state_now();
    const uint32_t flags = d3d8_device_load32(D3D8_DEV_FLAGS);
    const refill_plan plan = plan_refill(&state, *cursor, half, kickoff, flags);
    *cursor = plan.cursor + refill_fence_bytes_for(flags);
    *limit = plan.limit;
    return true;
}

uint32_t d3d8_pushbuffer_reserve(uint32_t dwords)
{
    const uint32_t cursor = d3d8_device_load32(D3D8_DEV_CURSOR);
    const uint32_t limit = d3d8_device_load32(D3D8_DEV_LIMIT);
    if (cursor + dwords * 4u >= limit + 0x200u) {
        uint32_t half, kickoff;
        sized_arguments(dwords, &half, &kickoff);
        roll_over_sized(half, kickoff);
    }
    return d3d8_device_load32(D3D8_DEV_CURSOR);
}

void d3d8_pushbuffer_drain(void)
{
    const uint32_t cursor = d3d8_device_load32(D3D8_DEV_CURSOR);
    if (consumer_fn != NULL && cursor > segment_start) {
        consumer_fn(consumer_context, segment_start, cursor);
    }
    segment_start = cursor;
    put_address = cursor;
}

uint32_t d3d8_pushbuffer_begin(void)
{
    uint32_t cursor = d3d8_device_load32(D3D8_DEV_CURSOR);
    if (cursor >= d3d8_device_load32(D3D8_DEV_LIMIT)) {
        roll_over();
        cursor = d3d8_device_load32(D3D8_DEV_CURSOR);
    }
    return cursor;
}

void d3d8_pushbuffer_end_at(uint32_t device, uint32_t before, uint32_t cursor)
{
    if (cursor > before) {
        dwords_written += (cursor - before) / 4u;
    }
    d3d8_guest_store32(device + D3D8_DEV_CURSOR, cursor);
}

void d3d8_pushbuffer_end(uint32_t cursor)
{
    const uint32_t before = d3d8_device_load32(D3D8_DEV_CURSOR);
    d3d8_pushbuffer_end_at(D3D8_DEVICE_BASE, before, cursor);
}

void d3d8_pushbuffer_emit_pair(uint32_t header, uint32_t value)
{
    for (;;) {
        const uint32_t advanced = d3d8_device_load32(D3D8_DEV_CURSOR) + 8u;
        if (advanced >= d3d8_device_load32(D3D8_DEV_LIMIT)) {
            roll_over();
            continue;
        }
        d3d8_device_store32(D3D8_DEV_CURSOR, advanced);
        d3d8_guest_store32(advanced - 8u, header);
        d3d8_guest_store32(advanced - 4u, value);
        dwords_written += 2u;
        return;
    }
}
