/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T952: additional real callback/unregistration controls; no SDL or hardware. */
#define _POSIX_C_SOURCE 200809L
#include "test_d3d8_support.h"
#include "xinput_devices.h"
#include "xinput_hle.h"
#include "xinput_source.h"
#include <errno.h>
#include <pthread.h>
#include <time.h>

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool inside, release_callback, unregister_started, unregistered;
static unsigned calls, cancellations;
static bool feedback_case;
static bool blocking_source(uint64_t index, unsigned port, xinput_pad_state *out, void *user)
{
    (void)index; (void)port; (void)user;
    pthread_mutex_lock(&gate);
    ++calls; inside = true; pthread_cond_broadcast(&changed);
    while (!release_callback) pthread_cond_wait(&changed, &gate);
    pthread_mutex_unlock(&gate);
    memset(out, 0, sizeof *out);
    return true;
}
static bool blocking_feedback(unsigned port, uint16_t left, uint16_t right, void *user)
{
    (void)port; (void)user;
    if (left == 0 && right == 0) { ++cancellations; return true; }
    xinput_pad_state ignored;
    return blocking_source(0, port, &ignored, NULL);
}
static void *invoke(void *unused)
{
    (void)unused;
    if (feedback_case) (void)xinput_feedback_send(2, 300, 700);
    else (void)xinput_source_poll_port(2);
    return NULL;
}
static void *unregister_source(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&gate);
    unregister_started = true; pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
    if (feedback_case) xinput_feedback_install(NULL, NULL);
    else xinput_source_install_ports(NULL, NULL);
    pthread_mutex_lock(&gate);
    unregistered = true; pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
    return NULL;
}
static void test_unregistration(bool feedback)
{
    inside = release_callback = unregister_started = unregistered = false;
    calls = cancellations = 0;
    feedback_case = feedback;
    if (feedback) xinput_feedback_install(blocking_feedback, NULL);
    else xinput_source_install_ports(blocking_source, NULL);
    pthread_t poller, remover;
    CHECK(pthread_create(&poller, NULL, invoke, NULL) == 0);
    pthread_mutex_lock(&gate);
    while (!inside) pthread_cond_wait(&changed, &gate);
    pthread_mutex_unlock(&gate);
    CHECK(pthread_create(&remover, NULL, unregister_source, NULL) == 0);
    pthread_mutex_lock(&gate);
    while (!unregister_started) pthread_cond_wait(&changed, &gate);
    struct timespec until;
    CHECK(clock_gettime(CLOCK_REALTIME, &until) == 0);
    until.tv_nsec += 100000000L;
    if (until.tv_nsec >= 1000000000L) { ++until.tv_sec; until.tv_nsec -= 1000000000L; }
    int rc = 0;
    while (!unregistered && rc == 0) rc = pthread_cond_timedwait(&changed, &gate, &until);
    CHECK(rc == ETIMEDOUT);
    CHECK(!unregistered);
    release_callback = true; pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
    CHECK(pthread_join(poller, NULL) == 0);
    CHECK(pthread_join(remover, NULL) == 0);
    CHECK(unregistered && calls == 1);
    if (feedback) {
        CHECK(cancellations == 4);
        CHECK(!xinput_feedback_send(2, 10, 20));
    } else CHECK(!xinput_source_poll_port(2));
    CHECK(calls == 1);
}
static unsigned motor_left[4], motor_right[4];
static bool record_motors(unsigned port, uint16_t left, uint16_t right, void *unused)
{
    (void)unused; motor_left[port] = left; motor_right[port] = right; return true;
}
static bool remove_while_polled(uint64_t index, unsigned port, xinput_pad_state *out, void *unused)
{
    (void)index; (void)unused;
    memset(out, 0xA5, sizeof *out);
    CHECK(xinput_pad_disconnect(port));
    CHECK(xinput_pad_connect(port));
    return true;
}
static void test_replacement_and_recursive_removal(void)
{
    const uint32_t type = 0x46C75Cu, out = SCRATCH_DATA + 0x100u, fb = SCRATCH_DATA + 0x200u;
    const uint32_t declarations[8] = {0x46C6E0u,8u,0x46C8A0u,4u,0x46C894u,4u,0x46C75Cu,4u};
    map_fixed(0x46C000u, 0x1000u); map_fixed(0x771000u, 0x1000u);
    memcpy(kernel_guest_at(SCRATCH_DATA, sizeof declarations), declarations, sizeof declarations);
    CHECK(xinput_pad_connect(1));
    CHECK(xinput_devices_init_empty(4, SCRATCH_DATA) == 0);
    uint32_t old = xinput_pad_open(type, 1, 0, 0);
    CHECK(old != 0);
    xinput_source_install_ports(remove_while_polled, NULL);
    CHECK(xinput_pad_state_read(old, out) == 0x48Fu);
    for (unsigned i = 0; i < 22; ++i) CHECK(((unsigned char *)kernel_guest_at(out, 22))[i] == 0);
    xinput_source_install_ports(NULL, NULL);
    uint32_t replacement = xinput_pad_open(type, 1, 0, 0);
    CHECK(replacement && replacement != old);
    xinput_feedback_install(record_motors, NULL);
    memset(kernel_guest_at(fb, 0x46u), 0, 0x46u);
    uint16_t strengths[2] = {1234, 5678};
    memcpy(kernel_guest_at(fb + 0x42u, 4), strengths, 4);
    CHECK(xinput_pad_feedback(replacement, fb) == 997u);
    xinput_pad_close(old);
    CHECK(motor_left[1] == 1234 && motor_right[1] == 5678);
    CHECK(*(uint32_t *)kernel_guest_at(fb, 4) == 997u);
    CHECK(xinput_pad_disconnect(1));
    CHECK(*(uint32_t *)kernel_guest_at(fb, 4) == 0x1Fu);
    CHECK(motor_left[1] == 0 && motor_right[1] == 0);
    xinput_pad_close(replacement);
    xinput_feedback_install(NULL, NULL);
}
int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    xinput_hle_init(); xinput_devices_reset(); xinput_source_reset();
    xinput_devices_enable_synthetic_pad(true); xinput_devices_enable_multiport(true);
    CHECK(xinput_pad_connect(2));
    test_unregistration(false); test_unregistration(true);
    test_replacement_and_recursive_removal();
    xinput_devices_reset(); xinput_source_reset();
    xinput_devices_enable_multiport(false); xinput_devices_enable_synthetic_pad(false);
    environment_end();
    printf("T952 lifetime: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
