/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HARNESS_STACKPROBE_CONTRACT_H
#define HARNESS_STACKPROBE_CONTRACT_H
#include <stdint.h>
#include <string.h>

#define HARNESS_STACKPROBE_VA 0x003C9D90u
#define HARNESS_STACKPROBE_SIZE 65u
#ifndef HARNESS_STACKPROBE_APPROVED
#define HARNESS_STACKPROBE_APPROVED 0
#endif

#if HARNESS_STACKPROBE_APPROVED
/* SHA-256 proof of the leaf and two flag-independent callers. No original
 * game bytes are stored here. Standard SHA padding supports these measured sizes. */
static uint32_t harness_sha_rotr(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}
static int harness_stackprobe_sha_proven(const uint8_t *code, unsigned size, const uint32_t expected[8])
{
    static const uint32_t k[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    uint32_t h[8] = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                     0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    uint8_t blocks[512] = {0};
    if (size > 422u) return 0;
    unsigned padded = ((size + 9u + 63u) / 64u) * 64u;
    memcpy(blocks, code, size);
    blocks[size] = 0x80;
    uint64_t bits = (uint64_t)size * 8u;
    for (unsigned i = 0; i < 8; i++) blocks[padded - 1u - i] = (uint8_t)(bits >> (i * 8u));
    for (unsigned block = 0; block < padded / 64u; block++) {
        uint32_t w[64]; const uint8_t *p = blocks + block * 64u;
        for (unsigned i = 0; i < 16; i++)
            w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 |
                   (uint32_t)p[i*4+2] << 8 | p[i*4+3];
        for (unsigned i = 16; i < 64; i++) {
            uint32_t s0 = harness_sha_rotr(w[i-15],7) ^ harness_sha_rotr(w[i-15],18) ^ (w[i-15] >> 3);
            uint32_t s1 = harness_sha_rotr(w[i-2],17) ^ harness_sha_rotr(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],j=h[7];
        for (unsigned i = 0; i < 64; i++) {
            uint32_t s1=harness_sha_rotr(e,6)^harness_sha_rotr(e,11)^harness_sha_rotr(e,25);
            uint32_t t1=j+s1+((e&f)^(~e&g))+k[i]+w[i];
            uint32_t s0=harness_sha_rotr(a,2)^harness_sha_rotr(a,13)^harness_sha_rotr(a,22);
            uint32_t t2=s0+((a&b)^(a&c)^(b&c));
            j=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=j;
    }
    return memcmp(h, expected, sizeof h) == 0;
}
static int harness_stackprobe_original_proven(const uint8_t *code)
{
    static const uint32_t expected[8] = {
        0xb77ad187u,0xf6f3db2au,0x5ae4f8e2u,0x14b023d7u,
        0xc62ce25au,0x38ce769au,0x993d2a8cu,0x81643760u
    };
    return harness_stackprobe_sha_proven(code, HARNESS_STACKPROBE_SIZE, expected);
}
static int harness_stackprobe_caller_proven(uint32_t va, const uint8_t *code)
{
    static const uint32_t caller1[8] = {
        0xd4f0269cu,0x5068d292u,0xa4da4f53u,0x5ee1f1c2u,
        0xfb3b6aecu,0xb595f8b4u,0xb87aad61u,0xa7665ce2u
    };
    static const uint32_t caller2[8] = {
        0x931de85eu,0x6ad46489u,0x970633a0u,0xe7a2e843u,
        0xdb5824fcu,0x08669df2u,0xa1e00658u,0xce4d30c0u
    };
    if (va == 0x00337BD0u) return harness_stackprobe_sha_proven(code, 422u, caller1);
    if (va == 0x0038FE80u) return harness_stackprobe_sha_proven(code, 256u, caller2);
    return 0;
}

#endif
#endif
