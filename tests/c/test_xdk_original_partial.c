/* SPDX-License-Identifier: GPL-3.0-or-later
 * Link fixture: omit one real weak capability symbol, not just a NULL row. */
#include "xdk_original.h"
#include "recomp_abi.h"
#include <stdio.h>
#ifndef XDK_ORIGINAL_MISSING_SYMBOL
#error "define XDK_ORIGINAL_MISSING_SYMBOL as 1 (lookup), 2 (version), or 3 (identity)"
#endif
#ifndef XDK_ORIGINAL_FIXTURE_VERSION
#define XDK_ORIGINAL_FIXTURE_VERSION 1
#endif
#if XDK_ORIGINAL_MISSING_SYMBOL != 1
static void fixture_body(void) { }
recomp_func_t recomp_lookup_original(uint32_t address)
{
    (void)address;
    return fixture_body;
}
#endif
#if XDK_ORIGINAL_MISSING_SYMBOL != 2
uint32_t recomp_original_profile_version(void) { return XDK_ORIGINAL_FIXTURE_VERSION; }
#endif
#if XDK_ORIGINAL_MISSING_SYMBOL != 3
const char *recomp_original_profile_identity(void)
{
#if XDK_ORIGINAL_FIXTURE_VERSION == 2
    return "shader-assembler-v2";
#else
    return "shader-assembler-v1";
#endif
}
#endif
int main(void)
{
    unsigned failures = 0u;
    if (xdk_original_ready()) { failures++; }
    if (xdk_original_configure(true)) { failures++; }
    if (!xdk_original_configure(false)) { failures++; }
    if (xdk_original_dispatch(0x3EE2B3u)) { failures++; }
    printf("partial original capability: 4 checks, %u failures\n", failures);
    return failures ? 1 : 0;
}
