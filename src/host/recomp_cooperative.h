/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_HOST_RECOMP_COOPERATIVE_H
#define TSFP_HOST_RECOMP_COOPERATIVE_H
#include <stdbool.h>
#include <stdint.h>
/* Trusted HOST provider, called on the same thread before a resolved guest callee.
 * Provider owns preservation of ALL modeled guest state and its own cleanup on
 * faults/stops. No host locks may span guest execution. No timing/event policy is
 * supplied here. Recursive safe points on this thread are suppressed; other guest
 * threads have independent guards and can call the same provider concurrently. */
typedef void (*recomp_cooperative_provider)(uint32_t callee,void *userdata);
/* Global configuration is quiescent: no guest/provider thread may be executing
 * or starting concurrently. Provider/userdata must outlive every possible call.
 * NULL disables the seam. Configuration from an active provider refuses without
 * changing either value. No configuration/reset reentry from provider is allowed;
 * cross-thread violations are outside this contract, not synchronized internally. */
bool recomp_cooperative_configure(recomp_cooperative_provider provider,void *userdata);
/* Disabled calls change nothing and need no armed root. An enabled call requires
 * host_run_arm on THIS thread; violation aborts rather than invoking a provider
 * without a valid stop target. Inner scopes clear the guard before propagating
 * unchanged diagnostic fields to the caller's outer scope/root. */
void recomp_call_safepoint(uint32_t callee);
/* Pure weak compiled-capability query: exactly1, absent/0/non1 false. This does
 * not configure a provider, look up code, access guest state or advance counters. */
bool recomp_cooperative_ready(void);
#endif
