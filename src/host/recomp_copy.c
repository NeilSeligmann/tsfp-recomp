/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T937: link-time observation of actual guest copies in the installed lifted body.
 * A lifted-only forced include preserves project-symbol calls; helpers delegate to libc;
 * guest bytes and copy chronology are never synthesized or replaced. */
#include "d3d8_resource.h"
#include "recomp_copy.h"
#include <string.h>
#include <stdint.h>
#include <stddef.h>

static _Thread_local bool observing;

static void *begin(void *destination, const void *source, size_t bytes)
{
    const uintptr_t dst = (uintptr_t)destination, src = (uintptr_t)source;
    if (bytes == 0u || bytes > UINT32_MAX || dst > UINT32_MAX || src > UINT32_MAX ||
        dst + bytes > UINT32_MAX || src + bytes > UINT32_MAX) return NULL;
    return d3d8_resource_copy_begin((uint32_t)dst, (uint32_t)src, (uint32_t)bytes);
}

void *recomp_guest_memcpy(void *destination, const void *source, size_t bytes)
{
    if (observing) return memcpy(destination, source, bytes);
    observing = true;
    void *transaction = begin(destination, source, bytes);
    void *result = memcpy(destination, source, bytes);
    if (transaction != NULL) (void)d3d8_resource_copy_end(transaction);
    observing = false;
    return result;
}

void *recomp_guest_memmove(void *destination, const void *source, size_t bytes)
{
    if (observing) return memmove(destination, source, bytes);
    observing = true;
    void *transaction = begin(destination, source, bytes);
    void *result = memmove(destination, source, bytes);
    if (transaction != NULL) (void)d3d8_resource_copy_end(transaction);
    observing = false;
    return result;
}
