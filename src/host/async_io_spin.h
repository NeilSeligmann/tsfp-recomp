/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T821: a pending overlapped read completes when the guest SPINS on it. OPT-IN (`--async-file-io-spin-complete N`),
 * INFERRED timing, default off, so every default boot is byte identical.
 *
 * THE GAP. The T743 asynchronous read model completes a queued NtReadFile on the virtual clock, and the clock services the
 * queue at every virtual vblank, at every NtReadFile and in a blocking wait. The retail PACK LOADER `sub_00060050` (the level and
 * data loader the front end starts after the player name, docs/boot-frontier.md row 27) does none of the three: it queues a
 * read (`0x29850`) and then loops `call 0x62B60` (the file service pump `sub_000294F0`) / `call 0x294B0` (finished?) until the
 * request's remaining byte count is zero. The pump's state 2 is `GetOverlappedResult(wait 0)` `0x37EA14`, lifted CRT code that
 * reads OVERLAPPED.Internal in guest memory, so it makes no HLE call. Nothing advances the virtual clock, Internal stays
 * STATUS_PENDING (0x103) for ever and the owner thread spins in lifted code with no HLE dispatch (measured: 523 s of wall
 * time with the watchdog, request state word 2, Internal 0x103, gdb on the live process).
 *
 * THE RULE. A guest thread that passes `threshold` consecutive cooperative safepoints (every lifted call) WITHOUT making an HLE
 * dispatch of its own, while a read is pending, completes the earliest pending read: the virtual clock advances to its due time,
 * never past it (`kernel_async_io_complete_next`, the blocking wait's rule for a poll). It stands for the CPU time the poll takes:
 * a lifted call is of the order of 100 to 200 cycles (INFERRED, a 733 MHz P3 class core), so 2,048 calls are about 0.3 to 0.6 ms,
 * the order of one 0x12000 byte request (access 0.115 ms plus bytes at 1 GB/s, xemu level T763). The threshold itself is
 * FABRICATED, a loop that does real work between dispatches for longer than that completes its read slightly early in virtual time.
 *
 * It reads and writes no guest word (the model writes the IoStatusBlock and the buffer exactly as for any completion) and never
 * runs while no read is pending.
 */

#ifndef TSFP_HOST_ASYNC_IO_SPIN_H
#define TSFP_HOST_ASYNC_IO_SPIN_H

#include <stdbool.h>
#include <stdint.h>

/** 0 turns the rule off (the default), otherwise the safepoints without an own dispatch that complete the earliest read. Clears the
 * per thread counts of the calling thread only; configure before the guest threads start. */
void async_io_spin_configure(unsigned threshold);
unsigned async_io_spin_threshold(void);

/** One cooperative safepoint of the calling thread (from the host's cooperative provider). No effect when off. */
void async_io_spin_note(void);

#endif /* TSFP_HOST_ASYNC_IO_SPIN_H */
