/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_RECOMP_COPY_REDIRECT_H
#define TSFP_RECOMP_COPY_REDIRECT_H
/* Include libc declarations before redirecting only mechanically lifted call sites.
 * This file is force-included only for lifted TUs, never the observer or allocator. */
#include <string.h>
#include "recomp_copy.h"
#define memcpy recomp_guest_memcpy
#define memmove recomp_guest_memmove
#endif
