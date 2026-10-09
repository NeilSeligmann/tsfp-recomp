/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_RECOMP_COPY_H
#define TSFP_RECOMP_COPY_H
#include <stddef.h>
void *recomp_guest_memcpy(void *destination, const void *source, size_t bytes);
void *recomp_guest_memmove(void *destination, const void *source, size_t bytes);
#endif
