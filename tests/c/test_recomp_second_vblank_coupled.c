/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T372: the two-event policy with --couple-vblank-effects. The device count is then the number of
 * completed waits applied (not the measured constants 1 and 2), the unbacked startup event adds
 * its own one, the credit-backed second event adds none (its wait already did) and only reads
 * the count. The uncoupled constants stay in tests/c/test_recomp_second_vblank.c.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "test_d3d8_support.h"
#include "d3d8_vblank_effects.h"
#include "host_runtime.h"
#include "kernel_clock.h"
#include "recomp_second_vblank.h"
#include <pthread.h>

#define DEV 0x3E3F60u
static uint32_t fs = 0x7C0000u, low = 0x7D0000u, high = 0x7D1000u, esp;
static unsigned calls;
static uint32_t expected_record;

static void host_fatal(uint32_t address, const char *text)
{
    host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED, address, 0u, text);
}
static bool confirmed(uint32_t handle, uint32_t base)
{
    CHECK_EQ_U32(handle, 2u);
    CHECK_EQ_U32(base, 0x7E0000u);
    return true;
}
static bool callback(uint32_t address, uint32_t lo, uint32_t hi, const uint32_t payload[3])
{
    CHECK_EQ_U32(address, 0x22020u);
    CHECK_EQ_U32(lo, low);
    CHECK_EQ_U32(hi, high);
    CHECK_EQ_U32(payload[0], expected_record);
    CHECK_EQ_U32(payload[1], 0u);
    CHECK_EQ_U32(payload[2], 0u);
    CHECK_EQ_U32(load(DEV + 0x1DE8u), expected_record);
    calls++;
    store(0x563918u, load(0x563918u) + 1u);
    return true;
}
static void run_poll(bool expected_stop)
{
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        recomp_second_vblank_poll(0x1538C0u, 1u, fs, esp, 0u);
        CHECK(!expected_stop);
    } else {
        CHECK(expected_stop);
    }
    host_run_disarm();
}
static void *worker(void *unused)
{
    (void)unused;
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        recomp_second_vblank_note_wait_completed(true, 2u, 0x7E0000u);
    }
    host_run_disarm();
    return NULL;
}
static void producer_wait(void)
{
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, worker, NULL) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
}
static void begin(unsigned setup_waits)
{
    d3d8_vblank_effects_configure(true);
    store(DEV + 0x1DE8u, 0u);
    store(DEV + 0x1DDCu, 0x02480104u);
    for (unsigned wait = 0u; wait < setup_waits; wait++) {
        (void)d3d8_vblank_effects_apply(0x1000u * (wait + 1u));
    }
    store(0x3E3F58u, DEV);
    store(0x563918u, 0u);
    store(esp, 0x3D455u);
    const uint32_t offsets[] = {0x1DB8u, 0x1DE4u, 0x1DECu, 0x1D9Cu, 0x1DA8u, 0x2448u, 0x244Cu};
    for (unsigned index = 0u; index < sizeof(offsets) / sizeof(offsets[0]); index++) {
        store(DEV + offsets[index], 0u);
    }
    CHECK(kernel_guest_write_u8(fs + 0x24u, 0u));
    CHECK(recomp_second_vblank_configure(true, callback, low, high, confirmed));
    recomp_second_vblank_bind_owner(1u, fs, 0x3801D9u, 0x37FE1Du);
    recomp_second_vblank_note_registration(0x22020u, 1u, fs);
    store(DEV + 0x1DB8u, 0x22020u);
}

int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    d3d8_hle_set_fatal(host_fatal);
    esp = SCRATCH_DATA + 128u;
    map_fixed(fs, 4096u);
    map_fixed(low, 4096u);
    map_fixed(0x563000u, 4096u);

    /* The measured boot: three setup waits, the startup event, the producer's wait, the credit. */
    begin(3u);
    CHECK_EQ_U32(load(DEV + 0x1DE8u), 3u);
    producer_wait(); /* a wait before the first event is discarded by the policy */
    expected_record = 4u; /* count 3 + the unbacked startup event */
    run_poll(false);
    CHECK_EQ_U32(calls, 1u);
    CHECK_EQ_U32(load(DEV + 0x1DE8u), 4u);
    store(esp, 0x3D66Du);
    (void)d3d8_vblank_effects_apply(0x9000u); /* the producer's Swap wait: count 5, applied 4 */
    producer_wait();
    CHECK_EQ_U32(load(DEV + 0x1DE8u), 5u);
    expected_record = 5u; /* delivered, never recounted */
    run_poll(false);
    CHECK_EQ_U32(calls, 2u);
    CHECK_EQ_U32(load(DEV + 0x1DE8u), 5u);
    CHECK_EQ_U32(load(0x563918u), 2u);
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));

    /* A count that is not the waits applied (the uncoupled constant 2) is refused. */
    begin(3u);
    store(DEV + 0x1DE8u, 1u);
    run_poll(true);
    CHECK_EQ_U32(calls, 2u);
    CHECK_EQ_U32(load(DEV + 0x1DE8u), 1u);
    CHECK(recomp_second_vblank_configure(false, NULL, 0u, 0u, NULL));
    d3d8_vblank_effects_configure(false);
    environment_end();
    printf("second vblank coupled: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
