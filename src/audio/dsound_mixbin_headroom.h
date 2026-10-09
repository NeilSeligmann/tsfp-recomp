/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_AUDIO_DSOUND_MIXBIN_HEADROOM_H
#define TSFP_AUDIO_DSOUND_MIXBIN_HEADROOM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Original byte state/three-bit submix shift, explicit PCM policy only.
 * Callback runs device -> headroom -> completion -> runtime locks; no reentry.
 * Identity packs the verified owned device heap and internal address. */
typedef bool (*dsound_mixbin_pcm_note)(uint64_t identity,uint32_t bin,uint32_t headroom);
typedef bool (*dsound_mixbin_bind_note)(uint64_t identity);
typedef bool (*dsound_mixbin_irql_fn)(uint8_t *irql);
typedef void (*dsound_mixbin_fatal_fn)(uint32_t entry,const char *reason);
void dsound_mixbin_headroom_configure(dsound_mixbin_pcm_note note,dsound_mixbin_bind_note bind,dsound_mixbin_irql_fn irql,
                                      dsound_mixbin_fatal_fn fatal);
size_t dsound_mixbin_headroom_register(void);
uint32_t dsound_mixbin_headroom_set(uint32_t interface,uint32_t bin,uint32_t headroom);
bool dsound_mixbin_headroom_snapshot(uint32_t interface,uint8_t amounts[32]);
#endif
