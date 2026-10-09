/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Isolated expected-return frames. No driver capability or engine handoff. */
#ifndef HARNESS_X87_RETURNS_H
#define HARNESS_X87_RETURNS_H
#include <stdint.h>

void harness_x87_returns_reset(uint32_t expected_outer);
void harness_x87_return_begin(uint32_t expected, uint32_t call_site);
void harness_x87_return_end(uint32_t expected, uint32_t call_site);
void harness_x87_return32(uint32_t *guest_esp, uint32_t immediate, uint32_t ret_site);
#endif
