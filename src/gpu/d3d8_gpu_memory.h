/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_D3D8_GPU_MEMORY_H
#define TSFP_D3D8_GPU_MEMORY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Physical GPU buffer spans translate through their held guest region; otherwise
 * synthetic test streams remain virtual. Plain-C surface models share this reader. */
bool d3d8_gpu_read_guest(void *context,uint32_t address,void *out,size_t bytes);
bool d3d8_gpu_read_virtual(void *context,uint32_t address,void *out,size_t bytes);
#endif
