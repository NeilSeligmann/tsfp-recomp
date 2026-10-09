/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_LIVE_REPORT_PENDING_H
#define TSFP_GPU_LIVE_REPORT_PENDING_H
/* T1246: the visibility reports a pipelined live frame still owes the guest. The live renderer runs a frame behind the guest thread
 * (live_render.c), so the report words of the frame's occlusion queries are written by the presenter thread after the Swap returned.
 * The serial renderer wrote them before the Swap returned, so the guest could never see a report slot half done. To keep that,
 * every guest entry that reads or rewrites a report slot (d3d8_visibility.c) calls `live_report_pending_wait` first: it returns once
 * no queued frame owes THAT slot a report. Slots are guest addresses, packages are the frame buffers of the pipeline. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct live_report_pending live_report_pending;

live_report_pending *live_report_pending_create(size_t capacity);
void live_report_pending_destroy(live_report_pending *pending);
/* Guest thread, before the frame is queued: `address` owes a report to the guest. False when the table is full (queue nothing). */
bool live_report_pending_add(live_report_pending *pending, uint32_t address, unsigned package);
/* Presenter thread: the report of `address` was written (or refused). address 0 settles every report of `package` (a frame ended). */
void live_report_pending_done(live_report_pending *pending, uint32_t address, unsigned package);
/* Guest thread: block while any queued frame still owes `address` a report. Returns the nanoseconds waited (0 when nothing was owed). */
uint64_t live_report_pending_wait(live_report_pending *pending, uint32_t address);
size_t live_report_pending_count(live_report_pending *pending);

#endif
