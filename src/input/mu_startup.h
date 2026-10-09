/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_MU_STARTUP_H
#define TSFP_MU_STARTUP_H
#include <stdbool.h>
#include <stddef.h>
#define MU_STARTUP_MAX 8u
typedef struct { unsigned port, slot; const char *path; } mu_startup_image;
/* Explicit writable EXISTING images only. No creation/formatting; callers must
 * run after guest memory adoption and before guest initialization. On refusal,
 * detach only attachments made by this call, preserving preexisting units. */
bool mu_startup_attach(const mu_startup_image *images, size_t count, char *error, size_t error_size);
#endif
