/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Executive pool allocation: ExAllocatePoolWithTag and ExFreePool.
 *
 * VERIFIED ORDINAL NUMBERS, resolved against tools/kernel_ordinals.py rather than
 * recalled. context.md section 6m records four ordinals being misremembered in a
 * single task, so every number here was looked up:
 *
 *   15  ExAllocatePoolWithTag   -- 14 call sites
 *   17  ExFreePool              -- 11 call sites
 *
 * NOT BOUND HERE: ExAllocatePool (ordinal 14). It has ZERO call sites in this
 * image, so implementing it would be writing code against no evidence of use. The
 * measurement is in generated/retail/ordinal_callsites.json.
 *
 * WHY A SEPARATE POOL RATHER THAN THE REGION ALLOCATOR. The guest's pool requests
 * are small and frequent; handing each one its own 64 KB-granular region would
 * waste three orders of magnitude and, worse, make two consecutive small
 * allocations land 64 KB apart, which hides any guest code that walks off the end
 * of one into the next. A heap packs them, so an overrun lands in the neighbour it
 * would have landed in on hardware.
 *
 * THE POOL IS NOT PAGED, AND THAT IS NOT MODELLED. Hardware distinguishes
 * NonPagedPool from PagedPool; the Xbox has no page file, so every pool is
 * resident and the distinction has no observable consequence here. ExAllocatePool-
 * WithTag takes no pool type argument, so nothing is being discarded.
 */

#ifndef TSFP_XBOX_KERNEL_POOL_H
#define TSFP_XBOX_KERNEL_POOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel_hle.h"

/**
 * Register the pool ordinals. Returns how many bound.
 *
 * Returns a count rather than a bool so a caller can assert the expected number:
 * a binding that silently failed to take would leave the ordinal reporting itself
 * as a stub, which looks like "not written yet" rather than "wired up wrong".
 */
size_t kernel_pool_register(void);

/** Release every pool allocation and the backing heap. For shutdown and tests. */
void kernel_pool_reset(void);

/** Live pool allocations. */
unsigned kernel_pool_live_count(void);

/** Bytes the guest currently holds from the pool, as requested (not rounded). */
uint64_t kernel_pool_live_bytes(void);

/**
 * Frees of an address the pool never issued.
 *
 * Counted rather than ignored: on hardware this is a bugcheck, and the one thing
 * worse than crashing on it is continuing as though it had not happened.
 */
uint32_t kernel_pool_bad_free_count(void);

/**
 * The tag an address was allocated with. False when the pool did not issue it.
 *
 * Exists so a test can prove the tag is actually recorded rather than accepted and
 * dropped, and so a leak report can name the subsystem that leaked.
 */
bool kernel_pool_tag_of(kernel_guest_ptr address, uint32_t *out_tag);

/** The requested size of an allocation. False when the pool did not issue it. */
bool kernel_pool_size_of(kernel_guest_ptr address, uint32_t *out_bytes);

#endif /* TSFP_XBOX_KERNEL_POOL_H */
