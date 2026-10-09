/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T200: observe the retained original shader compiler (XGAssembleShader, 0x003EE2B3)
 * inside the real title boot, without changing the production host.
 *
 * tsfp_shader_probe is tsfp_host (src/host/main.c compiled with main renamed) plus
 * three link-time wraps. Nothing in src/ is edited and the normal host is untouched.
 *
 *   --wrap=recomp_lookup_original   the opt-in route asks for the compiled alias of
 *                                   0x3EE2B3. This probe hands back probe_compiler,
 *                                   which brackets the REAL alias and returns exactly
 *                                   what it returned. Every other profile address
 *                                   gets its real alias.
 *   --wrap=sub_00383678             the title's RtlAllocateHeap(heap, flags, size),
 *                                   stdcall RET 12. Inside a compiler call it is
 *                                   logged and, when asked, the Kth allocation returns
 *                                   NULL exactly as the title heap would on exhaustion
 *                                   (only when the caller did not pass
 *                                   HEAP_GENERATE_EXCEPTIONS, whose failure raises).
 *   --wrap=sub_00383DF3             the title's RtlFreeHeap, logged to balance the heap.
 *
 * Scenarios (all optional, off by default, stripped before the host sees argv):
 *   --probe-out PATH             one JSON line per event appended here
 *   --probe-heap-log 1           T505: log every title RtlAllocateHeap and RtlFreeHeap inside a
 *                                compiler call to stderr, with the guest return address and
 *                                ebp chain of the caller, so a heap fault names the block and
 *                                the callers that freed it (see docs/shader-assembler-limits.md)
 *   --probe-repeat N             re-run each compiler call N more times on the same
 *                                frame and compare bytes and HRESULT with the first
 *   --probe-rendezvous MS        the first compiler call of each thread waits up to MS for
 *                                another thread to arrive so the two run concurrently
 *   --probe-fail-alloc K         the Kth title allocation inside a compiler call fails
 *   --probe-fail-call C          which compiler call that applies to (default 1)
 *   --probe-concurrent K         at the title's first call of the lock free buffer getter
 *                                0x3E6714 after three compiles, run K rounds in which that guest thread and a probe-created
 *                                second guest thread (own stack, control page and TLS, built
 *                                like a PsCreateSystemThreadEx thread) each run the compiler
 *                                at the same instant, on the title's own three sources in
 *                                different phase, and compare every output with the sequential
 *                                reference
 *   --probe-concurrent-serial    same rounds but a mutex serialises the two compiles: the
 *                                control that shows the second thread's context is sound
 *   --probe-concurrent-route     the rounds enter through xdk_original_dispatch, the
 *                                production route and its compiler lock, from the title's
 *                                first sub_003D59E0 call (shader upload) after three
 *                                compiles, instead of calling the alias directly. Overlap is
 *                                measured around the compiler body itself
 *   --probe-exit-after N         _exit(0) once N compiler calls finished, after logging
 *   --probe-corpus SPEC          T387: at the title's first call of the lock free getter
 *                                0x3E6714 after three compiles, feed the cases listed in SPEC
 *                                (one `name flags path` per line, the sources written by
 *                                `python -m tools.shaderscan.verify export`) through
 *                                xdk_original_dispatch, the production route and its lock,
 *                                one `corpus_begin` line before and one `corpus_call` line
 *                                after each, then _exit(0). A fault or stop in a case ends the
 *                                process there and the begin line names the case. A case is
 *                                never given an invented result.
 *   --probe-corpus-stack-fill V  before each case fill the 16 KiB of title stack below the call
 *                                frame with the byte V (0 to 255), to show whether a result
 *                                depends on stack memory the caller did not supply
 *   --probe-corpus-outputs 1     T385: also supply the constants output (argument 4) and the
 *                                compilation-errors output (argument 6), as the text corpus
 *                                is measured on the original, and record both buffers
 *   --probe-corpus-flush 1       place each source flush against an unmapped guard page
 *                                instead of in zero padding, as the reference does
 *   --probe-corpus-repeat N      T476: run each spec line N more times in a row in the same
 *                                process (`round` 1..N of its `corpus_call` records, round 0
 *                                is the first call). A name may repeat across spec lines, so
 *                                an ordering is just a longer spec
 *   --probe-corpus-concurrent K  T476: after the sequential calls, K rounds in which this
 *                                guest thread and a probe-created second guest thread each
 *                                call the compiler on a DIFFERENT spec line through the
 *                                route lock, released together by a two-party rendezvous.
 *                                Each call is a `corpus_call` record with `thread` and
 *                                `phase` "concurrent", then one `corpus_concurrent` summary.
 *                                Every record also carries a hash of the compiler's static
 *                                data (0x405B00..0x405C20) before and after the call
 *
 * An injected failure that makes the compiler fault ends the process through the host's
 * own stop report. The "begin" line written before the call identifies what was in
 * flight. A fault is never reported as a return.
 */
#define _GNU_SOURCE
#include "guest_mem.h"
#include "host_runtime.h"
#include "kernel_thread.h"
#include "recomp_abi.h"
#include "xdk_original.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

int tsfp_host_main(int argc, char **argv);
extern void recomp_original_003EE2B3(void);
extern void recomp_original_003E6714(void);
extern recomp_func_t __real_recomp_lookup_original(uint32_t address);
extern void __real_sub_00383678(void);
extern void __real_sub_00383DF3(void);

#define COMPILER_ADDRESS 0x003EE2B3u
#define GETTER_ADDRESS 0x003E6714u
#define HEAP_GENERATE_EXCEPTIONS 4u
#define FRAME_DWORDS 12u /* return address plus the 11 stdcall arguments */
#define MAX_HEX_BYTES 2048u
#define MAX_ALLOC_LOG 1024u
#define MAX_LIVE 4096u
#define MAX_REPEATS 64u

#define NO_STACK_FILL 0xFFFFFFFFu

typedef struct {
    const char *out_path;
    uint32_t repeat;
    uint32_t rendezvous_ms;
    uint32_t fail_alloc;
    uint32_t fail_call;
    uint32_t exit_after;
    uint32_t concurrent_rounds;
    uint32_t concurrent_serial;
    uint32_t concurrent_route;
    const char *corpus_spec;
    uint32_t corpus_flush;
    uint32_t corpus_stack_fill; /* NO_FILL, or the byte value */
    uint32_t corpus_outputs;
    uint32_t corpus_repeat;
    uint32_t corpus_concurrent;
    uint32_t heap_log;
} probe_config;

static probe_config config = {NULL, 0u, 0u, 0u, 1u, 0u, 0u, 0u, 0u, NULL, 0u, NO_STACK_FILL, 0u, 0u, 0u, 0u};
static pthread_mutex_t serial_lock = PTHREAD_MUTEX_INITIALIZER;
static int out_fd = -1;
static uint32_t next_seq = 1u;
static uint32_t finished_calls;
static uint32_t inside_now;
static uint32_t inside_max;
static uint32_t next_thread_index;

static pthread_mutex_t rendezvous_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rendezvous_cond = PTHREAD_COND_INITIALIZER;
static uint32_t rendezvous_arrived;

typedef struct {
    bool in_call;
    uint32_t thread_index;
    bool has_index;
    bool fail_armed;
    uint32_t fail_at;
    uint32_t alloc_count;
    uint32_t alloc_injected;
    uint32_t alloc_exception_flag_seen;
    uint32_t free_count;
    uint32_t log_count;
    uint32_t log[MAX_ALLOC_LOG][3];
    uint32_t live_count;
    uint32_t live[MAX_LIVE];
    uint32_t calls_started;
    bool suppress;
    uint64_t last_enter;
    uint64_t last_exit;
} thread_state;

static __thread thread_state tls;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint32_t guest_u32(uint32_t address)
{
    return *(const uint32_t *)(uintptr_t)address;
}

/* Fault-free read of guest memory: EFAULT instead of SIGSEGV for an unmapped range. */
static bool safe_read(uint32_t address, void *out, size_t length)
{
    struct iovec local = {out, length};
    struct iovec remote = {(void *)(uintptr_t)address, length};
    return address != 0u &&
           process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)length;
}

typedef struct {
    char *text;
    size_t used;
    size_t capacity;
} line;

static void put(line *l, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void put(line *l, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(l->text + l->used, l->capacity - l->used, format, args);
    va_end(args);
    if (written > 0 && (size_t)written < l->capacity - l->used) {
        l->used += (size_t)written;
    }
}

static void put_hex(line *l, const uint8_t *bytes, size_t count)
{
    static const char digits[] = "0123456789abcdef";
    put(l, "\"");
    for (size_t i = 0u; i < count && l->used + 3u < l->capacity; i++) {
        l->text[l->used++] = digits[bytes[i] >> 4];
        l->text[l->used++] = digits[bytes[i] & 15u];
    }
    l->text[l->used] = '\0';
    put(l, "\"");
}

static void emit(const line *l)
{
    if (out_fd >= 0 && l->used > 0u) {
        /* One write per line so concurrent threads never interleave a record. */
        if (write(out_fd, l->text, l->used) < 0) {
            abort();
        }
    }
}

typedef struct {
    uint32_t eax;
    uint32_t esp;
    uint32_t ebx, esi, edi, ebp;
    uint32_t fs0;
} regs;

static regs capture(void)
{
    regs r = {g_eax, g_esp, g_ebx, g_esi, g_edi, g_ebp, guest_u32(g_fs_base)};
    return r;
}

typedef struct {
    bool has_output;
    uint32_t object;
    uint32_t data;
    uint32_t size;
    uint32_t copied;
    uint8_t bytes[MAX_HEX_BYTES];
} output_copy;

/* The XGBuffer the compiler writes: refcount, data pointer, byte size (measured, see
 * tools/shaderscan/assemble.py). Only read when the HRESULT says success. */
static output_copy read_output(uint32_t slot, uint32_t hresult)
{
    output_copy out;
    memset(&out, 0, sizeof(out));
    if (hresult != 0u || !safe_read(slot, &out.object, 4u) || out.object == 0u) {
        return out;
    }
    uint32_t fields[3];
    if (!safe_read(out.object, fields, sizeof(fields))) {
        return out;
    }
    out.data = fields[1];
    out.size = fields[2];
    out.copied = out.size < MAX_HEX_BYTES ? out.size : MAX_HEX_BYTES;
    out.has_output = safe_read(out.data, out.bytes, out.copied);
    return out;
}

static bool same_output(const output_copy *a, const output_copy *b)
{
    return a->has_output == b->has_output && a->size == b->size && a->copied == b->copied &&
           memcmp(a->bytes, b->bytes, a->copied) == 0;
}

static void reset_call_state(uint32_t fail_at)
{
    tls.alloc_count = 0u;
    tls.alloc_injected = 0u;
    tls.alloc_exception_flag_seen = 0u;
    tls.free_count = 0u;
    tls.log_count = 0u;
    tls.live_count = 0u;
    tls.fail_at = fail_at;
    tls.fail_armed = fail_at != 0u;
}

#define HEAP_LOG_FRAMES 8u

/* T505: the guest caller chain of a title heap call, from the return address at esp and then
 * the ebp chain (`esp` is the value at entry, before the callee pops). Plain reads of live guest stack inside a compiler call, never more than
 * HEAP_LOG_FRAMES, and only while the chain climbs. */
static void log_heap_callers(uint32_t esp)
{
    fprintf(stderr, " ret=%#x", guest_u32(esp));
    uint32_t frame = g_ebp;
    for (uint32_t depth = 0u; depth < HEAP_LOG_FRAMES && frame >= 0x1000u; depth++) {
        const uint32_t next = guest_u32(frame);
        fprintf(stderr, " <- %#x", guest_u32(frame + 4u));
        if (next <= frame) {
            break;
        }
        frame = next;
    }
    fprintf(stderr, "\n");
}

void __wrap_sub_00383678(void)
{
    if (!tls.in_call) {
        __real_sub_00383678();
        return;
    }
    const uint32_t esp = g_esp;
    const uint32_t flags = guest_u32(esp + 8u);
    const uint32_t size = guest_u32(esp + 12u);
    tls.alloc_count++;
    if ((flags & HEAP_GENERATE_EXCEPTIONS) != 0u) {
        tls.alloc_exception_flag_seen++;
    }
    if (tls.fail_armed && tls.alloc_count == tls.fail_at &&
        (flags & HEAP_GENERATE_EXCEPTIONS) == 0u) {
        tls.fail_armed = false;
        tls.alloc_injected++;
        if (tls.log_count < MAX_ALLOC_LOG) {
            tls.log[tls.log_count][0] = flags;
            tls.log[tls.log_count][1] = size;
            tls.log[tls.log_count][2] = 2u; /* injected NULL */
            tls.log_count++;
        }
        g_eax = 0u;
        g_esp = esp + 16u; /* RET 12 plus the return address */
        return;
    }
    __real_sub_00383678();
    const uint32_t result = g_eax;
    if (config.heap_log != 0u) {
        fprintf(stderr, "heap alloc #%u size=%u flags=%#x ptr=%#x", tls.alloc_count, size, flags,
                result);
        log_heap_callers(esp);
    }
    if (tls.log_count < MAX_ALLOC_LOG) {
        tls.log[tls.log_count][0] = flags;
        tls.log[tls.log_count][1] = size;
        tls.log[tls.log_count][2] = result != 0u ? 1u : 0u;
        tls.log_count++;
    }
    if (result != 0u && tls.live_count < MAX_LIVE) {
        tls.live[tls.live_count++] = result;
    }
}

void __wrap_sub_00383DF3(void)
{
    if (!tls.in_call) {
        __real_sub_00383DF3();
        return;
    }
    const uint32_t pointer = guest_u32(g_esp + 12u);
    tls.free_count++;
    for (uint32_t i = 0u; i < tls.live_count; i++) {
        if (tls.live[i] == pointer) {
            tls.live[i] = tls.live[--tls.live_count];
            break;
        }
    }
    if (config.heap_log != 0u) {
        fprintf(stderr, "heap free #%u ptr=%#x", tls.free_count, pointer);
        log_heap_callers(g_esp);
    }
    __real_sub_00383DF3();
}

static void rendezvous(void)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += config.rendezvous_ms / 1000u;
    deadline.tv_nsec += (long)(config.rendezvous_ms % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&rendezvous_lock);
    rendezvous_arrived++;
    pthread_cond_broadcast(&rendezvous_cond);
    while (rendezvous_arrived < 2u) {
        if (pthread_cond_timedwait(&rendezvous_cond, &rendezvous_lock, &deadline) != 0) {
            break;
        }
    }
    pthread_mutex_unlock(&rendezvous_lock);
}

/* ---- concurrent phase ------------------------------------------------------------- */

#define MAX_ROUNDS 256u
#define ROUND_OUTPUT_BYTES 256u
#define DETAIL_ROUNDS 64u
#define MAX_REFERENCES 3u
#define STACK_REQUEST 0x10000u
#define SENTINEL_RETURN 0xDEAD0000u

typedef struct {
    uint32_t source_pointer;
    uint32_t source_size;
    uint32_t flags;
    uint32_t hresult;
    output_copy output;
    bool valid;
} reference_case;

typedef struct {
    uint32_t source_index;
    uint32_t hresult;
    int32_t esp_delta;
    bool same_as_reference;
    bool regs_preserved;
    bool fs0_preserved;
    uint64_t enter_ns;
    uint64_t exit_ns;
    uint32_t output_size;
    uint32_t output_copied;
    uint8_t output[ROUND_OUTPUT_BYTES];
} round_result;

typedef struct {
    uint32_t stack_top;
    uint32_t fs_base;
    uint32_t slot;
    bool ready;
} guest_context;

static reference_case references[MAX_REFERENCES];
static uint32_t reference_count;
static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sync_cond = PTHREAD_COND_INITIALIZER;
static uint32_t sync_arrived;
static uint32_t sync_generation;
static uint32_t rounds_completed[2];
static bool phase_aborted;
#define SYNC_TIMEOUT_MS 5000u
static round_result round_results[2][MAX_ROUNDS];
static guest_context peer_context;
static volatile uint32_t peer_stopped;

/* Two-party rendezvous that gives up instead of hanging when the other side stopped. */
static bool round_sync(void)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += SYNC_TIMEOUT_MS / 1000u;
    pthread_mutex_lock(&sync_lock);
    bool ok = true;
    const uint32_t generation = sync_generation;
    if (++sync_arrived == 2u) {
        sync_arrived = 0u;
        sync_generation++;
        pthread_cond_broadcast(&sync_cond);
    } else {
        while (generation == sync_generation && !phase_aborted) {
            if (pthread_cond_timedwait(&sync_cond, &sync_lock, &deadline) != 0) {
                phase_aborted = true;
                pthread_cond_broadcast(&sync_cond);
                ok = false;
                break;
            }
        }
        ok = ok && !phase_aborted;
    }
    pthread_mutex_unlock(&sync_lock);
    return ok;
}

static void sync_abort(void)
{
    pthread_mutex_lock(&sync_lock);
    phase_aborted = true;
    pthread_cond_broadcast(&sync_cond);
    pthread_mutex_unlock(&sync_lock);
}

static uint32_t region_alloc(uint32_t bytes)
{
    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = bytes;
    request.alignment = GUEST_ALLOCATION_GRANULARITY;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status = STATUS_SUCCESS;
    return guest_region_alloc(&request, &status);
}

/* A guest thread context built the way provision_thread builds one. */
static bool make_guest_context(guest_context *out, uint32_t stack_bytes)
{
    uint32_t region_bytes = 0u;
    kernel_thread_stack stack;
    memset(&stack, 0, sizeof(stack));
    if (!kernel_thread_stack_region_bytes(stack_bytes, &region_bytes)) {
        return false;
    }
    const uint32_t region = region_alloc(region_bytes);
    const uint32_t control = region_alloc(KERNEL_THREAD_CONTROL_BYTES);
    const uint32_t tls_block = region_alloc(0x1000u);
    const uint32_t monitor = region_alloc(KERNEL_THREAD_MONITOR_BYTES);
    const uint32_t scratch = region_alloc(0x1000u);
    if (region == 0u || control == 0u || tls_block == 0u || monitor == 0u || scratch == 0u ||
        !kernel_thread_stack_layout(&stack, region, stack_bytes) ||
        !kernel_thread_control_init(control, tls_block, monitor, KERNEL_THREAD_MONITOR_BYTES) ||
        !kernel_thread_publish_tls_end(control, tls_block, 0x40u)) {
        return false;
    }
    out->stack_top = stack.high - 0x100u;
    out->fs_base = control;
    out->slot = scratch;
    out->ready = true;
    return true;
}

static void build_frame(uint32_t esp, const reference_case *source, uint32_t slot)
{
    uint32_t *frame = (uint32_t *)(uintptr_t)esp;
    memset(frame, 0, FRAME_DWORDS * sizeof(*frame));
    frame[0] = SENTINEL_RETURN;
    frame[2] = source->source_pointer;
    frame[3] = source->source_size;
    frame[4] = source->flags;
    frame[6] = slot;
    *(uint32_t *)(uintptr_t)slot = 0u;
}

/* The compiler body with the in-flight counter and timestamps around it. */
static void compile_guts(uint64_t *enter_ns, uint64_t *exit_ns)
{
    tls.in_call = true;
    const uint32_t inside = __atomic_add_fetch(&inside_now, 1u, __ATOMIC_SEQ_CST);
    uint32_t seen = __atomic_load_n(&inside_max, __ATOMIC_SEQ_CST);
    while (inside > seen &&
           !__atomic_compare_exchange_n(&inside_max, &seen, inside, false, __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST)) {
    }
    *enter_ns = now_ns();
    recomp_original_003EE2B3();
    *exit_ns = now_ns();
    __atomic_sub_fetch(&inside_now, 1u, __ATOMIC_SEQ_CST);
    tls.in_call = false;
}

static void run_rounds(uint32_t thread_id, uint32_t base_esp, uint32_t slot)
{
    for (uint32_t round = 0u; round < config.concurrent_rounds; round++) {
        const uint32_t index = (round + thread_id) % reference_count;
        const reference_case *source = &references[index];
        build_frame(base_esp, source, slot);
        g_esp = base_esp;
        g_ebx = 0x11110000u + thread_id;
        g_esi = 0x22220000u + thread_id;
        g_edi = 0x33330000u + thread_id;
        g_ebp = 0x44440000u + thread_id;
        const uint32_t fs0 = guest_u32(g_fs_base);
        if (!round_sync()) {
            return;
        }
        if (config.concurrent_serial != 0u) {
            pthread_mutex_lock(&serial_lock);
        }
        reset_call_state(0u);
        if (config.concurrent_route != 0u) {
            tls.suppress = true;
            (void)xdk_original_dispatch(COMPILER_ADDRESS);
            tls.suppress = false;
        } else {
            compile_guts(&tls.last_enter, &tls.last_exit);
        }
        const uint64_t enter_ns = tls.last_enter;
        const uint64_t exit_ns = tls.last_exit;
        if (config.concurrent_serial != 0u) {
            pthread_mutex_unlock(&serial_lock);
        }
        const output_copy got = read_output(slot, g_eax);
        round_result *result = &round_results[thread_id][round];
        result->source_index = index;
        result->hresult = g_eax;
        result->esp_delta = (int32_t)(g_esp - base_esp);
        result->same_as_reference = g_eax == source->hresult && same_output(&got, &source->output);
        result->regs_preserved = g_ebx == 0x11110000u + thread_id &&
                                 g_esi == 0x22220000u + thread_id &&
                                 g_edi == 0x33330000u + thread_id &&
                                 g_ebp == 0x44440000u + thread_id;
        result->fs0_preserved = guest_u32(g_fs_base) == fs0;
        result->enter_ns = enter_ns;
        result->exit_ns = exit_ns;
        result->output_size = got.has_output ? got.size : 0u;
        result->output_copied = got.has_output && got.copied < ROUND_OUTPUT_BYTES
                                    ? got.copied
                                    : (got.has_output ? ROUND_OUTPUT_BYTES : 0u);
        memcpy(result->output, got.bytes, result->output_copied);
        rounds_completed[thread_id] = round + 1u;
    }
}

static void *peer_thread(void *argument)
{
    (void)argument;
    g_fs_base = peer_context.fs_base;
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        const uint32_t esp = peer_context.stack_top - FRAME_DWORDS * 4u;
        run_rounds(1u, esp, peer_context.slot);
    } else {
        __atomic_store_n(&peer_stopped, 1u, __ATOMIC_SEQ_CST);
        sync_abort();
    }
    host_run_disarm();
    return NULL;
}

static void emit_concurrent(uint32_t entry_esp, bool built, bool peer_ok)
{
    static char buffer[1u << 19];
    line l = {buffer, 0u, sizeof(buffer)};
    put(&l, "{\"kind\":\"concurrent\",\"rounds\":%u,\"built\":%s,\"peer_stopped\":%s,",
        config.concurrent_rounds, built ? "true" : "false", peer_ok ? "false" : "true");
    uint32_t overlap = 0u;
    uint32_t mismatches = 0u;
    uint32_t bad_stack = 0u;
    uint32_t bad_regs = 0u;
    uint32_t distinct_pairs = 0u;
    const uint32_t done_rounds = rounds_completed[0] < rounds_completed[1] ? rounds_completed[0]
                                                                          : rounds_completed[1];
    for (uint32_t r = 0u; built && r < done_rounds; r++) {
        const round_result *a = &round_results[0][r];
        const round_result *b = &round_results[1][r];
        if (a->enter_ns < b->exit_ns && b->enter_ns < a->exit_ns) {
            overlap++;
        }
        mismatches += (a->same_as_reference ? 0u : 1u) + (b->same_as_reference ? 0u : 1u);
        bad_stack += (a->esp_delta != 48 ? 1u : 0u) + (b->esp_delta != 48 ? 1u : 0u);
        bad_regs += (a->regs_preserved && a->fs0_preserved ? 0u : 1u) +
                    (b->regs_preserved && b->fs0_preserved ? 0u : 1u);
        distinct_pairs += a->source_index != b->source_index ? 1u : 0u;
    }
    put(&l, "\"completed_rounds\":[%u,%u],\"aborted\":%s,", rounds_completed[0],
        rounds_completed[1], phase_aborted ? "true" : "false");
    put(&l, "\"overlapping_rounds\":%u,\"mismatches\":%u,\"bad_stack\":%u,\"bad_regs\":%u,",
        overlap, mismatches, bad_stack, bad_regs);
    put(&l, "\"different_source_rounds\":%u,\"max_inside\":%u,\"entry_esp\":%u,", distinct_pairs,
        __atomic_load_n(&inside_max, __ATOMIC_SEQ_CST), entry_esp);
    put(&l, "\"results\":[");
    for (uint32_t r = 0u; built && r < done_rounds && r < DETAIL_ROUNDS; r++) {
        for (uint32_t t = 0u; t < 2u; t++) {
            const round_result *x = &round_results[t][r];
            put(&l, "%s{\"thread\":%u,\"round\":%u,\"source\":%u,\"hresult\":%u,\"size\":%u,\"hex\":",
                (r == 0u && t == 0u) ? "" : ",", t, r, x->source_index, x->hresult, x->output_size);
            put_hex(&l, x->output, x->output_copied);
            put(&l, "}");
        }
    }
    put(&l, "],\"references\":[");
    for (uint32_t i = 0u; i < reference_count; i++) {
        put(&l, "%s{\"size\":%u,\"flags\":%u,\"hresult\":%u,\"hex\":", i == 0u ? "" : ",",
            references[i].source_size, references[i].flags, references[i].hresult);
        put_hex(&l, references[i].output.bytes, references[i].output.copied);
        put(&l, "}");
    }
    put(&l, "]}\n");
    emit(&l);
}

static void concurrent_phase(uint32_t entry_esp)
{
    bool built = false;
    bool peer_ok = false;
    if (reference_count >= MAX_REFERENCES && config.concurrent_rounds <= MAX_ROUNDS &&
        make_guest_context(&peer_context, STACK_REQUEST) &&
        true) {
        sync_arrived = 0u;
        phase_aborted = false;
        rounds_completed[0] = rounds_completed[1] = 0u;
        built = true;
        pthread_t peer;
        if (pthread_create(&peer, NULL, peer_thread, NULL) == 0) {
            run_rounds(0u, entry_esp - 0x800u, entry_esp - 0x900u);
            pthread_join(peer, NULL);
            peer_ok = __atomic_load_n(&peer_stopped, __ATOMIC_SEQ_CST) == 0u;
        }
    }
    emit_concurrent(entry_esp, built, peer_ok);
}

static void write_call_body(line *l, const char *kind, uint32_t seq, uint32_t thread_index,
                            const uint32_t *frame, uint32_t flags, uint32_t size,
                            const uint8_t *source, size_t source_copied, bool source_ok)
{
    put(l, "{\"kind\":\"%s\",\"seq\":%u,\"thread\":%u,\"flags\":%u,\"size\":%u,", kind, seq,
        thread_index, flags, size);
    put(l, "\"frame\":[");
    for (uint32_t i = 0u; i < FRAME_DWORDS; i++) {
        put(l, "%s%u", i == 0u ? "" : ",", frame[i]);
    }
    put(l, "],\"source_readable\":%s,\"source_hex\":", source_ok ? "true" : "false");
    put_hex(l, source, source_copied);
}

static void probe_compiler(void)
{
    if (tls.suppress) {
        /* A concurrent-phase round entering through the route: just the body. */
        compile_guts(&tls.last_enter, &tls.last_exit);
        return;
    }
    const uint32_t entry_esp = g_esp;
    uint32_t frame[FRAME_DWORDS] = {0};
    const bool frame_ok = safe_read(entry_esp, frame, sizeof(frame));
    const uint32_t source_pointer = frame[2];
    const uint32_t source_size = frame[3];
    const uint32_t flags = frame[4];
    const uint32_t slot = frame[6];
    uint8_t source[MAX_HEX_BYTES];
    const size_t source_copied = source_size < MAX_HEX_BYTES ? source_size : MAX_HEX_BYTES;
    const bool source_ok = frame_ok && source_copied > 0u &&
                           safe_read(source_pointer, source, source_copied);
    if (!tls.has_index) {
        tls.thread_index = __atomic_fetch_add(&next_thread_index, 1u, __ATOMIC_SEQ_CST);
        tls.has_index = true;
    }
    const uint32_t seq = __atomic_fetch_add(&next_seq, 1u, __ATOMIC_SEQ_CST);
    const bool first_for_thread = tls.calls_started++ == 0u;
    const regs before = capture();
    uint32_t slot_before = 0u;
    if (!frame_ok || !safe_read(slot, &slot_before, sizeof(slot_before))) {
        slot_before = 0u;
    }

    char buffer[1u << 16];
    line l = {buffer, 0u, sizeof(buffer)};
    write_call_body(&l, "begin", seq, tls.thread_index, frame, flags, source_size, source,
                    source_ok ? source_copied : 0u, source_ok);
    put(&l, "}\n");
    emit(&l);

    if (config.rendezvous_ms != 0u && first_for_thread) {
        rendezvous();
    }
    const uint32_t fail_at = (config.fail_alloc != 0u && seq == config.fail_call)
                                 ? config.fail_alloc
                                 : 0u;
    reset_call_state(fail_at);
    if (config.concurrent_serial != 0u) {
        pthread_mutex_lock(&serial_lock);
    }
    uint64_t enter_ns = 0u;
    uint64_t exit_ns = 0u;
    compile_guts(&enter_ns, &exit_ns);
    if (config.concurrent_serial != 0u) {
        pthread_mutex_unlock(&serial_lock);
    }
    const uint32_t max_inside = __atomic_load_n(&inside_max, __ATOMIC_SEQ_CST);

    const regs after = capture();
    const output_copy first = read_output(slot, after.eax);
    const uint32_t allocs = tls.alloc_count;
    const uint32_t frees = tls.free_count;
    const uint32_t outstanding = tls.live_count;
    const uint32_t injected = tls.alloc_injected;
    const uint32_t exception_flag = tls.alloc_exception_flag_seen;
    const uint32_t logged = tls.log_count;

    line r = {buffer, 0u, sizeof(buffer)};
    write_call_body(&r, "call", seq, tls.thread_index, frame, flags, source_size, source,
                    source_ok ? source_copied : 0u, source_ok);
    put(&r, ",\"enter_ns\":%llu,\"exit_ns\":%llu,\"max_inside\":%u,", (unsigned long long)enter_ns,
        (unsigned long long)exit_ns, max_inside);
    put(&r, "\"hresult\":%u,\"esp_delta\":%d,\"regs_preserved\":%s,\"fs0_preserved\":%s,",
        after.eax, (int)(after.esp - before.esp),
        (after.ebx == before.ebx && after.esi == before.esi && after.edi == before.edi &&
         after.ebp == before.ebp)
            ? "true"
            : "false",
        after.fs0 == before.fs0 ? "true" : "false");
    put(&r, "\"output\":");
    if (first.has_output) {
        put(&r, "{\"object\":%u,\"data\":%u,\"size\":%u,\"hex\":", first.object, first.data,
            first.size);
        put_hex(&r, first.bytes, first.copied);
        put(&r, "}");
    } else {
        put(&r, "null");
    }
    put(&r, ",\"allocs\":%u,\"frees\":%u,\"outstanding\":%u,\"injected\":%u,", allocs, frees,
        outstanding, injected);
    put(&r, "\"exception_flag_allocs\":%u,\"alloc_log\":[", exception_flag);
    for (uint32_t i = 0u; i < logged; i++) {
        put(&r, "%s[%u,%u,%u]", i == 0u ? "" : ",", tls.log[i][0], tls.log[i][1], tls.log[i][2]);
    }
    put(&r, "],\"repeats\":[");

    /* Repeated use in one process: same frame, same source, same registers. Each repeat is
     * compared with the first call and its heap balance is recorded. */
    const uint32_t repeats = config.repeat < MAX_REPEATS ? config.repeat : MAX_REPEATS;
    for (uint32_t i = 0u; i < repeats && source_ok; i++) {
        g_esp = entry_esp;
        g_ebx = before.ebx;
        g_esi = before.esi;
        g_edi = before.edi;
        g_ebp = before.ebp;
        if (slot != 0u) {
            *(uint32_t *)(uintptr_t)slot = slot_before;
        }
        reset_call_state(0u);
        tls.in_call = true;
        recomp_original_003EE2B3();
        tls.in_call = false;
        const regs again = capture();
        const output_copy repeat = read_output(slot, again.eax);
        put(&r, "%s{\"hresult\":%u,\"esp_delta\":%d,\"same_output\":%s,\"same_hresult\":%s,",
            i == 0u ? "" : ",", again.eax, (int)(again.esp - before.esp),
            same_output(&first, &repeat) ? "true" : "false",
            again.eax == after.eax ? "true" : "false");
        put(&r, "\"output_hex\":");
        put_hex(&r, repeat.bytes, repeat.has_output ? repeat.copied : 0u);
        put(&r, ",\"allocs\":%u,\"frees\":%u,\"outstanding\":%u}", tls.alloc_count,
            tls.free_count, tls.live_count);
    }
    put(&r, "]}\n");
    emit(&r);

    if (config.concurrent_rounds != 0u && source_ok && after.eax == 0u && first.has_output &&
        reference_count < MAX_REFERENCES) {
        /* Keep the title's own source in guest memory the probe owns, plus its outputs. */
        const uint32_t copy = region_alloc(0x1000u);
        if (copy != 0u) {
            memcpy((void *)(uintptr_t)copy, source, source_copied);
            references[reference_count].source_pointer = copy;
            references[reference_count].source_size = source_size;
            references[reference_count].flags = flags;
            references[reference_count].hresult = after.eax;
            references[reference_count].output = first;
            reference_count++;
        }
    }
    const uint32_t done = __atomic_add_fetch(&finished_calls, 1u, __ATOMIC_SEQ_CST);
    if (config.exit_after != 0u && done >= config.exit_after) {
        _exit(0);
    }
}

static uint32_t route_phase_started;

/* The concurrent phase runs from the title's first call of the lock free buffer getter 0x003E6714
 * after three real compiles have supplied the sources. The getter takes no compiler lock,
 * so the rounds can enter through xdk_original_dispatch from inside it. */
static void maybe_run_route_phase(void)
{
    if (config.concurrent_rounds == 0u || tls.suppress ||
        __atomic_load_n(&reference_count, __ATOMIC_SEQ_CST) < MAX_REFERENCES ||
        __atomic_exchange_n(&route_phase_started, 1u, __ATOMIC_SEQ_CST) != 0u) {
        return;
    }
    const regs saved = capture();
    const uint32_t saved_ecx = g_ecx;
    const uint32_t saved_edx = g_edx;
    concurrent_phase(g_esp);
    g_eax = saved.eax;
    g_esp = saved.esp;
    g_ebx = saved.ebx;
    g_esi = saved.esi;
    g_edi = saved.edi;
    g_ebp = saved.ebp;
    g_ecx = saved_ecx;
    g_edx = saved_edx;
}


/* ---- corpus phase (T387) ---------------------------------------------------------- */

#define CORPUS_NAME_BYTES 128u
#define CORPUS_PATH_BYTES 1024u
#define CORPUS_PAGE 0x1000u
#define CORPUS_MAX_SOURCE 0x100000u
#define CORPUS_STACK_FILL_BYTES 0x4000u
#define CORPUS_EXPECTED_ESP_DELTA 48 /* return address plus the 11 stdcall arguments */
#define CORPUS_MAX_ENTRIES 16384u
#define CORPUS_MAX_SOURCES 256u
#define CORPUS_PEER_STACK 0x100000u
#define CORPUS_STATIC_ADDRESS 0x405B00u
#define CORPUS_STATIC_BYTES 0x120u

static uint32_t corpus_started;
static const char *const corpus_placements[2] = {"padded", "flush"};

static void json_string(line *l, const char *text)
{
    put(l, "\"");
    for (const char *c = text; *c != '\0'; c++) {
        if (*c == '"' || *c == '\\' || (unsigned char)*c < 0x20u) {
            put(l, "?");
        } else {
            put(l, "%c", *c);
        }
    }
    put(l, "\"");
}

/* A zero filled source mapping, or with `flush` the source ending exactly where an
 * unmapped guard page begins. Returns the guest address of the first source byte, 0 on
 * failure. */
static uint32_t corpus_place(const uint8_t *data, size_t length, bool flush)
{
    const uint32_t body_pages = (uint32_t)((length + CORPUS_PAGE - 1u) / CORPUS_PAGE) + 1u;
    const uint32_t bytes = (body_pages + 1u) * CORPUS_PAGE;
    const uint32_t base = region_alloc(bytes);
    if (base == 0u) {
        return 0u;
    }
    memset((void *)(uintptr_t)base, 0, bytes);
    if (!flush) {
        memcpy((void *)(uintptr_t)base, data, length);
        return base;
    }
    const uint32_t guard = base + body_pages * CORPUS_PAGE;
    if (mprotect((void *)(uintptr_t)guard, CORPUS_PAGE, PROT_NONE) != 0) {
        return 0u;
    }
    const uint32_t start = guard - (uint32_t)length;
    memcpy((void *)(uintptr_t)start, data, length);
    return start;
}

static bool corpus_load(const char *path, uint8_t **data, size_t *length)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }
    uint8_t *buffer = malloc(CORPUS_MAX_SOURCE + 1u);
    const size_t count = buffer != NULL ? fread(buffer, 1u, CORPUS_MAX_SOURCE + 1u, file) : 0u;
    fclose(file);
    if (buffer == NULL || count > CORPUS_MAX_SOURCE) {
        free(buffer);
        return false;
    }
    *data = buffer;
    *length = count;
    return true;
}

/* One distinct source file placed once in guest memory, shared by every spec line and
 * every repeat that names it. */
typedef struct {
    char path[CORPUS_PATH_BYTES];
    uint32_t pointer;
    uint32_t length;
} corpus_source;

typedef struct {
    char name[CORPUS_NAME_BYTES];
    uint32_t flags;
    uint32_t source;
} corpus_entry;

/* The frame, result slots and thread number one compiler call runs on. */
typedef struct {
    uint32_t base_esp;
    uint32_t slot;
    uint32_t constants_slot;
    uint32_t errors_slot;
    uint32_t thread;
} corpus_lane;

static corpus_source corpus_sources[CORPUS_MAX_SOURCES];
static uint32_t corpus_source_count;
static corpus_entry corpus_entries[CORPUS_MAX_ENTRIES];
static uint32_t corpus_entry_count;

/* FNV-1a over the guest static data the compiler keeps (0x405B00..0x405C1C, docs
 * shader-original-routes.md), 0 when unreadable. */
static uint64_t static_state_hash(void)
{
    uint8_t bytes[CORPUS_STATIC_BYTES];
    if (!safe_read(CORPUS_STATIC_ADDRESS, bytes, sizeof(bytes))) {
        return 0u;
    }
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0u; i < sizeof(bytes); i++) {
        hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    }
    return hash;
}

static void put_buffer(line *l, const char *label, const output_copy *buffer)
{
    put(l, ",\"%s_slot_object\":%u,\"%s\":", label, buffer->object, label);
    if (buffer->has_output) {
        put(l, "{\"size\":%u,\"copied\":%u,\"hex\":", buffer->size, buffer->copied);
        put_hex(l, buffer->bytes, buffer->copied);
        put(l, "}");
    } else {
        put(l, "null");
    }
}

/* One compiler call through the production route and its lock. Every field of the
 * result is read back and written as one record, so a stop later still leaves it. */
static void corpus_call(const corpus_entry *entry, const corpus_lane *lane, uint32_t index,
                        uint32_t round, const char *phase)
{
    char buffer[1u << 16];
    const corpus_source *source = &corpus_sources[entry->source];
    const bool flush = config.corpus_flush != 0u;
    const char *placement = corpus_placements[flush ? 1 : 0];
    if (config.corpus_stack_fill != NO_STACK_FILL) {
        memset((void *)(uintptr_t)(lane->base_esp - CORPUS_STACK_FILL_BYTES),
               (int)(config.corpus_stack_fill & 0xFFu), CORPUS_STACK_FILL_BYTES);
    }
    reference_case frame_source;
    memset(&frame_source, 0, sizeof(frame_source));
    frame_source.source_pointer = source->pointer;
    frame_source.source_size = source->length;
    frame_source.flags = entry->flags;
    build_frame(lane->base_esp, &frame_source, lane->slot);
    if (config.corpus_outputs != 0u) {
        /* Argument i is frame dword i + 1: constants is argument 4, errors argument 6. */
        *(uint32_t *)(uintptr_t)lane->constants_slot = 0u;
        *(uint32_t *)(uintptr_t)lane->errors_slot = 0u;
        ((uint32_t *)(uintptr_t)lane->base_esp)[5] = lane->constants_slot;
        ((uint32_t *)(uintptr_t)lane->base_esp)[7] = lane->errors_slot;
    }
    g_esp = lane->base_esp;
    g_ebx = 0x11110000u + lane->thread;
    g_esi = 0x22220000u + lane->thread;
    g_edi = 0x33330000u + lane->thread;
    g_ebp = 0x44440000u + lane->thread;
    const uint32_t fs0 = guest_u32(g_fs_base);

    line l = {buffer, 0u, sizeof(buffer)};
    put(&l, "{\"kind\":\"corpus_begin\",\"index\":%u,\"round\":%u,\"thread\":%u,\"phase\":\"%s\","
            "\"name\":", index, round, lane->thread, phase);
    json_string(&l, entry->name);
    put(&l, ",\"flags\":%u,\"length\":%u,\"placement\":\"%s\"}\n", entry->flags, source->length,
        placement);
    emit(&l);

    reset_call_state(0u);
    const uint64_t static_before = static_state_hash();
    tls.suppress = true;
    const uint64_t ask_ns = now_ns();
    const bool dispatched = xdk_original_dispatch(COMPILER_ADDRESS);
    const uint64_t done_ns = now_ns();
    tls.suppress = false;
    const uint64_t static_after = static_state_hash();
    const uint32_t hresult = g_eax;
    /* The slot is read whatever the HRESULT: a failure that leaves an object is a finding. */
    const output_copy got = read_output(lane->slot, 0u);
    const output_copy constants = read_output(lane->constants_slot, 0u);
    const output_copy errors = read_output(lane->errors_slot, 0u);
    const bool regs_preserved = g_ebx == 0x11110000u + lane->thread &&
                                g_esi == 0x22220000u + lane->thread &&
                                g_edi == 0x33330000u + lane->thread &&
                                g_ebp == 0x44440000u + lane->thread;

    line r = {buffer, 0u, sizeof(buffer)};
    put(&r, "{\"kind\":\"corpus_call\",\"index\":%u,\"round\":%u,\"thread\":%u,\"phase\":\"%s\","
            "\"name\":", index, round, lane->thread, phase);
    json_string(&r, entry->name);
    put(&r, ",\"placement\":\"%s\",\"dispatched\":%s,\"hresult\":%u,\"esp_delta\":%d,",
        placement, dispatched ? "true" : "false", hresult, (int)(g_esp - lane->base_esp));
    put(&r, "\"regs_preserved\":%s,\"fs0_preserved\":%s,", regs_preserved ? "true" : "false",
        guest_u32(g_fs_base) == fs0 ? "true" : "false");
    put(&r, "\"ask_ns\":%llu,\"enter_ns\":%llu,\"exit_ns\":%llu,\"done_ns\":%llu,",
        (unsigned long long)ask_ns, (unsigned long long)tls.last_enter,
        (unsigned long long)tls.last_exit, (unsigned long long)done_ns);
    put(&r, "\"static_before\":\"%016llx\",\"static_after\":\"%016llx\",",
        (unsigned long long)static_before, (unsigned long long)static_after);
    put(&r, "\"allocs\":%u,\"frees\":%u,\"outstanding\":%u,\"slot_object\":%u,\"output\":",
        tls.alloc_count, tls.free_count, tls.live_count, got.object);
    if (got.has_output) {
        put(&r, "{\"size\":%u,\"copied\":%u,\"hex\":", got.size, got.copied);
        put_hex(&r, got.bytes, got.copied);
        put(&r, "}");
    } else {
        put(&r, "null");
    }
    if (config.corpus_outputs != 0u) {
        put_buffer(&r, "constants", &constants);
        put_buffer(&r, "errors", &errors);
    }
    put(&r, "}\n");
    emit(&r);
}

static void corpus_fail(const char *name, const char *message)
{
    char buffer[512];
    line l = {buffer, 0u, sizeof(buffer)};
    put(&l, "{\"kind\":\"corpus_error\",\"name\":");
    json_string(&l, name);
    put(&l, ",\"error\":\"%s\"}\n", message);
    emit(&l);
    _exit(3);
}

/* The index of the placed source for `path`, loading and placing it on first use. */
static uint32_t corpus_source_for(const char *name, const char *path)
{
    for (uint32_t i = 0u; i < corpus_source_count; i++) {
        if (strcmp(corpus_sources[i].path, path) == 0) {
            return i;
        }
    }
    uint8_t *data = NULL;
    size_t length = 0u;
    if (corpus_source_count >= CORPUS_MAX_SOURCES || !corpus_load(path, &data, &length)) {
        corpus_fail(name, "source unreadable");
    }
    corpus_source *source = &corpus_sources[corpus_source_count];
    snprintf(source->path, sizeof(source->path), "%s", path);
    source->pointer = corpus_place(data, length, config.corpus_flush != 0u);
    source->length = (uint32_t)length;
    free(data);
    if (source->pointer == 0u) {
        corpus_fail(name, "source mapping failed");
    }
    return corpus_source_count++;
}

/* Every spec line, in order. The same name may appear more than once: the driver builds
 * its orderings (a failing source between two successes) as repeated lines. */
static void corpus_read_spec(void)
{
    FILE *spec = fopen(config.corpus_spec, "r");
    if (spec == NULL) {
        perror("--probe-corpus");
        _exit(3);
    }
    char text[CORPUS_NAME_BYTES + CORPUS_PATH_BYTES + 32u];
    while (fgets(text, sizeof(text), spec) != NULL) {
        char name[CORPUS_NAME_BYTES];
        char path[CORPUS_PATH_BYTES];
        uint32_t flags = 0u;
        if (sscanf(text, "%127s %u %1023s", name, &flags, path) != 3) {
            continue;
        }
        if (corpus_entry_count >= CORPUS_MAX_ENTRIES) {
            corpus_fail(name, "too many spec lines");
        }
        corpus_entry *entry = &corpus_entries[corpus_entry_count++];
        memcpy(entry->name, name, sizeof(entry->name));
        entry->flags = flags;
        entry->source = corpus_source_for(name, path);
    }
    fclose(spec);
}

/* ---- corpus concurrent phase (T476) ------------------------------------------------ */

static uint32_t corpus_peer_stopped;

/* Each thread runs `concurrent_rounds` rounds. In round r thread t takes entry
 * (r + t * count / 2) mod count, so the two threads never hold the same entry and a
 * full pass of `count` rounds runs every entry once on each thread. */
static void corpus_rounds(const corpus_lane *lane)
{
    const uint32_t count = corpus_entry_count;
    for (uint32_t round = 0u; round < config.corpus_concurrent; round++) {
        const uint32_t index = (round + lane->thread * (count / 2u)) % count;
        if (!round_sync()) {
            return;
        }
        corpus_call(&corpus_entries[index], lane, index, round, "concurrent");
        rounds_completed[lane->thread] = round + 1u;
    }
}

static void *corpus_peer(void *argument)
{
    (void)argument;
    g_fs_base = peer_context.fs_base;
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        corpus_lane lane;
        lane.base_esp = peer_context.stack_top - FRAME_DWORDS * 4u;
        lane.slot = peer_context.slot;
        lane.constants_slot = peer_context.slot + 0x10u;
        lane.errors_slot = peer_context.slot + 0x20u;
        lane.thread = 1u;
        corpus_rounds(&lane);
    } else {
        __atomic_store_n(&corpus_peer_stopped, 1u, __ATOMIC_SEQ_CST);
        sync_abort();
    }
    host_run_disarm();
    return NULL;
}

static void corpus_concurrent_phase(uint32_t entry_esp)
{
    char buffer[512];
    line l = {buffer, 0u, sizeof(buffer)};
    bool built = false;
    if (corpus_entry_count >= 2u && make_guest_context(&peer_context, CORPUS_PEER_STACK)) {
        sync_arrived = 0u;
        phase_aborted = false;
        rounds_completed[0] = rounds_completed[1] = 0u;
        built = true;
        pthread_t peer;
        if (pthread_create(&peer, NULL, corpus_peer, NULL) == 0) {
            corpus_lane lane;
            lane.base_esp = entry_esp - 0x800u;
            lane.slot = entry_esp - 0x900u;
            lane.constants_slot = entry_esp - 0x910u;
            lane.errors_slot = entry_esp - 0x920u;
            lane.thread = 0u;
            corpus_rounds(&lane);
            pthread_join(peer, NULL);
        }
    }
    put(&l, "{\"kind\":\"corpus_concurrent\",\"rounds\":%u,\"count\":%u,\"built\":%s,",
        config.corpus_concurrent, corpus_entry_count, built ? "true" : "false");
    put(&l, "\"completed_rounds\":[%u,%u],\"aborted\":%s,\"peer_stopped\":%s,\"max_inside\":%u}\n",
        rounds_completed[0], rounds_completed[1], phase_aborted ? "true" : "false",
        __atomic_load_n(&corpus_peer_stopped, __ATOMIC_SEQ_CST) != 0u ? "true" : "false",
        __atomic_load_n(&inside_max, __ATOMIC_SEQ_CST));
    emit(&l);
}

/* Runs from the first getter call after three compiles, from a frame well below the
 * getter's own. The process ends here so the rest of the boot never runs. */
static void maybe_run_corpus_phase(void)
{
    if (config.corpus_spec == NULL || tls.suppress ||
        __atomic_load_n(&finished_calls, __ATOMIC_SEQ_CST) < 3u ||
        __atomic_exchange_n(&corpus_started, 1u, __ATOMIC_SEQ_CST) != 0u) {
        return;
    }
    corpus_read_spec();
    const uint32_t entry_esp = g_esp;
    const corpus_lane lane = {entry_esp - 0x800u, entry_esp - 0x900u, entry_esp - 0x910u,
                              entry_esp - 0x920u, 0u};
    const uint32_t repeats = config.corpus_repeat < MAX_REPEATS ? config.corpus_repeat : MAX_REPEATS;
    for (uint32_t index = 0u; index < corpus_entry_count; index++) {
        /* Round 0 is the first call, rounds 1.. repeat it in the same process. */
        for (uint32_t round = 0u; round <= repeats; round++) {
            corpus_call(&corpus_entries[index], &lane, index, round, "sequential");
        }
    }
    if (config.corpus_concurrent != 0u) {
        corpus_concurrent_phase(entry_esp);
    }
    static char done[128];
    line l = {done, 0u, sizeof(done)};
    put(&l, "{\"kind\":\"corpus_done\",\"count\":%u}\n", corpus_entry_count);
    emit(&l);
    _exit(0);
}

static void probe_getter(void)
{
    maybe_run_corpus_phase();
    maybe_run_route_phase();
    recomp_original_003E6714();
}

recomp_func_t __wrap_recomp_lookup_original(uint32_t address)
{
    if (address == COMPILER_ADDRESS) {
        return probe_compiler;
    }
    if (address == GETTER_ADDRESS) {
        return probe_getter;
    }
    return __real_recomp_lookup_original(address);
}

static bool take_value(int argc, char **argv, int *index, const char *name, uint32_t *out)
{
    if (strcmp(argv[*index], name) != 0) {
        return false;
    }
    if (*index + 1 >= argc) {
        fprintf(stderr, "%s needs a value\n", name);
        exit(2);
    }
    *out = (uint32_t)strtoul(argv[++*index], NULL, 0);
    return true;
}

int main(int argc, char **argv)
{
    if (argc == 1) {
        /* Run with no arguments (the mutation harness runs every target that way) it only
         * says what it is and succeeds, so it can be rebuilt there without a false verdict. */
        puts("tsfp_shader_probe: test instrument, see tests/c/shader_compiler_probe.c");
        return 0;
    }
    char **forwarded = calloc((size_t)argc + 1u, sizeof(*forwarded));
    int count = 0;
    if (forwarded == NULL) {
        return 2;
    }
    for (int i = 0; i < argc; i++) {
        if (i > 0 && strcmp(argv[i], "--probe-out") == 0 && i + 1 < argc) {
            config.out_path = argv[++i];
        } else if (i > 0 && strcmp(argv[i], "--probe-corpus") == 0 && i + 1 < argc) {
            config.corpus_spec = argv[++i];
        } else if (i > 0 && (take_value(argc, argv, &i, "--probe-repeat", &config.repeat) ||
                             take_value(argc, argv, &i, "--probe-rendezvous",
                                        &config.rendezvous_ms) ||
                             take_value(argc, argv, &i, "--probe-fail-alloc", &config.fail_alloc) ||
                             take_value(argc, argv, &i, "--probe-fail-call", &config.fail_call) ||
                             take_value(argc, argv, &i, "--probe-exit-after",
                                        &config.exit_after) ||
                             take_value(argc, argv, &i, "--probe-concurrent",
                                        &config.concurrent_rounds) ||
                             take_value(argc, argv, &i, "--probe-concurrent-serial",
                                        &config.concurrent_serial) ||
                             take_value(argc, argv, &i, "--probe-concurrent-route",
                                        &config.concurrent_route) ||
                             take_value(argc, argv, &i, "--probe-corpus-flush",
                                        &config.corpus_flush) ||
                             take_value(argc, argv, &i, "--probe-corpus-stack-fill",
                                        &config.corpus_stack_fill) ||
                             take_value(argc, argv, &i, "--probe-corpus-outputs",
                                        &config.corpus_outputs) ||
                             take_value(argc, argv, &i, "--probe-corpus-repeat",
                                        &config.corpus_repeat) ||
                             take_value(argc, argv, &i, "--probe-heap-log", &config.heap_log) ||
                             take_value(argc, argv, &i, "--probe-corpus-concurrent",
                                        &config.corpus_concurrent))) {
            continue;
        } else {
            forwarded[count++] = argv[i];
        }
    }
    if (config.out_path != NULL) {
        out_fd = open(config.out_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (out_fd < 0) {
            perror("--probe-out");
            return 2;
        }
    }
    return tsfp_host_main(count, forwarded);
}
