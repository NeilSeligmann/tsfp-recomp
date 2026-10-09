/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1741: pure limits and hit filter of the guest write watch (guest_watch.h). No guest access, no signals.
 */
#include "guest_watch.h"

#include <stdio.h>

bool guest_watch_check(const guest_dump_set *set, char *error, size_t error_size)
{
    const char *reason = NULL;
    if (set == NULL || set->count == 0u) {
        reason = "no range";
    } else if (set->count > GUEST_WATCH_MAX_RANGES) {
        reason = "too many ranges";
    } else {
        for (unsigned index = 0u; index < set->count; index++) {
            if (set->ranges[index].length == 0u || set->ranges[index].length > GUEST_WATCH_MAX_LENGTH) {
                reason = "range longer than 64 bytes";
                break;
            }
        }
    }
    if (reason != NULL && error != NULL && error_size > 0u) {
        snprintf(error, error_size, "%s", reason);
    }
    return reason == NULL;
}

bool guest_watch_is_hit(uint32_t fault, uint32_t low, uint32_t length, bool changed)
{
    if (changed) {
        return true;
    }
    return fault >= low && (uint64_t)fault < (uint64_t)low + length;
}
