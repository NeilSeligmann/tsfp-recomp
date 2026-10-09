/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See kernel_fpstate.h for the signatures, the measured sites and what is and is not
 * modelled. One mutex-guarded table of (host thread, save area): guest threads are host
 * threads, so the owner of a save is the thread that made the call.
 */

#include "kernel_fpstate.h"

#include <pthread.h>
#include <stdbool.h>
#include <string.h>

#include "kernel_call.h"
#include "kernel_hle.h"
#include "nt_status.h"

typedef struct {
    pthread_t owner;
    uint32_t area;
} live_save;

static pthread_mutex_t table_mutex = PTHREAD_MUTEX_INITIALIZER;
static live_save table[KERNEL_FPSTATE_MAX_ACTIVE];
static uint32_t table_count;
static uint32_t anomalies;

/* Index of the most recent save of `area` by the calling thread, or table_count. Under the
 * mutex. */
static uint32_t find_latest(uint32_t area)
{
    const pthread_t self = pthread_self();
    for (uint32_t index = table_count; index > 0u; index--) {
        if (pthread_equal(table[index - 1u].owner, self) && table[index - 1u].area == area) {
            return index - 1u;
        }
    }
    return table_count;
}

/* Index of the most recent save by the calling thread of ANY area, or table_count. */
static uint32_t find_latest_of_thread(void)
{
    const pthread_t self = pthread_self();
    for (uint32_t index = table_count; index > 0u; index--) {
        if (pthread_equal(table[index - 1u].owner, self)) {
            return index - 1u;
        }
    }
    return table_count;
}

static bool read_area_argument(void *context, const char *who, uint32_t *area)
{
    if (context == NULL) {
        kernel_hle_log()("kernel: %s called with no argument frame -- the call boundary did "
                         "not supply one\n",
                         who);
        return false;
    }
    if (!kernel_frame_arg((const kernel_call_frame *)context, 0u, area)) {
        kernel_hle_log()("kernel: %s could not read its argument from the guest stack\n", who);
        return false;
    }
    return true;
}

/* Under the mutex: one report and one count. */
static void note_anomaly(void)
{
    anomalies++;
}

static uint32_t hle_save(void *context)
{
    uint32_t area = 0u;
    if (!read_area_argument(context, "KeSaveFloatingPointState", &area)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!kernel_guest_range_readable(area, KERNEL_FPSTATE_AREA_BYTES)) {
        pthread_mutex_lock(&table_mutex);
        note_anomaly();
        pthread_mutex_unlock(&table_mutex);
        kernel_hle_log()("kernel: KeSaveFloatingPointState(%#x) REFUSED: the save area is not "
                         "readable for %#x bytes, the real kernel would fault writing it\n",
                         area, KERNEL_FPSTATE_AREA_BYTES);
        return STATUS_ACCESS_VIOLATION;
    }
    pthread_mutex_lock(&table_mutex);
    if (table_count >= KERNEL_FPSTATE_MAX_ACTIVE) {
        note_anomaly();
        pthread_mutex_unlock(&table_mutex);
        kernel_hle_log()("kernel: KeSaveFloatingPointState(%#x) REFUSED: %u live saves is the "
                         "most this model records\n",
                         area, KERNEL_FPSTATE_MAX_ACTIVE);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    const bool repeated = find_latest(area) != table_count;
    if (repeated) {
        note_anomaly();
    }
    table[table_count].owner = pthread_self();
    table[table_count].area = area;
    table_count++;
    pthread_mutex_unlock(&table_mutex);
    if (repeated) {
        kernel_hle_log()("kernel: KeSaveFloatingPointState(%#x) saves into an area that already "
                         "holds a live save of this thread, recording it again\n",
                         area);
    }
    return STATUS_SUCCESS;
}

static uint32_t hle_restore(void *context)
{
    uint32_t area = 0u;
    if (!read_area_argument(context, "KeRestoreFloatingPointState", &area)) {
        return STATUS_SUCCESS;
    }
    pthread_mutex_lock(&table_mutex);
    const uint32_t found = find_latest(area);
    if (found == table_count) {
        note_anomaly();
        pthread_mutex_unlock(&table_mutex);
        kernel_hle_log()("kernel: KeRestoreFloatingPointState(%#x) has no matching save on this "
                         "thread, ignored\n",
                         area);
        return STATUS_SUCCESS;
    }
    const bool out_of_order = found != find_latest_of_thread();
    if (out_of_order) {
        note_anomaly();
    }
    memmove(&table[found], &table[found + 1u], (table_count - found - 1u) * sizeof(table[0]));
    table_count--;
    pthread_mutex_unlock(&table_mutex);
    if (out_of_order) {
        kernel_hle_log()("kernel: KeRestoreFloatingPointState(%#x) restores an area that is not "
                         "this thread's most recent save, honoured\n",
                         area);
    }
    return STATUS_SUCCESS;
}

void kernel_fpstate_reset(void)
{
    pthread_mutex_lock(&table_mutex);
    table_count = 0u;
    anomalies = 0u;
    pthread_mutex_unlock(&table_mutex);
}

uint32_t kernel_fpstate_depth(void)
{
    const pthread_t self = pthread_self();
    uint32_t depth = 0u;
    pthread_mutex_lock(&table_mutex);
    for (uint32_t index = 0u; index < table_count; index++) {
        if (pthread_equal(table[index].owner, self)) {
            depth++;
        }
    }
    pthread_mutex_unlock(&table_mutex);
    return depth;
}

uint32_t kernel_fpstate_total_depth(void)
{
    pthread_mutex_lock(&table_mutex);
    const uint32_t total = table_count;
    pthread_mutex_unlock(&table_mutex);
    return total;
}

uint32_t kernel_fpstate_anomaly_count(void)
{
    pthread_mutex_lock(&table_mutex);
    const uint32_t count = anomalies;
    pthread_mutex_unlock(&table_mutex);
    return count;
}

size_t kernel_fpstate_register(void)
{
    /* A fresh registration is a fresh kernel: the saves belong to a guest that is gone. */
    kernel_fpstate_reset();
    size_t registered = 0u;
    if (kernel_hle_register(ORD_KeSaveFloatingPointState, hle_save)) {
        registered++;
    }
    if (kernel_hle_register(ORD_KeRestoreFloatingPointState, hle_restore)) {
        registered++;
    }
    return registered;
}
