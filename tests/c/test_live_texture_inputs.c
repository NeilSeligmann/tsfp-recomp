/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T1246: the texture input store of a pipelined live frame. The presenter job must see guest memory as it stood at the Swap even
 * after the guest rewrote it, and a lookup the store cannot answer is a counted miss, never a read of live memory. */
#include "live_texture_inputs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

static uint8_t guest[0x20000];
static unsigned guest_reads;

static bool guest_reader(void *context, uint32_t address, void *out, size_t bytes)
{
    (void)context;
    guest_reads++;
    if ((uint64_t)address + bytes > sizeof guest) {
        return false;
    }
    memcpy(out, guest + address, bytes);
    return true;
}

static bool guest_resolver(void *context, const live_texture_binding *binding, uint32_t bytes, uint32_t *address, uint64_t *identity,
                           const char **refusal)
{
    (void)context;
    (void)bytes;
    if (binding->data == 0x7000u) {
        *refusal = "refused by the guest resolver";
        return false;
    }
    *address = binding->data + 0x100u; /* a resolved address that differs from Data */
    *identity = 77u;
    return true;
}

int main(void)
{
    live_texture_cache *cache = calloc(1u, sizeof *cache);
    live_texture_cache_init(cache, false);
    live_texture_inputs *inputs = live_texture_inputs_create(1u << 20);
    CHECK(inputs != NULL);
    /* swizzled DXT1 4x4 (format 0x0C): 8 source bytes */
    const live_texture_binding binding = {0u, 0x02210C29u, 0u, 0x1000u, 0u};
    memset(guest + 0x1100u, 0xA5, 8u);
    CHECK(live_texture_inputs_capture(inputs, cache, &binding, guest_resolver, NULL, guest_reader, NULL));
    CHECK(live_texture_inputs_entries(inputs) == 1u);
    CHECK(live_texture_inputs_bytes(inputs) == 8u);
    const unsigned reads_after_capture = guest_reads;
    CHECK(live_texture_inputs_capture(inputs, cache, &binding, guest_resolver, NULL, guest_reader, NULL)); /* idempotent */
    CHECK(live_texture_inputs_entries(inputs) == 1u && guest_reads == reads_after_capture);

    /* the guest rewrites the texture after the Swap: the presenter side still sees the Swap's bytes */
    memset(guest + 0x1100u, 0x11, 8u);
    uint32_t address = 0u;
    uint64_t identity = 0u;
    const char *refusal = NULL;
    CHECK(live_texture_inputs_resolver(inputs, &binding, 8u, &address, &identity, &refusal));
    CHECK(address == 0x1100u && identity == 77u);
    uint8_t seen[8];
    const unsigned reads_before = guest_reads;
    CHECK(live_texture_inputs_reader(inputs, address, seen, sizeof seen));
    CHECK(guest_reads == reads_before); /* served from the store, no live read */
    uint8_t expected[8];
    memset(expected, 0xA5, sizeof expected);
    CHECK(memcmp(seen, expected, sizeof seen) == 0);
    CHECK(live_texture_inputs_misses(inputs) == 0u);

    /* a resolver refusal is replayed by name */
    live_texture_binding refused = binding;
    refused.data = 0x7000u;
    CHECK(live_texture_inputs_capture(inputs, cache, &refused, guest_resolver, NULL, guest_reader, NULL));
    CHECK(!live_texture_inputs_resolver(inputs, &refused, 8u, &address, &identity, &refusal));
    CHECK(refusal != NULL && strcmp(refusal, "refused by the guest resolver") == 0);

    /* a binding never captured is a miss and does not touch guest memory */
    live_texture_binding other = binding;
    other.data = 0x3000u;
    CHECK(!live_texture_inputs_resolver(inputs, &other, 8u, &address, &identity, &refusal));
    CHECK(live_texture_inputs_misses(inputs) == 1u);
    CHECK(!live_texture_inputs_reader(inputs, 0x3100u, seen, sizeof seen));
    CHECK(live_texture_inputs_misses(inputs) == 2u);

    /* a registered render target is not read */
    CHECK(live_texture_register_target(cache, 1u, 0x9000u, 64u, 64u, 256u));
    live_texture_binding target = {0u, 0x02210C29u, 0u, 0x9000u, 0u};
    const unsigned reads_target = guest_reads;
    CHECK(live_texture_inputs_capture(inputs, cache, &target, guest_resolver, NULL, guest_reader, NULL));
    CHECK(guest_reads == reads_target);

    /* reset forgets the frame */
    live_texture_inputs_reset(inputs);
    CHECK(live_texture_inputs_entries(inputs) == 0u && live_texture_inputs_bytes(inputs) == 0u);
    CHECK(!live_texture_inputs_resolver(inputs, &binding, 8u, &address, &identity, &refusal));

    /* the budget bounds the frame: a store too small to hold the bytes says so */
    live_texture_inputs *tiny = live_texture_inputs_create(4u);
    CHECK(!live_texture_inputs_capture(tiny, cache, &binding, guest_resolver, NULL, guest_reader, NULL));
    live_texture_inputs_destroy(tiny);

    live_texture_inputs_destroy(inputs);
    live_texture_cache_free(cache);
    free(cache);
    printf("live_texture_inputs: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
