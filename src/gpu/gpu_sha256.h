/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_SHA256_H
#define TSFP_GPU_SHA256_H

#include <stddef.h>
#include <stdint.h>

/* SHA-256 (FIPS 180-4). The vertex-program corpus names every module after the digest of the
 * program's bytes (tools/nv2a/corpus.py), so the pushbuffer replay needs the same digest to find
 * the module of the program a stream uploaded. */
void gpu_sha256(const void *data, size_t size, uint8_t out_digest[32]);

/* Lower-case hexadecimal of a digest, 64 characters plus the terminator. */
void gpu_sha256_hex(const uint8_t digest[32], char out_hex[65]);

#endif
