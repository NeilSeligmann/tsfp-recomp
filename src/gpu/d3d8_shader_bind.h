/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_D3D8_SHADER_BIND_H
#define TSFP_D3D8_SHADER_BIND_H
#include <stdint.h>
/* Internal preflight for an exclusive binder call. Guest mappings and permissions,
 * declaration values and pushbuffer bookkeeping remain quiescent through apply.
 * source!=0 snapshots the original 64-dword declaration copy into words. */
void d3d8_shader_bind_prepare(uint32_t entry, uint32_t tagged, uint32_t source,
                              uint32_t words[64]);
#endif
