/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "xnet_eeprom.h"
#include "kernel_config.h"
#include "kernel_crypto.h"
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static xnet_eeprom_result read_source(const char *path, unsigned char *bytes, size_t length)
{
    if (!path || !*path) return XNET_EEPROM_OPEN_FAILED;
    /* Nonblocking open prevents an unsupported FIFO from waiting for a writer. */
    const int descriptor = open(path, O_RDONLY | O_NONBLOCK);
    if (descriptor < 0) return XNET_EEPROM_OPEN_FAILED;
    FILE *file = fdopen(descriptor, "rb");
    if (!file) { (void)close(descriptor); return XNET_EEPROM_OPEN_FAILED; }
    struct stat before, after;
    xnet_eeprom_result result = XNET_EEPROM_LOADED;
    if (fstat(fileno(file), &before) != 0) result = XNET_EEPROM_READ_FAILED;
    else if (!S_ISREG(before.st_mode)) result = XNET_EEPROM_NOT_REGULAR;
    else if ((uint64_t)before.st_size != length) result = XNET_EEPROM_WRONG_LENGTH;
    else if (fread(bytes, 1u, length, file) != length ||
             fgetc(file) != EOF || ferror(file) || fstat(fileno(file), &after) != 0 ||
             after.st_size != before.st_size ||
             after.st_mtim.tv_sec != before.st_mtim.tv_sec ||
             after.st_mtim.tv_nsec != before.st_mtim.tv_nsec ||
             after.st_ctim.tv_sec != before.st_ctim.tv_sec ||
             after.st_ctim.tv_nsec != before.st_ctim.tv_nsec) result = XNET_EEPROM_READ_FAILED;
    if (fclose(file) != 0 && result == XNET_EEPROM_LOADED) result = XNET_EEPROM_READ_FAILED;
    return result;
}
xnet_eeprom_result xnet_eeprom_load_keyed(const char *path, const char *key_path)
{
    uint8_t bytes[256], key[16], derived[16];
    xnet_eeprom_result result = read_source(path, bytes, sizeof(bytes));
    if (result != XNET_EEPROM_LOADED) return result;
    if (key_path) {
        if (read_source(key_path, key, sizeof(key)) != XNET_EEPROM_LOADED)
            return XNET_EEPROM_KEY_SOURCE_FAILED;
        if (!xnet_eeprom_derive_hd_key(bytes, key, derived)) return XNET_EEPROM_AUTH_FAILED;
        if (!kernel_config_set_eeprom_keyed(bytes, sizeof(bytes), key, sizeof(key)))
            return XNET_EEPROM_STORE_FULL;
    } else if (!kernel_config_set_eeprom(bytes, sizeof(bytes))) return XNET_EEPROM_STORE_FULL;
    return XNET_EEPROM_LOADED;
}
xnet_eeprom_result xnet_eeprom_load(const char *path)
{
    return xnet_eeprom_load_keyed(path, NULL);
}

const char *xnet_eeprom_result_name(xnet_eeprom_result result)
{
    switch (result) {
    case XNET_EEPROM_LOADED: return "loaded actual256-byte source";
    case XNET_EEPROM_OPEN_FAILED: return "source could not be opened";
    case XNET_EEPROM_NOT_REGULAR: return "source is not a regular file";
    case XNET_EEPROM_WRONG_LENGTH: return "source is not exactly256 bytes";
    case XNET_EEPROM_READ_FAILED: return "source read failed or changed during read";
    case XNET_EEPROM_STORE_FULL: return "configuration store cannot publish source";
    case XNET_EEPROM_AUTH_FAILED: return "EEPROM/key source authentication failed";
    case XNET_EEPROM_KEY_SOURCE_FAILED: return "key source is not a readable regular16-byte file";
    }
    return "invalid source result";
}


bool xnet_eeprom_derive_hd_key(const uint8_t eeprom[256], const uint8_t key[16],
                              uint8_t hd_key[16])
{
    if (!eeprom || !key || !hd_key) return false;
    uint8_t digest[20], plaintext[28], state[KERNEL_RC4_STATE_BYTES];
    kernel_crypto_hmac_sha1(key, 16u, eeprom, 20u, digest);
    memcpy(plaintext, eeprom + 20u, sizeof(plaintext));
    if (!kernel_crypto_rc4_key(state, digest, 20u) ||
        !kernel_crypto_rc4_crypt(state, plaintext, sizeof(plaintext))) return false;
    kernel_crypto_hmac_sha1(key, 16u, plaintext, sizeof(plaintext), digest);
    unsigned difference = 0u;
    for (unsigned i = 0u; i < 20u; ++i) difference |= digest[i] ^ eeprom[i];
    if (difference != 0u) return false;
    memcpy(hd_key, plaintext + 8u, 16u);
    return true;
}
