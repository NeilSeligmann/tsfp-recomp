/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * NTSTATUS codes and memory-manager constants used by the kernel HLE.
 *
 * These are interface constants, not implementation: the guest compares returned
 * status codes against the values its own headers were built with, so a wrong
 * value here is indistinguishable from a wrong implementation. They live in a
 * header rather than inline at each return so that the set is auditable and a
 * single value can be corrected in one place.
 *
 * NTSTATUS layout: bits 30-31 are severity, so every 0xC0000000-prefixed code is
 * an error and a success test is `(status & 0x80000000) == 0`.
 */

#ifndef TSFP_XBOX_NT_STATUS_H
#define TSFP_XBOX_NT_STATUS_H

#include <stdbool.h>
#include <stdint.h>

typedef uint32_t nt_status;

#define STATUS_SUCCESS 0x00000000u
#define STATUS_PENDING 0x00000103u
#define STATUS_NO_YIELD_PERFORMED 0x40000024u
#define STATUS_BUFFER_OVERFLOW 0x80000005u
#define STATUS_UNSUCCESSFUL 0xC0000001u
#define STATUS_NOT_IMPLEMENTED 0xC0000002u
#define STATUS_ACCESS_VIOLATION 0xC0000005u
#define STATUS_INVALID_HANDLE 0xC0000008u
#define STATUS_INVALID_PARAMETER 0xC000000Du
#define STATUS_NO_MEMORY 0xC0000017u
#define STATUS_END_OF_FILE 0xC0000011u
#define STATUS_CONFLICTING_ADDRESSES 0xC0000018u
#define STATUS_OBJECT_TYPE_MISMATCH 0xC0000024u
#define STATUS_INVALID_PAGE_PROTECTION 0xC0000045u
#define STATUS_MUTANT_NOT_OWNED 0xC0000046u
#define STATUS_MUTANT_LIMIT_EXCEEDED 0xC0000191u
#define STATUS_INSUFFICIENT_RESOURCES 0xC000009Au
#define STATUS_FREE_VM_NOT_AT_BASE 0xC000009Fu
#define STATUS_MEMORY_NOT_ALLOCATED 0xC00000A0u

/** True for any non-error NTSTATUS. */
static inline bool nt_success(nt_status status)
{
    return (status & 0x80000000u) == 0u;
}

/* Allocation type bits for NtAllocateVirtualMemory / NtFreeVirtualMemory. */
#define MEM_COMMIT 0x00001000u
#define MEM_RESERVE 0x00002000u
#define MEM_DECOMMIT 0x00004000u
#define MEM_RELEASE 0x00008000u
#define MEM_FREE 0x00010000u
#define MEM_PRIVATE 0x00020000u
#define MEM_RESET 0x00080000u
#define MEM_TOP_DOWN 0x00100000u
#define MEM_NOZERO 0x00800000u

#define MEM_ALLOCATION_TYPE_VALID (MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN | MEM_NOZERO | MEM_RESET)
#define MEM_FREE_TYPE_VALID (MEM_DECOMMIT | MEM_RELEASE)

/* Page protection values. Recorded and reported but not enforced; see
 * guest_mem.h for exactly what that means. */
#define PAGE_NOACCESS 0x00000001u
#define PAGE_READONLY 0x00000002u
#define PAGE_READWRITE 0x00000004u
#define PAGE_WRITECOPY 0x00000008u
#define PAGE_EXECUTE 0x00000010u
#define PAGE_EXECUTE_READ 0x00000020u
#define PAGE_EXECUTE_READWRITE 0x00000040u
#define PAGE_EXECUTE_WRITECOPY 0x00000080u
#define PAGE_GUARD 0x00000100u
#define PAGE_NOCACHE 0x00000200u
#define PAGE_WRITECOMBINE 0x00000400u

/* The access bits are mutually exclusive on NT; the cache and guard bits are
 * modifiers that may be OR-ed in. */
#define PAGE_ACCESS_MASK                                                                 \
    (PAGE_NOACCESS | PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE |     \
     PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)
#define PAGE_MODIFIER_MASK (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE)

/**
 * True when `protect` names exactly one access mode plus optional modifiers.
 *
 * Checked rather than waved through because PAGE_READWRITE (4) and MEM_COMMIT
 * (0x1000) are easy to transpose at a call site, and silently accepting a bogus
 * protection would hide that until something far away misbehaved.
 */
static inline bool nt_page_protect_valid(uint32_t protect)
{
    uint32_t access = protect & PAGE_ACCESS_MASK;
    if (access == 0u || (access & (access - 1u)) != 0u) {
        return false;
    }
    return (protect & ~(PAGE_ACCESS_MASK | PAGE_MODIFIER_MASK)) == 0u;
}

#endif /* TSFP_XBOX_NT_STATUS_H */
