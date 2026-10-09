/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_D3D8_CALLBACKS_H
#define TSFP_GPU_D3D8_CALLBACKS_H
#include <stddef.h>
#include <stdint.h>
/* Original 0x003D3530, stdcall(callback), ret4. Stores guest pointer bits only;
 * vertical blank callback execution is not yet modelled. EAX is callback. */
uint32_t d3d8_set_vertical_blank_callback(uint32_t callback);
/* Trusted default-NULL prepublication observer. It may reject a registration
 * before the guest slot changes. Configure quiescently; no implicit reset. */
typedef void (*d3d8_vblank_registration_observer)(uint32_t callback);
void d3d8_callbacks_set_vblank_observer(d3d8_vblank_registration_observer observer);
size_t d3d8_callbacks_register(void);
#endif
