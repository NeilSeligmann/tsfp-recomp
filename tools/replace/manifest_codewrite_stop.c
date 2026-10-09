/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1508: the registry-only manifest binary never executes a guest adapter. The code-write proof
 * build references the driver's stop hook, so the manifest link supplies a trapping definition. */
#include <stdint.h>

void harness_codewrite_stop(unsigned kind, uint32_t eip, uint32_t steps) __attribute__((noreturn));
void harness_codewrite_stop(unsigned kind, uint32_t eip, uint32_t steps)
{
    (void)kind;
    (void)eip;
    (void)steps;
    __builtin_trap();
}
