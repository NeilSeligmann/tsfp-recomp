/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1741: WRITE WATCH on guest memory (--watch-write), a census aid that logs WHO writes a guest range.
 *
 * The lifted code stores with plain host moves, so there is no store hook to enable. The watch therefore uses page
 * protection: the 4 KiB page(s) holding a watched range are made read-only, the first write to one faults
 * (SIGSEGV), the handler records the host PC, the fault address, the range bytes and a conservative scan of the host
 * stack, lets the single instruction run (trap flag, SIGTRAP) and re-protects. Cost when the flag is absent: none
 * (no handler, no protection, no thread, the lifted code is untouched). Cost when present: two signals per write
 * to a watched PAGE, whatever the offset, so the frame rate drops with the write traffic of that page.
 *
 * Ranges use the --dump-guest-range grammar (ADDR:LEN, *ADDR+OFF:LEN, **ADDR+OFF1+OFF2:LEN). Indirect ranges are
 * re-resolved every 20 ms by a poller thread and re-armed when the pointer chain changes, a null chain is simply
 * unarmed. The poller drains the bounded record ring, symbolizes the host PCs to lifted `sub_XXXXXXXX` names and
 * appends lines to the log. Limits: at most GUEST_WATCH_MAX_RANGES ranges of at most GUEST_WATCH_MAX_LENGTH bytes,
 * at most the given number of records (further hits are only counted), another thread that writes the same page in the
 * one-instruction window is missed, and a host syscall that writes into a watched page fails with EFAULT.
 */
#ifndef TSFP_HOST_GUEST_WATCH_H
#define TSFP_HOST_GUEST_WATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "guest_dump.h"

#define GUEST_WATCH_MAX_RANGES 4u
#define GUEST_WATCH_MAX_LENGTH 64u
#define GUEST_WATCH_DEFAULT_RECORDS 4096u
#define GUEST_WATCH_HARD_RECORDS 65536u

/** True when every range of `set` fits the watch limits (count 1..GUEST_WATCH_MAX_RANGES, length at most
 * GUEST_WATCH_MAX_LENGTH, so a range lies in at most two pages). `error` (may be NULL) gets the reason. */
bool guest_watch_check(const guest_dump_set *set, char *error, size_t error_size);

/** Pure filter: does a write that faulted at `fault` hit the watched range [low, low+length)? A store that changed the
 * range bytes (`changed`, compared before and after, so a wide store that started below the range counts) is a hit, an
 * unchanged range is a hit only when the store started inside it (a same value write). A neighbour write that leaves the
 * range alone is not. */
bool guest_watch_is_hit(uint32_t fault, uint32_t low, uint32_t length, bool changed);

/** Start watching. `log_path` is opened for append (NULL: stderr). `present` (may be NULL) gives the present index.
 * False (nothing armed) on a bad set, a log that cannot be opened or no handler. */
bool guest_watch_start(const guest_dump_set *set, const char *log_path, unsigned max_records, uint64_t (*present)(void),
                       uint64_t (*polls)(void));

/** The first-chance SIGSEGV hook of host_runtime (host_run_set_fault_hook). True when the fault was a write to a watched page
 * and was served. */
bool guest_watch_fault(int signal_number, void *info, void *context);

/** Disarm, restore the signal handlers and page protection, flush the remaining records and a `summary` line. Safe when
 * never started. */
void guest_watch_stop(void);

#endif /* TSFP_HOST_GUEST_WATCH_H */
