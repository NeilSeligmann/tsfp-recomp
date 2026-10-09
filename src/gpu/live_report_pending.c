/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "live_report_pending.h"

#include <pthread.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    uint32_t address;
    unsigned package;
} owed;

struct live_report_pending {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    owed *entries;
    size_t count, capacity;
};

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

live_report_pending *live_report_pending_create(size_t capacity)
{
    live_report_pending *pending = calloc(1u, sizeof *pending);
    if (pending == NULL) {
        return NULL;
    }
    pending->entries = calloc(capacity != 0u ? capacity : 1u, sizeof *pending->entries);
    if (pending->entries == NULL) {
        free(pending);
        return NULL;
    }
    pending->capacity = capacity;
    pthread_mutex_init(&pending->lock, NULL);
    pthread_cond_init(&pending->changed, NULL);
    return pending;
}

void live_report_pending_destroy(live_report_pending *pending)
{
    if (pending != NULL) {
        pthread_cond_destroy(&pending->changed);
        pthread_mutex_destroy(&pending->lock);
        free(pending->entries);
        free(pending);
    }
}

bool live_report_pending_add(live_report_pending *pending, uint32_t address, unsigned package)
{
    pthread_mutex_lock(&pending->lock);
    const bool room = pending->count < pending->capacity;
    if (room) {
        pending->entries[pending->count].address = address;
        pending->entries[pending->count].package = package;
        pending->count++;
    }
    pthread_mutex_unlock(&pending->lock);
    return room;
}

void live_report_pending_done(live_report_pending *pending, uint32_t address, unsigned package)
{
    pthread_mutex_lock(&pending->lock);
    size_t kept = 0u;
    bool removed = false;
    for (size_t i = 0u; i < pending->count; i++) {
        const bool match = pending->entries[i].package == package && (address == 0u || (!removed && pending->entries[i].address == address));
        if (match) {
            removed = true;
        } else {
            pending->entries[kept++] = pending->entries[i];
        }
    }
    pending->count = kept;
    pthread_cond_broadcast(&pending->changed);
    pthread_mutex_unlock(&pending->lock);
}

static bool is_owed(const live_report_pending *pending, uint32_t address)
{
    for (size_t i = 0u; i < pending->count; i++) {
        if (pending->entries[i].address == address) {
            return true;
        }
    }
    return false;
}

uint64_t live_report_pending_wait(live_report_pending *pending, uint32_t address)
{
    uint64_t start = 0u;
    bool waited = false;
    pthread_mutex_lock(&pending->lock);
    while (is_owed(pending, address)) {
        if (!waited) {
            waited = true;
            start = monotonic_ns();
        }
        pthread_cond_wait(&pending->changed, &pending->lock);
    }
    pthread_mutex_unlock(&pending->lock);
    return waited ? monotonic_ns() - start : 0u;
}

size_t live_report_pending_count(live_report_pending *pending)
{
    pthread_mutex_lock(&pending->lock);
    const size_t count = pending->count;
    pthread_mutex_unlock(&pending->lock);
    return count;
}
