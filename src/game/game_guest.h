/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * How hand-written game code touches guest memory.
 *
 * A guest address is a uint32_t and stays one. Guest memory is identity-mapped into
 * the low 4 GB (see context.md section 2), so a guest address becomes a host pointer by
 * adding the same offset the lifted code adds in XBOX_PTR, which ships as 0. Going
 * through that offset, rather than casting directly, keeps hand code in step with the
 * lifter if the memory model ever moves.
 *
 * Fields are read and written with memcpy so the compiler cannot assume alignment or
 * aliasing the guest never promised. At -O2 each helper is a single mov.
 */
#ifndef TSFP_GAME_GUEST_H
#define TSFP_GAME_GUEST_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The lifter's memory-model offset, defined by src/host/recomp_runtime.c and by the
 * harness runtime. Zero in every build this project makes. */
extern ptrdiff_t g_xbox_mem_offset;

typedef uint32_t guest_addr;

static inline void *game_host_ptr(guest_addr address)
{
    return (void *)((uintptr_t)address + (uintptr_t)g_xbox_mem_offset);
}

static inline uint32_t guest_read32(guest_addr address)
{
    uint32_t value;
    memcpy(&value, game_host_ptr(address), sizeof value);
    return value;
}

static inline void guest_write32(guest_addr address, uint32_t value)
{
    memcpy(game_host_ptr(address), &value, sizeof value);
}

static inline uint8_t guest_read8(guest_addr address)
{
    uint8_t value;
    memcpy(&value, game_host_ptr(address), sizeof value);
    return value;
}

static inline void guest_write8(guest_addr address, uint8_t value)
{
    memcpy(game_host_ptr(address), &value, sizeof value);
}

#endif /* TSFP_GAME_GUEST_H */
