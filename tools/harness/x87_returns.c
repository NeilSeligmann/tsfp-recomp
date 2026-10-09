/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Isolated RET validation only. Unexpected continuations cannot resume C. */
#include "x87_returns.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct expected_frame {
    uint32_t expected, call_site;
    unsigned consumed;
};
static __thread struct expected_frame frames[64];
static __thread unsigned depth;

static void refusal(const char *reason, uint32_t site, uint32_t esp,
                    uint32_t actual, uint32_t expected) {
    printf("FATAL isolated-return-infrastructure:%s site=%08X esp=%08X "
           "actual=%08X expected=%08X full-state-export=unsupported\n",
           reason, site, esp, actual, expected);
    fflush(stdout);
    _Exit(2); /* Never a guest trap, matched fault or native C continuation. */
}

void harness_x87_returns_reset(uint32_t expected_outer) {
    memset(frames, 0, sizeof(frames));
    depth = 1;
    frames[0].expected = expected_outer; /* Explicit caller contract, no default. */
}

void harness_x87_return_begin(uint32_t expected, uint32_t call_site) {
    if (!depth || depth >= 64 || frames[depth - 1].consumed)
        refusal("invalid-frame-push", call_site, 0, 0, expected);
    frames[depth].expected = expected;
    frames[depth].call_site = call_site;
    frames[depth].consumed = 0;
    ++depth;
}

void harness_x87_return_end(uint32_t expected, uint32_t call_site) {
    if (!depth) refusal("frame-underflow", call_site, 0, 0, expected);
    const struct expected_frame *frame = &frames[depth - 1];
    if (!frame->consumed || frame->expected != expected || frame->call_site != call_site)
        refusal("unvalidated-c-return", call_site, 0, frame->expected, expected);
    --depth;
    memset(&frames[depth], 0, sizeof(frames[depth]));
}

void harness_x87_return32(uint32_t *guest_esp, uint32_t immediate, uint32_t ret_site) {
    if (!depth || frames[depth - 1].consumed)
        refusal("missing-expected-frame", ret_site, *guest_esp, 0, 0);
    if (immediate) /* All six authenticated C3 sites have zero immediate. */
        refusal("unsupported-ret-immediate", ret_site, *guest_esp, immediate, 0);
    const uint32_t before = *guest_esp;
    uint32_t actual;
    /* Exactly one real m32 stack read; faults leave ESP/frame unmodified. */
    __asm__ volatile("movl (%1), %0" : "=r"(actual) : "r"((uintptr_t)before) : "memory");
    *guest_esp = before + 4u;
    __asm__ volatile("" : : : "memory");
    const uint32_t expected = frames[depth - 1].expected;
    if (actual != expected)
        refusal("unexpected-target", ret_site, *guest_esp, actual, expected);
    frames[depth - 1].consumed = 1;
}
