/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The seam between lifted guest code and the XDK HLE modules, keyed by ADDRESS.
 *
 * ================= WHY AN ADDRESS DISPATCHER IS NEEDED AT ALL =================
 *
 * The kernel path is keyed by ORDINAL because xboxkrnl is imported: the XBE has a
 * thunk table, the loader patches it, and `kernel_thunk.c` is the loader. D3D8,
 * DSOUND and XAPILIB are NOT imported. They are statically linked into the title and
 * live at known virtual addresses inside it, so there is no table to patch and no
 * ordinal to key on. The boundary is the set of addresses game `.text` transfers
 * control to, which `tools/gen_d3d8_surface.py` measures: 236 functions across seven
 * sections, 831 call sites.
 *
 * Until this file existed, `d3d8_hle_call`, `dsound_hle_call` and `xinput_hle_call`
 * had NO caller outside their own test suites. Three fully built, fully tested
 * boundaries were unreachable, and the XDK-surface metric read 0/236 and would have
 * kept reading zero however much of each module was implemented. This is the file
 * that makes them reachable, and `xdk_thunk_module_call_count` is how a test asserts
 * that it actually does rather than merely compiling.
 *
 * ========================= ONE DISPATCHER, NOT THREE =========================
 *
 * There is exactly one dispatch function and one ABI table. A per-library dispatcher
 * would triplicate the hardest part of the problem -- establishing how many bytes to
 * pop -- and triplicate the chance of getting it wrong in one copy only. The module a
 * call belongs to is a FIELD on the measured row, not a different code path: routing
 * is a switch at the bottom of a single function.
 *
 * It also reuses, rather than reimplements, what the kernel path already solved:
 *   - FRAME CONSTRUCTION is `kernel_call_frame`, unchanged. An XDK handler reaches
 *     its stack arguments with `kernel_frame_arg` and its register arguments with
 *     `kernel_frame_reg_arg`, exactly as a `Kf*` kernel handler does. None of the
 *     three `*_call` signatures changed: all three already take `void *context`, and
 *     `context` is now a `const kernel_call_frame *`.
 *   - THE ORDERED TRACE is `thunk_trace.h`, shared with the kernel path, so that the
 *     interleaving of kernel calls and XDK calls stays recoverable. That ordering is
 *     the deliverable of a bring-up run.
 *   - THE STOP POLICY is `host_run_stop`, with the same default and the same
 *     record-before-stopping discipline.
 *
 * ========================= THE STACK CONTRACT =========================
 *
 * Identical to the kernel path's, because the lifted code reaches both the same way:
 * at the instant the dispatcher is entered the caller has already pushed the guest
 * return address, so `g_esp` points at it and stack argument N sits at
 * `g_esp + 4 + 4*N`. WE are the callee, so WE do the cleanup the real `ret N` would
 * have done. Pop the wrong number of bytes and `esp` desyncs permanently; that does
 * not crash, it makes every later observation fiction.
 *
 * ==================== HOW A CALL ACTUALLY ARRIVES, AND THE GAP ====================
 *
 * `recomp_lookup_manual` is consulted FIRST by every indirect-dispatch macro the
 * lifter emits -- `RECOMP_ICALL`, `RECOMP_ICALL_SAFE`, `RECOMP_ICALL_SAFE_AT` and
 * `RECOMP_ITAIL` all do `_fn = recomp_lookup_manual(_va)` before anything else. That
 * is the production entry point and it is this file's.
 *
 * IT IS NOT, BY ITSELF, ENOUGH, AND THE REASON IS MEASURED. A DIRECT call is not
 * emitted as an indirect dispatch. The lifter emits it as
 * `RECOMP_ABI_CALL(0x003D57D0u, sub_003D57D0)`, which with `RECOMP_ABI_CHECK` off
 * expands to `(fn)()`: the VA is discarded and the lifted body is called by symbol.
 * A tail jump is worse -- a bare `sub_XXXX(); return;` with no VA at all. The surface
 * generator reports `slot 0, table 0, register 0, thunk 0` for all 831 attributed
 * sites, i.e. EVERY ONE IS DIRECT, and the 63 C++ adjustor thunks that are reached
 * through a vtable slot are themselves in game `.text` and tail-jump into the XDK
 * section directly. So on the lift as it stands today, nothing reaches this file.
 *
 * The lifter already has the mechanism to fix that and it is the same one the
 * decompilation phase runs on: `--manual-functions FILE`. An address in that file
 * gets no generated body, and every DIRECT call to it is re-emitted as
 * `PUSH32(esp, <retva>); RECOMP_ICALL_SAFE(<addr>, _icall_esp);` while every tail
 * jump becomes `RECOMP_ITAIL(<addr>)` -- both of which consult
 * `recomp_lookup_manual`. `tools/gen_xdk_manual_list.py` writes that file from the
 * measured surface. `docs/xdk-dispatch.md` records what a re-lift costs and what
 * else it needs.
 *
 * ================= WHAT THIS BOUNDARY CANNOT SEE, ON THE RECORD =================
 *
 * An address-keyed dispatcher intercepts CALLS. It does not and cannot intercept
 * code that does the work inline, and `docs/d3d8-usage.md` §9 measured exactly that
 * for the hottest GPU operation in the title:
 *
 *   - `D3DDevice_SetRenderState` IS INLINED into game `.text` at 0x000211C0. It reads
 *     D3D8's private header table at 0x475B08, ORs bits into D3D8's dirty mask at
 *     0x3E3AB8 (43 references from game chunks) and stores into D3D8's deferred
 *     shadow array at 0x3E3CC0 (3 references). Render state is therefore NOT a
 *     function-call boundary in this title, and no dispatcher can make it one.
 *   - Sampler state is written the same way, straight into D3D8's texture state array
 *     at 0x3E3AC0 + stage*0x80, 5 references from game chunks.
 *   - 0x003D6C90 and 0x003D6C60 are in the surface table and are NOT API functions:
 *     they are D3D8's private "emit one header and one parameter" primitives, called
 *     directly with a raw NV2A method header already in ecx. A dispatcher sees
 *     hardware methods there, not a D3D call.
 *
 * So this file makes 236 measured addresses reachable. It does NOT make the GPU
 * boundary complete, and anything that reads a 236/236 figure as "all GPU traffic is
 * intercepted" is reading it wrong.
 */

#ifndef TSFP_HOST_XDK_THUNK_H
#define TSFP_HOST_XDK_THUNK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "recomp_abi.h"

/**
 * The override seam, and the production entry point of this whole file.
 *
 * Declared here rather than only in the generated `recomp_types.h` for two reasons.
 * The generated header cannot be included by anything built under the project's strict
 * warning flags, and a suite that wants to exercise the real dispatch path has to be
 * able to resolve an address the same two-step way the lifter's macros do -- look up,
 * then call -- rather than reaching inside this module. `recomp_runtime.c` includes
 * both views of the ABI and is where a disagreement between them becomes a compile
 * error.
 *
 * Returns the normal dispatcher for an adopted surface address, a stop-only
 * dispatcher for an explicit override, or the synthetic monitor handler. Other
 * addresses return NULL. NULL is overwhelmingly the common answer: every indirect
 * call in the lifted program comes through here.
 */
/* Stop-only policy overrides are independent of the measured ABI/coverage registry.
 * Caller verifies each address is an original executable target. This API cannot
 * infer executable identity from an address or mapping probe. Adoption/reset/init
 * require quiescence; labels/reasons are copied, duplicates and adopted rows refused.
 * Duplicate labels at distinct addresses are allowed. Successful init clears overrides.
 * Direct generated symbol calls bypass lookup and need their own audited routing. */
#define XDK_STOP_LABEL_BYTES 128u
#define XDK_STOP_REASON_BYTES 256u
#define XDK_STOP_MAX_ENTRIES 256u
typedef struct xdk_stop_override {
    uint32_t address;
    const char *label;
    const char *reason;
} xdk_stop_override;
/* True only when the linked generated marker confirms all 14 stream/reference
 * stop trampolines. Weak marker absence or any incomplete boundary fails closed.
 * Pure capability query: no routing, policy, counter, trace or guest changes. */
bool xdk_thunk_stream_stops_ready(void);

/* Pure capability query for a trusted HOST array of normal dispatcher addresses.
 * Array storage must contain count readable uint32_t values for the whole call;
 * this is not a guarded guest-list API. NULL/empty/zero addresses, absent marker,
 * missing entries or marker values other than exactly1 fail closed. No routing,
 * policy, registration, counters, traces, guest reads/writes or register changes.
 * Caller chooses the required surface; duplicates do not add any capability. */
bool xdk_thunk_dispatch_boundaries_ready(const uint32_t *addresses,size_t count);

/* Direct generated trampolines use static caller-verified executable addresses and
 * diagnostic strings. This entry always stops, without registry, ABI or handler
 * lookup. Text must remain valid through stop reporting; null text uses diagnostics.
 * Caller is recorded only in the pending trace; no guest registers/stack are changed. */
void xdk_thunk_stop_at(uint32_t address, const char *label, const char *reason)
    __attribute__((noreturn));
bool xdk_thunk_stop_override_adopt(const xdk_stop_override *entries, size_t count);
void xdk_thunk_stop_override_reset(void);
size_t xdk_thunk_stop_override_count(void);
uint64_t xdk_thunk_stop_override_call_count(void);

/* Two bounded INDIRECT-only owned stream methods: 0x40733B stdcall1/RET4
 * and 0x4073D3 stdcall2/RET8.
 * NULL disables (default). Setup is quiescent; changes refuse active/pending
 * virtual calls. Enabling requires the 14 compiled stream stops and normal
 * dispatcher boundaries for all 41 adopted DSOUND rows. Existing explicit stops
 * win. Normal compiled bodies/direct routes remain unconditional stop entries.
 * Trusted callbacks must not mutate guest registers/stack or call configuration/
 * reset. Discontinuity owns its host policy cache; status owns guarded
 * output publication and protected ownership-alias refusal. It may stop after releasing locks. */
typedef uint32_t (*xdk_stream_virtual_handler)(uint32_t stream);
typedef uint32_t (*xdk_stream_status_handler)(uint32_t stream, uint32_t out);
bool xdk_thunk_set_stream_virtual_handlers(xdk_stream_virtual_handler discontinuity,
                                           xdk_stream_status_handler status);
/* Legacy setter configures (handler,NULL); NULL disables both methods. */
bool xdk_thunk_set_stream_virtual_handler(xdk_stream_virtual_handler handler);
/* Manual lookup reserves the configuration until that function is called. Host
 * same-thread cleanup MUST cancel an abandoned lookup (e.g. a pre-call ABI stop).
 * A different virtual lookup while reserved refuses without replacing the address.
 * Does not cancel an active call; its nested scope handles that cleanup. */
void xdk_thunk_stream_virtual_cancel_pending(void);
uint64_t xdk_thunk_stream_virtual_call_count(void);
uint64_t xdk_thunk_stream_virtual_refused_count(void);
size_t xdk_thunk_stream_virtual_pending_count(void);
size_t xdk_thunk_stream_virtual_active_count(void);

/* T392: the five INDIRECT stream methods a movie stream is called through (AddRef 0x40723F,
 * Release 0x407286, GetStatus 0x4073D3, Process 0x407424, Discontinuity 0x40733B). `owns` is given
 * the guest stack pointer at the return address and says whether the `this` argument above it is a
 * stream the movie model owns, with no side effect. `handler` then answers it (result in EAX,
 * `pop_bytes` of stdcall arguments above the return address) and must return true. When `owns` says
 * no, the call takes the route it took before: the startup virtual route for 0x40733B and 0x4073D3,
 * otherwise the compiled stop. Existing explicit stops win. Both NULL disables (default). Enabling
 * needs the 14 compiled stream stops. Callbacks must not change guest registers or the stack, and
 * the handler may stop after releasing its locks. */
typedef bool (*xdk_movie_method_owner)(uint32_t stack_pointer);
typedef bool (*xdk_movie_method_handler)(uint32_t address, uint32_t stack_pointer,
                                         uint32_t *result, uint32_t *pop_bytes);
bool xdk_thunk_set_movie_method_handler(xdk_movie_method_owner owns,
                                        xdk_movie_method_handler handler);
uint64_t xdk_thunk_movie_method_call_count(void);
/* T681: the same indirect methods for the passive completion model (GetStatus, Process, GetInfo of the passive
 * title streams, flag --passive-audio-completion). After the movie model declines, `owns` says whether the
 * `this` stream is a passive stream, `handler` answers it and returns true, or returns false to leave the call
 * to the startup virtual route and the compiled stop exactly as before. NULL disables (default). */
bool xdk_thunk_set_completion_method_handler(xdk_movie_method_owner owns,
                                             xdk_movie_method_handler handler);
uint64_t xdk_thunk_completion_method_call_count(void);

/* T421: IDirectSound::SynchPlayback (0x407A4C stdcall1/RET4), a DIRECT call from the XMV GetNextFrame,
 * routed through the one manual-list stop boundary of tools/config/movie_synch_boundaries.json.
 * `handler` gets the guest stack pointer at the return address, sets the EAX result and returns true,
 * the thunk layer pops the return address and the one argument. A handler that refuses stops the run
 * itself. NULL disables (default) and the compiled stop answers. Enabling needs the compiled stop
 * (xdk_thunk_synch_stop_ready), a lift without it refuses the setter. Setup is quiescent. */
typedef bool (*xdk_synch_handler)(uint32_t stack_pointer, uint32_t *result);
bool xdk_thunk_synch_stop_ready(void);
bool xdk_thunk_set_synch_handler(xdk_synch_handler handler);
uint64_t xdk_thunk_synch_call_count(void);
bool xdk_thunk_synch_route_enabled(void);

recomp_func_t recomp_lookup_manual(uint32_t xbox_va);

/**
 * Dispatch the XDK function at `address` directly, as a trampoline body.
 *
 * THE SECOND HALF OF THE RE-LIFT, AND NOT REDUNDANT. `--manual-functions` suppresses
 * the lifted body for an address but the generated dispatch table STILL carries an
 * entry for it, so `sub_XXXXXXXX` has to be defined by hand or the link fails. The
 * natural body is a one-line call to this function, which `tools/gen_xdk_manual_list.py`
 * emits for every measured address.
 *
 * Without it the only honest alternative is 236 empty functions, and an empty function
 * that the dispatch table can reach is a silent no-op -- which is the failure this
 * whole boundary exists to prevent. Routing the trampoline through the same dispatcher
 * means both ways in behave identically, including the stop policy and the ABI refusal.
 *
 * Does not return normally for an address with no implementation, no ABI or no module:
 * it stops the run exactly as the lookup path does.
 */
void xdk_thunk_dispatch_at(uint32_t address);

/**
 * Which HLE module owns an address.
 *
 * Two of the seven measured sections have no module, and that is recorded rather
 * than hidden: XNET (41/156) and
 * XMV (7/7) are measured boundaries with nothing behind them. A call to one of those
 * must stop with a message saying so, not fall through to whichever module happened
 * to be first in a chain of `if`s.
 */
typedef enum {
    /* Measured, but no HLE module exists for its section. */
    XDK_MODULE_NONE = 0,
    XDK_MODULE_D3D8,
    XDK_MODULE_DSOUND,
    XDK_MODULE_XINPUT,
    XDK_MODULE_XGRPH,
    XDK_MODULE_XONLINE,
    XDK_MODULE_XNET,
    XDK_MODULE_COUNT,
} xdk_module;

/** The human name of a module, for diagnostics. Never NULL. */
const char *xdk_module_name(xdk_module module);

/**
 * Which module owns a measured section name.
 *
 * The mapping from the generator's section string to a module lives here, in ONE
 * place, so that main.c can adopt the generated table by walking it rather than by
 * restating a mapping that would then drift. `NULL` and any unrecognised section
 * answer XDK_MODULE_NONE.
 */
xdk_module xdk_module_for_section(const char *section);

/**
 * How an XDK function takes its arguments and who cleans up after it.
 *
 * FOUR conventions, not one, and not a guess that there are four: `__stdcall` is the
 * XDK's default, the `Kf*`-style `__fastcall` appears in XDK libraries too, C++
 * member functions in D3D8 and DSOUND are `__thiscall` with `this` in ECX, and the
 * lifter's own per-function headers classify several D3D entries as cdecl -- which
 * matters because cdecl is CALLER cleanup, so the number of bytes we pop differs for
 * the same argument count.
 */
typedef enum {
    /* Arguments on the stack, callee pops them. */
    XDK_CC_STDCALL = 0,
    /* First argument in ecx, second in edx, remainder on the stack. The callee pops
     * only the stack remainder. */
    XDK_CC_FASTCALL,
    /* `this` in ecx, the rest on the stack, callee pops the stack arguments. The
     * same cleanup as stdcall; a separate value because a handler needs to know that
     * ecx is an object pointer and not argument zero. */
    XDK_CC_THISCALL,
    /* CALLER cleanup. We pop ONLY the return address however many arguments there
     * are, and popping them as well would eat the caller's own locals. */
    XDK_CC_CDECL,
} xdk_cc;

/** The human name of a convention, for diagnostics. Never NULL. */
const char *xdk_cc_name(xdk_cc cc);

/**
 * One row of the measured surface, injected at init.
 *
 * Deliberately a separate type from the generated `xdk_surface_entry`:
 * `src/xbox/xdk_surface.c` is derived from the user's own executable and is
 * gitignored, so a compile-time dependency on it would mean this module and its
 * suite could not build in a fresh clone. It carries NO ABI information, because the
 * measurement carries none -- site counts say how often a function is called, never
 * with how many arguments.
 */
typedef struct {
    uint32_t address;
    /* `.XTLID` name, or NULL where the image named nothing. BORROWED, not copied:
     * the generated table holds static string literals, so the caller's names must
     * outlive the module or at least the next xdk_thunk_shutdown. 94 of the 236 rows
     * are NULL on this image, which is the clean-room boundary working. */
    const char *name;
    xdk_module module;
} xdk_dispatch_entry;

/**
 * Adopt a measured surface. Copies `count` rows into heap storage.
 *
 * Returns false for a NULL table, a count of 0, a duplicate address, or an
 * allocation failure. A duplicate is refused rather than tolerated because the
 * lookup would then be order-dependent, and "which of the two rows won" is not a
 * question a dispatcher should have an answer to.
 *
 * Calling it again replaces the previous table without leaking, and resets every
 * declared ABI with it: an ABI is an assertion about a specific row, so carrying one
 * across a table swap would attach it to whatever now lives at that address.
 */
bool xdk_thunk_init(const xdk_dispatch_entry *table, size_t count);

/** Release the table. Idempotent, and leaves the module re-initialisable. */
void xdk_thunk_shutdown(void);

/** Rows in the adopted surface. 0 when uninitialised. */
size_t xdk_thunk_count(void);

/** True when `address` is in the adopted surface. EXACT match, never a neighbour. */
bool xdk_thunk_is_target(uint32_t address);

/** The module that owns `address`, or XDK_MODULE_NONE if it is not in the surface. */
xdk_module xdk_thunk_module_of(uint32_t address);

/** The `.XTLID` name for `address`, or NULL for an unnamed or absent row. */
const char *xdk_thunk_name_of(uint32_t address);

/* ===================== THE ABI, WHICH IS NOT GUESSED =====================
 *
 * `__stdcall` and `__thiscall` are callee-cleanup, so the dispatcher must pop exactly
 * what the caller pushed. Over-popping eats the caller's locals and `esp` never
 * recovers; under-popping leaves bytes behind and the damage surfaces arbitrarily far
 * from its cause. Neither failure crashes. Both produce a plausible, wrong trace.
 *
 * THERE IS THEREFORE NO DEFAULT. An address with no established ABI STOPS THE RUN
 * with a diagnostic naming it, which is a bug report somebody can act on in minutes.
 * Assuming zero arguments would be a silent wrong pop, which is not.
 *
 * THE TABLE IS EMPTY TODAY, AND THAT IS THE HONEST STATE. `tools/lift/callsites.py`
 * measures argument pushes per ORDINAL, through the thunk table; it has no notion of
 * an address target, so there is no measured arity for any of the 236 rows. Nothing
 * in the tree establishes one. Rather than ship 236 inferences, this module ships a
 * refusal and two ways to lift it:
 *
 *   - `xdk_thunk_declare_abi`, for an arity established BY HAND against the guest's
 *     own call sites, or known because somebody decompiled that function and is
 *     registering a real implementation for it. A hand verification is not a vote and
 *     gets no quorum test.
 *   - `xdk_thunk_declare_measured_abi`, for one that came from counting call sites.
 *     It applies the SAME two gates `stack_args_for()` applies to kernel ordinals,
 *     and for the same measured reasons: a NON-UNANIMOUS row is refused, and so is a
 *     row with fewer than XDK_MEASURED_ARITY_MIN_SITES voters, because "unanimous"
 *     over one site is a tautology -- one voter always agrees with itself.
 */

/**
 * Fewest call sites a measured arity needs before it is believed at all.
 *
 * THREE, matching `MEASURED_ARITY_MIN_SITES` in `kernel_thunk.c`, and for the reason
 * recorded there: three means a number has been agreed by at least two sites beyond
 * the first, which is corroboration rather than repetition. Two would admit a row
 * whose only two voters are register-indirect sites, which are structurally prone to
 * the same late-bracket undercount, so two correlated voters are closer to one than
 * to three. On the kernel side this gate caught three ordinals -- 46, 84 and 196 --
 * that a unanimity flag alone would have waved through, one of them on a single site
 * with twelve arguments.
 */
#define XDK_MEASURED_ARITY_MIN_SITES 3u

/**
 * Record an ABI established by hand for `address`.
 *
 * Returns false for an address absent from the adopted surface, for a convention
 * outside the enum, for `register_args` above 2, for `register_args` nonzero under a
 * convention that passes nothing in registers, or for `stack_args` above
 * XDK_ABI_MAX_STACK_ARGS. Each refusal is a disagreement between the caller and the
 * measurement, and accepting it silently would hide which of the two is wrong.
 */
bool xdk_thunk_declare_abi(uint32_t address, xdk_cc cc, uint32_t stack_args,
                           uint32_t register_args);

/**
 * Record an ABI that came from counting the guest's own call sites.
 *
 * Refuses, in addition to everything `xdk_thunk_declare_abi` refuses:
 *   - `unanimous` false. The sites disagreed, so the number is a minimum at best.
 *   - `sites` below XDK_MEASURED_ARITY_MIN_SITES. One or two voters are not a
 *     measurement, and the asymmetry settles it: refusing a row that was right costs
 *     a reported stop naming the address, while accepting one that was wrong costs a
 *     permanently desynced esp.
 *
 * A refusal here is not an error in the caller. It is the policy working, and the
 * address simply keeps stopping the run until somebody verifies it by hand.
 */
bool xdk_thunk_declare_measured_abi(uint32_t address, xdk_cc cc, uint32_t stack_args,
                                    uint32_t register_args, uint32_t sites,
                                    bool unanimous);

/**
 * Record an ABI read from the CALLEE's own terminating instruction.
 *
 * A third entry point rather than reusing either of the two above, because this is a
 * third class of evidence and conflating it with either would be a lie in one direction
 * or the other.
 *
 * NOT `xdk_thunk_declare_measured_abi`: that one gates on a SITE VOTE, and those gates
 * exist to defend against an estimator that takes a minimum across call sites. There is
 * no estimator here. `ret imm16` states the callee-cleanup byte count outright, read once
 * at the function itself, so a site count is not evidence about it at all. Applying the
 * quorum anyway would refuse 165 of 199 decisively-measured rows, 88 of them in sections
 * that have an HLE module.
 *
 * NOT `xdk_thunk_declare_abi` either, even though that path would accept every row: that
 * one means A HUMAN CHECKED THIS, and 199 machine-generated rows arriving through it
 * would make the distinction between hand-verified and tool-measured unrecoverable
 * exactly when somebody needs to know which they are looking at.
 *
 * `terminators` is how many distinct `ret` instructions the control-flow walk reached.
 * Refuses, in addition to everything `xdk_thunk_declare_abi` refuses:
 *   - `terminators` of 0. Nothing was read, so there is no measurement.
 *   - `unanimous` false. Two returns disagreeing means the walk crossed a function
 *     boundary, and a wrong extent is NOT biased in a knowable direction -- unlike a push
 *     count, which register saves can only inflate. Collapsing a conflict here would
 *     launder an extent bug into an arity, so it is refused rather than reduced.
 */
bool xdk_thunk_declare_callee_abi(uint32_t address, xdk_cc cc, uint32_t stack_args,
                                  uint32_t register_args, uint32_t terminators,
                                  bool unanimous);

/**
 * Largest stack-argument count accepted.
 *
 * Sixteen. The widest thing in either boundary measured so far is 12 dwords, from a
 * kernel ordinal, and the point of a bound is that a corrupt or mis-parsed row cannot
 * ask for a 4 GB pop. A row that genuinely needs more should raise this deliberately.
 */
#define XDK_ABI_MAX_STACK_ARGS 16u

/** True once an ABI has been established for `address`. */
bool xdk_thunk_abi_known(uint32_t address);

/**
 * How many bytes the dispatcher will pop for `address`, return address included.
 *
 * Returns false when no ABI is established, which is the condition that stops a run.
 * Exposed so a test can check the arithmetic directly rather than inferring it from
 * an `esp` delta, and so that a report can say what it would do.
 */
bool xdk_thunk_pop_bytes(uint32_t address, uint32_t *out);

/** How many rows have an established ABI. */
size_t xdk_thunk_abi_count(void);

/**
 * Which entry point a generated ABI row is declared through, because the class of
 * evidence decides the gate and the two gates are not interchangeable.
 */
typedef enum {
    /* `ret imm16` read at the callee. `count` is the number of `ret` instructions the
     * walk reached. Declared with `xdk_thunk_declare_callee_abi`: no site quorum. */
    XDK_ABI_FROM_CALLEE_RET = 0,
    /* A bare `ret` settled by the ABSENCE of caller cleanup. `count` is the number of
     * call sites that voted. Declared with `xdk_thunk_declare_measured_abi`, so the
     * three-voter quorum applies. */
    XDK_ABI_FROM_CALLER_VOTES,
} xdk_abi_evidence;

/** One row of the generated, gitignored `src/xbox/xdk_abi.inc` (tools/xdk_abi.py). */
typedef struct {
    uint32_t address;
    xdk_cc cc;
    uint32_t stack_args;
    uint32_t register_args;
    xdk_abi_evidence evidence;
    uint32_t count;
} xdk_abi_row;

/**
 * Declare every row through the entry point its evidence class belongs to, and return
 * how many were accepted. `*refused`, when non-NULL, receives how many were not, which
 * is also what a table that has drifted from the surface looks like: `declare` refuses
 * an address outside the adopted surface.
 *
 * Call AFTER `xdk_thunk_init`, which drops every declared ABI with the old surface.
 * Rows are all `unanimous` by construction: tools/xdk_abi.py refuses a conflicting row
 * rather than emitting it, so there is no conflict flag to carry.
 */
size_t xdk_thunk_declare_abi_rows(const xdk_abi_row *rows, size_t count, size_t *refused);

/**
 * `xdk_thunk_declare_abi_rows` over the table generated into `src/xbox/xdk_abi.inc`.
 * Declares nothing and returns 0 when that file did not exist at configure time, so a
 * fresh clone behaves exactly as before: every address stops for want of an ABI.
 */
size_t xdk_thunk_declare_generated_abis(size_t *refused);

/** How many rows the generated table holds, 0 when it was not generated. */
size_t xdk_thunk_generated_abi_count(void);

/* ===================== DISPATCH, COUNTS AND REPORTING ===================== */

/**
 * Stop the run at the first XDK address with no implementation.
 *
 * On by default, mirroring `kernel_thunk_set_stop_on_missing` exactly, and wired to
 * the same `--continue-on-missing`. A stub that silently returns 0 produces a
 * plausible, wrong trace that cannot be falsified from the inside, which is this
 * project's single worst failure mode -- so the default is a hard stop, and relaxing
 * it is a flag that announces itself.
 *
 * NOTE WHAT THIS DOES NOT DO. All three HLE modules already treat a measured but
 * unimplemented address as a STUB that reports once and returns a default. The
 * dispatcher does not rely on that: it checks the entry state itself and stops before
 * the module is ever asked. Under --continue-on-missing it falls through, and the
 * module's own stub reporting then applies.
 */
void xdk_thunk_set_stop_on_missing(bool stop);

/** Total XDK dispatches attempted, including refused ones. */
uint64_t xdk_thunk_call_count(void);

/**
 * How many calls this dispatcher routed INTO a given module.
 *
 * This is the assertion that the three `*_call` entry points have a production
 * caller. It counts only calls that actually reached `d3d8_hle_call`,
 * `dsound_hle_call` or `xinput_hle_call` from this file -- not dispatches that were
 * refused for want of an ABI, and not calls a test made directly -- so a nonzero
 * value cannot be produced by anything except the dispatcher doing its job.
 */
uint64_t xdk_thunk_module_call_count(xdk_module module);

/** How many dispatches were refused because no ABI was established. */
uint64_t xdk_thunk_abi_refused_count(void);

/** How many dispatches landed on an address absent from the adopted surface. */
uint64_t xdk_thunk_unknown_count(void);

/** How many dispatches landed on a measured address whose section has no module. */
uint64_t xdk_thunk_unrouted_count(void);

/** Clear every counter. The adopted table and declared ABIs are kept. */
void xdk_thunk_reset_counts(void);

/** A printf-style diagnostic sink. */
typedef int (*xdk_log_fn)(const char *format, ...);

/** Redirect diagnostics. Defaults to stderr; tests use this to capture output. */
void xdk_thunk_set_log(xdk_log_fn printer);

/** The current sink. Never NULL. */
xdk_log_fn xdk_thunk_log(void);

/**
 * Write what this boundary can and cannot reach.
 *
 * Per-section counts, how many rows have an ABI, how many dispatches happened and
 * where they went, and -- unconditionally, because it is the thing most likely to be
 * misread -- the inlined-SetRenderState caveat from the header comment. A report that
 * said "236 addresses routed" without saying "and render state is not among them"
 * would be the kind of plausible overstatement this project exists not to produce.
 */
void xdk_thunk_report(void);

#endif /* TSFP_HOST_XDK_THUNK_H */
