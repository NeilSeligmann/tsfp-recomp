/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1004 host controls, revised under T1011 to match immediate EOF and validation
 * behavior measured by T1007. Asynchronous delivery/failure/reset controls remain
 * host-model behavior. */
#define main t1004_existing_fixture_main
#include "test_kernel_async_io.c"
#undef main

static void completion_case(bool asynchronous, uint32_t length, uint64_t offset, uint32_t expected)
{
    setup();
    kernel_async_io_set_enabled(asynchronous);
    CHECK_EQ_U32(open_file(asynchronous ? OPEN_ASYNC : OPEN_SYNC), STATUS_SUCCESS);
    poison(OFF_BUFFER, 32u);
    const bool eof = length != 0u && offset >= FILE_BYTES;
    const bool queued = asynchronous && length != 0u && !eof;
    const uint32_t expected_status = eof ? STATUS_END_OF_FILE
                                         : queued ? STATUS_PENDING : STATUS_SUCCESS;
    CHECK_EQ_U32(read_file(read32(at(OFF_HANDLE)), event_handle(), at(OFF_IOSB),
                          at(OFF_BUFFER), length, offset), expected_status);
    if (eof) {
        CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_PENDING);
        CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), 0xA5A5A5A5u);
        CHECK(buffer_untouched(at(OFF_BUFFER), 32u));
        CHECK(!event_signalled());
        CHECK_EQ_U32(kernel_async_io_get_stats().submitted, 0u);
        teardown();
        return;
    }
    if (queued) {
        CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_PENDING);
        CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), 0xA5A5A5A5u);
        CHECK(buffer_untouched(at(OFF_BUFFER), 32u));
        CHECK(!event_signalled());
        (void)kernel_clock_advance_to(UINT64_MAX / 2u);
        CHECK_EQ_U32(kernel_async_io_service(), 1u);
    }
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), expected);
    CHECK(event_signalled());
    if (expected != 0u) {
        CHECK(buffer_matches(at(OFF_BUFFER), (uint32_t)offset, expected));
    }
    CHECK(buffer_untouched(at(OFF_BUFFER) + expected, 32u - expected));
    teardown();
}

static void invalid_file_case(bool stale)
{
    setup();
    (void)kernel_object_register();
    uint32_t handle = 0xDEADBEEFu;
    if (stale) {
        CHECK_EQ_U32(open_file(OPEN_SYNC), STATUS_SUCCESS);
        handle = read32(at(OFF_HANDLE));
        const uint32_t close_args[1] = {handle};
        CHECK_EQ_U32(call_ordinal(ORD_NT_CLOSE, close_args, 1u), STATUS_SUCCESS);
        CHECK_EQ_U32(open_file(OPEN_SYNC), STATUS_SUCCESS);
        CHECK(read32(at(OFF_HANDLE)) != handle);
    }
    poison(OFF_BUFFER, 32u);
    CHECK_EQ_U32(read_file(handle, event_handle(), at(OFF_IOSB), at(OFF_BUFFER), 16u, 0u),
                 STATUS_INVALID_HANDLE);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_PENDING);
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), 0xA5A5A5A5u);
    CHECK(buffer_untouched(at(OFF_BUFFER), 32u));
    CHECK(!event_signalled());
    teardown();
}

static void bad_event_case(bool asynchronous, unsigned kind)
{
    setup();
    kernel_async_io_set_enabled(asynchronous);
    CHECK_EQ_U32(open_file(asynchronous ? OPEN_ASYNC : OPEN_SYNC), STATUS_SUCCESS);
    const uint32_t file = read32(at(OFF_HANDLE));
    uint32_t event = kind == 0u ? 0xDEADBEEFu : file;
    if (kind == 2u) {
        event = event_handle();
        CHECK(kernel_object_release(event));
        CHECK_EQ_U32(kernel_object_create_event(1u, 0u, at(OFF_EVENT)), STATUS_SUCCESS);
        CHECK(event_handle() != event);
    }
    poison(OFF_BUFFER, 32u);
    const uint32_t expected_status = kind == 1u ? STATUS_OBJECT_TYPE_MISMATCH
                                                 : STATUS_INVALID_HANDLE;
    CHECK_EQ_U32(read_file(file, event, at(OFF_IOSB), at(OFF_BUFFER), 16u, 0u), expected_status);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_PENDING);
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), 0xA5A5A5A5u);
    CHECK(buffer_untouched(at(OFF_BUFFER), 32u));
    CHECK(!event_signalled());
    CHECK_EQ_U32(kernel_async_io_get_stats().submitted, 0u);
    teardown();
}

static void failed_delivery_and_reset(bool reset)
{
    setup();
    kernel_async_io_set_enabled(true);
    CHECK_EQ_U32(open_file(OPEN_ASYNC), STATUS_SUCCESS);
    guest_region_request request = {0};
    request.bytes = 4096u;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status;
    kernel_guest_ptr buffer = guest_region_alloc(&request, &status);
    CHECK(buffer != 0u);
    CHECK_EQ_U32(read_file(read32(at(OFF_HANDLE)), event_handle(), at(OFF_IOSB), buffer, 16u, 0u),
                 STATUS_PENDING);
    CHECK(guest_region_free(buffer));
    if (reset) {
        kernel_async_io_reset();
    }
    (void)kernel_clock_advance_to(UINT64_MAX / 2u);
    CHECK_EQ_U32(kernel_async_io_service(), reset ? 0u : 1u);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), reset ? STATUS_PENDING : STATUS_INVALID_PARAMETER);
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), reset ? 0xA5A5A5A5u : 0u);
    CHECK(event_signalled() == !reset);
    CHECK_EQ_U32(kernel_async_io_get_stats().failed, reset ? 0u : 1u);
    CHECK_EQ_U32(kernel_async_io_get_stats().pending, 0u);
    teardown();
}

/* NtClose is not the queue-reset API and does not cancel prefetched data. */
static void close_pending_file(void)
{
    setup();
    (void)kernel_object_register();
    kernel_async_io_set_enabled(true);
    CHECK_EQ_U32(open_file(OPEN_ASYNC), STATUS_SUCCESS);
    uint32_t handle = read32(at(OFF_HANDLE));
    poison(OFF_BUFFER, 32u);
    CHECK_EQ_U32(read_file(handle, event_handle(), at(OFF_IOSB), at(OFF_BUFFER), 16u, 0u),
                 STATUS_PENDING);
    const uint32_t close_args[1] = {handle};
    CHECK_EQ_U32(call_ordinal(ORD_NT_CLOSE, close_args, 1u), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_async_io_get_stats().pending, 1u);
    CHECK_EQ_U32(open_file(OPEN_ASYNC), STATUS_SUCCESS);
    CHECK(read32(at(OFF_HANDLE)) != handle);
    (void)kernel_clock_advance_to(UINT64_MAX / 2u);
    CHECK_EQ_U32(kernel_async_io_service(), 1u);
    CHECK_EQ_U32(read32(at(OFF_IOSB)), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(at(OFF_IOSB) + 4u), 16u);
    CHECK(buffer_matches(at(OFF_BUFFER), 0u, 16u));
    CHECK(event_signalled());
    teardown();
}

int main(void)
{
    for (unsigned asynchronous = 0u; asynchronous < 2u; asynchronous++) {
        completion_case(asynchronous != 0u, 0u, FILE_BYTES + 1u, 0u);
        completion_case(asynchronous != 0u, 16u, FILE_BYTES - 8u, 8u);
        completion_case(asynchronous != 0u, 16u, FILE_BYTES, 0u);
        completion_case(asynchronous != 0u, 16u, FILE_BYTES + 1u, 0u);
        for (unsigned kind = 0u; kind < 3u; kind++) {
            bad_event_case(asynchronous != 0u, kind);
        }
    }
    invalid_file_case(false);
    invalid_file_case(true);
    failed_delivery_and_reset(false);
    failed_delivery_and_reset(true);
    close_pending_file();
    printf("T1004/T1011: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
