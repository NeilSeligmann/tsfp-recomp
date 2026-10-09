/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_D3D8_METHOD_PACKET_H
#define TSFP_GPU_D3D8_METHOD_PACKET_H

#include <stddef.h>

/* Register the retail 0x003D52F0 two-argument method packet emitter. */
size_t d3d8_method_packet_register(void);

#endif
