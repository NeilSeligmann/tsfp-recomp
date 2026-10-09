/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "xinput_devices.h"
#include "xinput_hle.h"
#define TYPE 0x46C75Cu
#define DECL SCRATCH_DATA
#define OUT1 (SCRATCH_DATA + 0x100u)
#define OUT2 (SCRATCH_DATA + 0x120u)
static const uint32_t declarations[8] = {
    0x46C6E0u,8u,0x46C8A0u,4u,0x46C894u,4u,0x46C75Cu,4u
};
static void initialise(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    xinput_hle_init();
    xinput_devices_reset();
    xinput_devices_set_fatal(catching_fatal);
    map_fixed(0x46C000u, 0x1000u);
    map_fixed(0x771000u, 0x1000u);
    memcpy(kernel_guest_at(DECL, 32u), declarations, 32u);
    memset(kernel_guest_at(OUT1, 16u), 0xAAu, 16u);
    memset(kernel_guest_at(OUT2, 16u), 0xBBu, 16u);
}
static void check_untouched(void)
{
    CHECK_EQ_U32(load(TYPE), 5u);
    CHECK_EQ_U32(load(TYPE + 4u), 3u);
    CHECK_EQ_U32(load(TYPE + 8u), 6u);
    CHECK_EQ_U32(load(OUT1), 0xAAAAAAAAu);
    CHECK_EQ_U32(load(OUT2), 0xBBBBBBBBu);
}
static void test_preflight(void)
{
    initialise();
    RUN_EXPECTING_FATAL((void)xinput_devices_get(TYPE)); CHECK(fatal_seen);
    store(0x771454u, 0x11223344u);
    for (unsigned i = 0u; i < 8u; i++) {
        store(DECL + i * 4u, declarations[i] ^ 1u);
        RUN_EXPECTING_FATAL((void)xinput_devices_init_empty(4u, DECL)); CHECK(fatal_seen);
        CHECK_EQ_U32(load(0x771454u), 0x11223344u);
        store(DECL + i * 4u, declarations[i]);
    }
    RUN_EXPECTING_FATAL((void)xinput_devices_init_empty(3u, DECL)); CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)xinput_devices_init_empty(4u, 0x46CFF0u)); CHECK(fatal_seen);
    store(TYPE, 1u);
    RUN_EXPECTING_FATAL((void)xinput_devices_init_empty(4u, DECL)); CHECK(fatal_seen);
    CHECK_EQ_U32(load(0x771454u), 0x11223344u);
    store(TYPE, 0u);
    CHECK(xinput_hle_attach_synthetic_pad(0u));
    RUN_EXPECTING_FATAL((void)xinput_devices_init_empty(4u, DECL)); CHECK(fatal_seen);
    CHECK(xinput_hle_port_connected(0u));
    CHECK(xinput_hle_detach_synthetic_pad(0u));
    CHECK_EQ_U32(xinput_devices_init_empty(4u, DECL), 0u);
    CHECK_EQ_U32(load(0x771454u), 0u);
    RUN_EXPECTING_FATAL((void)xinput_devices_init_empty(4u, DECL)); CHECK(fatal_seen);
    store(TYPE, 5u); store(TYPE + 4u, 3u); store(TYPE + 8u, 6u);
    const uint32_t invalid[][2] = {{0u,OUT2},{OUT1,0u},{TYPE,OUT2},{OUT1,TYPE+4u},
        {OUT1,0x46C6E0u},{TYPE-2u,OUT2},{OUT1,OUT1},{OUT1,OUT1+1u},{OUT1,0x46CFFEu},{OUT1,0xFFFFFFFEu}};
    for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        RUN_EXPECTING_FATAL((void)xinput_devices_changes(TYPE, invalid[i][0], invalid[i][1]));
        CHECK(fatal_seen); check_untouched();
    }
    RUN_EXPECTING_FATAL((void)xinput_devices_peek(TYPE, OUT1, TYPE+8u));
    CHECK(fatal_seen); check_untouched();
    RUN_EXPECTING_FATAL((void)xinput_devices_get(TYPE+1u)); CHECK(fatal_seen); check_untouched();
    CHECK(xinput_hle_attach_synthetic_pad(3u));
    RUN_EXPECTING_FATAL((void)xinput_devices_get(TYPE)); CHECK(fatal_seen); check_untouched();
    xinput_devices_reset();
    CHECK(xinput_hle_port_connected(3u));
    environment_end();
}
/* The enumeration mask semantics the original bytes pin (tests/test_xinput_devices_oracle.py
 * measures them against the user's XBE); asserted here too so a plain build can kill their
 * mutants (T351 survivors xin-dev-get-*, xin-dev-peek-*, xin-dev-changes-*, xin-dev-status-*).
 * Per-component assertions on purpose: a swap inside a checked aggregate survives both. */
static void set_table(uint32_t current, uint32_t pending, uint32_t snapshot)
{
    store(TYPE, current); store(TYPE + 4u, pending); store(TYPE + 8u, snapshot);
}
static void test_get_consumes_pending_and_snapshots_current(void)
{
    initialise();
    CHECK_EQ_U32(xinput_devices_init_empty(4u, DECL), 0u);
    set_table(5u, 3u, 6u);
    CHECK_EQ_U32(xinput_devices_get(TYPE), 5u);
    CHECK_EQ_U32(load(TYPE), 5u);
    CHECK_EQ_U32(load(TYPE + 4u), 0u);
    CHECK_EQ_U32(load(TYPE + 8u), 5u);
    environment_end();
}
static void test_peek_reports_without_committing(void)
{
    initialise();
    CHECK_EQ_U32(xinput_devices_init_empty(4u, DECL), 0u);
    set_table(5u, 3u, 6u);
    CHECK_EQ_U32(xinput_devices_peek(TYPE, OUT1, OUT2), 5u);
    CHECK_EQ_U32(load(OUT1), 6u);
    CHECK_EQ_U32(load(OUT2), 0u);
    CHECK_EQ_U32(load(TYPE), 5u);
    CHECK_EQ_U32(load(TYPE + 4u), 3u);
    CHECK_EQ_U32(load(TYPE + 8u), 6u);
    /* Reconnect is the AND of all three masks, nonzero here (1 & 7 & 5). */
    set_table(5u, 1u, 7u);
    CHECK_EQ_U32(xinput_devices_peek(TYPE, OUT1, OUT2), 5u);
    CHECK_EQ_U32(load(OUT1), 7u);
    CHECK_EQ_U32(load(OUT2), 1u);
    environment_end();
}
static void test_changes_splits_insertions_from_removals(void)
{
    initialise();
    CHECK_EQ_U32(xinput_devices_init_empty(4u, DECL), 0u);
    set_table(5u, 3u, 6u);
    CHECK_EQ_U32(xinput_devices_changes(TYPE, OUT1, OUT2), 1u);
    CHECK_EQ_U32(load(OUT1), 1u);
    CHECK_EQ_U32(load(OUT2), 2u);
    CHECK_EQ_U32(load(TYPE + 4u), 0u);
    CHECK_EQ_U32(load(TYPE + 8u), 5u);
    /* A removal alone still answers "something changed". */
    set_table(4u, 2u, 6u);
    CHECK_EQ_U32(xinput_devices_changes(TYPE, OUT1, OUT2), 1u);
    CHECK_EQ_U32(load(OUT1), 0u);
    CHECK_EQ_U32(load(OUT2), 2u);
    /* No pending change reports nothing, even when the masks differ. */
    set_table(5u, 0u, 6u);
    CHECK_EQ_U32(xinput_devices_changes(TYPE, OUT1, OUT2), 0u);
    CHECK_EQ_U32(load(OUT1), 0u);
    CHECK_EQ_U32(load(OUT2), 0u);
    CHECK_EQ_U32(load(TYPE + 8u), 6u);
    environment_end();
}
static void test_status_reads_count_and_flag(void)
{
    initialise();
    CHECK_EQ_U32(xinput_devices_init_empty(4u, DECL), 0u);
    CHECK_EQ_U32(xinput_devices_enumeration_status(), 0u);
    store_byte(0x771370u, 1u);
    CHECK_EQ_U32(xinput_devices_enumeration_status(), 1u);
    store_byte(0x771370u, 0u);
    store(0x771454u, 7u);
    CHECK_EQ_U32(xinput_devices_enumeration_status(), 1u);
    environment_end();
}
static void test_touching_ranges_are_not_overlap(void)
{
    initialise();
    CHECK_EQ_U32(xinput_devices_init_empty(4u, DECL), 0u);
    set_table(5u, 3u, 6u);
    /* Adjacent outputs, and outputs touching the gamepad table's edges, share no byte
     * and must be accepted (an unexpected refusal aborts the suite here). */
    CHECK_EQ_U32(xinput_devices_peek(TYPE, OUT1, OUT1 + 4u), 5u);
    CHECK_EQ_U32(load(OUT1), 6u);
    CHECK_EQ_U32(load(OUT1 + 4u), 0u);
    CHECK_EQ_U32(xinput_devices_changes(TYPE, TYPE + 12u, TYPE - 4u), 1u);
    CHECK_EQ_U32(load(TYPE + 12u), 1u);
    CHECK_EQ_U32(load(TYPE - 4u), 2u);
    environment_end();
}
static void test_peek_dispatch_argument_order(void)
{
    initialise();
    CHECK_EQ_U32(xinput_devices_register(), 5u);
    const uint32_t init_args[2] = {4u, DECL};
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    CHECK(kernel_frame_build(&frame, call_scratch, 0x100u, init_args, 2u));
    CHECK_EQ_U32(xinput_hle_call(0x46DBCDu, &frame), 0u);
    set_table(5u, 1u, 7u);
    const uint32_t args[3] = {TYPE, OUT1, OUT2};
    memset(&frame, 0, sizeof(frame));
    CHECK(kernel_frame_build(&frame, call_scratch, 0x100u, args, 3u));
    CHECK_EQ_U32(xinput_hle_call(0x46DB91u, &frame), 5u);
    CHECK_EQ_U32(load(OUT1), 7u);
    CHECK_EQ_U32(load(OUT2), 1u);
    environment_end();
}
static void test_dispatch(void)
{
    initialise();
    CHECK_EQ_U32(xinput_devices_register(), 5u);
    const uint32_t args[2] = {4u,DECL};
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    CHECK(kernel_frame_build(&frame, call_scratch, 0x100u, args, 2u));
    frame.stack_limit = frame.stack_ptr + 8u;
    RUN_EXPECTING_FATAL((void)xinput_hle_call(0x46DBCDu, &frame)); CHECK(fatal_seen);
    frame.stack_limit = frame.stack_ptr + 12u;
    CHECK_EQ_U32(xinput_hle_call(0x46DBCDu, &frame), 0u);
    CHECK_EQ_U32(xinput_hle_call(0x46F3A8u, &frame), 0u);
    const uint64_t calls = xinput_hle_entry(0x46DBCDu)->call_count;
    xinput_devices_reset();
    CHECK(xinput_hle_entry(0x46DBCDu)->call_count == calls);
    CHECK(xinput_hle_entry(0x46DBCDu)->state == XINPUT_ENTRY_IMPLEMENTED);
    CHECK_EQ_U32(xinput_devices_init_empty(4u, DECL), 0u);
    environment_end();
}
int main(void)
{
    test_preflight(); test_dispatch();
    test_get_consumes_pending_and_snapshots_current();
    test_peek_reports_without_committing();
    test_changes_splits_insertions_from_removals();
    test_status_reads_count_and_flag();
    test_touching_ranges_are_not_overlap();
    test_peek_dispatch_argument_order();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
