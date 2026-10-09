/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_HOST_HOST_SNAPSHOT_H
#define TSFP_HOST_HOST_SNAPSHOT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* T1153 (docs/state-snapshot.md): a whole-process snapshot of the running host at a poll index.
 *
 * The host does not serialise its own state. Its guest threads are native pthreads running lifted C on host
 * stacks, so a thread continuation, the kernel objects, the open files, the GPU replay model and the audio and
 * input state all live in the process image. The snapshot is therefore a DMTCP checkpoint of the whole process
 * tree (tools/snapshot), taken by calling dmtcp_checkpoint() at a poll index of port 0, which the title issues
 * once per frame and which is a function of the guest, never of wall time. The same index on a resumed run
 * continues from that point. Valid only for the exact host binary and shared libraries it was taken on, the
 * launcher (tools.snapshot) pins and verifies them. */

/* Result of dmtcp_checkpoint() (DMTCP 3.2.0 include/dmtcp.h). */
#define HOST_SNAPSHOT_AFTER_CHECKPOINT 1
#define HOST_SNAPSHOT_AFTER_RESTART 2

/* Replaceable for tests. Returns the dmtcp_checkpoint() result, or a negative value when the DMTCP layer is not
 * loaded. The default is the weak dmtcp_checkpoint symbol of libdmtcp.so. */
typedef int (*host_snapshot_checkpoint_fn)(void);
void host_snapshot_set_checkpoint_function(host_snapshot_checkpoint_fn function);

/* Take the snapshot when port 0 has been polled `at_poll` times (at_poll >= 1). Installs the poll observer.
 * Refuses with a reason in `error` when `at_poll` is 0. */
bool host_snapshot_arm(uint64_t at_poll, char *error, size_t error_size);
/* T1153: end the run deterministically when port 0 has been polled `at_poll` times (the title stops being driven by
 * wall time or a timeout): `stop` is called once, from the polling thread, outside the device lock, and must end the run
 * (the host passes a function that calls host_run_stop). For bisecting and for round trip tests of a title that no longer
 * stops by itself. Installs the poll observer. Refuses when at_poll is 0 or `stop` is NULL. */
typedef void (*host_snapshot_stop_fn)(uint64_t polls);
bool host_snapshot_arm_stop(uint64_t at_poll, host_snapshot_stop_fn stop, char *error, size_t error_size);
/* True once the snapshot was taken or this process was resumed from it (for the exit report). */
uint64_t host_snapshot_taken_at(void);
/* Forget the arming (tests). */
void host_snapshot_reset(void);

/* The first device file descriptor that a snapshot cannot carry (a GPU, DRM render node), as a link target in
 * `path`, or false. The software Vulkan device (lavapipe) holds none. Exposed for tests. */
bool host_snapshot_find_device_fd(char *path, size_t path_size);
/* One file-backed mapping path per line, deduplicated, for the launcher's library pin. Returns the count, or -1. */
int host_snapshot_write_mapped_files(const char *out_path);
#endif
