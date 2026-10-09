/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T819: the guest memory probe (kernel_guest_at) remembers readable pages per thread. Production code changes a guest
 * page's protection in exactly two places (guest_mem.c unmap, kernel_thread.c guard bands) and both flush the cache.
 * A test that calls mprotect or munmap on guest memory itself must do the same, so include this header AFTER the
 * system headers: it routes the two calls through wrappers that flush after the change.
 */
#ifndef TSFP_PROBE_CACHE_WRAP_H
#define TSFP_PROBE_CACHE_WRAP_H

#include <stddef.h>
#include <sys/mman.h>

#include "kernel_call.h"

static inline int tsfp_test_mprotect(void *address, size_t length, int protection)
{
    kernel_guest_probe_change_begin();
    const int result = mprotect(address, length, protection);
    kernel_guest_probe_cache_flush();
    return result;
}

static inline int tsfp_test_munmap(void *address, size_t length)
{
    kernel_guest_probe_change_begin();
    const int result = munmap(address, length);
    kernel_guest_probe_cache_flush();
    return result;
}

#define mprotect tsfp_test_mprotect
#define munmap tsfp_test_munmap

#endif
