/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_VOICE_OBSERVER_H
#define TSFP_VOICE_OBSERVER_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef bool (*voice_read_fn)(uint32_t address, void *out, size_t bytes);
void voice_observer_dump(FILE *dump, uint32_t previous[12], voice_read_fn read);
bool voice_observer_target(uint32_t address);
/* Returns the retail lookup result using the two resident descriptors. No guest writes. */
int32_t voice_observer_sample(uint32_t label, voice_read_fn read);
void voice_observer_note(FILE *log, uint32_t callee, uint32_t esp, uint32_t ecx, uint32_t eax,
                         uint64_t ticks, uint64_t hz, uint64_t frames, voice_read_fn read);
#endif
