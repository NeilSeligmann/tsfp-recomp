/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_XNET_EEPROM_H
#define TSFP_XNET_EEPROM_H
#include <stdbool.h>
#include <stdint.h>
/* Actual configured regular-file source snapshot. No defaults or key synthesis.
 * Failure preserves installed settings. Startup/quiescent operation. */
typedef enum {
    XNET_EEPROM_LOADED = 0, XNET_EEPROM_OPEN_FAILED, XNET_EEPROM_NOT_REGULAR,
    XNET_EEPROM_WRONG_LENGTH, XNET_EEPROM_READ_FAILED, XNET_EEPROM_STORE_FULL,
    XNET_EEPROM_AUTH_FAILED, XNET_EEPROM_KEY_SOURCE_FAILED
} xnet_eeprom_result;
xnet_eeprom_result xnet_eeprom_load(const char *path);
xnet_eeprom_result xnet_eeprom_load_keyed(const char *path, const char *key_path);
/* INFERRED source transform, validated against genuine xemu DATA321/323.
 * Authenticates encrypted EEPROM region; wrong inputs preserve output. No keys
 * are created, defaulted or published. Does not infer a key from encrypted bytes. */
bool xnet_eeprom_derive_hd_key(const uint8_t eeprom[256], const uint8_t key[16],
                              uint8_t hd_key[16]);
const char *xnet_eeprom_result_name(xnet_eeprom_result result);
#endif
