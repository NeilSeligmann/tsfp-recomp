/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gpu_sha256.h"

#include <string.h>

static const uint32_t round_constants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};

static uint32_t rotate_right(uint32_t value, unsigned bits)
{
    return (value >> bits) | (value << (32u - bits));
}

static void compress(uint32_t state[8], const uint8_t block[64])
{
    uint32_t schedule[64];
    for (unsigned i = 0u; i < 16u; i++) {
        schedule[i] = ((uint32_t)block[i * 4u] << 24) | ((uint32_t)block[i * 4u + 1u] << 16) |
                      ((uint32_t)block[i * 4u + 2u] << 8) | (uint32_t)block[i * 4u + 3u];
    }
    for (unsigned i = 16u; i < 64u; i++) {
        const uint32_t s0 = rotate_right(schedule[i - 15u], 7) ^ rotate_right(schedule[i - 15u], 18) ^
                            (schedule[i - 15u] >> 3);
        const uint32_t s1 = rotate_right(schedule[i - 2u], 17) ^ rotate_right(schedule[i - 2u], 19) ^
                            (schedule[i - 2u] >> 10);
        schedule[i] = schedule[i - 16u] + s0 + schedule[i - 7u] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (unsigned i = 0u; i < 64u; i++) {
        const uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const uint32_t choose = (e & f) ^ (~e & g);
        const uint32_t first = h + sum1 + choose + round_constants[i] + schedule[i];
        const uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t second = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void gpu_sha256(const void *data, size_t size, uint8_t out_digest[32])
{
    uint32_t state[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                         0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    const uint8_t *bytes = data;
    size_t offset = 0u;
    for (; size - offset >= 64u; offset += 64u) {
        compress(state, bytes + offset);
    }
    uint8_t tail[128];
    const size_t remainder = size - offset;
    memset(tail, 0, sizeof tail);
    if (remainder != 0u) {
        memcpy(tail, bytes + offset, remainder);
    }
    tail[remainder] = 0x80u;
    const size_t tail_length = remainder < 56u ? 64u : 128u;
    const uint64_t bit_length = (uint64_t)size * 8u;
    for (unsigned i = 0u; i < 8u; i++) {
        tail[tail_length - 1u - i] = (uint8_t)(bit_length >> (8u * i));
    }
    compress(state, tail);
    if (tail_length == 128u) {
        compress(state, tail + 64u);
    }
    for (unsigned i = 0u; i < 8u; i++) {
        out_digest[i * 4u] = (uint8_t)(state[i] >> 24);
        out_digest[i * 4u + 1u] = (uint8_t)(state[i] >> 16);
        out_digest[i * 4u + 2u] = (uint8_t)(state[i] >> 8);
        out_digest[i * 4u + 3u] = (uint8_t)state[i];
    }
}

void gpu_sha256_hex(const uint8_t digest[32], char out_hex[65])
{
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0u; i < 32u; i++) {
        out_hex[i * 2u] = digits[digest[i] >> 4];
        out_hex[i * 2u + 1u] = digits[digest[i] & 15u];
    }
    out_hex[64] = '\0';
}
