/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent T1012 registered NtReadFile acceptance. Only reuse ordinal/setup
 * helpers; the old main's assertions are never executed. Guest reference:
 * T1007 kernel1.0.4627.1, notification Events,4096-byte known file. */
#define main t1012_fixture_helpers_main
#include "test_kernel_async_io.c"
#undef main

#define REFERENCE_BYTES 4096u
#define REFERENCE_EOF 0xC0000011u
#define IOSB_BEFORE 0x13579BDFu
#define IOSB_AFTER 0x2468ACE0u

static unsigned cases;

static void reference_setup(uint32_t options, bool selected_set, bool neighbor_set)
{
    setup();
    (void)kernel_object_register();
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/music.bin", host_root);
    FILE *stream = fopen(path, "wb");
    CHECK(stream != NULL);
    if (stream == NULL) exit(EXIT_FAILURE);
    for (uint32_t i = 0u; i < REFERENCE_BYTES; i++) file_bytes[i] = (uint8_t)(i & 0xFFu);
    CHECK_EQ_U32(fwrite(file_bytes, 1u, REFERENCE_BYTES, stream), REFERENCE_BYTES);
    CHECK_EQ_U32(fclose(stream), 0u);
    CHECK_EQ_U32(open_file(options), STATUS_SUCCESS);
    CHECK(kernel_object_release(event_handle()));
    CHECK_EQ_U32(kernel_object_create_event(0u, 0u, at(OFF_EVENT)), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_object_create_event(0u, 0u, at(OFF_EVENT2)), STATUS_SUCCESS);
    if (selected_set) CHECK_EQ_U32(kernel_object_event_set(event_handle(), NULL), STATUS_SUCCESS);
    if (neighbor_set) CHECK_EQ_U32(kernel_object_event_set(event2_handle(), NULL), STATUS_SUCCESS);
    CHECK(event_state(event_handle()) == selected_set);
    CHECK(event_state(event2_handle()) == neighbor_set);
    write32(at(OFF_IOSB) - 4u, IOSB_BEFORE);
    write32(at(OFF_IOSB) + 8u, IOSB_AFTER);
    poison(OFF_BUFFER - 4u, 72u);
    cases++;
}

static void preserve_guards(bool neighbor_set)
{
    CHECK_EQ_U32(read32(at(OFF_IOSB) - 4u), IOSB_BEFORE);
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 8u), IOSB_AFTER);
    CHECK_EQ_U32(read32(at(OFF_BUFFER) - 4u), 0xA5A5A5A5u);
    CHECK_EQ_U32(read32(at(OFF_BUFFER) + 64u), 0xA5A5A5A5u);
    CHECK(event_state(event2_handle()) == neighbor_set);
}

static void observe(const char *family, uint32_t options, uint32_t returned)
{
    printf("CASE %u %s options=%#x returned=%#x iosb=%#x/%#x selected=%u neighbor=%u\n",
           cases, family, (unsigned)options, (unsigned)returned,
           (unsigned)read32(at(OFF_IOSB)), (unsigned)read32(at(OFF_IOSB) + 4u),
           event_state(event_handle()) ? 1u : 0u, event_state(event2_handle()) ? 1u : 0u);
}

static void eof_case(uint32_t options, uint64_t offset, bool selected_set, bool neighbor_set)
{
    reference_setup(options, selected_set, neighbor_set);
    kernel_async_io_set_enabled(true);
    const uint32_t status = read_file(read32(at(OFF_HANDLE)), event_handle(), at(OFF_IOSB),
                                      at(OFF_BUFFER), 16u, offset);
    observe("EOF", options, status);
    CHECK_EQ_U32(status, REFERENCE_EOF);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_PENDING);
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), 0xA5A5A5A5u);
    CHECK(!event_state(event_handle()));
    CHECK(buffer_untouched(at(OFF_BUFFER), 64u));
    CHECK_EQ_U32(kernel_async_io_get_stats().submitted, 0u);
    preserve_guards(neighbor_set);
    teardown();
}

static void success_case(uint32_t options, bool zero, bool selected_set, bool neighbor_set)
{
    reference_setup(options, selected_set, neighbor_set);
    kernel_async_io_set_enabled(true);
    const uint32_t status = read_file(read32(at(OFF_HANDLE)), event_handle(), at(OFF_IOSB),
                                      at(OFF_BUFFER), zero ? 0u : 16u, zero ? 6144u : 4088u);
    observe(zero ? "ZERO" : "PARTIAL", options, status);
    CHECK_EQ_U32(status, STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), zero ? 0u : 8u);
    CHECK(event_state(event_handle()));
    if (!zero) CHECK(buffer_matches(at(OFF_BUFFER), 4088u, 8u));
    CHECK(buffer_untouched(at(OFF_BUFFER) + (zero ? 0u : 8u), zero ? 64u : 56u));
    CHECK_EQ_U32(kernel_async_io_get_stats().submitted, 0u);
    preserve_guards(neighbor_set);
    teardown();
}

static void validation_case(uint32_t options, unsigned kind, bool selected_set)
{
    reference_setup(options, selected_set, !selected_set);
    kernel_async_io_set_enabled(true);
    uint32_t file = read32(at(OFF_HANDLE));
    uint32_t event = event_handle();
    uint32_t expected = STATUS_INVALID_HANDLE;
    if (kind == 0u) file = 0xDEADBEEFu;
    if (kind == 1u) {
        const uint32_t close_args[1] = {file};
        CHECK_EQ_U32(call_ordinal(ORD_NT_CLOSE, close_args, 1u), STATUS_SUCCESS);
    }
    if (kind == 2u) event = 0xDEADBEEFu;
    if (kind == 3u) {
        event = file;
        expected = STATUS_OBJECT_TYPE_MISMATCH;
    }
    if (kind == 4u) {
        CHECK_EQ_U32(kernel_object_create_event(0u, 0u, at(OFF_DUP)), STATUS_SUCCESS);
        event = read32(at(OFF_DUP));
        const uint32_t close_args[1] = {event};
        CHECK_EQ_U32(call_ordinal(ORD_NT_CLOSE, close_args, 1u), STATUS_SUCCESS);
    }
    const uint32_t status = read_file(file, event, at(OFF_IOSB), at(OFF_BUFFER), 16u, 0u);
    observe("VALIDATION", options, status);
    CHECK_EQ_U32(status, expected);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_PENDING);
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), 0xA5A5A5A5u);
    CHECK(event_state(event_handle()) == selected_set);
    CHECK(buffer_untouched(at(OFF_BUFFER), 64u));
    CHECK_EQ_U32(kernel_async_io_get_stats().submitted, 0u);
    preserve_guards(!selected_set);
    teardown();
}

/* Positive queued reads are existing host-model regressions, not a new
 * measurement of accepted asynchronous EOF/error or sub-poll timing. */
static void positive_queued_case(bool file_object, bool close_file)
{
    reference_setup(OPEN_ASYNC, file_object, true);
    kernel_async_io_set_enabled(true);
    kernel_async_io_set_file_object_enabled(file_object);
    const uint32_t file = read32(at(OFF_HANDLE));
    const uint32_t status = read_file(file, file_object ? 0u : event_handle(), at(OFF_IOSB),
                                     at(OFF_BUFFER), 32u, 0u);
    observe(file_object ? "FILE_OBJECT" : "QUEUED", OPEN_ASYNC, status);
    CHECK_EQ_U32(status, STATUS_PENDING);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_PENDING);
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), 0xA5A5A5A5u);
    CHECK(buffer_untouched(at(OFF_BUFFER), 64u));
    CHECK(event_state(event_handle()) == file_object);
    CHECK_EQ_U32(kernel_async_io_get_stats().submitted, 1u);
    if (file_object) {
        CHECK_EQ_U32(kernel_async_io_wait_file(file, false, 0u), KERNEL_ASYNC_WAIT_SATISFIED);
    } else {
        if (close_file) {
            const uint32_t close_args[1] = {file};
            CHECK_EQ_U32(call_ordinal(ORD_NT_CLOSE, close_args, 1u), STATUS_SUCCESS);
        }
        (void)kernel_clock_advance_to(UINT64_MAX / 2u);
        CHECK_EQ_U32(kernel_async_io_service(), 1u);
    }
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), 32u);
    CHECK(buffer_matches(at(OFF_BUFFER), 0u, 32u));
    CHECK(buffer_untouched(at(OFF_BUFFER) + 32u, 32u));
    CHECK(event_state(event_handle()));
    CHECK_EQ_U32(kernel_async_io_get_stats().pending, 0u);
    preserve_guards(true);
    teardown();
}

int main(void)
{
    const uint32_t options[2] = {0x60u, 0x40u};
    for (unsigned mode = 0u; mode < 2u; mode++) {
        for (unsigned states = 0u; states < 4u; states++) {
            eof_case(options[mode], 4096u, (states & 1u) != 0u, (states & 2u) != 0u);
            eof_case(options[mode], 6144u, (states & 1u) != 0u, (states & 2u) != 0u);
            success_case(options[mode], true, (states & 1u) != 0u, (states & 2u) != 0u);
            success_case(options[mode], false, (states & 1u) != 0u, (states & 2u) != 0u);
        }
        for (unsigned kind = 0u; kind < 5u; kind++) {
            validation_case(options[mode], kind, false);
            validation_case(options[mode], kind, true);
        }
    }
    positive_queued_case(false, false);
    positive_queued_case(false, true);
    positive_queued_case(true, false);
    printf("T1012: %u cases, %d checks, %d failures\n", cases, checks, failures);
    return failures == 0 ? 0 : 1;
}
