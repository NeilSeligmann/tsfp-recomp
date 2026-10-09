/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_AUDIO_DSOUND_HRTF_H
#define TSFP_AUDIO_DSOUND_HRTF_H
#include <stdbool.h>
#include <stdint.h>
typedef bool (*dsound_hrtf_irql_provider)(uint8_t *out);
typedef uint32_t (*dsound_hrtf_critical_call)(uint32_t cs);
typedef void (*dsound_hrtf_fatal_fn)(uint32_t address, const char *reason);
void dsound_hrtf_set_irql_provider(dsound_hrtf_irql_provider provider);
void dsound_hrtf_set_fatal(dsound_hrtf_fatal_fn fatal);
/* Tests may wrap genuine calls to measure return propagation. NULL restores defaults. */
void dsound_hrtf_set_critical_calls(dsound_hrtf_critical_call enter,
                                  dsound_hrtf_critical_call leave);
/* Original 0x00406AB6, no arguments, plain ret. Selects guest function table only;
 * no HRTF processing or audio playback is implemented here. */
uint32_t dsound_use_light_hrtf(void);
unsigned dsound_hrtf_register(void);
#endif
