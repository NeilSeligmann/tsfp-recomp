/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See d3d8_gpu.h for the model and what it leaves out. Every function names the original it ports.
 */

#include "d3d8_gpu.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "d3d8_overlay.h"
#include "d3d8_pushbuffer.h"
#include "d3d8_vblank_effects.h"
#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_clock.h"
#include "kernel_hle.h"
#include "nt_status.h"

#define KERNEL_ORDINAL_KE_SET_EVENT 145u
#define KERNEL_ORDINAL_KE_WAIT_FOR_SINGLE_OBJECT 159u
#define KERNEL_ORDINAL_MM_ALLOCATE_CONTIGUOUS_EX 166u

/* 0x003DACB0: the control block, from the allocation wrapper's flag word 0xAC800000 (MEASURED in the
 * oracle's allocation log: size 0x60, alignment 0x1000, protect 4). The semaphore is its first
 * dword and 0x003DACDE zeroes 64 bytes from +0x20. */
#define CONTROL_BLOCK_BYTES 0x60u
#define CONTROL_BLOCK_ALIGNMENT 0x1000u
#define CONTROL_BLOCK_PROTECT 0x4u
#define CONTROL_BLOCK_NOTIFIER 0x20u
#define CONTROL_BLOCK_NOTIFIER_BYTES 0x40u

#define DEV_CONTROL_NOTIFIER 0x2490u
#define DEV_CONTROL_NOTIFIER_END 0x248Cu
#define DEV_CALLBACK 0x19FCu
#define DEV_FLAGS_ALTERNATE_PUT 0x4u
#define DEV_FLAGS_REFILLED 0x1000u

#define SEMAPHORE_AT_CREATE 3u
#define METHOD_SEMAPHORE_RELEASE 0x1D70u
#define METHOD_CLEAR_SURFACE 0x1D94u

/* KEVENT as the library initialises it (0x003DD144): Type 0, Size 4, SignalState 1, and a wait list
 * linked to itself at +8 and +12. The first dword is 0x00040000. */
#define EVENT_HEADER 0x00040000u
#define EVENT_SIGNAL_STATE 0x4u
#define EVENT_LIST 0x8u

/* The kick history ring, sized by 0x003D6360 from the two sizes SetPushBufferSize stored. */
#define HISTORY_ENTRY_BYTES 8u

static d3d8_gpu_command initial_stream[D3D8_GPU_STREAM_CAPACITY];
static d3d8_gpu_command *stream = initial_stream;
static size_t stream_capacity = D3D8_GPU_STREAM_CAPACITY;
static size_t stream_limit = D3D8_GPU_STREAM_CAPACITY;
static size_t stream_length;
static d3d8_gpu_recorded_observer recorded_observer;
static d3d8_gpu_stats stats;
static struct {
    uint32_t address;
    uint64_t count;
} elided[D3D8_GPU_ELIDED_TABLE];
static size_t elided_used;
static uint32_t history_heap;
static uint64_t reset_count; /* survives the reset it counts */

uint64_t d3d8_gpu_reset_count(void)
{
    return reset_count;
}

void d3d8_gpu_reset(void)
{
    reset_count++;
    if (stream != initial_stream) free(stream);
    stream = initial_stream;
    stream_capacity = D3D8_GPU_STREAM_CAPACITY;
    memset(&stats, 0, sizeof(stats));
    stream_length = 0u;
    elided_used = 0u;
    history_heap = 0u;
}

void d3d8_gpu_set_recorded_observer(d3d8_gpu_recorded_observer observer)
{
    recorded_observer = observer;
}

bool d3d8_gpu_set_stream_limit(size_t limit)
{
    /* Configuration persists through CreateDevice/reset, but never changes a
     * running recording or its index associations. */
    if (limit < D3D8_GPU_STREAM_CAPACITY || limit > D3D8_GPU_STREAM_MAX_CAPACITY ||
        stream_length != 0u || stats.commands_recorded != 0u || stats.commands_dropped != 0u)
        return false;
    stream_limit = limit;
    return true;
}

size_t d3d8_gpu_stream_capacity(void) { return stream_capacity; }
size_t d3d8_gpu_stream_limit(void) { return stream_limit; }

size_t d3d8_gpu_stream_count(void)
{
    return stream_length;
}

d3d8_gpu_command d3d8_gpu_stream_at(size_t index)
{
    return index < stream_length ? stream[index] : (d3d8_gpu_command){0u, 0u, 0u};
}

void d3d8_gpu_stream_discard(size_t count)
{
    if (count > stream_length) {
        count = stream_length;
    }
    memmove(stream, stream + count, (stream_length - count) * sizeof(stream[0]));
    stream_length -= count;
    stats.commands_discarded += count;
}

d3d8_gpu_stats d3d8_gpu_get_stats(void)
{
    return stats;
}

uint64_t d3d8_gpu_vblank_count(void)
{
    return stats.vblanks;
}

uint64_t d3d8_gpu_elided_count(uint32_t address)
{
    for (size_t index = 0u; index < elided_used; index++) {
        if (elided[index].address == address) {
            return elided[index].count;
        }
    }
    return 0u;
}

void d3d8_gpu_note_elided(uint32_t address)
{
    stats.emissions_elided++;
    for (size_t index = 0u; index < elided_used; index++) {
        if (elided[index].address == address) {
            elided[index].count++;
            return;
        }
    }
    if (elided_used >= D3D8_GPU_ELIDED_TABLE) {
        d3d8_hle_fatal(address, "more than %u distinct emitters elided: raise D3D8_GPU_ELIDED_TABLE",
                       (unsigned)D3D8_GPU_ELIDED_TABLE);
    }
    elided[elided_used].address = address;
    elided[elided_used].count = 1u;
    elided_used++;
}

/* --- the consumer ----------------------------------------------------------------------- */

static void record(uint32_t subchannel, uint32_t method, uint32_t data)
{
    if (stream_length >= stream_capacity) {
        if (stream_capacity >= stream_limit) {
            stats.stream_limit_failures++;
            stats.commands_dropped++;
            return;
        }
        size_t capacity = stream_capacity * 2u;
        if (capacity > stream_limit) capacity = stream_limit;
        if (capacity > SIZE_MAX / sizeof(*stream)) {
            stats.stream_allocation_failures++;
            stats.commands_dropped++;
            return;
        }
        d3d8_gpu_command *grown;
        if (stream == initial_stream) {
            grown = malloc(capacity * sizeof(*stream));
            if (grown != NULL) memcpy(grown, stream, stream_length * sizeof(*stream));
        } else {
            grown = realloc(stream, capacity * sizeof(*stream));
        }
        if (grown == NULL) {
            stats.stream_allocation_failures++;
            stats.commands_dropped++;
            return;
        }
        stream = grown;
        stream_capacity = capacity;
        stats.stream_growths++;
    }
    stream[stream_length].method = method;
    stream[stream_length].data = data;
    stream[stream_length].subchannel = subchannel;
    stream_length++;
    if (stats.stream_commands_peak < stream_length) stats.stream_commands_peak = stream_length;
    stats.commands_recorded++;
    if (subchannel != D3D8_GPU_SUBCHANNEL_3D) {
        stats.commands_other_subchannel++;
        return; /* never a CLEAR_SURFACE, whatever its method number */
    }
    if (method == METHOD_CLEAR_SURFACE) {
        stats.clear_surfaces++;
    }
}

/*
 * Walk [begin, end) as NV2A method headers: bits 2-12 the method, bits 13-15 the subchannel (T391: the
 * subchannel used to be dropped, so the fence's subchannel-5 0x310 was indistinguishable from the 3D
 * class's SET_DITHER_ENABLE at 0x310), bits 18-28 the count, bit 30 set for a non-incrementing run.
 * A header whose run does not fit the range is counted as malformed and the rest of the range abandoned, because past it nothing can be told apart from data. The
 * ring-wrap jump (bit 0 set) and the old-style jump are not part of a handed-off range (the library
 * writes the first after the hand-off), so one found inside is malformed too.
 */
static void record_range(uint32_t begin, uint32_t end)
{
    uint32_t position = begin;
    while (position < end) {
        const uint32_t header = d3d8_guest_load32(position);
        stats.dwords_consumed++;
        position += 4u;
        if (header == 0u) {
            continue;
        }
        const uint32_t count = (header >> 18) & 0x7FFu;
        if ((header & 3u) == 1u || (header & 0xE0000003u) == 0x20000000u ||
            count > (end - position) / 4u) {
            stats.malformed_dwords++;
            return;
        }
        const uint32_t method = header & 0x1FFCu;
        const uint32_t subchannel = (header >> 13) & 7u;
        const bool increasing = (header & 0x40000000u) == 0u;
        for (uint32_t index = 0u; index < count; index++) {
            record(subchannel, increasing ? method + 4u * index : method, d3d8_guest_load32(position));
            position += 4u;
            stats.dwords_consumed++;
        }
    }
}

/* The consumer: record the range, then tell the observer (T262). It runs at the kick, so guest memory
 * is as it stands when the GPU is handed the range. */
static void consume(void *context, uint32_t begin, uint32_t end)
{
    (void)context;
    record_range(begin, end);
    if (recorded_observer != NULL) {
        recorded_observer();
    }
}

/* --- creation --------------------------------------------------------------------------- */

static void initialise_event(uint32_t address)
{
    d3d8_guest_store32(address, EVENT_HEADER);
    d3d8_guest_store32(address + EVENT_SIGNAL_STATE, 1u);
    d3d8_guest_store32(address + EVENT_LIST, address + EVENT_LIST);
    d3d8_guest_store32(address + EVENT_LIST + 4u, address + EVENT_LIST);
}

/* 0x003D63BA to 0x003D63F2: one history entry per kickoff segment in the ring, rounded up to a power of
 * two, so the mask is the entry count less one. */
static bool create_history(void)
{
    const uint32_t size = d3d8_guest_load32(D3D8_GLOBAL_PUSHBUFFER_SIZE);
    const uint32_t kickoff = d3d8_guest_load32(D3D8_GLOBAL_KICKOFF_SIZE);
    if (kickoff == 0u) {
        d3d8_hle_fatal(0x003D6360u, "a kickoff size of 0 divides by zero in the original");
    }
    uint32_t segments = (size - 1u) / kickoff;
    uint32_t bits = 0u;
    do {
        segments >>= 1;
        bits++;
    } while (segments != 0u);
    const uint32_t entries = 1u << bits;

    if (history_heap == 0u || !guest_heap_valid(history_heap)) {
        history_heap = guest_heap_create(0u, GUEST_HEAP_CHUNK_MIN, 0u);
        if (history_heap == 0u) {
            return false;
        }
    }
    const uint32_t history = guest_heap_alloc(history_heap, entries * HISTORY_ENTRY_BYTES);
    if (history == 0u) {
        return false;
    }
    d3d8_device_store32(D3D8_DEV_HISTORY_MASK, entries - 1u);
    d3d8_device_store32(D3D8_DEV_HISTORY_RING, history);
    return true;
}

static void refill_tail(void);
static bool fence_reached_at(uint32_t fence, uint32_t counter, uint32_t semaphore);

bool d3d8_gpu_create(void)
{
    d3d8_gpu_reset();

    const uint32_t args[5] = {CONTROL_BLOCK_BYTES, 0u, 0xFFFFFFFFu, CONTROL_BLOCK_ALIGNMENT,
                              CONTROL_BLOCK_PROTECT};
    const uint32_t block = d3d8_kernel_call(KERNEL_ORDINAL_MM_ALLOCATE_CONTIGUOUS_EX, args, 5u);
    if (block == 0u) {
        return false;
    }
    d3d8_device_store32(D3D8_DEV_CONTROL_BLOCK, block);
    d3d8_device_store32(D3D8_DEV_SEMAPHORE, block);
    d3d8_device_store32(DEV_CONTROL_NOTIFIER_END, block + CONTROL_BLOCK_NOTIFIER * 2u);
    d3d8_device_store32(DEV_CONTROL_NOTIFIER, block + CONTROL_BLOCK_NOTIFIER);
    for (uint32_t offset = 0u; offset < CONTROL_BLOCK_NOTIFIER_BYTES; offset += 4u) {
        d3d8_guest_store32(block + CONTROL_BLOCK_NOTIFIER + offset, 0u);
    }
    d3d8_guest_store32(block, SEMAPHORE_AT_CREATE);

    if (!create_history()) {
        return false;
    }
    initialise_event(D3D8_DEVICE_BASE + D3D8_DEV_VBLANK_EVENT);
    initialise_event(D3D8_DEVICE_BASE + D3D8_DEV_FENCE_EVENT);
    d3d8_gpu_attach();
    return true;
}

void d3d8_gpu_attach(void)
{
    d3d8_pushbuffer_set_consumer(consume, NULL);
    d3d8_pushbuffer_set_refill_tail(refill_tail);
    d3d8_pushbuffer_set_fence_reached(fence_reached_at);
}

/* --- kick and fences -------------------------------------------------------------------- */

/* 0x003D67B0 writes this packet at the cursor: header 0x0004A310 (subchannel 5, method 0x310, one data
 * dword) with ((cursor * 8 | fence & 0x1F) << 2) | wrap count & 3, header 0x00041D70 with the fence
 * (SEMAPHORE_RELEASE), then 0x00041D90 with 0 twice (colour clear value). MEASURED from the bytes
 * 0x003D67C7 to 0x003D6810 and from the ring the original leaves (tests/test_d3d8_refill_oracle.py). What
 * the hardware does with the software method on subchannel 5 is NOT measured: the first dword is the
 * ring address and the fence's low bits, so the INFERENCE is a history marker the driver's interrupt
 * handler reads. */
#define METHOD_COLOR_CLEAR_VALUE 0x1D90u
#define FENCE_HEADER(subchannel, method) (0x00040000u | ((subchannel) << 13) | (method))

static void write_fence_packet(uint32_t cursor, uint32_t fence)
{
    const uint32_t wraps = d3d8_device_load32(D3D8_DEV_WRAP_COUNT);
    d3d8_guest_store32(cursor, FENCE_HEADER(D3D8_GPU_SUBCHANNEL_SOFTWARE, D3D8_GPU_METHOD_FENCE_NOTIFY));
    d3d8_guest_store32(cursor + 4u, (((cursor << 3) | (fence & 0x1Fu)) << 2) | (wraps & 3u));
    d3d8_guest_store32(cursor + 8u, FENCE_HEADER(D3D8_GPU_SUBCHANNEL_3D, METHOD_SEMAPHORE_RELEASE));
    d3d8_guest_store32(cursor + 12u, fence);
    d3d8_guest_store32(cursor + 16u, FENCE_HEADER(D3D8_GPU_SUBCHANNEL_3D, METHOD_COLOR_CLEAR_VALUE));
    d3d8_guest_store32(cursor + 20u, 0u);
    d3d8_guest_store32(cursor + 24u, FENCE_HEADER(D3D8_GPU_SUBCHANNEL_3D, METHOD_COLOR_CLEAR_VALUE));
    d3d8_guest_store32(cursor + 28u, 0u);
}

void d3d8_gpu_kick(void)
{
    if ((d3d8_device_load32(D3D8_DEV_FLAGS) & DEV_FLAGS_ALTERNATE_PUT) != 0u) {
        d3d8_hle_fatal(0x003D6690u, "the kick with device flag 4 puts device+0x770 instead of the "
                                    "cursor, which nothing in this boot sets and nothing models");
    }
    stats.kicks++;
    d3d8_pushbuffer_drain();
    /* Every inserted fence is complete: the last one inserted is the counter less 2. */
    const uint32_t semaphore = d3d8_device_load32(D3D8_DEV_SEMAPHORE);
    if (semaphore != 0u) {
        d3d8_guest_store32(semaphore, d3d8_device_load32(D3D8_DEV_FENCE) - 2u);
    }
}

uint32_t d3d8_gpu_fence_insert(uint32_t flags)
{
    const uint32_t cursor = d3d8_pushbuffer_begin();
    const uint32_t fence = d3d8_device_load32(D3D8_DEV_FENCE);

    write_fence_packet(cursor, fence);
    d3d8_pushbuffer_end(cursor + D3D8_PUSHBUFFER_FENCE_PACKET_BYTES);
    const uint32_t slot = (fence >> 1) & 0x3Fu;
    d3d8_device_store32(D3D8_DEV_HISTORY_TABLE + slot * 8u, fence);
    d3d8_device_store32(D3D8_DEV_HISTORY_TABLE + slot * 8u + 4u, cursor);
    if ((flags & 1u) != 0u) {
        const uint32_t next = d3d8_device_load32(D3D8_DEV_HISTORY_MASK) &
                              (d3d8_device_load32(D3D8_DEV_HISTORY_CURSOR) + 1u);
        const uint32_t ring = d3d8_device_load32(D3D8_DEV_HISTORY_RING);
        d3d8_device_store32(D3D8_DEV_HISTORY_CURSOR, next);
        d3d8_guest_store32(ring + next * 8u, fence);
        d3d8_guest_store32(ring + next * 8u + 4u, cursor);
    }
    d3d8_device_store32(D3D8_DEV_FENCE, fence + 2u);
    d3d8_device_store32(D3D8_DEV_FENCE_STALL, 0u);
    stats.fences_inserted++;
    if ((flags & 2u) == 0u) {
        d3d8_gpu_kick();
    }
    return fence;
}

/* 0x003D6AE0 to 0x003D6B14, the end of the refill 0x003D69E0 (T391): with device flag 0x800 the flag 0x1000
 * is set and the GPU kicked, otherwise a fence is inserted with flags 3 (second history, no kick of its
 * own) and the GPU kicked. */
static void refill_tail(void)
{
    const uint32_t flags = d3d8_device_load32(D3D8_DEV_FLAGS);
    if ((flags & D3D8_PUSHBUFFER_FLAG_REFILL_WITHOUT_FENCE) != 0u) {
        d3d8_device_store32(D3D8_DEV_FLAGS, flags | DEV_FLAGS_REFILLED);
    } else {
        (void)d3d8_gpu_fence_insert(3u);
    }
    d3d8_gpu_kick();
}

/* The completion test of 0x003D6870: the fence is reached when the counter's distance to it is not
 * less than the counter's distance to the semaphore, in unsigned arithmetic. */
static bool fence_reached_at(uint32_t fence, uint32_t counter, uint32_t semaphore)
{
    return (counter - fence) >= (counter - semaphore);
}

static bool fence_reached(uint32_t fence)
{
    return fence_reached_at(fence, d3d8_device_load32(D3D8_DEV_FENCE),
                            d3d8_guest_load32(d3d8_device_load32(D3D8_DEV_SEMAPHORE)));
}

void d3d8_gpu_fence_wait(uint32_t fence, uint32_t mode)
{
    (void)mode;
    stats.fence_waits++;
    if (d3d8_device_load32(D3D8_DEV_SEMAPHORE) == 0u) {
        d3d8_hle_fatal(0x003D6870u, "a fence wait before CreateDevice made the control block");
    }
    if (fence_reached(fence)) {
        return;
    }
    if (d3d8_device_load32(DEV_CALLBACK) != 0u) {
        d3d8_hle_fatal(0x003D6870u, "a user callback at device+0x19FC is installed: not modelled");
    }
    stats.fence_waits_blocked++;
    if (fence == d3d8_device_load32(D3D8_DEV_FENCE)) {
        (void)d3d8_gpu_fence_insert(0u);
    }
    d3d8_gpu_kick();
    if (!fence_reached(fence)) {
        d3d8_hle_fatal(0x003D6870u, "fence %u was never inserted (counter %u): the wait would "
                                    "never end", (unsigned)fence,
                       (unsigned)d3d8_device_load32(D3D8_DEV_FENCE));
    }
}

/* --- the vertical blank ----------------------------------------------------------------- */

static d3d8_vblank_wait_observer vblank_observer;
void d3d8_gpu_set_vblank_observer(d3d8_vblank_wait_observer observer)
{
    vblank_observer = observer;
}
bool d3d8_gpu_model_blank(void)
{
    /* The same refresh class and clock floor as a completed wait, without the event pair. */
    const unsigned refresh_hz = (d3d8_device_load32(0x1DDCu) & 0x00400000u) != 0u ? 60u : 50u;
    uint64_t blank_time = 0u;
    if (!d3d8_vblank_effects_enabled() || !kernel_clock_frame_floor(refresh_hz, &blank_time)) {
        return false;
    }
    stats.vblanks++;
    (void)d3d8_vblank_effects_apply((uint32_t)blank_time); /* a refused state is fatal, never a silent no-op */
    d3d8_overlay_consume_vblank();
    return true;
}
void d3d8_gpu_wait_vblank(void)
{
    const uint32_t event = D3D8_DEVICE_BASE + D3D8_DEV_VBLANK_EVENT;
    /* 0x003D3550 clears SignalState itself, in guest memory, with no kernel call. */
    d3d8_guest_store32(event + EVENT_SIGNAL_STATE, 0u);

    /* The blank happens while the guest waits: the DPC the hardware interrupt would queue calls
     * KeSetEvent (0x003DC3A8, 0x003DC76E), and this is the same call, from the same point every run. */
    /* Use the matched display row, just as AdapterEnumModes decodes its refresh class.
     * GPU/device recreation must not reset the CPU clock: guest time stays monotonic. */
    const unsigned refresh_hz =
        (d3d8_device_load32(0x1DDCu) & 0x00400000u) != 0u ? 60u : 50u;
    uint64_t blank_time = 0u;
    const bool clock_completed = kernel_clock_frame_floor(refresh_hz, &blank_time);
    stats.vblanks++;
    const uint32_t set_args[3] = {event, 0u, 0u};
    const uint32_t previous_signal = d3d8_kernel_call(KERNEL_ORDINAL_KE_SET_EVENT, set_args, 3u);

    /* KeWaitForSingleObject(event, WaitReason 6, WaitMode 1, Alertable 0, no timeout). */
    const uint32_t wait_args[5] = {event, 6u, 1u, 0u, 0u};
    const uint32_t wait_status = d3d8_kernel_call(KERNEL_ORDINAL_KE_WAIT_FOR_SINGLE_OBJECT, wait_args, 5u);
    const kernel_entry *set_entry = kernel_hle_entry(KERNEL_ORDINAL_KE_SET_EVENT);
    const kernel_entry *wait_entry = kernel_hle_entry(KERNEL_ORDINAL_KE_WAIT_FOR_SINGLE_OBJECT);
    /* This routine cleared SignalState before the set. Only previous-state0
     * is an eligible transition; KeSetEvent's LONG is not a success status. */
    const bool completed = set_entry && wait_entry &&
        set_entry->state == KERNEL_ENTRY_IMPLEMENTED &&
        wait_entry->state == KERNEL_ENTRY_IMPLEMENTED && clock_completed &&
        previous_signal == 0u && wait_status == STATUS_SUCCESS;
    /* T372, default off: a completed wait is one helper run's derivable device effects. The
     * timestamp is the blank's own virtual time (the clock floor), never a wall time. */
    if (completed && d3d8_vblank_effects_enabled()) {
        const uint64_t before = d3d8_vblank_effects_applied();
        (void)d3d8_vblank_effects_apply((uint32_t)blank_time);
        if (d3d8_vblank_effects_applied() == before + 1u) {
            d3d8_overlay_consume_vblank();
        }
    }
    if (vblank_observer != NULL) {
        vblank_observer(completed);
    }
}

/* --- handlers --------------------------------------------------------------------------- */

static uint32_t argument(const void *context, unsigned index, uint32_t address)
{
    uint32_t value = 0u;
    if (!kernel_frame_arg((const kernel_call_frame *)context, index, &value)) {
        d3d8_hle_fatal(address, "argument %u of the call cannot be read", index);
    }
    return value;
}

static uint32_t handler_kick(void *context)
{
    /* 0x003D6690 takes the device in ecx, which is the global every other function loads. */
    (void)context;
    d3d8_gpu_kick();
    return 0u;
}

static uint32_t handler_fence_insert(void *context)
{
    return d3d8_gpu_fence_insert(argument(context, 0u, 0x003D67B0u));
}

/* 0x003D3630: `push 0; call 0x003D67B0; ret`. A bare stdcall with no arguments that inserts a fence
 * with flags 0 (no second history entry, kick at once) and returns the fence value in eax. The
 * title calls it from 0x00018E20 and stores the result as its own fence. */
static uint32_t handler_insert_fence_and_kick(void *context)
{
    (void)context;
    return d3d8_gpu_fence_insert(0u);
}

static uint32_t handler_fence_wait(void *context)
{
    d3d8_gpu_fence_wait(argument(context, 0u, 0x003D6870u), argument(context, 1u, 0x003D6870u));
    return 0u;
}

/* 0x003D34B0, stdcall(fence), ret 4: `push 4; push fence; call 0x003D6870`, so a wait on `fence` with
 * mode 4. The title (0x00018E5C) discards the result. */
static uint32_t handler_block_until_fence(void *context)
{
    d3d8_gpu_fence_wait(argument(context, 0u, 0x003D34B0u), 4u);
    return 0u;
}

/* 0x003D34A0, stdcall(), a jump stub to 0x003D6B80: `mov eax,[0x3E3F58]; mov ecx,[eax+0x2C]; push 2;
 * push ecx; call 0x003D6870; ret`. BlockUntilIdle: a wait with mode 2 on the device's NEXT fence value, the
 * same wait d3d8_present.c performs. The title discards the result (T549). */
static uint32_t handler_block_until_idle(void *context)
{
    (void)context;
    d3d8_gpu_fence_wait(d3d8_device_load32(D3D8_DEV_FENCE), 2u);
    return 0u;
}

static uint32_t handler_wait_vblank(void *context)
{
    (void)context;
    d3d8_gpu_wait_vblank();
    return 0u;
}

size_t d3d8_gpu_register(void)
{
    static const struct {
        uint32_t address;
        d3d8_fn handler;
    } handlers[] = {
        {0x003D6690u, handler_kick},
        {0x003D67B0u, handler_fence_insert},
        {0x003D3630u, handler_insert_fence_and_kick},
        {0x003D34B0u, handler_block_until_fence},
        {0x003D34A0u, handler_block_until_idle},
        {0x003D6870u, handler_fence_wait},
        {0x003D3550u, handler_wait_vblank},
    };
    size_t registered = 0u;
    for (size_t index = 0u; index < sizeof(handlers) / sizeof(handlers[0]); index++) {
        if (d3d8_hle_register(handlers[index].address, handlers[index].handler)) {
            registered++;
        }
    }
    return registered;
}
