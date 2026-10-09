/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xdk_original.h"
#include "host_runtime.h"
#include "recomp_abi.h"
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

extern recomp_func_t recomp_lookup_original(uint32_t address) __attribute__((weak));
extern uint32_t recomp_original_profile_version(void) __attribute__((weak));
extern const char *recomp_original_profile_identity(void) __attribute__((weak));

static const uint32_t profile[] = {
    0x003E6714u, 0x003EA301u, 0x003EE2B3u, 0x003F1791u, 0x003F42A0u,
    0x003FC8A8u, 0x00402BCFu, 0x00402C5Cu, 0x003EFEA7u, 0x003F22B4u, 0x003F36F1u,
    0x00400D98u, 0x003F17AEu, 0x003F1786u, 0x003F17D4u,
};
#define PROFILE_COUNT (sizeof(profile) / sizeof(profile[0]))
static pthread_mutex_t configuration_lock = PTHREAD_MUTEX_INITIALIZER;
static bool configured;
static size_t configured_count;
static recomp_func_t functions[PROFILE_COUNT];
static size_t active_calls;

/* The compiler keeps mutable state in the image's static data (measured, T200: the
 * compile and the member destructor write 0x405B00..0x405C1C and 0x4063A0.., and the
 * destructor works on the shared container at 0x405B18). Two guest threads inside it at
 * once hang, throw or fault (measured with the probe in tests/c/shader_compiler_probe.c).
 * The title never overlaps calls, so one host lock around every entry that can touch it
 * keeps that true for any thread schedule. It is recursive per thread because the
 * bodies call each other through the dispatcher. 0x3E6714 is three instructions that
 * read one field of the caller's own object, so it stays lock free. */
#define READ_ONLY_ENTRY 0x003E6714u
static pthread_mutex_t serial_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local size_t serial_depth;

static bool collect_profile(recomp_func_t *out, size_t *count_out)
{
    if (recomp_lookup_original == NULL || recomp_original_profile_version == NULL ||
        recomp_original_profile_identity == NULL) {
        return false;
    }
    const uint32_t version = recomp_original_profile_version();
    const char *expected_identity;
    size_t count;
    if (version == 1u) {
        expected_identity = "shader-assembler-v1";
        count = 5u;
    } else if (version == 2u) {
        expected_identity = "shader-assembler-v2";
        count = 8u;
    } else if (version == 3u) {
        expected_identity = "shader-assembler-v3";
        count = 9u;
    } else if (version == 4u) {
        expected_identity = "shader-assembler-v4";
        count = 10u;
    } else if (version == 5u) {
        expected_identity = "shader-assembler-v5";
        count = 11u;
    } else if (version == 6u) {
        expected_identity = "shader-assembler-v6";
        count = 12u;
    } else if (version == 7u) {
        expected_identity = "shader-assembler-v7";
        count = 13u;
    } else if (version == 8u) {
        expected_identity = "shader-assembler-v8";
        count = PROFILE_COUNT;
    } else {
        return false;
    }
    const char *identity = recomp_original_profile_identity();
    if (identity == NULL || strcmp(identity, expected_identity) != 0) {
        return false;
    }
    for (size_t i = 0u; i < count; i++) {
        const recomp_func_t function = recomp_lookup_original(profile[i]);
        if (function == NULL) {
            return false;
        }
        if (out != NULL) {
            out[i] = function;
        }
    }
    if (count_out != NULL) {
        *count_out = count;
    }
    return true;
}

bool xdk_original_ready(void)
{
    return collect_profile(NULL, NULL);
}

bool xdk_original_configure(bool enabled)
{
    recomp_func_t selected[PROFILE_COUNT] = {0};
    size_t selected_count = 0u;
    pthread_mutex_lock(&configuration_lock);
    if (active_calls != 0u || (enabled && !collect_profile(selected, &selected_count))) {
        pthread_mutex_unlock(&configuration_lock);
        return false;
    }
    if (enabled) {
        memcpy(functions, selected, sizeof(functions));
    } else {
        memset(functions, 0, sizeof(functions));
    }
    configured_count = enabled ? selected_count : 0u;
    configured = enabled;
    pthread_mutex_unlock(&configuration_lock);
    return true;
}

static void serial_enter(void)
{
    if (serial_depth == 0u) {
        pthread_mutex_lock(&serial_lock);
    }
    serial_depth++;
}

static void release_call(bool serialized)
{
    if (serialized) {
        if (serial_depth == 0u) {
            abort();
        }
        serial_depth--;
        if (serial_depth == 0u) {
            pthread_mutex_unlock(&serial_lock);
        }
    }
    pthread_mutex_lock(&configuration_lock);
    if (active_calls == 0u) {
        abort();
    }
    active_calls--;
    pthread_mutex_unlock(&configuration_lock);
}

bool xdk_original_dispatch(uint32_t address)
{
    size_t index = 0u;
    while (index < PROFILE_COUNT && profile[index] != address) {
        index++;
    }
    if (index == PROFILE_COUNT) {
        return false;
    }
    pthread_mutex_lock(&configuration_lock);
    if (!configured || index >= configured_count) {
        pthread_mutex_unlock(&configuration_lock);
        return false;
    }
    if (active_calls == SIZE_MAX) {
        pthread_mutex_unlock(&configuration_lock);
        abort();
    }
    const recomp_func_t function = functions[index];
    active_calls++;
    pthread_mutex_unlock(&configuration_lock);

    const bool serialized = address != READ_ONLY_ENTRY;
    if (serialized) {
        serial_enter();
    }
    if (!host_run_armed()) {
        release_call(serialized);
        abort();
    }
    volatile host_run_scope scope = HOST_RUN_SCOPE_INITIALIZER;
    if (!host_run_scope_init(&scope)) {
        release_call(serialized);
        abort();
    }
    if (sigsetjmp(*host_run_scope_jmp(&scope), 0) == 0) {
        if (!host_run_scope_push(&scope)) {
            release_call(serialized);
            host_run_stop(HOST_STOP_UNIMPLEMENTED, address, 0u,
                          "original XDK stop scope unavailable");
            abort();
        }
        function();
        if (!host_run_scope_pop(&scope)) {
            abort();
        }
        release_call(serialized);
        return true;
    }
    if (!host_run_scope_pop(&scope)) {
        abort();
    }
    release_call(serialized);
    host_run_rethrow(host_run_result());
}
