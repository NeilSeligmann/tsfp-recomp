/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_XGRPH_OBJECT_LIFETIME_H
#define TSFP_GPU_XGRPH_OBJECT_LIFETIME_H

#include <stddef.h>

/* Register measured XGRPH members: vtable resets at 0x003EF236, 0x003EF3B7,
 * and 0x004029BD, plus the read-only XGBuffer getter at 0x003E6714. */
size_t xgrph_object_lifetime_register(void);

#endif
