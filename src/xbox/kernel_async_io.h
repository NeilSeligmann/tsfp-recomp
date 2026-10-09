/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Asynchronous (overlapped) NtReadFile completion on the virtual clock (T743).
 *
 * WHY. The title's file service `sub_000294F0` (pump, once per frame from `sub_0003C440`
 * through `0x62B60`) issues `ReadFile` `0x37CC08` with an OVERLAPPED. The wrapper stores
 * STATUS_PENDING (0x103) in OVERLAPPED.Internal, calls NtReadFile with the OVERLAPPED's own
 * hEvent and its Internal/InternalHigh pair as the IoStatusBlock, and treats a status other than
 * STATUS_PENDING as "completed": it returns TRUE. The pump only ever advances on the PENDING
 * shape: ReadFile FALSE with ERROR_IO_PENDING (0x3E5) moves the request to state 2 and a later
 * GetOverlappedResult(wait 0) `0x37EA14` moves it on once Internal is no longer 0x103. A
 * ReadFile that returns TRUE leaves the request in state 1 and the pump issues the same read
 * again every frame, so `sub_000294B0` (the stream's "read finished" test) never turns true and
 * the stream never reaches `Process`. Measured: 3,097 identical reads of `\pak\musicts.pak` in
 * 3,605 frames, the request's remaining-bytes word [0x66139C] 0x12000 and its state word
 * [0x6613A4] 1 at every present (docs/boot-frontier.md, T743).
 *
 * WHAT THE MODEL IS. With `kernel_async_io_set_enabled(true)` an NtReadFile that supplies an Event
 * on a handle opened WITHOUT FILE_SYNCHRONOUS_IO_ALERT/NONALERT (an asynchronous handle, the
 * title's `CreateFile(.., FILE_FLAG_OVERLAPPED)`) is queued: the data is read from the backing
 * at issue (file position semantics are the synchronous ones), the guest buffer, the
 * IoStatusBlock and the Event are NOT touched, the call returns STATUS_PENDING. The request
 * completes later on a serial drive (one request at a time, in issue order): the guest buffer is
 * filled, IoStatusBlock {STATUS_SUCCESS, bytes} is written and the Event is set. The service
 * runs at every virtual vblank (`kernel_clock_set_frame_hook`) and at every NtReadFile.
 * A nonqueued synchronous NtReadFile follows the same successful publication order: buffer,
 * IOSB, then a supplied Event HANDLE. Failed reads return before this success-only signal path.
 *
 * XEMU-LEVEL timing, not hardware (T763). The PENDING answer is not a timing guess: the title's own
 * code only accepts it. The completion TIME comes from a measurement of xemu v0.8.136 (the T763
 * fixture `tests/fixtures/t763_ntreadfile_probe`, `docs/t763-ntreadfile-xemu.md`): a request takes
 * access + bytes / rate of virtual clock time, with the access and the rate PER VOLUME
 * (KERNEL_ASYNC_IO_DISC_*, KERNEL_ASYNC_IO_HDD_*), the volume taken from the file's path. xemu is an
 * emulator with its own drive model on host time, so these are xemu-level numbers (a spread of
 * about 0.06 to 7.5 ms per 0x12000 bytes, median 0.35 to 0.5 ms), NOT a DVD-ROM or a hard disk.
 * Opt-in (`--async-file-io`), default off, so every default boot is byte identical.
 *
 * WHAT xemu DOES NOT SHOW: its guest kernel finishes the read BEFORE NtReadFile returns (status
 * SUCCESS, IoStatusBlock written, Event set, buffer filled, first poll sees all four), so the
 * PENDING window itself and the order of the IoStatusBlock write against the Event are not
 * measurable there (they differ by under 10 us of poll). The model keeps the PENDING answer for
 * the retail title and orders the completion as buffer, IoStatusBlock, Event.
 *
 * T764 adds four parts on top, each measured on the retail wrappers under Unicorn first
 * (tests/test_overlapped_waits_oracle.py, docs/boot-frontier.md "Waits on pending reads"):
 *
 *  1. BLOCKING WAITS (with `--async-file-io`, replaces the named abort of kernel_thread.c). The
 *     retail GetOverlappedResult `0x37EA14` with wait != 0 calls `0x3800BF` -> `0x380029` ->
 *     NtWaitForSingleObjectEx(handle, WaitMode 1, Alertable 0, NULL) on the OVERLAPPED's hEvent, or
 *     on the FILE HANDLE when hEvent is 0, and a ReadFile without an OVERLAPPED waits the same way
 *     through NtWaitForSingleObject(handle, 0, NULL) on STATUS_PENDING. A wait on the Event of a
 *     pending read (or the file object of an Event-less one) advances the virtual clock to the due
 *     time of the earliest request that would signal it, NEVER past it, completes everything due by
 *     then (the drive is serial, so earlier requests finish first) and returns. A finite relative
 *     timeout that ends first advances the clock to the deadline only and times out. A poll
 *     (timeout 0) never advances, it only completes what is already due. kernel_async_io_wait_*.
 *  2. EVENT-LESS READS (opt-in `--async-file-io-file-object`, INFERRED: the NT file object rule). An
 *     NtReadFile with Event 0 on an asynchronous handle is queued as well. The file object is the
 *     dispatcher object a waiter sees: cleared when the read is issued, set when an Event-less
 *     read completes (the NT I/O manager rule, the retail wrappers wait on the handle on exactly
 *     this assumption). Kept per file identity (kernel_object_file_identity), a duplicate handle
 *     shares it. The title's loader helper `sub_00028D50` and the XMV open and reads
 *     (`0x444A2D`, `0x444994`) issue reads this way. Off, an Event-less read stays synchronous.
 *  3. APC ROUTINE: REFUSED BY NAME. No retail NtReadFile call site passes one (all six push 0,
 *     measured) and there is no ReadFileEx, so delivery at an alertable wait is not needed by the
 *     title. With the flag on, an NtReadFile with an ApcRoutine on an asynchronous handle is
 *     refused with STATUS_NOT_IMPLEMENTED and counted (kernel_async_io_apc_refused), never
 *     silently completed. An alertable wait stays refused by kernel_thread.c.
 *  4. CANCELLATION: REFUSED BY NAME, by absence: NtCancelIo and NtQueueApcThread are not imported
 *     by the title (measured on the import table), so nothing can ask. A pending read whose handle
 *     is closed still completes.
 *
 * NOT MODELLED, named so a later task does not assume it: an ApcRoutine (refused above), an
 * alertable wait, KeWaitForSingleObject / KeWaitForMultipleObjects / NtWaitForMultipleObjectsEx
 * on a pending read's object (no retail caller), absolute timeouts, cancellation, a write.
 */

#ifndef TSFP_XBOX_KERNEL_ASYNC_IO_H
#define TSFP_XBOX_KERNEL_ASYNC_IO_H

#include <stdbool.h>
#include <stdint.h>

#include "kernel_call.h"

/* The volume a request is served from. Each has its own drive (T763: xemu measured both). */
typedef enum {
    KERNEL_ASYNC_IO_VOLUME_DISC = 0,
    KERNEL_ASYNC_IO_VOLUME_HDD = 1,
    KERNEL_ASYNC_IO_VOLUME_COUNT
} kernel_async_io_volume;

/* XEMU-LEVEL (T763, docs/t763-ntreadfile-xemu.md), NOT hardware: access time and byte rate per
 * volume, a line through the median time xemu v0.8.136 takes to complete an unbuffered overlapped
 * read of 0x800, 0x12000 and 0x40000 bytes (two idle boots, 160 STATUS_PENDING reads, fit by
 * tests/fixtures/t763_ntreadfile_probe/analyze.py --fit, rounded). The rate is nearly flat in
 * the data (xemu copies through host memory), the access term carries the time. Pinned inside the
 * measured spread of every size by tests/test_t763_xemu_ntreadfile.py. */
#define KERNEL_ASYNC_IO_DISC_ACCESS_US 115u
#define KERNEL_ASYNC_IO_DISC_BYTES_PER_SECOND 1000000000u
#define KERNEL_ASYNC_IO_HDD_ACCESS_US 85u
#define KERNEL_ASYNC_IO_HDD_BYTES_PER_SECOND 1200000000u
/* The largest read held in the queue, bytes. A larger one stays synchronous and is reported. */
#define KERNEL_ASYNC_IO_MAX_BYTES (16u * 1024u * 1024u)
/* How many requests can be pending at once. */
#define KERNEL_ASYNC_IO_QUEUE 32u

/* NtCreateFile/NtOpenFile options that make a handle synchronous. */
#define KERNEL_ASYNC_IO_SYNCHRONOUS_OPTIONS 0x30u
/* T785: FILE_NO_INTERMEDIATE_BUFFERING. xemu answers PENDING only with it (T763); the retail title opens
 * its overlapped files with 0x48, which has it. A buffered asynchronous handle completes inside the call. */
#define KERNEL_ASYNC_IO_NO_BUFFERING_OPTION 0x08u

typedef struct {
    uint32_t file_handle;
    uint32_t event_handle;
    kernel_guest_ptr io_status;
    kernel_guest_ptr buffer;
    /* Host copy of the bytes, `length` of them, allocated with malloc. Ownership passes to the
     * queue when kernel_async_io_submit returns true. */
    uint8_t *data;
    uint32_t length;
    uint32_t requested;
    /* The drive that serves it (kernel_async_io_volume_of_path). */
    kernel_async_io_volume volume;
} kernel_async_read;

typedef struct {
    unsigned submitted;
    unsigned completed;
    unsigned pending;
    unsigned refused;
    unsigned failed;
    uint64_t bytes;
    /* T764: requests queued without an Event (the file object is signalled), blocking waits that
     * advanced the clock, waits that timed out, and ApcRoutine reads refused by name. */
    unsigned file_object_reads;
    unsigned waits;
    unsigned wait_timeouts;
    unsigned apc_refused;
    /* T821: requests completed by kernel_async_io_complete_next (the spin rule, INFERRED). */
    unsigned spin_completions;
    /* Interactive elapsed-time service, INFERRED host-to-model timing. */
    unsigned elapsed_completions;
    unsigned elapsed_calls;
    unsigned elapsed_backwards;
    unsigned elapsed_rebases;
} kernel_async_io_stats;

void kernel_async_io_set_enabled(bool enabled);
bool kernel_async_io_enabled(void);

/* T764: also queue a read that supplies NO Event (the file object is signalled instead). Needs the
 * base flag. The enabled flag stays when this is changed. */
void kernel_async_io_set_file_object_enabled(bool enabled);
bool kernel_async_io_file_object_enabled(void);

/* The read is queued, not completed, when enabled, `open_options` carries neither synchronous bit, has
 * FILE_NO_INTERMEDIATE_BUFFERING (T785, a buffered asynchronous read completes SUCCESS inside the call)
 * and either `event_handle` is non-zero or the file object model is enabled. */
bool kernel_async_io_eligible(uint32_t open_options, uint32_t event_handle);

/* T764: true (and counted) when an NtReadFile with `apc_routine` on this handle is REFUSED: the
 * model is enabled, the handle is asynchronous and an ApcRoutine was supplied. */
bool kernel_async_io_apc_refused(uint32_t open_options, uint32_t apc_routine);

/* Queue the read. False (and ownership stays with the caller) when the queue is full. */
bool kernel_async_io_submit(kernel_async_read *request);

/* Complete every request whose time has come on the virtual clock. Returns how many. */
unsigned kernel_async_io_service(void);

/* Interactive-only caller supplies CLOCK_MONOTONIC nanoseconds. While reads are
 * pending, anchor wall/model epochs and advance to actual elapsed model ticks,
 * never backwards and never force an individual request due. Empty/new queues
 * start fresh epochs; external model runahead re-anchors at its observed floor
 * without adding another elapsed delta, so a continuously pending queue progresses.
 * Disabled model or backwards timestamps do not advance time. Default/evidence
 * callers retain kernel_async_io_service without this INFERRED timing policy. */
unsigned kernel_async_io_service_elapsed(uint64_t monotonic_nanoseconds);
/* T1289: true when no request is queued (a lock free read). The safepoint skips the clock read and service_elapsed then. */
bool kernel_async_io_idle(void);

/* kernel_clock_frame_hook shaped: run the service once, ignoring the count. */
void kernel_async_io_service_hook(void);

/* The volume a guest path is on: the HDD for \Device\Harddisk* and the C: E: F: X: Y: Z: drive
 * letters (in any of the \??\ , \DosDevices\ or bare forms), the disc for everything else
 * (D:, \Device\CdRom*, a relative path). */
kernel_async_io_volume kernel_async_io_volume_of_path(const char *path);

/* Virtual clock ticks a request of `bytes` on `volume` takes, access included. */
uint64_t kernel_async_io_service_ticks(kernel_async_io_volume volume, uint32_t bytes);

/* T764: the result of a blocking wait. */
typedef enum {
    /* Nothing pending would signal the object: not this model's wait (the caller keeps its own
     * answer, a named refusal). */
    KERNEL_ASYNC_WAIT_NONE = 0,
    /* The object is signalled (an Event, or the file object). The caller still consumes an
     * auto-reset Event with its own try-wait. */
    KERNEL_ASYNC_WAIT_SATISFIED,
    /* The finite timeout ended first: the clock is at the deadline and nothing awaited was done. */
    KERNEL_ASYNC_WAIT_TIMED_OUT,
} kernel_async_wait;

/* Wait on the Event `handle` that a pending read will set, or on the file object of `file_handle`
 * (the file identity: a duplicate handle is the same file object). `finite` false is an infinite
 * wait, true a relative timeout of `timeout_100ns` (0 is a poll). Completes everything already
 * due first, then, only while the object is not signalled and a request that would signal it is
 * pending, advances the virtual clock to that request's due time (NEVER past it, and not past the
 * deadline of a finite wait) and completes what is due. Off (not enabled) it is NONE and does
 * nothing. */
kernel_async_wait kernel_async_io_wait_event(uint32_t handle, bool finite, uint64_t timeout_100ns);
kernel_async_wait kernel_async_io_wait_file(uint32_t file_handle, bool finite,
                                            uint64_t timeout_100ns);


/* T821 (INFERRED, opt-in through async_io_spin): how many requests are pending. */
unsigned kernel_async_io_pending(void);

/* T821: complete the EARLIEST pending request now. A guest that polls an IoStatusBlock or an
 * OVERLAPPED in its own memory (the retail pack loader `sub_00060050`: pump `0x294F0` then the
 * `0x294B0` finished test in a loop with no vblank and no NtReadFile in it) never reaches the two
 * places the service runs, so a pending read would never complete. This advances the virtual
 * clock to that request's due time, NEVER past it, and completes what is due, exactly what a
 * blocking wait does (T764) but for a poll. Returns false when nothing is pending or the model is
 * off. The caller decides WHEN (async_io_spin.c); counted in `spin_completions`. */
bool kernel_async_io_complete_next(void);

kernel_async_io_stats kernel_async_io_get_stats(void);

/* Drop every queued request (freeing the host copies) and the counters. The enabled flag stays. */
void kernel_async_io_reset(void);

#endif /* TSFP_XBOX_KERNEL_ASYNC_IO_H */
