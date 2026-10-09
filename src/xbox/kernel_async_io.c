/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See kernel_async_io.h for what is modelled, why, and what is INFERRED.
 */

#include "kernel_async_io.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "guest_structs.h"
#include "kernel_clock.h"
#include "kernel_hle.h"
#include "kernel_object.h"
#include "nt_status.h"

typedef struct {
    bool in_use;
    kernel_async_read read;
    uint64_t due_ticks;
    /* T764: the file object this request signals, 0 when it signals its Event instead. */
    uint32_t file_identity;
} pending_read;

/* T764: the file object's signal state, per file identity. */
typedef struct {
    bool in_use;
    uint32_t identity;
    bool signalled;
} file_object_state;

#define FILE_OBJECT_TABLE 64u

static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pending_read queue[KERNEL_ASYNC_IO_QUEUE];
static bool enabled;
static bool file_object_enabled;
static file_object_state file_objects[FILE_OBJECT_TABLE];
/* When the drive finishes its last queued request: the next one cannot start before it. */
static uint64_t drive_free_ticks;
static kernel_async_io_stats stats;
static bool elapsed_epoch;
/* T1289: stats.pending published for the lock free idle test of the safepoint (written under queue_lock). */
static _Atomic unsigned pending_mirror;
static uint64_t elapsed_wall_start, elapsed_wall_last, elapsed_model_start;

void kernel_async_io_set_enabled(bool value)
{
    pthread_mutex_lock(&queue_lock);
    enabled = value;
    elapsed_epoch = false;
    pthread_mutex_unlock(&queue_lock);
}

bool kernel_async_io_enabled(void)
{
    pthread_mutex_lock(&queue_lock);
    const bool value = enabled;
    pthread_mutex_unlock(&queue_lock);
    return value;
}

void kernel_async_io_set_file_object_enabled(bool value)
{
    pthread_mutex_lock(&queue_lock);
    file_object_enabled = value;
    pthread_mutex_unlock(&queue_lock);
}

bool kernel_async_io_file_object_enabled(void)
{
    pthread_mutex_lock(&queue_lock);
    const bool value = file_object_enabled;
    pthread_mutex_unlock(&queue_lock);
    return value;
}

bool kernel_async_io_eligible(uint32_t open_options, uint32_t event_handle)
{
    if ((open_options & KERNEL_ASYNC_IO_SYNCHRONOUS_OPTIONS) != 0u) {
        return false;
    }
    if ((open_options & KERNEL_ASYNC_IO_NO_BUFFERING_OPTION) == 0u) {
        return false;
    }
    return kernel_async_io_enabled() &&
           (event_handle != 0u || kernel_async_io_file_object_enabled());
}

bool kernel_async_io_apc_refused(uint32_t open_options, uint32_t apc_routine)
{
    if (apc_routine == 0u || (open_options & KERNEL_ASYNC_IO_SYNCHRONOUS_OPTIONS) != 0u) {
        return false;
    }
    pthread_mutex_lock(&queue_lock);
    const bool refused = enabled;
    if (refused) {
        stats.apc_refused++;
    }
    pthread_mutex_unlock(&queue_lock);
    return refused;
}

static bool path_has_prefix(const char *path, const char *prefix)
{
    for (; *prefix != '\0'; path++, prefix++) {
        const char a = (*path >= 'a' && *path <= 'z') ? (char)(*path - 'a' + 'A') : *path;
        const char b = (*prefix >= 'a' && *prefix <= 'z') ? (char)(*prefix - 'a' + 'A') : *prefix;
        if (a != b) {
            return false;
        }
    }
    return true;
}

kernel_async_io_volume kernel_async_io_volume_of_path(const char *path)
{
    if (path == NULL) {
        return KERNEL_ASYNC_IO_VOLUME_DISC;
    }
    if (path_has_prefix(path, "\\Device\\Harddisk")) {
        return KERNEL_ASYNC_IO_VOLUME_HDD;
    }
    static const char *const roots[] = {"\\??\\", "\\DosDevices\\", ""};
    for (unsigned i = 0u; i < sizeof(roots) / sizeof(roots[0]); i++) {
        const size_t length = strlen(roots[i]);
        if (!path_has_prefix(path, roots[i]) || path[length] == '\0' || path[length + 1u] != ':') {
            continue;
        }
        const char letter = (path[length] >= 'a' && path[length] <= 'z') ? (char)(path[length] - 'a' + 'A')
                                                                           : path[length];
        if (strchr("CEFXYZ", letter) != NULL) {
            return KERNEL_ASYNC_IO_VOLUME_HDD;
        }
    }
    return KERNEL_ASYNC_IO_VOLUME_DISC;
}

uint64_t kernel_async_io_service_ticks(kernel_async_io_volume volume, uint32_t bytes)
{
    const uint64_t frequency = KERNEL_CLOCK_FREQUENCY_HZ;
    const bool hdd = volume == KERNEL_ASYNC_IO_VOLUME_HDD;
    const uint64_t access_us = hdd ? KERNEL_ASYNC_IO_HDD_ACCESS_US : KERNEL_ASYNC_IO_DISC_ACCESS_US;
    const uint64_t rate = hdd ? KERNEL_ASYNC_IO_HDD_BYTES_PER_SECOND : KERNEL_ASYNC_IO_DISC_BYTES_PER_SECOND;
    const uint64_t access = (access_us * frequency + 999999u) / 1000000u;
    /* Rounded up, so a request never completes early. */
    const uint64_t transfer = ((uint64_t)bytes * frequency + rate - 1u) / rate;
    return access + transfer;
}

/* The file object of `identity`, created clear on first use. A slot whose file has no live handle
 * and nothing pending on it is reused when the table is full. NULL when every slot is busy.
 * Called with the queue lock held. */
static file_object_state *file_object_locked(uint32_t identity, bool create)
{
    file_object_state *free_slot = NULL;
    file_object_state *reusable = NULL;
    for (unsigned i = 0u; i < FILE_OBJECT_TABLE; i++) {
        file_object_state *entry = &file_objects[i];
        if (!entry->in_use) {
            if (free_slot == NULL) {
                free_slot = entry;
            }
            continue;
        }
        if (entry->identity == identity) {
            return entry;
        }
        if (reusable == NULL && !kernel_object_file_identity_live(entry->identity)) {
            bool busy = false;
            for (unsigned j = 0u; j < KERNEL_ASYNC_IO_QUEUE; j++) {
                busy = busy || (queue[j].in_use && queue[j].file_identity == entry->identity);
            }
            if (!busy) {
                reusable = entry;
            }
        }
    }
    file_object_state *slot = free_slot != NULL ? free_slot : reusable;
    if (!create || slot == NULL) {
        return NULL;
    }
    memset(slot, 0, sizeof(*slot));
    slot->in_use = true;
    slot->identity = identity;
    return slot;
}

bool kernel_async_io_submit(kernel_async_read *request)
{
    if (request == NULL || request->data == NULL) {
        return false;
    }
    const uint64_t now = kernel_clock_peek();
    pthread_mutex_lock(&queue_lock);
    pending_read *slot = NULL;
    for (unsigned i = 0u; i < KERNEL_ASYNC_IO_QUEUE; i++) {
        if (!queue[i].in_use) {
            slot = &queue[i];
            break;
        }
    }
    if (slot == NULL) {
        stats.refused++;
        pthread_mutex_unlock(&queue_lock);
        return false;
    }
    uint32_t identity = 0u;
    if (request->event_handle == 0u) {
        /* No Event: the waiter's object is the file object, cleared by the issue (the NT rule). */
        identity = kernel_object_file_identity(request->file_handle);
        file_object_state *object = file_object_locked(identity, true);
        if (object == NULL) {
            stats.refused++;
            pthread_mutex_unlock(&queue_lock);
            return false;
        }
        object->signalled = false;
        stats.file_object_reads++;
    }
    const uint64_t start = drive_free_ticks > now ? drive_free_ticks : now;
    if (stats.pending == 0u) elapsed_epoch = false;
    slot->file_identity = identity;
    slot->due_ticks = start + kernel_async_io_service_ticks(request->volume, request->length);
    drive_free_ticks = slot->due_ticks;
    slot->read = *request;
    slot->in_use = true;
    request->data = NULL;
    stats.submitted++;
    stats.pending++;
    atomic_store_explicit(&pending_mirror, stats.pending, memory_order_relaxed);
    pthread_mutex_unlock(&queue_lock);
    return true;
}

static void write_result(const kernel_async_read *read, uint32_t status, uint32_t information)
{
    if (read->io_status != 0u) {
        (void)kernel_guest_write_u32(read->io_status + (uint32_t)offsetof(guest_io_status_block, status),
                                     status);
        (void)kernel_guest_write_u32(
            read->io_status + (uint32_t)offsetof(guest_io_status_block, information), information);
    }
}

/* Complete one request: buffer, then IoStatusBlock, then the Event, the order the I/O manager
 * uses (the event is the last thing a waiter can observe). Called with the queue lock held. */
static void complete_locked(pending_read *entry)
{
    const kernel_async_read *read = &entry->read;
    uint32_t status = STATUS_SUCCESS;
    uint32_t information = read->length;
    if (read->length != 0u) {
        uint8_t *destination = kernel_guest_at(read->buffer, read->length);
        if (destination == NULL) {
            kernel_hle_log()("kernel: async NtReadFile completion cannot write %u byte(s) to guest "
                             "address %#x, completed with STATUS_INVALID_PARAMETER\n",
                             (unsigned)read->length, (unsigned)read->buffer);
            status = STATUS_INVALID_PARAMETER;
            information = 0u;
            stats.failed++;
        } else {
            memcpy(destination, read->data, read->length);
            stats.bytes += read->length;
        }
    }
    write_result(read, status, information);
    if (read->event_handle == 0u) {
        file_object_state *object = file_object_locked(entry->file_identity, false);
        if (object != NULL) {
            object->signalled = true;
        }
    } else {
        bool previous = false;
        const nt_status set = kernel_object_event_set(read->event_handle, &previous);
        if (set != STATUS_SUCCESS) {
            kernel_hle_log()(
                "kernel: async NtReadFile completion could not set Event %#x (status %#x)\n",
                (unsigned)read->event_handle, (unsigned)set);
        }
    }
    free(entry->read.data);
    memset(entry, 0, sizeof(*entry));
    stats.completed++;
    stats.pending--;
    atomic_store_explicit(&pending_mirror, stats.pending, memory_order_relaxed);
}

/* The pending request that is due first (issue order: the drive is serial, so due times only grow along
 * it), NULL when none. Queue lock held. */
static pending_read *earliest_any_locked(void)
{
    pending_read *oldest = NULL;
    for (unsigned i = 0u; i < KERNEL_ASYNC_IO_QUEUE; i++) {
        if (queue[i].in_use && (oldest == NULL || queue[i].due_ticks < oldest->due_ticks)) {
            oldest = &queue[i];
        }
    }
    return oldest;
}

unsigned kernel_async_io_service(void)
{
    const uint64_t now = kernel_clock_peek();
    unsigned completed = 0u;
    pthread_mutex_lock(&queue_lock);
    if (stats.pending == 0u) {
        pthread_mutex_unlock(&queue_lock);
        return 0u;
    }
    for (;;) {
        pending_read *oldest = earliest_any_locked();
        if (oldest == NULL || oldest->due_ticks > now) {
            break;
        }
        complete_locked(oldest);
        completed++;
    }
    pthread_mutex_unlock(&queue_lock);
    return completed;
}

/* Floor elapsed nanoseconds to clock ticks, saturating rather than wrapping. */
static uint64_t elapsed_ticks(uint64_t nanoseconds)
{
    const uint64_t seconds = nanoseconds / UINT64_C(1000000000);
    const uint64_t fraction = nanoseconds % UINT64_C(1000000000);
    if (seconds > UINT64_MAX / KERNEL_CLOCK_FREQUENCY_HZ) return UINT64_MAX;
    const uint64_t whole = seconds * KERNEL_CLOCK_FREQUENCY_HZ;
    const uint64_t part = fraction * KERNEL_CLOCK_FREQUENCY_HZ / UINT64_C(1000000000);
    return part > UINT64_MAX - whole ? UINT64_MAX : whole + part;
}

bool kernel_async_io_idle(void)
{
    /* No request is queued: service_elapsed would only reset the elapsed epoch, which every submit with an empty queue resets too. */
    return atomic_load_explicit(&pending_mirror, memory_order_relaxed) == 0u;
}

unsigned kernel_async_io_service_elapsed(uint64_t monotonic_nanoseconds)
{
    const uint64_t now = kernel_clock_peek();
    pthread_mutex_lock(&queue_lock);
    if (!enabled || stats.pending == 0u) {
        elapsed_epoch = false;
        pthread_mutex_unlock(&queue_lock);
        return 0u;
    }
    stats.elapsed_calls++;
    if (!elapsed_epoch) {
        elapsed_epoch = true;
        elapsed_wall_start = elapsed_wall_last = monotonic_nanoseconds;
        elapsed_model_start = now;
    } else if (monotonic_nanoseconds < elapsed_wall_last) {
        stats.elapsed_backwards++;
        pthread_mutex_unlock(&queue_lock);
        return 0u;
    }
    elapsed_wall_last = monotonic_nanoseconds;
    const uint64_t delta = elapsed_ticks(monotonic_nanoseconds - elapsed_wall_start);
    uint64_t target = delta > UINT64_MAX - elapsed_model_start ? UINT64_MAX : elapsed_model_start + delta;
    if (now > target) {
        /* A frame or explicit wait supplied newer model time. Resume wall
         * progress from that observed floor rather than waiting for an older
         * epoch to catch up while a continuously nonempty queue polls. No
         * elapsed delta is added to this external advance in the same call. */
        elapsed_model_start = target = now;
        elapsed_wall_start = monotonic_nanoseconds;
        stats.elapsed_rebases++;
    }
    pthread_mutex_unlock(&queue_lock);
    /* No queue lock across clock publication or completion; advance_to keeps
     * existing model/frame progress when it is ahead of this wall epoch. */
    (void)kernel_clock_advance_to(target);
    const unsigned completed = kernel_async_io_service();
    pthread_mutex_lock(&queue_lock);
    stats.elapsed_completions += completed;
    pthread_mutex_unlock(&queue_lock);
    return completed;
}

/* The earliest pending request that signals the awaited object: an Event handle, or the file
 * object of `identity` (an Event-less request). NULL when there is none. Queue lock held. */
static const pending_read *earliest_locked(bool by_event, uint32_t key)
{
    const pending_read *found = NULL;
    for (unsigned i = 0u; i < KERNEL_ASYNC_IO_QUEUE; i++) {
        const bool match = by_event ? queue[i].read.event_handle == key
                                    : queue[i].read.event_handle == 0u &&
                                          queue[i].file_identity == key;
        if (queue[i].in_use && match && (found == NULL || queue[i].due_ticks < found->due_ticks)) {
            found = &queue[i];
        }
    }
    return found;
}

static uint64_t timeout_ticks(uint64_t timeout_100ns)
{
    const uint64_t frequency = KERNEL_CLOCK_FREQUENCY_HZ;
    if (timeout_100ns > UINT64_MAX / frequency) {
        return UINT64_MAX;
    }
    return (timeout_100ns * frequency + 9999999u) / 10000000u;
}

static kernel_async_wait wait_for(bool by_event, uint32_t key, bool finite, uint64_t timeout_100ns)
{
    if (!kernel_async_io_enabled()) {
        return KERNEL_ASYNC_WAIT_NONE;
    }
    const uint64_t start = kernel_clock_peek();
    const uint64_t span = finite ? timeout_ticks(timeout_100ns) : UINT64_MAX;
    const uint64_t deadline = span > UINT64_MAX - start ? UINT64_MAX : start + span;
    kernel_async_wait result = KERNEL_ASYNC_WAIT_NONE;
    bool advanced = false;
    for (;;) {
        (void)kernel_async_io_service();
        bool signalled = false;
        pthread_mutex_lock(&queue_lock);
        if (by_event) {
            (void)kernel_object_event_signaled(key, &signalled);
        } else {
            const file_object_state *object = file_object_locked(key, false);
            signalled = object != NULL && object->signalled;
        }
        const pending_read *target = earliest_locked(by_event, key);
        const uint64_t due = target != NULL ? target->due_ticks : 0u;
        const bool pending = target != NULL;
        pthread_mutex_unlock(&queue_lock);
        if (signalled) {
            result = KERNEL_ASYNC_WAIT_SATISFIED;
            break;
        }
        if (!pending) {
            break;
        }
        if (due > deadline) {
            /* The timeout ends first: time passes up to the deadline, not to the request. */
            (void)kernel_clock_advance_to(deadline);
            (void)kernel_async_io_service();
            result = KERNEL_ASYNC_WAIT_TIMED_OUT;
            advanced = true;
            break;
        }
        /* To the due time of the awaited request, exactly: its completion is the next thing
         * the service finds. */
        (void)kernel_clock_advance_to(due);
        advanced = true;
    }
    pthread_mutex_lock(&queue_lock);
    if (result == KERNEL_ASYNC_WAIT_TIMED_OUT) {
        stats.wait_timeouts++;
    } else if (result == KERNEL_ASYNC_WAIT_SATISFIED && advanced) {
        stats.waits++;
    }
    pthread_mutex_unlock(&queue_lock);
    return result;
}

kernel_async_wait kernel_async_io_wait_event(uint32_t handle, bool finite, uint64_t timeout_100ns)
{
    return wait_for(true, handle, finite, timeout_100ns);
}

kernel_async_wait kernel_async_io_wait_file(uint32_t file_handle, bool finite,
                                            uint64_t timeout_100ns)
{
    return wait_for(false, kernel_object_file_identity(file_handle), finite, timeout_100ns);
}

unsigned kernel_async_io_pending(void)
{
    pthread_mutex_lock(&queue_lock);
    const unsigned pending = stats.pending;
    pthread_mutex_unlock(&queue_lock);
    return pending;
}

bool kernel_async_io_complete_next(void)
{
    if (!kernel_async_io_enabled()) {
        return false;
    }
    pthread_mutex_lock(&queue_lock);
    const pending_read *oldest = earliest_any_locked();
    const bool any = oldest != NULL;
    const uint64_t due = any ? oldest->due_ticks : 0u;
    pthread_mutex_unlock(&queue_lock);
    if (!any) {
        return false;
    }
    (void)kernel_clock_advance_to(due);
    const unsigned completed = kernel_async_io_service();
    if (completed != 0u) {
        pthread_mutex_lock(&queue_lock);
        stats.spin_completions += completed;
        pthread_mutex_unlock(&queue_lock);
    }
    return completed != 0u;
}

void kernel_async_io_service_hook(void)
{
    (void)kernel_async_io_service();
}

kernel_async_io_stats kernel_async_io_get_stats(void)
{
    pthread_mutex_lock(&queue_lock);
    const kernel_async_io_stats copy = stats;
    pthread_mutex_unlock(&queue_lock);
    return copy;
}

void kernel_async_io_reset(void)
{
    pthread_mutex_lock(&queue_lock);
    for (unsigned i = 0u; i < KERNEL_ASYNC_IO_QUEUE; i++) {
        free(queue[i].read.data);
    }
    memset(queue, 0, sizeof(queue));
    memset(file_objects, 0, sizeof(file_objects));
    memset(&stats, 0, sizeof(stats));
    atomic_store_explicit(&pending_mirror, 0u, memory_order_relaxed);
    drive_free_ticks = 0u;
    elapsed_epoch = false;
    pthread_mutex_unlock(&queue_lock);
}
