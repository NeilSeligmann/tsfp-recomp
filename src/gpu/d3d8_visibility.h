/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_D3D8_VISIBILITY_H
#define TSFP_D3D8_VISIBILITY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* T1246: called with the slot's guest address before a report slot is read or rewritten (see d3d8_visibility.c). NULL removes. */
void d3d8_visibility_set_drain_hook(void (*hook)(uint32_t slot_address));
uint32_t d3d8_visibility_begin(void);
uint32_t d3d8_visibility_slot(uint32_t index);
uint32_t d3d8_visibility_end(uint32_t index);
uint32_t d3d8_visibility_result(uint32_t index,uint32_t count,uint32_t timestamp);
/* Only mapped registered report pages; payload first, done last. Timestamp model is FABRICATED. */
bool d3d8_visibility_complete(uint32_t offset,uint32_t samples,uint64_t timestamp);
bool d3d8_visibility_complete_identity(uint32_t offset,uint32_t address,uint64_t generation,uint32_t samples,uint64_t timestamp);
size_t d3d8_visibility_register(void);
#endif
