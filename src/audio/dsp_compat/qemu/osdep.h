/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Standalone compatibility surface; no QEMU machine, timer or memory model. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define MIN(a,b) ((a)<(b)?(a):(b))
