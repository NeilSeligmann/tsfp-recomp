/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * XBE section residency: ordinals 327 XeLoadSection and 328 XeUnloadSection.
 *
 * WHAT THERE IS TO GET WRONG HERE, in order of how badly it hurts.
 *
 *   1. THE ARITY. The measured table publishes TWO stack arguments for 327 and it is
 *      WRONG -- its single-site bracket counts the call site's `push esi` prologue as
 *      an argument (src/xbox/kernel_xe.h derives the real 1 three ways). A handler that
 *      believed 2 would still work on a frame long enough to hold a second slot, so one
 *      test hands over a frame with EXACTLY ONE argument and requires success, and
 *      another hands over an empty frame and requires refusal. Those two together pin
 *      the count from both sides. Getting it wrong also moves the guest's modelled esp
 *      by four bytes forever, which produces a plausible wrong trace rather than a
 *      crash.
 *
 *   2. THE REFERENCE COUNT BEING REAL. This is the trap the whole module exists for.
 *      xbe_map() already copied every section, demand-paged or not, so the pages are
 *      resident before the title asks and a handler that returned STATUS_SUCCESS and
 *      did nothing else would look PERFECT: the title gets a working VirtualAddress
 *      either way and the boot advances by exactly as many calls. The lie only surfaces
 *      on the matching unload. So the tests below read the count out of GUEST memory at
 *      Section+0x18 after every call, and one of them requires an unmatched unload to
 *      be reported as an underflow rather than clamped.
 *
 *   3. THE FIELD OFFSET. +0x18 sits between SectionNameAddress at +0x14 and
 *      HeadSharedPageReferenceCountAddress at +0x1C, both of which hold live values.
 *      Writing the count one slot either side would corrupt a pointer the guest uses
 *      and still pass a test that only ever reads back the field it wrote. So the
 *      neighbours are asserted unchanged.
 *
 *   4. THE HANDLE NOT BEING AN INDEX. A handle is a guest POINTER straight at an
 *      XBE_SECTION_HEADER, measured at 0x0037C977 (`mov eax, edi`). Using the derived
 *      index where the pointer belongs, or vice versa, would write a count into low
 *      guest memory. Two sections are set up with distinct counts so that a call
 *      against one cannot satisfy an assertion about the other.
 *
 *   5. THE BOUNDS CHECK. A pointer into the MIDDLE of a header reads every field at the
 *      wrong offset and every value still looks plausible, so stride alignment is
 *      checked as well as range.
 *
 * DELIBERATELY FREE OF LIFTED CODE AND OF THE REAL XBE. The tests build their own XBE
 * header and section table in scratch guest memory, so the layout under test is the one
 * src/xbox/kernel_xe.h derives rather than whatever happens to be in build/default.xbe.
 * The real image's numbers appear only as the shape the synthetic table imitates:
 * section 13 of this title is "$$XTINFO", 0x98 bytes at 0x007F4020, flags 0x08.
 *
 * ONE PATH IS DELIBERATELY UNCOVERED and said so rather than faked: the
 * kernel_guest_write_u32 failure arms. No ctest binary can produce a guest address that
 * reads fine and then refuses a write, because kernel_guest_at() applies the same test
 * to both.
 */

#include "kernel_xe.h"

#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "nt_status.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        checks++;                                                                       \
        if (!(cond)) {                                                                   \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                       \
            failures++;                                                                  \
        }                                                                               \
    } while (0)

#define CHECK_EQ_U32(actual, expected)                                                  \
    do {                                                                                \
        checks++;                                                                       \
        uint32_t a_ = (uint32_t)(actual);                                                \
        uint32_t e_ = (uint32_t)(expected);                                              \
        if (a_ != e_) {                                                                   \
            printf("FAIL %s:%d  %s == %#x, expected %#x\n", __FILE__, __LINE__, #actual,  \
                   (unsigned)a_, (unsigned)e_);                                           \
            failures++;                                                                   \
        }                                                                               \
    } while (0)

/* The guest tests the sign of the NTSTATUS: `test eax, eax; jge` at 0x0037C989 and
 * 0x0037C9A8. A failure the guest reads as success is worse than no failure at all, so
 * every refusal is required to have the top bit set. */
#define CHECK_GUEST_SEES_FAILURE(status)                                                 \
    do {                                                                                \
        checks++;                                                                       \
        uint32_t s_ = (uint32_t)(status);                                                \
        if ((s_ & 0x80000000u) == 0u) {                                                   \
            printf("FAIL %s:%d  %s == %#x, which the guest's `jge` reads as SUCCESS\n",    \
                   __FILE__, __LINE__, #status, (unsigned)s_);                            \
            failures++;                                                                   \
        }                                                                               \
    } while (0)

static char captured[32768];
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
            captured_len = sizeof(captured) - 1u;
        }
    }
    return written;
}

static void reset_capture(void)
{
    captured[0] = '\0';
    captured_len = 0u;
}

static bool captured_contains(const char *needle)
{
    return strstr(captured, needle) != NULL;
}

static unsigned captured_occurrences(const char *needle)
{
    unsigned found = 0u;
    const char *at = captured;
    const size_t len = strlen(needle);
    while ((at = strstr(at, needle)) != NULL) {
        found++;
        at += len;
    }
    return found;
}

#define ORD_XE_LOAD_SECTION 327u
#define ORD_XE_UNLOAD_SECTION 328u

/* XBE_SECTION_HEADER field offsets, restated here on purpose. The test must assert
 * against the layout src/xbox/kernel_xe.h DERIVES, not against whatever the
 * implementation happens to use -- sharing a constant would make a shifted-offset
 * mutation invisible. */
#define SECTION_BYTES 0x38u
#define OFF_FLAGS 0x00u
#define OFF_VIRTUAL_ADDRESS 0x04u
#define OFF_VIRTUAL_SIZE 0x08u
#define OFF_FILE_ADDRESS 0x0Cu
#define OFF_FILE_SIZE 0x10u
#define OFF_NAME_ADDRESS 0x14u
#define OFF_REFERENCE_COUNT 0x18u
#define OFF_HEAD_PAGE_COUNT 0x1Cu
#define OFF_TAIL_PAGE_COUNT 0x20u

#define OFF_HEADER_NUMBER_OF_SECTIONS 0x11Cu
#define OFF_HEADER_SECTION_HEADERS 0x120u
#define XBEH 0x48454258u

/*
 * The synthetic image. One 0x8000 guest region laid out like an XBE's header region:
 *
 *     +0x0000  XBE_HEADER            magic, NumberOfSections, SectionHeadersAddress
 *     +0x0200  section name strings
 *     +0x0370  section table         SECTION_COUNT entries of 0x38, as in the real image
 *     +0x1000  section bodies        mapped, so the residency probe passes
 *     +0x4000  call frames
 */
#define REGION_BYTES 0x8000u
#define HEADER_AT 0x0000u
#define NAMES_AT 0x0200u
#define TABLE_AT 0x0370u
#define BODIES_AT 0x1000u
#define FRAMES_AT 0x4000u
#define FRAME_BYTES 0x200u
#define SECTION_COUNT 4u

/* Which synthetic section is which. Section 2 deliberately points its VirtualAddress at
 * memory the host has not mapped. */
#define SEC_XTINFO 0u
#define SEC_PRELOAD 1u
#define SEC_NOT_MAPPED 2u
#define SEC_SPARE 3u

static kernel_guest_ptr region;
static kernel_guest_ptr table;
static kernel_guest_ptr unmapped_addr;

static kernel_guest_ptr section_at(uint32_t index)
{
    return table + index * SECTION_BYTES;
}

static uint32_t read32(kernel_guest_ptr addr)
{
    uint32_t value = 0xDEADBEEFu;
    if (!kernel_guest_read_u32(addr, &value)) {
        printf("FATAL could not read guest 0x%08X\n", (unsigned)addr);
        exit(EXIT_FAILURE);
    }
    return value;
}

static void write32(kernel_guest_ptr addr, uint32_t value)
{
    if (!kernel_guest_write_u32(addr, value)) {
        printf("FATAL could not write guest 0x%08X\n", (unsigned)addr);
        exit(EXIT_FAILURE);
    }
}

static void write_name(kernel_guest_ptr addr, const char *text)
{
    for (size_t i = 0u;; i++) {
        if (!kernel_guest_write_u8(addr + (uint32_t)i, (uint8_t)text[i])) {
            printf("FATAL could not write a name byte\n");
            exit(EXIT_FAILURE);
        }
        if (text[i] == '\0') {
            return;
        }
    }
}

/*
 * A guest address this host has NOT mapped, found rather than assumed.
 *
 * msync(MS_ASYNC) is the same probe the handler uses, so this cannot silently pick a
 * mapped address and turn the residency test into a false pass. If every candidate is
 * mapped the suite says SKIPPED instead of asserting something it did not establish.
 */
static kernel_guest_ptr find_unmapped_guest_address(void)
{
    static const unsigned long candidates[] = {
        0xE0000000uL, 0xE4000000uL, 0xE8000000uL, 0xEC000000uL,
        0xF0000000uL, 0xF4000000uL, 0xF8000000uL, 0xFC000000uL,
    };
    for (size_t i = 0u; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (msync((void *)(uintptr_t)candidates[i], 0x1000u, MS_ASYNC) != 0) {
            return (kernel_guest_ptr)candidates[i];
        }
    }
    return 0u;
}

static void build_image(void)
{
    table = region + TABLE_AT;

    write32(region + HEADER_AT, XBEH);
    write32(region + OFF_HEADER_NUMBER_OF_SECTIONS, SECTION_COUNT);
    write32(region + OFF_HEADER_SECTION_HEADERS, table);

    const kernel_guest_ptr names = region + NAMES_AT;
    write_name(names + 0x00u, "$$XTINFO");
    write_name(names + 0x20u, ".text");
    write_name(names + 0x40u, "$$XSIMAGE");
    write_name(names + 0x60u, "$$XTIMAGE");

    static const struct {
        uint32_t flags;
        uint32_t body_offset;
        uint32_t size;
        uint32_t name_offset;
    } plan[SECTION_COUNT] = {
        /* Shaped on this title's section 13: demand-paged, 0x98 bytes, not page
         * aligned, so the handler's outward page rounding is exercised for real. */
        {0x08u, BODIES_AT + 0x20u, 0x98u, 0x00u},
        {0x16u, BODIES_AT + 0x1000u, 0x100u, 0x20u},
        {0x18u, 0u /* filled in with an unmapped address below */, 0x1000u, 0x40u},
        {0x28u, BODIES_AT + 0x2000u, 0x40u, 0x60u},
    };

    for (uint32_t i = 0u; i < SECTION_COUNT; i++) {
        const kernel_guest_ptr section = section_at(i);
        write32(section + OFF_FLAGS, plan[i].flags);
        write32(section + OFF_VIRTUAL_ADDRESS,
                (i == SEC_NOT_MAPPED) ? unmapped_addr : region + plan[i].body_offset);
        write32(section + OFF_VIRTUAL_SIZE, plan[i].size);
        write32(section + OFF_FILE_ADDRESS, 0x5F5000u + i * 0x1000u);
        write32(section + OFF_FILE_SIZE, plan[i].size);
        write32(section + OFF_NAME_ADDRESS, region + NAMES_AT + plan[i].name_offset);
        write32(section + OFF_REFERENCE_COUNT, 0u);
        /* The shared-page counter chain, built the way the real image's is: each
         * section's tail counter address is the next section's head. Live values whose
         * survival proves the count went to +0x18 and not to a neighbour. */
        write32(section + OFF_HEAD_PAGE_COUNT, region + 0x100u + i * 2u);
        write32(section + OFF_TAIL_PAGE_COUNT, region + 0x100u + (i + 1u) * 2u);
    }
}

static void setup(void)
{
    kernel_hle_init();
    guest_mem_reset();
    kernel_xe_reset();
    CHECK_EQ_U32(kernel_xe_register(), 2u);

    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = REGION_BYTES;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status = STATUS_SUCCESS;
    region = guest_region_alloc(&request, &status);
    if (region == 0u) {
        printf("FATAL could not allocate the scratch region (status %#x)\n",
               (unsigned)status);
        exit(EXIT_FAILURE);
    }

    unmapped_addr = find_unmapped_guest_address();
    build_image();
    kernel_xe_set_image_base(region);

    kernel_hle_set_log(capture_printer);
    reset_capture();
}

static void teardown(void)
{
    kernel_xe_set_image_base(0u);
    kernel_xe_reset();
    kernel_hle_set_log(NULL);
    guest_mem_reset();
    region = 0u;
    table = 0u;
}

/*
 * Call an ordinal with `count` arguments.
 *
 * `count` is a parameter rather than a constant 1 so that the arity can be pinned from
 * BOTH sides: one argument must work and zero must be refused.
 */
static uint32_t call_xe(unsigned ordinal, const uint32_t *args, unsigned count)
{
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    if (!kernel_frame_build(&frame, region + FRAMES_AT, FRAME_BYTES, args, count)) {
        printf("FATAL could not build a call frame\n");
        exit(EXIT_FAILURE);
    }
    /* Clamping the limit to exactly the frame built is what makes a read of argument 1
     * from a one-argument frame FAIL rather than returning whatever sits past the
     * arguments. Without it the arity tests below prove nothing. */
    frame.stack_limit = region + FRAMES_AT + (count + 1u) * 4u;
    return kernel_hle_call(ordinal, &frame);
}

static uint32_t load_section(kernel_guest_ptr section)
{
    const uint32_t args[1] = {section};
    return call_xe(ORD_XE_LOAD_SECTION, args, 1u);
}

static uint32_t unload_section(kernel_guest_ptr section)
{
    const uint32_t args[1] = {section};
    return call_xe(ORD_XE_UNLOAD_SECTION, args, 1u);
}

static uint32_t guest_reference_count(kernel_guest_ptr section)
{
    return read32(section + OFF_REFERENCE_COUNT);
}

/* ------------------------------------------------------------------------- */

/* Mutation: delete either binding in kernel_xe_register(). Without this the handler can
 * be perfect and never dispatched -- the registration is the easiest thing in the
 * project to forget, because kernel_thunk.c has no per-module dispatch. */
static void test_both_ordinals_are_implemented_not_stubs(void)
{
    setup();
    static const struct {
        unsigned ordinal;
        const char *name;
    } expected[2] = {
        /* Names resolved against tools/kernel_ordinals.py, which generated the table
         * this reads back. Asserting the NAME as well as the state is what catches a
         * handler bound to the wrong ordinal number. */
        {ORD_XE_LOAD_SECTION, "XeLoadSection"},
        {ORD_XE_UNLOAD_SECTION, "XeUnloadSection"},
    };
    for (size_t i = 0u; i < 2u; i++) {
        const kernel_entry *entry = kernel_hle_entry(expected[i].ordinal);
        CHECK(entry != NULL);
        if (entry) {
            CHECK_EQ_U32(entry->state, KERNEL_ENTRY_IMPLEMENTED);
            CHECK(entry->name != NULL && strcmp(entry->name, expected[i].name) == 0);
        }
    }

    /* A real implementation must never emit the stub notice, or the backlog report
     * keeps listing work that is already done. */
    reset_capture();
    (void)load_section(section_at(SEC_XTINFO));
    (void)unload_section(section_at(SEC_XTINFO));
    CHECK(!captured_contains("not implemented"));
    CHECK(!captured_contains("stub"));
    teardown();
}

/* The synthetic header is read back through the same path the handler uses, so a later
 * test that depends on the bounds check cannot be passing because the check is off. */
static void test_the_section_table_is_read_from_guest_memory(void)
{
    setup();
    CHECK_EQ_U32(kernel_xe_image_base(), region);

    kernel_guest_ptr found_table = 0u;
    uint32_t found_count = 0u;
    CHECK(kernel_xe_section_table(&found_table, &found_count));
    CHECK_EQ_U32(found_table, table);
    CHECK_EQ_U32(found_count, SECTION_COUNT);

    for (uint32_t i = 0u; i < SECTION_COUNT; i++) {
        uint32_t index = 0xFFFFFFFFu;
        CHECK(kernel_xe_section_index(section_at(i), &index));
        CHECK_EQ_U32(index, i);
    }
    teardown();
}

/*
 * THE CENTRAL TEST. A load must increment the count that lives in GUEST memory, and the
 * neighbouring fields must be untouched.
 *
 * Mutations this kills: not writing the count at all (the one the brief warns about,
 * invisible in a live run because the pages are already mapped); writing it at +0x14 or
 * +0x1C; writing a constant 1 instead of an increment.
 */
static void test_a_load_increments_the_guests_own_reference_count(void)
{
    setup();
    const kernel_guest_ptr section = section_at(SEC_XTINFO);

    const uint32_t name_before = read32(section + OFF_NAME_ADDRESS);
    const uint32_t head_before = read32(section + OFF_HEAD_PAGE_COUNT);
    const uint32_t tail_before = read32(section + OFF_TAIL_PAGE_COUNT);
    const uint32_t va_before = read32(section + OFF_VIRTUAL_ADDRESS);

    CHECK_EQ_U32(guest_reference_count(section), 0u);
    CHECK_EQ_U32(load_section(section), STATUS_SUCCESS);
    CHECK_EQ_U32(guest_reference_count(section), 1u);
    CHECK_EQ_U32(kernel_xe_load_count(), 1u);

    /* The count went to +0x18 and nowhere else. */
    CHECK_EQ_U32(read32(section + OFF_NAME_ADDRESS), name_before);
    CHECK_EQ_U32(read32(section + OFF_HEAD_PAGE_COUNT), head_before);
    CHECK_EQ_U32(read32(section + OFF_TAIL_PAGE_COUNT), tail_before);
    CHECK_EQ_U32(read32(section + OFF_VIRTUAL_ADDRESS), va_before);

    /* The name is read out of guest memory and quoted, which is how a trace reader knows
     * WHICH section moved. */
    CHECK(captured_contains("$$XTINFO"));
    CHECK(captured_contains("section 0"));
    teardown();
}

/* Mutation: increment by a constant, or write `references` back unchanged. A single
 * load/unload pair would still look balanced; three nested ones do not. */
static void test_nested_loads_count_up_and_unloads_count_back_down(void)
{
    setup();
    const kernel_guest_ptr section = section_at(SEC_XTINFO);

    for (uint32_t expected = 1u; expected <= 3u; expected++) {
        CHECK_EQ_U32(load_section(section), STATUS_SUCCESS);
        CHECK_EQ_U32(guest_reference_count(section), expected);
    }
    for (uint32_t expected = 2u;; expected--) {
        CHECK_EQ_U32(unload_section(section), STATUS_SUCCESS);
        CHECK_EQ_U32(guest_reference_count(section), expected);
        if (expected == 0u) {
            break;
        }
    }
    CHECK_EQ_U32(kernel_xe_load_count(), 3u);
    CHECK_EQ_U32(kernel_xe_unload_count(), 3u);
    CHECK_EQ_U32(kernel_xe_underflow_count(), 0u);
    teardown();
}

/* The sequence the title actually performs at 0x0038109C..0x003810CB: load, use the
 * VirtualAddress, unload. It must leave the count exactly where it started. */
static void test_the_titles_own_load_use_unload_sequence_balances(void)
{
    setup();
    const kernel_guest_ptr section = section_at(SEC_XTINFO);

    CHECK_EQ_U32(load_section(section), STATUS_SUCCESS);
    /* What sub_0037C97B returns to its caller, read from the same field. */
    CHECK_EQ_U32(read32(section + OFF_VIRTUAL_ADDRESS), region + BODIES_AT + 0x20u);
    CHECK_EQ_U32(unload_section(section), STATUS_SUCCESS);
    CHECK_EQ_U32(guest_reference_count(section), 0u);
    CHECK_EQ_U32(kernel_xe_underflow_count(), 0u);
    teardown();
}

/*
 * THE UNDERFLOW, which is the only place a load that forgot to count can be caught.
 *
 * Required: counted, named in the log, answered with a status the guest reads as
 * failure, and the count LEFT AT ZERO rather than wrapped. A wrap to 0xFFFFFFFF would
 * then let four billion unmatched unloads succeed.
 */
static void test_an_unmatched_unload_is_an_underflow_not_a_silent_clamp(void)
{
    setup();
    const kernel_guest_ptr section = section_at(SEC_XTINFO);

    CHECK_EQ_U32(guest_reference_count(section), 0u);
    const uint32_t status = unload_section(section);
    CHECK_GUEST_SEES_FAILURE(status);
    CHECK_EQ_U32(kernel_xe_underflow_count(), 1u);
    CHECK_EQ_U32(kernel_xe_unload_count(), 0u);
    CHECK_EQ_U32(guest_reference_count(section), 0u);
    CHECK(captured_contains("UNDERFLOW"));
    teardown();
}

/* An underflow must still be reported after a balanced pair, not just from a cold
 * start -- that is the shape a real counting bug takes. */
static void test_one_unload_too_many_underflows(void)
{
    setup();
    const kernel_guest_ptr section = section_at(SEC_XTINFO);

    CHECK_EQ_U32(load_section(section), STATUS_SUCCESS);
    CHECK_EQ_U32(unload_section(section), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_xe_underflow_count(), 0u);

    CHECK_GUEST_SEES_FAILURE(unload_section(section));
    CHECK_EQ_U32(kernel_xe_underflow_count(), 1u);
    CHECK_EQ_U32(guest_reference_count(section), 0u);
    teardown();
}

/*
 * The handle is a POINTER, not an index, and the two sections hold distinct counts so
 * that a call against one cannot satisfy an assertion about the other.
 *
 * Mutation: use the derived section index where the guest pointer belongs. That writes
 * a count into low guest memory and leaves both real counts at zero.
 */
static void test_a_load_moves_only_the_section_it_was_handed(void)
{
    setup();
    const kernel_guest_ptr first = section_at(SEC_XTINFO);
    const kernel_guest_ptr second = section_at(SEC_PRELOAD);

    CHECK_EQ_U32(load_section(second), STATUS_SUCCESS);
    CHECK_EQ_U32(load_section(second), STATUS_SUCCESS);
    CHECK_EQ_U32(guest_reference_count(second), 2u);
    CHECK_EQ_U32(guest_reference_count(first), 0u);

    CHECK_EQ_U32(load_section(first), STATUS_SUCCESS);
    CHECK_EQ_U32(guest_reference_count(first), 1u);
    CHECK_EQ_U32(guest_reference_count(second), 2u);

    /* Unloading the wrong section is then an underflow, which is the whole point of
     * keeping the count per section rather than globally. */
    CHECK_EQ_U32(unload_section(first), STATUS_SUCCESS);
    CHECK_GUEST_SEES_FAILURE(unload_section(first));
    CHECK_EQ_U32(guest_reference_count(second), 2u);
    teardown();
}

/* The preload flag changes nothing about the counting. It is reported, so a trace shows
 * which kind of section the title asked for, and that is all. */
static void test_a_preload_section_is_counted_like_any_other(void)
{
    setup();
    const kernel_guest_ptr section = section_at(SEC_PRELOAD);
    CHECK_EQ_U32(load_section(section), STATUS_SUCCESS);
    CHECK_EQ_U32(guest_reference_count(section), 1u);
    CHECK(captured_contains("PRELOAD"));
    reset_capture();
    CHECK_EQ_U32(load_section(section_at(SEC_XTINFO)), STATUS_SUCCESS);
    CHECK(captured_contains("demand-paged"));
    teardown();
}

/* ARITY, from the side the measured table gets wrong. ONE argument must be enough. A
 * handler that read argument 1 would refuse this frame, and 327's published arity of 2
 * is exactly that mistake. */
static void test_one_argument_is_enough(void)
{
    setup();
    const uint32_t args[1] = {section_at(SEC_XTINFO)};
    CHECK_EQ_U32(call_xe(ORD_XE_LOAD_SECTION, args, 1u), STATUS_SUCCESS);
    CHECK_EQ_U32(guest_reference_count(section_at(SEC_XTINFO)), 1u);
    CHECK_EQ_U32(call_xe(ORD_XE_UNLOAD_SECTION, args, 1u), STATUS_SUCCESS);
    CHECK_EQ_U32(guest_reference_count(section_at(SEC_XTINFO)), 0u);
    teardown();
}

/* ARITY, from the other side. A frame with NO arguments must be refused rather than
 * reading the return address as a section handle. */
static void test_one_argument_is_required(void)
{
    setup();
    CHECK_GUEST_SEES_FAILURE(call_xe(ORD_XE_LOAD_SECTION, NULL, 0u));
    CHECK_GUEST_SEES_FAILURE(call_xe(ORD_XE_UNLOAD_SECTION, NULL, 0u));
    CHECK_EQ_U32(kernel_xe_load_count(), 0u);
    CHECK_EQ_U32(kernel_xe_unload_count(), 0u);
    CHECK_EQ_U32(kernel_xe_refused_count(), 2u);
    teardown();
}

/*
 * OUT OF RANGE MUST FAIL. A handle one entry past the table is the most dangerous wrong
 * input available, because the bytes there are real guest memory and every field reads
 * as a plausible number.
 */
static void test_a_handle_past_the_end_of_the_table_is_refused(void)
{
    setup();
    const kernel_guest_ptr past = section_at(SECTION_COUNT);
    uint32_t index = 0u;
    CHECK(!kernel_xe_section_index(past, &index));

    CHECK_GUEST_SEES_FAILURE(load_section(past));
    CHECK_GUEST_SEES_FAILURE(unload_section(past));
    CHECK_EQ_U32(kernel_xe_load_count(), 0u);
    CHECK_EQ_U32(kernel_xe_unload_count(), 0u);
    CHECK_EQ_U32(kernel_xe_refused_count(), 2u);
    /* And nothing was written where a header would have been. */
    CHECK_EQ_U32(read32(past + OFF_REFERENCE_COUNT), 0u);
    CHECK(captured_contains("not a section of this image"));
    teardown();
}

/* Below the table is the other direction, and a check that only tested the upper bound
 * would pass without this. */
static void test_a_handle_below_the_table_is_refused(void)
{
    setup();
    CHECK_GUEST_SEES_FAILURE(load_section(table - SECTION_BYTES));
    CHECK_EQ_U32(kernel_xe_load_count(), 0u);
    CHECK_EQ_U32(kernel_xe_refused_count(), 1u);
    teardown();
}

/* Misaligned is in range and still wrong: every field would be read at the wrong
 * offset and every value would look plausible. */
static void test_a_handle_inside_a_header_is_refused(void)
{
    setup();
    const kernel_guest_ptr middle = section_at(SEC_PRELOAD) + 4u;
    uint32_t index = 0u;
    CHECK(!kernel_xe_section_index(middle, &index));
    CHECK_GUEST_SEES_FAILURE(load_section(middle));
    CHECK_EQ_U32(kernel_xe_load_count(), 0u);
    CHECK_EQ_U32(guest_reference_count(section_at(SEC_PRELOAD)), 0u);
    teardown();
}

/* 0xFFFFFFFF is what _XGetSectionHandleA@4 returns for a name it did not find
 * (`or eax, 0xFFFFFFFF` at 0x0037C96D). It must be named as that rather than reported
 * as a generic bad pointer, because the two have different causes. */
static void test_the_not_found_sentinel_is_refused_by_name(void)
{
    setup();
    CHECK_GUEST_SEES_FAILURE(load_section(0xFFFFFFFFu));
    CHECK(captured_contains("NOT-FOUND"));
    CHECK_EQ_U32(kernel_xe_load_count(), 0u);
    teardown();
}

static void test_a_null_handle_is_refused(void)
{
    setup();
    CHECK_GUEST_SEES_FAILURE(load_section(0u));
    CHECK_GUEST_SEES_FAILURE(unload_section(0u));
    CHECK_EQ_U32(kernel_xe_refused_count(), 2u);
    CHECK(captured_contains("XeLoadSection(NULL)"));
    teardown();
}

static void test_a_missing_frame_is_reported(void)
{
    setup();
    CHECK_GUEST_SEES_FAILURE(kernel_hle_call(ORD_XE_LOAD_SECTION, NULL));
    CHECK_GUEST_SEES_FAILURE(kernel_hle_call(ORD_XE_UNLOAD_SECTION, NULL));
    CHECK(captured_contains("no argument frame"));
    CHECK_EQ_U32(kernel_xe_refused_count(), 2u);
    teardown();
}

/*
 * RESIDENCY IS THE ONE PROMISE XeLoadSection MAKES, so a section the host did not map
 * must be refused rather than handed back a success the memory cannot honour.
 *
 * Skipped with a message, never silently passed, if no unmapped guest address can be
 * found -- an assertion that depended on an address being unmapped without checking
 * would be the kind of false pass this project keeps a scar from.
 */
static void test_a_section_whose_pages_are_not_mapped_is_refused(void)
{
    setup();
    if (unmapped_addr == 0u) {
        printf("SKIPPED test_a_section_whose_pages_are_not_mapped_is_refused: every "
               "candidate guest address was already mapped\n");
        teardown();
        return;
    }
    const kernel_guest_ptr section = section_at(SEC_NOT_MAPPED);
    CHECK_GUEST_SEES_FAILURE(load_section(section));
    CHECK_EQ_U32(kernel_xe_not_resident_count(), 1u);
    CHECK_EQ_U32(kernel_xe_load_count(), 0u);
    CHECK_EQ_U32(guest_reference_count(section), 0u);
    CHECK(captured_contains("NOT MAPPED"));
    teardown();
}

/* The sections the host DID map must pass the same probe, or the check would be
 * refusing everything and the test above would pass for the wrong reason. */
static void test_the_mapped_sections_pass_the_residency_probe(void)
{
    setup();
    CHECK_EQ_U32(load_section(section_at(SEC_XTINFO)), STATUS_SUCCESS);
    CHECK_EQ_U32(load_section(section_at(SEC_PRELOAD)), STATUS_SUCCESS);
    CHECK_EQ_U32(load_section(section_at(SEC_SPARE)), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_xe_not_resident_count(), 0u);
    CHECK_EQ_U32(kernel_xe_load_count(), 3u);
    teardown();
}

/*
 * The host's eager mapping is announced ONCE on the first 0 -> 1 transition.
 *
 * Without it a trace implies a paging event that did not happen. Once rather than per
 * call, because a warning repeated for every load hides the next one.
 */
static void test_the_already_mapped_image_is_announced_once(void)
{
    setup();
    CHECK_EQ_U32(load_section(section_at(SEC_XTINFO)), STATUS_SUCCESS);
    CHECK_EQ_U32(captured_occurrences("NOTHING WAS PAGED IN"), 1u);
    CHECK_EQ_U32(load_section(section_at(SEC_PRELOAD)), STATUS_SUCCESS);
    CHECK_EQ_U32(load_section(section_at(SEC_SPARE)), STATUS_SUCCESS);
    CHECK_EQ_U32(captured_occurrences("NOTHING WAS PAGED IN"), 1u);
    teardown();
}

/* A 1 -> 0 transition leaves pages mapped that hardware would have decommitted. Counted
 * and said EVERY time, because each one is a later read that will quietly succeed where
 * hardware would fault. */
static void test_the_last_unload_reports_that_the_pages_stay(void)
{
    setup();
    const kernel_guest_ptr section = section_at(SEC_XTINFO);
    CHECK_EQ_U32(load_section(section), STATUS_SUCCESS);
    CHECK_EQ_U32(load_section(section), STATUS_SUCCESS);
    reset_capture();

    CHECK_EQ_U32(unload_section(section), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_xe_stale_resident_count(), 0u);
    CHECK(!captured_contains("DECOMMITTED"));

    CHECK_EQ_U32(unload_section(section), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_xe_stale_resident_count(), 1u);
    CHECK(captured_contains("DECOMMITTED"));
    teardown();
}

/* An increment that would wrap is refused, not wrapped. A wrap to zero would make the
 * very next unload underflow and blame the guest for our arithmetic. */
static void test_a_reference_count_at_the_maximum_is_refused_not_wrapped(void)
{
    setup();
    const kernel_guest_ptr section = section_at(SEC_XTINFO);
    write32(section + OFF_REFERENCE_COUNT, 0xFFFFFFFFu);

    CHECK_GUEST_SEES_FAILURE(load_section(section));
    CHECK_EQ_U32(guest_reference_count(section), 0xFFFFFFFFu);
    CHECK_EQ_U32(kernel_xe_load_count(), 0u);
    CHECK(captured_contains("would wrap"));
    teardown();
}

/*
 * WITH NO IMAGE BASE the bounds check cannot run, and that is ANNOUNCED rather than
 * passed off as a check that happened. Reference counting still works, because the count
 * lives in the handle's own structure.
 *
 * This is the state the tree is in if the host forgets kernel_xe_set_image_base(), which
 * is the easiest line in the wiring to miss.
 */
static void test_no_image_base_disables_the_bounds_check_loudly(void)
{
    setup();
    kernel_xe_set_image_base(0u);
    reset_capture();

    kernel_guest_ptr found_table = 0u;
    uint32_t found_count = 0u;
    CHECK(!kernel_xe_section_table(&found_table, &found_count));

    const kernel_guest_ptr section = section_at(SEC_XTINFO);
    CHECK_EQ_U32(load_section(section), STATUS_SUCCESS);
    CHECK_EQ_U32(guest_reference_count(section), 1u);
    CHECK(captured_contains("CANNOT BE BOUNDS-CHECKED"));
    /* "section ?" rather than "section 0", so no line implies a validation that did not
     * happen. */
    CHECK(captured_contains("section ?"));

    /* Said once, not once per call. */
    CHECK_EQ_U32(load_section(section), STATUS_SUCCESS);
    CHECK_EQ_U32(captured_occurrences("CANNOT BE BOUNDS-CHECKED"), 1u);
    CHECK_EQ_U32(guest_reference_count(section), 2u);
    teardown();
}

/*
 * A base that is not an XBE is HOST WIRING broken, and must not read as an empty
 * section table. An empty table would put every handle out of range and fail every load
 * for a reason that has nothing to do with the guest.
 */
static void test_a_base_without_the_xbeh_magic_is_not_an_empty_table(void)
{
    setup();
    write32(region + HEADER_AT, 0x12345678u);
    reset_capture();

    kernel_guest_ptr found_table = 0u;
    uint32_t found_count = 0u;
    CHECK(!kernel_xe_section_table(&found_table, &found_count));

    /* Counting still works and the gap is announced, rather than every handle being
     * refused as out of range. */
    CHECK_EQ_U32(load_section(section_at(SEC_XTINFO)), STATUS_SUCCESS);
    CHECK(captured_contains("CANNOT BE BOUNDS-CHECKED"));
    CHECK_EQ_U32(kernel_xe_refused_count(), 0u);
    teardown();
}

/* A NumberOfSections large enough to overflow `table + count * 0x38` must be refused,
 * or the bound would admit a wild handle as in range. */
static void test_an_overflowing_section_count_is_refused(void)
{
    setup();
    kernel_guest_ptr found_table = 0u;
    uint32_t found_count = 0u;

    /* An absurd count is refused by the flat cap. */
    write32(region + OFF_HEADER_NUMBER_OF_SECTIONS, 0xFFFFFFF0u);
    CHECK(!kernel_xe_section_table(&found_table, &found_count));

    /* Zero sections is not an empty image, it is an unreadable header. */
    write32(region + OFF_HEADER_NUMBER_OF_SECTIONS, 0u);
    CHECK(!kernel_xe_section_table(&found_table, &found_count));

    /*
     * AND THE CASE THE FLAT CAP DOES NOT COVER, which is the only one that exercises
     * the wrap check: a count small enough to pass the cap, against a table address
     * high enough that `table + count * 0x38` wraps past 2^32. 100 * 0x38 = 0x2320 and
     * only 0xFF bytes remain above 0xFFFFFF00, so the upper bound would wrap to a low
     * address and admit essentially any handle as in range.
     *
     * Written this way on purpose: the first version of this test used only the absurd
     * count above, the flat cap caught it first, and the wrap check had NO coverage at
     * all -- a mutation that deleted it survived.
     */
    write32(region + OFF_HEADER_NUMBER_OF_SECTIONS, 100u);
    write32(region + OFF_HEADER_SECTION_HEADERS, 0xFFFFFF00u);
    CHECK(!kernel_xe_section_table(&found_table, &found_count));
    teardown();
}

/* Reset clears the counters so one case cannot leak into the next, and KEEPS the image
 * base, which is host wiring installed once. A reset that detached it would silently
 * disable the bounds check for every later call. */
static void test_reset_clears_the_counters_and_keeps_the_image_base(void)
{
    setup();
    const kernel_guest_ptr section = section_at(SEC_XTINFO);
    CHECK_EQ_U32(load_section(section), STATUS_SUCCESS);
    CHECK_GUEST_SEES_FAILURE(load_section(0u));
    CHECK(kernel_xe_load_count() == 1u && kernel_xe_refused_count() == 1u);

    kernel_xe_reset();
    CHECK_EQ_U32(kernel_xe_load_count(), 0u);
    CHECK_EQ_U32(kernel_xe_unload_count(), 0u);
    CHECK_EQ_U32(kernel_xe_refused_count(), 0u);
    CHECK_EQ_U32(kernel_xe_underflow_count(), 0u);
    CHECK_EQ_U32(kernel_xe_not_resident_count(), 0u);
    CHECK_EQ_U32(kernel_xe_stale_resident_count(), 0u);
    CHECK_EQ_U32(kernel_xe_image_base(), region);

    /* The GUEST's count is not a host counter and reset must not touch it: it is the
     * title's own state, and zeroing it would turn the matching unload into a phantom
     * underflow. */
    CHECK_EQ_U32(guest_reference_count(section), 1u);
    teardown();
}

/* ------------------------------------------------------------------------- */

int main(void)
{
    printf("kernel Xe section HLE tests\n");

    test_both_ordinals_are_implemented_not_stubs();
    test_the_section_table_is_read_from_guest_memory();

    test_a_load_increments_the_guests_own_reference_count();
    test_nested_loads_count_up_and_unloads_count_back_down();
    test_the_titles_own_load_use_unload_sequence_balances();
    test_an_unmatched_unload_is_an_underflow_not_a_silent_clamp();
    test_one_unload_too_many_underflows();
    test_a_load_moves_only_the_section_it_was_handed();
    test_a_preload_section_is_counted_like_any_other();

    test_one_argument_is_enough();
    test_one_argument_is_required();

    test_a_handle_past_the_end_of_the_table_is_refused();
    test_a_handle_below_the_table_is_refused();
    test_a_handle_inside_a_header_is_refused();
    test_the_not_found_sentinel_is_refused_by_name();
    test_a_null_handle_is_refused();
    test_a_missing_frame_is_reported();

    test_a_section_whose_pages_are_not_mapped_is_refused();
    test_the_mapped_sections_pass_the_residency_probe();
    test_the_already_mapped_image_is_announced_once();
    test_the_last_unload_reports_that_the_pages_stay();
    test_a_reference_count_at_the_maximum_is_refused_not_wrapped();

    test_no_image_base_disables_the_bounds_check_loudly();
    test_a_base_without_the_xbeh_magic_is_not_an_empty_table();
    test_an_overflowing_section_count_is_refused();
    test_reset_clears_the_counters_and_keeps_the_image_base();

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
