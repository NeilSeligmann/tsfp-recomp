/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xonline_offline.h"
#include "kernel_call.h"
#include "xonline_hle.h"

static xonline_offline_stats state;

void xonline_offline_reset(void) { state = (xonline_offline_stats){0}; }
xonline_offline_stats xonline_offline_stats_get(void) { return state; }

bool xonline_offline_startup(uint32_t reserved, uint32_t *hresult)
{
    if (reserved != 0u || hresult == NULL || state.references == UINT32_MAX ||
        state.startups == UINT64_MAX) {
        return false;
    }
    state.startups++;
    state.references++;
    *hresult = 0u;
    return true;
}

bool xonline_offline_get_users(uint32_t users, uint32_t count, uint32_t *hresult)
{
    if (hresult == NULL) {
        return false;
    }
    state.user_queries++;
    if (state.references == 0u) {
        *hresult = XONLINE_E_NOT_INITIALIZED;
        return true;
    }
    /* Check both spans before starting the compound output. This catches NULL,
     * unmapped and guard-page pointers without clearing a valid first output when
     * the second pointer is obviously unusable. The probe proves readability only;
     * a later protection change or read-only page can still make either write fail. */
    if (kernel_guest_at(users, XONLINE_USERS_BYTES) == NULL ||
        kernel_guest_at(count, sizeof(uint32_t)) == NULL) {
        return false;
    }
    uint8_t zeros[XONLINE_USERS_BYTES] = {0};
    if (!kernel_guest_write_bytes(users, zeros, sizeof(zeros)) || !kernel_guest_write_u32(count, 0u)) {
        return false;
    }
    *hresult = 0u;
    return true;
}

uint32_t xonline_offline_cleanup(void)
{
    state.cleanups++;
    if (state.references == 0u) {
        return XONLINE_E_NOT_INITIALIZED;
    }
    state.references--;
    return 0u;
}

static uint32_t startup_handler(void *context)
{
    uint32_t reserved = 0u, hresult = 0u;
    if (!kernel_frame_arg(context, 0u, &reserved)) {
        xonline_hle_fatal(XONLINE_STARTUP_ENTRY, "XOnlineStartup argument is unreadable");
    }
    if (!xonline_offline_startup(reserved, &hresult)) {
        xonline_hle_fatal(XONLINE_STARTUP_ENTRY,
                          "XOnlineStartup reserved %#x is not modelled (the initialiser 0x413161 reads it)",
                          reserved);
    }
    return hresult;
}

static uint32_t get_users_handler(void *context)
{
    uint32_t users = 0u, count = 0u, hresult = 0u;
    if (!kernel_frame_arg(context, 0u, &users) || !kernel_frame_arg(context, 1u, &count)) {
        xonline_hle_fatal(XONLINE_GET_USERS_ENTRY, "XOnlineGetUsers arguments are unreadable");
    }
    if (!xonline_offline_get_users(users, count, &hresult)) {
        xonline_hle_fatal(XONLINE_GET_USERS_ENTRY, "XOnlineGetUsers buffer %#x or count %#x is not writable",
                          users, count);
    }
    return hresult;
}

static uint32_t cleanup_handler(void *context)
{
    (void)context;
    return xonline_offline_cleanup();
}

/* T1103: original 00413005 -> 0041A14F reads the actual state pointer first,
 * then its one stack argument and state+4. No offline title ID is invented. */
static uint32_t title_id_handler(void *context)
{
    uint32_t guest_state = 0u, title_id = 0u, current_title_id = 0u;
    if (!kernel_guest_read_u32(XONLINE_STATE_POINTER, &guest_state)) {
        xonline_hle_fatal(XONLINE_TITLE_ID_ENTRY, "XOnlineTitleIdIsSameTitle state pointer is unreadable");
    }
    if (!kernel_frame_arg(context, 0u, &title_id)) {
        xonline_hle_fatal(XONLINE_TITLE_ID_ENTRY, "XOnlineTitleIdIsSameTitle argument is unreadable");
    }
    const uint32_t field = guest_state + 4u; /* original x86 address arithmetic wraps */
    if (!kernel_guest_read_u32(field, &current_title_id)) {
        xonline_hle_fatal(XONLINE_TITLE_ID_ENTRY,
                          "XOnlineTitleIdIsSameTitle state title at %#x is unreadable", field);
    }
    return title_id == current_title_id ? 1u : 0u;
}

/* T1071: the other 26 library wrappers. Each is `mov ecx,[0x7715DC]` plus a jump or call into a
 * thiscall method of the state block. MEASURED under Unicorn (tests/test_xonline_no_state_oracle.py
 * parses this table and re-runs the original bytes with [0x7715DC] == 0): with no state block the
 * method returns 0x80150005 and writes nothing (NS_ERROR), except GetLogonUsers and FriendsGetLatest
 * which return 0 (NS_ZERO, a NULL pointer / empty result). Columns: address, name, stdcall argument
 * dwords (equal to the generated xdk_abi.inc rows). After XOnlineStartup the state block would be
 * live and its methods (logon tasks, friends, mutelist, signature checks, notifications) read
 * service state this module does not model, so those calls are refused by name rather than
 * answered with a made-up success. T1103 title equality instead reads the actual guest state;
 * a missing state still refuses its precise unreadable field, never returns a guessed result. */
#define NS_ERROR XONLINE_E_NOT_INITIALIZED
#define NS_ZERO 0u
#define XONLINE_NOSTATE_TABLE(X) \
    X(0x00412E50u, SignatureVerify, 4u, NS_ERROR) \
    X(0x00412E5Bu, SignatureVerifyGetResults, 3u, NS_ERROR) \
    X(0x00412F9Eu, TaskContinue, 1u, NS_ERROR) \
    X(0x00412FA9u, TaskClose, 1u, NS_ERROR) \
    X(0x00412FBFu, Logon, 5u, NS_ERROR) \
    X(0x00412FCEu, LogonTaskGetResults, 1u, NS_ERROR) \
    X(0x00412FD9u, GetLogonUsers, 0u, NS_ZERO) \
    X(0x00412FE4u, GetServiceInfo, 2u, NS_ERROR) \
    X(0x00412FEFu, Sub00412FEF, 4u, NS_ERROR) \
    X(0x00412FFAu, TitleUpdate, 1u, NS_ERROR) \
    X(0x00413010u, NotificationSetState, 6u, NS_ERROR) \
    X(0x00413034u, Sub00413034, 3u, NS_ERROR) \
    X(0x0041303Fu, Sub0041303F, 7u, NS_ERROR) \
    X(0x0041306Cu, FriendsStartup, 2u, NS_ERROR) \
    X(0x00413077u, FriendsEnumerate, 3u, NS_ERROR) \
    X(0x00413082u, FriendsEnumerateFinish, 1u, NS_ERROR) \
    X(0x0041308Du, FriendsGetLatest, 3u, NS_ZERO) \
    X(0x00413098u, FriendsRemove, 2u, NS_ERROR) \
    X(0x004130A3u, FriendsRequestByName, 4u, NS_ERROR) \
    X(0x004130AEu, FriendsGameInvite, 5u, NS_ERROR) \
    X(0x004130CFu, FriendsRevokeGameInvite, 5u, NS_ERROR) \
    X(0x004130F0u, FriendsAnswerRequest, 3u, NS_ERROR) \
    X(0x004130FBu, FriendsAnswerGameInvite, 3u, NS_ERROR) \
    X(0x00413106u, FriendsGetAcceptedGameInvite, 1u, NS_ERROR) \
    X(0x00413111u, MutelistGet, 6u, NS_ERROR) \
    X(0x00413120u, MutelistAdd, 4u, NS_ERROR)

static uint32_t service_call(uint32_t address, const char *name, uint32_t no_state_return)
{
    state.service_calls++;
    if (state.references != 0u) {
        xonline_hle_fatal(address,
                          "XOnline%s after XOnlineStartup: the initialised state block is not modelled "
                          "(offline only Startup, GetUsers and Cleanup are)", name);
    }
    return no_state_return;
}

#define X(address, name, args, result) \
    static uint32_t service_##name(void *context) \
    { \
        (void)context; \
        return service_call(address, #name, result); \
    }
XONLINE_NOSTATE_TABLE(X)
#undef X

size_t xonline_offline_register(void)
{
    size_t taken = (size_t)xonline_hle_register(XONLINE_STARTUP_ENTRY, startup_handler) +
                   (size_t)xonline_hle_register(XONLINE_GET_USERS_ENTRY, get_users_handler) +
                   (size_t)xonline_hle_register(XONLINE_CLEANUP_ENTRY, cleanup_handler) +
                   (size_t)xonline_hle_register(XONLINE_TITLE_ID_ENTRY, title_id_handler);
#define X(address, name, args, result) taken += (size_t)xonline_hle_register(address, service_##name);
    XONLINE_NOSTATE_TABLE(X)
#undef X
    return taken;
}
