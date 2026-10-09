/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "kernel_event_handle.h"

#include <stddef.h>

#include "guest_structs.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "kernel_object.h"
#include "kernel_sync.h"
#include "nt_status.h"

/* Original wrapper 0x37FF30 pushes InitialState, (ManualReset == 0),
 * ObjectAttributes, and HandleOut at 0x37FF50..0x37FF61, so ManualReset 0 is
 * NtCreateEvent Type 1 (SynchronizationEvent, auto reset) and ManualReset 1 is
 * Type 0 (NotificationEvent, manual reset). Its return address is 0x37FF67.
 * BOOLEAN uses its low byte; upper bits are not part of InitialState.
 *
 * T263 MEASURED (it corrects T142 "exactly one caller" and T8g "five"): the
 * wrapper has SIX direct callers, 0 tail jumps, 0 stored pointers, and ordinal
 * 189's thunk slot 0x475898 is referenced only at 0x37FF61 inside it. Run on
 * their pushed arguments, five give NtCreateEvent(out, NULL, 1, 0):
 * 0x291FD [.text], and four in XONLINE (0x4194A3, 0x41A3F5, 0x41A776,
 * 0x424F02). The sixth, XNET 0x431D5F (thunk 0x431D58, called from 0x3BCF0E,
 * which stores the handle at [ebp+0x54]), passes bManualReset 1 and gives
 * NtCreateEvent(out, NULL, 0, 0), a Type 0 manual-reset event, initially clear.
 * T270 models that: Type 0 is created like Type 1, and the wait side
 * (kernel_object_event_try_wait) keeps a Type 0 event signaled. Whether any of
 * the six runs in a boot is NOT DERIVABLE from the image (docs/shader-inputs.md
 * section 12 and T270 in docs/tasks.md carry the reachability measurement).
 *
 * The wrapper's NAMED branch (name != NULL at 0x37FF36) builds its
 * OBJECT_ATTRIBUTES through the shared helper 0x381B7C, which stores root
 * 0xFFFFFFFC, the name pointer, attributes 0x80 -- and that branch is
 * STATICALLY DEAD: all six callers push a NULL name (T263). The duplicate-create
 * mapping downstream (status 0x40000000 to last-error 0xB7 at
 * 0x37FF6B..0x37FF77) is only reachable through the dead branch. So a non-NULL
 * OBJECT_ATTRIBUTES here is refused, with the same report discipline as the
 * named-mutant refusal: no name-keyed namespace is modelled because no in-image
 * call site can present a name to this ordinal.
 */
static uint32_t create_event(void *context)
{
    const kernel_call_frame *frame = context;
    uint32_t args[4];
    for (unsigned i = 0u; i < 4u; i++) {
        if (!kernel_frame_arg(frame, i, &args[i])) {
            return STATUS_INVALID_PARAMETER;
        }
    }
    if (args[1] != 0u) {
        uint32_t root_directory = 0u;
        uint32_t object_name = 0u;
        uint32_t attributes = 0u;
        if (!kernel_guest_read_u32(kernel_guest_add(args[1],
                                                    (uint32_t)offsetof(guest_object_attributes,
                                                                       root_directory)),
                                   &root_directory) ||
            !kernel_guest_read_u32(kernel_guest_add(args[1],
                                                    (uint32_t)offsetof(guest_object_attributes,
                                                                       object_name)),
                                   &object_name) ||
            !kernel_guest_read_u32(kernel_guest_add(args[1],
                                                    (uint32_t)offsetof(guest_object_attributes,
                                                                       attributes)),
                                   &attributes)) {
            kernel_hle_log()("kernel: NtCreateEvent could not read the OBJECT_ATTRIBUTES "
                             "at %#x\n",
                             (unsigned)args[1]);
            return STATUS_INVALID_PARAMETER;
        }
        /* Unlike NtCreateMutant, no OBJECT_ATTRIBUTES shape at all is accepted here.
         * T263 MEASURED all six wrapper callers (T270): every one reaches this ordinal with
         * a NULL ObjectAttributes, so the only builder of a non-NULL block is the wrapper's
         * dead named branch (helper 0x381B7C, NAMED shape only). A non-NULL block of any
         * shape, named or unnamed-with-attributes, is outside the measured contract. */
        kernel_hle_log()("kernel: NtCreateEvent with a NAMED-path OBJECT_ATTRIBUTES "
                         "%#x (root %#x, name %#x, attributes %#x) is NOT IMPLEMENTED -- "
                         "the named branch is statically dead in this image and no "
                         "object namespace exists, refusing rather than creating an "
                         "unnamed one\n",
                         (unsigned)args[1], (unsigned)root_directory,
                         (unsigned)object_name, (unsigned)attributes);
        return STATUS_NOT_IMPLEMENTED;
    }
    if (kernel_sync_current_irql() != KERNEL_IRQL_PASSIVE ||
        (args[2] != 0u && args[2] != 1u) || (args[3] & 0xFFu) != 0u) {
        kernel_hle_log()("kernel: NtCreateEvent REFUSED: only unnamed Type 0 or 1, "
                         "initially clear at PASSIVE_LEVEL is supported\n");
        return STATUS_NOT_IMPLEMENTED;
    }
    return kernel_object_create_event(args[2], args[3], args[0]);
}

/* Ordinal 225 NtSetEvent(HANDLE EventHandle, PLONG PreviousState OPTIONAL), stdcall, TWO
 * arguments (nxdk NtSetEvent@8). MEASURED at all four sites by reading the disassembly:
 *
 *     .text  0x0037FF97  XAPI SetEvent wrapper: push 0; push [esp+8]; call [0x4758A0]; the
 *                        result is tested with `jl` (negative -> its error mapper 0x37E9FD,
 *                        return FALSE) and a non-negative result returns TRUE
 *     DSOUND 0x0040B27E  push 0; push eax;        call [slot]   result unused
 *     DSOUND 0x0040E3B3  push 0; push [eax+4];    call [slot]   result unused
 *     DSOUND 0x0040E40C  push 0; push [eax-4];    call [slot]   result unused
 *
 * The last pushed is the handle and the first pushed is PreviousState, the literal 0 at
 * EVERY site. So a NULL PreviousState is the only measured mode, and a non-NULL one is
 * REFUSED loudly with STATUS_NOT_IMPLEMENTED and no state change, as NtCreateEvent refuses
 * its unmeasured modes. Only the sign of the status is observed, and only by SetEvent.
 *
 * INFERRED semantics (the NT contract): a live EVENT handle becomes signaled, success; a
 * dead handle is STATUS_INVALID_HANDLE; a live handle of another kind is
 * STATUS_OBJECT_TYPE_MISMATCH (ObReferenceObjectByHandle against ExEventObjectType). The
 * recorded state is consumed by NtWaitForSingleObject(Ex) (T8g, kernel_thread.c): a signaled
 * event satisfies a wait and a Type 1 (auto-reset) event is cleared by it, while a Type 0
 * (manual-reset, T270) event stays signaled. NtClearEvent (186) and NtPulseEvent (205) have no
 * handler, so nothing resets a signaled Type 0 event. No waiter is ever parked (a wait that
 * would block is refused), so there is no waiter to release here. */
static uint32_t set_event(void *context)
{
    const kernel_call_frame *frame = context;
    uint32_t args[2];
    if (frame == NULL) {
        kernel_hle_log()("kernel: NtSetEvent called with no argument frame\n");
        return STATUS_INVALID_PARAMETER;
    }
    for (unsigned i = 0u; i < 2u; i++) {
        if (!kernel_frame_arg(frame, i, &args[i])) {
            kernel_hle_log()("kernel: NtSetEvent could not read argument %u from the guest "
                             "stack\n",
                             i);
            return STATUS_INVALID_PARAMETER;
        }
    }
    if (args[1] != 0u) {
        kernel_hle_log()("kernel: NtSetEvent(%#x, %#x) REFUSED: a non-NULL PreviousState is "
                         "unmeasured (all 4 sites pass the literal 0)\n",
                         (unsigned)args[0], (unsigned)args[1]);
        return STATUS_NOT_IMPLEMENTED;
    }
    const nt_status status = kernel_object_event_set(args[0], NULL);
    if (status != STATUS_SUCCESS) {
        kernel_hle_log()("kernel: NtSetEvent(%#x) failed with status %#x\n", (unsigned)args[0],
                         (unsigned)status);
    }
    return status;
}

unsigned kernel_event_handle_register(void)
{
    unsigned bound = kernel_hle_register(189u, create_event) ? 1u : 0u;
    bound += kernel_hle_register(225u, set_event) ? 1u : 0u;
    return bound;
}
