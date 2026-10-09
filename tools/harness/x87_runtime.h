/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HARNESS_ISOLATED_X87_RUNTIME_H
#define HARNESS_ISOLATED_X87_RUNTIME_H
#include <stdint.h>
/* Only an authenticated isolated lift may call these. No driver capability. */
int harness_x87_reset(const unsigned char state[86]);
void harness_x87_copy(unsigned char state[86]);
void harness_x87_fld32(uint32_t address);
void harness_x87_fmul32(uint32_t address);
void harness_x87_fstp32(uint32_t address);
/* Need x87_native_ext.c (operations 4..6): FXCH st(1), one FPREM iteration, FSTP st(1). */
void harness_x87_fxch1(void);
void harness_x87_fprem(void);
void harness_x87_fstp1(void);
#endif
