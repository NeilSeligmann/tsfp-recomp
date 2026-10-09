/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T851: independent native checks for the Event HANDLE seam used by NtReadFile.
 * This exercises the shared object API, not a new kernel completion policy.
 */
#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_object.h"
#include "nt_status.h"

#include <stdio.h>
#include <string.h>

static int failures;
static unsigned checks;

#define CHECK_EQ(actual, expected)                                                        \
    do {                                                                                  \
        checks++;                                                                         \
        uint32_t actual_ = (uint32_t)(actual);                                            \
        uint32_t expected_ = (uint32_t)(expected);                                       \
        if (actual_ != expected_) {                                                       \
            printf("FAIL %s:%d %s = %#x, expected %#x\n", __FILE__, __LINE__, #actual, \
                   (unsigned)actual_, (unsigned)expected_);                               \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        checks++;                                                                         \
        if (!(condition)) {                                                               \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition);                   \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

int main(void)
{
    kernel_object_reset();
    guest_mem_reset();
    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = 4096u;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status = STATUS_SUCCESS;
    const kernel_guest_ptr output = guest_region_alloc(&request, &status);
    CHECK(output != 0u);
    CHECK_EQ(status, STATUS_SUCCESS);

    const uint32_t marker = 0xA5A55A5Au;
    CHECK(kernel_guest_write_u32(output, marker));
    CHECK_EQ(kernel_object_create_event(1u, 0u, output), STATUS_SUCCESS);
    uint32_t selected = 0u;
    CHECK(kernel_guest_read_u32(output, &selected));
    CHECK(selected != 0u && selected != output);
    CHECK(kernel_guest_write_u32(output + 4u, marker));
    CHECK_EQ(kernel_object_create_event(1u, 0u, output + 4u), STATUS_SUCCESS);
    uint32_t neighbor = 0u;
    CHECK(kernel_guest_read_u32(output + 4u, &neighbor));
    CHECK(neighbor != 0u && neighbor != selected);

    bool signalled = true;
    CHECK(kernel_object_event_signaled(selected, &signalled));
    CHECK(!signalled);
    CHECK_EQ(kernel_object_event_set(selected, NULL), STATUS_SUCCESS);
    CHECK(kernel_object_event_signaled(selected, &signalled));
    CHECK(signalled);
    CHECK(kernel_object_event_signaled(neighbor, &signalled));
    CHECK(!signalled);

    /* NtReadFile skips this call for a NULL Event; the object API rejects handle zero. */
    CHECK_EQ(kernel_object_event_set(0u, NULL), STATUS_INVALID_HANDLE);
    CHECK_EQ(kernel_object_event_set(0xDEADBEEFu, NULL), STATUS_INVALID_HANDLE);

    /* Closing/reusing the slot as another event cannot let the stale value signal it. */
    CHECK(kernel_object_release(selected));
    CHECK(kernel_guest_write_u32(output + 8u, marker));
    CHECK_EQ(kernel_object_create_event(1u, 0u, output + 8u), STATUS_SUCCESS);
    uint32_t replacement_event = 0u;
    CHECK(kernel_guest_read_u32(output + 8u, &replacement_event));
    CHECK(replacement_event != 0u && replacement_event != selected);
    CHECK_EQ(kernel_object_event_set(selected, NULL), STATUS_INVALID_HANDLE);
    CHECK(kernel_object_event_signaled(replacement_event, &signalled));
    CHECK(!signalled);

    /* A live, non-Event handle is a type mismatch, not an Event target. */
    const uint32_t file_handle = kernel_object_create(KERNEL_OBJECT_FILE, 0x851u);
    CHECK(file_handle != 0u && file_handle != replacement_event);
    CHECK_EQ(kernel_object_event_set(file_handle, NULL), STATUS_OBJECT_TYPE_MISMATCH);
    CHECK(kernel_object_event_signaled(neighbor, &signalled));
    CHECK(!signalled);

    kernel_object_reset();
    guest_mem_reset();
    if (failures != 0) {
        printf("T851 event handle review: %d/%u checks failed\n", failures, checks);
        return 1;
    }
    printf("T851 event handle review: %u checks passed\n", checks);
    return 0;
}
