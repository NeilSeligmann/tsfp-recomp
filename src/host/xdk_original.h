/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_HOST_XDK_ORIGINAL_H
#define TSFP_HOST_XDK_ORIGINAL_H
#include <stdbool.h>
#include <stdint.h>

/* Versioned CPU shader-assembler profiles, independent of HLE implementation counts.
 * Queries come from trusted compiled generation, not guest pointers or runtime
 * registrations. The compiled queries must be pure/nonreentrant and return
 * live compiled definitions; arbitrary hostile function/data pointers are outside
 * this contract. Version/identity bind the generator contract, not cryptographic
 * attestation of machine code. Missing/partial/incorrect capability fails closed. */
/* Version1/identity shader-assembler-v1 requires exactly the original five CPU
 * entries. Version2 adds the three verified text destructor helpers. Version3 adds
 * the original 0x003EFEA7 global-delete wrapper; it routes only with its exact v3
 * identity and complete nine-entry compiled profile. Older profile boundaries remain
 * exact. Versions4/5/6 require ten/eleven/twelve entries. Version7 adds
 * original 0x003F17AE recursive cleanup and requires all thirteen entries with
 * exact shader-assembler-v7 identity. Version8 adds original003F1786/003F17D4
 * cleanup variants and requires exactly fifteen compiled entries. */
bool xdk_original_ready(void);

/* Default disabled. Setup/teardown is quiescent: no concurrent configuration or
 * capability-query mutation. Enabling snapshots exactly v1 five, v2 eight or v3 nine
 * compiled pointers (or v4 ten/v5 eleven/v6 twelve/v7 thirteen/v8 fifteen) only after
 * complete validation. Failure preserves the previous configuration.
 * Changes while any original executes (including nested calls) are refused.
 * Disabling needs no compiled capability. No module/ABI/coverage state changes. */
bool xdk_original_configure(bool enabled);

/* False for disabled/nonprofile addresses, without guest-state changes. True
 * after the selected original returned; it owns ALL modeled ABI/register/stack
 * effects. Caller must arm host_run first. Nested calls are allowed. Stops/faults
 * release active bookkeeping and rethrow unchanged; guest state is NOT restored.
 * The configuration lock never spans original execution. A separate lock does, for
 * every entry except the read-only 0x003E6714 (the compiler mutates static data, so two
 * guest threads inside it at once is unsafe, see xdk_original.c). It is held per thread
 * across nested calls and released on return, stop and fault. A call waiting for it
 * counts as active, so reconfiguration is refused meanwhile. Host pthread cancellation
 * is unsupported. */
bool xdk_original_dispatch(uint32_t address);
#endif
