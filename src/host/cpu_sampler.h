/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1235: --cpu-profile FILE, an in-process CPU sampling profiler (no ptrace, which the container refuses).
 * SIGPROF at a fixed rate of process CPU time; the handler stores (thread id, instruction pointer) in a static
 * array (async-signal-safe: no allocation, no locks). At the stop the samples are written as text, one per line,
 * `tid rip`, after a header `# base <exe load address> <exe path>`, and tools/cpu_profile_report.py resolves
 * them with addr2line into the self-time per thread and function. It samples CPU time only: a blocked thread is
 * invisible here (the thread state sampler of tools/timing_probe.py shows those). Observation only, never feeds
 * back into the run. DEFAULT OFF.
 *
 * T1495 phase marking: `kill -USR1 <pid>` (pid in FILE.pid) writes the samples since the last dump to FILE.<phase> and
 * resets the counter, the phase is the first word of FILE.phase else a counter; FILE.ack records each finished dump.
 */
#ifndef TSFP_HOST_CPU_SAMPLER_H
#define TSFP_HOST_CPU_SAMPLER_H

#include <stdbool.h>

/* Start sampling at `hz` samples per CPU second. False when a sampler is already running or the timer fails. */
bool cpu_sampler_start(const char *path, unsigned hz, bool wall);
/* wall = true: instead of SIGPROF a sampler thread signals every thread `hz` times per WALL second and each handler
 * stores an 8 frame call chain, so threads blocked in a wait are attributed to the function that waits (the file's
 * `# mode wall`; every line is `tid rip0 rip1 ...`, innermost first). */
/* Stop and write the file. Safe without a start. */
void cpu_sampler_stop(void);

#endif
