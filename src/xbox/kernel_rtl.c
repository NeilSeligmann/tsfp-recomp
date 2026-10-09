/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See kernel_rtl.h for the verified ordinal numbers and the ordinals deliberately
 * left unbound.
 *
 * SIGNATURES, WITH THE STACK-ARGUMENT COUNT MEASURED FROM THIS IMAGE:
 *
 *   VOID    __stdcall RtlInitAnsiString(PANSI_STRING DestinationString,
 *                                       PCSZ SourceString);      // 2 stack args
 *   ULONG   __stdcall RtlNtStatusToDosError(NTSTATUS Status);     // 1 stack arg
 *   BOOLEAN __stdcall RtlEqualString(const STRING *String1,
 *                                    const STRING *String2,
 *                                    BOOLEAN CaseInSensitive);   // 3 stack args
 *
 * None is in the `Kf*` or `Obf*` fastcall families, so all three take every argument
 * on the stack. See src/xbox/kernel_call.h. The DEF-file decorations agree and are
 * mechanical about it: `RtlInitAnsiString@8`, `RtlNtStatusToDosError@4`,
 * `RtlEqualString@12`, none with a LEADING `@` -- see kernel_rtl.h ARITY-OK(279).
 */

#include "kernel_rtl.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "guest_structs.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "nt_status.h"

#define ORD_RtlCompareMemoryUlong 269u
#define ORD_RtlEqualString 279u
#define ORD_RtlInitAnsiString 289u
#define ORD_RtlNtStatusToDosError 301u
#define ORD_RtlRaiseException 302u
#define ORD_RtlTimeFieldsToTime 304u
#define ORD_RtlTimeToTimeFields 305u

/* ---------------------------------------------------------------------------
 * THE LOCK.
 *
 * Both handlers are pure functions of their arguments, so the TRANSLATION needs no
 * lock. The COUNTERS do: two guest threads run, `unmapped_count++` is a non-atomic
 * read-modify-write, and a lost increment makes the one diagnostic that says "a
 * status you did not anticipate is now live" under-report. `last_unmapped` is a
 * separate word from the counter, so without a lock a reader can see a count of 1
 * with a stale status beside it and go looking for the wrong value.
 *
 * RECURSIVE, following kernel_object.c and guest_mem.c, because the critical
 * section calls kernel_hle_log() and the sink is caller-supplied.
 * ------------------------------------------------------------------------- */

static pthread_mutex_t rtl_lock;
static pthread_once_t rtl_lock_once = PTHREAD_ONCE_INIT;

static void rtl_lock_init(void)
{
    pthread_mutexattr_t attr;
    (void)pthread_mutexattr_init(&attr);
    (void)pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    (void)pthread_mutex_init(&rtl_lock, &attr);
    (void)pthread_mutexattr_destroy(&attr);
}

static void rtl_enter(void)
{
    (void)pthread_once(&rtl_lock_once, rtl_lock_init);
    (void)pthread_mutex_lock(&rtl_lock);
}

static void rtl_leave(void)
{
    (void)pthread_mutex_unlock(&rtl_lock);
}

static uint32_t unmapped_count;
static nt_status last_unmapped;
static uint32_t truncated_count;
static uint32_t nonascii_fold_count;
static uint8_t last_nonascii_fold;
static uint32_t compare_refused_count;
static uint32_t time_refused_count;

/* ===========================================================================
 * RtlNtStatusToDosError (ordinal 301, 28 call sites)
 * ===========================================================================
 *
 * WHY THIS TABLE IS SHORT, AND WHY IT MUST STAY SHORT.
 *
 * The real function carries hundreds of entries. Reproducing that from memory is
 * precisely the failure this project keeps being bitten by: a wrong mapping does
 * not fault, the guest simply takes the wrong error branch, and the symptom appears
 * somewhere else entirely. There is no test that can catch a mapping we invented,
 * because the expected value would be invented too. So a row exists here ONLY when
 * BOTH halves of it can be sourced:
 *
 *   1. the STATUS is evidenced in this project, and
 *   2. the WIN32 NUMBER comes from something better than recollection.
 *
 * A status that fails (2) deliberately falls through to the fallback and is LOGGED,
 * even when we hold an opinion about what it ought to map to. That is the whole
 * discipline: five sourced rows plus a loud fallback beats twelve rows of which
 * seven are guesses, because the guesses are the ones that fail silently.
 *
 * WHERE THE WIN32 NUMBERS COME FROM. `docs/provenance.md` lists
 * `sp00nznet/xboxrecomp` (MIT) as an accepted source for kernel ordinal tables, and
 * its own ordinal-301 bridge carries this same mapping with the same 317 default.
 * Every number below is corroborated against it. That is a licence-compatible,
 * already-approved path, and it is NOT a Microsoft XDK header, which
 * docs/provenance.md forbids outright. Attribution is in docs/provenance.md's
 * third-party table.
 *
 * WHAT THE INPUTS ACTUALLY ARE. At 31 of the 32 call sites the argument is a
 * runtime value -- the status a preceding Nt* or Mm* call returned -- so the table
 * cannot be derived from the call sites at all; they name no status. It must instead
 * cover what OUR handlers return, which is what nt_status.h enumerates. One closed
 * evidence chain was traced end to end: guest sub_0037CB7D sign-tests the result of
 * sub_0037CA38, which returns STATUS_NO_MEMORY (0xC0000017 at recomp_0043.c:25692)
 * or propagates the status of ordinals 165 and 178 -- both of which we implement --
 * and hands it straight to ordinal 301.
 *
 * THE FALLBACK IS LOAD-BEARING, NOT THEORETICAL. One site passes a hard-coded
 * literal: recomp_0061.c:29785 pushes 0x80072747, which is not a valid NTSTATUS at
 * all (it is HRESULT-shaped, facility 7) and sits in the socket/XOnline code. So the
 * unmapped path is exercised by the image itself on a constant input.
 *
 * WHY THE FALLBACK MUST BE NONZERO AND POSITIVE. 16 of the 32 sites do nothing with
 * the result but compare it against zero, and 4 of those use a SIGNED `jg`. So a
 * fallback of 0 would report success for a failure, and a fallback with the high bit
 * set would test as "not greater than zero" and take the success branch too. 317 is
 * nonzero and positive, and at those 16 sites it is therefore not merely a
 * placeholder -- it is branch-equivalent to the true answer.
 */

/* STATUS_PENDING. Not in nt_status.h, and MEASURED in this image rather than
 * recalled: src/xbox/guest_structs.h records callers pre-setting an
 * IO_STATUS_BLOCK's status field to 0x103 and sign-testing it back on the async
 * path, at `0x0037CC49  mov dword ptr [esi], edi  ; edi = 0x103`. */
#define KERNEL_RTL_STATUS_PENDING 0x00000103u

typedef struct {
    nt_status status;
    uint32_t dos_error;
    const char *status_name;
    const char *error_name;
} status_mapping;

static const status_mapping STATUS_MAP[] = {
    {STATUS_SUCCESS, KERNEL_RTL_ERROR_SUCCESS, "STATUS_SUCCESS", "ERROR_SUCCESS"},
    {STATUS_INVALID_HANDLE, 6u, "STATUS_INVALID_HANDLE", "ERROR_INVALID_HANDLE"},
    {STATUS_NO_MEMORY, 8u, "STATUS_NO_MEMORY", "ERROR_NOT_ENOUGH_MEMORY"},
    {STATUS_INVALID_PARAMETER, 87u, "STATUS_INVALID_PARAMETER", "ERROR_INVALID_PARAMETER"},
    /* INFERRED, not measured: NT's RtlNtStatusToDosError maps STATUS_INVALID_INFO_CLASS
     * (0xC0000003) to ERROR_INVALID_PARAMETER. Safe at the compare-against-zero sites
     * (87 is nonzero and positive, like 317). Produced by NtQueryInformationFile for any
     * class this project does not answer. */
    {0xC0000003u, 87u, "STATUS_INVALID_INFO_CLASS", "ERROR_INVALID_PARAMETER"},

    /* STATUS_PENDING IS THE ONE ROW THAT IS NOT OPTIONAL, and it is here because of
     * a recorded failure rather than for completeness. It is an INFORMATIONAL status
     * (high bit clear), so it is not a failure at all, and letting it reach the
     * generic fallback answers "is this request still in flight?" with "there is no
     * message text for that number".
     *
     * The accepted upstream records what that costs: Shin Megami Tensei: Nine's
     * resource loader issues a read, asks for the error, and marks the object as
     * loading ONLY when the answer is ERROR_IO_PENDING. With 317 the object stayed
     * idle and the title sat in its first boot state forever with everything else
     * working -- a hang with no crash and no diagnostic, arbitrarily far from the
     * cause. STATUS_PENDING is measured in THIS image's IO_STATUS_BLOCK handling,
     * so the same trap is reachable here. */
    {KERNEL_RTL_STATUS_PENDING, 997u, "STATUS_PENDING", "ERROR_IO_PENDING"},

    /* Measured live: TimeSplitters Future Perfect opens "\pak\overlay.pak", which is
     * not on its disc, gets 0xC0000034 and passes it to this function. The
     * NTSTATUS to Win32 pair is the documented NT table (MS-ERREF, nxdk). */
    {0xC0000034u, 2u, "STATUS_OBJECT_NAME_NOT_FOUND", "ERROR_FILE_NOT_FOUND"},

    /* FILE/KERNEL-LAYER STATUSES (T1214). INFERRED, not measured: the standard NT
     * NTSTATUS to Win32 table (the one ntdll RtlNtStatusToDosError and nxdk/ReactOS
     * and xemu's guest kernel share). The Xbox kernel table lives in the kernel image,
     * not the XBE, and no xemu probe has measured these rows. 0xC0000035 is the one
     * that matters: Map Maker's save-over issues FILE_CREATE on an existing name, gets
     * the collision and asks for the error; the real table answers 183 and 317 broke
     * the title's branch. Safe at the compare-against-zero sites (all nonzero, positive). */
    {0x80000005u, 234u, "STATUS_BUFFER_OVERFLOW", "ERROR_MORE_DATA"},
    {0x80000006u, 18u, "STATUS_NO_MORE_FILES", "ERROR_NO_MORE_FILES"},
    {0xC0000004u, 24u, "STATUS_INFO_LENGTH_MISMATCH", "ERROR_BAD_LENGTH"},
    {0xC000000Fu, 2u, "STATUS_NO_SUCH_FILE", "ERROR_FILE_NOT_FOUND"},
    {0xC0000010u, 1u, "STATUS_INVALID_DEVICE_REQUEST", "ERROR_INVALID_FUNCTION"},
    {0xC0000011u, 38u, "STATUS_END_OF_FILE", "ERROR_HANDLE_EOF"},
    {0xC0000022u, 5u, "STATUS_ACCESS_DENIED", "ERROR_ACCESS_DENIED"},
    {0xC0000023u, 122u, "STATUS_BUFFER_TOO_SMALL", "ERROR_INSUFFICIENT_BUFFER"},
    {0xC0000024u, 6u, "STATUS_OBJECT_TYPE_MISMATCH", "ERROR_INVALID_HANDLE"},
    {0xC0000033u, 123u, "STATUS_OBJECT_NAME_INVALID", "ERROR_INVALID_NAME"},
    {0xC0000035u, 183u, "STATUS_OBJECT_NAME_COLLISION", "ERROR_ALREADY_EXISTS"},
    {0xC000003Au, 3u, "STATUS_OBJECT_PATH_NOT_FOUND", "ERROR_PATH_NOT_FOUND"},
    {0xC0000043u, 32u, "STATUS_SHARING_VIOLATION", "ERROR_SHARING_VIOLATION"},
    {0xC000007Fu, 112u, "STATUS_DISK_FULL", "ERROR_DISK_FULL"},
    {0xC000009Au, 1450u, "STATUS_INSUFFICIENT_RESOURCES", "ERROR_NO_SYSTEM_RESOURCES"},
    {0xC00000BAu, 5u, "STATUS_FILE_IS_A_DIRECTORY", "ERROR_ACCESS_DENIED"},
    {0xC0000101u, 145u, "STATUS_DIRECTORY_NOT_EMPTY", "ERROR_DIR_NOT_EMPTY"},
    {0xC0000103u, 267u, "STATUS_NOT_A_DIRECTORY", "ERROR_DIRECTORY"},
    {0xC000014Fu, 1005u, "STATUS_UNRECOGNIZED_VOLUME", "ERROR_UNRECOGNIZED_VOLUME"},
};

/*
 * DELIBERATELY NOT MAPPED, though our own handlers return every one of them:
 * STATUS_UNSUCCESSFUL, STATUS_NOT_IMPLEMENTED, STATUS_ACCESS_VIOLATION,
 * STATUS_CONFLICTING_ADDRESSES, STATUS_INVALID_PAGE_PROTECTION,
 * STATUS_FREE_VM_NOT_AT_BASE and STATUS_MEMORY_NOT_ALLOCATED.
 *
 * Each of these I could offer a plausible Win32 number for from general knowledge,
 * and not one of them is corroborated by the accepted upstream or by anything in
 * this image. Writing them down would convert seven guesses into seven rows that
 * look exactly as authoritative as the five above, and nothing downstream could
 * ever tell them apart. They therefore take the fallback and LOG, which is both
 * branch-correct at the 16 compare-against-zero sites and self-announcing at the
 * rest. The log line names the status, so the first run that actually needs one
 * tells us which to go and source properly.
 *
 * The same applies to the 20-odd further statuses measured in the guest's own code
 * (0xC000009A is the most common in the image at 37 occurrences, then 0xC000006D at
 * 16) whose Win32 numbers this project cannot source. They are listed in the task
 * report rather than guessed at here.
 */

#define STATUS_MAP_COUNT (sizeof(STATUS_MAP) / sizeof(STATUS_MAP[0]))

/* The statuses this project PRODUCES but cannot source a Win32 number for. Kept so
 * the fallback diagnostic can name them: "STATUS_ACCESS_VIOLATION is deliberately
 * unmapped" is actionable, where a bare 0xC0000005 sends the next person back to
 * nt_status.h to find out what they are even looking at. */
static const status_mapping UNSOURCED[] = {
    {STATUS_UNSUCCESSFUL, 0u, "STATUS_UNSUCCESSFUL", NULL},
    {STATUS_NOT_IMPLEMENTED, 0u, "STATUS_NOT_IMPLEMENTED", NULL},
    {STATUS_ACCESS_VIOLATION, 0u, "STATUS_ACCESS_VIOLATION", NULL},
    {STATUS_CONFLICTING_ADDRESSES, 0u, "STATUS_CONFLICTING_ADDRESSES", NULL},
    {STATUS_INVALID_PAGE_PROTECTION, 0u, "STATUS_INVALID_PAGE_PROTECTION", NULL},
    {STATUS_FREE_VM_NOT_AT_BASE, 0u, "STATUS_FREE_VM_NOT_AT_BASE", NULL},
    {STATUS_MEMORY_NOT_ALLOCATED, 0u, "STATUS_MEMORY_NOT_ALLOCATED", NULL},
};

static const char *unsourced_status_name(nt_status status)
{
    for (size_t i = 0u; i < sizeof(UNSOURCED) / sizeof(UNSOURCED[0]); i++) {
        if (UNSOURCED[i].status == status) {
            return UNSOURCED[i].status_name;
        }
    }
    return NULL;
}

uint32_t kernel_rtl_status_to_dos_error(nt_status status)
{
    for (size_t i = 0u; i < STATUS_MAP_COUNT; i++) {
        if (STATUS_MAP[i].status == status) {
            return STATUS_MAP[i].dos_error;
        }
    }

    /* UNMAPPED. Deliberately not guessed. Reported loudly and counted, because the
     * whole point of a short table is that growing it is a visible, evidenced act
     * rather than an accumulation of plausible-looking rows. */
    rtl_enter();
    unmapped_count++;
    last_unmapped = status;
    const char *known = unsourced_status_name(status);
    if (known) {
        /* We know what it is called and that our own handlers produce it; what we do
         * not have is a sourceable Win32 number. Saying so is the difference between
         * an actionable report and a bare hex value. */
        kernel_hle_log()("kernel: RtlNtStatusToDosError(%s = %#x) -- this project "
                         "produces this status but cannot source its Win32 number, so "
                         "it is deliberately unmapped; returning "
                         "ERROR_MR_MID_NOT_FOUND (%u). It is now live: source the "
                         "real value and add a row in kernel_rtl.c\n",
                         known, status, KERNEL_RTL_ERROR_MR_MID_NOT_FOUND);
    } else {
        kernel_hle_log()("kernel: RtlNtStatusToDosError(%#x) has no mapping -- "
                         "returning ERROR_MR_MID_NOT_FOUND (%u). %s\n",
                         status, KERNEL_RTL_ERROR_MR_MID_NOT_FOUND,
                         nt_success(status)
                             ? "NOTE: the high bit is CLEAR, so this is a success or "
                               "informational status, not a failure -- the generic "
                               "fallback may be the wrong KIND of answer here (see "
                               "the STATUS_PENDING note above)"
                             : "An unanticipated failure status is now live");
    }
    rtl_leave();
    return KERNEL_RTL_ERROR_MR_MID_NOT_FOUND;
}

/* ===========================================================================
 * RtlInitAnsiString (ordinal 289, 22 call sites)
 * ===========================================================================
 *
 * THE BEHAVIOUR IS MEASURED, NOT RECALLED. The guest contains its own open-coded
 * copy of this function at sub_0037C9C4, and src/xbox/guest_structs.h records it
 * instruction by instruction:
 *
 *     0x0037C9CC  mov  dword ptr [ebp - 4], eax   ; +0x04 Buffer   <- src   (32-bit)
 *     0x0037C9D2  mov  dl, byte ptr [eax]         ; strlen loop, BYTE-wise
 *     0x0037C9DB  mov  word ptr [ebp - 8], ax     ; +0x00 Length        (16-bit)
 *     0x0037C9E3  mov  word ptr [ebp - 6], ax     ; +0x02 MaximumLength (16-bit)
 *
 * So: Buffer is the source pointer itself (NOTHING IS COPIED -- this function does
 * not allocate, which is why the guest never calls RtlFreeAnsiString), Length is a
 * byte-wise strlen, and MaximumLength is set from the same register as Length.
 *
 * MaximumLength == Length + 1 is pinned separately and more strongly, by 18
 * statically initialised instances in the image that ALL satisfy both
 * `Length == strlen(Buffer)` and `MaximumLength == Length + 1` -- a joint condition
 * that could not hold 18 times under any other rule. See guest_structs.h.
 *
 * The byte-wise strlen is also what proves the characters are single-byte, i.e.
 * ANSI and not UTF-16. We do not have to assume it.
 */

/* The largest Length the guest's 16-bit field can express. MaximumLength must hold
 * Length + 1, so the longest representable string is one shorter still. */
#define ANSI_STRING_MAX_LENGTH 0xFFFEu

static uint32_t hle_rtl_init_ansi_string(void *context)
{
    if (!context) {
        kernel_hle_log()("kernel: RtlInitAnsiString called with no argument frame -- "
                         "the call boundary did not supply one\n");
        return 0u;
    }
    const kernel_call_frame *frame = (const kernel_call_frame *)context;

    uint32_t destination = 0u;
    uint32_t source = 0u;
    if (!kernel_frame_arg(frame, 0u, &destination)
        || !kernel_frame_arg(frame, 1u, &source)) {
        kernel_hle_log()("kernel: RtlInitAnsiString could not read its two arguments "
                         "from the guest stack\n");
        return 0u;
    }

    /* A NULL destination has nowhere to put the answer. Reported rather than
     * ignored: the guest would then read an uninitialised descriptor off its own
     * stack and the resulting length would be whatever was there. */
    guest_object_string *target =
        (guest_object_string *)kernel_guest_at(destination, sizeof(guest_object_string));
    if (!target) {
        kernel_hle_log()("kernel: RtlInitAnsiString(dest=%#x) -- destination is not a "
                         "usable guest address for an 8-byte OBJECT_STRING\n",
                         destination);
        return 0u;
    }

    /* A NULL source. NOT MEASURED: the open-coded copy at sub_0037C9C4 is only
     * observed on a non-NULL path, and no call site in this image passes a constant
     * NULL, so hardware behaviour here is unverified. An empty descriptor is chosen
     * because it is the one answer that cannot be mistaken for a real string, and it
     * is reported so that if the guest ever does take this path we find out. */
    if (source == 0u) {
        kernel_hle_log()("kernel: RtlInitAnsiString(dest=%#x, src=NULL) -- emptying "
                         "the descriptor; this path is NOT measured from the image\n",
                         destination);
        target->length = 0u;
        target->maximum_length = 0u;
        target->buffer = 0u;
        return 0u;
    }

    /* Byte-wise strlen, exactly as the guest's own copy does it, bounded by what
     * the 16-bit field can represent. Each byte access rejects an unmapped or
     * unreadable (PROT_NONE) page; an unterminated readable string also needs this
     * length bound. The probe does not prevent a concurrent unmap (kernel_call.h). */
    uint32_t length = 0u;
    bool terminated = false;
    while (length <= ANSI_STRING_MAX_LENGTH) {
        const unsigned char *byte =
            (const unsigned char *)kernel_guest_at((kernel_guest_ptr)(source + length), 1u);
        if (!byte) {
            kernel_hle_log()("kernel: RtlInitAnsiString(src=%#x) -- the source ran "
                             "out of usable guest memory after %u bytes\n",
                             source, length);
            break;
        }
        if (*byte == 0u) {
            terminated = true;
            break;
        }
        length++;
    }

    if (!terminated) {
        /* Clamped, and counted. The real kernel's 16-bit field cannot describe a
         * longer string either, so truncating is not the wrong answer -- silently
         * truncating is. */
        truncated_count++;
        if (length > ANSI_STRING_MAX_LENGTH) {
            length = ANSI_STRING_MAX_LENGTH;
        }
        kernel_hle_log()("kernel: RtlInitAnsiString(src=%#x) -- no NUL within %u "
                         "bytes; Length clamped to %u, which is all the guest's "
                         "16-bit field can hold\n",
                         source, ANSI_STRING_MAX_LENGTH, length);
    }

    /* The three measured fields, in the measured widths. The casts are narrowing on
     * purpose and are provably safe: the loop above cannot leave `length` above
     * ANSI_STRING_MAX_LENGTH, so Length + 1 cannot overflow 16 bits. */
    target->length = (uint16_t)length;
    target->maximum_length = (uint16_t)(length + 1u);
    /* Buffer is the SOURCE POINTER, not a copy. Pinned by 0x0037C9CC storing the
     * incoming pointer straight into +0x04. */
    target->buffer = (guest_va)source;

    /* VOID on hardware. */
    return 0u;
}

static uint32_t hle_rtl_nt_status_to_dos_error(void *context)
{
    if (!context) {
        kernel_hle_log()("kernel: RtlNtStatusToDosError called with no argument frame "
                         "-- the call boundary did not supply one\n");
        return KERNEL_RTL_ERROR_MR_MID_NOT_FOUND;
    }
    const kernel_call_frame *frame = (const kernel_call_frame *)context;

    uint32_t status = 0u;
    if (!kernel_frame_arg(frame, 0u, &status)) {
        kernel_hle_log()("kernel: RtlNtStatusToDosError could not read its status "
                         "argument from the guest stack\n");
        /* NOT 0. Returning ERROR_SUCCESS for a call we failed to read would tell the
         * guest its operation succeeded, which is the most damaging possible lie
         * from this particular function. */
        return KERNEL_RTL_ERROR_MR_MID_NOT_FOUND;
    }
    return kernel_rtl_status_to_dos_error(status);
}

/* ===========================================================================
 * RtlEqualString (ordinal 279, 1 call site) -- the boot's 30th kernel call
 * ===========================================================================
 *
 * The arity evidence is in kernel_rtl.h under ARITY-OK(279) and is not repeated here.
 * What belongs here is the SEMANTICS, and which parts of them are measured.
 *
 * MEASURED, from the single site 0x0037C952 inside `_XGetSectionHandleA@4`:
 *
 *   - Both operands are 8-byte OBJECT_STRINGs built by ordinal 289 immediately before
 *     the call, so `Length` is a byte-wise strlen of single-byte characters and
 *     `Buffer` ALIASES the caller's storage. That is why nothing is copied below.
 *   - The result is consumed as `test al, al`, so it is a BOOLEAN in AL. Returning 1
 *     for equal and 0 for not-equal is therefore the full contract. Any nonzero would
 *     do for TRUE, and 1 is used because that is the value the guest's own `push 1`
 *     uses for TRUE in the argument it passes in.
 *   - `CaseInSensitive` arrives as the literal 1 at the only site, so the
 *     case-SENSITIVE path is NOT exercised by this image at all. It is implemented
 *     anyway because the argument exists and a handler that ignored it would silently
 *     fold for a caller that asked not to.
 *
 * NOT MEASURED, and this is the honest limit of what one call site can tell us:
 *
 *   - THE ORDER of the length check and the byte comparison. This is unobservable and
 *     also does not matter: two counted strings of different lengths cannot be equal,
 *     so short-circuiting on length is a consequence of the semantics rather than a
 *     claim about the real function's instruction order. It is, however, what makes a
 *     zero-length descriptor with a NULL Buffer safe, which is a real behaviour --
 *     such a descriptor is never dereferenced here.
 *   - FOLDING ABOVE 0x7F. The real `RtlUpperChar` (ordinal 294 is a different
 *     function; upcasing is ordinal 311) consults the kernel's OEM code-page table,
 *     and we have NOT measured that table from this image. So bytes 0x80..0xFF are
 *     compared WITHOUT folding, and meeting one on a case-insensitive comparison is
 *     COUNTED in `nonascii_fold_count` and NAMED in the log. That is deliberately not
 *     a silent approximation: the one caller compares XBE section names
 *     ("$$XTINFO", "$$XTIMAGE", "$$XSIMAGE" at 0x00380FB7..0x00380FD1) which are pure
 *     ASCII, so the count is expected to stay at zero, and if it ever does not then
 *     the log line says which byte to go and measure.
 *   - WHAT HARDWARE DOES WITH A NULL DESCRIPTOR. It would fault. We report and answer
 *     FALSE, because a fault inside a handler destroys the diagnostic that would have
 *     said where it came from. `kernel_rtl_strings_equal` distinguishes that case from
 *     a real mismatch via its return value, which the handler cannot do -- the ABI has
 *     one byte of room and no way to say "I could not tell".
 */

/* ASCII-only upper-casing. `nonascii` is SET, never cleared, so one byte above 0x7F
 * anywhere in a comparison is enough to flag the whole comparison. */
static unsigned char rtl_fold_ascii(unsigned char byte, bool *nonascii)
{
    if (byte >= 0x80u) {
        *nonascii = true;
        return byte;
    }
    if (byte >= (unsigned char)'a' && byte <= (unsigned char)'z') {
        return (unsigned char)(byte - 0x20u);
    }
    return byte;
}

bool kernel_rtl_strings_equal(uint32_t string1, uint32_t string2, bool case_insensitive,
                              bool *equal)
{
    if (!equal) {
        return false;
    }

    const guest_object_string *first =
        (const guest_object_string *)kernel_guest_at(string1, sizeof(guest_object_string));
    const guest_object_string *second =
        (const guest_object_string *)kernel_guest_at(string2, sizeof(guest_object_string));
    if (!first || !second) {
        kernel_hle_log()("kernel: RtlEqualString(%#x, %#x) -- %s is not a usable guest "
                         "address for an 8-byte OBJECT_STRING\n",
                         string1, string2,
                         !first ? (!second ? "neither operand" : "String1") : "String2");
        return false;
    }

    /* Read both lengths ONCE, into locals. kernel_call.h records that the frame is not
     * a snapshot and guest memory can be read twice; comparing a field we then index
     * with must not be able to change between the two uses. */
    const uint32_t length1 = first->length;
    const uint32_t length2 = second->length;
    if (length1 != length2) {
        *equal = false;
        return true;
    }

    /* Equal lengths of zero. No buffer is touched, which is what makes an empty
     * descriptor with a NULL Buffer -- the shape ordinal 289 writes for a NULL source
     * -- a legitimate operand rather than a fault. */
    if (length1 == 0u) {
        *equal = true;
        return true;
    }

    const unsigned char *bytes1 =
        (const unsigned char *)kernel_guest_at(first->buffer, (size_t)length1);
    const unsigned char *bytes2 =
        (const unsigned char *)kernel_guest_at(second->buffer, (size_t)length2);
    if (!bytes1 || !bytes2) {
        kernel_hle_log()("kernel: RtlEqualString -- %u byte(s) at Buffer %#x / %#x is "
                         "not usable guest memory; REFUSED rather than answered\n",
                         (unsigned)length1, (unsigned)first->buffer,
                         (unsigned)second->buffer);
        return false;
    }

    bool nonascii = false;
    unsigned char offender = 0u;
    bool match = true;
    for (uint32_t i = 0u; i < length1; i++) {
        unsigned char left = bytes1[i];
        unsigned char right = bytes2[i];
        if (case_insensitive) {
            bool high = false;
            left = rtl_fold_ascii(left, &high);
            right = rtl_fold_ascii(right, &high);
            if (high && !nonascii) {
                nonascii = true;
                offender = bytes1[i] >= 0x80u ? bytes1[i] : bytes2[i];
            }
        }
        if (left != right) {
            match = false;
            /* NOT a `break`. The scan continues so that an unmeasured byte LATER in
             * the span is still counted: stopping at the first difference would hide
             * the one thing this counter exists to surface, and would make the count
             * depend on where the mismatch happened to be. */
        }
    }

    if (nonascii) {
        rtl_enter();
        nonascii_fold_count++;
        last_nonascii_fold = offender;
        rtl_leave();
        kernel_hle_log()("kernel: RtlEqualString(CaseInSensitive) met byte %#02x, which "
                         "is above 0x7F -- compared WITHOUT folding, because the "
                         "kernel's OEM upper-case table is NOT measured from this "
                         "image. Answer is %s\n",
                         (unsigned)offender, match ? "TRUE" : "FALSE");
    }

    *equal = match;
    return true;
}

static uint32_t hle_rtl_equal_string(void *context)
{
    if (!context) {
        kernel_hle_log()("kernel: RtlEqualString called with no argument frame -- the "
                         "call boundary did not supply one\n");
        return 0u;
    }
    const kernel_call_frame *frame = (const kernel_call_frame *)context;

    uint32_t string1 = 0u;
    uint32_t string2 = 0u;
    uint32_t case_insensitive = 0u;
    if (!kernel_frame_arg(frame, 0u, &string1) || !kernel_frame_arg(frame, 1u, &string2)
        || !kernel_frame_arg(frame, 2u, &case_insensitive)) {
        kernel_hle_log()("kernel: RtlEqualString could not read its three arguments "
                         "from the guest stack\n");
        return 0u;
    }

    bool equal = false;
    if (!kernel_rtl_strings_equal(string1, string2, case_insensitive != 0u, &equal)) {
        /* FALSE, and the reason was already logged by the call above.
         *
         * This is the one place the ABI forces a lossy answer, so it is worth being
         * exact about which way to lose. The sole caller is `_XGetSectionHandleA@4`
         * searching the XBE section table, and FALSE there means "not this section",
         * so a refused comparison makes the search continue and ultimately report the
         * section as absent. TRUE would instead hand the title a section handle chosen
         * by a comparison we could not perform -- a wrong answer that looks like a
         * right one, which is strictly worse than a missing section. */
        return 0u;
    }
    /* BOOLEAN in AL. 1 and 0, matching the TRUE the guest itself pushes. */
    return equal ? 1u : 0u;
}

/* ===========================================================================
 * RtlCompareMemoryUlong (ordinal 269)
 * =========================================================================== */

static uint32_t hle_rtl_compare_memory_ulong(void *context)
{
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t source = 0u;
    uint32_t length = 0u;
    uint32_t pattern = 0u;
    if (!context || !kernel_frame_arg(frame, 0u, &source) ||
        !kernel_frame_arg(frame, 1u, &length) || !kernel_frame_arg(frame, 2u, &pattern)) {
        kernel_hle_log()("kernel: RtlCompareMemoryUlong could not read its three arguments "
                         "from the guest stack, answering 0 matched bytes\n");
        return 0u;
    }
    /* Whole words only, so a trailing 1 to 3 bytes never count (INFERRED from NT). */
    const uint32_t words = length / 4u;
    if (words == 0u) {
        return 0u;
    }
    const unsigned char *base = (const unsigned char *)kernel_guest_at(source, words * 4u);
    if (base == NULL) {
        rtl_enter();
        compare_refused_count++;
        rtl_leave();
        kernel_hle_log()("kernel: RtlCompareMemoryUlong(%#x, %u, %#x): the source range is "
                         "not readable guest memory, answering 0 matched bytes\n",
                         (unsigned)source, (unsigned)length, (unsigned)pattern);
        return 0u;
    }
    uint32_t matched = 0u;
    while (matched < words) {
        /* memcpy: the source need not be aligned (INFERRED from NT: no alignment rule). */
        uint32_t word = 0u;
        memcpy(&word, base + matched * 4u, sizeof(word));
        if (word != pattern) {
            break;
        }
        matched++;
    }
    return matched * 4u;
}

/* ===========================================================================
 * RtlTimeToTimeFields (305) and RtlTimeFieldsToTime (304)
 * ===========================================================================
 *
 * A time is 100ns ticks since 1601-01-01, a Monday. The calendar is proleptic Gregorian.
 * Day arithmetic goes through days since 1970-01-01 (Hinnant's civil algorithms) and the
 * 134774-day gap between the two epochs, so only one leap rule is written down. */

#define TICKS_PER_MS 10000ull
#define TICKS_PER_SECOND 10000000ull
#define TICKS_PER_DAY 864000000000ull
#define DAYS_1601_TO_1970 134774
#define TIME_FIELDS_BYTES 16u
#define MIN_YEAR 1601
#define MAX_YEAR 32767

static int64_t days_from_civil(int64_t year, unsigned month, unsigned day)
{
    year -= month <= 2u ? 1 : 0;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const int64_t year_of_era = year - era * 400;
    const int64_t day_of_year =
        (153 * (int64_t)(month > 2u ? month - 3u : month + 9u) + 2) / 5 + (int64_t)day - 1;
    const int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

static void civil_from_days(int64_t days, int64_t *year, unsigned *month, unsigned *day)
{
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const int64_t day_of_era = days - era * 146097;
    const int64_t year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const int64_t day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const int64_t month_phase = (5 * day_of_year + 2) / 153;
    *day = (unsigned)(day_of_year - (153 * month_phase + 2) / 5 + 1);
    *month = (unsigned)(month_phase < 10 ? month_phase + 3 : month_phase - 9);
    *year = year_of_era + era * 400 + (*month <= 2u ? 1 : 0);
}

static bool is_leap_year(int64_t year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static unsigned days_in_month(int64_t year, unsigned month)
{
    static const unsigned lengths[12] = {31u, 28u, 31u, 30u, 31u, 30u,
                                         31u, 31u, 30u, 31u, 30u, 31u};
    return (month == 2u && is_leap_year(year)) ? 29u : lengths[month - 1u];
}

static void time_refused(const char *who, const char *reason)
{
    rtl_enter();
    time_refused_count++;
    const uint32_t count = time_refused_count;
    rtl_leave();
    kernel_hle_log()("kernel: %s REFUSED: %s (time refusal %u)\n", who, reason,
                     (unsigned)count);
}

static void put_u16(unsigned char *base, unsigned offset, uint16_t value)
{
    base[offset] = (unsigned char)(value & 0xFFu);
    base[offset + 1u] = (unsigned char)(value >> 8);
}

static uint16_t get_u16(const unsigned char *base, unsigned offset)
{
    return (uint16_t)((uint16_t)base[offset] | (uint16_t)((uint16_t)base[offset + 1u] << 8));
}

static uint32_t hle_rtl_time_to_time_fields(void *context)
{
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t time_ptr = 0u;
    uint32_t fields_ptr = 0u;
    if (!context || !kernel_frame_arg(frame, 0u, &time_ptr) ||
        !kernel_frame_arg(frame, 1u, &fields_ptr)) {
        time_refused("RtlTimeToTimeFields", "its two arguments could not be read");
        return 0u;
    }
    const unsigned char *in = (const unsigned char *)kernel_guest_at(time_ptr, 8u);
    unsigned char *out = (unsigned char *)kernel_guest_at(fields_ptr, TIME_FIELDS_BYTES);
    if (in == NULL || out == NULL) {
        time_refused("RtlTimeToTimeFields", "the time or the fields are not guest memory");
        return 0u;
    }
    uint64_t ticks = 0u;
    for (unsigned i = 0u; i < 8u; i++) {
        ticks |= (uint64_t)in[i] << (8u * i);
    }
    if ((ticks >> 63) != 0u) {
        time_refused("RtlTimeToTimeFields", "the time is negative, before 1601");
        return 0u;
    }
    const int64_t days = (int64_t)(ticks / TICKS_PER_DAY);
    const uint64_t rest = ticks % TICKS_PER_DAY;
    int64_t year = 0;
    unsigned month = 0u;
    unsigned day = 0u;
    civil_from_days(days - DAYS_1601_TO_1970, &year, &month, &day);
    if (year > MAX_YEAR) {
        time_refused("RtlTimeToTimeFields", "the year does not fit a CSHORT");
        return 0u;
    }
    put_u16(out, 0u, (uint16_t)year);
    put_u16(out, 2u, (uint16_t)month);
    put_u16(out, 4u, (uint16_t)day);
    put_u16(out, 6u, (uint16_t)(rest / (3600u * TICKS_PER_SECOND)));
    put_u16(out, 8u, (uint16_t)(rest / (60u * TICKS_PER_SECOND) % 60u));
    put_u16(out, 10u, (uint16_t)(rest / TICKS_PER_SECOND % 60u));
    put_u16(out, 12u, (uint16_t)(rest / TICKS_PER_MS % 1000u));
    /* 1601-01-01 was a Monday, and Sunday is 0. */
    put_u16(out, 14u, (uint16_t)((days + 1) % 7));
    return 0u;
}

static uint32_t hle_rtl_time_fields_to_time(void *context)
{
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t fields_ptr = 0u;
    uint32_t time_ptr = 0u;
    if (!context || !kernel_frame_arg(frame, 0u, &fields_ptr) ||
        !kernel_frame_arg(frame, 1u, &time_ptr)) {
        time_refused("RtlTimeFieldsToTime", "its two arguments could not be read");
        return 0u;
    }
    const unsigned char *in = (const unsigned char *)kernel_guest_at(fields_ptr, TIME_FIELDS_BYTES);
    unsigned char *out = (unsigned char *)kernel_guest_at(time_ptr, 8u);
    if (in == NULL || out == NULL) {
        time_refused("RtlTimeFieldsToTime", "the fields or the time are not guest memory");
        return 0u;
    }
    /* CSHORT is signed: a negative field reads as negative and fails the range checks.
     * Weekday (+14) is never read, the guest does not initialise it (MEASURED). */
    const int year = (int16_t)get_u16(in, 0u);
    const int month = (int16_t)get_u16(in, 2u);
    const int day = (int16_t)get_u16(in, 4u);
    const int hour = (int16_t)get_u16(in, 6u);
    const int minute = (int16_t)get_u16(in, 8u);
    const int second = (int16_t)get_u16(in, 10u);
    const int millisecond = (int16_t)get_u16(in, 12u);
    if (year < MIN_YEAR || month < 1 || month > 12 || day < 1 ||
        (unsigned)day > days_in_month(year, (unsigned)month) || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59 || millisecond < 0 ||
        millisecond > 999) {
        time_refused("RtlTimeFieldsToTime", "a field is out of range, answering FALSE");
        return 0u;
    }
    const int64_t days = days_from_civil(year, (unsigned)month, (unsigned)day) + DAYS_1601_TO_1970;
    const uint64_t ticks =
        (uint64_t)days * TICKS_PER_DAY +
        (uint64_t)((hour * 60 + minute) * 60 + second) * TICKS_PER_SECOND +
        (uint64_t)millisecond * TICKS_PER_MS;
    if ((ticks >> 63) != 0u) {
        time_refused("RtlTimeFieldsToTime", "the time overflows a signed 64-bit count");
        return 0u;
    }
    for (unsigned i = 0u; i < 8u; i++) {
        out[i] = (unsigned char)((ticks >> (8u * i)) & 0xFFu);
    }
    return 1u;
}

uint32_t kernel_rtl_compare_refused_count(void)
{
    rtl_enter();
    const uint32_t snapshot = compare_refused_count;
    rtl_leave();
    return snapshot;
}

uint32_t kernel_rtl_time_refused_count(void)
{
    rtl_enter();
    const uint32_t snapshot = time_refused_count;
    rtl_leave();
    return snapshot;
}

/* ---------------------------------------------------------------------------
 * Observation, for tests.
 * ------------------------------------------------------------------------- */

void kernel_rtl_reset(void)
{
    rtl_enter();
    unmapped_count = 0u;
    last_unmapped = STATUS_SUCCESS;
    truncated_count = 0u;
    nonascii_fold_count = 0u;
    last_nonascii_fold = 0u;
    compare_refused_count = 0u;
    time_refused_count = 0u;
    rtl_leave();
}

uint32_t kernel_rtl_truncated_count(void)
{
    rtl_enter();
    const uint32_t snapshot = truncated_count;
    rtl_leave();
    return snapshot;
}

uint32_t kernel_rtl_nonascii_fold_count(void)
{
    rtl_enter();
    const uint32_t snapshot = nonascii_fold_count;
    rtl_leave();
    return snapshot;
}

uint8_t kernel_rtl_last_nonascii_fold(void)
{
    rtl_enter();
    const uint8_t snapshot = last_nonascii_fold;
    rtl_leave();
    return snapshot;
}

uint32_t kernel_rtl_unmapped_count(void)
{
    rtl_enter();
    uint32_t snapshot = unmapped_count;
    rtl_leave();
    return snapshot;
}

nt_status kernel_rtl_last_unmapped(void)
{
    rtl_enter();
    nt_status snapshot = last_unmapped;
    rtl_leave();
    return snapshot;
}

/* ARITY-OK(302): ONE stack argument, a pointer to an EXCEPTION_RECORD. Import slot
 * 0x475888, measured row {302, 1, 5 lifted sites, unanimous}, and the nxdk oracle
 * RtlRaiseException@4 agrees. The 5 lifted sites are exactly 3 guest call sites:
 *   - 0x0037FD53 in sub_0037FD08, the XAPI RaiseException(code, flags, nargs, args)
 *     body. The record sits at ebp-80, ExceptionAddress is the literal 0x37FD08 and
 *     NumberParameters is min(nargs, 15). Its callers are __CxxThrowException (code
 *     0xE06D7363, flags 1 non-continuable, 3 params, Info[0] = 0x19930520) and the CRT
 *     FP raiser (codes 0xC000008E to 0xC0000093, flags 0, continuable).
 *   - 0x00383D8F in RtlAllocateHeap and 0x0038465B in RtlReAllocateHeap. Both raise
 *     STATUS_NO_MEMORY 0xC0000017 with flags 0 and 1 param (the rounded size), and only
 *     when the caller passed HEAP_GENERATE_EXCEPTIONS (flag 4).
 *
 * THERE IS NO EXCEPTION DISPATCH MODEL in this host. The fs:[0] chains exist in guest
 * memory but the __except funclets are lifted as dead code nothing calls, and RtlUnwind
 * (312) is unimplemented. The faithful recovery today is therefore to decode the record,
 * report it in full and stop through kernel_hle_fatal(302, ...). The FP exception path
 * is continuable on hardware (the guest expects a RETURN) and would need real dispatch.
 * All three sites are conditional error arms and none is reached by the measured boot.
 *
 * Record layout, as read here: ExceptionCode +0, ExceptionFlags +4, ExceptionRecord +8,
 * ExceptionAddress +0xC, NumberParameters +0x10 (capped at 15 like the guest), then
 * ExceptionInformation[] from +0x14, of which up to four dwords reach the message. A
 * pointer or field that cannot be read still goes fatal, with the record named as
 * unreadable. A frame that cannot supply the pointer is the same, never a quiet refusal.
 *
 * The value returned after the hook is the ExceptionCode (0 when unreadable). Only a
 * capturing test hook ever sees it, because the host's hook stops the guest. */
#define RTL_EXCEPTION_MAX_PARAMETERS 15u
#define RTL_EXCEPTION_INFO_SHOWN 4u

static uint32_t hle_rtl_raise_exception(void *context)
{
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t record = 0u;

    if (frame == NULL || !kernel_frame_arg(frame, 0u, &record)) {
        kernel_hle_fatal(ORD_RtlRaiseException,
                         "RtlRaiseException(<record pointer unreadable>) with no "
                         "dispatch model, stopping");
        return 0u;
    }

    uint32_t code = 0u;
    uint32_t flags = 0u;
    uint32_t nested = 0u;
    uint32_t address = 0u;
    uint32_t count = 0u;
    if (record == 0u || !kernel_guest_read_u32(record, &code) ||
        !kernel_guest_read_u32(record + 4u, &flags) ||
        !kernel_guest_read_u32(record + 8u, &nested) ||
        !kernel_guest_read_u32(record + 0xCu, &address) ||
        !kernel_guest_read_u32(record + 0x10u, &count)) {
        kernel_hle_fatal(ORD_RtlRaiseException,
                         "RtlRaiseException(record 0x%08X unreadable) with no "
                         "dispatch model, stopping",
                         record);
        return 0u;
    }
    if (count > RTL_EXCEPTION_MAX_PARAMETERS) {
        count = RTL_EXCEPTION_MAX_PARAMETERS;
    }

    char info[64];
    size_t used = 0u;
    info[0] = '\0';
    for (uint32_t i = 0u; i < count && i < RTL_EXCEPTION_INFO_SHOWN; i++) {
        uint32_t value = 0u;
        const bool readable = kernel_guest_read_u32(record + 0x14u + 4u * i, &value);
        const int written = readable
                                ? snprintf(info + used, sizeof(info) - used, "%s0x%08X",
                                           i == 0u ? ": " : " ", value)
                                : snprintf(info + used, sizeof(info) - used,
                                           "%s<unreadable>", i == 0u ? ": " : " ");
        if (written > 0 && (size_t)written < sizeof(info) - used) {
            used += (size_t)written;
        }
    }
    if (count > RTL_EXCEPTION_INFO_SHOWN && used + 5u < sizeof(info)) {
        (void)snprintf(info + used, sizeof(info) - used, " ...");
    }

    kernel_hle_fatal(ORD_RtlRaiseException,
                     "RtlRaiseException(code 0x%08X, flags 0x%X, address 0x%08X, "
                     "%u params%s%s) with no dispatch model, stopping",
                     code, flags, address, count, info,
                     nested != 0u ? ", nested record" : "");
    return code;
}

/* ARITY-OK(279): THREE stack arguments, (String1, String2, CaseInSensitive). The full
 * evidence -- the verbatim push sequence at the only site, the forced stack balance
 * that derives 3 from a loop with no `add esp`, and the CC0 DEF decoration
 * `RtlEqualString@12` -- is in kernel_rtl.h, because it is long and because the ABI row
 * in src/host/kernel_thunk.c points at that file rather than at this one.
 *
 * ARITY-OK(289): TWO stack arguments, (DestinationString, SourceString). The
 * measured table publishes 1 and is WRONG -- this is the ordinal the arity check's
 * own docstring cites as the known under-report, and an independent measurement
 * agrees with 2. The per-site vote over 21 bracketed sites is {1: 1, 2: 17, 3: 2,
 * 4: 1}: mode 2, with the three high sites explained by callee-saved pushes inside
 * the lifter's bracket (recomp_0043.c:27655 is the textbook `push edi; mov
 * edi,[ebp+8]` save) and the single low site by a bracket opening after the source
 * pointer was already pushed (recomp_0044.c:10561 pushes `esi+0x40` before the
 * bracket at 10566, which then catches only the destination `ebp-24`). Corroborated
 * by the argument SHAPES: the destination is a stack local at every site (ebp-8,
 * ebp-12, ebp-24, ebp-68 -- an 8-byte OBJECT_STRING being built in the caller's
 * frame) and the source is a string pointer, which at recomp_0062.c:6523 is the
 * literal 0x4A1524, a .rdata address holding "\Device\Harddisk0\partition0". A
 * one-argument reading cannot account for a destination AND a source, and this
 * function has nowhere else to get either.
 *
 * ARITY-OK(301): one stack argument, the NTSTATUS. Listed here even though the
 * measurement is already unanimous and confident (29 of 29 sites push exactly 1),
 * because that unanimity is the evidence and it is worth stating where it can be
 * found rather than leaving the one confident ordinal in this file looking
 * unexamined. */
static const struct {
    unsigned ordinal;
    kernel_fn handler;
} bindings[] = {
    {ORD_RtlCompareMemoryUlong, hle_rtl_compare_memory_ulong},
    {ORD_RtlEqualString, hle_rtl_equal_string},
    {ORD_RtlInitAnsiString, hle_rtl_init_ansi_string},
    {ORD_RtlNtStatusToDosError, hle_rtl_nt_status_to_dos_error},
    {ORD_RtlRaiseException, hle_rtl_raise_exception},
    {ORD_RtlTimeFieldsToTime, hle_rtl_time_fields_to_time},
    {ORD_RtlTimeToTimeFields, hle_rtl_time_to_time_fields},
};

size_t kernel_rtl_register(void)
{
    size_t bound = 0u;
    for (size_t i = 0u; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
        if (kernel_hle_register(bindings[i].ordinal, bindings[i].handler)) {
            bound++;
        }
    }
    return bound;
}
