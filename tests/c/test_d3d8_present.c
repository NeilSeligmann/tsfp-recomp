/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Synthetic end-of-frame recording, not original-XBE equivalence evidence. */
#include "test_d3d8_support.h"
#include "d3d8_present.h"

static unsigned observed;
static uint64_t observed_last_number;
static uint32_t observed_last_swap_counter;

static void observe(const d3d8_frame_record *record)
{
    observed++;
    observed_last_number = record->number;
    observed_last_swap_counter = record->swap_counter;
}

static unsigned second_observed;
static unsigned second_saw_first_count;
static uint64_t second_number;

static void observe_second(const d3d8_frame_record *record)
{
    second_observed++;
    second_saw_first_count = observed; /* the first observer has already run for this record */
    second_number = record->number;
}

int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    (void)d3d8_device_register();
    write_title_parameters(SCRATCH_DATA, 0x140u);
    const uint32_t args[6] = {0u, 1u, 0u, 0u, SCRATCH_DATA, SCRATCH_DATA + 0x200u};
    CHECK_EQ_U32(call_stdcall(0x003D9230u, args, 6u), 0u);
    d3d8_present_reset();
    d3d8_device_store32(0x1DE0u, 0u); /* mode-set polling is tested separately */
    const uint32_t target = d3d8_device_load32(0x1A04u);
    CHECK_EQ_U32(d3d8_swap(2u), 1u);
    CHECK_EQ_U32(d3d8_frame_queue_count(), 0u);
    CHECK_EQ_U32(d3d8_swap(4u), 1u);
    CHECK_EQ_U32(d3d8_device_load32(0x1A04u), target);
    CHECK_EQ_U32(d3d8_frame_queue_count(), 1u);
    CHECK_EQ_U32(d3d8_frame_queue_at(0).vblank, 1u);
    CHECK_EQ_U32(d3d8_frame_queue_at(0).width, 640u);
    CHECK_EQ_U32(d3d8_frame_queue_at(0).height, 480u);
    for (unsigned i = 0; i < D3D8_FRAME_QUEUE_CAPACITY; i++) {
        (void)d3d8_swap(2u);
        (void)d3d8_swap(4u);
    }
    CHECK_EQ_U32(d3d8_frame_queue_count(), D3D8_FRAME_QUEUE_CAPACITY);
    CHECK_EQ_U32(d3d8_frame_queue_total(), D3D8_FRAME_QUEUE_CAPACITY + 1u);
    CHECK_EQ_U32(d3d8_frame_queue_overruns(), 1u);
    CHECK_EQ_U32(d3d8_frame_queue_at(0).number, 2u);
    CHECK_EQ_U32(d3d8_frame_queue_at(D3D8_FRAME_QUEUE_CAPACITY).number, 0u);

    /* T422: nothing observed the presents above (default NULL). Once set, the observer is called once
     * per Swap(4) with the record just queued and never at Swap(2). */
    CHECK_EQ_U32(observed, 0u);
    const uint64_t queued = d3d8_frame_queue_total();
    d3d8_present_set_observer(observe);
    (void)d3d8_swap(2u);
    CHECK_EQ_U32(observed, 0u);
    const uint32_t counter = d3d8_swap(4u);
    CHECK_EQ_U32(observed, 1u);
    CHECK(observed_last_number == queued + 1u);
    CHECK_EQ_U32(observed_last_swap_counter, counter);
    CHECK(d3d8_frame_queue_total() == observed_last_number);
    /* The reset keeps it (configuration), clearing it stops the calls. */
    d3d8_present_reset();
    (void)d3d8_swap(2u);
    (void)d3d8_swap(4u);
    CHECK_EQ_U32(observed, 2u);
    d3d8_present_set_observer(NULL);
    (void)d3d8_swap(2u);
    (void)d3d8_swap(4u);
    CHECK_EQ_U32(observed, 2u);

    /* T838: the SECOND observer slot (the live renderer's) runs with the same record, after the first, independent of it: both run
     * together, either alone runs, clearing one leaves the other, and Swap(2) calls neither. */
    CHECK_EQ_U32(second_observed, 0u);
    d3d8_present_set_observer(observe);
    d3d8_present_set_second_observer(observe_second);
    (void)d3d8_swap(2u);
    CHECK_EQ_U32(observed, 2u);
    CHECK_EQ_U32(second_observed, 0u);
    (void)d3d8_swap(4u);
    CHECK_EQ_U32(observed, 3u);
    CHECK_EQ_U32(second_observed, 1u);
    CHECK(second_saw_first_count == 3u && second_number == observed_last_number); /* after the first, same record */
    d3d8_present_set_observer(NULL);
    (void)d3d8_swap(2u);
    (void)d3d8_swap(4u);
    CHECK_EQ_U32(observed, 3u);
    CHECK_EQ_U32(second_observed, 2u);
    d3d8_present_reset(); /* configuration, kept like the first */
    (void)d3d8_swap(2u);
    (void)d3d8_swap(4u);
    CHECK_EQ_U32(second_observed, 3u);
    d3d8_present_set_second_observer(NULL);
    (void)d3d8_swap(2u);
    (void)d3d8_swap(4u);
    CHECK_EQ_U32(second_observed, 3u);
    environment_end();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
