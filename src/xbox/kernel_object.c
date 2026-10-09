/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See kernel_object.h for why the handle table is shared rather than per-subsystem.
 *
 * ASSUMED SIGNATURES. Stack-argument counts are measured; the parameter meanings
 * are from the published export list. An arity error is silent, so if a disassembly
 * contradicts one of these, this comment is what to fix.
 *
 *   NTSTATUS __stdcall  NtClose(HANDLE Handle);                        // 1 stack arg
 *   NTSTATUS __stdcall  NtCreateMutant(PHANDLE, POBJECT_ATTRIBUTES OPTIONAL,
 *                                      BOOLEAN InitialOwner);          // 3 stack args
 *   void     __fastcall ObfDereferenceObject(PVOID Object);            // ECX, 0 stack
 *   void     __fastcall ObfReferenceObject(PVOID Object);              // ECX, 0 stack
 *   NTSTATUS __stdcall  ObReferenceObjectByHandle(HANDLE, POBJECT_TYPE,
 *                                                 PVOID *Out);         // 3 stack args
 *
 * The DEF-file decorations are mechanical about which of those is which convention:
 * `NtClose@4` and `ObReferenceObjectByHandle@12` have no leading `@`, while
 * `@ObfDereferenceObject@4` and `@ObfReferenceObject@4` do. See kernel_object.h.
 */

#include "kernel_object.h"

#include <pthread.h>
#include <limits.h>

#include "guest_structs.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "kernel_thread.h"
#include "nt_status.h"

/* Address of a guest struct member, 0 on 32-bit wrap (which every accessor refuses). */
#define GUEST_FIELD(base, member) \
    kernel_guest_add((base), (uint32_t)offsetof(guest_object_attributes, member))

#define THREAD_BODY_SPAN 0x124u
static bool overlap(uint32_t left, uint32_t left_size, uint32_t right, uint32_t right_size);

#define ORD_NtClose 187u
#define ORD_NtCreateMutant 192u
#define ORD_NtReleaseMutant 221u
#define ORD_NtDuplicateObject 197u

/* ULONG Options of NtDuplicateObject: the one measured value (INFERRED name, see the handler). */
#define KERNEL_OBJECT_DUPLICATE_SAME_ACCESS 2u

static const char *kind_name(kernel_object_kind kind)
{
    switch (kind) {
    case KERNEL_OBJECT_THREAD: return "THREAD";
    case KERNEL_OBJECT_FILE: return "FILE";
    case KERNEL_OBJECT_EVENT: return "EVENT";
    case KERNEL_OBJECT_SEMAPHORE: return "SEMAPHORE";
    case KERNEL_OBJECT_MUTANT: return "MUTANT";
    case KERNEL_OBJECT_SYMLINK: return "SYMLINK";
    case KERNEL_OBJECT_OTHER: return "OTHER";
    case KERNEL_OBJECT_NONE: break;
    }
    return "unknown";
}

/* STATUS_INSUFFICIENT_RESOURCES. Named here because nt_status.h does not carry it. */
#define KERNEL_OBJECT_STATUS_INSUFFICIENT_RESOURCES 0xC000009Au
#define ORD_ObReferenceObjectByHandle 246u
#define ORD_ObfReferenceObject 251u
#define ORD_ObfDereferenceObject 250u

/*
 * HANDLE LAYOUT AND RECYCLING.
 *
 * A closed slot is reissued, so what stops a stale handle in a guest variable from
 * naming the NEW object is that the handle VALUE differs: every reissue of a slot bumps
 * that slot's generation, and the generation is part of the value.
 *
 *     bits 31..24  generation, high 8 bits
 *     bits 23..16  0xE1, the constant marker
 *     bits 15..10  generation, low 6 bits
 *     bits  9..2   slot index (256 slots)
 *     bits  1..0   zero
 *
 * Slot 0, generation 0 is 0x00E10000 and slot 1 is 0x00E10004, exactly the values the
 * table issued before recycling existed, so the boot trace does not move. The marker
 * byte is why no handle can ever be 0, 0xFFFFFFFF (current process), 0xFFFFFFFE (current
 * thread) or 0xFFFFFFFC (the root directory the title's CreateMutexA wrapper passes):
 * none of them has 0xE1 in bits 23..16. Handles stay sparse, high and a multiple of 4.
 *
 * WHY A GENERATION RATHER THAN "NEVER REUSE A VALUE". Never reusing is the lifetime cap
 * this replaces. Reusing the value at once is what NT does, and it makes a stale handle
 * a use-after-free the guest cannot even detect. Other code here relies on the
 * difference: `kernel_file.c` reclaims its open-file slots by asking whether the recorded
 * handle is still live, and `kernel_thread.c` finds its record by handle value. Under
 * plain index reuse both would see the stale value as live again.
 *
 * FIFO REUSE. Free slots are handed out oldest-freed first, never lowest-first, so one
 * slot is not hammered while 255 others sit idle. That spreads the 14 generation bits
 * (16384 reissues per slot) over the whole table.
 *
 * WHEN THE GENERATION RUNS OUT the slot is RETIRED, not wrapped. Wrapping would let a
 * handle issued 16384 closes ago alias a live one, which is the property this exists to
 * rule out. A retired slot is counted and logged, and the table degrades to refusing
 * creation only after 256 x 16384 = 4,194,304 lifetime handles, or sooner if the guest
 * leaks. INFERRED, not measured: no title is known to approach that, the boot uses
 * about 13.
 */
#define KERNEL_OBJECT_HANDLE_MARKER 0x00E10000u
#define KERNEL_OBJECT_SLOT_BITS 8u
#define KERNEL_OBJECT_GENERATION_LOW_BITS 6u
#define KERNEL_OBJECT_GENERATION_COUNT 16384u

_Static_assert(KERNEL_OBJECT_MAX == (1 << KERNEL_OBJECT_SLOT_BITS),
               "the handle layout has exactly 8 slot bits");

static uint32_t handle_encode(unsigned slot, unsigned generation)
{
    const uint32_t low = generation & ((1u << KERNEL_OBJECT_GENERATION_LOW_BITS) - 1u);
    const uint32_t high = generation >> KERNEL_OBJECT_GENERATION_LOW_BITS;
    return KERNEL_OBJECT_HANDLE_MARKER | ((uint32_t)slot << 2) | (low << 10) | (high << 24);
}

/* The slot a handle names. Any value yields one, so the caller must still compare the
 * whole handle against the entry: that comparison is what rejects a wrong marker, a
 * misaligned value and a stale generation alike. */
static unsigned handle_slot(uint32_t handle)
{
    return (handle >> 2) & (KERNEL_OBJECT_MAX - 1u);
}


/* ---------------------------------------------------------------------------
 * THE LOCK.
 *
 * `NtClose` is the second kernel call the guest makes, and it now arrives on a
 * different host thread from the `PsCreateSystemThreadEx` that issued the handle.
 * So this table is genuinely shared, and the races in it are the kind that produce
 * a wrong answer rather than a crash:
 *
 *   - `kernel_object_create` is a bare read-modify-write of `next_slot`. Two
 *     threads both reading the same value both take the same slot and are both
 *     returned the IDENTICAL handle for two different objects. The second one's
 *     fields overwrite the first's, so the first object has no record at all; the
 *     guest's later close of a handle we really did issue then reports "handle was
 *     never issued by us" and bumps `bad_close_count` -- our diagnostic accusing
 *     the guest of our own bug.
 *   - `references++` and `references--` are non-atomic. A lost increment means the
 *     next `NtClose` destroys an object the guest still holds; a lost decrement
 *     leaks the entry.
 *   - `*entry = (kernel_object_entry){0}` is a multi-word store, so a concurrent
 *     scan can see `in_use` true with `handle` already zeroed and fail to find a
 *     live handle.
 *
 * RECURSIVE, for the same reason as `guest_mem.c`: these critical sections call
 * `kernel_hle_log()`, whose sink is caller-supplied and may well ask this module a
 * question. Tests install custom sinks.
 *
 * LOCK ORDER. `kernel_thread.c` holds its own table lock while calling
 * `kernel_object_create`, and nothing here ever calls into `kernel_thread.c`. So
 * the order is always thread-table then object-table, and there is no cycle.
 *
 * NOT FIXED, on the record: `kernel_object_find` returns a pointer INTO the table,
 * which outlives the lock. A caller can hold it across a close that zeroes the
 * entry, and since slots are now recycled it can also see a DIFFERENT live object in
 * that slot rather than a zeroed one. Fixing that means returning a copy and changing
 * every call site.
 * ------------------------------------------------------------------------- */

static pthread_mutex_t object_lock;
static pthread_once_t object_lock_once = PTHREAD_ONCE_INIT;

static void object_lock_init(void)
{
    pthread_mutexattr_t attr;
    (void)pthread_mutexattr_init(&attr);
    (void)pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    (void)pthread_mutex_init(&object_lock, &attr);
    (void)pthread_mutexattr_destroy(&attr);
}

static void object_enter(void)
{
    (void)pthread_once(&object_lock_once, object_lock_init);
    (void)pthread_mutex_lock(&object_lock);
}

static void object_leave(void)
{
    (void)pthread_mutex_unlock(&object_lock);
}

static kernel_object_entry objects[KERNEL_OBJECT_MAX];
/* The generation the NEXT issue of each slot will carry. */
static uint16_t generation[KERNEL_OBJECT_MAX];
/* Slots never issued yet are taken in index order, then freed ones come back through a
 * ring, oldest first. Zero is a valid initial state, so no test has to call reset. */
static unsigned next_fresh;
static uint16_t free_queue[KERNEL_OBJECT_MAX];
static unsigned free_head;
static unsigned free_count;
static unsigned retired_count;
static uint32_t bad_close_count;
static uint32_t reference_by_handle_count;
static uint32_t unchecked_type_count;
static unsigned mutant_unowned_release_count;

static void kernel_object_reset_nolock(void)
{
    for (unsigned i = 0u; i < KERNEL_OBJECT_MAX; i++) {
        objects[i] = (kernel_object_entry){0};
        generation[i] = 0u;
        free_queue[i] = 0u;
    }
    next_fresh = 0u;
    free_head = 0u;
    free_count = 0u;
    retired_count = 0u;
    bad_close_count = 0u;
    reference_by_handle_count = 0u;
    unchecked_type_count = 0u;
    mutant_unowned_release_count = 0u;
}

static uint32_t kernel_object_bad_close_count_nolock(void)
{
    return bad_close_count;
}

static unsigned kernel_object_live_count_nolock(void)
{
    unsigned live = 0u;
    for (unsigned i = 0u; i < KERNEL_OBJECT_MAX; i++) {
        if (objects[i].in_use &&
            (objects[i].kind != KERNEL_OBJECT_THREAD || objects[i].handle_open)) {
            live++;
        }
    }
    return live;
}

static uint32_t kernel_object_create_nolock(kernel_object_kind kind, uint32_t owner_tag)
{
    if (next_fresh >= KERNEL_OBJECT_MAX && free_count == 0u) {
        kernel_hle_log()("kernel: handle table exhausted: %u slots, %u retired after %u "
                         "generations each\n",
                         (unsigned)KERNEL_OBJECT_MAX, retired_count,
                         (unsigned)KERNEL_OBJECT_GENERATION_COUNT);
        return 0u;
    }
    unsigned slot = 0u;
    if (next_fresh < KERNEL_OBJECT_MAX) {
        slot = next_fresh++;
    } else {
        slot = free_queue[free_head];
        free_head = (free_head + 1u) % KERNEL_OBJECT_MAX;
        free_count--;
    }

    kernel_object_entry *entry = &objects[slot];
    entry->handle = handle_encode(slot, generation[slot]);
    entry->kind = kind;
    entry->owner_tag = owner_tag;
    entry->references = 1u;
    entry->in_use = true;
    entry->handle_open = kind == KERNEL_OBJECT_THREAD;
    return entry->handle;
}

/* Free a live slot and either queue it for reuse or retire it. */
static void kernel_object_release_nolock(kernel_object_entry *entry)
{
    const unsigned slot = (unsigned)(entry - objects);
    *entry = (kernel_object_entry){0};
    generation[slot]++;
    if (generation[slot] >= KERNEL_OBJECT_GENERATION_COUNT) {
        retired_count++;
        kernel_hle_log()("kernel: handle slot %u RETIRED after %u generations, so no "
                         "stale handle can alias a live one\n",
                         slot, (unsigned)KERNEL_OBJECT_GENERATION_COUNT);
        return;
    }
    free_queue[(free_head + free_count) % KERNEL_OBJECT_MAX] = (uint16_t)slot;
    free_count++;
}

static kernel_object_entry *mutable_find_any(uint32_t handle)
{
    kernel_object_entry *entry = &objects[handle_slot(handle)];
    return (entry->in_use && entry->handle == handle) ? entry : NULL;
}

static kernel_object_entry *mutable_find(uint32_t handle)
{
    kernel_object_entry *entry = mutable_find_any(handle);
    return entry && (entry->kind != KERNEL_OBJECT_THREAD || entry->handle_open) ? entry : NULL;
}
static kernel_object_entry *find_thread_body(uint32_t body)
{
    if (body == 0u) return NULL;
    for (unsigned i = 0u; i < KERNEL_OBJECT_MAX; i++) {
        kernel_object_entry *entry = &objects[i];
        if (entry->in_use && entry->kind == KERNEL_OBJECT_THREAD &&
            entry->thread_body == body) return entry;
    }
    return NULL;
}

/* The FILE-body seam. Installed at registration time by kernel_file_object.c and
 * never under the lock; see kernel_object.h for the order argument. */
static kernel_object_file_body_ops file_body_ops;

static kernel_object_entry *find_file_body_entry(uint32_t body)
{
    if (body == 0u) return NULL;
    for (unsigned i = 0u; i < KERNEL_OBJECT_MAX; i++) {
        kernel_object_entry *entry = &objects[i];
        if (entry->in_use && entry->kind == KERNEL_OBJECT_FILE &&
            entry->file_body == body) return entry;
    }
    return NULL;
}

/* Free a FILE entry's slot and hand its body back to the provider. Under the
 * lock; `release` is a leaf (its own lock and the guest heap only). */
static void release_file_entry_nolock(kernel_object_entry *entry)
{
    const uint32_t body = entry->file_body;
    kernel_object_release_nolock(entry);
    if (body != 0u && file_body_ops.release != NULL) file_body_ops.release(body);
}

static const kernel_object_entry *kernel_object_find_nolock(uint32_t handle)
{
    return mutable_find(handle);
}

/* One close: drop a reference, and free the slot when it was the last. */
static void close_entry_nolock(kernel_object_entry *entry)
{
    if (entry->kind == KERNEL_OBJECT_THREAD) {
        entry->handle_open = false;
        entry->references = entry->thread_body_references;
        if (entry->thread_body == 0u) kernel_object_release_nolock(entry);
        return;
    }
    if (entry->references > 1u) {
        entry->references--;
        return;
    }
    release_file_entry_nolock(entry);
}

static uint32_t nt_close_nolock(void *context)
{
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t handle = 0u;
    if (!kernel_frame_arg(frame, 0u, &handle)) {
        kernel_hle_log()("kernel: NtClose with no argument frame\n");
        return STATUS_INVALID_HANDLE;
    }

    kernel_object_entry *entry = mutable_find(handle);
    if (entry == NULL) {
        /* Counted and reported, never silently succeeded. Closing a handle we never
         * issued means our handle space and the guest's have diverged, which is a
         * much worse problem than the close itself and would otherwise be invisible. */
        bad_close_count++;
        kernel_hle_log()("kernel: NtClose(%#x) -- handle was never issued by us\n",
                         (unsigned)handle);
        return STATUS_INVALID_HANDLE;
    }

    close_entry_nolock(entry);
    return STATUS_SUCCESS;
}

/*
 * ARITY-OK(192): THREE stack arguments, (PHANDLE MutantHandle, POBJECT_ATTRIBUTES OPTIONAL,
 * BOOLEAN InitialOwner). ONE call site exists in the image, at 0x0037FFD9, inside the title's
 * own CreateMutexA wrapper at 0x0037FFB1. The measured row is `{192u, 3u, 1u, 1}`: its
 * "unanimous" flag is a tautology over one site, so `stack_args_for()` refuses it and the
 * count comes from the oracle (`NtCreateMutant@12`, a different kernel build) unless a hand
 * row is added. The site gives independent evidence of its own:
 *
 *     0x0037FFD1  push [ebp+0xc]            ; arg2 InitialOwner
 *     0x0037FFD4  push eax                  ; arg1 OBJECT_ATTRIBUTES, or 0
 *     0x0037FFD5  lea eax,[ebp+0x10]; push eax ; arg0 PHANDLE, aliasing the name argument
 *     0x0037FFD9  call dword ptr [0x4758A4]
 *
 * 0x0037FFD1 is a CONTROL-FLOW MERGE (`jmp 0x37ffd1` from the named path, fall-through from
 * the unnamed path), so no push can sit before it that only one path made. That bounds the
 * window to exactly these three pushes, and both paths load eax with the OBJECT_ATTRIBUTES
 * pointer or 0, which is arg1.
 *
 * THE ONLY CALLER passes (0, 0, 0): a global constructor at 0x003D21B0 creates one UNNAMED
 * mutex that is NOT initially owned and keeps the handle at 0x00771C68. So that is the only
 * shape implemented. A named mutant or an initial owner is REFUSED with a report rather than
 * answered: a name needs an object namespace (and the wrapper's root 0xFFFFFFFC and
 * attributes 0x80 are not derived), and an initial owner needs the calling thread's identity.
 *
 * T142 RE-MEASURED the whole reachability of the named path, so the refusal above is
 * now known to guard STATICALLY DEAD code rather than an unexercised feature:
 *   - The wrapper 0x37FFB1 has exactly ONE caller in the image, 0x3D21CA inside the
 *     constructor 0x3D21B0, pushing (0, 0, 0) -- name NULL. The E8 byte-sweep upper
 *     bound equals the linear-decode count (1 site), and NO section of the XBE holds
 *     the wrapper's address as a dword, so there is no indirect dispatch either.
 *   - The shared OBJECT_ATTRIBUTES builder 0x381B7C (root 0xFFFFFFFC, attributes 0x80)
 *     has exactly TWO call sites, 0x37FF47 and 0x37FFC8, both on the name != NULL
 *     branches of CreateEventA/CreateMutexA, and both branches are unreachable because
 *     each wrapper's single caller passes a NULL name. An earlier follow-up note spoke
 *     of "seven callers"; the measured count is one per wrapper.
 *   - The wrappers' duplicate-create mapping (status 0x40000000 to last-error 0xB7 at
 *     0x37FFE3..0x37FFEA) sits downstream of the dead branch only, so no create-vs-open
 *     or collision semantics were ever exercised by this title and none are modelled.
 * A Win32NamedObjects-style name table here would therefore be INVENTED, not recovered.
 *
 * NOT MODELLED, said where it matters: there is no wait queue. Ownership and the recursion count are
 * recorded by the wait side (233/234, T8g) and NtReleaseMutant.
 */
static uint32_t nt_create_mutant_nolock(void *context)
{
    if (!context) {
        kernel_hle_log()("kernel: NtCreateMutant called with no argument frame\n");
        return STATUS_INVALID_PARAMETER;
    }
    const kernel_call_frame *frame = (const kernel_call_frame *)context;

    uint32_t args[3];
    for (unsigned i = 0u; i < 3u; i++) {
        if (!kernel_frame_arg(frame, i, &args[i])) {
            kernel_hle_log()("kernel: NtCreateMutant could not read argument %u from the "
                             "guest stack\n",
                             i);
            return STATUS_INVALID_PARAMETER;
        }
    }
    const kernel_guest_ptr handle_out = args[0];
    const kernel_guest_ptr object_attributes = args[1];
    /* BOOLEAN is one byte on NT. The guest pushes a whole dword, so the upper bytes are
     * whatever its BOOL held and are not part of the argument. */
    const bool initial_owner = (args[2] & 0xFFu) != 0u;

    if (handle_out == 0u) {
        kernel_hle_log()("kernel: NtCreateMutant has no handle out-parameter\n");
        return STATUS_INVALID_PARAMETER;
    }

    uint32_t root_directory = 0u;
    uint32_t object_name = 0u;
    uint32_t attributes = 0u;
    if (object_attributes != 0u &&
        (!kernel_guest_read_u32(GUEST_FIELD(object_attributes, object_name), &object_name) ||
         !kernel_guest_read_u32(GUEST_FIELD(object_attributes, root_directory),
                                &root_directory) ||
         !kernel_guest_read_u32(GUEST_FIELD(object_attributes, attributes), &attributes))) {
        kernel_hle_log()("kernel: NtCreateMutant could not read the OBJECT_ATTRIBUTES at "
                         "%#x\n",
                         (unsigned)object_attributes);
        return STATUS_INVALID_PARAMETER;
    }
    if (object_name != 0u) {
        /* The only shape that carries root 0xFFFFFFFC and attributes 0x80: the title's
         * CreateMutexA wrapper at 0x00381B7C builds exactly that for a NAMED mutex. Both
         * are printed so the refusal names what the guest actually asked for. */
        kernel_hle_log()("kernel: NtCreateMutant with a NAMED mutant (OBJECT_ATTRIBUTES "
                         "%#x, root %#x, name %#x, attributes %#x) is NOT IMPLEMENTED -- no "
                         "object namespace exists, refusing rather than creating an unnamed "
                         "one\n",
                         (unsigned)object_attributes, (unsigned)root_directory,
                         (unsigned)object_name, (unsigned)attributes);
        return STATUS_NOT_IMPLEMENTED;
    }
    if (object_attributes != 0u && (root_directory != 0u || attributes != 0u)) {
        /* Unnamed, so the root and the flags have nothing to apply to. REPORTED rather than
         * dropped: OBJ_INHERIT would change what a child process sees of this handle. */
        kernel_hle_log()("kernel: NtCreateMutant ignores root %#x and attributes %#x on an "
                         "unnamed mutant\n",
                         (unsigned)root_directory, (unsigned)attributes);
    }
    if (initial_owner) {
        kernel_hle_log()("kernel: NtCreateMutant with InitialOwner set is NOT IMPLEMENTED "
                         "-- ownership needs the calling thread's identity, refusing\n");
        return STATUS_NOT_IMPLEMENTED;
    }

    const uint32_t handle = kernel_object_create_nolock(KERNEL_OBJECT_MUTANT,
                                                        ORD_NtCreateMutant);
    if (handle == 0u) {
        return KERNEL_OBJECT_STATUS_INSUFFICIENT_RESOURCES;
    }
    if (!kernel_guest_write_u32(handle_out, handle)) {
        /* The guest never learns this handle, so nothing will ever close it. */
        close_entry_nolock(mutable_find(handle));
        kernel_hle_log()("kernel: NtCreateMutant could not write the handle to %#x, "
                         "handle released\n",
                         (unsigned)handle_out);
        return STATUS_INVALID_PARAMETER;
    }
    kernel_hle_log()("kernel: NtCreateMutant -> handle %#x (UNNAMED, not owned; a wait "
                     "that would block is NOT MODELLED)\n",
                     (unsigned)handle);
    return STATUS_SUCCESS;
}

/*
 * Ordinal 221 NtReleaseMutant(HANDLE MutantHandle, PLONG PreviousCount OPTIONAL), stdcall, TWO
 * arguments (the oracle and nxdk `NtReleaseMutant@8` agree with the site). ONE call site in the
 * image, found by the thunk slot 0x4758A8 (referenced once, as `call dword ptr`):
 *
 *     0x00380009  push 0                 ; arg1 PreviousCount, the literal NULL
 *     0x0038000B  push [esp+8]           ; arg0 the handle (esp+8 is the wrapper's own argument
 *                                        ;   once the first push has moved esp)
 *     0x0038000F  call dword ptr [0x4758A8]
 *     0x00380015  test eax,eax; jl 0x38001E ; negative -> error mapper 0x37E9FD, return FALSE
 *                                        ; otherwise return TRUE; `ret 4`
 *
 * That is the title's ReleaseMutex. Its only caller, 0x003ABD74, is the Unlock method (vtable
 * slot +0x10 of the class whose vtable is at 0x004B3600) of the lock class the global
 * constructor at 0x003D21B0 instantiates over the mutex handle it keeps at 0x00771C68. The
 * paired Lock method 0x003ABD40 calls the WaitForSingleObject wrapper 0x003800BF(handle, -1).
 * On a zero result Unlock logs "Error leaving critical section\n" (0x004B3638) and returns;
 * the result is read for nothing else. Because both methods are reached through the vtable, a
 * direct-call scan sees a single caller and cannot see a second user of the mutex.
 *
 * MEASURED: arity, argument order, PreviousCount NULL at the only site, the handle's origin
 * (NtCreateMutant 192, whose one measured shape is unnamed and not initially owned), and that
 * only the sign of the status is observed. So the one measured mode is PreviousCount NULL, and
 * a non-NULL one is REFUSED loudly (STATUS_NOT_IMPLEMENTED) with nothing written and nothing
 * judged, as 225 refuses its unmeasured PreviousState.
 *
 * INFERRED, labelled in every report: the NT contract. A dead handle is STATUS_INVALID_HANDLE,
 * a live handle of another kind STATUS_OBJECT_TYPE_MISMATCH, and releasing a mutant its caller
 * does not own is STATUS_MUTANT_NOT_OWNED (KeReleaseMutant raises it when the owner thread is
 * not the current one).
 *
 * OWNERSHIP (T8g). 233/234 now acquire a mutant (kernel_thread.c): an unowned one is taken by the
 * calling thread identity (kernel_thread_current_identity, a guest thread's own handle or the
 * boot value) with recursion count 1, an owned-by-caller one counts up, and one held by another
 * identity is a would-block that the wait side refuses. So this handler now has a real success:
 * the recorded owner is the caller, the count drops by one and the mutant is unowned at 0. Any
 * other caller, or an unowned mutant, is STATUS_MUTANT_NOT_OWNED, counted and reported, which is
 * also what the title's own Unlock logs on ("Error leaving critical section"). No waiter is woken
 * because no waiter ever blocks. PreviousCount stays refused, unmeasured: the object API
 * (kernel_object_mutant_release) already returns the count before the release for the day a site
 * asks. NOT MODELLED: NT marks a mutant ABANDONED when its owner thread exits and wakes the
 * waiters with STATUS_ABANDONED_WAIT_0, here it stays owned by the dead identity.
 */
static nt_status mutant_release_nolock(kernel_object_entry *entry, uint32_t owner,
                                       uint32_t *previous);
static uint32_t nt_release_mutant_nolock(void *context)
{
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t args[2];
    for (unsigned i = 0u; i < 2u; i++) {
        if (!kernel_frame_arg(frame, i, &args[i])) {
            kernel_hle_log()("kernel: NtReleaseMutant could not read argument %u from the "
                             "guest stack\n",
                             i);
            return STATUS_INVALID_PARAMETER;
        }
    }
    if (args[1] != 0u) {
        kernel_hle_log()("kernel: NtReleaseMutant(%#x, %#x) REFUSED: a non-NULL PreviousCount "
                         "is unmeasured (the one site passes the literal 0)\n",
                         (unsigned)args[0], (unsigned)args[1]);
        return STATUS_NOT_IMPLEMENTED;
    }
    kernel_object_entry *entry = mutable_find(args[0]);
    if (entry == NULL) {
        kernel_hle_log()("kernel: NtReleaseMutant(%#x) -- not a live handle\n",
                         (unsigned)args[0]);
        return STATUS_INVALID_HANDLE;
    }
    if (entry->kind != KERNEL_OBJECT_MUTANT) {
        kernel_hle_log()("kernel: NtReleaseMutant(%#x) -- the handle is not a mutant\n",
                         (unsigned)args[0]);
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    const nt_status status = mutant_release_nolock(entry, kernel_thread_current_identity(), NULL);
    if (status == STATUS_MUTANT_NOT_OWNED) {
        mutant_unowned_release_count++;
        kernel_hle_log()("kernel: NtReleaseMutant(%#x) -> NOT OWNED, STATUS_MUTANT_NOT_OWNED "
                         "(INFERRED NT contract): the mutant is unowned or held by another "
                         "thread identity, so the guest released a mutant it does not hold\n",
                         (unsigned)args[0]);
    }
    return status;
}

/*
 * Ordinal 197 NtDuplicateObject(HANDLE SourceHandle, PHANDLE TargetHandle, ULONG Options),
 * stdcall, THREE arguments (the oracle and nxdk `NtDuplicateObject@12` agree with the site;
 * desktop NT takes seven, the Xbox kernel's is the short form). ONE call site, found by the
 * thunk slot 0x4757AC (referenced once, as `call dword ptr`), inside the title's XAPI
 * DuplicateHandle wrapper at 0x0037CBE2, which takes SEVEN arguments (`ret 0x1c`) and forwards
 * three:
 *
 *     0x0037CBE2  push [esp+0x1c]        ; arg2 Options        <- wrapper arg 6
 *     0x0037CBE6  push [esp+0x14]        ; arg1 TargetHandle   <- wrapper arg 3 (lpTargetHandle)
 *     0x0037CBEA  push [esp+0x10]        ; arg0 SourceHandle   <- wrapper arg 1 (hSourceHandle)
 *     0x0037CBEE  call dword ptr [0x4757AC]
 *     0x0037CBF4  test eax,eax; jl 0x37CBFD ; negative -> error mapper 0x37E9FD, return FALSE
 *
 * The wrapper's other four arguments (hSourceProcess, hTargetProcess, dwDesiredAccess,
 * bInheritHandle) are DROPPED, so the kernel never sees a process, an access mask or an
 * inherit flag. Its address is also listed in the XTLID library table (0x0089C444), a registry
 * and not a call.
 *
 * THE ONE CALLER is 0x0042CCE8 in the XONLINE section (a cache/file routine), with
 * `DuplicateHandle(0, [0x0054C404], 0, &local, 0, 0, 2)`: the local is preset to -1, the source
 * is the global written at 0x00384F69 with the result of the CreateFile wrapper called as
 * (path, GENERIC_READ|GENERIC_WRITE 0xC0000000, share 0, NULL, disposition 2, 0, 0), so it is
 * a FILE handle, and Options is the literal 2. On success it runs, on the DUPLICATE,
 * NtQueryInformationFile (211, class 0x22, via 0x0037D111), NtReadFile (219, 0x8000-byte reads
 * via 0x0037CC08) and NtWriteFile (236 via 0x0037CCF5), all with a NULL ByteOffset (the shared
 * position), and finally closes the duplicate (NtClose via 0x0037CBC4) when it is not -1. The
 * title never closes the duplicate's source first.
 *
 * MEASURED: arity, argument order, Options 2 only, a non-NULL TargetHandle, a FILE source, and
 * the use of the result through read/write/query/close. INFERRED, labelled in every report:
 * that 2 is DUPLICATE_SAME_ACCESS (1 is DUPLICATE_CLOSE_SOURCE; nxdk's header names them), the
 * dead-handle status STATUS_INVALID_HANDLE, and that a duplicate shares the original's file
 * object (position, access, backing), which is what NT does and what the reads and writes above
 * rely on.
 *
 * WHAT IS IMPLEMENTED, and nothing more. A new FILE entry with `file_dup_of` = the ORIGINAL
 * handle (never another duplicate: a duplicate of a duplicate flattens to the root), one
 * reference of its own, no body of its own. kernel_file resolves any FILE handle to its open-file
 * state through kernel_object_file_identity, so the duplicate reads, writes, queries and seeks
 * the original's open and shares its position, and an open-file slot is reclaimed only when
 * kernel_object_file_identity_live says no handle (original or duplicate) is left. REFUSED
 * loudly (STATUS_NOT_IMPLEMENTED, nothing created, *TargetHandle untouched): any Options but
 * exactly 2 (judged as the whole ULONG, before the handle), a NULL TargetHandle, the two pseudo
 * handles 0xFFFFFFFF and 0xFFFFFFFE (a real NT mode, unmeasured), and every live kind except
 * FILE. A duplicate of an EVENT, MUTANT, SEMAPHORE or THREAD would need state SHARED between
 * the handles (the signal flag, the owner and recursion count, the retained body references),
 * which this table keeps per handle, so each of those would need a shared object record first
 * and a wait that reads it. A dead handle is STATUS_INVALID_HANDLE; a full table is
 * STATUS_INSUFFICIENT_RESOURCES; an unwritable TargetHandle is STATUS_INVALID_PARAMETER and
 * the new entry is released again.
 */
static uint32_t nt_duplicate_object_nolock(void *context)
{
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t args[3];
    for (unsigned i = 0u; i < 3u; i++) {
        if (!kernel_frame_arg(frame, i, &args[i])) {
            kernel_hle_log()("kernel: NtDuplicateObject could not read argument %u from the "
                             "guest stack\n",
                             i);
            return STATUS_INVALID_PARAMETER;
        }
    }
    const uint32_t source = args[0];
    const kernel_guest_ptr target_out = args[1];
    const uint32_t options = args[2];

    if (options != KERNEL_OBJECT_DUPLICATE_SAME_ACCESS) {
        kernel_hle_log()("kernel: NtDuplicateObject(%#x, %#x, Options %#x) REFUSED: only "
                         "Options 2 (DUPLICATE_SAME_ACCESS, INFERRED name) is measured, the "
                         "one site passes the literal 2\n",
                         (unsigned)source, (unsigned)target_out, (unsigned)options);
        return STATUS_NOT_IMPLEMENTED;
    }
    if (target_out == 0u) {
        kernel_hle_log()("kernel: NtDuplicateObject(%#x) REFUSED: a NULL TargetHandle is "
                         "unmeasured (the one site passes the address of a local)\n",
                         (unsigned)source);
        return STATUS_NOT_IMPLEMENTED;
    }
    if (source == 0xFFFFFFFFu || source == 0xFFFFFFFEu) {
        kernel_hle_log()("kernel: NtDuplicateObject(%#x) REFUSED: duplicating a pseudo handle "
                         "(current %s) is unmeasured\n",
                         (unsigned)source, source == 0xFFFFFFFFu ? "process" : "thread");
        return STATUS_NOT_IMPLEMENTED;
    }
    const kernel_object_entry *entry = mutable_find(source);
    if (entry == NULL) {
        kernel_hle_log()("kernel: NtDuplicateObject(%#x) -- not a live handle\n",
                         (unsigned)source);
        return STATUS_INVALID_HANDLE;
    }
    if (entry->kind != KERNEL_OBJECT_FILE) {
        kernel_hle_log()("kernel: NtDuplicateObject(%#x) REFUSED: a %s handle is not "
                         "supported, only FILE is measured and the other kinds need state "
                         "shared between the handles\n",
                         (unsigned)source, kind_name(entry->kind));
        return STATUS_NOT_IMPLEMENTED;
    }
    const uint32_t original = entry->file_dup_of != 0u ? entry->file_dup_of : entry->handle;

    const uint32_t handle = kernel_object_create_nolock(KERNEL_OBJECT_FILE,
                                                        ORD_NtDuplicateObject);
    if (handle == 0u) {
        return KERNEL_OBJECT_STATUS_INSUFFICIENT_RESOURCES;
    }
    mutable_find(handle)->file_dup_of = original;
    if (!kernel_guest_write_u32(target_out, handle)) {
        /* The guest never learns this handle, so nothing will ever close it. */
        close_entry_nolock(mutable_find(handle));
        kernel_hle_log()("kernel: NtDuplicateObject could not write the handle to %#x, "
                         "duplicate released\n",
                         (unsigned)target_out);
        return STATUS_INVALID_PARAMETER;
    }
    kernel_hle_log()("kernel: NtDuplicateObject(%#x) -> FILE handle %#x sharing the open of "
                     "%#x (Options 2 = same access, INFERRED; position and backing are "
                     "shared)\n",
                     (unsigned)source, (unsigned)handle, (unsigned)original);
    return STATUS_SUCCESS;
}

/* __fastcall: the object pointer is in ECX. Measured as 0 stack arguments over 18
 * call sites, which is what a one-argument fastcall looks like from the stack. */
static uint32_t obf_dereference_nolock(void *context)
{
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t object = 0u;
    if (!kernel_frame_reg_arg(frame, 0u, &object)) {
        kernel_hle_log()("kernel: ObfDereferenceObject called with no register arguments\n");
        return 0u;
    }
    kernel_object_entry *entry = find_thread_body(object);
    if (entry != NULL) {
        if (entry->thread_body_references > 0u) {
            entry->thread_body_references--;
            entry->references--;
        }
        return 0u;
    }
    entry = find_file_body_entry(object);
    if (entry != NULL) {
        /* A FILE body reference from ordinal 246. When the guest already closed
         * the handle, this balancing dereference is the LAST reference and the
         * entry and its body go away, exactly as the wrapper at 0x37D14F expects
         * of its unconditional ObfDereferenceObject. A dereference with no
         * outstanding body reference is refused and reported, never allowed to
         * steal the open handle's own reference. */
        if (entry->file_body_references == 0u) {
            kernel_hle_log()("kernel: ObfDereferenceObject(%#x) on a FILE body "
                             "with no outstanding reference -- refused\n",
                             (unsigned)object);
            return 0u;
        }
        entry->file_body_references--;
        entry->references--;
        if (entry->references == 0u) release_file_entry_nolock(entry);
        return 0u;
    }
    entry = mutable_find(object);
    if (entry != NULL && entry->kind != KERNEL_OBJECT_THREAD && entry->kind != KERNEL_OBJECT_EVENT &&
        entry->references > 0u)
        entry->references--;
    return 0u;
}

/*
 * ARITY-OK(246): THREE stack arguments, (Handle, ObjectType, ReturnedObject). The
 * evidence, and why 3 rather than desktop NT's 6, is in kernel_object.h. No ABI_TABLE
 * row is needed: the measured row has 12 unanimous voters and is accepted as it stands.
 *
 * Argument order is now reached at the original GetExitCodeThread wrapper
 * 0x37FD93: (Handle, PsThreadObjectType, &handle_slot). The slot is overwritten
 * with a mapped THREAD body, then the title reads BYTE+4/DWORD+0x120 and passes
 * the same exact body to fastcall ObfDereferenceObject.
 */
static uint32_t ob_reference_by_handle_nolock(void *context)
{
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t handle = 0u;
    uint32_t object_type = 0u;
    uint32_t returned_object = 0u;
    if (!kernel_frame_arg(frame, 0u, &handle) || !kernel_frame_arg(frame, 1u, &object_type)
        || !kernel_frame_arg(frame, 2u, &returned_object)) {
        kernel_hle_log()("kernel: ObReferenceObjectByHandle could not read its three "
                         "arguments from the guest stack\n");
        return STATUS_INVALID_PARAMETER;
    }

    /* NtCurrentThread() is -2 and is resolved by the kernel itself, not issued by the
     * handle table: nxdk's GetCurrentThread returns (HANDLE)-2 and its callers feed it
     * straight to ObReferenceObjectByHandle (lib/winapi/thread.c), and the title does
     * the same through SetThreadPriority/GetExitCodeThread (0x37FC24, 0x37FD83). It
     * resolves to the CALLING thread's own record, so the reference is real, the body
     * is that thread's mapped KTHREAD and the guest's balancing ObfDereferenceObject
     * releases it. The record is found by the thread's own handle even after the
     * title CloseHandle'd it (the body outlives the handle). The boot thread has no
     * record and no KTHREAD here, so it still gets the honest refusal below. */
    bool handle_is_own_thread = false;
    if (handle == 0xFFFFFFFEu) {
        const uint32_t own = kernel_thread_current_identity();
        const kernel_object_entry *own_entry = own != KERNEL_THREAD_IDENTITY_BOOT ? mutable_find_any(own) : NULL;
        if (own_entry == NULL || own_entry->kind != KERNEL_OBJECT_THREAD || own_entry->thread_body == 0u) {
            if (returned_object != 0u)
                kernel_hle_log()("kernel: ObReferenceObjectByHandle(NtCurrentThread) from the "
                                 "boot or a host thread, which has no thread record or KTHREAD "
                                 "here -- REFUSED and ReturnedObject left untouched\n");
            return STATUS_INVALID_HANDLE;
        }
        handle = own;
        handle_is_own_thread = true;
    }
    /* EVENT conversion is unavailable regardless of the output address. No
     * event handle is represented by a dereferenceable surrogate body. */
    const kernel_object_entry *candidate = mutable_find(handle);
    if (candidate != NULL && candidate->kind == KERNEL_OBJECT_EVENT) {
        kernel_hle_log()("kernel: EVENT mapped object body is not implemented\n");
        return STATUS_NOT_IMPLEMENTED;
    }
    /* For supported conversion kinds the out-parameter is checked first, so NULL can
     * never be reported as an invalid handle -- two different bugs with two different
     * fixes, and the real kernel would fault here rather than return. */
    if (returned_object == 0u) {
        kernel_hle_log()("kernel: ObReferenceObjectByHandle(%#x) with a NULL "
                         "ReturnedObject -- REFUSED, there is nowhere to put the "
                         "answer\n",
                         (unsigned)handle);
        return STATUS_INVALID_PARAMETER;
    }

    kernel_object_entry *entry = handle_is_own_thread ? mutable_find_any(handle) : mutable_find(handle);
    if (entry == NULL) {
        /* NOTHING IS WRITTEN on this path. Writing a zero would be worse than writing
         * nothing: the guest checks the STATUS, and a zeroed out-parameter beside a
         * failure status is indistinguishable from a kernel that succeeded and handed
         * back a NULL object. Leaving the caller's slot alone keeps whatever sentinel
         * it put there. */
        kernel_hle_log()("kernel: ObReferenceObjectByHandle(%#x) -- handle was never "
                         "issued by us; REFUSED and ReturnedObject left untouched\n",
                         (unsigned)handle);
        return STATUS_INVALID_HANDLE;
    }

    if (entry->kind == KERNEL_OBJECT_THREAD &&
        (entry->thread_body == 0u || entry->references == UINT32_MAX ||
         entry->thread_body_references == UINT32_MAX)) {
        kernel_hle_log()("kernel: THREAD body unavailable or reference count exhausted\n");
        return STATUS_NOT_IMPLEMENTED;
    }
    for (unsigned i = 0u; i < KERNEL_OBJECT_MAX; i++) {
        if (objects[i].in_use && objects[i].kind == KERNEL_OBJECT_THREAD &&
            objects[i].thread_body != 0u &&
            overlap(returned_object, 4u, objects[i].thread_body, THREAD_BODY_SPAN))
            return STATUS_INVALID_PARAMETER;
    }
    if (entry->kind == KERNEL_OBJECT_THREAD) {
        unsigned char body[THREAD_BODY_SPAN];
        if (!kernel_guest_read_bytes(entry->thread_body, body, sizeof(body)))
            return STATUS_ACCESS_VIOLATION;
    }
    if (object_type != 0u) {
        /* RECORDED, NOT ENFORCED, and counted so the omission is visible. There is no
         * OBJECT_TYPE registry here, so we cannot tell a matching type from a
         * mismatched one. The three types this image actually passes are MEASURED --
         * ordinals 16 `ExEventObjectType`, 71 `IoFileObjectType` and 259
         * `PsThreadObjectType`, all DATA exports whose addresses we do not publish, so
         * we cannot recognise even those. Returning a type mismatch we did not detect
         * would be inventing a failure. */
        unchecked_type_count++;
        kernel_hle_log()("kernel: ObReferenceObjectByHandle(%#x) supplied ObjectType "
                         "%#x, which is NOT checked -- this host has no OBJECT_TYPE "
                         "registry, so a mismatched handle would be accepted\n",
                         (unsigned)handle, (unsigned)object_type);
    }

    /* What gets published: a THREAD's mapped body, a FILE's fabricated body when
     * the provider built one (the wrapper's own reads at 0x37D1A9..0x37D1C8 land
     * on it), and otherwise the legacy handle surrogate. */
    uint32_t published = entry->handle;
    if (entry->kind == KERNEL_OBJECT_THREAD) published = entry->thread_body;
    else if (entry->kind == KERNEL_OBJECT_FILE && entry->file_body != 0u)
        published = entry->file_body;

    /* Write the answer BEFORE taking the reference, so a write that fails cannot leave
     * a reference the guest has no pointer to and will therefore never release. */
    if (!kernel_guest_write_u32((kernel_guest_ptr)returned_object, published)) {
        kernel_hle_log()("kernel: ObReferenceObjectByHandle(%#x) could not write the "
                         "object pointer to %#x -- REFUSED, and no reference taken\n",
                         (unsigned)handle, (unsigned)returned_object);
        return STATUS_INVALID_PARAMETER;
    }

    entry->references++;
    if (entry->kind == KERNEL_OBJECT_THREAD) entry->thread_body_references++;
    else if (entry->kind == KERNEL_OBJECT_FILE && entry->file_body != 0u)
        entry->file_body_references++;
    reference_by_handle_count++;
    return STATUS_SUCCESS;
}

static uint32_t obf_reference_nolock(void *context)
{
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t object = 0u;
    if (!kernel_frame_reg_arg(frame, 0u, &object)) {
        kernel_hle_log()("kernel: ObfReferenceObject called with no register arguments\n");
        return 0u;
    }
    kernel_object_entry *entry = find_thread_body(object);
    if (entry != NULL) {
        if (entry->thread_body_references > 0u && entry->references < UINT32_MAX &&
            entry->thread_body_references < UINT32_MAX) {
            entry->thread_body_references++;
            entry->references++;
        } else kernel_hle_log()("kernel: THREAD body reference refused\n");
        return 0u;
    }
    entry = find_file_body_entry(object);
    if (entry != NULL) {
        if (entry->references < UINT32_MAX && entry->file_body_references < UINT32_MAX) {
            entry->file_body_references++;
            entry->references++;
        } else kernel_hle_log()("kernel: FILE body reference refused\n");
        return 0u;
    }
    entry = mutable_find(object);
    if (entry != NULL && entry->kind != KERNEL_OBJECT_THREAD &&
        entry->kind != KERNEL_OBJECT_EVENT) entry->references++;
    return 0u;
}


/* --- the locked public surface ------------------------------------------- */

void kernel_object_reset(void)
{
    object_enter();
    kernel_object_reset_nolock();
    object_leave();
}

uint32_t kernel_object_bad_close_count(void)
{
    object_enter();
    const uint32_t result = kernel_object_bad_close_count_nolock();
    object_leave();
    return result;
}

unsigned kernel_object_live_count(void)
{
    object_enter();
    const unsigned result = kernel_object_live_count_nolock();
    object_leave();
    return result;
}

uint32_t kernel_object_create(kernel_object_kind kind, uint32_t owner_tag)
{
    object_enter();
    const uint32_t result = kernel_object_create_nolock(kind, owner_tag);
    object_leave();
    return result;
}

unsigned kernel_object_retired_count(void)
{
    object_enter();
    const unsigned result = retired_count;
    object_leave();
    return result;
}

bool kernel_object_release(uint32_t handle)
{
    object_enter();
    kernel_object_entry *entry = mutable_find(handle);
    if (entry != NULL) {
        close_entry_nolock(entry);
    }
    object_leave();
    return entry != NULL;
}

const kernel_object_entry *kernel_object_find(uint32_t handle)
{
    object_enter();
    const kernel_object_entry *result = kernel_object_find_nolock(handle);
    object_leave();
    return result;
}

static bool overlap(uint32_t left, uint32_t left_size, uint32_t right, uint32_t right_size)
{
    return (uint64_t)left < (uint64_t)right + right_size &&
           (uint64_t)right < (uint64_t)left + left_size;
}
static bool bind_thread_body_nolock(uint32_t handle, uint32_t owner_tag, uint32_t body)
{
    kernel_object_entry *entry = mutable_find(handle);
    unsigned char bytes[THREAD_BODY_SPAN];
    if (entry == NULL || entry->kind != KERNEL_OBJECT_THREAD ||
        entry->owner_tag != owner_tag || entry->thread_body != 0u ||
        entry->thread_body_references != 0u || body == 0u || (body & 3u) != 0u ||
        (uint64_t)body + THREAD_BODY_SPAN > UINT64_C(0x100000000) ||
        !kernel_guest_read_bytes(body, bytes, sizeof(bytes))) return false;
    for (unsigned i = 0u; i < KERNEL_OBJECT_MAX; i++) {
        if (objects[i].in_use && objects[i].kind == KERNEL_OBJECT_THREAD &&
            objects[i].thread_body != 0u &&
            overlap(body, THREAD_BODY_SPAN, objects[i].thread_body, THREAD_BODY_SPAN))
            return false;
    }
    entry->thread_body = body;
    return true;
}
bool kernel_object_bind_thread_body(uint32_t handle, uint32_t owner_tag, uint32_t body)
{
    object_enter();
    const bool result = bind_thread_body_nolock(handle, owner_tag, body);
    object_leave();
    return result;
}
bool kernel_object_publish_thread_body(uint32_t handle, uint32_t owner_tag,
                                       uint32_t body, uint32_t handle_out)
{
    object_enter();
    bool result = handle_out != 0u && bind_thread_body_nolock(handle, owner_tag, body);
    if (result) {
        for (unsigned i = 0u; i < KERNEL_OBJECT_MAX; i++) {
            if (objects[i].in_use && objects[i].kind == KERNEL_OBJECT_THREAD &&
                objects[i].thread_body != 0u &&
                overlap(handle_out, 4u, objects[i].thread_body, THREAD_BODY_SPAN)) {
                result = false;
                break;
            }
        }
        if (result) result = kernel_guest_write_u32(handle_out, handle);
        if (!result) mutable_find_any(handle)->thread_body = 0u;
    }
    object_leave();
    return result;
}
bool kernel_object_write_optional_thread_id(uint32_t out, uint32_t id)
{
    object_enter();
    bool result = out != 0u;
    for (unsigned i = 0u; result && i < KERNEL_OBJECT_MAX; i++) {
        if (objects[i].in_use && objects[i].kind == KERNEL_OBJECT_THREAD &&
            objects[i].thread_body != 0u &&
            overlap(out, 4u, objects[i].thread_body, THREAD_BODY_SPAN)) result = false;
    }
    if (result) result = kernel_guest_write_u32(out, id);
    object_leave();
    return result;
}
nt_status kernel_object_create_event(uint32_t type, uint32_t initial, uint32_t out)
{
    /* Type 0 NotificationEvent (manual reset, T270) and Type 1 SynchronizationEvent (auto reset).
     * Any other EVENT_TYPE, and an initially signaled event, are unmeasured and refused. */
    if ((type != 0u && type != 1u) || (initial & 0xFFu) != 0u) return STATUS_NOT_IMPLEMENTED;
    object_enter();
    uint32_t previous;
    if (out == 0u || !kernel_guest_read_u32(out, &previous)) {
        object_leave();
        return STATUS_INVALID_PARAMETER;
    }
    for (unsigned i = 0u; i < KERNEL_OBJECT_MAX; i++) {
        if (objects[i].in_use && objects[i].kind == KERNEL_OBJECT_THREAD &&
            objects[i].thread_body != 0u &&
            overlap(out, 4u, objects[i].thread_body, THREAD_BODY_SPAN)) {
            object_leave();
            return STATUS_INVALID_PARAMETER;
        }
    }
    /* Same-bytes write probes all four output bytes before allocation. A short
     * transfer leaves the copied prefix equal to its original value. Mappings
     * and the output slot must stay quiescent through publication; this is a
     * guest write, not a permission-only query or an atomic compound guarantee. */
    if (!kernel_guest_write_u32(out, previous)) {
        object_leave();
        return STATUS_INVALID_PARAMETER;
    }
    const uint32_t handle = kernel_object_create_nolock(KERNEL_OBJECT_EVENT, 189u);
    if (handle == 0u) {
        object_leave();
        return KERNEL_OBJECT_STATUS_INSUFFICIENT_RESOURCES;
    }
    kernel_object_entry *entry = mutable_find(handle);
    entry->event_type = type;
    entry->event_initial_state = initial & 0xFFu;
    entry->event_signaled = (initial & 0xFFu) != 0u;
    if (!kernel_guest_write_u32(out, handle)) {
        kernel_object_release_nolock(entry);
        object_leave();
        return STATUS_INVALID_PARAMETER;
    }
    object_leave();
    return STATUS_SUCCESS;
}

nt_status kernel_object_event_set(uint32_t handle, bool *previous)
{
    object_enter();
    kernel_object_entry *entry = mutable_find(handle);
    nt_status status = STATUS_SUCCESS;
    if (entry == NULL) {
        status = STATUS_INVALID_HANDLE;
    } else if (entry->kind != KERNEL_OBJECT_EVENT) {
        status = STATUS_OBJECT_TYPE_MISMATCH;
    } else {
        if (previous != NULL) *previous = entry->event_signaled;
        entry->event_signaled = true;
    }
    object_leave();
    return status;
}

nt_status kernel_object_event_clear(uint32_t handle)
{
    object_enter();
    kernel_object_entry *entry = mutable_find(handle);
    nt_status status = STATUS_SUCCESS;
    if (entry == NULL) {
        status = STATUS_INVALID_HANDLE;
    } else if (entry->kind != KERNEL_OBJECT_EVENT) {
        status = STATUS_OBJECT_TYPE_MISMATCH;
    } else {
        entry->event_signaled = false;
    }
    object_leave();
    return status;
}

bool kernel_object_event_signaled(uint32_t handle, bool *out)
{
    object_enter();
    const kernel_object_entry *entry = mutable_find(handle);
    const bool result = entry != NULL && entry->kind == KERNEL_OBJECT_EVENT;
    if (result && out != NULL) *out = entry->event_signaled;
    object_leave();
    return result;
}

nt_status kernel_object_event_try_wait(uint32_t handle, bool *acquired)
{
    if (acquired != NULL) *acquired = false;
    object_enter();
    kernel_object_entry *entry = mutable_find(handle);
    nt_status status = STATUS_SUCCESS;
    if (entry == NULL) {
        status = STATUS_INVALID_HANDLE;
    } else if (entry->kind != KERNEL_OBJECT_EVENT) {
        status = STATUS_OBJECT_TYPE_MISMATCH;
    } else if (entry->event_signaled) {
        /* Type 1 is NT's SynchronizationEvent: a satisfied wait resets it. */
        if (entry->event_type == 1u) entry->event_signaled = false;
        if (acquired != NULL) *acquired = true;
    }
    object_leave();
    return status;
}

nt_status kernel_object_mutant_try_acquire(uint32_t handle, uint32_t owner, bool *acquired)
{
    if (acquired != NULL) *acquired = false;
    object_enter();
    kernel_object_entry *entry = mutable_find(handle);
    nt_status status = STATUS_SUCCESS;
    if (entry == NULL) {
        status = STATUS_INVALID_HANDLE;
    } else if (entry->kind != KERNEL_OBJECT_MUTANT) {
        status = STATUS_OBJECT_TYPE_MISMATCH;
    } else if (entry->mutant_count == 0u) {
        entry->mutant_owner = owner;
        entry->mutant_count = 1u;
        if (acquired != NULL) *acquired = true;
    } else if (entry->mutant_owner == owner) {
        if (entry->mutant_count >= KERNEL_OBJECT_MUTANT_RECURSION_MAX) {
            status = STATUS_MUTANT_LIMIT_EXCEEDED;
        } else {
            entry->mutant_count++;
            if (acquired != NULL) *acquired = true;
        }
    }
    object_leave();
    return status;
}

/* Caller holds the object lock. */
static nt_status mutant_release_nolock(kernel_object_entry *entry, uint32_t owner,
                                       uint32_t *previous)
{
    if (entry->mutant_count == 0u || entry->mutant_owner != owner) {
        return STATUS_MUTANT_NOT_OWNED;
    }
    if (previous != NULL) *previous = entry->mutant_count;
    entry->mutant_count--;
    if (entry->mutant_count == 0u) entry->mutant_owner = 0u;
    return STATUS_SUCCESS;
}

nt_status kernel_object_mutant_release(uint32_t handle, uint32_t owner, uint32_t *previous)
{
    object_enter();
    kernel_object_entry *entry = mutable_find(handle);
    nt_status status;
    if (entry == NULL) {
        status = STATUS_INVALID_HANDLE;
    } else if (entry->kind != KERNEL_OBJECT_MUTANT) {
        status = STATUS_OBJECT_TYPE_MISMATCH;
    } else {
        status = mutant_release_nolock(entry, owner, previous);
    }
    object_leave();
    return status;
}

bool kernel_object_mutant_state(uint32_t handle, uint32_t *owner, uint32_t *count)
{
    object_enter();
    const kernel_object_entry *entry = mutable_find(handle);
    const bool result = entry != NULL && entry->kind == KERNEL_OBJECT_MUTANT;
    if (result) {
        if (owner != NULL) *owner = entry->mutant_owner;
        if (count != NULL) *count = entry->mutant_count;
    }
    object_leave();
    return result;
}

bool kernel_object_get_thread_body_copy(uint32_t body, kernel_object_entry *out)
{
    if (out == NULL) return false;
    object_enter();
    const kernel_object_entry *entry = find_thread_body(body);
    const bool result = entry != NULL && entry->thread_body_references != 0u;
    if (result) *out = *entry;
    object_leave();
    return result;
}
bool kernel_object_rollback_thread_body(uint32_t handle, uint32_t owner_tag)
{
    object_enter();
    kernel_object_entry *entry = mutable_find_any(handle);
    const bool result = entry != NULL && entry->kind == KERNEL_OBJECT_THREAD &&
                        entry->owner_tag == owner_tag && entry->thread_body_references == 0u;
    if (result) kernel_object_release_nolock(entry);
    object_leave();
    return result;
}
bool kernel_object_detach_thread_bodies(void)
{
    object_enter();
    for (unsigned i = 0u; i < KERNEL_OBJECT_MAX; i++) {
        if (objects[i].in_use && objects[i].kind == KERNEL_OBJECT_THREAD &&
            objects[i].thread_body != 0u && objects[i].thread_body_references != 0u) {
            object_leave();
            return false;
        }
    }
    for (unsigned i = 0u; i < KERNEL_OBJECT_MAX; i++) {
        if (objects[i].in_use && objects[i].kind == KERNEL_OBJECT_THREAD &&
            objects[i].thread_body != 0u) kernel_object_release_nolock(&objects[i]);
    }
    object_leave();
    return true;
}

bool kernel_object_get_copy(uint32_t handle, kernel_object_entry *out)
{
    if (!out) {
        return false;
    }
    object_enter();
    const kernel_object_entry *entry = kernel_object_find_nolock(handle);
    const bool found = entry != NULL;
    if (found) {
        *out = *entry;
    }
    object_leave();
    return found;
}

static uint32_t handle_nt_close(void *context)
{
    object_enter();
    const uint32_t result = nt_close_nolock(context);
    object_leave();
    return result;
}

static uint32_t handle_nt_create_mutant(void *context)
{
    object_enter();
    const uint32_t result = nt_create_mutant_nolock(context);
    object_leave();
    return result;
}

static uint32_t handle_nt_release_mutant(void *context)
{
    object_enter();
    const uint32_t result = nt_release_mutant_nolock(context);
    object_leave();
    return result;
}

static uint32_t handle_nt_duplicate_object(void *context)
{
    object_enter();
    const uint32_t result = nt_duplicate_object_nolock(context);
    object_leave();
    return result;
}

static uint32_t handle_obf_dereference_object(void *context)
{
    object_enter();
    const uint32_t result = obf_dereference_nolock(context);
    object_leave();
    return result;
}

static uint32_t handle_obf_reference_object(void *context)
{
    object_enter();
    const uint32_t result = obf_reference_nolock(context);
    object_leave();
    return result;
}

static uint32_t handle_ob_reference_object_by_handle(void *context)
{
    /* FILE bodies are fabricated OUTSIDE the table lock: the provider consults
     * kernel_file, whose own lock order is file -> object, so running it under
     * the object lock would invert that order. It binds the body back through
     * kernel_object_bind_file_body, and a handle that dies in the gap is
     * re-judged under the lock below as it always was. */
    if (file_body_ops.fabricate != NULL && context != NULL) {
        const kernel_call_frame *frame = (const kernel_call_frame *)context;
        uint32_t handle = 0u;
        kernel_object_entry copy;
        if (kernel_frame_arg(frame, 0u, &handle) &&
            kernel_object_get_copy(handle, &copy) &&
            copy.kind == KERNEL_OBJECT_FILE && copy.file_body == 0u) {
            (void)file_body_ops.fabricate(handle);
        }
    }
    object_enter();
    const uint32_t result = ob_reference_by_handle_nolock(context);
    object_leave();
    return result;
}

void kernel_object_set_file_body_ops(kernel_object_file_body_ops ops)
{
    /* Registration-time, like kernel_hle_register: not guarded against concurrent
     * dispatch, because nothing dispatches before registration completes. */
    file_body_ops = ops;
}

bool kernel_object_bind_file_body(uint32_t handle, uint32_t body, uint32_t fs_name)
{
    object_enter();
    kernel_object_entry *entry = mutable_find(handle);
    const bool result = entry != NULL && entry->kind == KERNEL_OBJECT_FILE &&
                        entry->file_body == 0u && body != 0u && (body & 3u) == 0u &&
                        find_file_body_entry(body) == NULL;
    if (result) {
        entry->file_body = body;
        entry->file_fs_name = fs_name;
        entry->file_body_references = 0u;
    }
    object_leave();
    return result;
}

bool kernel_object_find_file_body(uint32_t body, uint32_t *out_handle,
                                  uint32_t *out_fs_name)
{
    object_enter();
    const kernel_object_entry *entry = find_file_body_entry(body);
    const bool result = entry != NULL;
    if (result) {
        if (out_handle != NULL) *out_handle = entry->handle;
        if (out_fs_name != NULL) *out_fs_name = entry->file_fs_name;
    }
    object_leave();
    return result;
}

uint32_t kernel_object_reference_by_handle_count(void)
{
    object_enter();
    const uint32_t result = reference_by_handle_count;
    object_leave();
    return result;
}

uint32_t kernel_object_file_identity(uint32_t handle)
{
    object_enter();
    const kernel_object_entry *entry = mutable_find(handle);
    /* Only a FILE duplicate carries a nonzero file_dup_of. */
    const uint32_t identity = (entry != NULL && entry->file_dup_of != 0u) ? entry->file_dup_of
                                                                            : handle;
    object_leave();
    return identity;
}

bool kernel_object_file_identity_live(uint32_t identity)
{
    object_enter();
    bool live = mutable_find(identity) != NULL;
    /* 0 is the "not a duplicate" marker of every ORIGINAL entry, so it names no identity. */
    for (unsigned i = 0u; !live && identity != 0u && i < KERNEL_OBJECT_MAX; i++) {
        /* A released entry is zeroed whole, so only a live FILE duplicate carries a nonzero
         * file_dup_of. */
        live = objects[i].file_dup_of == identity;
    }
    object_leave();
    return live;
}

unsigned kernel_object_mutant_unowned_release_count(void)
{
    object_enter();
    const unsigned result = mutant_unowned_release_count;
    object_leave();
    return result;
}

uint32_t kernel_object_unchecked_type_count(void)
{
    object_enter();
    const uint32_t result = unchecked_type_count;
    object_leave();
    return result;
}

unsigned kernel_object_register(void)
{
    static const struct {
        unsigned ordinal;
        kernel_fn handler;
    } bindings[] = {
        {ORD_NtClose, handle_nt_close},
        {ORD_NtCreateMutant, handle_nt_create_mutant},
        {ORD_NtReleaseMutant, handle_nt_release_mutant},
        {ORD_NtDuplicateObject, handle_nt_duplicate_object},
        {ORD_ObReferenceObjectByHandle, handle_ob_reference_object_by_handle},
        {ORD_ObfReferenceObject, handle_obf_reference_object},
        {ORD_ObfDereferenceObject, handle_obf_dereference_object},
    };

    unsigned bound = 0u;
    for (size_t i = 0u; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
        if (kernel_hle_register(bindings[i].ordinal, bindings[i].handler)) {
            bound++;
        }
    }
    return bound;
}
