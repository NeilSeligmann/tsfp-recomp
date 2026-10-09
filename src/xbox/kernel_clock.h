/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The guest's ONE time base: a deterministic virtual time-stamp counter.
 *
 * WHY ONE. The title reads time three ways and, on hardware, they are the same crystal:
 * `rdtsc` directly (sub_00153890 and its 20 call sites, sub_00153B00, sub_001538C0, the D3D
 * ISR timer at sub_003DC0D0), its own QueryPerformanceCounter, which IS `rdtsc`
 * (sub_003D1263, MEASURED, 5 callers), and the kernel's KeQueryPerformanceCounter
 * (ordinal 126, one site, in XNET). A policy that gave them different clocks would let a
 * guest that cross-checks one against another see a console that does not exist.
 *
 * THE RATE IS THE TITLE'S OWN ASSUMPTION, MEASURED. 733,333,333 Hz (0x2BB5C755) is
 * an immediate at four places: the QueryPerformanceFrequency body (0x003D127C) and the
 * three seconds conversions at 0x001538A6, 0x001539BE and 0x00153B16. Nothing else in the
 * image carries a CPU frequency (no float or double of 733.33e6 or its reciprocal).
 *
 * WHAT ADVANCES IT, and why two things:
 *
 *   1. EVERY READ CREEPS BY KERNEL_CLOCK_READ_STEP_TICKS. This is the old counter's rule and
 *      it is kept on purpose: a guest that polls the clock for a deadline must make
 *      progress with no frame ever completing, or the poll is a guaranteed hang. 1000 ticks
 *      is 1.36 microseconds.
 *   2. EVERY FRAME RAISES A FLOOR by exactly one refresh period (kernel_clock_frame), called
 *      from the virtual vblank. The clock never reads below the floor, so after N frames at
 *      60 Hz it has advanced N/60 of a second to the tick, which is what the title's own
 *      seconds conversion (ticks / 733333333) then recovers.
 *
 * With no frame ever reported the counter is EXACTLY the old one (1000 per read), which is
 * what keeps every existing baseline where it was. THE CLOCK IS NOT WALL TIME: it depends only
 * on the sequence of reads and frames, so a run replays.
 *
 * KeQueryPerformanceCounter (126) and KeQueryPerformanceFrequency (127) share
 * this clock through EDX:EAX results. The observed ULONG of KeTickCount (156)
 * is published to its bound DATA export address. KeQuerySystemTime (128) remains the
 * host wall clock; it is calendar time, separate from elapsed CPU ticks.
 */

#ifndef TSFP_XBOX_KERNEL_CLOCK_H
#define TSFP_XBOX_KERNEL_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

/* The Xbox CPU clock the title assumes, in Hz. */
#define KERNEL_CLOCK_FREQUENCY_HZ 733333333u

/* How far one read advances the counter when no frame has. Unchanged from the counter
 * this replaces (src/host/recomp_runtime.c before this module), so a run with no vblank
 * is bit-identical to what it was. */
#define KERNEL_CLOCK_READ_STEP_TICKS 1000u

/* The virtual time-stamp counter. Strictly increasing across every caller and every
 * thread: it creeps by KERNEL_CLOCK_READ_STEP_TICKS per read and never reads below the
 * frame floor. The first read of a fresh clock returns KERNEL_CLOCK_READ_STEP_TICKS. */
uint64_t kernel_clock_read(void);

/* One vertical blank at `refresh_hz` (60 or 50 for this title): raise the floor by
 * KERNEL_CLOCK_FREQUENCY_HZ / refresh_hz ticks, with the remainder carried so that
 * `refresh_hz` frames add exactly KERNEL_CLOCK_FREQUENCY_HZ.
 *
 * Returns false and changes nothing for a rate outside 1..1000, because a zero or garbage
 * rate would otherwise divide by zero or stall time while looking like progress. The caller
 * should announce that. */
bool kernel_clock_frame(unsigned refresh_hz);

/* T743: a function called after every vertical blank raised the floor (outside the clock lock), so
 * a device model that completes work on the virtual clock (kernel_async_io) is serviced once per
 * frame. NULL removes it. One hook, set by the host at startup. */
typedef void (*kernel_clock_frame_hook)(void);
void kernel_clock_set_frame_hook(kernel_clock_frame_hook hook);

/* kernel_clock_frame that also reports the raised floor, read in the same critical section.
 * The floor is the virtual time of the blank itself: it depends only on the frames
 * reported, never on how many reads any thread made, so it is a deterministic timestamp for
 * the vblank (T372). `floor_after` may be NULL. */
bool kernel_clock_frame_floor(unsigned refresh_hz, uint64_t *floor_after);

/* The counter without advancing it, for a reader that must not perturb the clock. */
uint64_t kernel_clock_peek(void);

/* KERNEL_CLOCK_FREQUENCY_HZ, as the 64-bit value KeQueryPerformanceFrequency returns. */
uint64_t kernel_clock_frequency(void);

/* The counter in whole milliseconds (INFERRED to be KeTickCount's unit: the Xbox system
 * timer ticks at 1 ms). Does not advance the clock. Truncates to 32 bits like the guest's
 * ULONG. */
uint32_t kernel_clock_tick_count_ms(void);

/* KeStallExecutionProcessor (ordinal 151) in virtual time.
 *
 * `VOID __stdcall KeStallExecutionProcessor(ULONG MicroSeconds)`: ONE stack argument, callee
 * pops it (`ret 4`). MEASURED at all 15 sites (unanimous row {151, 1, 15}, then hand-checked
 * in the disassembly): each pushes exactly one value, either a literal (1, 10, 0x14, 0x32,
 * 0x2710) or a register (edi, ebx), and none reads EAX afterwards. 9 sites are
 * `call [0x475920]`, the other 6 are `call esi/ebx` through a register loaded from the thunk
 * slot (XNET 0x0043A394, 0x0043A3B0, 0x0043A3E3, 0x0043A3EF, 0x0043AF7D, 0x0043AF99). The
 * register-indirect ones are invisible to a direct-call grep. The nxdk .def row is
 * KeStallExecutionProcessor@4, so the oracle agrees. EVERY measured caller is a hardware
 * poll or settle delay (a bounded counter loop around an MMIO test, or a fixed settle after an
 * MMIO write), the largest literal being 0x2710 = 10,000 microseconds.
 *
 * WHAT IT DOES HERE. It does NOT spin and does not sleep: it advances THE ONE VIRTUAL CLOCK by
 * the requested time, so a guest that stalls 10 ms and then reads `rdtsc`, QPC or KeTickCount
 * sees at least 10 ms go by, at zero host cost, identically on every run. The conversion is
 * exact: microseconds * 733,333,333 / 1,000,000 ticks, with the sub-tick remainder CARRIED so
 * that 1,000,000 stalls of 1 us add exactly one second. The stall starts from the later of the
 * counter and the frame floor, because a stall that began below the floor would otherwise
 * be absorbed by the next read's `max(now, floor)` and advance nothing.
 *
 * REFUSED LOUDLY: a request above KERNEL_CLOCK_STALL_MAX_US (1 second, INFERRED sanity bound;
 * the measured maximum is 10 ms) advances nothing and reports. A garbage argument would
 * otherwise jump every time base by hours with no trace. Zero is accepted and advances
 * nothing.
 *
 * Returns the ticks added (0 when refused), for tests; the export itself returns VOID. */
#define KERNEL_CLOCK_STALL_MAX_US 1000000u
uint64_t kernel_clock_stall_us(uint32_t microseconds);

/* T764: advance the counter to AT LEAST `ticks` without the per-read creep, for a wait on a device
 * model that completes work on this clock (kernel_async_io). The counter becomes the later of
 * itself, the frame floor and `ticks`, never less, so it is monotone and a deadline in the past
 * advances nothing. Returns the counter afterwards (equal to `ticks` when `ticks` was ahead of
 * both). The sub-tick stall remainder is untouched. */
uint64_t kernel_clock_advance_to(uint64_t ticks);

/* Back to a fresh clock. For explicit session restart and tests, never device recreation. */
void kernel_clock_reset(void);

/* Register the performance exports 126 and 127 and the stall export 151. */
unsigned kernel_clock_register(void);

/* Bind the observed 32-bit KeTickCount DATA export to writable guest memory.
 * Zero detaches it before its mapping is released. Read/frame/reset publishes
 * milliseconds without perturbing the counter. Mapping must outlive the binding. */
bool kernel_clock_bind_tick_count(uint32_t address);

/* T1237: where the guest's time came from. total_ticks is what a reader sees, floor_ticks is the frames' share
 * (frames * 733,333,333 / refresh), the rest is creep above the floor (reads_above_floor reads at
 * KERNEL_CLOCK_READ_STEP_TICKS each), KeStallExecutionProcessor and waits that advanced the clock to a due time. */
typedef struct kernel_clock_stats {
    uint64_t total_ticks, floor_ticks;
    uint64_t reads, reads_above_floor, frames;
    uint64_t stall_ticks, advance_to_ticks, advance_to_calls;
} kernel_clock_stats;
kernel_clock_stats kernel_clock_get_stats(void);

#endif /* TSFP_XBOX_KERNEL_CLOCK_H */
