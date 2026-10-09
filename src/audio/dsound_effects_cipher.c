/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsound_effects_cipher.h"
#include <string.h>

typedef struct generator { uint32_t low, high; } generator;
static void step(generator *g)
{
    const uint32_t low = g->low, high = g->high;
    const uint32_t feedback = ((((low << 3) ^ (low << 1) ^ low) << 28) ^ high) & 0x80000000u;
    g->low = (low >> 1) | (high << 31);
    g->high = (high >> 1) | feedback;
}
static generator setup(void)
{
    generator g = {0x49DE12BAu, 0x7FA49BCAu};
    /* The seed call itself advances once, followed by 80 output calls. */
    for (unsigned i = 0u; i < 81u; i++) step(&g);
    return g;
}
static void encode(generator g, uint8_t bytes[8])
{
    for (unsigned i = 0u; i < 4u; i++) {
        bytes[i] = (uint8_t)(g.low >> (i * 8u));
        bytes[i + 4u] = (uint8_t)(g.high >> (i * 8u));
    }
}
void dsound_effects_cipher_setup(uint8_t key[8])
{
    encode(setup(), key);
}
bool dsound_effects_cipher_decode(const uint8_t key[8], const uint8_t *source,
                                 uint8_t *destination, size_t bytes)
{
    if (key == NULL || (bytes != 0u && (source == NULL || destination == NULL))) return false;
    uint8_t initial[8], feedback[8];
    generator g = setup();
    encode(g, initial);
    memcpy(feedback, key, sizeof(feedback));
    step(&g);
    for (size_t i = 0u; i < bytes; i++) {
        const unsigned index = (unsigned)(i & 7u);
        const uint8_t input = source[i];
        const uint8_t folded = (uint8_t)(input - feedback[index] * initial[index]);
        step(&g);
        destination[i] = initial[index] ^ (uint8_t)g.low ^ folded;
    }
    return true;
}
