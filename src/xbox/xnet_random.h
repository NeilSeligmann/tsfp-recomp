/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_XBOX_XNET_RANDOM_H
#define TSFP_XBOX_XNET_RANDOM_H
#include <stddef.h>
#include <stdint.h>

#define XNET_RANDOM_ENTRY 0x00431700u
#define XNET_CREATE_KEY_ENTRY 0x0043170Bu
#define XNET_STACK_POINTER 0x007715ECu
#define XNET_NOT_INITIALIZED 10093u
/* Original wrapper readiness, stdcall-two ABI and key layout are measured.
 * Entropy is supplied by the host OS, not an emulation of the opaque RC4 state
 * at stack+0x88. No stack object, link or network service is initialized here.
 * Failed entropy/guest transfers stop by name; a guest transfer can leave a
 * prefix, and concurrent mapping lifetime is not pinned (kernel_call.h). */
size_t xnet_random_register(void);
#endif
