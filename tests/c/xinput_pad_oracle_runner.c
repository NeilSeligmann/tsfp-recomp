/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T717: runs the synthetic pad adapter through the scenario tests/test_xinput_pad_oracle.py runs on the original
 * bytes, and writes the transcript the test compares byte for byte. */
#include "test_d3d8_support.h"
#include "xinput_devices.h"
#include "xinput_hle.h"
#define TYPE 0x46C75Cu
#define SCRATCH SCRATCH_DATA
static const uint32_t declarations[8] = {
    0x46C6E0u,8u,0x46C8A0u,4u,0x46C894u,4u,0x46C75Cu,4u
};
static FILE *output;
static void put(const void *bytes, size_t length)
{
    if (fwrite(bytes, length, 1u, output) != 1u) exit(2);
}
static void put32(uint32_t value) { put(&value, 4u); }
static void put_buffer(uint32_t at, uint32_t length) { put(kernel_guest_at(at, length), length); }
static const uint32_t tables[6] = {0x46C6E0u, 0x46C75Cu, 0x46C7C0u, 0x46C894u, 0x46C8A0u, 0x46C8ACu};
static const uint32_t ports[6] = {0u, 1u, 2u, 3u, 4u, 0xFFFFFFFFu};
static const uint32_t slots[5] = {0u, 1u, 2u, 3u, 0xFFFFFFFFu};
static void start(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    xinput_hle_init();
    xinput_devices_reset();
    xinput_devices_set_fatal(catching_fatal);
    map_fixed(0x46C000u, 0x1000u);
    map_fixed(0x771000u, 0x1000u);
    memcpy(kernel_guest_at(SCRATCH, 32u), declarations, 32u);
    CHECK(xinput_hle_attach_synthetic_pad(0u));
    xinput_devices_enable_synthetic_pad(true);
    CHECK_EQ_U32(xinput_devices_init_empty(4u, SCRATCH), 0u);
}
/* T723: Open on every table, port and slot, closing what opened, then a second open, as tests/test_xinput_pad_oracle.py. */
static int open_matrix(void)
{
    start();
    for (size_t t = 0u; t < 6u; t++)
        for (size_t p = 0u; p < 6u; p++)
            for (size_t l = 0u; l < 5u; l++) {
                const uint32_t handle = xinput_pad_open(tables[t], ports[p], slots[l], 0u);
                put32(handle != 0u);
                if (handle != 0u) {
                    put32(xinput_pad_open(tables[t], ports[p], slots[l], 0u) != 0u);
                    xinput_pad_close(handle);
                }
            }
    return 0;
}
/* T723 removal: argv[4] is "<consume><open><live><change>" flags as digits, argv[5] the pad vector. */
static void state_call(uint32_t handle, uint32_t buffer)
{
    memset(kernel_guest_at(buffer, 32u), 0xAAu, 32u);
    put32(xinput_pad_state_read(handle, buffer)); put_buffer(buffer, 32u);
}
static void changes_call(void)
{
    const uint32_t added = SCRATCH + 0x100u, removed = SCRATCH + 0x110u;
    memset(kernel_guest_at(added, 16u), 0xAAu, 16u);
    memset(kernel_guest_at(removed, 16u), 0xBBu, 16u);
    put32(xinput_devices_changes(TYPE, added, removed));
    put_buffer(added, 4u); put_buffer(removed, 4u); put_buffer(TYPE, 12u);
}
static int removal(const char *flags, const uint8_t *vector)
{
    const bool consume = flags[0] == '1', open = flags[1] == '1', live = flags[2] == '1', change = flags[3] == '1';
    const bool feedback_before = flags[4] == '1';
    xinput_pad_state pad = {0};
    int16_t thumbs[4];
    memcpy(&pad.digital_buttons, vector, 2u); memcpy(pad.analog, vector + 2u, 8u);
    memcpy(thumbs, vector + 10u, 8u);
    pad.thumb_left_x = thumbs[0]; pad.thumb_left_y = thumbs[1];
    pad.thumb_right_x = thumbs[2]; pad.thumb_right_y = thumbs[3];
    start();
    const uint32_t buffer = SCRATCH + 0x200u, feedback = SCRATCH + 0x300u;
    if (consume) changes_call();
    const uint32_t handle = open ? xinput_pad_open(TYPE, 0u, 0u, 0u) : 0u;
    if (open) put32(handle != 0u);
    if (live) state_call(handle, buffer);
    if (change) CHECK(xinput_hle_set_synthetic_pad_state(0u, pad));
    if (feedback_before) {
        memset(kernel_guest_at(feedback, 0x48u), 0u, 0x48u);
        put32(xinput_pad_feedback(handle, feedback));
        state_call(handle, buffer);
        put_buffer(feedback, 4u);
    }
    xinput_pad_remove();
    put_buffer(TYPE, 12u);
    changes_call();
    if (open) {
        state_call(handle, buffer);
        state_call(handle, buffer);
        memset(kernel_guest_at(buffer, 32u), 0xAAu, 32u);
        put32(xinput_pad_capabilities(handle, buffer)); put_buffer(buffer, 32u);
        memset(kernel_guest_at(feedback, 0x48u), 0u, 0x48u);
        const uint16_t motors[2] = {0x1111u, 0x2222u};
        memcpy(kernel_guest_at(feedback + 0x42u, 4u), motors, 4u);
        put32(xinput_pad_feedback(handle, feedback)); put_buffer(feedback, 4u);
        put32((uint32_t)xinput_pad_output_count());
        put32(xinput_pad_open(TYPE, 0u, 0u, 0u) != 0u);
        xinput_pad_close(handle);
    }
    put32(xinput_pad_open(TYPE, 0u, 0u, 0u) != 0u);
    changes_call();
    return 0;
}
static int scenario(const uint8_t *vector, const uint8_t *polling)
{
    uint16_t buttons, left, right;
    int16_t thumbs[4];
    xinput_pad_state pad = {0};
    memcpy(&buttons, vector, 2u); memcpy(pad.analog, vector + 2u, 8u);
    memcpy(thumbs, vector + 10u, 8u); memcpy(&left, vector + 18u, 2u); memcpy(&right, vector + 20u, 2u);
    pad.digital_buttons = buttons;
    pad.thumb_left_x = thumbs[0]; pad.thumb_left_y = thumbs[1];
    pad.thumb_right_x = thumbs[2]; pad.thumb_right_y = thumbs[3];
    start();
    const uint32_t added = SCRATCH + 0x100u, removed = SCRATCH + 0x110u;
    const uint32_t buffer = SCRATCH + 0x200u, feedback = SCRATCH + 0x300u;
    memset(kernel_guest_at(added, 16u), 0xAAu, 16u);
    memset(kernel_guest_at(removed, 16u), 0xBBu, 16u);
    put32(xinput_devices_changes(TYPE, added, removed));
    put_buffer(added, 4u); put_buffer(removed, 4u); put_buffer(TYPE, 12u);
    const uint32_t polling_at = SCRATCH + 0x400u;
    if (polling != NULL) memcpy(kernel_guest_at(polling_at, 4u), polling, 4u);
    const uint32_t handle = xinput_pad_open(TYPE, 0u, 0u, polling != NULL ? polling_at : 0u);
    put32(handle != 0u);
    memset(kernel_guest_at(buffer, 32u), 0xAAu, 32u);
    put32(xinput_pad_capabilities(handle, buffer)); put_buffer(buffer, 32u);
    memset(kernel_guest_at(buffer, 32u), 0xAAu, 32u);
    put32(xinput_pad_state_read(handle, buffer)); put_buffer(buffer, 32u);
    memset(kernel_guest_at(buffer, 32u), 0xAAu, 32u);
    put32(xinput_pad_state_read(handle, buffer)); put_buffer(buffer, 32u);
    CHECK(xinput_hle_set_synthetic_pad_state(0u, pad));
    memset(kernel_guest_at(buffer, 32u), 0xAAu, 32u);
    put32(xinput_pad_state_read(handle, buffer)); put_buffer(buffer, 32u);
    memset(kernel_guest_at(feedback, 0x48u), 0u, 0x48u);
    const uint16_t motors[2] = {left, right};
    memcpy(kernel_guest_at(feedback + 0x42u, 4u), motors, 4u);
    put32(xinput_pad_feedback(handle, feedback));
    put_buffer(feedback, 4u);
    /* The original completes after a few ms, here the next poll completes it. */
    memset(kernel_guest_at(buffer, 32u), 0xAAu, 32u);
    put32(xinput_pad_state_read(handle, buffer)); put_buffer(buffer, 32u);
    put_buffer(feedback, 4u);
    uint8_t report[6];
    CHECK(xinput_pad_last_output(report));
    put(report, 6u);
    put32((uint32_t)xinput_pad_output_count());
    xinput_pad_close(handle);
    return 0;
}
/* T731 script: tokens as in tests/test_xinput_pad_oracle.py, O open, C close, P poll, digits set the buttons. */
static int script(const char *tokens)
{
    start();
    const uint32_t buffer = SCRATCH + 0x200u;
    uint32_t handle = 0u;
    for (const char *at = tokens; *at != '\0'; at++) {
        if (*at == 'O') {
            handle = xinput_pad_open(TYPE, 0u, 0u, 0u);
            put32(handle != 0u);
        } else if (*at == 'C') {
            xinput_pad_close(handle);
        } else if (*at == 'P') {
            state_call(handle, buffer);
        } else if (*at >= '0' && *at <= '9') {
            xinput_pad_state pad = {0};
            pad.digital_buttons = (uint16_t)(*at - '0');
            CHECK(xinput_hle_set_synthetic_pad_state(0u, pad));
        } else return 2;
    }
    return 0;
}
/* T731 reinsertion: argv[4] flags "<consume removal><opened first><change before removal>", as the test. */
static int reinsert(const char *flags)
{
    const bool consume = flags[0] == '1', first_open = flags[1] == '1', change = flags[2] == '1';
    start();
    const uint32_t buffer = SCRATCH + 0x200u, feedback = SCRATCH + 0x300u;
    changes_call();
    uint32_t handle = first_open ? xinput_pad_open(TYPE, 0u, 0u, 0u) : 0u;
    if (first_open) {
        put32(handle != 0u);
        state_call(handle, buffer);
    }
    if (change) {
        xinput_pad_state moved = {0};
        moved.digital_buttons = 0x1234u; memset(moved.analog, 0x80, 8u); moved.thumb_left_x = 1;
        CHECK(xinput_hle_set_synthetic_pad_state(0u, moved));
        if (first_open) state_call(handle, buffer);
    }
    xinput_pad_remove();
    put_buffer(TYPE, 12u);
    if (consume) changes_call();
    if (first_open) xinput_pad_close(handle);
    xinput_pad_insert();
    put_buffer(TYPE, 12u);
    changes_call();
    changes_call();
    handle = xinput_pad_open(TYPE, 0u, 0u, 0u);
    put32(handle != 0u);
    state_call(handle, buffer);
    state_call(handle, buffer);
    xinput_pad_state again = {0};
    again.digital_buttons = 1u; memset(again.analog, 0x40, 8u);
    CHECK(xinput_hle_set_synthetic_pad_state(0u, again));
    state_call(handle, buffer);
    memset(kernel_guest_at(feedback, 0x48u), 0u, 0x48u);
    put32(xinput_pad_feedback(handle, feedback)); put_buffer(feedback, 4u);
    state_call(handle, buffer);
    put_buffer(feedback, 4u);
    put32((uint32_t)xinput_pad_output_count());
    xinput_pad_remove();
    put_buffer(TYPE, 12u);
    changes_call();
    return 0;
}
static int hex_nibble(char digit) { return digit <= '9' ? digit - '0' : digit - 'a' + 10; }
int main(int argc, char **argv)
{
    if (argc < 3 || argc > 5) return 2;
    FILE *in = fopen(argv[1], "rb");
    output = fopen(argv[2], "wb");
    uint8_t vector[22] = {0};
    if (output == NULL || in == NULL) return 2;
    const char *mode = argc >= 4 ? argv[3] : "scenario";
    const bool takes_vector = strcmp(mode, "removal") == 0 || strcmp(mode, "scenario") == 0 ||
        strcmp(mode, "polling") == 0;
    if (takes_vector && fread(vector, sizeof(vector), 1u, in) != 1u) return 2;
    fclose(in);
    int status = 2;
    if (strcmp(mode, "scenario") == 0) status = scenario(vector, NULL);
    else if (strcmp(mode, "open_matrix") == 0) status = open_matrix();
    else if (strcmp(mode, "removal") == 0 && argc == 5) status = removal(argv[4], vector);
    else if (strcmp(mode, "script") == 0 && argc == 5) status = script(argv[4]);
    else if (strcmp(mode, "reinsert") == 0 && argc == 5) status = reinsert(argv[4]);
    else if (strcmp(mode, "polling") == 0 && argc == 5 && strlen(argv[4]) == 8u) {
        uint8_t block[4];
        for (unsigned i = 0u; i < 4u; i++)
            block[i] = (uint8_t)(hex_nibble(argv[4][2u * i]) * 16 + hex_nibble(argv[4][2u * i + 1u]));
        status = scenario(vector, block);
    }
    fclose(output);
    xinput_devices_enable_synthetic_pad(false);
    environment_end();
    return status == 0 && failures == 0 ? 0 : 1;
}
