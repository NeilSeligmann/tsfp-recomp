/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The object at `Prcb+0x250` -- the field that decides whether this title's main
 * thread walks the real Xbox kernel's PE headers or not.
 *
 * WHY THIS SUITE IS SEPARATE, AND WHY IT HAS NO LIFTED CODE IN IT. The guest's own
 * gate is three instructions at guest VA `0x00381E76`:
 *
 *     mov  eax, fs:[0x20]
 *     cmp  dword ptr [eax+0x250], 0
 *     jne  <return>                  ; non-null: the GDT path is never entered
 *     jmp  0x00381D63                ; null:     mov eax, [0x8001003C], and a fault
 *
 * Everything that decides which way that branch goes is guest memory our own code
 * writes, so it is checkable without any of the 2.56 M lines of lifted C (which are
 * derived from the user's own executable and are not committed). This suite reads
 * the same addresses the guest reads and asserts the guest would take the `jne`.
 *
 * EVERY CHECK HERE IS MUTATION-TESTED. `kernel_thread.h` records the measurement
 * behind each offset; this file records what breaks if the implementation stops
 * honouring it. Each invariant below was confirmed to FAIL when the implementation
 * was deliberately broken in the matching way:
 *
 *   monitor block not zero-filled        -> the_monitor_block_is_zero_filled...
 *   block too small to reach +0x24       -> a_block_too_short_for_... /
 *                                           the_allocated_block_covers_every_offset...
 *   one shared block, not per-thread     -> each_thread_gets_its_own_monitor_block
 *   the field at +0x250 left null        -> the_guests_gate_reads_non_zero... /
 *                                           a_null_monitor_block_is_refused
 *   block carved out of the control page -> the_monitor_block_is_its_own_allocation
 *
 * A test that has not been shown to fail has not been shown to test anything.
 */

#include "kernel_thread.h"

#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "kernel_object.h"
#include "nt_status.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                       \
            failures++;                                                                  \
        }                                                                               \
    } while (0)

#define CHECK_EQ_U32(actual, expected)                                                  \
    do {                                                                                \
        uint32_t a_ = (uint32_t)(actual);                                                \
        uint32_t e_ = (uint32_t)(expected);                                              \
        if (a_ != e_) {                                                                  \
            printf("FAIL %s:%d  %s == %#x, expected %#x\n", __FILE__, __LINE__,          \
                   #actual, (unsigned)a_, (unsigned)e_);                                 \
            failures++;                                                                  \
        }                                                                               \
    } while (0)

static int quiet_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    (void)format;
    va_end(args);
    return 0;
}

static uint32_t alloc_region(uint32_t bytes)
{
    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = bytes;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status = STATUS_SUCCESS;
    const kernel_guest_ptr base = guest_region_alloc(&request, &status);
    if (base == 0u) {
        printf("FAIL could not allocate %u guest bytes (status %#x)\n", bytes,
               (unsigned)status);
        failures++;
    }
    return (uint32_t)base;
}

/* Read the field the guest's gate reads, through the same two indirections: the
 * `fs` base gives the KPCR, KPCR+0x20 gives the Prcb, Prcb+0x250 is the field.
 * Deliberately NOT short-cut to `control + 0x28 + 0x250`: the point of the test is
 * that the chain the guest walks arrives somewhere real. */
static bool read_guest_gate(uint32_t fs_base, uint32_t *out_field)
{
    uint32_t prcb = 0u;
    if (!kernel_guest_read_u32(fs_base + KERNEL_PCR_PRCB, &prcb)) {
        return false;
    }
    if (prcb == 0u) {
        return false;
    }
    return kernel_guest_read_u32(prcb + KERNEL_PRCB_MONITOR, out_field);
}

/* Do two [base, base+bytes) ranges share a byte? */
static bool ranges_overlap(uint32_t a_base, uint32_t a_bytes, uint32_t b_base,
                           uint32_t b_bytes)
{
    return a_base < b_base + b_bytes && b_base < a_base + a_bytes;
}

/* ------------------------------------------------------------------------- */
/* The gate itself. */
/* ------------------------------------------------------------------------- */

/*
 * The whole point of the change, stated as the guest sees it.
 *
 * MUTATION: drop the `prcb_data + KERNEL_PRCB_MONITOR` write from
 * kernel_thread_control_init, or write 0 to it, and this fails -- which is the
 * guest falling back into the kernel-PE-header walk and the 0x8001003C fault.
 */
static void test_the_guests_gate_reads_non_zero_so_the_gdt_path_is_not_taken(void)
{
    const uint32_t control = alloc_region(KERNEL_THREAD_CONTROL_BYTES);
    const uint32_t tls = alloc_region(GUEST_PAGE_SIZE);
    const uint32_t monitor = alloc_region(KERNEL_THREAD_MONITOR_BYTES);
    if (control == 0u || tls == 0u || monitor == 0u) {
        return;
    }
    CHECK(kernel_thread_control_init(control, tls, monitor,
                                     KERNEL_THREAD_MONITOR_BYTES));

    uint32_t field = 0xFFFFFFFFu;
    CHECK(read_guest_gate(control, &field));
    /* `jne` is taken only for a non-zero field. This is the branch. */
    CHECK(field != 0u);
    /* And it must be the block we allocated, not merely some non-zero number: the
     * guest calls through field+0x14 and dereferences field+0x20 and +0x24, so a
     * non-zero value that is not a real allocation is worse than a zero. */
    CHECK_EQ_U32(field, monitor);

    (void)guest_region_free(control);
    (void)guest_region_free(tls);
    (void)guest_region_free(monitor);
}

/*
 * A null block is refused rather than accepted as a degraded mode.
 *
 * HONEST ABOUT WHAT THIS DOES NOT PROVE. Deleting the `monitor_base == 0u` guard
 * from kernel_thread_control_init does NOT make this fail, and that was measured
 * rather than assumed: the zero-filling loop rejects a null base on its first write
 * because `kernel_guest_at` returns NULL for address 0, so the guard is redundant by
 * construction and removing it is an equivalent mutant. What this test pins is the
 * CONTRACT -- a null block never yields a control page that looks built -- which is
 * worth pinning however it is currently enforced, because a future change that
 * pre-zeroes the block somewhere else would take the accidental enforcement away.
 */
static void test_a_null_monitor_block_is_refused(void)
{
    const uint32_t control = alloc_region(KERNEL_THREAD_CONTROL_BYTES);
    const uint32_t tls = alloc_region(GUEST_PAGE_SIZE);
    if (control == 0u || tls == 0u) {
        return;
    }
    CHECK(!kernel_thread_control_init(control, tls, 0u, KERNEL_THREAD_MONITOR_BYTES));

    (void)guest_region_free(control);
    (void)guest_region_free(tls);
}

/* ------------------------------------------------------------------------- */
/* The block's contents. */
/* ------------------------------------------------------------------------- */

/*
 * Zero-filled by us, not by the allocator.
 *
 * The region allocator returns fresh mmap pages, which are zero, so a test that
 * merely allocated and checked would pass with the zero-fill deleted. This one
 * writes 0xAA over the whole block FIRST, so it can only pass if
 * kernel_thread_control_init zeroes it.
 *
 * MUTATION: remove the zero-filling loop from kernel_thread_control_init and this
 * fails. It matters because a non-zero monitor+0x14 is an indirect call to an
 * address nobody chose, and a non-zero monitor+0x20 is a 0x24-byte guest write to
 * one.
 */
static void test_the_monitor_block_is_zero_filled_even_over_dirty_memory(void)
{
    const uint32_t control = alloc_region(KERNEL_THREAD_CONTROL_BYTES);
    const uint32_t tls = alloc_region(GUEST_PAGE_SIZE);
    const uint32_t monitor = alloc_region(KERNEL_THREAD_MONITOR_BYTES);
    if (control == 0u || tls == 0u || monitor == 0u) {
        return;
    }
    for (uint32_t offset = 0u; offset < KERNEL_THREAD_MONITOR_BYTES; offset += 4u) {
        CHECK(kernel_guest_write_u32(monitor + offset, 0xAAAAAAAAu));
    }

    CHECK(kernel_thread_control_init(control, tls, monitor,
                                     KERNEL_THREAD_MONITOR_BYTES));

    uint32_t value = 0u;
    unsigned dirty = 0u;
    for (uint32_t offset = 0u; offset < KERNEL_THREAD_MONITOR_BYTES; offset += 4u) {
        CHECK(kernel_guest_read_u32(monitor + offset, &value));
        if (value != 0u) {
            dirty++;
        }
    }
    CHECK_EQ_U32(dirty, 0u);

    /* Called out individually, because these are the three the guest actually
     * touches and a partial zero-fill that happened to miss one of them would
     * otherwise be reported only as a count. */
    CHECK(kernel_guest_read_u32(monitor + KERNEL_MONITOR_CALLBACK, &value));
    CHECK_EQ_U32(value, 0u);
    CHECK(kernel_guest_read_u32(monitor + KERNEL_MONITOR_BLOCK_A, &value));
    CHECK_EQ_U32(value, 0u);
    CHECK(kernel_guest_read_u32(monitor + KERNEL_MONITOR_BLOCK_B, &value));
    CHECK_EQ_U32(value, 0u);

    (void)guest_region_free(control);
    (void)guest_region_free(tls);
    (void)guest_region_free(monitor);
}

/*
 * A block that does not reach the highest offset the guest dereferences is refused.
 *
 * The guest reads monitor+0x24, so 0x28 bytes is the floor. Checked right at the
 * boundary in both directions, because a `<=`/`<` slip is exactly the mistake this
 * is here to catch and only the boundary can tell the two apart.
 *
 * MUTATION: delete the `monitor_bytes < KERNEL_MONITOR_BYTES_TOUCHED` guard, or
 * change it to compare against a smaller constant, and this fails.
 */
static void test_a_block_too_short_for_the_measured_offsets_is_refused(void)
{
    const uint32_t control = alloc_region(KERNEL_THREAD_CONTROL_BYTES);
    const uint32_t tls = alloc_region(GUEST_PAGE_SIZE);
    const uint32_t monitor = alloc_region(KERNEL_THREAD_MONITOR_BYTES);
    if (control == 0u || tls == 0u || monitor == 0u) {
        return;
    }

    /* The guest's highest touched offset is +0x24, a 4-byte read, so 0x28 is the
     * smallest honest block and 0x27 is one byte short of it. */
    CHECK_EQ_U32(KERNEL_MONITOR_BYTES_TOUCHED, KERNEL_MONITOR_BLOCK_B + 4u);

    CHECK(!kernel_thread_control_init(control, tls, monitor, 0u));
    CHECK(!kernel_thread_control_init(control, tls, monitor,
                                      KERNEL_MONITOR_CALLBACK));
    CHECK(!kernel_thread_control_init(control, tls, monitor,
                                      KERNEL_MONITOR_BLOCK_B));
    CHECK(!kernel_thread_control_init(control, tls, monitor,
                                      KERNEL_MONITOR_BYTES_TOUCHED - 1u));
    /* Exactly at the floor: accepted. */
    CHECK(kernel_thread_control_init(control, tls, monitor,
                                     KERNEL_MONITOR_BYTES_TOUCHED));

    (void)guest_region_free(control);
    (void)guest_region_free(tls);
    (void)guest_region_free(monitor);
}

/* ------------------------------------------------------------------------- */
/* The Prcb's own layout inside the control page. */
/* ------------------------------------------------------------------------- */

/*
 * The Prcb must fit in the control page and must not reach the KTHREAD stub.
 *
 * The Prcb starts at KPCR+0x28 and the guest touches up to +0x254 of it, so it
 * spans 0x28..0x27C of the control page while the KTHREAD stub sits at 0x800.
 *
 * MUTATION: move KERNEL_THREAD_KTHREAD_OFFSET down to 0x100, or shrink
 * KERNEL_THREAD_CONTROL_BYTES to 0x200, and this fails -- as does the guard in
 * kernel_thread_control_init, which refuses to build such a page at all.
 */
static void test_the_prcb_fits_in_the_control_page_clear_of_the_kthread(void)
{
    CHECK(KERNEL_PCR_PRCB_DATA + KERNEL_PRCB_BYTES_TOUCHED
          <= KERNEL_THREAD_KTHREAD_OFFSET);
    CHECK(KERNEL_THREAD_KTHREAD_OFFSET + KERNEL_KTHREAD_TLS_DATA + 4u
          <= KERNEL_THREAD_CONTROL_BYTES);
    /* 0x254 is one past the field at 0x250, which is the highest the guest reads. */
    CHECK_EQ_U32(KERNEL_PRCB_BYTES_TOUCHED, KERNEL_PRCB_MONITOR + 4u);
    CHECK(KERNEL_PRCB_VTABLE_OBJECT < KERNEL_PRCB_MONITOR);
    CHECK(KERNEL_PRCB_ZEROED_FIELD < KERNEL_PRCB_VTABLE_OBJECT);
    /* The guest-ABI offsets pinned ONCE as literals (the measured Prcb field offsets
     * the guest's own instructions use). The probes below read through the macros,
     * which is only sound because a mutated macro fails here first. */
    CHECK_EQ_U32(KERNEL_PRCB_ZEROED_FIELD, 0x1Cu);
    CHECK_EQ_U32(KERNEL_PRCB_VTABLE_OBJECT, 0x24Cu);
    CHECK_EQ_U32(KERNEL_PRCB_MONITOR, 0x250u);

    const uint32_t control = alloc_region(KERNEL_THREAD_CONTROL_BYTES);
    const uint32_t tls = alloc_region(GUEST_PAGE_SIZE);
    const uint32_t monitor = alloc_region(KERNEL_THREAD_MONITOR_BYTES);
    if (control == 0u || tls == 0u || monitor == 0u) {
        return;
    }
    CHECK(kernel_thread_control_init(control, tls, monitor,
                                     KERNEL_THREAD_MONITOR_BYTES));

    /* Literal fs offsets: the guest reads fs:[0x20] and expects its own PrcbData at
     * +0x28, and probing through KERNEL_PCR_PRCB/KERNEL_PCR_PRCB_DATA would move
     * this check together with a mutated constant. */
    uint32_t prcb = 0u;
    CHECK(kernel_guest_read_u32(control + 0x20u, &prcb));
    CHECK_EQ_U32(prcb, control + 0x28u);

    /* Prcb+0x1C is the one Prcb field the guest writes (a zero, at guest VA
     * 0x0044127D). Prove it is writable rather than assuming the page covers it. */
    uint32_t value = 0u;
    CHECK(kernel_guest_write_u32(prcb + KERNEL_PRCB_ZEROED_FIELD, 0x5A5A5A5Au));
    CHECK(kernel_guest_read_u32(prcb + KERNEL_PRCB_ZEROED_FIELD, &value));
    CHECK_EQ_U32(value, 0x5A5A5A5Au);

    /* Prcb+0x24C is a different, vtable'd object and is DELIBERATELY left null; see
     * kernel_thread.h. Asserted so that giving it a value becomes a decision
     * somebody has to make here rather than a side effect somewhere else. */
    CHECK(kernel_guest_read_u32(prcb + KERNEL_PRCB_VTABLE_OBJECT, &value));
    CHECK_EQ_U32(value, 0u);

    (void)guest_region_free(control);
    (void)guest_region_free(tls);
    (void)guest_region_free(monitor);
}

/* ------------------------------------------------------------------------- */
/* Through the real ordinal: the allocation, and one block per thread. */
/* ------------------------------------------------------------------------- */

static bool fake_has_code(uint32_t guest_va)
{
    return guest_va >= 0x10000u;
}

static void fake_enter(const kernel_thread_launch *launch)
{
    (void)launch;
}

static const kernel_thread_host_ops FAKE_OPS = {
    .has_code = fake_has_code,
    .enter = fake_enter,
    .terminate = NULL,
};

/* Call ordinal 255 with a full 10-argument frame; returns the handle written. */
static uint32_t create_thread(uint32_t start_routine, uint32_t system_routine)
{
    const uint32_t scratch = alloc_region(0x4000u);
    if (scratch == 0u) {
        return 0u;
    }
    const uint32_t args[10] = {
        scratch, 0u, 0u, 0u, 0u, start_routine, 0u, 0u, 0u, system_routine,
    };
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    CHECK(kernel_frame_build(&frame, scratch + 0x100u, 0x100u, args, 10u));
    CHECK_EQ_U32(kernel_hle_call(255u, &frame), (uint32_t)STATUS_SUCCESS);
    uint32_t handle = 0u;
    CHECK(kernel_guest_read_u32(scratch, &handle));
    return handle;
}

static void begin_case(void)
{
    kernel_hle_init();
    kernel_hle_set_log(quiet_log);
    kernel_object_reset();
    CHECK(kernel_thread_reset());
    CHECK_EQ_U32(kernel_thread_register(), 10u);
    CHECK(kernel_thread_set_host_ops(&FAKE_OPS));
}

static void end_case(void)
{
    CHECK_EQ_U32(kernel_thread_join_all(5000u), 0u);
    CHECK(kernel_thread_set_host_ops(NULL));
    CHECK(kernel_thread_reset());
    kernel_hle_set_log(NULL);
}

/*
 * The block the ordinal really allocates is long enough for every offset.
 *
 * Checks the allocation's REQUESTED size, not its rounded size: the page allocator
 * rounds up to 0x1000, so a request of 0x20 bytes would still yield a page and a
 * test that looked at the rounded size would pass against it.
 *
 * MUTATION: change provision_thread's `request.bytes = KERNEL_THREAD_MONITOR_BYTES`
 * to 0x20 and this fails on the `requested` check -- and if the floor guard in
 * control_init is also removed, it fails on the coverage check too.
 */
static void test_the_allocated_block_covers_every_offset_the_guest_touches(void)
{
    begin_case();
    const uint32_t handle = create_thread(0x200000u, 0x37FE1Du);
    kernel_thread_record record;
    memset(&record, 0, sizeof(record));
    CHECK(kernel_thread_get(handle, &record));
    CHECK(record.monitor_base != 0u);

    const guest_region *region = guest_region_at(record.monitor_base);
    CHECK(region != NULL);
    if (region) {
        /* What provision_thread asked for, before any rounding. */
        CHECK(region->requested >= KERNEL_MONITOR_BYTES_TOUCHED);
        CHECK_EQ_U32(region->requested, KERNEL_THREAD_MONITOR_BYTES);
        /* And the highest byte the guest dereferences is inside the region. */
        CHECK(record.monitor_base + KERNEL_MONITOR_BLOCK_B + 4u
              <= region->address + region->size);
    }

    /* The gate the guest reads really resolves to this block. */
    uint32_t field = 0u;
    CHECK(read_guest_gate(record.control_base, &field));
    CHECK_EQ_U32(field, record.monitor_base);
    end_case();
}

/*
 * One block per thread, not one shared block.
 *
 * ON THE RECORD: the guest never writes the block, so a single shared block would
 * be observationally identical to the guest as the code stands today. This test
 * pins our chosen design rather than a guest-observable requirement -- the design
 * being that the block is allocated and released with the Prcb that points at it,
 * one to one, so a future callee that does write through it cannot reach another
 * thread's.
 *
 * MUTATION: make provision_thread use a single static block for every thread and
 * this fails.
 */
static void test_each_thread_gets_its_own_monitor_block(void)
{
    begin_case();
    kernel_thread_record record[3];
    memset(record, 0, sizeof(record));
    for (unsigned i = 0u; i < 3u; i++) {
        const uint32_t handle = create_thread(0x200000u + i * 0x1000u, 0x37FE1Du);
        CHECK(kernel_thread_get(handle, &record[i]));
        CHECK(record[i].monitor_base != 0u);
    }
    for (unsigned i = 0u; i < 3u; i++) {
        for (unsigned j = i + 1u; j < 3u; j++) {
            CHECK(record[i].monitor_base != record[j].monitor_base);
            CHECK(!ranges_overlap(record[i].monitor_base, KERNEL_THREAD_MONITOR_BYTES,
                                  record[j].monitor_base,
                                  KERNEL_THREAD_MONITOR_BYTES));
            /* Each thread's gate must resolve to its OWN block; two Prcbs pointing
             * at one block is the same defect seen from the guest's side. */
            uint32_t field_i = 0u;
            uint32_t field_j = 0u;
            CHECK(read_guest_gate(record[i].control_base, &field_i));
            CHECK(read_guest_gate(record[j].control_base, &field_j));
            CHECK_EQ_U32(field_i, record[i].monitor_base);
            CHECK_EQ_U32(field_j, record[j].monitor_base);
            CHECK(field_i != field_j);
        }
    }
    end_case();
}

/*
 * Its own allocation, overlapping nothing else the thread owns.
 *
 * This is the invariant upstream's maintainer enforced when they rejected
 * overlaying KTHREAD on the TIB: a guest write through one of the structure's
 * offsets must not be able to land in a neighbour. See kernel_thread.h.
 *
 * MUTATION: set `record->monitor_base = control + 0xC00` instead of allocating, and
 * this fails on the control-page overlap check.
 */
static void test_the_monitor_block_is_its_own_allocation(void)
{
    begin_case();
    const uint32_t handle = create_thread(0x200000u, 0x37FE1Du);
    kernel_thread_record record;
    memset(&record, 0, sizeof(record));
    CHECK(kernel_thread_get(handle, &record));
    CHECK(record.monitor_base != 0u);

    /* It is a region base in its own right, not an address inside another region. */
    const guest_region *region = guest_region_at(record.monitor_base);
    CHECK(region != NULL);

    CHECK(!ranges_overlap(record.monitor_base, KERNEL_THREAD_MONITOR_BYTES,
                          record.control_base, KERNEL_THREAD_CONTROL_BYTES));
    CHECK(!ranges_overlap(record.monitor_base, KERNEL_THREAD_MONITOR_BYTES,
                          record.stack_region, record.stack_region_bytes));
    CHECK(record.tls_base != 0u);
    CHECK(!ranges_overlap(record.monitor_base, KERNEL_THREAD_MONITOR_BYTES,
                          record.tls_base, KERNEL_THREAD_TLS_MIN));
    end_case();
}

/*
 * Releasing a thread releases its block too.
 *
 * MUTATION: drop the `monitor_base` arm from release_record and this fails: the
 * region count does not come back down, which is a page leaked per thread created.
 */
static void test_releasing_a_thread_releases_its_monitor_block(void)
{
    begin_case();
    const size_t before = guest_mem_region_count();
    const uint32_t handle = create_thread(0x200000u, 0x37FE1Du);
    kernel_thread_record record;
    memset(&record, 0, sizeof(record));
    CHECK(kernel_thread_get(handle, &record));
    const uint32_t monitor = record.monitor_base;
    CHECK(monitor != 0u);
    CHECK(guest_region_at(monitor) != NULL);

    CHECK_EQ_U32(kernel_thread_join_all(5000u), 0u);
    CHECK(kernel_thread_reset());

    /* The scratch region create_thread allocated for the out-parameters is still
     * live and deliberately not counted against the thread, so the comparison is
     * "no thread-owned region survived", not "the count is identical". */
    CHECK(guest_region_at(monitor) == NULL);
    CHECK(guest_mem_region_count() <= before + 1u);

    CHECK(kernel_thread_set_host_ops(NULL));
    kernel_hle_set_log(NULL);
}

int main(void)
{
    test_the_guests_gate_reads_non_zero_so_the_gdt_path_is_not_taken();
    test_a_null_monitor_block_is_refused();
    test_the_monitor_block_is_zero_filled_even_over_dirty_memory();
    test_a_block_too_short_for_the_measured_offsets_is_refused();
    test_the_prcb_fits_in_the_control_page_clear_of_the_kthread();
    test_the_allocated_block_covers_every_offset_the_guest_touches();
    test_each_thread_gets_its_own_monitor_block();
    test_the_monitor_block_is_its_own_allocation();
    test_releasing_a_thread_releases_its_monitor_block();

    if (failures != 0) {
        printf("prcb_monitor: %d check(s) failed\n", failures);
        return 1;
    }
    printf("prcb_monitor: all checks passed\n");
    return 0;
}
