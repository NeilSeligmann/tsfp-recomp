/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "live_texture_watch.h"
#include <pthread.h>
#include <stdbool.h>

typedef struct {
    uint32_t address, bytes;
} watch_range;

static pthread_mutex_t watch_lock = PTHREAD_MUTEX_INITIALIZER;
static watch_range pending[LIVE_TEXTURE_WATCH_PENDING];
static size_t pending_count;
static bool pending_overflow;
static uint64_t noted_total, overflow_total;

void live_texture_watch_note(uint32_t address, uint32_t bytes)
{
    if (bytes == 0u) {
        return;
    }
    pthread_mutex_lock(&watch_lock);
    noted_total++;
    if (pending_count < LIVE_TEXTURE_WATCH_PENDING) {
        pending[pending_count++] = (watch_range){address, bytes};
    } else if (!pending_overflow) {
        pending_overflow = true;
        overflow_total++;
    }
    pthread_mutex_unlock(&watch_lock);
}

size_t live_texture_watch_drain(live_texture_cache *cache)
{
    watch_range local[LIVE_TEXTURE_WATCH_PENDING];
    pthread_mutex_lock(&watch_lock);
    const size_t count = pending_count;
    for (size_t index = 0u; index < count; index++) {
        local[index] = pending[index];
    }
    const bool overflow = pending_overflow;
    pending_count = 0u;
    pending_overflow = false;
    pthread_mutex_unlock(&watch_lock);
    size_t stale = 0u;
    if (overflow) {
        stale += live_texture_note_write(cache, 0u, (size_t)1u << 32);
    }
    for (size_t index = 0u; index < count; index++) {
        stale += live_texture_note_write(cache, local[index].address, local[index].bytes);
    }
    return stale;
}

void live_texture_watch_counts(uint64_t *noted, uint64_t *overflows)
{
    pthread_mutex_lock(&watch_lock);
    if (noted != NULL) {
        *noted = noted_total;
    }
    if (overflows != NULL) {
        *overflows = overflow_total;
    }
    pthread_mutex_unlock(&watch_lock);
}

void live_texture_watch_reset(void)
{
    pthread_mutex_lock(&watch_lock);
    pending_count = 0u;
    pending_overflow = false;
    noted_total = overflow_total = 0u;
    pthread_mutex_unlock(&watch_lock);
}

bool live_texture_watch_pending(size_t index, uint32_t *address, uint32_t *bytes)
{
    bool found = false;
    pthread_mutex_lock(&watch_lock);
    if (index < pending_count) {
        *address = pending[index].address;
        *bytes = pending[index].bytes;
        found = true;
    }
    pthread_mutex_unlock(&watch_lock);
    return found;
}
