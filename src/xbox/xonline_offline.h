/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_XONLINE_OFFLINE_H
#define TSFP_GPU_XONLINE_OFFLINE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* T904: the XONLINE library surface the Challenge menu query (title function 0x32030) calls, for a
 * console with no network link and no Xbox Live account on its drive. INFERRED, opt-in
 * (`--xonline-offline`), xemu-level at best and never hardware: it is the original library's code
 * path for that state read from the image, NOT a measurement of a console and NOT a model of
 * sign-in or any network service. The library state lives on the host (the title never reads the
 * guest words 0x7715DC and 0x7715E0 on that offline route). T1103 title equality is a
 * separate original-grounded predicate of the actual guest state pointer 0x7715DC; this
 * offline initializer does not invent that pointer or a title ID for it.
 *
 * 0x00413496 XOnlineStartup(reserved), stdcall ret 4. The original calls WSAStartup(0x200) first,
 * which does not need a link, then allocates a state block (a second call only bumps its reference
 * count) and returns the HRESULT of its initialiser. Modelled: 0 (S_OK). Only reserved == 0 is
 * modelled: the original hands it to the initialiser (0x413161), whose reading is not lifted, so
 * another value is refused by name.
 * 0x00412FB4 XOnlineGetUsers(users, count), stdcall ret 8: a jump with ecx = the state block to
 * 0x414D37. No state: 0x80150005 and nothing written. Otherwise it zeroes 0x700 bytes at users,
 * zeroes *count, then fills one record per account it finds on the drive (none here) and returns 0.
 * 0x00413593 XOnlineCleanup(), bare ret. No state: 0x80150005. Otherwise it drops one reference,
 * frees the state at zero and returns 0. */
#define XONLINE_STARTUP_ENTRY 0x00413496u
#define XONLINE_GET_USERS_ENTRY 0x00412FB4u
#define XONLINE_CLEANUP_ENTRY 0x00413593u
#define XONLINE_TITLE_ID_ENTRY 0x00413005u
#define XONLINE_STATE_POINTER 0x007715DCu
#define XONLINE_E_NOT_INITIALIZED 0x80150005u
#define XONLINE_USERS_BYTES 0x700u

typedef struct {
    uint32_t references; /* live XOnlineStartup references, 0 means no state block */
    uint64_t startups, cleanups, user_queries, service_calls;
} xonline_offline_stats;

void xonline_offline_reset(void);
xonline_offline_stats xonline_offline_stats_get(void);
/* Returns the HRESULT. A nonzero `reserved` returns false (the caller refuses). */
bool xonline_offline_startup(uint32_t reserved, uint32_t *hresult);
/* Writes zero users and count after readable-span preflight. A failed guest write returns false;
 * writes are not atomic against read-only pages or concurrent protection changes. */
bool xonline_offline_get_users(uint32_t users, uint32_t count, uint32_t *hresult);
uint32_t xonline_offline_cleanup(void);
/* Registers 30 handlers: T904/T1071 offline wrappers and T1103 equality against
 * actual guest state. Missing/unreadable state has no fabricated equality result. */
size_t xonline_offline_register(void);
#endif
