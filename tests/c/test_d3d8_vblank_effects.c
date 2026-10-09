/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T372: the helper's derivable device effects (src/gpu/d3d8_vblank_effects.c), coupled to
 * completed waits. The original-vs-C comparison is tests/test_d3d8_vblank_effects_oracle.py,
 * this suite pins exact numbers, the all-or-nothing refusals and the wait coupling (default
 * off, completed waits only, the blank's own virtual time as the timestamp).
 */
#include "test_d3d8_support.h"

#include "d3d8_flip.h"
#include "d3d8_gpu.h"
#include "d3d8_vblank_effects.h"
#include "kernel_clock.h"

#define DEV D3D8_DEVICE_BASE
#define CHECK_STR_EQ(actual, expected)                                                    \
    do {                                                                                  \
        const char *got_ = (actual);                                                      \
        CHECK(strcmp(got_, (expected)) == 0);                                             \
        if (strcmp(got_, (expected)) != 0) printf("    got: %s\n    want: %s\n", got_, (expected)); \
    } while (0)
#define FOURTH_PERIOD_TICKS (4u * (KERNEL_CLOCK_FREQUENCY_HZ / 60u))

static void initialise(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    d3d8_gpu_set_vblank_observer(NULL);
    store(0x3E3F58u, DEV);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    CHECK(d3d8_gpu_create());
    store(DEV + D3D8_VBLANK_DEV_MODE, 0x00400000u);
    kernel_clock_reset();
    d3d8_vblank_effects_configure(false);
}

static uint32_t wait_failed(void *context)
{
    (void)context;
    return STATUS_UNSUCCESSFUL;
}

static void apply_exact(void)
{
    initialise();
    store(DEV + D3D8_VBLANK_DEV_COUNT, 1u);
    store(DEV + D3D8_VBLANK_DEV_FLIP_INDEX, 5u);
    d3d8_vblank_record record = d3d8_vblank_effects_apply(0x2000u);
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_COUNT), 2u);
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_TIMESTAMP_LAST), 0x2000u);
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_TIMESTAMP_DELTA), 0u); /* no previous timestamp */
    CHECK_EQ_U32(record.count, 2u);
    CHECK_EQ_U32(record.flip_index, 5u);
    CHECK_EQ_U32(record.flags, 0u);
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_THRESHOLD), 0u);
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_FIELD_STATUS), 0u); /* never invented */

    record = d3d8_vblank_effects_apply(0x2000u + 12222222u);
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_TIMESTAMP_DELTA), 12222222u);
    CHECK_EQ_U32(record.count, 3u);
    CHECK_EQ_U32(d3d8_vblank_effects_applied(), 2u); /* one per helper run applied */

    /* The 32-bit wrap: low halves only, as the original's int subtraction. */
    store(DEV + D3D8_VBLANK_DEV_TIMESTAMP_LAST, 0xFFFFFF00u);
    (void)d3d8_vblank_effects_apply(0x100u);
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_TIMESTAMP_DELTA), 0x200u);

    /* The threshold: hit means flags 2 and the threshold moves on, behind means untouched. */
    store(DEV + D3D8_VBLANK_DEV_COUNT, 4u);
    store(DEV + D3D8_VBLANK_DEV_THRESHOLD, 5u);
    record = d3d8_vblank_effects_apply(0x3000u);
    CHECK_EQ_U32(record.flags, 2u);
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_THRESHOLD), 6u);
    store(DEV + D3D8_VBLANK_DEV_COUNT, 4u);
    store(DEV + D3D8_VBLANK_DEV_THRESHOLD, 2u);
    record = d3d8_vblank_effects_apply(0x3000u);
    CHECK_EQ_U32(record.flags, 0u);
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_THRESHOLD), 2u);
    environment_end();
}

static void flips_are_processed(void)
{
    initialise();
    store(DEV + D3D8_VBLANK_DEV_COUNT, 3u);
    store(DEV + D3D8_VBLANK_DEV_TIMESTAMP_LAST, 0x1111u);
    /* T407: a flip in the consumer slot (index parity 0 selects slot 0) no longer refuses. Due
     * at count 4 (the helper's new count), it flips: flags 1, the record carries the consumer
     * index AFTER the flip and the threshold is NOT touched although it equals the count. */
    store(DEV + D3D8_VBLANK_DEV_FLIP_INDEX, 4u);
    store(DEV + D3D8_VBLANK_DEV_FLIP_SLOT0, 1u);
    store(DEV + D3D8_VBLANK_DEV_FLIP_SLOT0 + 4u, 4u);
    store(DEV + D3D8_VBLANK_DEV_FLIP_SLOT0 + 8u, 0x01234000u);
    store(DEV + D3D8_VBLANK_DEV_THRESHOLD, 4u);
    d3d8_vblank_record record = d3d8_vblank_effects_apply(0x9999u);
    CHECK_EQ_U32(record.count, 4u);
    CHECK_EQ_U32(record.flip_index, 5u);
    CHECK_EQ_U32(record.flags, 1u);
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_FLIP_SLOT0), 0u);
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_THRESHOLD), 4u);
    CHECK_EQ_U32(load(0x003E6408u), 0x01234000u);
    /* Odd parity selects slot 1, and a pending word in slot 0 for a later count is then
     * irrelevant. The next blank finds nothing due: the threshold applies again. */
    store(DEV + D3D8_VBLANK_DEV_FLIP_SLOT0, 1u);
    store(DEV + D3D8_VBLANK_DEV_FLIP_SLOT0 + 4u, 99u);
    record = d3d8_vblank_effects_apply(0x9999u);
    CHECK_EQ_U32(record.count, 5u);
    CHECK_EQ_U32(record.flip_index, 5u);
    CHECK_EQ_U32(record.flags, 0u);
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_FLIP_SLOT0), 1u);
    /* A flip for a later count waits, and the one for the count after it flips. */
    store(DEV + D3D8_VBLANK_DEV_FLIP_INDEX, 6u);
    store(DEV + D3D8_VBLANK_DEV_FLIP_SLOT0 + 4u, 7u);
    record = d3d8_vblank_effects_apply(0x9999u);
    CHECK_EQ_U32(record.flags, 0u);
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_FLIP_SLOT0), 1u);
    record = d3d8_vblank_effects_apply(0x9999u);
    CHECK_EQ_U32(record.count, 7u);
    CHECK_EQ_U32(record.flags, 1u);
    CHECK_EQ_U32(record.flip_index, 7u);
    environment_end();
}

static void refusals_write_nothing(void)
{
    initialise();
    store(DEV + D3D8_VBLANK_DEV_COUNT, 3u);
    store(DEV + D3D8_VBLANK_DEV_TIMESTAMP_LAST, 0x1111u);
    store(DEV + D3D8_VBLANK_DEV_FLIP_INDEX, 5u);
    /* The field status port is observable only with mode bit 0x200000 and not 0x1000000. */
    store(DEV + D3D8_VBLANK_DEV_MODE, 0x02680104u);
    RUN_EXPECTING_FATAL((void)d3d8_vblank_effects_apply(0x9999u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_COUNT), 3u);
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_TIMESTAMP_LAST), 0x1111u); /* nothing written */
    store(DEV + D3D8_VBLANK_DEV_MODE, 0x03680104u); /* port skipped by the helper itself */
    RUN_EXPECTING_FATAL((void)d3d8_vblank_effects_apply(0x9999u));
    CHECK(!fatal_seen);
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_COUNT), 4u);
    store(DEV + D3D8_VBLANK_DEV_MODE, 0x02480104u); /* the measured mode: unobservable */
    RUN_EXPECTING_FATAL((void)d3d8_vblank_effects_apply(0x9999u));
    CHECK(!fatal_seen);
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_COUNT), 5u);
    environment_end();
}

static void waits_couple_only_when_enabled(void)
{
    initialise();
    /* Default off: a completed wait leaves the device count and timestamps alone. */
    d3d8_gpu_wait_vblank();
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_COUNT), 0u);
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_TIMESTAMP_LAST), 0u);
    CHECK_EQ_U32(d3d8_vblank_effects_applied(), 0u);
    CHECK(!d3d8_vblank_effects_enabled());

    d3d8_vblank_effects_configure(true);
    CHECK(d3d8_vblank_effects_enabled());
    d3d8_gpu_wait_vblank();
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_COUNT), 1u);
    /* The clock was framed once by the uncoupled wait, so the second blank is at 2 periods:
     * the timestamp is the clock floor, not a clock read (which would creep). */
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_TIMESTAMP_LAST), 2u * (KERNEL_CLOCK_FREQUENCY_HZ / 60u));
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_TIMESTAMP_DELTA), 0u);
    (void)kernel_clock_read(); /* reads and stalls must not move the next timestamp */
    (void)kernel_clock_read();
    CHECK(kernel_clock_stall_us(100000u) > 0u);
    CHECK(kernel_clock_peek() > 3u * (KERNEL_CLOCK_FREQUENCY_HZ / 60u));
    d3d8_gpu_wait_vblank();
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_COUNT), 2u);
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_TIMESTAMP_LAST), 3u * (KERNEL_CLOCK_FREQUENCY_HZ / 60u));
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_TIMESTAMP_DELTA), KERNEL_CLOCK_FREQUENCY_HZ / 60u);
    CHECK_EQ_U32(d3d8_vblank_effects_applied(), 2u);
    CHECK_EQ_U32(d3d8_gpu_vblank_count(), 3u);

    /* A wait that does not complete is not a vblank: no count. */
    CHECK(kernel_hle_register(159u, wait_failed));
    d3d8_gpu_wait_vblank();
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_COUNT), 2u);
    CHECK_EQ_U32(d3d8_vblank_effects_applied(), 2u);
    CHECK_EQ_U32(d3d8_gpu_vblank_count(), 4u);
    CHECK(kernel_register_all() > 0u);

    /* Enabling resets the applied counter, and a refused state stops the wait itself. */
    d3d8_vblank_effects_configure(true);
    CHECK_EQ_U32(d3d8_vblank_effects_applied(), 0u);
    store(DEV + D3D8_VBLANK_DEV_MODE, 0x02680104u); /* the field status port would be observable */
    RUN_EXPECTING_FATAL(d3d8_gpu_wait_vblank());
    CHECK(fatal_seen);
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_COUNT), 2u);
    CHECK_EQ_U32(d3d8_vblank_effects_applied(), 0u);
    d3d8_vblank_effects_configure(false);
    environment_end();
}

static void four_blanks_floor(void)
{
    initialise();
    d3d8_vblank_effects_configure(true);
    for (unsigned index = 0u; index < 4u; index++) {
        d3d8_gpu_wait_vblank();
    }
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_COUNT), 4u);
    CHECK_EQ_U32(load(DEV + D3D8_VBLANK_DEV_TIMESTAMP_LAST), FOURTH_PERIOD_TICKS);
    d3d8_vblank_effects_configure(false);
    environment_end();
}

/* T513: the schedule trace suffix seeds the original helper under the oracle, so it carries every word
 * the helper started from and the record and threshold it left, and is empty unless both opt-ins are on. */
static void trace_carries_the_helper_words(void)
{
    initialise();
    char out[512];
    memset(out, 'x', sizeof out);
    d3d8_vblank_effects_trace(out, sizeof out);
    CHECK(out[0] == '\0'); /* both off */
    d3d8_vblank_effects_configure(true);
    d3d8_vblank_effects_trace(out, sizeof out);
    CHECK(out[0] == '\0'); /* effects only */
    d3d8_vblank_effects_configure(false);
    d3d8_flip_configure(true);
    d3d8_flip_reset();
    memset(out, 'x', sizeof out);
    d3d8_vblank_effects_trace(out, sizeof out);
    CHECK(out[0] == '\0'); /* the flip model without the effects */
    d3d8_vblank_effects_configure(true);
    d3d8_vblank_effects_trace(out, sizeof out);
    CHECK_STR_EQ(out, " flip-in=0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 flip-out=0,0,0,0"); /* before the first run */
    store(DEV + D3D8_VBLANK_DEV_CALLBACK, 0x22020u);
    store(DEV + D3D8_VBLANK_DEV_MODE, 0x02480104u);
    store(DEV + D3D8_FLIP_DEV_DISPLAY_START_OFF, 1u);
    store(DEV + D3D8_VBLANK_DEV_FLIP_INDEX, 4u);
    store(DEV + D3D8_VBLANK_DEV_COUNT, 3u);
    store(DEV + D3D8_VBLANK_DEV_THRESHOLD, 4u);
    store(DEV + D3D8_FLIP_DEV_LAST_TARGET, 7u);
    store(DEV + D3D8_FLIP_DEV_PRODUCER, 5u);
    store(DEV + D3D8_FLIP_DEV_SLOT0, 1u);
    store(DEV + D3D8_FLIP_DEV_SLOT0 + 4u, 4u);
    store(DEV + D3D8_FLIP_DEV_SLOT0 + 8u, 0x01234000u);
    store(DEV + D3D8_FLIP_DEV_SLOT1, 0u);
    store(DEV + D3D8_FLIP_DEV_SLOT1 + 4u, 2u);
    store(DEV + D3D8_FLIP_DEV_SLOT1 + 8u, 0xABCu);
    store(DEV + D3D8_FLIP_DEV_GAMMA_PENDING, 0x11u);
    store(DEV + D3D8_FLIP_DEV_GAMMA_PENDING + 4u, 0x22u);
    (void)d3d8_vblank_effects_apply(0x5000u);
    d3d8_vblank_effects_trace(out, sizeof out);
    /* The words BEFORE the run (count 3, consumer 4), the record and threshold AFTER it (count 4,
     * consumer 5, threshold untouched by a processed flip, flags 1). */
    CHECK_STR_EQ(out, " flip-in=139296,38273284,1,4,3,4,7,5,1,4,19087360,0,2,2748,17,34 flip-out=4,5,4,1");
    CHECK_EQ_U32(d3d8_vblank_effects_last_input().consumer, 4u);
    CHECK_EQ_U32(d3d8_vblank_effects_last_record().flip_index, 5u);
    /* A threshold hit with no flip: flags 2 and the bumped threshold in the out words. */
    store(DEV + D3D8_VBLANK_DEV_COUNT, 4u);
    store(DEV + D3D8_VBLANK_DEV_THRESHOLD, 5u);
    (void)d3d8_vblank_effects_apply(0x6000u);
    d3d8_vblank_effects_trace(out, sizeof out);
    CHECK_STR_EQ(out, " flip-in=139296,38273284,1,5,4,5,7,5,0,4,19087360,0,2,2748,17,34 flip-out=5,5,6,2");
    /* A short buffer is cut, never overrun, and a missing one is ignored. */
    char small[8];
    memset(small, 'x', sizeof small);
    d3d8_vblank_effects_trace(small, sizeof small);
    CHECK(small[7] == '\0' && strncmp(small, " flip-in", 7) == 0);
    d3d8_vblank_effects_trace(NULL, 0u);
    d3d8_vblank_effects_trace(out, 0u);
    d3d8_vblank_effects_configure(true);
    CHECK_EQ_U32(d3d8_vblank_effects_last_record().count, 0u); /* configuring clears them */
    CHECK_EQ_U32(d3d8_vblank_effects_last_input().count, 0u);
    d3d8_vblank_effects_configure(false);
    d3d8_flip_configure(false);
    environment_end();
}

int main(void)
{
    apply_exact();
    flips_are_processed();
    refusals_write_nothing();
    waits_couple_only_when_enabled();
    four_blanks_floor();
    trace_carries_the_helper_words();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
