/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Tests for the object group, and the first suite this module has ever had.
 *
 * WHY IT EXISTS NOW. `kernel_object.c` had no suite of its own: `NtClose` and the two
 * `Obf*` ordinals were exercised only incidentally, from `test_guest_thread.c` and the
 * file suites, as a side effect of testing something else. Ordinal 246
 * `ObReferenceObjectByHandle` is the first handler here whose FAILURE paths carry real
 * information -- it has three of them and they return three different statuses -- and an
 * incidental test cannot reach a failure path on purpose.
 *
 * WHAT THE TESTS ARE FOR. The risk in this module is not a wrong offset, it is a
 * PLAUSIBLE wrong status or a silently skipped write. Three specific confusions are each
 * pinned by their own assertion:
 *
 *   - A NULL out-parameter and an unissued handle are DIFFERENT BUGS with different
 *     fixes, so they must not collapse into one status. The test passes both at once to
 *     prove which is checked first.
 *   - A refused call must leave the caller's slot ALONE. Writing a zero beside a failure
 *     status is indistinguishable from succeeding with a NULL object, so the slot is
 *     poisoned first and the poison must survive.
 *   - THREAD references return their bound mapped body and round-trip through
 *     ObfDereferenceObject. Unbound THREAD bodies refuse without writing an answer;
 *     other object kinds retain their legacy handle surrogate.
 */

#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_event_handle.h"
#include "kernel_hle.h"
#include "kernel_object.h"
#include "nt_status.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */

static int failures;
static int checks;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        checks++;                                                                        \
        if (!(cond)) {                                                                   \
            failures++;                                                                  \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                     \
            fflush(stdout);                                                              \
        }                                                                                \
    } while (0)

#define CHECK_EQ_U32(actual, expected)                                                  \
    do {                                                                                \
        checks++;                                                                        \
        uint32_t check_a = (uint32_t)(actual);                                            \
        uint32_t check_e = (uint32_t)(expected);                                          \
        if (check_a != check_e) {                                                         \
            failures++;                                                                   \
            printf("  FAIL %s:%d  %s == %s (got %#x, want %#x)\n", __FILE__, __LINE__,    \
                   #actual, #expected, check_a, check_e);                                 \
            fflush(stdout);                                                               \
        }                                                                                 \
    } while (0)

static char captured[8192];
static size_t captured_len;

static int capture_printer(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int written = vsnprintf(captured + captured_len, sizeof(captured) - captured_len,
                            format, args);
    va_end(args);
    if (written > 0) {
        captured_len += (size_t)written;
        if (captured_len >= sizeof(captured)) {
            captured_len = sizeof(captured) - 1;
        }
    }
    return written;
}

static void reset_capture(void)
{
    captured[0] = '\0';
    captured_len = 0;
}

static bool captured_contains(const char *needle)
{
    return strstr(captured, needle) != NULL;
}

#define SCRATCH_BYTES 0x4000u
/* Frames live below this, the out-parameter slots above, so a frame that overran would
 * corrupt a slot the test then checks rather than going unnoticed. */
#define SCRATCH_OUT 0x200u
#define OUT_SLOT (SCRATCH_OUT + 0x40u)
#define TYPE_SLOT (SCRATCH_OUT + 0x80u)

static kernel_guest_ptr scratch;

static void setup(void)
{
    kernel_hle_init();
    guest_mem_reset();
    kernel_object_reset();
    CHECK_EQ_U32(kernel_object_register(), 7u);
    kernel_hle_set_log(capture_printer);
    reset_capture();

    guest_region_request request = {
        .bytes = SCRATCH_BYTES,
        .alignment = 0u,
        .lowest_physical = 0u,
        .highest_physical = 0u,
        .protect = PAGE_READWRITE,
        .state = MEM_COMMIT,
        .contiguous = true,
        .fixed_base = 0u,
    };
    nt_status status = STATUS_SUCCESS;
    scratch = guest_region_alloc(&request, &status);
    if (scratch == 0u) {
        printf("  FATAL: could not allocate scratch (status %#x)\n", status);
        exit(EXIT_FAILURE);
    }
}

static void teardown(void)
{
    kernel_hle_set_log(NULL);
    guest_mem_reset();
    kernel_object_reset();
    scratch = 0u;
}

static kernel_guest_ptr at(uint32_t offset)
{
    return (kernel_guest_ptr)(scratch + offset);
}

/* 0xA5A5A5A5 is not a handle, not zero, and not an address this test uses, so a slot
 * still holding it provably was not written. */
#define POISON 0xA5A5A5A5u

static void poison_slot(uint32_t offset)
{
    if (!kernel_guest_write_u32(at(offset), POISON)) {
        printf("  FATAL: cannot poison the out slot\n");
        exit(EXIT_FAILURE);
    }
}

static uint32_t read_slot(uint32_t offset)
{
    uint32_t value = 0u;
    if (!kernel_guest_read_u32(at(offset), &value)) {
        printf("  FATAL: cannot read the out slot\n");
        exit(EXIT_FAILURE);
    }
    return value;
}

static uint32_t reference_by_handle(uint32_t handle, uint32_t object_type, uint32_t out)
{
    const uint32_t args[3] = {handle, object_type, out};
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    if (!kernel_frame_build(&frame, scratch, SCRATCH_OUT, args, 3u)) {
        printf("  FATAL: could not build a call frame\n");
        exit(EXIT_FAILURE);
    }
    return kernel_hle_call(246u, &frame);
}

static uint32_t references_of(uint32_t handle)
{
    const kernel_object_entry *entry = kernel_object_find(handle);
    return entry ? entry->references : 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------------- */

static void test_registration_binds_all_four_ordinals(void)
{
    setup();
    static const unsigned ordinals[] = {187u, 246u, 250u, 251u};
    CHECK_EQ_U32(kernel_hle_implemented_count(ordinals, 4u), 4u);
    teardown();
}

static void test_a_live_handle_is_referenced_and_returned(void)
{
    setup();
    const uint32_t handle = kernel_object_create(KERNEL_OBJECT_THREAD, 0x1234u);
    CHECK(handle != 0u);
    CHECK_EQ_U32(references_of(handle), 1u);
    poison_slot(OUT_SLOT);
    CHECK_EQ_U32(reference_by_handle(handle, 0u, (uint32_t)at(OUT_SLOT)), STATUS_NOT_IMPLEMENTED);
    CHECK_EQ_U32(read_slot(OUT_SLOT), POISON);
    CHECK_EQ_U32(references_of(handle), 1u);
    CHECK(kernel_object_bind_thread_body(handle, 0x1234u, scratch + 0x1000u));
    reset_capture();

    poison_slot(OUT_SLOT);
    CHECK_EQ_U32(reference_by_handle(handle, 0u, (uint32_t)at(OUT_SLOT)), STATUS_SUCCESS);
    CHECK_EQ_U32(read_slot(OUT_SLOT), scratch + 0x1000u);
    CHECK_EQ_U32(references_of(handle), 2u);
    CHECK_EQ_U32(kernel_object_reference_by_handle_count(), 1u);
    /* No ObjectType was passed, so nothing was skipped and nothing is logged. */
    CHECK_EQ_U32(kernel_object_unchecked_type_count(), 0u);
    CHECK_EQ_U32(captured_len, 0u);
    teardown();
}

/* A mapped THREAD pointer round-trips through its separate retained body reference.
 *
 * Reference by handle, then hand the value straight back to ObfDereferenceObject in ECX
 * exactly as `_SetThreadPriority@8` does at 0x0037FC5C, and the count must return to what
 * it was. The returned body remains distinct from its issued handle. */
static void test_the_returned_pointer_round_trips_through_dereference(void)
{
    setup();
    const uint32_t handle = kernel_object_create(KERNEL_OBJECT_THREAD, 0u);
    CHECK(kernel_object_bind_thread_body(handle, 0u, scratch + 0x1000u));
    poison_slot(OUT_SLOT);
    CHECK_EQ_U32(reference_by_handle(handle, 0u, (uint32_t)at(OUT_SLOT)), STATUS_SUCCESS);
    CHECK_EQ_U32(references_of(handle), 2u);

    const uint32_t object = read_slot(OUT_SLOT);
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    const uint32_t none[1] = {0u};
    if (!kernel_frame_build(&frame, scratch, SCRATCH_OUT, none, 0u)) {
        printf("  FATAL: could not build a fastcall frame\n");
        exit(EXIT_FAILURE);
    }
    /* ECX, not the stack. `Obf*` is __fastcall; a handler reading kernel_frame_arg(0)
     * would get the return address. */
    kernel_frame_set_registers(&frame, object, 0u);
    (void)kernel_hle_call(250u, &frame);
    CHECK_EQ_U32(references_of(handle), 1u);
    teardown();
}

/* A HANDLE WE NEVER ISSUED: the out-parameter must be left exactly as the caller left it.
 *
 * Writing a zero here would be worse than writing nothing, because the guest checks the
 * status and a zeroed slot beside a failure is indistinguishable from a kernel that
 * succeeded and returned NULL. */
static void test_an_unissued_handle_is_refused_and_writes_nothing(void)
{
    setup();
    poison_slot(OUT_SLOT);
    CHECK_EQ_U32(reference_by_handle(0x00E1DEADu, 0u, (uint32_t)at(OUT_SLOT)),
                 STATUS_INVALID_HANDLE);
    CHECK_EQ_U32(read_slot(OUT_SLOT), POISON);
    CHECK_EQ_U32(kernel_object_reference_by_handle_count(), 0u);
    CHECK(captured_contains("never issued by us"));
    teardown();
}

/* A NULL out-parameter is a DIFFERENT failure from an unissued handle, and the order the
 * two are checked in is asserted rather than assumed.
 *
 * Both faults are present at once. STATUS_INVALID_PARAMETER means the out-parameter was
 * checked first, which is the right way round: the caller's own bug is reported ahead of
 * a handle mismatch that may well be a consequence of it. */
static void test_a_null_out_parameter_is_reported_before_the_handle_is_judged(void)
{
    setup();
    CHECK_EQ_U32(reference_by_handle(0x00E1DEADu, 0u, 0u), STATUS_INVALID_PARAMETER);
    CHECK(captured_contains("NULL ReturnedObject"));
    CHECK(!captured_contains("never issued by us"));

    /* And with a perfectly good handle it is still a parameter error. */
    reset_capture();
    const uint32_t handle = kernel_object_create(KERNEL_OBJECT_FILE, 0u);
    CHECK_EQ_U32(reference_by_handle(handle, 0u, 0u), STATUS_INVALID_PARAMETER);
    /* No reference was taken, so nothing leaked. */
    CHECK_EQ_U32(references_of(handle), 1u);
    CHECK_EQ_U32(kernel_object_reference_by_handle_count(), 0u);
    teardown();
}

/* A NON-NULL ObjectType SUCCEEDS, IS COUNTED, AND IS NAMED.
 *
 * It is not enforced -- there is no OBJECT_TYPE registry -- and the three things that
 * must all be true are that the call still works, that the omission is visible in a
 * counter, and that the log says which type went unchecked. Returning a type mismatch we
 * did not detect would be inventing a failure. */
static void test_an_object_type_is_recorded_not_enforced(void)
{
    setup();
    const uint32_t handle = kernel_object_create(KERNEL_OBJECT_FILE, 0u);
    poison_slot(OUT_SLOT);
    /* A plausible OBJECT_TYPE address: the three this image passes are the data exports
     * for ordinals 16, 71 and 259, and their values are addresses like this one. */
    CHECK_EQ_U32(reference_by_handle(handle, (uint32_t)at(TYPE_SLOT),
                                    (uint32_t)at(OUT_SLOT)),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(read_slot(OUT_SLOT), handle);
    CHECK_EQ_U32(kernel_object_unchecked_type_count(), 1u);
    CHECK(captured_contains("NOT checked"));
    teardown();
}

/* ARGUMENT ORDER: the ObjectType is argument 1 and the out-parameter is argument 2.
 *
 * Pinned at the real call site by `_SetThreadPriority@8` at 0x0037FC2B, which pushes
 * PsThreadObjectType into the MIDDLE slot. Here it is pinned behaviourally: both slots
 * are poisoned, and only the one at argument 2 may change. A handler that had them the
 * other way round would write the handle over the ObjectType and leave the real
 * out-parameter poisoned, which is exactly what these two assertions separate. */
static void test_the_object_type_slot_is_never_written(void)
{
    setup();
    const uint32_t handle = kernel_object_create(KERNEL_OBJECT_FILE, 0u);
    poison_slot(OUT_SLOT);
    poison_slot(TYPE_SLOT);
    CHECK_EQ_U32(reference_by_handle(handle, (uint32_t)at(TYPE_SLOT),
                                    (uint32_t)at(OUT_SLOT)),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(read_slot(OUT_SLOT), handle);
    CHECK_EQ_U32(read_slot(TYPE_SLOT), POISON);
    teardown();
}

static void test_a_frame_too_short_for_three_arguments_is_reported(void)
{
    setup();
    const uint32_t handle = kernel_object_create(KERNEL_OBJECT_THREAD, 0u);
    const uint32_t args[2] = {handle, 0u};
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    if (!kernel_frame_build(&frame, scratch, 3u * 4u, args, 2u)) {
        printf("  FATAL: could not build a short frame\n");
        exit(EXIT_FAILURE);
    }
    CHECK_EQ_U32(kernel_hle_call(246u, &frame), STATUS_INVALID_PARAMETER);
    CHECK(captured_contains("three arguments"));
    /* Nothing was referenced on the way to failing. */
    CHECK_EQ_U32(references_of(handle), 1u);
    teardown();
}

/* THE REFERENCE MUST OUTLIVE A CLOSE, which is the point of counting at all.
 *
 * NtClose on a handle with two references decrements rather than destroying, so the
 * object the guest still holds a pointer to stays findable. A handler that ignored the
 * count would make the second close report a handle we never issued -- our own
 * diagnostic accusing the guest of our bug, which kernel_object.c warns about. */
static void test_a_close_while_referenced_decrements_rather_than_destroys(void)
{
    setup();
    const uint32_t handle = kernel_object_create(KERNEL_OBJECT_FILE, 0u);
    poison_slot(OUT_SLOT);
    CHECK_EQ_U32(reference_by_handle(handle, 0u, (uint32_t)at(OUT_SLOT)), STATUS_SUCCESS);
    CHECK_EQ_U32(references_of(handle), 2u);

    const uint32_t close_args[1] = {handle};
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    if (!kernel_frame_build(&frame, scratch, SCRATCH_OUT, close_args, 1u)) {
        printf("  FATAL: could not build a close frame\n");
        exit(EXIT_FAILURE);
    }
    CHECK_EQ_U32(kernel_hle_call(187u, &frame), STATUS_SUCCESS);
    CHECK(kernel_object_find(handle) != NULL);
    CHECK_EQ_U32(references_of(handle), 1u);
    CHECK_EQ_U32(kernel_object_bad_close_count(), 0u);

    /* The second close is the one that destroys it. */
    CHECK_EQ_U32(kernel_hle_call(187u, &frame), STATUS_SUCCESS);
    CHECK(kernel_object_find(handle) == NULL);
    CHECK_EQ_U32(kernel_object_bad_close_count(), 0u);
    teardown();
}

static void test_reset_clears_the_new_counters(void)
{
    setup();
    const uint32_t handle = kernel_object_create(KERNEL_OBJECT_FILE, 0u);
    poison_slot(OUT_SLOT);
    CHECK_EQ_U32(reference_by_handle(handle, (uint32_t)at(TYPE_SLOT),
                                    (uint32_t)at(OUT_SLOT)),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_object_reference_by_handle_count(), 1u);
    CHECK_EQ_U32(kernel_object_unchecked_type_count(), 1u);
    kernel_object_reset();
    CHECK_EQ_U32(kernel_object_reference_by_handle_count(), 0u);
    CHECK_EQ_U32(kernel_object_unchecked_type_count(), 0u);
    CHECK_EQ_U32(kernel_object_live_count(), 0u);
    teardown();
}

/* EVENT handles record creation facts but expose no mapped body. */
static void test_event_publication_and_body_refusal(void)
{
    setup();
    poison_slot(OUT_SLOT);
    CHECK_EQ_U32(kernel_object_create_event(2u, 0u, (uint32_t)at(OUT_SLOT)), STATUS_NOT_IMPLEMENTED);
    CHECK_EQ_U32(kernel_object_create_event(0u, 1u, (uint32_t)at(OUT_SLOT)), STATUS_NOT_IMPLEMENTED);
    CHECK_EQ_U32(kernel_object_create_event(1u, 1u, (uint32_t)at(OUT_SLOT)), STATUS_NOT_IMPLEMENTED);
    CHECK_EQ_U32(read_slot(OUT_SLOT), POISON);
    CHECK_EQ_U32(kernel_object_live_count(), 0u);
    CHECK_EQ_U32(kernel_object_create_event(1u, 0x100u, (uint32_t)at(OUT_SLOT)), STATUS_SUCCESS);
    const uint32_t handle = read_slot(OUT_SLOT);
    kernel_object_entry entry;
    CHECK(kernel_object_get_copy(handle, &entry));
    CHECK_EQ_U32(entry.kind, KERNEL_OBJECT_EVENT);
    CHECK_EQ_U32(entry.event_type, 1u);
    CHECK_EQ_U32(entry.event_initial_state, 0u);
    CHECK_EQ_U32(entry.references, 1u);
    /* T270: Type 0 (NotificationEvent) is published as its own object, initially clear. */
    CHECK_EQ_U32(kernel_object_create_event(0u, 0u, (uint32_t)at(OUT_SLOT)), STATUS_SUCCESS);
    const uint32_t notification = read_slot(OUT_SLOT);
    CHECK(notification != handle);
    kernel_object_entry notification_entry;
    CHECK(kernel_object_get_copy(notification, &notification_entry));
    CHECK_EQ_U32(notification_entry.kind, KERNEL_OBJECT_EVENT);
    CHECK_EQ_U32(notification_entry.event_type, 0u);
    CHECK_EQ_U32(notification_entry.event_initial_state, 0u);
    CHECK_EQ_U32(notification_entry.references, 1u);
    CHECK_EQ_U32(kernel_object_live_count(), 2u);
    CHECK(kernel_object_release(notification));
    CHECK_EQ_U32(kernel_object_live_count(), 1u);
    poison_slot(OUT_SLOT);
    CHECK_EQ_U32(reference_by_handle(handle, 0u, (uint32_t)at(OUT_SLOT)), STATUS_NOT_IMPLEMENTED);
    CHECK_EQ_U32(reference_by_handle(handle, 0u, 0u), STATUS_NOT_IMPLEMENTED);
    CHECK_EQ_U32(read_slot(OUT_SLOT), POISON);
    CHECK_EQ_U32(references_of(handle), 1u);
    kernel_call_frame frame = {0};
    kernel_frame_set_registers(&frame, handle, 0u);
    (void)kernel_hle_call(251u, &frame);
    CHECK_EQ_U32(references_of(handle), 1u);
    (void)kernel_hle_call(250u, &frame);
    CHECK_EQ_U32(references_of(handle), 1u);
    CHECK(kernel_object_release(handle));
    CHECK(!kernel_object_get_copy(handle, &entry));
    CHECK_EQ_U32(kernel_object_live_count(), 0u);
    teardown();
}

/* ===================== NtCreateMutant (ordinal 192) ===================== */

/*
 * The one shape the image uses: the global constructor at 0x003D21B0 calls the title's
 * CreateMutexA wrapper with (0, 0, 0), which reaches NtCreateMutant with no
 * OBJECT_ATTRIBUTES and InitialOwner FALSE. The handle goes into the slot that held the
 * wrapper's name argument. Statuses are hex literals, never read through a macro.
 */
#define ORD_NT_CREATE_MUTANT 192u
#define MUTANT_OA_OFFSET (SCRATCH_OUT + 0x100u)
#define MUTANT_NAME_STRING_OFFSET (SCRATCH_OUT + 0x120u)

/* The wrapper's own OBJECT_ATTRIBUTES for a named mutex: root 0xFFFFFFFC, a name, and
 * attributes 0x80 (0x0037FFC8 calls the builder at 0x00381B7C which stores exactly these). */
static uint32_t build_mutant_oa(uint32_t name_pointer)
{
    if (!kernel_guest_write_u32(at(MUTANT_OA_OFFSET) + 0u, 0xFFFFFFFCu) ||
        !kernel_guest_write_u32(at(MUTANT_OA_OFFSET) + 4u, name_pointer) ||
        !kernel_guest_write_u32(at(MUTANT_OA_OFFSET) + 8u, 0x80u)) {
        printf("  FATAL: cannot build the OBJECT_ATTRIBUTES\n");
        exit(EXIT_FAILURE);
    }
    return (uint32_t)at(MUTANT_OA_OFFSET);
}

static uint32_t create_mutant(uint32_t handle_out, uint32_t object_attributes,
                              uint32_t initial_owner)
{
    const uint32_t args[3] = {handle_out, object_attributes, initial_owner};
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    if (!kernel_frame_build(&frame, scratch, SCRATCH_OUT, args, 3u)) {
        printf("  FATAL: could not build a call frame\n");
        exit(EXIT_FAILURE);
    }
    return kernel_hle_call(ORD_NT_CREATE_MUTANT, &frame);
}

/* MUTATION: drop the binding from kernel_object_register or change the ordinal. */
static void test_the_mutant_ordinal_is_implemented(void)
{
    setup();
    const kernel_entry *entry = kernel_hle_entry(ORD_NT_CREATE_MUTANT);
    CHECK(entry != NULL);
    if (entry) {
        CHECK_EQ_U32(entry->state, KERNEL_ENTRY_IMPLEMENTED);
        CHECK(strcmp(entry->name, "NtCreateMutant") == 0);
    }
    teardown();
}

/*
 * THE GUEST'S CALL: no OBJECT_ATTRIBUTES, InitialOwner 0. A real handle of the MUTANT
 * kind lands in the slot, and the first handle after a reset is 0xE10000, the value the
 * header documents for the whole table.
 *
 * MUTATION: wrong kind, no handle write, or a refusal of the unnamed shape.
 */
static void test_an_unnamed_unowned_mutant_gets_a_handle(void)
{
    setup();
    poison_slot(OUT_SLOT);
    CHECK_EQ_U32(create_mutant((uint32_t)at(OUT_SLOT), 0u, 0u), 0x00000000u);
    const uint32_t handle = read_slot(OUT_SLOT);
    CHECK_EQ_U32(handle, 0x00E10000u);
    const kernel_object_entry *entry = kernel_object_find(handle);
    CHECK(entry != NULL);
    if (entry) {
        CHECK_EQ_U32(entry->kind, KERNEL_OBJECT_MUTANT);
    }
    CHECK_EQ_U32(kernel_object_live_count(), 1u);
    CHECK(captured_contains("NOT MODELLED"));
    teardown();
}

/* An OBJECT_ATTRIBUTES with a NULL name is still anonymous. MUTATION: refuse it. */
static void test_attributes_with_no_name_are_still_anonymous(void)
{
    setup();
    poison_slot(OUT_SLOT);
    CHECK_EQ_U32(create_mutant((uint32_t)at(OUT_SLOT), build_mutant_oa(0u), 0u),
                 0x00000000u);
    CHECK_EQ_U32(kernel_object_live_count(), 1u);
    teardown();
}

/*
 * A NAMED mutant is refused, loudly, with nothing issued and the slot untouched. It must
 * NOT be answered with an anonymous one: the title would then believe two creators of one
 * name shared an object.
 *
 * MUTATION: ignore the name and create anyway.
 */
static void test_a_named_mutant_is_refused_and_creates_nothing(void)
{
    setup();
    poison_slot(OUT_SLOT);
    CHECK_EQ_U32(create_mutant((uint32_t)at(OUT_SLOT),
                               build_mutant_oa((uint32_t)at(MUTANT_NAME_STRING_OFFSET)),
                               0u),
                 0xC0000002u);
    CHECK_EQ_U32(read_slot(OUT_SLOT), POISON);
    CHECK_EQ_U32(kernel_object_live_count(), 0u);
    CHECK(captured_contains("NAMED"));
    teardown();
}

/*
 * The refusal must say WHAT the guest asked for. The wrapper at 0x00381B7C builds root
 * 0xFFFFFFFC and attributes 0x80, values `guest_structs.h` used to claim never occurred,
 * so the log is where a later reader first sees them. Literals, not macros.
 *
 * MUTATION: drop root and attributes from the refusal message and this fails.
 */
static void test_a_named_mutant_refusal_reports_its_root_and_attributes(void)
{
    setup();
    poison_slot(OUT_SLOT);
    CHECK_EQ_U32(create_mutant((uint32_t)at(OUT_SLOT),
                               build_mutant_oa((uint32_t)at(MUTANT_NAME_STRING_OFFSET)),
                               0u),
                 0xC0000002u);
    CHECK(captured_contains("root 0xfffffffc"));
    CHECK(captured_contains("attributes 0x80"));
    teardown();
}

/*
 * An UNNAMED mutant whose OBJECT_ATTRIBUTES still carries a root and flags is created, and
 * the ignored values are reported instead of dropped.
 *
 * MUTATION: remove the report, or make it fire for a NULL OBJECT_ATTRIBUTES (the guest's
 * real call), and the first or second half fails.
 */
static void test_an_unnamed_mutants_ignored_root_and_attributes_are_reported(void)
{
    setup();
    poison_slot(OUT_SLOT);
    CHECK_EQ_U32(create_mutant((uint32_t)at(OUT_SLOT), build_mutant_oa(0u), 0u), 0x00000000u);
    CHECK(captured_contains("ignores root 0xfffffffc and attributes 0x80"));
    /* Either field alone is enough to be reported: root zero, flags only, then the reverse. */
    captured[0] = '\0';
    captured_len = 0u;
    (void)build_mutant_oa(0u);
    CHECK(kernel_guest_write_u32(at(MUTANT_OA_OFFSET) + 0u, 0u));
    CHECK_EQ_U32(create_mutant((uint32_t)at(OUT_SLOT), (uint32_t)at(MUTANT_OA_OFFSET), 0u),
                 0x00000000u);
    CHECK(captured_contains("ignores root 0 and attributes 0x80"));
    captured[0] = '\0';
    captured_len = 0u;
    CHECK(kernel_guest_write_u32(at(MUTANT_OA_OFFSET) + 0u, 0xFFFFFFFCu));
    CHECK(kernel_guest_write_u32(at(MUTANT_OA_OFFSET) + 8u, 0u));
    CHECK_EQ_U32(create_mutant((uint32_t)at(OUT_SLOT), (uint32_t)at(MUTANT_OA_OFFSET), 0u),
                 0x00000000u);
    CHECK(captured_contains("ignores root 0xfffffffc and attributes 0"));
    /* The guest's real call passes no OBJECT_ATTRIBUTES at all, which is silent. */
    captured[0] = '\0';
    captured_len = 0u;
    CHECK_EQ_U32(create_mutant((uint32_t)at(OUT_SLOT), 0u, 0u), 0x00000000u);
    CHECK(!captured_contains("ignores root"));
    teardown();
}

/* InitialOwner needs the calling thread's identity, which this module has not got.
 * MUTATION: ignore the flag. */
static void test_an_initially_owned_mutant_is_refused(void)
{
    setup();
    poison_slot(OUT_SLOT);
    CHECK_EQ_U32(create_mutant((uint32_t)at(OUT_SLOT), 0u, 1u), 0xC0000002u);
    CHECK_EQ_U32(read_slot(OUT_SLOT), POISON);
    CHECK_EQ_U32(kernel_object_live_count(), 0u);
    CHECK(captured_contains("InitialOwner"));
    teardown();
}

/*
 * BOOLEAN is ONE BYTE. The guest pushes a whole dword, so garbage above bit 7 is not part
 * of the argument: 0x100 is FALSE and 0xFFFFFF01 is TRUE.
 *
 * MUTATION: test the whole dword, and the first call is refused.
 */
static void test_initial_owner_is_the_low_byte_only(void)
{
    setup();
    poison_slot(OUT_SLOT);
    CHECK_EQ_U32(create_mutant((uint32_t)at(OUT_SLOT), 0u, 0xFFFFFF00u), 0x00000000u);
    CHECK_EQ_U32(kernel_object_live_count(), 1u);
    CHECK_EQ_U32(create_mutant((uint32_t)at(OUT_SLOT), 0u, 0xFFFFFF01u), 0xC0000002u);
    CHECK_EQ_U32(kernel_object_live_count(), 1u);
    teardown();
}

/* MUTATION: dereference the out-parameter without the NULL check, or accept it. */
static void test_a_missing_handle_slot_is_refused(void)
{
    setup();
    CHECK_EQ_U32(create_mutant(0u, 0u, 0u), 0xC000000Du);
    CHECK_EQ_U32(kernel_object_live_count(), 0u);
    /* The right diagnosis, not just the right status: a failed write of a handle that was
     * issued also returns 0xC000000D, so only the log tells the two bugs apart. */
    CHECK(captured_contains("no handle out-parameter"));
    teardown();
}

/* An unreadable OBJECT_ATTRIBUTES is a refusal, not a guess that the mutant is anonymous.
 * Three bad shapes, each a different way to be bad. 0xFFFFFFFC: the name field at +4 wraps to
 * address 0. 0xFFFFFFFE: it wraps to address 2, which is NOT null, so before the accessors
 * learned to reject it this faulted the host (MEASURED: the original suite crashed on it).
 * 0x1000: in range and never mapped, which only the mapping probe can refuse. */
static void test_an_unreadable_attributes_block_is_refused(void)
{
    setup();
    static const uint32_t bad_blocks[] = {0xFFFFFFFCu, 0xFFFFFFFEu, 0x00001000u};
    for (unsigned i = 0u; i < 3u; i++) {
        poison_slot(OUT_SLOT);
        CHECK_EQ_U32(create_mutant((uint32_t)at(OUT_SLOT), bad_blocks[i], 0u), 0xC000000Du);
        CHECK_EQ_U32(read_slot(OUT_SLOT), POISON);
        CHECK_EQ_U32(kernel_object_live_count(), 0u);
    }
    teardown();
}

/*
 * A handle slot the host cannot write is reported, never answered with success: the slot is
 * the guest's only way to learn the handle. The handle that was issued is RELEASED, because
 * nothing the guest holds names it, so nothing would ever close it. (The earlier version of
 * this test said the handle stays open "matching NT". That is not established here, and
 * leaking one of 256 live slots per bad call is a worse default.)
 *
 * Three bad slots: 0xFFFFFFFE runs a dword past the 4 GB limit, 0x1000 is in range and
 * unmapped (the probe), 0xFFFFFFFC is the last dword of the space and unmapped.
 *
 * MUTATION: disable the failure branch of the handle write, or skip the release.
 */
static void test_an_unwritable_handle_slot_is_reported(void)
{
    setup();
    static const uint32_t bad_slots[] = {0xFFFFFFFEu, 0x00001000u, 0xFFFFFFFCu};
    for (unsigned i = 0u; i < 3u; i++) {
        reset_capture();
        CHECK_EQ_U32(create_mutant(bad_slots[i], 0u, 0u), 0xC000000Du);
        CHECK(captured_contains("could not write the handle"));
        CHECK_EQ_U32(kernel_object_live_count(), 0u);
    }
    teardown();
}

/* ARITY: three stack arguments. MUTATION: read fewer or skip the frame check. */
static void test_a_frame_too_short_for_the_mutant_arguments_is_refused(void)
{
    setup();
    const uint32_t args[2] = {(uint32_t)at(OUT_SLOT), 0u};
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    if (!kernel_frame_build(&frame, scratch, 3u * 4u, args, 2u)) {
        printf("  FATAL: could not build a short frame\n");
        exit(EXIT_FAILURE);
    }
    CHECK_EQ_U32(kernel_hle_call(ORD_NT_CREATE_MUTANT, &frame), 0xC000000Du);
    CHECK_EQ_U32(kernel_object_live_count(), 0u);
    teardown();
}

/* A mutant handle closes like any other, through the shared NtClose. */
static void test_a_mutant_handle_closes_through_nt_close(void)
{
    setup();
    poison_slot(OUT_SLOT);
    CHECK_EQ_U32(create_mutant((uint32_t)at(OUT_SLOT), 0u, 0u), 0x00000000u);
    const uint32_t close_args[1] = {read_slot(OUT_SLOT)};
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    if (!kernel_frame_build(&frame, scratch, SCRATCH_OUT, close_args, 1u)) {
        printf("  FATAL: could not build a close frame\n");
        exit(EXIT_FAILURE);
    }
    CHECK_EQ_U32(kernel_hle_call(187u, &frame), 0x00000000u);
    CHECK_EQ_U32(kernel_object_live_count(), 0u);
    teardown();
}

/* ============ NtCreateEvent (ordinal 189): the named shape (T142) ============ */

/*
 * The event side of the same dead path. CreateEventA 0x0037FF30 has exactly one caller
 * in the image (0x000291FD, four zero arguments), so the helper-built OBJECT_ATTRIBUTES
 * {root 0xFFFFFFFC, name, attributes 0x80} can never reach ordinal 189 from in-image
 * code. These tests pin the refusal contract for that shape anyway: the refusal is what
 * protects the measured boot from tampered or miscomputed state, and from a future
 * edit quietly "implementing" a namespace nothing measured. Statuses and the builder's
 * values are hex literals, never macros.
 */
#define ORD_NT_CREATE_EVENT 189u
#define EVENT_OA_OFFSET (SCRATCH_OUT + 0x140u)
#define EVENT_NAME_STRING_OFFSET (SCRATCH_OUT + 0x160u)

static uint32_t build_event_oa(uint32_t name_pointer)
{
    if (!kernel_guest_write_u32(at(EVENT_OA_OFFSET) + 0u, 0xFFFFFFFCu) ||
        !kernel_guest_write_u32(at(EVENT_OA_OFFSET) + 4u, name_pointer) ||
        !kernel_guest_write_u32(at(EVENT_OA_OFFSET) + 8u, 0x80u)) {
        printf("  FATAL: cannot build the event OBJECT_ATTRIBUTES\n");
        exit(EXIT_FAILURE);
    }
    return (uint32_t)at(EVENT_OA_OFFSET);
}

static uint32_t create_event_189(uint32_t handle_out, uint32_t object_attributes,
                                 uint32_t event_type, uint32_t initial_state)
{
    const uint32_t args[4] = {handle_out, object_attributes, event_type, initial_state};
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    if (!kernel_frame_build(&frame, scratch, SCRATCH_OUT, args, 4u)) {
        printf("  FATAL: could not build a call frame\n");
        exit(EXIT_FAILURE);
    }
    return kernel_hle_call(ORD_NT_CREATE_EVENT, &frame);
}

/* The helper's exact shape is refused, nothing is issued, and the slot is untouched.
 * MUTATION: drop the OBJECT_ATTRIBUTES gate in create_event and the event is created,
 * so the status, the slot and the live count all fail. */
static void test_a_named_event_is_refused_and_creates_nothing(void)
{
    setup();
    CHECK_EQ_U32(kernel_event_handle_register(), 2u);
    poison_slot(OUT_SLOT);
    CHECK_EQ_U32(create_event_189((uint32_t)at(OUT_SLOT),
                                  build_event_oa((uint32_t)at(EVENT_NAME_STRING_OFFSET)),
                                  1u, 0u),
                 0xC0000002u);
    CHECK_EQ_U32(read_slot(OUT_SLOT), POISON);
    CHECK_EQ_U32(kernel_object_live_count(), 0u);
    CHECK(captured_contains("NAMED"));
    teardown();
}

/* The refusal names what the guest asked for, like the mutant's.
 * MUTATION: drop root or attributes from the report. */
static void test_a_named_event_refusal_reports_its_root_and_attributes(void)
{
    setup();
    CHECK_EQ_U32(kernel_event_handle_register(), 2u);
    CHECK_EQ_U32(create_event_189((uint32_t)at(OUT_SLOT),
                                  build_event_oa((uint32_t)at(EVENT_NAME_STRING_OFFSET)),
                                  1u, 0u),
                 0xC0000002u);
    CHECK(captured_contains("root 0xfffffffc"));
    CHECK(captured_contains("attributes 0x80"));
    teardown();
}

/* UNLIKE the mutant, an attributes block with a NULL name is refused too: the only
 * builder that can feed this ordinal emits the named shape or no block at all, so an
 * unnamed-with-attributes block is outside the measured contract either way.
 * MUTATION: fold the event gate into the mutant's tolerate-and-report behaviour. */
static void test_an_event_attributes_block_is_refused_even_unnamed(void)
{
    setup();
    CHECK_EQ_U32(kernel_event_handle_register(), 2u);
    poison_slot(OUT_SLOT);
    CHECK_EQ_U32(create_event_189((uint32_t)at(OUT_SLOT), build_event_oa(0u), 1u, 0u),
                 0xC0000002u);
    CHECK_EQ_U32(read_slot(OUT_SLOT), POISON);
    CHECK_EQ_U32(kernel_object_live_count(), 0u);
    teardown();
}

/* An unreadable attributes block is a parameter error, not a guess, mirroring the
 * mutant's three bad shapes. MUTATION: skip the read check and refuse as NAMED. */
static void test_an_unreadable_event_attributes_block_is_refused(void)
{
    setup();
    CHECK_EQ_U32(kernel_event_handle_register(), 2u);
    static const uint32_t bad_blocks[] = {0xFFFFFFFCu, 0xFFFFFFFEu, 0x00001000u};
    for (unsigned i = 0u; i < 3u; i++) {
        poison_slot(OUT_SLOT);
        CHECK_EQ_U32(create_event_189((uint32_t)at(OUT_SLOT), bad_blocks[i], 1u, 0u),
                     0xC000000Du);
        CHECK_EQ_U32(read_slot(OUT_SLOT), POISON);
        CHECK_EQ_U32(kernel_object_live_count(), 0u);
    }
    teardown();
}

/* The measured call still works beside the new gate, its handle closes through the
 * shared NtClose, and the next create takes the next FRESH slot (freed slots queue
 * behind unissued ones), so the named refusal costs the real path nothing. Handle
 * values are the layout literals from kernel_object.c, derived by hand, not macros.
 * MUTATION: refuse the NULL-attributes shape, or skip the release in NtClose. */
static void test_the_measured_unnamed_event_still_creates_and_recycles(void)
{
    setup();
    CHECK_EQ_U32(kernel_event_handle_register(), 2u);
    poison_slot(OUT_SLOT);
    CHECK_EQ_U32(create_event_189((uint32_t)at(OUT_SLOT), 0u, 1u, 0u), 0x00000000u);
    const uint32_t first = read_slot(OUT_SLOT);
    CHECK_EQ_U32(first, 0x00E10000u);
    CHECK_EQ_U32(kernel_object_live_count(), 1u);
    const uint32_t close_args[1] = {first};
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    if (!kernel_frame_build(&frame, scratch, SCRATCH_OUT, close_args, 1u)) {
        printf("  FATAL: could not build a close frame\n");
        exit(EXIT_FAILURE);
    }
    CHECK_EQ_U32(kernel_hle_call(187u, &frame), 0x00000000u);
    CHECK_EQ_U32(kernel_object_live_count(), 0u);
    /* The stale handle no longer resolves, and the reissue is slot 1, not slot 0. */
    CHECK(kernel_object_find(first) == NULL);
    CHECK_EQ_U32(create_event_189((uint32_t)at(OUT_SLOT), 0u, 1u, 0u), 0x00000000u);
    CHECK_EQ_U32(read_slot(OUT_SLOT), 0x00E10004u);
    teardown();
}


/* ===================== handle recycling (T54) ===================== */

static uint32_t nt_close(uint32_t handle)
{
    const uint32_t args[1] = {handle};
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    if (!kernel_frame_build(&frame, scratch, SCRATCH_OUT, args, 1u)) {
        printf("  FATAL: could not build a close frame\n");
        exit(EXIT_FAILURE);
    }
    return kernel_hle_call(187u, &frame);
}

/*
 * The table used to be a LIFETIME cap: next_slot only incremented, so the 257th create
 * failed however many handles had been closed. Closing frees the slot, and the reissued
 * handle carries generation 1 in bits 10..15, so slot 0 comes back as 0x00E10400 and not
 * as 0x00E10000. Literals, derived by hand from the layout in kernel_object.c.
 *
 * MUTATION: drop the release in NtClose, or reissue generation 0.
 */
static void test_a_closed_slot_is_reissued_under_a_new_generation(void)
{
    setup();
    uint32_t handles[KERNEL_OBJECT_MAX];
    for (unsigned i = 0u; i < KERNEL_OBJECT_MAX; i++) {
        handles[i] = kernel_object_create(KERNEL_OBJECT_EVENT, i);
        CHECK(handles[i] != 0u);
    }
    CHECK_EQ_U32(handles[0], 0x00E10000u);
    CHECK_EQ_U32(handles[1], 0x00E10004u);
    CHECK_EQ_U32(handles[255], 0x00E103FCu);
    reset_capture();
    CHECK_EQ_U32(kernel_object_create(KERNEL_OBJECT_EVENT, 0u), 0u);
    CHECK(captured_contains("exhausted"));

    for (unsigned i = 0u; i < KERNEL_OBJECT_MAX; i++) {
        CHECK_EQ_U32(nt_close(handles[i]), 0x00000000u);
    }
    CHECK_EQ_U32(kernel_object_live_count(), 0u);
    CHECK_EQ_U32(kernel_object_create(KERNEL_OBJECT_EVENT, 0u), 0x00E10400u);
    CHECK_EQ_U32(kernel_object_create(KERNEL_OBJECT_EVENT, 0u), 0x00E10404u);
    CHECK_EQ_U32(kernel_object_live_count(), 2u);
    teardown();
}

/*
 * THE POINT OF THE GENERATION. A guest that kept the old value of a closed handle must
 * not reach the object that now owns the slot: lookup, close and reference-by-handle all
 * refuse it, and the live object survives the stale close.
 *
 * MUTATION: do not bump the generation on release, and every check here fails at once.
 */
static void test_a_stale_handle_never_aliases_the_reissued_one(void)
{
    setup();
    uint32_t handles[KERNEL_OBJECT_MAX];
    for (unsigned i = 0u; i < KERNEL_OBJECT_MAX; i++) {
        handles[i] = kernel_object_create(KERNEL_OBJECT_EVENT, i);
    }
    CHECK_EQ_U32(nt_close(handles[0]), 0x00000000u);
    const uint32_t reissued = kernel_object_create(KERNEL_OBJECT_FILE, 0x77u);
    CHECK_EQ_U32(reissued, 0x00E10400u);
    CHECK(reissued != handles[0]);

    CHECK(kernel_object_find(handles[0]) == NULL);
    CHECK(kernel_object_find(reissued) != NULL);

    poison_slot(OUT_SLOT);
    CHECK_EQ_U32(reference_by_handle(handles[0], 0u, (uint32_t)at(OUT_SLOT)), 0xC0000008u);
    CHECK_EQ_U32(read_slot(OUT_SLOT), POISON);

    CHECK_EQ_U32(nt_close(handles[0]), 0xC0000008u);
    CHECK_EQ_U32(kernel_object_bad_close_count(), 1u);
    const kernel_object_entry *entry = kernel_object_find(reissued);
    CHECK(entry != NULL);
    if (entry) {
        CHECK_EQ_U32(entry->kind, KERNEL_OBJECT_FILE);
        CHECK_EQ_U32(entry->owner_tag, 0x77u);
    }
    teardown();
}

/* A value that carries a wrong marker byte, misaligned low bits, a wrong generation or a
 * slot nobody holds is not a handle, even though it decodes to a live slot. Lookup decodes
 * the slot and then compares the WHOLE value, so these are rejected by that comparison.
 * MUTATION: compare only the slot. */
static void test_values_that_are_not_handles_are_not_found(void)
{
    setup();
    const uint32_t handle = kernel_object_create(KERNEL_OBJECT_EVENT, 0u);
    CHECK_EQ_U32(handle, 0x00E10000u);
    CHECK(kernel_object_find(0x00E10001u) == NULL);
    CHECK(kernel_object_find(0x00E10002u) == NULL);
    CHECK(kernel_object_find(0x00E20000u) == NULL);
    CHECK(kernel_object_find(0x01E10000u) == NULL);
    CHECK(kernel_object_find(0x00E10004u) == NULL);
    CHECK(kernel_object_find(0xFFFFFFFFu) == NULL);
    CHECK(kernel_object_find(0xFFFFFFFEu) == NULL);
    CHECK(kernel_object_find(0xFFFFFFFCu) == NULL);
    CHECK(kernel_object_find(0u) == NULL);
    CHECK(kernel_object_find(handle) != NULL);
    teardown();
}

/* Host-side release: frees the slot, honours references, refuses what is not live. */
static void test_host_release_frees_the_slot_once(void)
{
    setup();
    const uint32_t handle = kernel_object_create(KERNEL_OBJECT_EVENT, 0u);
    CHECK(kernel_object_release(handle));
    CHECK_EQ_U32(kernel_object_live_count(), 0u);
    CHECK(!kernel_object_release(handle));
    CHECK(!kernel_object_release(0u));
    teardown();
}

/* Test-side compression of a handle to a 22-bit index, written out from the layout and
 * deliberately not sharing code with kernel_object.c: slot, then low 6 and high 8
 * generation bits. */
static uint32_t handle_index(uint32_t handle)
{
    return ((handle >> 2) & 0xFFu) | (((handle >> 10) & 0x3Fu) << 8) | ((handle >> 24) << 14);
}

/*
 * THE WHOLE LIFETIME, EXHAUSTIVELY: create and close one object at a time until the table
 * gives up. 256 slots x 16384 generations is 4,194,304 handles, and no handle VALUE may be
 * issued twice in that span (a bitmap of all 2^22 indices), each must carry the 0xE1 marker
 * and be a multiple of 4 (so it cannot be 0, -1, -2 or the 0xFFFFFFFC root), and when the
 * generations run out every slot is RETIRED and creation is refused rather than wrapping.
 *
 * MUTATION: wrap the generation instead of retiring (issued handles repeat), or retire
 * early or late (the count of 4194304 moves).
 */
static void test_no_handle_value_is_issued_twice_before_the_table_retires(void)
{
    setup();
    static uint8_t seen[(1u << 22) / 8u];
    uint32_t issued = 0u;
    uint32_t repeats = 0u;
    uint32_t malformed = 0u;
    /* Bounded, so a table that never retires fails the count below instead of hanging. */
    while (issued <= 4200000u) {
        const uint32_t handle = kernel_object_create(KERNEL_OBJECT_EVENT, 0u);
        if (handle == 0u) {
            break;
        }
        issued++;
        const uint32_t index = handle_index(handle);
        if (seen[index >> 3] & (uint8_t)(1u << (index & 7u))) {
            repeats++;
        }
        seen[index >> 3] |= (uint8_t)(1u << (index & 7u));
        if (((handle >> 16) & 0xFFu) != 0xE1u || (handle & 3u) != 0u) {
            malformed++;
        }
        if (!kernel_object_release(handle)) {
            printf("  FATAL: could not release a handle just issued\n");
            exit(EXIT_FAILURE);
        }
    }
    CHECK_EQ_U32(issued, 4194304u);
    CHECK_EQ_U32(repeats, 0u);
    CHECK_EQ_U32(malformed, 0u);
    CHECK_EQ_U32(kernel_object_retired_count(), 256u);
    CHECK_EQ_U32(kernel_object_live_count(), 0u);
    CHECK(captured_contains("RETIRED"));
    reset_capture();
    CHECK_EQ_U32(kernel_object_create(KERNEL_OBJECT_EVENT, 0u), 0u);
    CHECK(captured_contains("exhausted"));
    CHECK(captured_contains("256 retired"));

    kernel_object_reset();
    CHECK_EQ_U32(kernel_object_retired_count(), 0u);
    CHECK_EQ_U32(kernel_object_create(KERNEL_OBJECT_EVENT, 0u), 0x00E10000u);
    teardown();
}

/* ===================== guest accessors (T55) ===================== */

/* The wrap helper: a sum that leaves 32 bits, or a NULL base, is 0, which every accessor
 * refuses. MUTATION: plain addition. */
static void test_guest_add_refuses_a_wrap_and_a_null_base(void)
{
    CHECK_EQ_U32(kernel_guest_add(0x1000u, 4u), 0x1004u);
    CHECK_EQ_U32(kernel_guest_add(0xFFFFFFFCu, 3u), 0xFFFFFFFFu);
    CHECK_EQ_U32(kernel_guest_add(0xFFFFFFFCu, 4u), 0u);
    CHECK_EQ_U32(kernel_guest_add(0xFFFFFFFEu, 4u), 0u);
    CHECK_EQ_U32(kernel_guest_add(0xFFFFFFFFu, 0xFFFFFFFFu), 0u);
    CHECK_EQ_U32(kernel_guest_add(0u, 4u), 0u);
}

/*
 * The accessors turn an unmapped in-range address into a refusal. Two real pages are
 * mapped below 2 GB and the second is unmapped again, so the hole is exact: a range that
 * ends inside the first page passes, one that crosses into the hole or starts in it does
 * not, and a zero-length request is never probed. Addresses 0x1000 and 0x2 are below
 * vm.mmap_min_addr, so they cannot be mapped by anyone.
 *
 * MUTATION: drop the probe, and the hole reads as mapped (and a bare read would fault).
 */
static void test_unmapped_in_range_addresses_are_refused(void)
{
    const long page = sysconf(_SC_PAGESIZE);
    void *base = mmap(NULL, (size_t)page * 2u, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (base == MAP_FAILED || (uintptr_t)base > 0xFFFFFFFFu - (uintptr_t)page * 2u) {
        printf("  FATAL: cannot map two low pages\n");
        exit(EXIT_FAILURE);
    }
    (void)munmap((char *)base + page, (size_t)page);
    const kernel_guest_ptr first = (kernel_guest_ptr)(uintptr_t)base;
    const kernel_guest_ptr hole = first + (kernel_guest_ptr)page;

    CHECK(kernel_guest_at(first, 4u) != NULL);
    CHECK(kernel_guest_at(first + (kernel_guest_ptr)page - 4u, 4u) != NULL);
    CHECK(kernel_guest_at(first + (kernel_guest_ptr)page - 4u, 5u) == NULL);
    CHECK(kernel_guest_at(first, (size_t)page * 2u) == NULL);
    CHECK(kernel_guest_at(hole, 4u) == NULL);
    CHECK(kernel_guest_at(hole, 0u) != NULL);

    uint32_t value = 0xDEADBEEFu;
    uint8_t byte = 0xA5u;
    CHECK(kernel_guest_write_u32(first, 0x11223344u));
    CHECK(kernel_guest_read_u32(first, &value));
    CHECK_EQ_U32(value, 0x11223344u);
    value = 0xDEADBEEFu;
    CHECK(!kernel_guest_read_u32(hole, &value));
    CHECK_EQ_U32(value, 0xDEADBEEFu);
    CHECK(!kernel_guest_write_u32(hole, 1u));
    CHECK(!kernel_guest_read_u8(hole, &byte));
    CHECK_EQ_U32(byte, 0xA5u);
    CHECK(!kernel_guest_write_u8(hole, 1u));
    CHECK(!kernel_guest_read_u32(first + (kernel_guest_ptr)page - 2u, &value));

    CHECK(!kernel_guest_read_u32(0x1000u, &value));
    CHECK(!kernel_guest_write_u32(0x2u, 1u));
    CHECK(!kernel_guest_read_u32(0xFFFFFFFEu, &value));
    CHECK(!kernel_guest_read_u8(0u, &byte));
    CHECK(kernel_guest_at(0xFFFFFFFFu, 2u) == NULL);
    (void)munmap(base, (size_t)page);
}

int main(void)
{
    printf("kernel object HLE tests\n");

    test_registration_binds_all_four_ordinals();
    test_a_live_handle_is_referenced_and_returned();
    test_the_returned_pointer_round_trips_through_dereference();
    test_an_unissued_handle_is_refused_and_writes_nothing();
    test_a_null_out_parameter_is_reported_before_the_handle_is_judged();
    test_an_object_type_is_recorded_not_enforced();
    test_the_object_type_slot_is_never_written();
    test_a_frame_too_short_for_three_arguments_is_reported();
    test_a_close_while_referenced_decrements_rather_than_destroys();
    test_reset_clears_the_new_counters();
    test_event_publication_and_body_refusal();

    test_the_mutant_ordinal_is_implemented();
    test_an_unnamed_unowned_mutant_gets_a_handle();
    test_attributes_with_no_name_are_still_anonymous();
    test_a_named_mutant_is_refused_and_creates_nothing();
    test_a_named_mutant_refusal_reports_its_root_and_attributes();
    test_an_unnamed_mutants_ignored_root_and_attributes_are_reported();
    test_an_initially_owned_mutant_is_refused();
    test_initial_owner_is_the_low_byte_only();
    test_a_missing_handle_slot_is_refused();
    test_an_unwritable_handle_slot_is_reported();
    test_an_unreadable_attributes_block_is_refused();
    test_a_frame_too_short_for_the_mutant_arguments_is_refused();
    test_a_mutant_handle_closes_through_nt_close();

    test_a_named_event_is_refused_and_creates_nothing();
    test_a_named_event_refusal_reports_its_root_and_attributes();
    test_an_event_attributes_block_is_refused_even_unnamed();
    test_an_unreadable_event_attributes_block_is_refused();
    test_the_measured_unnamed_event_still_creates_and_recycles();

    test_a_closed_slot_is_reissued_under_a_new_generation();
    test_a_stale_handle_never_aliases_the_reissued_one();
    test_values_that_are_not_handles_are_not_found();
    test_host_release_frees_the_slot_once();
    test_no_handle_value_is_issued_twice_before_the_table_retires();
    test_guest_add_refuses_a_wrap_and_a_null_base();
    test_unmapped_in_range_addresses_are_refused();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
