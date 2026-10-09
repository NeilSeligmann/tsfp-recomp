/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T407: the flip queue and flip processor (src/gpu/d3d8_flip.c). The original-vs-C comparison is
 * tests/test_d3d8_flip_oracle.py (dword for dword over 0x003DC200 and 0x003DC5E0). This suite pins
 * exact literals the oracle printed (tools/vblank_helper_oracle.py flip-due, flip-due-gamma,
 * flip-chain), the named refusals, the hardware record and the Swap hook: default off, queued only
 * with the coupled vblank effects, completed by the next completed wait.
 */
#include "test_d3d8_support.h"

#include "d3d8_flip.h"
#include "d3d8_gpu.h"
#include "d3d8_present.h"
#include "d3d8_vblank_effects.h"
#include "kernel_clock.h"

#define DEV D3D8_DEVICE_BASE
#define FRONT_ADDRESS 0x01234000u

static void initialise(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    d3d8_gpu_set_vblank_observer(NULL);
    store(0x3E3F58u, DEV);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    CHECK(d3d8_gpu_create());
    kernel_clock_reset();
    d3d8_vblank_effects_configure(false);
    d3d8_flip_configure(false);
    d3d8_flip_reset();
}

static void seed_identity_ramps(void)
{
    for (uint32_t ramp = 0u; ramp < 2u; ramp++) {
        for (uint32_t entry = 0u; entry < D3D8_FLIP_GAMMA_CHANNEL_BYTES; entry++) {
            for (uint32_t channel = 0u; channel < 3u; channel++) {
                const uint32_t address = D3D8_FLIP_DEV_GAMMA_RAMPS + ramp * D3D8_FLIP_GAMMA_RAMP_BYTES +
                                         channel * D3D8_FLIP_GAMMA_CHANNEL_BYTES + entry;
                const uint32_t word = load(DEV + (address & ~3u));
                const uint32_t shift = (address & 3u) * 8u;
                store(DEV + (address & ~3u), (word & ~(0xFFu << shift)) | (entry << shift));
            }
        }
    }
}

static void set_slot(uint32_t slot, uint32_t pending, uint32_t target, uint32_t value)
{
    const uint32_t base = D3D8_FLIP_DEV_SLOT0 + slot * D3D8_FLIP_SLOT_BYTES;
    store(DEV + base, pending);
    store(DEV + base + 4u, target);
    store(DEV + base + 8u, value);
}

static unsigned process(void)
{
    d3d8_flip_lock();
    const unsigned flips = d3d8_flip_process_locked();
    d3d8_flip_unlock();
    return flips;
}

static unsigned queue(uint32_t data)
{
    d3d8_flip_lock();
    const char *refusal = d3d8_flip_queue_refusal_locked(data);
    CHECK(refusal == NULL);
    const unsigned flips = refusal == NULL ? d3d8_flip_queue_locked(data) : 0u;
    d3d8_flip_unlock();
    return flips;
}

static const char *refusal_of(uint32_t data)
{
    d3d8_flip_lock();
    const char *refusal = d3d8_flip_queue_refusal_locked(data);
    d3d8_flip_unlock();
    return refusal;
}

/* The oracle's flip-due state: count 3, slot 0 pending for count 4 (here count 4 already, the
 * original runs it after count++), a gamma ramp pending. */
static void process_one_flip(void)
{
    initialise();
    seed_identity_ramps();
    store(DEV + D3D8_FLIP_DEV_COUNT, 4u);
    store(DEV + D3D8_FLIP_DEV_GAMMA_PENDING, 1u);
    store(DEV + D3D8_FLIP_DEV_DISPLAY_START_OFF, 1u); /* as after CreateDevice */
    set_slot(0u, 1u, 4u, FRONT_ADDRESS);
    CHECK_EQ_U32(process(), 1u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_SLOT0), 0u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_CONSUMER), 1u);
    CHECK_EQ_U32(load(D3D8_FLIP_GLOBAL_VALUE), FRONT_ADDRESS);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_GAMMA_PENDING), 0u);
    const d3d8_flip_hardware hardware = d3d8_flip_hardware_get();
    CHECK_EQ_U32(hardware.flips, 1u);
    CHECK_EQ_U32(hardware.display_start_writes, 0u); /* +0x1B8 set: no display start */
    CHECK_EQ_U32(hardware.gamma_uploads, 1u);
    CHECK_EQ_U32(hardware.pgraph_increments, 1u);
    CHECK_EQ_U32(hardware.event_count, 2u);
    CHECK_EQ_U32(hardware.events[0].kind, D3D8_FLIP_HW_GAMMA_RAMP);
    CHECK_EQ_U32(hardware.events[1].kind, D3D8_FLIP_HW_PGRAPH_INCREMENT);
    CHECK_EQ_U32(hardware.events[1].value, 2u);
    /* The oracle's ramp upload is 768 bytes, red, green, blue interleaved: 0 0 0 1 1 1 2 2 2. */
    for (uint32_t entry = 0u; entry < 256u; entry++) {
        for (uint32_t channel = 0u; channel < 3u; channel++) {
            CHECK_EQ_U32(hardware.gamma[entry * 3u + channel], entry);
        }
    }
    /* A second pass finds nothing: the slot is clear and the index moved to the empty slot. */
    CHECK_EQ_U32(process(), 0u);
    CHECK_EQ_U32(d3d8_flip_hardware_get().flips, 1u);
    environment_end();
}

static void display_start_and_chain(void)
{
    initialise();
    store(DEV + D3D8_FLIP_DEV_COUNT, 4u);
    store(DEV + D3D8_FLIP_DEV_DISPLAY_START_OFF, 0u);
    set_slot(0u, 1u, 4u, FRONT_ADDRESS);
    set_slot(1u, 1u, 4u, FRONT_ADDRESS + 0x1000u);
    CHECK_EQ_U32(process(), 2u); /* oracle flip-chain: both flip, consumer index +2 */
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_CONSUMER), 2u);
    CHECK_EQ_U32(load(D3D8_FLIP_GLOBAL_VALUE), FRONT_ADDRESS + 0x1000u);
    const d3d8_flip_hardware hardware = d3d8_flip_hardware_get();
    CHECK_EQ_U32(hardware.display_start_writes, 2u);
    CHECK_EQ_U32(hardware.display_start, FRONT_ADDRESS + 0x1000u);
    CHECK_EQ_U32(hardware.events[0].kind, D3D8_FLIP_HW_DISPLAY_START);
    CHECK_EQ_U32(hardware.events[0].value, FRONT_ADDRESS);
    CHECK_EQ_U32(hardware.pgraph_increments, 2u);
    CHECK_EQ_U32(hardware.gamma_uploads, 0u);

    /* A flip for a later blank, then the other slot only: nothing flips. */
    initialise();
    store(DEV + D3D8_FLIP_DEV_COUNT, 1u);
    set_slot(0u, 1u, 9u, FRONT_ADDRESS);
    CHECK_EQ_U32(process(), 0u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_SLOT0), 1u);
    set_slot(0u, 0u, 0u, 0u);
    set_slot(1u, 1u, 1u, FRONT_ADDRESS);
    CHECK_EQ_U32(process(), 0u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_SLOT1), 1u);
    CHECK_EQ_U32(d3d8_flip_hardware_get().event_count, 0u);
    environment_end();
}

static void queue_arithmetic(void)
{
    initialise();
    /* The default interval (0 reads as 1): the target is the next blank, the threshold one
     * interval after it, the slot is filled, the producer index moves, and nothing is due yet. */
    store(DEV + D3D8_FLIP_DEV_COUNT, 5u);
    const uint32_t data = d3d8_flip_swap_method_data(FRONT_ADDRESS, 0u);
    CHECK_EQ_U32(data, ((FRONT_ADDRESS | 1u) << 5) | 1u);
    CHECK_EQ_U32(queue(data), 0u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_SLOT0), 1u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_SLOT0 + 4u), 6u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_SLOT0 + 8u), FRONT_ADDRESS);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_PRODUCER), 1u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_LAST_TARGET), 6u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_THRESHOLD), 7u);
    CHECK_EQ_U32(d3d8_flip_hardware_get().queued, 1u);
    CHECK_EQ_U32(d3d8_flip_hardware_get().flips, 0u);

    /* The second flip lands in slot 1 with the target one interval after the first. */
    CHECK_EQ_U32(queue(d3d8_flip_swap_method_data(FRONT_ADDRESS + 0x1000u, 2u)), 0u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_SLOT1 + 4u), 8u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_THRESHOLD), 10u);
    /* A third finds its slot still pending: the instantaneous GPU has no FLIP_STALL. */
    CHECK(refusal_of(d3d8_flip_swap_method_data(FRONT_ADDRESS, 1u)) != NULL);

    /* The blank passes: the count reaches the first target and the flip runs. */
    store(DEV + D3D8_FLIP_DEV_COUNT, 6u);
    CHECK_EQ_U32(process(), 1u);
    CHECK_EQ_U32(load(D3D8_FLIP_GLOBAL_VALUE), FRONT_ADDRESS);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_CONSUMER), 1u);
    CHECK(refusal_of(d3d8_flip_swap_method_data(FRONT_ADDRESS, 1u)) == NULL);
    store(DEV + D3D8_FLIP_DEV_COUNT, 8u);
    CHECK_EQ_U32(process(), 1u);
    CHECK_EQ_U32(load(D3D8_FLIP_GLOBAL_VALUE), FRONT_ADDRESS + 0x1000u);

    /* The immediate bit shows a flip at once, in the same call, when the target has passed. */
    initialise();
    store(DEV + D3D8_FLIP_DEV_COUNT, 5u);
    const uint32_t immediate = d3d8_flip_swap_method_data(FRONT_ADDRESS, 0x80000000u);
    CHECK_EQ_U32(immediate, ((FRONT_ADDRESS | 8u) << 5) | 1u);
    CHECK_EQ_U32(queue(immediate), 1u);
    CHECK_EQ_U32(load(D3D8_FLIP_GLOBAL_VALUE), FRONT_ADDRESS);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_LAST_TARGET), 5u);
    environment_end();
}

static void swap_data_bits(void)
{
    /* Interval 1 ORs 1, 2 ORs 2, 4 ORs 3 (not 4), the sign bit ORs 8, 0 reads as 1. */
    CHECK_EQ_U32(d3d8_flip_swap_method_data(0x1000u, 0u), ((0x1000u | 1u) << 5) | 1u);
    CHECK_EQ_U32(d3d8_flip_swap_method_data(0x1000u, 1u), ((0x1000u | 1u) << 5) | 1u);
    CHECK_EQ_U32(d3d8_flip_swap_method_data(0x1000u, 2u), ((0x1000u | 2u) << 5) | 1u);
    CHECK_EQ_U32(d3d8_flip_swap_method_data(0x1000u, 3u), ((0x1000u | 3u) << 5) | 1u);
    CHECK_EQ_U32(d3d8_flip_swap_method_data(0x1000u, 4u), ((0x1000u | 3u) << 5) | 1u);
    CHECK_EQ_U32(d3d8_flip_swap_method_data(0x1000u, 0x80000000u), ((0x1000u | 8u) << 5) | 1u);
    CHECK_EQ_U32(d3d8_flip_swap_method_data(0x1000u, 0x80000004u), ((0x1000u | 11u) << 5) | 1u);
}

static void refusals_write_nothing(void)
{
    initialise();
    store(DEV + D3D8_FLIP_DEV_COUNT, 3u);
    const uint32_t flip = d3d8_flip_swap_method_data(FRONT_ADDRESS, 1u);
    CHECK(refusal_of(flip) == NULL);
    CHECK(refusal_of(0u) != NULL);
    CHECK(refusal_of(2u | (FRONT_ADDRESS << 5)) != NULL);
    CHECK(refusal_of(14u) != NULL);
    CHECK(refusal_of(0x21u) == NULL); /* only the low five bits are the type, 0x21 is a flip */
    CHECK(refusal_of(0x11u) != NULL); /* type 17: bit 4 is part of the type */
    store(DEV + D3D8_FLIP_DEV_CALLBACK, 0x22020u);
    CHECK(refusal_of(flip) != NULL);
    store(DEV + D3D8_FLIP_DEV_CALLBACK, 0u);
    set_slot(0u, 1u, 9u, 0u);
    CHECK(refusal_of(flip) != NULL);
    store(DEV + D3D8_FLIP_DEV_PRODUCER, 1u);
    CHECK(refusal_of(flip) == NULL); /* slot 1 is the producer's now */
    environment_end();
}

/* The Swap hook, over a device CreateDevice made. Swap(2) is the prepare (the flip is queued),
 * Swap(4) the finish and the wait that completes it. */
static void swap_hook(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    (void)d3d8_device_register();
    write_title_parameters(SCRATCH_DATA, 0x140u);
    const uint32_t args[6] = {0u, 1u, 0u, 0u, SCRATCH_DATA, SCRATCH_DATA + 0x200u};
    CHECK_EQ_U32(call_stdcall(0x003D9230u, args, 6u), 0u);
    d3d8_present_reset();
    d3d8_device_store32(0x1DE0u, 0u); /* mode-set polling is tested separately */
    d3d8_flip_reset();
    const uint32_t front = d3d8_device_load32(0x1A18u);
    /* The original shifts the address left by five into the method data and the handler shifts
     * it back, so the top five bits are lost and the low four are the interval bits. */
    const uint32_t shown = (((d3d8_guest_load32(front + 4u) | 1u) << 5) >> 5) & ~0xFu;

    /* Default off: a Swap leaves every flip word alone and records nothing. */
    d3d8_vblank_effects_configure(false);
    (void)d3d8_swap(2u);
    (void)d3d8_swap(4u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_SLOT0), 0u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_PRODUCER), 0u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_THRESHOLD), 0u);
    CHECK_EQ_U32(d3d8_flip_hardware_get().queued, 0u);

    /* Coupled effects alone do not queue: the flip model is its own opt-in. The effects' waits
     * leave the flip words alone, and the flip model without the effects queues nothing either. */
    d3d8_vblank_effects_configure(true);
    CHECK(!d3d8_flip_enabled());
    (void)d3d8_swap(2u);
    (void)d3d8_swap(4u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_SLOT0), 0u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_PRODUCER), 0u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_THRESHOLD), 0u);
    d3d8_vblank_effects_configure(false);
    d3d8_flip_configure(true);
    CHECK(d3d8_flip_enabled());
    (void)d3d8_swap(2u);
    (void)d3d8_swap(4u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_PRODUCER), 0u);
    CHECK_EQ_U32(d3d8_flip_hardware_get().queued, 0u);
    d3d8_vblank_effects_configure(true);

    /* Both on: Swap(2) queues, the wait in Swap(4) is the blank that shows it. */
    const uint32_t count = load(DEV + D3D8_FLIP_DEV_COUNT);
    (void)d3d8_swap(2u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_SLOT0), 1u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_SLOT0 + 4u), count + 1u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_SLOT0 + 8u), shown);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_PRODUCER), 1u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_LAST_TARGET), count + 1u);
    CHECK_EQ_U32(d3d8_flip_hardware_get().queued, 1u);
    CHECK_EQ_U32(d3d8_flip_hardware_get().flips, 0u);
    (void)d3d8_swap(4u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_COUNT), count + 1u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_SLOT0), 0u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_CONSUMER), 1u);
    CHECK_EQ_U32(load(D3D8_FLIP_GLOBAL_VALUE), shown);
    const d3d8_flip_hardware hardware = d3d8_flip_hardware_get();
    CHECK_EQ_U32(hardware.flips, 1u);
    CHECK_EQ_U32(hardware.display_start_writes, 1u); /* +0x1B8 was cleared above */
    CHECK_EQ_U32(hardware.display_start, shown);
    CHECK_EQ_U32(hardware.pgraph_increments, 1u);
    CHECK_EQ_U32(hardware.gamma_uploads, 1u); /* CreateDevice leaves the first ramp pending */
    printf("  ramp byte 1 of green %u, hardware.gamma[4] %u\n",
           (unsigned)d3d8_device_load32(D3D8_FLIP_DEV_GAMMA_RAMPS + 0x100u + 0u), (unsigned)hardware.gamma[4]);

    /* Steady state: every Swap pair queues and completes one flip. */
    for (uint32_t frame = 0u; frame < 6u; frame++) {
        (void)d3d8_swap(2u);
        (void)d3d8_swap(4u);
    }
    CHECK_EQ_U32(d3d8_flip_hardware_get().flips, 7u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_CONSUMER), 7u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_PRODUCER), 7u);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_SLOT0) | load(DEV + D3D8_FLIP_DEV_SLOT1), 0u);

    /* A flip callback is a refusal that writes nothing. */
    store(DEV + D3D8_FLIP_DEV_CALLBACK, 0x22020u);
    const uint32_t producer = load(DEV + D3D8_FLIP_DEV_PRODUCER);
    RUN_EXPECTING_FATAL((void)d3d8_swap(2u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(load(DEV + D3D8_FLIP_DEV_PRODUCER), producer);
    store(DEV + D3D8_FLIP_DEV_CALLBACK, 0u);
    /* Two prepares without a finish fill both slots, a third is refused (no FLIP_STALL). */
    (void)d3d8_swap(2u);
    (void)d3d8_swap(2u);
    RUN_EXPECTING_FATAL((void)d3d8_swap(2u));
    CHECK(fatal_seen);
    d3d8_vblank_effects_configure(false);
    d3d8_flip_configure(false);
    environment_end();
}

int main(void)
{
    process_one_flip();
    display_start_and_chain();
    queue_arithmetic();
    swap_data_bits();
    refusals_write_nothing();
    swap_hook();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
