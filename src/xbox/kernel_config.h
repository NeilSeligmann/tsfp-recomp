/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The console's persisted configuration: ExQueryNonVolatileSetting (ordinal 24).
 *
 * ORDINAL NUMBER, RESOLVED NOT RECALLED. 24 is ExQueryNonVolatileSetting, read out
 * of `tools/kernel_ordinals.py`. It is not in that file's `SUSPECT_ON_XDK_5849`
 * list.
 *
 * WHY IT IS NEXT. It is where the bring-up run stopped once ordinal 47 was
 * implemented: thread 2 reaches it at guest 0x00381B71, after ordinals 255, 187,
 * 277, 294, 47, 107 and 113.
 *
 * ARITY: 5 STACK ARGUMENTS, AND THE MEASURED TABLE HAS NO ROW AT ALL FOR IT.
 *
 *     NTSTATUS __stdcall ExQueryNonVolatileSetting(
 *             ULONG ValueIndex, ULONG *Type, PVOID Value, ULONG ValueLength,
 *             ULONG *ResultLength);
 *
 * `generated/retail/ordinal_callsites.json` reports ONE call site and
 * `kernel_arity.inc` carries no entry, classifying 24 as a DATA export instead. Both
 * are artifacts of how the guest reaches it: every call goes through the import jump
 * stub `sub_0038486A`, whose entire body is `jmp [0x4757EC]`. `callsites.py` looks
 * for the thunk slot as an indirect CALL target inside a bracketed call site, and a
 * tail `jmp` is neither, so the 12 real call sites -- which are direct
 * `call sub_0038486A` -- are invisible to it. This is the same blind spot that hides
 * 2 of ordinal 47's sites, in a more severe form: here it hides all of them.
 *
 * So the count was taken by hand at all 12 sites, and all 12 push exactly 5
 * arguments. Listed with the ValueIndex each passes, because the literals are what
 * make the argument ORDER certain:
 *
 *     return addr   ValueIndex  ValueLength  ResultLength
 *     0x0037D3D4    7           4            NULL
 *     0x0037D477    8           4            NULL
 *     0x0037D4C3    0x103       4            NULL
 *     0x0037D4EC    0x104       4            NULL
 *     0x00381B71    forwarded   forwarded    forwarded   (sub_00381B5A, 5 params)
 *     0x003820F0    0xA         4            NULL
 *     0x003D12A0    9           4            NULL
 *     0x003D12E8    0x11        4            NULL
 *     0x0041344A    0x100       0xC          &param
 *     0x00415CCE    0x102       0x10         &local
 *     0x0043AE47    0x101       6            &local
 *     0x00441690    0xFFFF      0x100        aliased
 *
 * TWO IDIOMS HAD TO BE DISCOUNTED TO GET 5, and both are why a naive push count
 * reads 6 or 7 here:
 *   - `push ecx` with no matching pop, twice at several sites, is the compiler
 *     RESERVING two 4-byte locals -- their addresses are then taken with `lea
 *     eax, [ebp-4]` / `[ebp-8]` and passed as Type and Value.
 *   - `push 0x10` / `pop esi` is `mov esi, 0x10`, not an argument. Likewise
 *     `push 4` / `pop ecx`.
 *   - and at 0x0041344A a leading `push esi` is a register SAVE, balanced by a
 *     `pop esi` at 0x00413450 after the call.
 * Counting pushes without discounting these is exactly the over-count that
 * `tools/lift/callsites.py` documents, in three new flavours.
 *
 * `ResultLength` IS OPTIONAL, ON THE EVIDENCE: the six 4-byte sites pass a literal
 * 0 for it. A handler that dereferenced it unconditionally would fault on the first
 * call the guest makes. This is derived from the call sites, not assumed.
 *
 * WHAT THE GUEST DOES WITH THE ANSWER, where it is visible. At 0x0037D3BC the
 * 4-byte Value is read back and compared `<= 9`; the function returns 1 above that
 * and 0 at or below, and on a failing status returns 0. `Type` is written by the
 * real kernel but is NOT READ at that site -- so the type value this module reports
 * is unconstrained by any measurement, and is labelled as such below rather than
 * presented as derived.
 *
 * ================= THE SETTINGS THEMSELVES ARE NOT INVENTED =================
 *
 * These settings live in the console's EEPROM: region, language, A/V flags, clock
 * settings and so on. We do not have them, and making them up would be worse than
 * useless -- a fabricated language or video mode sends the engine down a
 * configuration path that looks deliberate and is not, and the resulting divergence
 * would be attributed to the lift rather than to this file.
 *
 * So the store here starts EMPTY, and what happens to an index with no stored value
 * is an explicit, switchable POLICY rather than a quiet default:
 *
 *   KERNEL_CONFIG_UNKNOWN_ZEROS   -- succeed, zero-fill, and SAY SO once per index.
 *       The bring-up default, because it is the policy that keeps the trace moving
 *       and the guest has a defined response to a zero. Every use is reported with
 *       the word FABRICATED, following the precedent of the host's --stub-status
 *       banner: a made-up value that announces itself is a hint, one that does not
 *       is a lie.
 *   KERNEL_CONFIG_UNKNOWN_FAIL    -- return a failure status and write nothing.
 *       Honest in a different direction, and the measured call sites all have an
 *       error arm. Kept switchable because which of the two gets further is an
 *       empirical question about this title, and the answer belongs in a run log
 *       rather than in a guess made here.
 *
 * `kernel_config_set_setting()` is how a later task supplies a REAL value once it
 * has one: derived from a dump, or from the guest's own default path. Nothing in
 * this module has to change for that.
 */

#ifndef TSFP_XBOX_KERNEL_CONFIG_H
#define TSFP_XBOX_KERNEL_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "kernel_hle.h"

/*
 * Two NT status codes this module needs that `src/xbox/nt_status.h` does not carry.
 * Defined here rather than added there because that header is outside this task's
 * ownership. They are the published NT values, and they are the ones the real
 * export returns for these two conditions.
 */
#define KERNEL_CONFIG_STATUS_BUFFER_TOO_SMALL 0xC0000023u
#define KERNEL_CONFIG_STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034u

/* The largest setting this module will hold or hand back. 0x100 is the largest
 * ValueLength any measured call site asks for (at 0xFFFF), so a bound below it
 * would turn a legitimate query into a buffer error. */
#define KERNEL_CONFIG_VALUE_MAX 0x100u

/** What to do with an index that has no stored value. See the header comment. */
typedef enum {
    KERNEL_CONFIG_UNKNOWN_ZEROS = 0,
    KERNEL_CONFIG_UNKNOWN_FAIL,
} kernel_config_unknown_policy;

/** Register the configuration ordinals with the HLE dispatcher. Returns how many. */
unsigned kernel_config_register(void);

/** Drop every stored setting, reset the counters and restore the default policy. */
void kernel_config_reset(void);

/** Choose what an index with no stored value does. */
void kernel_config_set_unknown_policy(kernel_config_unknown_policy policy);

/**
 * Supply a real value for `index`.
 *
 * `bytes` is copied, so the caller keeps ownership. False for a length above
 * `KERNEL_CONFIG_VALUE_MAX`, a null `bytes` with a nonzero length, or a full table.
 * This is the seam a later task uses to install derived settings; no handler code
 * changes when it does.
 */
bool kernel_config_set_setting(uint32_t index, const void *bytes, uint32_t length);

/* Actual source-backed256-byte EEPROM: atomically publish FFFF and MAC101 at
 * bytes64:70. Their query capacity/order follows measured Complex4627 branches.
 * Does not decrypt/publish XboxHDKey, manufacture configuration or load files. */
bool kernel_config_set_eeprom(const void *bytes, uint32_t length);
bool kernel_config_eeprom_available(void);
/* Authenticate actual EEPROM/key before atomically publishing source321/323.
 * Raw install/reset removes key availability; failure leaves old sources intact. */
bool kernel_config_set_eeprom_keyed(const void *bytes, uint32_t length,
                                    const void *key, uint32_t key_bytes);
bool kernel_config_eeprom_key(unsigned ordinal, uint8_t output[16]);

/** How many settings are stored. */
unsigned kernel_config_setting_count(void);

/**
 * How many queries were answered with a FABRICATED zero value.
 *
 * A nonzero count is a statement about how much of the run rests on data we do not
 * have, and it is reported at the end of a run for exactly that reason. It is not a
 * failure; it is a measurement of how far the fabrication reaches.
 */
unsigned kernel_config_fabricated_count(void);

/** How many queries were refused because the index had no value and the policy is FAIL. */
unsigned kernel_config_refused_count(void);

/** How many queries were answered out of the store. */
unsigned kernel_config_served_count(void);

/**
 * The distinct ValueIndex values this run has queried, oldest first.
 *
 * The point of a bring-up run is to learn what the guest asks for, so the indices
 * are recorded rather than merely counted. Writes up to `capacity` entries and
 * returns how many distinct indices there are, which may exceed `capacity`.
 */
unsigned kernel_config_queried_indices(uint32_t *out, unsigned capacity);

/* Query through a real kernel_call_frame using only supplied settings. A missing
 * value takes the existing FAIL-policy arm for this request; it never changes
 * the session default or supplies fabricated zeros. Used by original XNET seed
 * collection; all normal query writes/status/counters and alias order are shared. */
uint32_t kernel_config_query_stored(void *context);

#endif /* TSFP_XBOX_KERNEL_CONFIG_H */
