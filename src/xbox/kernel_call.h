/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The HLE call boundary: how a kernel handler reaches its arguments.
 *
 * CALLING CONVENTION. MOST xboxkrnl exports are __stdcall, but NOT ALL -- an
 * earlier version of this comment claimed "every" and was wrong, which mattered
 * because the single most-called ordinal in the image is one of the exceptions.
 *
 * The `Kf*` family is __fastcall, taking its first argument in ECX. MEASURED at
 * real call sites: `KfLowerIrql` (ordinal 161, **133 call sites, the most of any
 * ordinal**) is reached by `mov cl, al` / `mov cl, bl` with nothing pushed, while
 * `RtlLeaveCriticalSection` (ordinal 294, 114 sites) is reached by `push ebx`.
 * `KeRaiseIrqlToDpcLevel` (129, 112 sites) takes no argument at all and returns the
 * old IRQL in AL.
 *
 * A fastcall handler that read `kernel_frame_arg(0)` would get the return address
 * or a saved register and never know. Use `kernel_frame_reg_arg()` for those, and
 * see `context.md` §6p for the full per-ordinal call-site ranking.
 *
 * THE `f` IS THE TELL, AND IT IS CORROBORATED BY MEASUREMENT. `Kf*` and `Obf*` are
 * the fastcall families. `tools/lift/callsites.py`, which counts the guest's own
 * argument pushes, independently reports **0 stack arguments for
 * `ObfDereferenceObject` (ordinal 250) across 18 call sites** — exactly what a
 * one-argument fastcall looks like from the stack's point of view. A handler that
 * believed that zero would dereference nothing at all.
 *
 * For a __stdcall export, at the instant the guest's CALL transfers control the
 * stack looks like this, with ESP pointing at the return address and arguments
 * pushed right-to-left above it:
 *
 *     ESP+0x00   return address
 *     ESP+0x04   argument 0   (leftmost in the C declaration)
 *     ESP+0x08   argument 1
 *     ...
 *
 * Callee-cleanup (the RET imm16) is the caller's problem, not ours: a recompiled
 * guest does not execute a real RET into our handler, it calls us and then adjusts
 * its own modelled ESP. So this header only has to describe argument *fetch*.
 *
 * WHY NOT CHANGE kernel_fn. `kernel_fn` stays `uint32_t (*)(void *context)` and
 * this module defines what `context` points at: a kernel_call_frame. That keeps
 * one dispatch signature for all 371 ordinals regardless of arity, which is the
 * property that makes a table of them possible at all. Widening kernel_fn to a
 * fixed argument count would be a guess -- ordinals range from zero-argument
 * (KeQueryPerformanceCounter) to eight-plus (NtCreateFile) -- and a variadic
 * signature would lose the type information it pretended to add.
 *
 * KNOWN LIMITATIONS of this boundary, on the record:
 *   - Integer results use the handler's uint32_t return for EAX; a 64-bit result
 *     also sets result_high and has_result_high in the frame for EDX. Dispatch
 *     preserves EDX when has_result_high is false. A float return in ST0 still
 *     has no result field in this boundary.
 *   - Arguments are read from guest memory on demand. Repeated reads can observe
 *     intervening guest-memory changes; the frame is not a stack snapshot.
 *   - No argument count is carried. A handler knows its own arity from the
 *     function it implements; the frame cannot catch a caller that pushed too few.
 *     `stack_limit` bounds the damage to a reported error rather than a wild read.
 */

#ifndef TSFP_XBOX_KERNEL_CALL_H
#define TSFP_XBOX_KERNEL_CALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "kernel_hle.h"

/**
 * One kernel call's view of the guest stack.
 *
 * Holds a guest address, never a host pointer, because a four-byte guest pointer
 * cannot hold a host one. Under the identity mapping the conversion is a
 * zero-extension, but it goes through kernel_guest_at() so there is a single place
 * to change if the memory model ever stops being identity-mapped.
 */
typedef struct {
    /* Guest ESP at entry: points at the return address, so argument N is at
     * stack_ptr + 4 + 4*N. */
    kernel_guest_ptr stack_ptr;
    /* One past the last readable stack byte, or 0 for "unbounded". A recompiler
     * knows the guest thread's stack extent and should supply it, which turns a
     * wrong-arity call into a reported error instead of a read of whatever
     * happened to be above the stack. */
    kernel_guest_ptr stack_limit;
    /* ECX and EDX at entry, for the __fastcall `Kf*` exports. Carried in the frame
     * rather than fetched, because by the time a handler runs the recompiled guest's
     * modelled registers may already have moved on. A stdcall handler ignores these
     * and a fastcall one ignores the stack, so one frame still serves both. */
    uint32_t ecx;
    uint32_t edx;
    /* Whether ecx/edx were actually supplied. Without this, a caller that forgot to
     * set them is indistinguishable from one passing a legitimate zero -- and a
     * zero IRQL is a perfectly plausible value, which is exactly the kind of wrong
     * answer that surfaces far from its cause. */
    bool has_registers;
    /* Optional EDX half of a 64-bit result. Dispatch preserves EDX otherwise. */
    uint32_t result_high;
    bool has_result_high;
} kernel_call_frame;

/**
 * Translate a guest address to a host pointer, or NULL.
 *
 * Rejects address 0, any range that would run past the 4 GB guest limit, and any range
 * with an UNMAPPED or UNREADABLE page in it (a process_vm_readv probe, see
 * kernel_call.c: the host MMU is the guest page table, so the kernel is asked rather
 * than the host faulted). A PROT_NONE guard page is mapped but unreadable, so it is
 * refused rather than handed back as a pointer that faults on first use (T81). Two
 * limits remain. Readability does not prove writability, so a read-only page passes
 * and a write through the returned pointer can still fault. And the probe is a check,
 * not a lock, so a concurrent unmap or re-protect can invalidate the answer before the
 * caller uses it. Both residual faults are caught by the armed structured fault
 * handler (host_runtime.c sigsetjmp machinery) and become a stop record rather than a
 * bare crash. A zero-length range is not probed.
 */
void *kernel_guest_at(kernel_guest_ptr addr, size_t length);

/* T1153: the REAL pid of this process for process_vm_readv/writev on itself (the raw system call, uncached, when
 * the host runs under the DMTCP snapshot layer, whose getpid() is virtual). Cached otherwise. */
pid_t kernel_host_pid(void);

/**
 * Is every page of [addr, addr + length) readable right now, without faulting?
 *
 * The probe behind kernel_guest_at(), exported for callers that need the answer for a
 * range they will not immediately translate (kernel_xe.c checks a whole section body).
 * Same contract and the same two residual limits as above: readable is not writable,
 * and the answer can be invalidated by a concurrent unmap. A zero-length range reports
 * true without asking the kernel, because it names no byte.
 */
bool kernel_guest_range_readable(kernel_guest_ptr addr, size_t length);

/* T819: the probe remembers readable pages per thread. Whoever unmaps a guest range or changes a guest page's
 * protection calls this afterwards so a remembered page cannot outlive its mapping. */
void kernel_guest_probe_cache_flush(void);
/* T827: call BEFORE changing the protection of guest pages (munmap, mprotect), then kernel_guest_probe_cache_flush
 * after. Stops the syscall-free fast read path and waits for the reads in flight. */
void kernel_guest_probe_change_begin(void);
/* T827 counters: guest reads and writes done with a plain copy, and the ones that needed a system call. */
void kernel_guest_copy_stats(uint64_t *fast_reads, uint64_t *fast_writes, uint64_t *syscalls);
/* Probe counters for the stop report and tests: pages answered from the cache, probe system calls made. */
void kernel_guest_probe_stats(uint64_t *cache_hits, uint64_t *syscalls);
/* T1289: the probe cache misses by cause (cold, conflict, stale) and the number of epoch flushes (guest page changes). */
void kernel_guest_probe_miss_stats(uint64_t *cold, uint64_t *conflict, uint64_t *stale, uint64_t *epochs);

/**
 * `base + offset` in guest space, or 0 when `base` is 0 or the sum leaves 32 bits.
 *
 * Plain `uint32_t` addition wraps: `0xFFFFFFFE + 4` is 2, a low address the accessors
 * accept as in range. Zero is refused by every accessor, so a caller that builds its
 * field addresses with this needs no separate overflow check.
 */
kernel_guest_ptr kernel_guest_add(kernel_guest_ptr base, uint32_t offset);

/**
 * Guarded copies through the host kernel, respecting current read/write permissions.
 * Return true only for a complete transfer; errors and short transfers return false.
 * On failure a bulk read destination or guest write range may already contain a
 * transferred prefix. Transfers and compound operations are not atomic, and mapping
 * lifetime is not pinned between calls. No fallback or fabricated bytes are supplied.
 * Null guest or host addresses and ranges beyond 4 GB are refused. A zero-length
 * transfer with non-null addresses succeeds without inspecting permissions/mapping.
 * Host buffers must remain valid for the requested length during the call.
 */
bool kernel_guest_read_bytes(kernel_guest_ptr addr, void *out, size_t length);
bool kernel_guest_write_bytes(kernel_guest_ptr addr, const void *source, size_t length);

/** Read a 32-bit value; false on failure, preserving *out. Unaligned reads are allowed. */
bool kernel_guest_read_u32(kernel_guest_ptr addr, uint32_t *out);

/** Write a 32-bit value; false on failure. A page-crossing write may partially transfer. */
bool kernel_guest_write_u32(kernel_guest_ptr addr, uint32_t value);

/**
 * Read a single byte from guest memory; false on failure, preserving *out.
 *
 * Byte width is not a convenience here. `KPCR.Irql` is a byte and the guest reads
 * it as one -- all 6 measured sites are `MEM8(XBOX_FS_BASE + 0x24)` -- so a
 * 32-bit accessor would both read three bytes that belong to other fields and
 * invite writing them.
 */
bool kernel_guest_read_u8(kernel_guest_ptr addr, uint8_t *out);

/** Write a single byte to guest memory. False when the address is unusable. */
bool kernel_guest_write_u8(kernel_guest_ptr addr, uint8_t value);

/**
 * Fetch stdcall argument `index` (0-based, leftmost first).
 *
 * False when the frame is NULL, has no stack pointer, or the slot falls outside
 * `stack_limit`. Handlers must treat that as a parameter error rather than
 * substituting a zero, because a zeroed argument to an allocator is a plausible
 * value and would fail somewhere else entirely.
 */
bool kernel_frame_arg(const kernel_call_frame *frame, unsigned index, uint32_t *out);

/**
 * Lay out a synthetic stdcall frame in a caller-supplied guest buffer.
 *
 * For tests, and for any caller that has arguments in registers rather than on a
 * modelled stack. Writes a zero return address followed by `arg_count` arguments,
 * then points `frame` at it. False if the buffer cannot hold (arg_count + 1)
 * slots, so a frame is never half-built.
 */
bool kernel_frame_build(kernel_call_frame *frame, kernel_guest_ptr buffer,
                        uint32_t buffer_bytes, const uint32_t *args, unsigned arg_count);

/**
 * Fetch __fastcall register argument `index`: 0 is ECX, 1 is EDX.
 *
 * False when the frame is NULL, carries no registers, or `index` exceeds 1 --
 * never a substituted zero, because zero is a legitimate value for every register
 * argument the kernel takes. A `Kf*` handler must use this and NOT
 * `kernel_frame_arg`, which would read the return address.
 */
bool kernel_frame_reg_arg(const kernel_call_frame *frame, unsigned index, uint32_t *out);

/** Attach fastcall register arguments to a frame, marking them as supplied. */
void kernel_frame_set_registers(kernel_call_frame *frame, uint32_t ecx, uint32_t edx);

#endif /* TSFP_XBOX_KERNEL_CALL_H */
