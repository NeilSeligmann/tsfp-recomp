/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Handles and object lifetime: the layer every other kernel subsystem hands out
 * references through.
 *
 * `NtClose` (ordinal 187) is the second kernel call the guest makes, measured by
 * running it, and it arrives holding a handle THIS process invented -- the boot
 * trace shows `eax = 0x00E10000`, the exact value `PsCreateSystemThreadEx` returned.
 * So a handle is not an opaque token we can ignore: the guest stores it, passes it
 * back, and expects close to mean something.
 *
 * WHY A SHARED TABLE RATHER THAN PER-SUBSYSTEM HANDLES. `NtClose` is not
 * thread-specific, file-specific or event-specific; it takes any handle. Keeping the
 * registry here means a subsystem registers its own objects and `NtClose` needs no
 * knowledge of what kinds exist. The alternative -- `NtClose` switching on a type it
 * has to guess from the handle value -- is how a closed file handle ends up freeing
 * a thread record.
 *
 * ARITIES ARE MEASURED (`tools/lift/callsites.py`, counting the guest's own argument
 * pushes): `NtClose` 1 stack argument over 33 sites, `ObfDereferenceObject` **0**
 * over 18 sites. That zero is not an error -- `Obf*` is __fastcall, so the object
 * pointer is in ECX. A handler trusting the zero would dereference nothing.
 *
 * NOT MODELLED: there is no real reference counting against host resources, no
 * waitable state, and no security descriptor. A reference count is tracked so that
 * an unbalanced close or dereference is *visible*, which is the part that catches
 * guest-side bugs; it does not gate any teardown yet.
 *
 * =====================================================================
 * ORDINAL 246: ObReferenceObjectByHandle -- the handle-to-pointer seam
 * =====================================================================
 *
 *     NTSTATUS __stdcall ObReferenceObjectByHandle(
 *             HANDLE Handle, POBJECT_TYPE ObjectType OPTIONAL,
 *             PVOID *ReturnedObject);                        // 3 stack args
 *
 * ARITY-OK(246): THREE stack arguments, and this one needs NO hand-written ABI_TABLE
 * row -- `kernel_arity.inc` carries `{246u, 3u, 12u, 1}`, which is 12 voters and
 * unanimous, so it clears `MEASURED_ARITY_MIN_SITES` (3) and `stack_args_for()`
 * accepts it on its own. The reason to record evidence anyway is that THREE is the
 * surprising answer: desktop NT's `ObReferenceObjectByHandle` takes SIX arguments
 * (Handle, DesiredAccess, ObjectType, AccessMode, Object, HandleInformation), so
 * anyone checking this against NT knowledge would expect 6 and conclude the
 * measurement was broken. It is not -- the Xbox kernel's variant really is the
 * three-argument form, and two independent sources say so:
 *
 *   - `xboxkrnl.exe.def` in XboxDev/nxdk (CC0-1.0, verdict "USE -- derive and cite" in
 *     docs/clean-sources-audit.md) lists `ObReferenceObjectByHandle@12  @ 246 NONAME`.
 *     The MSVC `@N` suffix is the TOTAL BYTE SIZE of the argument list, so 12 bytes is
 *     three dwords. No LEADING `@`, so __stdcall -- unlike ordinals 250 and 251 in the
 *     same file, which are `@ObfDereferenceObject@4` and `@ObfReferenceObject@4` and
 *     ARE fastcall.
 *   - The same header's prototype names the three: Handle, an OPTIONAL ObjectType, and
 *     an OUT pointer to the object.
 *
 * AND IT IS HAND-COUNTED AT ALL 12 SITES, which is better than either. Thunk slot
 * 0x004757DC occurs exactly 12 times in the image and every occurrence is preceded by
 * `FF 15` (`call dword ptr [disp32]`) -- no `jmp [slot]` import stub and no
 * register-indirect load, so the scanner's blind spots do not apply and 12 is COMPLETE.
 * All 12 are the same four-instruction idiom: `lea` the out-slot, push it, push the
 * ObjectType data export's VALUE, push the handle, call. No site has an `add esp, N`
 * after the call, confirming callee cleanup.
 *
 * THE ORDER IS PINNED BY A NAMED FUNCTION, not inferred. `_SetThreadPriority@8` at
 * 0x0037FC24 is `push ebp; mov ebp, esp` with no locals and no register saves:
 *
 *     0x0037FC27  lea  eax,[ebp+8]; push eax     ; arg2 ReturnedObject
 *     0x0037FC2B  push dword ptr [0x475878]      ; arg1 PsThreadObjectType -- MIDDLE
 *     0x0037FC31  push dword ptr [ebp+8]         ; arg0 HANDLE hThread
 *     0x0037FC34  call dword ptr [0x4757DC]
 *     ...
 *     0x0037FC5C  mov  ecx,[ebp+8]               ; the SAME slot, now a PETHREAD
 *     0x0037FC5F  call dword ptr [0x4757D4]      ; ObfDereferenceObject, fastcall
 *     0x0037FC72  pop  ebp
 *     0x0037FC73  ret  8
 *
 * Three facts fall out of that one site. The in/out ALIASING on `[ebp+8]` -- a handle
 * goes in and an object pointer comes back through the same slot -- is the idiom
 * NtOpenFile already uses at 0x00380D32. The ObjectType sits in the MIDDLE, and it
 * MATCHES THE HANDLE KIND at every site (file handles take ordinal 71
 * `IoFileObjectType`, thread handles ordinal 259 `PsThreadObjectType`, event handles
 * ordinal 16 `ExEventObjectType`), which no other argument order explains. And `pop
 * ebp; ret 8` with three pushes and no `sub esp` forces the callee to pop exactly 12
 * bytes, deriving the arity rather than checking it.
 *
 * ON THE RECORD, because a brief here said otherwise: ordinal 249
 * `ObSymbolicLinkObjectType` is NOT IMPORTED BY THIS TITLE AT ALL. The OBJECT_TYPE data
 * exports actually present are 16, 71 and 259, and all three appear in
 * `MEASURED_DATA_ORDINALS[]` in kernel_arity.inc -- the scanner classified them as data
 * exports, which is exactly what a pushed OBJECT_TYPE address requires.
 *
 * NOT REACHED BY THE BOOT, measured rather than assumed: the trace runs to 124 kernel
 * calls and stops on ordinal 327 `XeLoadSection`, and ordinal 246 appears nowhere in
 * it. This handler is therefore forward investment, and the honesty of its failure
 * paths matters more than their frequency. The 12 sites say where it will first be
 * reached from: `_SetThreadPriority@8`, `_SetThreadPriorityBoost@8`,
 * `_GetExitCodeThread@8`, `_XGetFilePhysicalSortKey@4`, and eight event-handle sites in
 * the 0x0043xxxx-0x0047xxxx range.
 *
 * THREAD bodies now reuse the owned control-page KTHREAD storage. The measured
 * GetExitCodeThread wrapper at 0x37FD83 reads BYTE+4 and, when nonzero, DWORD+0x120.
 * A retained body reference keeps that exact mapping alive through NtClose;
 * thread reset refuses outstanding references and detaches identities before
 * unmapping. Only confirmed guest termination publishes those two fields.
 *
 * FILE handles can now carry a fabricated guest FILE_OBJECT body too, provided by
 * kernel_file_object.c for the ONE measured file-handle site (0x37D166 inside
 * `_XGetFilePhysicalSortKey@4`, the only site in the image that passes ordinal 71
 * IoFileObjectType -- every other site passes a thread or event type). The
 * provider is a registered seam rather than an include, because kernel_file holds
 * its own lock while calling into this table (file -> object) and a fabrication
 * path that ran under the object lock would invert that order; it therefore runs
 * BEFORE the lock is taken, and binding is re-judged under it. A FILE handle the
 * provider declines (no provider installed, no kernel_file slot, no filesystem
 * identity) keeps the legacy handle surrogate, and the remaining non-THREAD kinds
 * keep it unconditionally.
 */

#ifndef TSFP_XBOX_KERNEL_OBJECT_H
#define TSFP_XBOX_KERNEL_OBJECT_H

#include <stdbool.h>
#include <stdint.h>
#include "nt_status.h"

/**
 * How many live handles we track at once. Closed slots are reissued under a new
 * generation (see kernel_object.c), so this bounds LIVE handles, not lifetime ones.
 * It is also the handle layout's 8-bit slot field: changing it means changing that.
 */
#define KERNEL_OBJECT_MAX 256

/** What a handle refers to, so a close reaches the right owner. */
typedef enum {
    KERNEL_OBJECT_NONE = 0,
    KERNEL_OBJECT_THREAD,
    KERNEL_OBJECT_FILE,
    KERNEL_OBJECT_EVENT,
    KERNEL_OBJECT_SEMAPHORE,
    KERNEL_OBJECT_MUTANT,
    /* A handle from NtOpenSymbolicLinkObject. Owned by kernel_file.c, which keeps the target. */
    KERNEL_OBJECT_SYMLINK,
    KERNEL_OBJECT_OTHER,
} kernel_object_kind;

/** One live handle. `owner_tag` is the owning subsystem's own identifier. */
typedef struct {
    uint32_t handle;
    kernel_object_kind kind;
    uint32_t owner_tag;
    uint32_t references;
    bool in_use;
    /* THREAD only: distinct open handle and retained mapped-body references.
     * references = (handle_open ? 1 : 0) + thread_body_references. Closed bodies
     * remain session-owned until thread reset atomically detaches them. */
    bool handle_open;
    uint32_t thread_body;
    uint32_t thread_body_references;
    /* EVENT only: recorded creation metadata, not a wait/signal implementation. */
    uint32_t event_type;
    uint32_t event_initial_state;
    /* EVENT only: the signal state NtSetEvent (225) sets and a satisfied wait on a type 1
     * (auto-reset) event consumes (T8g). */
    bool event_signaled;
    /* MUTANT only (T8g): the recording thread identity (kernel_thread_current_identity) and
     * the recursion count. mutant_count == 0 means unowned, and then mutant_owner is 0. */
    uint32_t mutant_owner;
    uint32_t mutant_count;
    /* FILE only: fabricated guest FILE_OBJECT body (0 when none), the filesystem
     * name dword ordinal 76 answers for it, and the outstanding body references
     * taken through ordinal 246. references = 1 (the open handle, until NtClose)
     * + file_body_references, mirroring the THREAD accounting. */
    uint32_t file_body;
    uint32_t file_fs_name;
    uint32_t file_body_references;
    /* FILE only, set by NtDuplicateObject (197): the handle of the ORIGINAL open this entry
     * duplicates (always the root, never another duplicate), 0 for an entry that is itself an
     * open. kernel_file keys its open-file state on the original handle and reaches it for a
     * duplicate through kernel_object_file_identity. */
    uint32_t file_dup_of;
} kernel_object_entry;

/** Register the object ordinals with the HLE dispatcher. Returns how many bound. */
unsigned kernel_object_register(void);

/** Drop every handle. Quiescent session/test setup only: no concurrent callers,
 * active waits or retained subsystem identities may survive this reset. */
void kernel_object_reset(void);

/**
 * Issue a handle for `kind`, or 0 when the table is full.
 *
 * Handles are deliberately sparse and high so one can never be mistaken for a small
 * integer, an array index or a guest pointer if it leaks somewhere it should not.
 */
uint32_t kernel_object_create(kernel_object_kind kind, uint32_t owner_tag);

/**
 * Look up a live handle, or NULL. A handle whose slot has since been closed and reissued
 * is NULL: the generation inside the value no longer matches.
 */
const kernel_object_entry *kernel_object_find(uint32_t handle);

/** Copy a live entry under the object lock. Failed lookup preserves *out.
 * A successful copy is an admission snapshot, not a reference/lifetime pin. */
bool kernel_object_get_copy(uint32_t handle, kernel_object_entry *out);

/* Trusted host-owned THREAD storage, readable for the measured 0x124-byte span.
 * External remapping/tampering is unsupported; caller owns the unchanged mapping
 * until detach/rollback, and calls under thread->object
 * lock order. Exact aligned body identities cannot overlap other bound bodies.
 * Publish additionally writes HandleOut under the object lock; failure restores
 * the identity before another reference can observe it. No full ETHREAD claim. */
bool kernel_object_bind_thread_body(uint32_t handle, uint32_t owner_tag, uint32_t body);
bool kernel_object_publish_thread_body(uint32_t handle, uint32_t owner_tag,
                                       uint32_t body, uint32_t handle_out);
/* Optional ThreadId output; guarded copy under the object lock refuses any
 * bound THREAD body overlap. Failure changes no body or reference identity. */
bool kernel_object_write_optional_thread_id(uint32_t out, uint32_t id);
/* Measured EVENT Type 1 (SynchronizationEvent) and Type 0 (NotificationEvent, T270), initial
 * BOOLEAN false only. Atomic table publication +
 * same-bytes writable-output probe and guarded HandleOut write, with allocation
 * rollback on failure. Output/mappings must remain quiescent; the probe writes
 * guest bytes and this is not atomic against external mapping changes. No object body,
 * waiting or signaling is claimed. Unsupported metadata returns NOT_IMPLEMENTED;
 * invalid output/THREAD-body alias returns INVALID_PARAMETER. */
nt_status kernel_object_create_event(uint32_t type, uint32_t initial, uint32_t out);
/* NtSetEvent's object half: set the EVENT handle's signal state. `*previous` (may be NULL)
 * receives the state before. STATUS_INVALID_HANDLE for a handle that is not live,
 * STATUS_OBJECT_TYPE_MISMATCH for a live handle of another kind; either leaves every
 * entry unchanged. Never waits, wakes or touches guest memory. */
nt_status kernel_object_event_set(uint32_t handle, bool *previous);
/* Clear a live EVENT handle without applying wait semantics. Returns INVALID_HANDLE or
 * OBJECT_TYPE_MISMATCH without changing any entry for a rejected handle. */
nt_status kernel_object_event_clear(uint32_t handle);
/* The recorded signal state of a live EVENT handle. False (and *out untouched) for any
 * other handle. */
bool kernel_object_event_signaled(uint32_t handle, bool *out);

/* The wait side (T8g), each one atomic under the object lock. None of them blocks: a caller that
 * gets `*acquired == false` decides what a wait that cannot proceed means.
 *
 * kernel_object_event_try_wait: a signaled event satisfies the wait (`*acquired` true) and a type
 * 1 (NT SynchronizationEvent, auto-reset, the only type NtCreateEvent creates) is consumed by it,
 * a type 0 (NotificationEvent) stays signaled. An unsignaled event leaves `*acquired` false.
 * kernel_object_mutant_try_acquire: an unowned mutant, or one already owned by `owner`, is taken
 * (count 1, or count + 1), one owned by another identity is not. A recursion past
 * KERNEL_OBJECT_MUTANT_RECURSION_MAX is STATUS_MUTANT_LIMIT_EXCEEDED with `*acquired` false.
 * kernel_object_mutant_release: `owner` must be the recorded owner, else STATUS_MUTANT_NOT_OWNED
 * (also for an unowned mutant) with nothing changed and *previous untouched. On success the count
 * drops by one, the mutant is unowned at 0, and `*previous` (may be NULL) is the count BEFORE the
 * release (INFERRED, the value a PreviousCount output would carry).
 * All three: dead handle STATUS_INVALID_HANDLE, other live kind STATUS_OBJECT_TYPE_MISMATCH, and
 * `*acquired` is false on every non-success. kernel_object_mutant_state reads owner and count of a
 * live MUTANT (false otherwise). */
#define KERNEL_OBJECT_MUTANT_RECURSION_MAX 0x10000u
nt_status kernel_object_event_try_wait(uint32_t handle, bool *acquired);
nt_status kernel_object_mutant_try_acquire(uint32_t handle, uint32_t owner, bool *acquired);
nt_status kernel_object_mutant_release(uint32_t handle, uint32_t owner, uint32_t *previous);
bool kernel_object_mutant_state(uint32_t handle, uint32_t *owner, uint32_t *count);
/* Requires an outstanding body reference; preserves Out on failure. */
bool kernel_object_get_thread_body_copy(uint32_t body, kernel_object_entry *out);
/* Creation rollback only; refuses outstanding body references. */
bool kernel_object_rollback_thread_body(uint32_t handle, uint32_t owner_tag);
/* Atomic all-bound-THREAD detach, only if every body has zero references.
 * Must precede any unmapping; thread reset holds its own lock/gate throughout. */
bool kernel_object_detach_thread_bodies(void);

/*
 * The FILE-body seam, installed by kernel_file_object_register(). `fabricate` is
 * called with NO lock held, before ordinal 246 takes the table lock, for a live
 * FILE handle with no body yet; it allocates guest memory, binds it back through
 * kernel_object_bind_file_body and returns the body VA (or 0 to decline, which
 * leaves the legacy handle surrogate in force). `release` is called UNDER the
 * table lock when a body-carrying entry's last reference goes away, and must only
 * touch its own leaf state. Installation is registration-time, not reset-time:
 * kernel_object_reset leaves the ops installed, exactly as the HLE dispatcher
 * keeps its registrations. NOT thread-safe against concurrent dispatch: install
 * before the guest runs, as every other registration is.
 */
typedef struct {
    uint32_t (*fabricate)(uint32_t handle);
    void (*release)(uint32_t body);
} kernel_object_file_body_ops;
void kernel_object_set_file_body_ops(kernel_object_file_body_ops ops);

/* Bind a fabricated body to a live FILE handle that has none. False on a dead or
 * non-FILE handle, an already-bound entry, or a zero/misaligned body, so a racing
 * second fabrication is told to free its block rather than leak it. */
bool kernel_object_bind_file_body(uint32_t handle, uint32_t body, uint32_t fs_name);

/* The live FILE entry whose body is exactly `body`: its handle and filesystem
 * name dword. False when no live entry carries that body, which is how ordinal 76
 * tells a fabricated FILE_OBJECT from a handle value or a stray pointer. */
bool kernel_object_find_file_body(uint32_t body, uint32_t *out_handle,
                                  uint32_t *out_fs_name);

/**
 * Ordinal 221 NtReleaseMutant calls that found a live MUTANT handle and answered
 * STATUS_MUTANT_NOT_OWNED: the mutant was unowned or owned by another thread identity. A wait
 * (233/234, T8g) is what makes a mutant owned, so nonzero means the guest unlocked without a
 * lock, from a thread that does not hold it, or locked through a path this host does not model.
 */
unsigned kernel_object_mutant_unowned_release_count(void);

/**
 * The handle kernel_file's open-file state is keyed on, for a FILE handle: the original open's
 * handle when `handle` is a live duplicate (NtDuplicateObject, ordinal 197), `handle` itself
 * otherwise. A value that is not a live FILE duplicate is returned UNCHANGED, so a lookup of a
 * dead or foreign handle fails exactly as it did before duplicates existed. Takes the object
 * lock, so a caller holding kernel_file's lock follows the file -> object order.
 */
uint32_t kernel_object_file_identity(uint32_t handle);

/**
 * Whether an open-file identity still has a live handle: the original handle is live, or any
 * live duplicate of it is. kernel_file reclaims an open-file slot only when this is false, so a
 * duplicate keeps the shared open alive after the original is closed (NT keeps the file object
 * until its last handle goes).
 */
bool kernel_object_file_identity_live(uint32_t identity);

/** Live handle count. */
unsigned kernel_object_live_count(void);

/**
 * Slots taken out of service for good because their generation counter ran out. Nonzero
 * means a title has opened and closed over 16000 handles through one slot, and every
 * retired slot is permanent capacity lost.
 */
unsigned kernel_object_retired_count(void);

/**
 * Close a live handle from host code, as NtClose would. Honours the reference count.
 * False for a handle that is not live. For a subsystem that issued a handle and then
 * failed to hand it to the guest, so the slot is not leaked.
 */
bool kernel_object_release(uint32_t handle);

/**
 * Closes attempted on a handle that is not live (never issued, or already closed).
 * Non-zero means a real bug.
 */
uint32_t kernel_object_bad_close_count(void);

/**
 * How many times ordinal 246 published an object pointer, of any kind: a THREAD
 * body, a fabricated FILE body, or the legacy handle surrogate.
 *
 * A surrogate is not a failure by itself -- it round-trips correctly through
 * `Obf{Reference,Dereference}Object` -- but a guest that DEREFERENCES one is
 * reading garbage, which is exactly why FILE handles with a filesystem identity
 * now get a real body instead (see the file header).
 */
uint32_t kernel_object_reference_by_handle_count(void);

/**
 * How many ordinal-246 calls passed a non-NULL ObjectType we could not check.
 *
 * There is no OBJECT_TYPE registry in this host, so a type argument is RECORDED and not
 * enforced. Counted rather than ignored: enforcing a type we cannot identify would
 * invent failures, and silently dropping it would hide the day the guest starts relying
 * on the kernel to reject a mismatched handle.
 */
uint32_t kernel_object_unchecked_type_count(void);

#endif /* TSFP_XBOX_KERNEL_OBJECT_H */
