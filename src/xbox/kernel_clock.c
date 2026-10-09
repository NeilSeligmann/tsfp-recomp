/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The deterministic virtual time-stamp counter. See kernel_clock.h for the policy and the
 * measurements behind it.
 *
 * ONE MUTEX, NOT ATOMICS. A read is `max(now + step, floor)`, two dependent operations, and
 * a frame is an addition with a carry. A lock-free form would need a compare-exchange loop
 * for the read and a second one for the frame, to save a lock that is taken a handful of
 * times per frame. The counter this replaces was a bare `static uint64_t` incremented by
 * every guest thread with no exclusion at all, which loses updates on a multi-core host and
 * so was not even monotonic across threads.
 */

#include "kernel_clock.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include "kernel_call.h"
#include "kernel_hle.h"

#define MAX_REFRESH_HZ 1000u

static pthread_mutex_t clock_lock = PTHREAD_MUTEX_INITIALIZER;
/* T1289: atomic so kernel_clock_peek needs no lock (every XDK call and safepoint peeks). Writers still hold clock_lock and both values only
 * ever grow (reset aside), so a peek that sees one of them older than the other still returns a value no older than any earlier peek. */
static _Atomic uint64_t now_ticks;
static _Atomic uint64_t floor_ticks;
/* Remainder of FREQUENCY / refresh, accumulated in units of 1/refresh tick. Kept against the
 * rate that produced it, so a change of rate starts a fresh remainder instead of
 * mixing denominators. */
static uint64_t frame_carry;
static unsigned carry_rate;
/* Sub-tick remainder of the stalls, in millionths of a tick (the stall's own denominator is
 * 1,000,000 microseconds per second). Carried so that 1,000,000 stalls of 1 us add exactly
 * one second. */
static uint64_t stall_carry;
static uint32_t tick_count_address;
static kernel_clock_frame_hook frame_hook;
/* T1237: where guest time came from, for the timing report (host observation only, nothing here feeds back). */
static kernel_clock_stats stats;

void kernel_clock_set_frame_hook(kernel_clock_frame_hook hook)
{
    pthread_mutex_lock(&clock_lock);
    frame_hook = hook;
    pthread_mutex_unlock(&clock_lock);
}

/* Called under the clock lock. Only the observed ULONG at the export address
 * is modelled: GetTickCount at 0x0037EB7C reads exactly this dword. */
static bool publish_tick_count(void)
{
    const uint64_t ticks = now_ticks > floor_ticks ? now_ticks : floor_ticks;
    const uint32_t ms = (uint32_t)((ticks / KERNEL_CLOCK_FREQUENCY_HZ) * 1000u +
        (ticks % KERNEL_CLOCK_FREQUENCY_HZ) * 1000u / KERNEL_CLOCK_FREQUENCY_HZ);
    return tick_count_address == 0u || kernel_guest_write_u32(tick_count_address, ms);
}

bool kernel_clock_bind_tick_count(uint32_t address)
{
    if (address != 0u && kernel_guest_at(address, sizeof(uint32_t)) == NULL) {
        return false;
    }
    pthread_mutex_lock(&clock_lock);
    tick_count_address = address;
    const bool okay = publish_tick_count();
    pthread_mutex_unlock(&clock_lock);
    return okay;
}

uint64_t kernel_clock_read(void)
{
    pthread_mutex_lock(&clock_lock);
    stats.reads++;
    now_ticks += KERNEL_CLOCK_READ_STEP_TICKS;
    if (now_ticks < floor_ticks) {
        now_ticks = floor_ticks;
    } else {
        stats.reads_above_floor++;
    }
    (void)publish_tick_count();
    const uint64_t reading = now_ticks;
    pthread_mutex_unlock(&clock_lock);
    return reading;
}

bool kernel_clock_frame_floor(unsigned refresh_hz, uint64_t *floor_after)
{
    if (refresh_hz == 0u || refresh_hz > MAX_REFRESH_HZ) {
        return false;
    }
    pthread_mutex_lock(&clock_lock);
    if (carry_rate != refresh_hz) {
        carry_rate = refresh_hz;
        frame_carry = 0u;
    }
    stats.frames++;
    floor_ticks += KERNEL_CLOCK_FREQUENCY_HZ / refresh_hz;
    frame_carry += KERNEL_CLOCK_FREQUENCY_HZ % refresh_hz;
    if (frame_carry >= refresh_hz) {
        frame_carry -= refresh_hz;
        floor_ticks++;
    }
    (void)publish_tick_count();
    if (floor_after != NULL) {
        *floor_after = floor_ticks;
    }
    const kernel_clock_frame_hook hook = frame_hook;
    pthread_mutex_unlock(&clock_lock);
    if (hook != NULL) {
        hook();
    }
    return true;
}

bool kernel_clock_frame(unsigned refresh_hz)
{
    return kernel_clock_frame_floor(refresh_hz, NULL);
}

uint64_t kernel_clock_stall_us(uint32_t microseconds)
{
    if (microseconds > KERNEL_CLOCK_STALL_MAX_US) {
        kernel_hle_log()("kernel: KeStallExecutionProcessor(%u) refused: above the %u us "
                         "bound (measured maximum 10000), clock not advanced\n",
                         microseconds, KERNEL_CLOCK_STALL_MAX_US);
        return 0u;
    }
    pthread_mutex_lock(&clock_lock);
    /* From the later of the counter and the floor, or a stall below the floor is swallowed
     * by the next read's max(). */
    if (now_ticks < floor_ticks) {
        now_ticks = floor_ticks;
    }
    const uint64_t scaled = (uint64_t)microseconds * KERNEL_CLOCK_FREQUENCY_HZ + stall_carry;
    const uint64_t ticks = scaled / 1000000u;
    stall_carry = scaled % 1000000u;
    stats.stall_ticks += ticks;
    now_ticks += ticks;
    (void)publish_tick_count();
    pthread_mutex_unlock(&clock_lock);
    return ticks;
}

uint64_t kernel_clock_advance_to(uint64_t ticks)
{
    pthread_mutex_lock(&clock_lock);
    if (now_ticks < floor_ticks) {
        now_ticks = floor_ticks;
    }
    if (now_ticks < ticks) {
        stats.advance_to_ticks += ticks - now_ticks;
        stats.advance_to_calls++;
    }
    if (now_ticks < ticks) {
        now_ticks = ticks;
    }
    (void)publish_tick_count();
    const uint64_t reading = now_ticks;
    pthread_mutex_unlock(&clock_lock);
    return reading;
}

kernel_clock_stats kernel_clock_get_stats(void)
{
    pthread_mutex_lock(&clock_lock);
    kernel_clock_stats copy = stats;
    copy.total_ticks = now_ticks > floor_ticks ? now_ticks : floor_ticks;
    copy.floor_ticks = floor_ticks;
    pthread_mutex_unlock(&clock_lock);
    return copy;
}

uint64_t kernel_clock_peek(void)
{
    const uint64_t now = atomic_load_explicit(&now_ticks, memory_order_relaxed);
    const uint64_t floor = atomic_load_explicit(&floor_ticks, memory_order_relaxed);
    return now > floor ? now : floor;
}

uint64_t kernel_clock_frequency(void)
{
    return KERNEL_CLOCK_FREQUENCY_HZ;
}

uint32_t kernel_clock_tick_count_ms(void)
{
    const uint64_t ticks = kernel_clock_peek();
    /* Divide first to avoid overflowing a full-width TSC before truncating to ULONG. */
    return (uint32_t)((ticks / KERNEL_CLOCK_FREQUENCY_HZ) * 1000u +
        (ticks % KERNEL_CLOCK_FREQUENCY_HZ) * 1000u / KERNEL_CLOCK_FREQUENCY_HZ);
}

void kernel_clock_reset(void)
{
    pthread_mutex_lock(&clock_lock);
    now_ticks = 0u;
    floor_ticks = 0u;
    frame_carry = 0u;
    carry_rate = 0u;
    memset(&stats, 0, sizeof stats);
    stall_carry = 0u;
    (void)publish_tick_count();
    pthread_mutex_unlock(&clock_lock);
}

/* The title stores EDX:EAX after 126 at 0x004414EE and consumes both halves
 * after 127 at 0x00441502. No guest stack arguments (nxdk @0 and hand count). */
static uint32_t clock_result(void *context, uint64_t value)
{
    kernel_call_frame *frame = (kernel_call_frame *)context;
    if (frame != NULL) {
        frame->result_high = (uint32_t)(value >> 32);
        frame->has_result_high = true;
    }
    return (uint32_t)value;
}

static uint32_t hle_performance_counter(void *context)
{
    return clock_result(context, kernel_clock_read());
}

static uint32_t hle_performance_frequency(void *context)
{
    return clock_result(context, kernel_clock_frequency());
}

/* VOID export: EAX is not read by any measured caller, so 0 by convention. A frame that
 * cannot supply the argument is reported, never read as 0 (a zero stall is a real value). */
static uint32_t hle_stall_execution_processor(void *context)
{
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t microseconds = 0u;
    if (frame == NULL || !kernel_frame_arg(frame, 0u, &microseconds)) {
        kernel_hle_log()("kernel: KeStallExecutionProcessor could not read its MicroSeconds "
                         "argument from the guest stack, clock not advanced\n");
        return 0u;
    }
    (void)kernel_clock_stall_us(microseconds);
    return 0u;
}

unsigned kernel_clock_register(void)
{
    unsigned bound = 0u;
    if (kernel_hle_register(151u, hle_stall_execution_processor)) {
        bound++;
    }
    if (kernel_hle_register(126u, hle_performance_counter)) {
        bound++;
    }
    if (kernel_hle_register(127u, hle_performance_frequency)) {
        bound++;
    }
    return bound;
}
