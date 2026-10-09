/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1633: what the host observes of the guest for the event-driven route (docs/input-replay.md, "Event driven route"):
 * file I/O of the guest (kernel_file observer), calls of chosen guest functions (the cooperative safepoint), a cheap
 * presented-frame fingerprint and sampled guest memory. Everything here only OBSERVES: it never writes a guest byte or
 * changes a result the title sees, so the flags that turn it on are ignored by the route identity.
 *
 * Counters are monotonic and atomic. The route (src/input/xinput_route.c) registers WATCHES (one per file or call
 * condition) when it is created, then reads them and takes its own baselines, so nothing here keeps a history.
 */
#ifndef TSFP_HOST_ROUTE_PROBE_H
#define TSFP_HOST_ROUTE_PROBE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "kernel_file.h"
#include "xinput_route.h"

#define ROUTE_PROBE_MAX_WATCHES 24u
#define ROUTE_PROBE_MAX_MEM 8u

void route_probe_reset(void); /* tests: forget watches, counters, the log and the memory list */
/* Turn the observers on (file observer registered with the kernel layer). Idempotent. The facts for the recorder and the
 * event log need it too. */
void route_probe_enable(void);
bool route_probe_enabled(void);
/* Clock override for tests; NULL restores CLOCK_MONOTONIC. now_ms is never 0 for a real event. */
void route_probe_set_clock(uint64_t (*now_ms)(void *user), void *user);

/* Observers. Cheap, thread safe, no-ops while disabled. `kind` and `path` are what kernel_file_set_observer passes. */
void route_probe_note_file(kernel_file_event_kind kind, const char *path, uint64_t bytes);
void route_probe_note_call(uint32_t va);
void route_probe_note_frame(uint64_t fingerprint);
/* True when a call watch exists (the host must then run the cooperative safepoint provider, main.c). */
bool route_probe_calls_watched(void);

/* The view the route uses (route hooks `probe`). */
const xinput_route_probe *route_probe_view(void);

/* Recorder facts for the `# mark-info:` line: what happened since the previous mark (or since enable), then the next segment
 * starts. Signature of xinput_record_mark_facts_fn. */
bool route_probe_mark_facts(unsigned mark, char *out, size_t out_size, void *user);

/* Event log (--route-event-log FILE): one line per event, `t=<ms since open> poll=<port 0 poll> <kind> <detail>`. */
bool route_probe_log_open(const char *path, char *error, size_t error_size);
void route_probe_log_close(void);
bool route_probe_log_active(void);
void route_probe_log_line(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* Sampled guest memory for the log (--route-log-mem SPEC, SPEC = [*]ADDR[+OFF][:W]). Changes are logged as
 * `mem <spec> <old> -> <new>`. Add before start. */
bool route_probe_mem_add(const char *spec, char *error, size_t error_size);
typedef bool (*route_probe_read_fn)(uint32_t address, unsigned width, uint32_t *value, void *user);
/* One sampling pass (tests call it directly). */
void route_probe_mem_sample(route_probe_read_fn read, void *user);
/* Start a thread sampling every `period_ms` until route_probe_reset / route_probe_mem_stop. */
bool route_probe_mem_start(route_probe_read_fn read, void *user, unsigned period_ms);
void route_probe_mem_stop(void);

/* End of run summary lines on stdout (per path counters of the files watched, totals). */
void route_probe_report(void);
#endif
