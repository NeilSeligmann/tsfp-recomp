/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T792: the guest write watch (live_texture_watch): notes queue, drain invalidates by range, overflow invalidates all. */
#include "live_texture_watch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

static bool reader(void *context, uint32_t address, void *out, size_t bytes)
{
    (void)context;
    (void)address;
    memset(out, 0x5A, bytes);
    return true;
}

int main(void)
{
    live_texture_cache *cache = calloc(1u, sizeof *cache);
    live_texture_cache_init(cache, false);
    live_texture_watch_reset();
    /* swizzled DXT1 4x4 (format 0x0C, exponents 2 and 2): 8 source bytes at Data */
    live_texture_binding probe = {0u, 0x02210C29u, 0u, 0x1000u, 0u};
    live_texture_result result;
    live_texture_lookup(cache, &probe, reader, NULL, &result);
    CHECK(result.source == LIVE_TEXTURE_SOURCE_GUEST);
    live_texture_binding second = probe;
    second.data = 0x9000u;
    live_texture_lookup(cache, &second, reader, NULL, &result);
    CHECK(result.source == LIVE_TEXTURE_SOURCE_GUEST);
    size_t valid = 0u;
    for (size_t index = 0u; index < LIVE_TEXTURE_CACHE_ENTRIES; index++) valid += cache->entries[index].in_use && cache->entries[index].valid;
    CHECK(valid == 2u);

    /* a zero length note is nothing */
    live_texture_watch_note(0x1000u, 0u);
    uint32_t address, bytes;
    CHECK(!live_texture_watch_pending(0u, &address, &bytes));
    CHECK(live_texture_watch_drain(cache) == 0u);

    /* a note outside the source range leaves both textures valid, one inside stales exactly that one */
    live_texture_watch_note(0x2000u, 0x100u);
    CHECK(live_texture_watch_pending(0u, &address, &bytes) && address == 0x2000u && bytes == 0x100u);
    CHECK(live_texture_watch_drain(cache) == 0u);
    CHECK(!live_texture_watch_pending(0u, &address, &bytes));
    live_texture_watch_note(0x0FF0u, 0x11u);
    CHECK(live_texture_watch_drain(cache) == 1u);
    valid = 0u;
    for (size_t index = 0u; index < LIVE_TEXTURE_CACHE_ENTRIES; index++) valid += cache->entries[index].in_use && cache->entries[index].valid;
    CHECK(valid == 1u);
    CHECK(live_texture_watch_drain(cache) == 0u);

    /* overflow: more notes than the queue holds invalidate every texture */
    live_texture_watch_reset();
    live_texture_lookup(cache, &probe, reader, NULL, &result);
    for (uint32_t index = 0u; index < LIVE_TEXTURE_WATCH_PENDING; index++) live_texture_watch_note(0x8000000u + index * 0x100u, 4u);
    CHECK(live_texture_watch_pending(LIVE_TEXTURE_WATCH_PENDING - 1u, &address, &bytes));
    uint64_t noted = 0u, overflows = 0u;
    live_texture_watch_counts(&noted, &overflows);
    CHECK(noted == LIVE_TEXTURE_WATCH_PENDING && overflows == 0u);
    live_texture_watch_note(0x8000u, 4u);
    live_texture_watch_note(0x8100u, 4u);
    live_texture_watch_counts(&noted, &overflows);
    CHECK(noted == LIVE_TEXTURE_WATCH_PENDING + 2u && overflows == 1u);
    CHECK(!live_texture_watch_pending(LIVE_TEXTURE_WATCH_PENDING, &address, &bytes));
    CHECK(live_texture_watch_drain(cache) == 2u);
    valid = 0u;
    for (size_t index = 0u; index < LIVE_TEXTURE_CACHE_ENTRIES; index++) valid += cache->entries[index].in_use && cache->entries[index].valid;
    CHECK(valid == 0u);
    /* the overflow flag is consumed by the drain: a later drain with a valid texture and no notes keeps it */
    live_texture_lookup(cache, &probe, reader, NULL, &result);
    CHECK(live_texture_watch_drain(cache) == 0u);
    CHECK(cache->entries[result.entry].valid);
    live_texture_watch_reset();
    live_texture_watch_counts(&noted, &overflows);
    CHECK(noted == 0u && overflows == 0u);

    live_texture_cache_free(cache);
    free(cache);
    printf("live texture watch: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
