/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_AUDIO_DSOUND_EFFECTS_CIPHER_H
#define TSFP_AUDIO_DSOUND_EFFECTS_CIPHER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Private image decoder corresponding to original 40F720/40F74D mode0.
 * No guest API/global generator replacement, DSP execution or acknowledgment.
 * Setup produces the original eight-byte initial key. Decode snapshots key,
 * then reads source/writes destination in byte order, including overlaps.
 * All nonempty host spans must be readable/writable and remain quiescent. */
void dsound_effects_cipher_setup(uint8_t key[8]);
bool dsound_effects_cipher_decode(const uint8_t key[8], const uint8_t *source,
                                 uint8_t *destination, size_t bytes);
#endif
