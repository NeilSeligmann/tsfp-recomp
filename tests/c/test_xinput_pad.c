/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T717: the opt-in FABRICATED synthetic pad. Contracts are measured against the original bytes in
 * tests/test_xinput_pad_oracle.py, this suite pins the policy and every refusal in a plain build. */
#include "test_d3d8_support.h"
#include "xinput_devices.h"
#include "xinput_hle.h"
#include "xinput_source.h"
#include <pthread.h>
#define TYPE 0x46C75Cu
#define DECL SCRATCH_DATA
#define OUT (SCRATCH_DATA + 0x100u)
#define FEEDBACK (SCRATCH_DATA + 0x200u)
#define HANDLE 0x58504430u
#define POLL (SCRATCH_DATA + 0x300u)
static const uint32_t declarations[8] = {
    0x46C6E0u,8u,0x46C8A0u,4u,0x46C894u,4u,0x46C75Cu,4u
};
static void initialise(bool pad)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    xinput_hle_init();
    xinput_devices_reset();
    xinput_devices_enable_synthetic_pad(false);
    xinput_devices_set_fatal(catching_fatal);
    map_fixed(0x46C000u, 0x1000u);
    map_fixed(0x771000u, 0x1000u);
    memcpy(kernel_guest_at(DECL, 32u), declarations, 32u);
    if (pad) {
        CHECK(xinput_hle_attach_synthetic_pad(0u));
        xinput_devices_enable_synthetic_pad(true);
    }
}
static void finish(void)
{
    xinput_devices_enable_synthetic_pad(false);
    environment_end();
}
static uint32_t opened(void)
{
    CHECK_EQ_U32(xinput_devices_init_empty(4u, DECL), 0u);
    const uint32_t handle = xinput_pad_open(TYPE, 0u, 0u, 0u);
    CHECK_EQ_U32(handle, HANDLE);
    return handle;
}
static void fill(uint32_t at, uint8_t value, uint32_t bytes) { memset(kernel_guest_at(at, bytes), value, bytes); }
static void test_default_off_keeps_the_empty_backend(void)
{
    initialise(false);
    CHECK(!xinput_devices_synthetic_pad_enabled());
    CHECK_EQ_U32(xinput_devices_init_empty(4u, DECL), 0u);
    CHECK_EQ_U32(load(TYPE), 0u); CHECK_EQ_U32(load(TYPE + 4u), 0u); CHECK_EQ_U32(load(TYPE + 8u), 0u);
    store(OUT, 0xAAAAAAAAu); store(OUT + 4u, 0xBBBBBBBBu);
    CHECK_EQ_U32(xinput_devices_changes(TYPE, OUT, OUT + 4u), 0u);
    CHECK_EQ_U32(load(OUT), 0u); CHECK_EQ_U32(load(OUT + 4u), 0u);
    RUN_EXPECTING_FATAL((void)xinput_pad_open(TYPE, 0u, 0u, 0u)); CHECK(fatal_seen);
    /* The pad functions are not registered without the flag: a boot that never asks is byte identical. */
    CHECK_EQ_U32(xinput_devices_register(), 5u);
    CHECK(xinput_hle_entry(0x46E133u)->state == XINPUT_ENTRY_STUB);
    finish();
}
static void test_enable_requires_port_zero_synthetic(void)
{
    /* An attached HLE pad alone, without the adapter flag, stays refused. */
    initialise(false);
    CHECK(xinput_hle_attach_synthetic_pad(0u));
    RUN_EXPECTING_FATAL((void)xinput_devices_init_empty(4u, DECL)); CHECK(fatal_seen);
    finish();
    initialise(false);
    xinput_devices_enable_synthetic_pad(true);
    RUN_EXPECTING_FATAL((void)xinput_devices_init_empty(4u, DECL)); CHECK(fatal_seen);
    CHECK(xinput_hle_attach_synthetic_pad(1u));
    RUN_EXPECTING_FATAL((void)xinput_devices_init_empty(4u, DECL)); CHECK(fatal_seen);
    CHECK(xinput_hle_attach_synthetic_pad(0u));
    RUN_EXPECTING_FATAL((void)xinput_devices_init_empty(4u, DECL)); CHECK(fatal_seen);
    finish();
    initialise(true);
    CHECK(xinput_hle_attach_synthetic_pad(2u));
    RUN_EXPECTING_FATAL((void)xinput_devices_init_empty(4u, DECL)); CHECK(fatal_seen);
    finish();
}
static void test_insertion_is_reported_exactly_once(void)
{
    initialise(true);
    CHECK_EQ_U32(xinput_devices_init_empty(4u, DECL), 0u);
    CHECK_EQ_U32(load(TYPE), 1u); CHECK_EQ_U32(load(TYPE + 4u), 1u); CHECK_EQ_U32(load(TYPE + 8u), 0u);
    store(OUT, 0xAAAAAAAAu); store(OUT + 4u, 0xBBBBBBBBu);
    CHECK_EQ_U32(xinput_devices_changes(TYPE, OUT, OUT + 4u), 1u);
    CHECK_EQ_U32(load(OUT), 1u); CHECK_EQ_U32(load(OUT + 4u), 0u);
    CHECK_EQ_U32(xinput_devices_changes(TYPE, OUT, OUT + 4u), 0u);
    CHECK_EQ_U32(load(OUT), 0u); CHECK_EQ_U32(load(OUT + 4u), 0u);
    CHECK_EQ_U32(xinput_devices_get(TYPE), 1u);
    /* Other device tables stay empty. */
    CHECK_EQ_U32(load(0x46C6E0u), 0u); CHECK_EQ_U32(load(0x46C7C0u), 0u);
    CHECK_EQ_U32(load(0x46C894u), 0u); CHECK_EQ_U32(load(0x46C8A0u), 0u);
    finish();
}
static void test_open_refuses_unknown_shapes_and_returns_null_for_measured_ones(void)
{
    initialise(true);
    RUN_EXPECTING_FATAL((void)xinput_pad_open(TYPE, 0u, 0u, 0u)); CHECK(fatal_seen);
    CHECK_EQ_U32(xinput_devices_init_empty(4u, DECL), 0u);
    /* Unknown tables and polling parameters are still refused. */
    const uint32_t bad[][4] = {{TYPE+1u,0u,0u,0u},{0u,0u,0u,0u},{TYPE,0u,0u,1u},{0x46C6E0u,0u,0u,1u}};
    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); i++) {
        RUN_EXPECTING_FATAL((void)xinput_pad_open(bad[i][0], bad[i][1], bad[i][2], bad[i][3]));
        CHECK(fatal_seen);
    }
    /* T723, measured NULL: every known non-gamepad table, absent ports, slot 1. No session effect. */
    const uint32_t tables[] = {0x46C6E0u, 0x46C75Cu, 0x46C7C0u, 0x46C894u, 0x46C8A0u, 0x46C8ACu};
    const uint32_t ports[] = {0u, 1u, 2u, 3u, 4u, 0xFFFFFFFFu};
    const uint32_t slots[] = {0u, 1u, 2u, 3u, 0xFFFFFFFFu};
    for (size_t t = 0u; t < 6u; t++)
        for (size_t p = 0u; p < 6u; p++)
            for (size_t l = 0u; l < 5u; l++) {
                const bool pad = tables[t] == TYPE && ports[p] == 0u && slots[l] != 1u;
                if (pad) continue;
                CHECK_EQ_U32(xinput_pad_open(tables[t], ports[p], slots[l], 0u), 0u);
            }
    /* Only slot 1 selects another slot: 2, 3 and 0xFFFFFFFF open like slot 0, and Close ends it. */
    for (size_t l = 0u; l < 5u; l++) {
        if (slots[l] == 1u) continue;
        CHECK_EQ_U32(xinput_pad_open(TYPE, 0u, slots[l], 0u), HANDLE);
        CHECK_EQ_U32(xinput_pad_open(TYPE, 0u, slots[l], 0u), 0u);
        xinput_pad_close(HANDLE);
    }
    CHECK_EQ_U32(xinput_pad_open(TYPE, 0u, 0u, 0u), HANDLE);
    CHECK_EQ_U32(xinput_pad_open(TYPE, 0u, 0u, 0u), 0u);
    finish();
}
static void test_removal_reports_once_and_fails_the_open_handle(void)
{
    initialise(true);
    const uint32_t handle = opened();
    CHECK_EQ_U32(xinput_devices_changes(TYPE, OUT, OUT + 4u), 1u);
    fill(OUT, 0xAAu, 32u);
    xinput_pad_state pad = {0};
    pad.digital_buttons = 0x1234u; pad.thumb_left_x = 100;
    memset(pad.analog, 0x80, 8u);
    CHECK(xinput_hle_set_synthetic_pad_state(0u, pad));
    CHECK_EQ_U32(xinput_pad_state_read(handle, OUT), 0u);
    xinput_pad_remove();
    CHECK_EQ_U32(xinput_hle_connected_count(), 0u);
    CHECK_EQ_U32(load(TYPE), 0u); CHECK_EQ_U32(load(TYPE + 4u), 1u); CHECK_EQ_U32(load(TYPE + 8u), 1u);
    /* Removed pad: 0x48F, the last sample is written, bytes past it are untouched. */
    fill(OUT, 0xAAu, 32u);
    CHECK_EQ_U32(xinput_pad_state_read(handle, OUT), 0x48Fu);
    CHECK_EQ_U32(load(OUT), 2u);
    CHECK_EQ_U32(load(OUT + 4u) & 0xFFFFu, 0x1234u);
    CHECK(((const uint8_t *)kernel_guest_at(OUT, 32u))[22] == 0xAAu);
    fill(OUT, 0xAAu, 32u);
    CHECK_EQ_U32(xinput_pad_capabilities(handle, OUT), 0x48Fu);
    const uint8_t *caps = kernel_guest_at(OUT, 32u);
    CHECK(caps[0] == 0xAAu && caps[1] == 0u && caps[2] == 0u && caps[3] == 0xAAu);
    fill(FEEDBACK, 0u, 0x48u);
    fill(FEEDBACK + 0x42u, 0x11u, 4u);
    CHECK_EQ_U32(xinput_pad_feedback(handle, FEEDBACK), 0x48Fu);
    CHECK_EQ_U32(load(FEEDBACK), 0x48Fu);
    CHECK_EQ_U32((uint32_t)xinput_pad_output_count(), 0u);
    /* The title consumes the removal once. */
    CHECK_EQ_U32(xinput_devices_changes(TYPE, OUT, OUT + 4u), 1u);
    CHECK_EQ_U32(load(OUT), 0u); CHECK_EQ_U32(load(OUT + 4u), 1u);
    CHECK_EQ_U32(xinput_devices_changes(TYPE, OUT, OUT + 4u), 0u);
    /* Open while open and removed, and after Close, is NULL. Use after Close is refused. */
    CHECK_EQ_U32(xinput_pad_open(TYPE, 0u, 0u, 0u), 0u);
    xinput_pad_close(handle);
    CHECK_EQ_U32(xinput_pad_open(TYPE, 0u, 0u, 0u), 0u);
    RUN_EXPECTING_FATAL((void)xinput_pad_state_read(handle, OUT)); CHECK(fatal_seen);
    RUN_EXPECTING_FATAL(xinput_pad_close(handle)); CHECK(fatal_seen);
    RUN_EXPECTING_FATAL(xinput_pad_remove()); CHECK(fatal_seen);
    finish();
}
static void test_removal_before_the_insertion_was_consumed_and_with_feedback_pending(void)
{
    initialise(true);
    CHECK_EQ_U32(xinput_devices_init_empty(4u, DECL), 0u);
    xinput_pad_remove();
    CHECK_EQ_U32(load(TYPE), 0u); CHECK_EQ_U32(load(TYPE + 4u), 1u); CHECK_EQ_U32(load(TYPE + 8u), 0u);
    CHECK_EQ_U32(xinput_devices_changes(TYPE, OUT, OUT + 4u), 0u);
    CHECK_EQ_U32(xinput_pad_open(TYPE, 0u, 0u, 0u), 0u);
    finish();
    /* Removal without a poll: the change counts (T731, measured), so the first read is packet 2 with it. */
    initialise(true);
    const uint32_t unpolled = opened();
    xinput_pad_state moved = {0};
    moved.digital_buttons = 0x0201u;
    CHECK(xinput_hle_set_synthetic_pad_state(0u, moved));
    xinput_pad_remove();
    fill(OUT, 0xAAu, 32u);
    CHECK_EQ_U32(xinput_pad_state_read(unpolled, OUT), 0x48Fu);
    CHECK_EQ_U32(load(OUT), 2u);
    CHECK_EQ_U32(load(OUT + 4u) & 0xFFFFu, 0x0201u);
    finish();
    initialise(true);
    const uint32_t handle = opened();
    fill(FEEDBACK, 0u, 0x48u);
    CHECK_EQ_U32(xinput_pad_feedback(handle, FEEDBACK), 997u);
    RUN_EXPECTING_FATAL(xinput_pad_remove()); CHECK(fatal_seen);
    CHECK_EQ_U32(xinput_hle_connected_count(), 1u);
    finish();
}
static void test_handle_and_buffer_refusals(void)
{
    initialise(true);
    RUN_EXPECTING_FATAL(xinput_pad_close(HANDLE)); CHECK(fatal_seen);
    const uint32_t handle = opened();
    fill(OUT, 0xAAu, 32u);
    const uint32_t wrong[] = {0u, handle + 1u, 0x80000289u};
    for (size_t i = 0u; i < 3u; i++) {
        RUN_EXPECTING_FATAL((void)xinput_pad_capabilities(wrong[i], OUT)); CHECK(fatal_seen);
        RUN_EXPECTING_FATAL((void)xinput_pad_state_read(wrong[i], OUT)); CHECK(fatal_seen);
        RUN_EXPECTING_FATAL((void)xinput_pad_feedback(wrong[i], OUT)); CHECK(fatal_seen);
        RUN_EXPECTING_FATAL(xinput_pad_close(wrong[i])); CHECK(fatal_seen);
    }
    CHECK_EQ_U32(load(OUT), 0xAAAAAAAAu);
    RUN_EXPECTING_FATAL((void)xinput_pad_capabilities(handle, 0u)); CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)xinput_pad_state_read(handle, 0u)); CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)xinput_pad_feedback(handle, 0u)); CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)xinput_pad_state_read(handle, 0xFFFFFFF0u)); CHECK(fatal_seen);
    /* Mapped for a few bytes only: the page ends 0x46D000, the whole output must fit. */
    fill(0x46CFF8u, 0xAAu, 8u);
    RUN_EXPECTING_FATAL((void)xinput_pad_capabilities(handle, 0x46CFF8u)); CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)xinput_pad_state_read(handle, 0x46CFF0u)); CHECK(fatal_seen);
    CHECK_EQ_U32(load(0x46CFF8u), 0xAAAAAAAAu);
    CHECK_EQ_U32(load(0x46CFF0u), 0u);
    /* A notification event in the feedback header is not recovered. */
    fill(FEEDBACK, 0u, 0x46u);
    store(FEEDBACK + 4u, 0x1234u);
    RUN_EXPECTING_FATAL((void)xinput_pad_feedback(handle, FEEDBACK)); CHECK(fatal_seen);
    CHECK_EQ_U32(load(FEEDBACK), 0u);
    CHECK_EQ_U32((uint32_t)xinput_pad_output_count(), 0u);
    finish();
}
static void test_capabilities_bytes(void)
{
    initialise(true);
    const uint32_t handle = opened();
    fill(OUT, 0xAAu, 32u);
    CHECK_EQ_U32(xinput_pad_capabilities(handle, OUT), 0u);
    const uint8_t *at = kernel_guest_at(OUT, 32u);
    CHECK(at[0] == 1u && at[1] == 0u && at[2] == 0u);
    for (unsigned i = 3u; i < 25u; i++) CHECK(at[i] == 0xFFu);
    for (unsigned i = 25u; i < 32u; i++) CHECK(at[i] == 0xAAu);
    finish();
}
static void test_state_packet_and_layout(void)
{
    initialise(true);
    const uint32_t handle = opened();
    fill(OUT, 0xAAu, 32u);
    CHECK_EQ_U32(xinput_pad_state_read(handle, OUT), 0u);
    const uint8_t *at = kernel_guest_at(OUT, 32u);
    CHECK_EQ_U32(load(OUT), 1u);
    for (unsigned i = 4u; i < 22u; i++) CHECK(at[i] == 0u);
    for (unsigned i = 22u; i < 32u; i++) CHECK(at[i] == 0xAAu);
    CHECK_EQ_U32(xinput_pad_state_read(handle, OUT), 0u);
    CHECK_EQ_U32(load(OUT), 1u);
    xinput_pad_state pad = {0};
    pad.digital_buttons = 0x10u;
    for (unsigned i = 0u; i < 8u; i++) pad.analog[i] = (uint8_t)(i + 1u);
    pad.thumb_left_x = 100; pad.thumb_left_y = -200; pad.thumb_right_x = 300; pad.thumb_right_y = -400;
    CHECK(xinput_hle_set_synthetic_pad_state(0u, pad));
    CHECK_EQ_U32(xinput_pad_state_read(handle, OUT), 0u);
    const uint8_t expected[22] = {2,0,0,0, 0x10,0, 0,0,0,0,0,0,0,0,
        100,0, 0x38,0xFF, 0x2C,1, 0x70,0xFE};
    CHECK(memcmp(at, expected, 22u) == 0);
    CHECK_EQ_U32(xinput_pad_state_read(handle, OUT), 0u);
    CHECK_EQ_U32(load(OUT), 2u);
    for (unsigned i = 22u; i < 32u; i++) CHECK(at[i] == 0xAAu);
    finish();
}
/* Measured threshold: bytes below 0x20 read as 0, 0x20 and above pass, the packet follows the raw report. */
static void test_analog_threshold_and_raw_packet(void)
{
    initialise(true);
    const uint32_t handle = opened();
    xinput_pad_state pad = {0};
    CHECK_EQ_U32(xinput_pad_state_read(handle, OUT), 0u);
    const uint8_t *at = kernel_guest_at(OUT, 32u);
    uint32_t packet = load(OUT);
    const uint8_t values[] = {1u, 0x1Fu, 0x20u, 0x21u, 0xFFu};
    const uint8_t views[] = {0u, 0u, 0x20u, 0x21u, 0xFFu};
    for (unsigned i = 0u; i < 8u; i++)
        for (size_t v = 0u; v < sizeof(values); v++) {
            memset(pad.analog, 0, 8u);
            pad.analog[i] = values[v];
            CHECK(xinput_hle_set_synthetic_pad_state(0u, pad));
            CHECK_EQ_U32(xinput_pad_state_read(handle, OUT), 0u);
            CHECK(at[6u + i] == views[v]);
            for (unsigned other = 0u; other < 8u; other++) if (other != i) CHECK(at[6u + other] == 0u);
            CHECK(load(OUT) != packet);
            packet = load(OUT);
        }
    finish();
}
static void test_feedback_pends_then_completes_and_sends_rumble(void)
{
    initialise(true);
    const uint32_t handle = opened();
    uint8_t report[6];
    CHECK(!xinput_pad_last_output(report));
    fill(FEEDBACK, 0u, 0x48u);
    fill(OUT, 0u, 32u);
    memcpy(kernel_guest_at(FEEDBACK + 0x42u, 4u), (const uint8_t[]){0x34, 0x12, 0x78, 0x56}, 4u);
    CHECK_EQ_U32(xinput_pad_feedback(handle, FEEDBACK), 997u);
    CHECK_EQ_U32(load(FEEDBACK), 0x3E5u);
    CHECK(xinput_pad_last_output(report));
    const uint8_t expected[6] = {0, 6, 0x34, 0x12, 0x78, 0x56};
    CHECK(memcmp(report, expected, 6u) == 0);
    CHECK_EQ_U32((uint32_t)xinput_pad_output_count(), 1u);
    /* Still pending until the next poll, the original completes within 20 ms. */
    CHECK_EQ_U32(load(FEEDBACK), 0x3E5u);
    CHECK_EQ_U32(xinput_pad_state_read(handle, OUT), 0u);
    CHECK_EQ_U32(load(FEEDBACK), 0u);
    /* Bytes beyond the status word and the rumble pair are untouched. */
    CHECK(((const uint8_t *)kernel_guest_at(FEEDBACK, 0x48u))[0x47] == 0u);
    xinput_pad_close(handle);
    RUN_EXPECTING_FATAL((void)xinput_pad_state_read(handle, OUT)); CHECK(fatal_seen);
    finish();
}
static void test_dispatch_through_registered_handlers(void)
{
    initialise(true);
    CHECK_EQ_U32(xinput_devices_register(), 5u);
    CHECK_EQ_U32(xinput_devices_register_pad(), 5u);
    kernel_call_frame frame;
    const uint32_t init_args[2] = {4u, DECL};
    memset(&frame, 0, sizeof(frame));
    CHECK(kernel_frame_build(&frame, call_scratch, 0x100u, init_args, 2u));
    CHECK_EQ_U32(xinput_hle_call(0x46DBCDu, &frame), 0u);
    const uint32_t open_args[4] = {TYPE, 0u, 0u, 0u};
    memset(&frame, 0, sizeof(frame));
    CHECK(kernel_frame_build(&frame, call_scratch, 0x100u, open_args, 4u));
    CHECK_EQ_U32(xinput_hle_call(0x46E133u, &frame), HANDLE);
    const uint32_t state_args[2] = {HANDLE, OUT};
    memset(&frame, 0, sizeof(frame));
    CHECK(kernel_frame_build(&frame, call_scratch, 0x100u, state_args, 2u));
    CHECK_EQ_U32(xinput_hle_call(0x46E36Du, &frame), 0u);
    CHECK_EQ_U32(load(OUT), 1u);
    CHECK_EQ_U32(xinput_hle_call(0x46E195u, &frame), 0u);
    const uint32_t close_args[1] = {HANDLE};
    memset(&frame, 0, sizeof(frame));
    CHECK(kernel_frame_build(&frame, call_scratch, 0x100u, close_args, 1u));
    (void)xinput_hle_call(0x46E189u, &frame);
    /* Too few stack arguments is refused loudly. */
    memset(&frame, 0, sizeof(frame));
    CHECK(kernel_frame_build(&frame, call_scratch, 0x100u, open_args, 4u));
    frame.stack_limit = frame.stack_ptr + 8u;
    RUN_EXPECTING_FATAL((void)xinput_hle_call(0x46E133u, &frame)); CHECK(fatal_seen);
    finish();
}
static void test_reset_clears_the_session_not_the_policy(void)
{
    initialise(true);
    (void)opened();
    xinput_devices_reset();
    CHECK(xinput_devices_synthetic_pad_enabled());
    /* Guest memory is not session state: a cold init still needs zero masks. */
    RUN_EXPECTING_FATAL((void)xinput_devices_init_empty(4u, DECL)); CHECK(fatal_seen);
    store(TYPE, 0u); store(TYPE + 4u, 0u); store(TYPE + 8u, 0u);
    CHECK_EQ_U32(xinput_devices_init_empty(4u, DECL) , 0u);
    CHECK_EQ_U32(xinput_pad_open(TYPE, 0u, 0u, 0u), HANDLE);
    finish();
}
static void set_buttons(uint16_t buttons)
{
    xinput_pad_state pad = {0};
    pad.digital_buttons = buttons;
    CHECK(xinput_hle_set_synthetic_pad_state(0u, pad));
}
/* T731: the packet number counts raw report changes since the Open, every Open starts at 1. */
static void test_packet_counts_raw_changes_per_open(void)
{
    initialise(true);
    CHECK_EQ_U32(xinput_devices_init_empty(4u, DECL), 0u);
    set_buttons(1u); set_buttons(2u);  /* before the Open: not counted */
    CHECK_EQ_U32(xinput_hle_synthetic_report_changes(0u), 2u);
    const uint32_t handle = xinput_pad_open(TYPE, 0u, 0u, 0u);
    CHECK_EQ_U32(handle, HANDLE);
    CHECK_EQ_U32(xinput_pad_state_read(handle, OUT), 0u); CHECK_EQ_U32(load(OUT), 1u);
    set_buttons(3u); set_buttons(2u); set_buttons(2u); set_buttons(4u);  /* three changes, one repeat */
    CHECK_EQ_U32(xinput_pad_state_read(handle, OUT), 0u); CHECK_EQ_U32(load(OUT), 4u);
    xinput_pad_state same = {0};
    same.digital_buttons = 4u; same.packet_number = 99u;  /* the packet field is not part of the report */
    CHECK(xinput_hle_set_synthetic_pad_state(0u, same));
    CHECK_EQ_U32(xinput_pad_state_read(handle, OUT), 0u); CHECK_EQ_U32(load(OUT), 4u);
    xinput_pad_close(handle);
    set_buttons(5u);
    const uint32_t again = xinput_pad_open(TYPE, 0u, 0u, 0u);
    CHECK_EQ_U32(xinput_pad_state_read(again, OUT), 0u); CHECK_EQ_U32(load(OUT), 1u);
    set_buttons(6u);
    CHECK_EQ_U32(xinput_pad_state_read(again, OUT), 0u); CHECK_EQ_U32(load(OUT), 2u);
    /* A refused set (no device) counts nothing. */
    CHECK(!xinput_hle_set_synthetic_pad_state(1u, (xinput_pad_state){0}));
    CHECK_EQ_U32(xinput_hle_synthetic_report_changes(1u), 0u);
    CHECK_EQ_U32(xinput_hle_synthetic_report_changes(9u), 0u);
    finish();
}
/* T731: only the status word and the rumble pair of the feedback block are written. Bytes 4..0x41 are library
 * scratch in the original (pool pointers that move with the open history, measured), they are left alone. */
static void test_feedback_leaves_the_reserved_bytes_alone(void)
{
    initialise(true);
    const uint32_t handle = opened();
    fill(FEEDBACK, 0x5Au, 0x48u);
    store(FEEDBACK + 4u, 0u);
    store(FEEDBACK, 0u);
    memcpy(kernel_guest_at(FEEDBACK + 0x42u, 4u), (const uint8_t[]){1, 2, 3, 4}, 4u);
    CHECK_EQ_U32(xinput_pad_feedback(handle, FEEDBACK), 997u);
    const uint8_t *at = kernel_guest_at(FEEDBACK, 0x48u);
    for (unsigned i = 8u; i < 0x42u; i++) CHECK(at[i] == 0x5Au);
    CHECK_EQ_U32(load(FEEDBACK + 4u), 0u);
    CHECK(at[0x46] == 0x5Au && at[0x47] == 0x5Au);
    CHECK_EQ_U32(xinput_pad_state_read(handle, OUT), 0u);
    for (unsigned i = 8u; i < 0x42u; i++) CHECK(at[i] == 0x5Au);
    finish();
}
static void put_polling(uint8_t flags, uint8_t input, uint8_t output, uint8_t reserved)
{
    memcpy(kernel_guest_at(POLL, 4u), (const uint8_t[]){flags, input, output, reserved}, 4u);
}
/* T731: polling blocks. fAutoPoll set with non-zero intervals opens like the NULL block, the rest is refused. */
static void test_polling_blocks(void)
{
    initialise(true);
    CHECK_EQ_U32(xinput_devices_init_empty(4u, DECL), 0u);
    const uint8_t accepted[][4] = {{1,1,0,0},{1,8,8,0},{1,255,1,0},{3,1,8,0},{3,255,255,0},{3,8,1,0}};
    for (size_t i = 0u; i < sizeof(accepted) / sizeof(accepted[0]); i++) {
        put_polling(accepted[i][0], accepted[i][1], accepted[i][2], accepted[i][3]);
        const uint32_t handle = xinput_pad_open(TYPE, 0u, 0u, POLL);
        CHECK_EQ_U32(handle, HANDLE);
        xinput_pad_close(handle);
    }
    const uint8_t refused[][4] = {{0,1,1,0},{0,8,8,0},{2,1,1,0},{1,0,8,0},{3,8,0,0},{5,1,1,0},{1,1,1,1},{0x81,1,1,0}};
    for (size_t i = 0u; i < sizeof(refused) / sizeof(refused[0]); i++) {
        put_polling(refused[i][0], refused[i][1], refused[i][2], refused[i][3]);
        RUN_EXPECTING_FATAL((void)xinput_pad_open(TYPE, 0u, 0u, POLL)); CHECK(fatal_seen);
        if (i < 3u)
            CHECK(strcmp(fatal_text, "polling without fAutoPoll is not recovered") == 0);
        if (i == 3u)
            CHECK(strcmp(fatal_text, "input interval 0 does not return in the oracle") == 0);
        if (i == 4u)
            CHECK(strcmp(fatal_text, "output interval 0 does not return in the oracle") == 0);
    }
    /* Unmapped block and blocks with an Open that returns NULL are refused, none opened a pad. */
    RUN_EXPECTING_FATAL((void)xinput_pad_open(TYPE, 0u, 0u, 5u)); CHECK(fatal_seen);
    put_polling(1u, 8u, 8u, 0u);
    RUN_EXPECTING_FATAL((void)xinput_pad_open(TYPE, 1u, 0u, POLL)); CHECK(fatal_seen);
    CHECK(strcmp(fatal_text, "polling parameters with an Open that returns NULL are not recovered") == 0);
    RUN_EXPECTING_FATAL((void)xinput_pad_open(TYPE, 0u, 1u, POLL)); CHECK(fatal_seen);
    CHECK(strcmp(fatal_text, "polling parameters with an Open that returns NULL are not recovered") == 0);
    RUN_EXPECTING_FATAL((void)xinput_pad_open(0x46C6E0u, 0u, 0u, POLL)); CHECK(fatal_seen);
    CHECK(strcmp(fatal_text, "polling parameters with an Open that returns NULL are not recovered") == 0);
    const uint32_t handle = xinput_pad_open(TYPE, 0u, 0u, POLL);
    CHECK_EQ_U32(handle, HANDLE);
    RUN_EXPECTING_FATAL((void)xinput_pad_open(TYPE, 0u, 0u, POLL)); CHECK(fatal_seen);
    finish();
}
static uint32_t consume_removal(void)
{
    return xinput_devices_changes(TYPE, OUT, OUT + 4u);
}
/* T731 reinsertion: the table reports one insertion (and the unconsumed removal too), the new pad is at rest. */
static void test_reinsertion(void)
{
    initialise(true);
    const uint32_t handle = opened();
    CHECK_EQ_U32(consume_removal(), 1u);
    set_buttons(0x1234u);
    xinput_pad_remove();
    RUN_EXPECTING_FATAL(xinput_pad_insert()); CHECK(fatal_seen);  /* the old handle is still open */
    CHECK(strcmp(fatal_text, "reinsertion with the old handle still open is not recovered") == 0);
    xinput_pad_close(handle);
    CHECK_EQ_U32(consume_removal(), 1u); CHECK_EQ_U32(load(OUT), 0u); CHECK_EQ_U32(load(OUT + 4u), 1u);
    xinput_pad_insert();
    CHECK_EQ_U32(xinput_hle_connected_count(), 1u);
    CHECK_EQ_U32(load(TYPE), 1u); CHECK_EQ_U32(load(TYPE + 4u), 1u); CHECK_EQ_U32(load(TYPE + 8u), 0u);
    RUN_EXPECTING_FATAL(xinput_pad_insert()); CHECK(fatal_seen);  /* not removed */
    CHECK_EQ_U32(consume_removal(), 1u); CHECK_EQ_U32(load(OUT), 1u); CHECK_EQ_U32(load(OUT + 4u), 0u);
    CHECK_EQ_U32(consume_removal(), 0u);
    const uint32_t fresh = xinput_pad_open(TYPE, 0u, 0u, 0u);
    CHECK_EQ_U32(fresh, HANDLE);
    fill(OUT, 0xAAu, 32u);
    CHECK_EQ_U32(xinput_pad_state_read(fresh, OUT), 0u);
    const uint8_t rest[22] = {1,0,0,0};
    CHECK(memcmp(kernel_guest_at(OUT, 22u), rest, 22u) == 0);  /* at rest, packet 1 */
    set_buttons(2u);
    CHECK_EQ_U32(xinput_pad_state_read(fresh, OUT), 0u); CHECK_EQ_U32(load(OUT), 2u);
    finish();
    /* Removal never consumed: the reinsertion reports an insertion and a removal at once. */
    initialise(true);
    CHECK_EQ_U32(xinput_devices_init_empty(4u, DECL), 0u);
    xinput_pad_remove();
    xinput_pad_insert();
    CHECK_EQ_U32(load(TYPE), 1u); CHECK_EQ_U32(load(TYPE + 4u), 1u); CHECK_EQ_U32(load(TYPE + 8u), 0u);
    CHECK_EQ_U32(consume_removal(), 1u); CHECK_EQ_U32(load(OUT), 1u); CHECK_EQ_U32(load(OUT + 4u), 0u);
    finish();
    /* Inserting without the adapter or before init is refused. */
    initialise(false);
    RUN_EXPECTING_FATAL(xinput_pad_insert()); CHECK(fatal_seen);
    finish();
}
/* T731: the FABRICATED removal source acts after the n-th GetState, exactly like an explicit remove there. */
static void test_scheduled_removal(void)
{
    uint8_t scheduled[32], explicit_removal[32];
    initialise(true);
    xinput_pad_remove_after_polls(0u);
    uint32_t handle = opened();
    for (unsigned i = 0u; i < 20u; i++) {
        CHECK_EQ_U32(xinput_pad_state_read(handle, OUT), 0u);
        fill(FEEDBACK, 0u, 0x48u);
        CHECK_EQ_U32(xinput_pad_feedback(handle, FEEDBACK), 997u);
    }
    CHECK_EQ_U32(xinput_hle_connected_count(), 1u);  /* default: never */
    finish();
    initialise(true);
    xinput_pad_remove_after_polls(3u);
    handle = opened();
    CHECK_EQ_U32(consume_removal(), 1u);
    for (unsigned i = 1u; i <= 3u; i++) {
        CHECK_EQ_U32(xinput_hle_connected_count(), 1u);
        set_buttons((uint16_t)i);
        fill(OUT, 0xAAu, 32u);
        CHECK_EQ_U32(xinput_pad_state_read(handle, OUT), 0u);  /* the third read still succeeds */
        fill(FEEDBACK, 0u, 0x48u);
        if (i < 3u) CHECK_EQ_U32(xinput_pad_feedback(handle, FEEDBACK), 997u);
    }
    CHECK_EQ_U32(xinput_hle_connected_count(), 0u);
    memcpy(scheduled, kernel_guest_at(OUT, 32u), 32u);
    CHECK_EQ_U32(xinput_pad_feedback(handle, FEEDBACK), 0x48Fu);
    CHECK_EQ_U32(load(TYPE), 0u); CHECK_EQ_U32(load(TYPE + 4u), 1u); CHECK_EQ_U32(load(TYPE + 8u), 1u);
    CHECK_EQ_U32(consume_removal(), 1u); CHECK_EQ_U32(load(OUT), 0u); CHECK_EQ_U32(load(OUT + 4u), 1u);
    finish();
    initialise(true);
    xinput_pad_remove_after_polls(0u);
    handle = opened();
    CHECK_EQ_U32(consume_removal(), 1u);
    for (unsigned i = 1u; i <= 3u; i++) {
        set_buttons((uint16_t)i);
        fill(OUT, 0xAAu, 32u);
        CHECK_EQ_U32(xinput_pad_state_read(handle, OUT), 0u);
        fill(FEEDBACK, 0u, 0x48u);
        if (i < 3u) CHECK_EQ_U32(xinput_pad_feedback(handle, FEEDBACK), 997u);
    }
    xinput_pad_remove();
    memcpy(explicit_removal, kernel_guest_at(OUT, 32u), 32u);
    CHECK(memcmp(scheduled, explicit_removal, 32u) == 0);
    CHECK_EQ_U32(xinput_pad_feedback(handle, FEEDBACK), 0x48Fu);
    CHECK_EQ_U32(load(TYPE), 0u); CHECK_EQ_U32(load(TYPE + 4u), 1u); CHECK_EQ_U32(load(TYPE + 8u), 1u);
    finish();
    /* The schedule is policy: it survives a reset, the poll count does not. */
    initialise(true);
    xinput_pad_remove_after_polls(1u);
    xinput_devices_reset();
    handle = opened();
    CHECK_EQ_U32(xinput_pad_state_read(handle, OUT), 0u);
    CHECK_EQ_U32(xinput_hle_connected_count(), 0u);
    xinput_pad_remove_after_polls(0u);
    finish();
}
/* T945: explicit host multiport model, independent of the port0 USB oracle. */
static unsigned feedback_calls[4];
static uint16_t motor_left[4], motor_right[4];
static uint64_t seen_polls[4];
static bool host_states(uint64_t index, unsigned port, xinput_pad_state *state, void *user)
{
    (void)user;
    seen_polls[port] = index;
    state->digital_buttons = (uint16_t)(1u << port);
    state->analog[port] = (uint8_t)(32u + port);
    state->thumb_left_x = (int16_t)(100 + (int)port);
    return true;
}
static bool motors(unsigned port, uint16_t left, uint16_t right, void *user)
{
    (void)user;
    feedback_calls[port]++; motor_left[port] = left; motor_right[port] = right;
    return port != 3u; /* unsupported hardware output does not fabricate capability */
}
static bool decline_port(uint64_t index, unsigned port, xinput_pad_state *state, void *user)
{
    (void)index; (void)port; CHECK(user == (void *)1);
    state->digital_buttons = 0xFFFFu; return false;
}
static bool legacy_state(uint64_t index, xinput_pad_state *state, void *user)
{
    (void)index; CHECK(user == (void *)2); state->digital_buttons = 0x80u; return true;
}
static void *hotplug_worker(void *user)
{
    (void)user;
    for (unsigned i = 0; i < 1000; i++) { (void)xinput_pad_disconnect(3u); (void)xinput_pad_connect(3u); }
    return NULL;
}
static void test_host_multiport_state_and_feedback(void)
{
    initialise(false);
    xinput_source_reset();
    xinput_devices_enable_synthetic_pad(true);
    xinput_devices_enable_multiport(true);
    CHECK(!xinput_pad_connect(4u));
    for (unsigned port = 0; port < 4; port++) CHECK(xinput_pad_connect(port));
    CHECK_EQ_U32(xinput_devices_init_empty(4u, DECL), 0u);
    CHECK_EQ_U32(xinput_devices_get(TYPE), 15u);
    xinput_source_install_ports(host_states, NULL);
    xinput_feedback_install(motors, NULL);
    uint32_t handles[4];
    for (unsigned port = 0; port < 4; port++) {
        handles[port] = xinput_pad_open(TYPE, port, 0u, 0u);
        CHECK(handles[port] != 0u);
        for (unsigned other = 0; other < port; other++) CHECK(handles[port] != handles[other]);
        CHECK_EQ_U32(xinput_pad_open(TYPE, port, 0u, 0u), 0u);
        fill(OUT, 0xA5u, 24u);
        CHECK_EQ_U32(xinput_pad_state_read(handles[port], OUT), 0u);
        CHECK_EQ_U32(load(OUT), 2u);
        CHECK_EQ_U32(*(uint16_t *)kernel_guest_at(OUT + 4u, 2u), 1u << port);
        CHECK_EQ_U32(((uint8_t *)kernel_guest_at(OUT + 6u, 8u))[port], 32u + port);
        CHECK_EQ_U32(((uint8_t *)kernel_guest_at(OUT + 22u, 2u))[0], 0xA5u);
        CHECK_EQ_U32((uint32_t)seen_polls[port], 0u);
        fill(FEEDBACK + port * 0x80u, 0xA5u, 0x46u);
        store(FEEDBACK + port * 0x80u + 4u, 0u);
        uint16_t values[2] = {(uint16_t)(1000u + port), (uint16_t)(2000u + port)};
        memcpy(kernel_guest_at(FEEDBACK + port * 0x80u + 0x42u, 4u), values, 4u);
        CHECK_EQ_U32(xinput_pad_feedback(handles[port], FEEDBACK + port * 0x80u), 997u);
        CHECK_EQ_U32(motor_left[port], 1000u + port); CHECK_EQ_U32(motor_right[port], 2000u + port);
        CHECK_EQ_U32(((uint8_t *)kernel_guest_at(FEEDBACK + port * 0x80u + 8u, 1u))[0], 0xA5u);
    }
    CHECK_EQ_U32(xinput_pad_state_read(handles[0], OUT), 0u);
    CHECK_EQ_U32(load(FEEDBACK), 0u);
    CHECK_EQ_U32(load(FEEDBACK + 0x80u), 997u); /* completion is port-specific */
    CHECK(xinput_pad_disconnect(1u));
    CHECK_EQ_U32(load(FEEDBACK + 0x80u), 0x1Fu);
    CHECK_EQ_U32(motor_left[1], 0u); CHECK_EQ_U32(motor_right[1], 0u);
    CHECK_EQ_U32(xinput_hle_pad_state(1u).digital_buttons, 0u);
    CHECK_EQ_U32(xinput_pad_state_read(handles[1], OUT), 0x48Fu);
    for (unsigned byte = 0; byte < 22; byte++) CHECK_EQ_U32(((uint8_t *)kernel_guest_at(OUT, 22u))[byte], 0u);
    CHECK_EQ_U32(xinput_devices_changes(TYPE, OUT, OUT + 4u), 1u);
    CHECK_EQ_U32(load(OUT), 0u); CHECK_EQ_U32(load(OUT + 4u), 2u);
    CHECK(xinput_pad_connect(1u));
    const uint32_t replacement = xinput_pad_open(TYPE, 1u, 0u, 0u);
    CHECK(replacement != 0u && replacement != handles[1]);
    CHECK_EQ_U32(xinput_pad_state_read(handles[1], OUT), 0x48Fu);
    CHECK_EQ_U32(xinput_pad_state_read(replacement, OUT), 0u);
    CHECK_EQ_U32(load(OUT), 2u);
    CHECK_EQ_U32(xinput_devices_changes(TYPE, OUT, OUT + 4u), 1u);
    CHECK_EQ_U32(load(OUT), 2u); CHECK_EQ_U32(load(OUT + 4u), 0u);
    xinput_source_install_ports(decline_port, (void *)1);
    xinput_source_install(legacy_state, (void *)2);
    CHECK_EQ_U32(xinput_pad_state_read(handles[0], OUT), 0u);
    CHECK_EQ_U32(*(uint16_t *)kernel_guest_at(OUT + 4u, 2u), 0x80u);
    CHECK_EQ_U32(xinput_pad_state_read(handles[2], OUT), 0u);
    CHECK_EQ_U32(*(uint16_t *)kernel_guest_at(OUT + 4u, 2u), 4u); /* legacy is port0 only */
    xinput_source_install_ports(host_states, NULL);
    pthread_t worker;
    CHECK_EQ_U32((uint32_t)pthread_create(&worker, NULL, hotplug_worker, NULL), 0u);
    for (unsigned i = 0; i < 1000; i++) {
        uint32_t status = xinput_pad_state_read(handles[3], OUT);
        CHECK(status == 0u || status == 0x48Fu);
    }
    CHECK_EQ_U32((uint32_t)pthread_join(worker, NULL), 0u);
    CHECK_EQ_U32(xinput_pad_state_read(handles[3], OUT), 0x48Fu);
    xinput_pad_close(handles[1]); /* cannot close or cancel a replacement connection */
    CHECK_EQ_U32(xinput_pad_state_read(replacement, OUT), 0u);
    for (unsigned port = 0; port < 4; port++) xinput_pad_close(port == 1u ? replacement : handles[port]);
    CHECK_EQ_U32(motor_left[0], 0u); CHECK_EQ_U32(motor_left[2], 0u); CHECK_EQ_U32(motor_left[3], 0u);
    xinput_devices_reset();
    xinput_source_reset();
    xinput_devices_enable_multiport(false);
    finish();
}

int main(void)
{
    test_default_off_keeps_the_empty_backend();
    test_enable_requires_port_zero_synthetic();
    test_insertion_is_reported_exactly_once();
    test_open_refuses_unknown_shapes_and_returns_null_for_measured_ones();
    test_removal_reports_once_and_fails_the_open_handle();
    test_removal_before_the_insertion_was_consumed_and_with_feedback_pending();
    test_handle_and_buffer_refusals();
    test_capabilities_bytes();
    test_state_packet_and_layout();
    test_analog_threshold_and_raw_packet();
    test_feedback_pends_then_completes_and_sends_rumble();
    test_dispatch_through_registered_handlers();
    test_reset_clears_the_session_not_the_policy();
    test_packet_counts_raw_changes_per_open();
    test_feedback_leaves_the_reserved_bytes_alone();
    test_polling_blocks();
    test_reinsertion();
    test_scheduled_removal();
    test_host_multiport_state_and_feedback();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
