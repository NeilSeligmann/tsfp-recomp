/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T1153: the poll-indexed snapshot trigger. A fake checkpoint function stands in for DMTCP, so the test checks
 * WHEN the checkpoint is requested (exactly once, at the Nth poll of port 0, never for another port), what
 * the arming refuses, and the launcher inputs (mapped-file list, device file scan). */
#define _POSIX_C_SOURCE 200809L
#include "test_d3d8_support.h"
#include "host_snapshot.h"
#include "xinput_devices.h"
#include "xinput_hle.h"
#include "xinput_source.h"
#include <unistd.h>
#define TYPE 0x46C75Cu
#define DECL SCRATCH_DATA
static const uint32_t declarations[8] = {0x46C6E0u, 8u, 0x46C8A0u, 4u, 0x46C894u, 4u, 0x46C75Cu, 4u};
static int g_calls;
static uint64_t g_polls_at_call;
static int g_result = HOST_SNAPSHOT_AFTER_CHECKPOINT;
static int g_stops;
static uint64_t g_stop_polls;
static void fake_stop(uint64_t polls)
{
    g_stops++;
    g_stop_polls = polls;
}
static int fake_checkpoint(void)
{
    g_calls++;
    g_polls_at_call = xinput_source_port_poll_count(0u);
    return g_result;
}
static void initialise(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    xinput_hle_init();
    xinput_devices_reset();
    xinput_source_reset();
    xinput_devices_set_fatal(catching_fatal);
    map_fixed(0x46C000u, 0x1000u);
    map_fixed(0x771000u, 0x1000u);
    memcpy(kernel_guest_at(DECL, 32u), declarations, 32u);
    CHECK(xinput_hle_attach_synthetic_pad(0u));
    xinput_devices_enable_synthetic_pad(true);
    CHECK_EQ_U32(xinput_devices_init_empty(4u, DECL), 0u);
    g_calls = 0;
    g_polls_at_call = 0u;
    g_result = HOST_SNAPSHOT_AFTER_CHECKPOINT;
    g_stops = 0;
    g_stop_polls = 0u;
    host_snapshot_reset();
    host_snapshot_set_checkpoint_function(fake_checkpoint);
}
static void finish(void)
{
    host_snapshot_reset();
    xinput_source_reset();
    xinput_devices_enable_synthetic_pad(false);
    environment_end();
}
static void poll_n(unsigned port, unsigned count)
{
    for (unsigned i = 0u; i < count; i++) (void)xinput_source_poll_port(port);
}
static void test_checkpoint_exactly_at_the_nth_poll_of_port_0(void)
{
    initialise();
    char error[100];
    CHECK(host_snapshot_arm(5u, error, sizeof error));
    poll_n(0u, 4u);
    CHECK_EQ_U32((uint32_t)g_calls, 0u);
    poll_n(1u, 9u);
    CHECK_EQ_U32((uint32_t)g_calls, 0u);
    poll_n(0u, 1u);
    CHECK_EQ_U32((uint32_t)g_calls, 1u);
    CHECK_EQ_U32((uint32_t)g_polls_at_call, 5u);
    CHECK_EQ_U32((uint32_t)host_snapshot_taken_at(), 5u);
    poll_n(0u, 20u);
    CHECK_EQ_U32((uint32_t)g_calls, 1u);
    finish();
}
static void test_arming_refusals_and_the_resumed_result(void)
{
    initialise();
    char error[100] = "";
    CHECK(!host_snapshot_arm(0u, error, sizeof error));
    CHECK(strstr(error, "at least 1") != NULL);
    g_result = HOST_SNAPSHOT_AFTER_RESTART;
    CHECK(host_snapshot_arm(1u, error, sizeof error));
    poll_n(0u, 2u);
    CHECK_EQ_U32((uint32_t)g_calls, 1u);
    finish();
}
static void test_stop_at_poll_ends_the_run_once_at_the_exact_poll(void)
{
    initialise();
    char error[100] = "";
    CHECK(!host_snapshot_arm_stop(0u, fake_stop, error, sizeof error));
    CHECK(strstr(error, "at least 1") != NULL);
    CHECK(!host_snapshot_arm_stop(3u, NULL, error, sizeof error));
    CHECK(host_snapshot_arm_stop(7u, fake_stop, error, sizeof error));
    poll_n(1u, 20u);
    poll_n(0u, 6u);
    CHECK_EQ_U32((uint32_t)g_stops, 0u);
    poll_n(0u, 1u);
    CHECK_EQ_U32((uint32_t)g_stops, 1u);
    CHECK_EQ_U32((uint32_t)g_stop_polls, 7u);
    poll_n(0u, 30u);
    CHECK_EQ_U32((uint32_t)g_stops, 1u);
    finish();
}
static void test_snapshot_then_stop_at_the_same_poll_checkpoints_first(void)
{
    initialise();
    char error[100];
    CHECK(host_snapshot_arm(4u, error, sizeof error));
    CHECK(host_snapshot_arm_stop(4u, fake_stop, error, sizeof error));
    poll_n(0u, 4u);
    CHECK_EQ_U32((uint32_t)g_calls, 1u);
    CHECK_EQ_U32((uint32_t)g_stops, 1u);
    finish();
}
static void test_unarmed_never_checkpoints(void)
{
    initialise();
    poll_n(0u, 50u);
    CHECK_EQ_U32((uint32_t)g_calls, 0u);
    CHECK_EQ_U32((uint32_t)host_snapshot_taken_at(), 0u);
    finish();
}
static void test_mapped_file_list_and_device_scan(void)
{
    char path[] = "/tmp/tsfp-snapshot-maps-XXXXXX";
    const int descriptor = mkstemp(path);
    CHECK(descriptor >= 0);
    close(descriptor);
    const int count = host_snapshot_write_mapped_files(path);
    CHECK(count > 0);
    FILE *file = fopen(path, "r");
    CHECK(file != NULL);
    char line[4200];
    bool saw_libc = false;
    int lines = 0;
    while (file != NULL && fgets(line, sizeof line, file) != NULL) {
        lines++;
        CHECK(line[0] == '/');
        if (strstr(line, "libc.so") != NULL) saw_libc = true;
    }
    if (file != NULL) fclose(file);
    unlink(path);
    CHECK_EQ_U32((uint32_t)lines, (uint32_t)count);
    CHECK(saw_libc);
    char device[300];
    CHECK(!host_snapshot_find_device_fd(device, sizeof device));
}
int main(void)
{
    test_checkpoint_exactly_at_the_nth_poll_of_port_0();
    test_arming_refusals_and_the_resumed_result();
    test_unarmed_never_checkpoints();
    test_stop_at_poll_ends_the_run_once_at_the_exact_poll();
    test_snapshot_then_stop_at_the_same_poll_checkpoints_first();
    test_mapped_file_list_and_device_scan();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
