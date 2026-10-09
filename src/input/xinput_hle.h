/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * XAPI input HLE: the input boundary.
 *
 * ====================== WHY THIS IS A BOUNDARY AT ALL ======================
 *
 * Same shape as the audio boundary. XAPILIB.lib is statically linked into the title,
 * so XInput is not something the guest imports -- it is code inside the guest. There
 * is no import table to intercept, so the boundary is a set of ADDRESSES, and
 * replacing it means watching for control transfers from game `.text` into the XDK
 * code that was linked alongside it.
 *
 * ================= HOW THE SURFACE WAS FOUND, AND A CORRECTION =================
 *
 * The brief for this module expected XAPI to be linked into `.text` with no section of
 * its own, which would have made `tools/gen_d3d8_surface.py` useless here -- it works
 * by scanning game `.text` for `call rel32` / `jmp rel32` and bucketing the targets by
 * which NAMED section they land in, so a target inside `.text` itself is invisible to
 * it. That expectation is HALF RIGHT, and the half that is wrong is the useful half.
 *
 * XAPILIB really is split across two places, and the split falls exactly along the
 * line that matters. `.XTLID` resolves 111 XAPILIB symbols in this image. NINETY-FIVE
 * of them do sit in `.text` -- `ReadFile`, `CreateThread`, `HeapAlloc`, the `lstr*`
 * family, the save-game and content calls, the time conversions -- and those are
 * indeed invisible to the surface scan. But ALL SIXTEEN of the device and input
 * symbols sit in `XPP`, the Xbox Peripheral Port section, which is a real named
 * section with its own section header and which `gen_d3d8_surface.py` ALREADY scans.
 *
 * So the input surface needed no new tool. It needed the existing one pointed at the
 * right section name, and the answer is `XPP: 13 functions, 21 sites`. The section
 * bounds below are measured from the section header, exactly as DSOUND's are, so the
 * unknown-target diagnostic here is as sharp as the audio boundary's rather than the
 * weaker guess a sectionless surface would have forced.
 *
 * ========================= WHAT IS MEASURED =========================
 *
 * `.XTLID` is the strongest evidence available: it resolves XDK symbol START
 * addresses, so a name is not something a misdecoded byte can acquire. Every row in
 * the table in `xinput_hle.c` carries a name; not one is unnamed, which is the DSOUND
 * situation rather than the D3D one, where 74 of 85 rows have nothing but an address.
 *
 * THE SITE COUNTS HERE ARE EXACT, WHICH IS TRUE OF NO OTHER BOUNDARY IN THIS TREE.
 * That is a measured claim and not a hope. Both of the known failure modes were
 * checked for directly:
 *
 *   - OVER-COUNTING, which `d3d8_hle.h` warns about: a naive `E8` scan produces false
 *     positives wherever a misdecode lands. Here every row is an `.XTLID` symbol start
 *     and the generator's own entry-point test passes 236 of 236 rows image-wide with
 *     zero suspects, so a misdecode would have to land on a named symbol boundary to
 *     survive. It does not.
 *   - UNDER-COUNTING via `jmp [slot]` import thunks, which is the blind spot that cost
 *     this project a measurement once: `kernel_config.h` records
 *     `tools/lift/callsites.py` losing ALL TWELVE sites of ordinal 24
 *     (`ExQueryNonVolatileSetting`) because every one went through a stub whose body is
 *     a single `jmp dword ptr [slot]`, so the scanner saw the stub and attributed
 *     nothing. The same shape was expected here. IT DOES NOT APPLY, and the reason is
 *     structural: these are statically linked XDK function bodies, not kernel imports,
 *     so there is no thunk slot for them to hide behind.
 *
 *     That was not left as an argument. An exhaustive sweep of every section of the
 *     image, at EVERY byte alignment rather than only aligned dwords, found that the
 *     ONLY dwords anywhere in the executable holding any of the fifteen XPP code
 *     addresses are the fifteen `.XTLID` records themselves, at 0x0089CCDC..0x0089CD4C.
 *     Zero function pointers, zero vtable slots, zero thunk slots, zero jump-table
 *     entries. The generator's own attribution breakdown says the same thing from the
 *     other direction: of 831 attributed sites image-wide, `slot 0, table 0,
 *     register 0, thunk 0` -- all 831 are direct.
 *
 *     This also disposes of the 1,172 sites the generator sets aside as ambiguous
 *     indirect dispatch. None of them can possibly reach XPP, because reaching it
 *     indirectly would require its address to be STORED somewhere, and it is not
 *     stored anywhere.
 *
 * THE RESIDUAL UNCERTAINTY, STATED HONESTLY. The sweep proves no STORED pointer
 * exists. An address computed at runtime by arithmetic would evade it. No compiler
 * emits that for a static call, so this is a theoretical gap rather than a live one --
 * but it is the gap, and it is not nothing.
 *
 * WHAT THE LOW COUNTS ACTUALLY MEAN. `XInputGetState` has ONE call site and that is
 * not a scanning artefact. The title funnels all input through two thin wrappers in
 * game `.text`: the function at 0x0018FF70 holds both `XInputGetState` and
 * `XInputSetState`, and the one at 0x00190340 holds `XInputOpen`, `XInputClose`
 * twice, `XInputGetCapabilities` and `XGetDeviceChanges`. The XDK boundary is crossed
 * once per call per wrapper and all the fan-out lives above them, in game code this
 * boundary never sees. So a site count of 1 on `XInputGetState` means "called from one
 * place, every frame", and it ranks far below what it is worth. That is precisely the
 * row the runtime count in `xinput_hle_report()` exists to re-rank.
 *
 * ===================== MEASURED INPUT HANDLERS =====================
 *
 * xinput_devices.c registers five bounded enumeration handlers after this registry
 * is initialized. Initialization explicitly selects empty ports; device queries
 * reproduce the measured current/changed/previous table contracts. USB enumeration,
 * host input, device opening and state delivery remain separate work. Unsupported
 * addresses retain the reporting discipline below.
 *
 * The reporting discipline is therefore the substance, and it mirrors
 * `src/audio/dsound_hle.c` and `src/xbox/kernel_hle.c` exactly:
 *   - A STUB REPORTS ONCE, by `.XTLID` name where the image named it and by address
 *     where it did not. `XInputGetState` is polled once per frame per port; reporting
 *     per call would bury every one-shot enumeration call, and the enumeration order
 *     is the single thing this module exists to learn.
 *   - AN UNKNOWN TARGET REPORTS EVERY TIME. That is not unfinished work, it is our
 *     table disagreeing with the binary, which is a worse problem and must not be
 *     rate-limited into invisibility.
 *   - THE REPORT IS THE BACKLOG, busiest first. Whatever logs is what remains.
 *
 * ===================== NOTHING IS READ FROM HARDWARE =====================
 *
 * There is no gamepad in this container and no display to aim one at. This module does
 * not open a device, does not enumerate a USB bus, does not poll anything, and does
 * not read one byte from any host input API. It is READ-FROM-NOTHING, by construction
 * and not as a half-finished host backend: a host backend is out of scope and there is
 * no stub of one here to mistake for a broken one.
 *
 * Every port therefore reports EMPTY by default, and `xinput_hle_report()` says so in
 * as many words so that a reader of a run log cannot mistake "no device connected"
 * for a device that failed to open. A synthetic pad is available and is a FLAG that
 * ANNOUNCES ITSELF, in the same voice as `--stub-status`, `--mount`, the
 * `ExQueryNonVolatileSetting` zero-fill policy and DirectSound's codec readiness.
 */

#ifndef TSFP_INPUT_XINPUT_HLE_H
#define TSFP_INPUT_XINPUT_HLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Guest pointer width, kept distinct from a host pointer for the same reason
 * `dsound_hle.h` does it: a guest pointer is four bytes and storing a host pointer in
 * one truncates it. */
typedef uint32_t xinput_guest_ptr;

/* Bounds of the `XPP` section in the retail image, measured from its own section
 * header: vaddr 0x0046C680, vsize 0x000090EC. Used only to sharpen the diagnostic for
 * an unknown target: inside these bounds means our table is incomplete, outside them
 * means the caller dispatched something that is not a peripheral function at all. Two
 * different bugs, two messages. This is the same real-section test the audio boundary
 * gets, and the reason the input boundary gets it too is the correction above. */
#define XINPUT_SECTION_VA_BEGIN 0x0046C680u
#define XINPUT_SECTION_VA_END 0x0047576Cu

/**
 * How many functions the measured surface has.
 *
 * FIFTEEN, where `gen_d3d8_surface.py` reports thirteen for `XPP`, and the difference
 * is deliberate rather than a disagreement. The generator lists a function only if it
 * has at least one call site whose ORIGIN is `.text`, so two named `XPP` code symbols
 * are missing from it: `XReadMUMetaData`, whose one measured caller is in `XONLINE`
 * rather than game code, and `XVoiceCreateMediaObject`, which has no measured caller
 * at all. Both are real functions at real addresses in the boundary, so both are rows
 * here, carrying a site count of 0.
 *
 * A zero in `sites` therefore means "no call site measured from game `.text`". It does
 * NOT mean unreachable, and the two zero rows are the proof of the distinction: one of
 * them demonstrably is reached, just not from the origin the generator counts.
 *
 * This is also why `xinput_hle_crosscheck()` against an unmodified generated table
 * reports exactly two disagreements rather than none, which the suite asserts
 * directly. An expected divergence that is written down and tested is a cross-check;
 * one that is quietly tolerated is drift.
 */
#define XINPUT_FUNCTION_COUNT 15u

/**
 * How many call sites the measured surface has, summed over every row.
 *
 * Twenty-one, agreeing exactly with the generator's `XPP` total despite the two extra
 * rows, because both of those rows contribute zero.
 */
#define XINPUT_SITE_COUNT 21u

/**
 * An XAPI input function implementation.
 *
 * Arguments are not modelled, for the same reason `dsound_fn` and `kernel_fn` do not
 * model them: the call boundary comes from decompiled guest code which has its own
 * view of each signature, and inventing a uniform one here would be a guess to unpick
 * later. `context` is reserved for the per-call state that boundary will supply.
 */
typedef uint32_t (*xinput_fn)(void *context);

typedef enum {
    /* Not in the measured table: calling it means the table and the binary
     * disagree. Never a legitimate state for a measured row. */
    XINPUT_ENTRY_ABSENT = 0,
    /* A stub that reports itself once and returns its default. */
    XINPUT_ENTRY_STUB,
    /* A real implementation. There are currently none. */
    XINPUT_ENTRY_IMPLEMENTED,
} xinput_entry_state;

/**
 * What kind of peripheral a row serves.
 *
 * `XPP` is the peripheral port section, not an input section, so three different
 * subsystems share it. The classification is here so that a run log is readable
 * without the XDK documentation next to it: without it, a reader seeing
 * `XVoiceCreateMediaObjectEx` at the top of an input backlog would reasonably
 * conclude the table is wrong. It is a label on measured rows, not a guess about
 * behaviour.
 */
typedef enum {
    /* Gamepad and device enumeration. The input surface proper, 10 rows. */
    XINPUT_KIND_INPUT = 0,
    /* Memory units: removable storage that arrives through the same enumeration as a
     * gamepad, which is why it is in this section and not with the file layer. 3 rows. */
    XINPUT_KIND_MEMORY_UNIT,
    /* Voice, which shares the section and nothing else. 2 rows. */
    XINPUT_KIND_VOICE,
} xinput_fn_kind;

typedef struct {
    /* Guest VA of the call target. The table is keyed by this because the executable
     * carries no import entry for any of them. */
    uint32_t address;
    /* `.XTLID` name, or NULL if the image never named it. All 15 rows are named today;
     * the field is nullable because a table regenerated from another image may not be
     * so lucky, and a NULL that crashes the log is a bad way to find out. */
    const char *name;
    /* Measured call sites whose origin is game `.text`. EXACT for this surface -- see
     * the header's note on why, which is specific to XPP and does not generalise. Zero
     * means "none measured from game code", not "unreachable". */
    uint32_t sites;
    xinput_fn_kind kind;
    xinput_fn handler;
    xinput_entry_state state;
    uint32_t default_return;
    /* Times called this run. This is the observed ranking, which supersedes `sites`
     * the moment it is nonzero -- and for this surface it matters more than usual,
     * because the title funnels input through two wrappers and the static counts
     * understate the hottest functions by design rather than by error. */
    uint64_t call_count;
    bool reported;
} xinput_entry;

/** The human name of a kind, for diagnostics. Never NULL. */
const char *xinput_hle_kind_name(xinput_fn_kind kind);

/**
 * How a row identifies itself in a log: its `.XTLID` name, or its ADDRESS when the
 * image never named it.
 *
 * Public rather than internal so the suite can test both branches. All 15 rows in this
 * image are named, so the unnamed branch is unreachable through the compiled-in table
 * and would otherwise be untested code in the one layer that must never crash -- and
 * "reports by address otherwise" is a requirement, not an implementation detail.
 *
 * `buffer` is used only for the unnamed case; the named case returns the name itself
 * and does not touch it. Never returns NULL, including for a NULL entry or a NULL
 * buffer, because a diagnostic path that can hand a NULL to `printf("%s")` is a crash
 * in the exact place that exists to explain crashes.
 */
const char *xinput_hle_entry_label(const xinput_entry *entry, char *buffer, size_t len);

/* ===========================================================================
 * THE DATA SYMBOLS, WHICH ARE NOT CALL TARGETS.
 *
 * `.XTLID` names six symbols in `XPP` that are DATA, not code: the device-type
 * descriptor tables the title passes BY ADDRESS to the enumeration and open calls.
 * They are measured facts about the same section and they are recorded here for one
 * concrete diagnostic reason.
 *
 * If something ever dispatches to one of these addresses, the useful message is not
 * "unknown target". It is "that is a data table, not a function" -- which points
 * straight at an argument having been passed where a call target was expected, a
 * mistake the lifted code is entirely capable of making. Without this list that bug
 * presents as a mysterious unknown address inside the section.
 *
 * WHAT THE IMMEDIATES SHOW, AND IT IS WORTH KNOWING. These tables are referenced from
 * game `.text` as plain immediates, which is directly visible:
 *   `XDEVICE_TYPE_GAMEPAD_TABLE` at 0x0018FEE9, 0x00190351 and 0x001903CF -- the last
 *   of these is five bytes before the `XInputOpen` call at 0x001903D4, so the title
 *   demonstrably opens GAMEPAD devices.
 *   `XDEVICE_TYPE_MEMORY_UNIT_TABLE` at 0x000258C5, 0x000259D4, 0x0018FEC1, 0x003B568F
 *   and 0x003B56A7.
 *   `XDEVICE_TYPE_IR_REMOTE_TABLE` is LINKED BUT NEVER REFERENCED FROM `.text`. The
 *   title has no IR remote support, so the DVD-remote path is not something this
 *   boundary will ever be asked for, and that is measured rather than assumed.
 * =========================================================================== */

/** Named DATA symbols in the XPP section. */
#define XINPUT_DATA_SYMBOL_COUNT 6u

typedef struct {
    uint32_t address;
    const char *name;
} xinput_data_symbol;

/** The measured data symbols, for callers that want to walk them. */
const xinput_data_symbol *xinput_hle_data_symbols(size_t *out_count);

/** The data symbol at EXACT `address`, or NULL. */
const xinput_data_symbol *xinput_hle_data_symbol(uint32_t address);

/** Reset every entry to an unreported stub, and reset all device and layout state. */
void xinput_hle_init(void);

/**
 * Register a real implementation for a measured address.
 *
 * False for an address not in the table, because registering against one would mean
 * either the table or the caller is wrong and accepting it silently hides which.
 */
bool xinput_hle_register(uint32_t address, xinput_fn handler);

/** Set what a stub at `address` returns. Defaults to 0. False for an unknown address. */
bool xinput_hle_set_default_return(uint32_t address, uint32_t value);

/** Look up an entry by EXACT address, or NULL. Never returns a neighbour. */
const xinput_entry *xinput_hle_entry(uint32_t address);

/** Look up an entry by `.XTLID` name, or NULL. Unnamed rows are never matched. */
const xinput_entry *xinput_hle_entry_by_name(const char *name);

/** The measured table in its declared order, for callers that want to walk it. */
const xinput_entry *xinput_hle_table(size_t *out_count);

/**
 * Invoke the XAPI input function at `address`.
 *
 * A stub reports itself once, by name where known and by address otherwise, then
 * returns its default. An address absent from the table reports EVERY time, because
 * it means our surface and the binary disagree and that must not be rate-limited
 * away.
 */
uint32_t xinput_hle_call(uint32_t address, void *context);

/** How many measured functions have real implementations. */
size_t xinput_hle_implemented_count(void);

/** How many distinct functions have been called at least once this run. */
size_t xinput_hle_touched_count(void);

/** How many calls landed on an address absent from the table. */
uint64_t xinput_hle_unknown_call_count(void);

/**
 * Write the backlog, busiest first.
 *
 * ORDERING, BECAUSE IT IS THE WHOLE POINT OF THE REPORT. Primary key is runtime
 * `call_count` descending; ties break on measured `sites` descending; remaining ties
 * break on address ascending so the output is deterministic. Runtime observation
 * dominates a static count because it is a fact about this run rather than an
 * estimate over the whole image -- so before the guest has called anything the report
 * is exactly the static ranking, and after a trace it is the observed one, with the
 * static count still ordering everything the trace never reached.
 */
void xinput_hle_report(void);

/* ===========================================================================
 * PORTS AND DEVICES: THE DEFAULT IS NOTHING CONNECTED.
 *
 * The console has four controller ports. That is a fact about the physical hardware,
 * observable from the front of the machine, and is the one thing in this section that
 * is not in question.
 *
 * WHAT IS IN QUESTION IS EVERYTHING ELSE, so the default is EMPTY on all four. We
 * have no gamepad, no USB host controller, and no measurement of what this title does
 * when enumeration returns nothing. Reporting a device would fabricate the first bit
 * the title reads, and this codebase's standing rule -- set by `--stub-status`,
 * `--mount` and the `ExQueryNonVolatileSetting` zero-fill policy -- is that every
 * fabrication is a flag and announces itself.
 *
 * EMPTY IS ONLY HONEST IF IT IS NOT ALSO A MYSTERY, which is the lesson
 * `dsound_hle.h` draws from a codec that defaults to not-ready. The plausible cost of
 * an empty port is a title that boots to a menu nothing can dismiss, or one that
 * waits forever on a "please reconnect the controller" modal, so the first time the
 * guest asks about a port and the answer is EMPTY, this module says so, names those
 * symptoms, and names the switch that gets past them. A signposted dead end beats an
 * unexplained one.
 * =========================================================================== */

/** Controller ports on the physical console. */
#define XINPUT_PORT_COUNT 4u

typedef enum {
    /* The honest default. Nothing is connected because nothing is read. */
    XINPUT_PORT_EMPTY = 0,
    /* FABRICATED. A pad that does not exist, announced when attached. There is no
     * host backend behind this: it reports presence and nothing else. */
    XINPUT_PORT_SYNTHETIC,
} xinput_port_state;

/**
 * Attach the synthetic pad to `port`, announcing the fabrication once.
 *
 * False for a port index outside 0..XINPUT_PORT_COUNT-1. Attaching prints a
 * FABRICATED banner naming what is being faked, because presence changes which branch
 * the title's whole input init takes and a run log that does not record it cannot be
 * interpreted.
 *
 * NOTE WHAT THIS DOES NOT DO. It does not synthesise button or axis values. Presence is
 * one bit; everything else stays at rest unless something explicitly fabricates it
 * through `xinput_hle_set_synthetic_pad_state`, which announces itself separately
 * because it is a separate and larger lie.
 */
bool xinput_hle_attach_synthetic_pad(unsigned port);

/** Remove the synthetic pad from `port`. False for an out-of-range port. */
bool xinput_hle_detach_synthetic_pad(unsigned port);

/** The state of `port`, or XINPUT_PORT_EMPTY for an out-of-range index. */
xinput_port_state xinput_hle_port_state(unsigned port);

/**
 * Ask whether `port` has a device, as the guest-facing enumeration path would.
 *
 * The first EMPTY answer reports the symptoms to expect and the switch that avoids
 * them. Later EMPTY answers are silent -- this is polled once per frame per port.
 */
bool xinput_hle_port_connected(unsigned port);

/** How many ports currently report a device. Zero by default, always. */
unsigned xinput_hle_connected_count(void);

/** How many connection queries were answered EMPTY. */
uint64_t xinput_hle_empty_query_count(void);

/* ===========================================================================
 * THE GUEST STATE LAYOUT: DERIVED FROM THIS IMAGE, AND STILL NOT THE DEFAULT.
 *
 * `docs/guest-structs.md` does not document `XINPUT_STATE` or `XINPUT_GAMEPAD`. It does
 * document the METHOD -- offsets from direct accesses at call sites, widths from the
 * access widths, corroborated across independent sites -- and that method was applied
 * here rather than skipped. The layout below is MEASURED from the retail image. No
 * external header was consulted, and the one place where a remembered desktop layout
 * would have misled is called out explicitly below.
 *
 * WHY THAT CAUTION IS NOT THEATRE. A lifter patch found upstream declared
 * `IO_STATUS_BLOCK` at sixteen bytes where the guest's is eight, and wrote eight bytes
 * past the end of every one it touched. A layout from the nearest familiar-looking
 * source is a memory corruption with a plausible name on it.
 *
 * ===================== THE DERIVED LAYOUT =====================
 *
 * Buffer identified from the one game-side consumer: `XInputGetState` is called at
 * `.text:0x0018FFB9` from the wrapper opening `sub esp, 0x18` at 0x0018FF70, and the
 * buffer is handed over by `lea ecx, [esp+0x10]` at 0x0018FFA5. Every offset below is
 * that base plus the displacement actually encoded in the instruction.
 *
 * | Offset | Width | Name              | Independent support                  | Basis |
 * |--------|-------|-------------------|--------------------------------------|-------|
 * | `0x00` | 4     | packet number     | XPP writes 0x0046E3A5, game reads 0x00190303 -- 2 sections | Measured |
 * | `0x04` | 2     | digital buttons   | `movzx ecx, word ptr [esp+0x14]` 0x0018FFE2 | Measured (see below) |
 * | `0x06` | 8     | analog run        | 8-entry table walk 0x00190010..0x0019002B, plus 2 extra accesses | Measured |
 * | `0x0E` | 2     | thumb left X      | `movsx ecx, word ptr [esp+0x1e]` 0x00190034 | Measured |
 * | `0x10` | 2     | thumb left Y      | `movsx` 0x00190094                   | Measured |
 * | `0x12` | 2     | thumb right X     | `movsx` 0x001900F6                   | Measured |
 * | `0x14` | 2     | thumb right Y     | `movsx` 0x00190158                   | Measured |
 * | —      | —     | **fields 22**     | last field ends at 0x16; frame is 0x18 | Measured |
 *
 * The thumb axes are read with `movsx`, not `movzx`, so they are SIGNED. That is a
 * measured fact about the access, not an assumption about what a stick reports.
 *
 * THE SINGLE MOST IMPORTANT ROW IS THE ANALOG RUN, AND IT IS WHERE RECOLLECTION WOULD
 * HAVE LOST. The run is EIGHT bytes, not six. The loop at 0x00190010 walks a table of
 * 8 entries of 8 bytes at `.rdata:0x0047DF40`, bounded by `cmp eax, 0x40`, whose first
 * dwords are exactly 0..7 and which is followed immediately by an unrelated ASCII
 * string -- so the run is eight, read out of the image rather than reasoned about. Six
 * of the eight are only ever thresholded against the immediate 0x3C to synthesise a
 * digital bit; entries SIX and SEVEN get a second, analog rescaling path
 * (0x001901C4 and 0x0019021D) alongside the four stick axes, which is what identifies
 * them as the two TRIGGERS sharing the array with the six pressure-sensitive face
 * buttons.
 *
 * So the desktop-shaped model -- six analog buttons and two separate trigger fields --
 * is wrong about this structure, and it is wrong in a way that would have silently
 * mapped the triggers to the wrong two bytes. This is the `IO_STATUS_BLOCK` trap in its
 * natural habitat, and the only thing that avoided it was reading the table.
 *
 * ===================== WHAT IS STILL NOT DERIVABLE =====================
 *
 *   - WHICH PHYSICAL BUTTON each of the six face bytes is. Offsets and the 6+2 split
 *     are measured; the identities are not. The title's bit masks are internal to its
 *     own input layer and name nothing.
 *   - THE TWO TRAILING BYTES at `0x16` and `0x17`. Measured as untouched and inside the
 *     frame. Whether they are padding or an unused field is not decidable here, so the
 *     declared size below is 22 and NOT 24: the module will not write into bytes whose
 *     nature is undecided, even bytes it is confident are slack.
 *   - WHETHER THE DIGITAL FIELD IS REALLY 16 BITS WIDE. The access width is 2, which is
 *     measured. But only bits 0..7 are ever exercised -- the mask table at 0x0047DF00
 *     holds 0x01..0x80 only -- so a 1-byte field plus an unread neighbour would be
 *     indistinguishable from this image. The width is taken from the access, which is
 *     the documented method, and the ambiguity is recorded rather than resolved.
 *
 * ================= WHY IT IS STILL NOT THE DEFAULT =================
 *
 * Because this module cannot check it. The derivation is strong -- three mutually
 * constraining structures, three array strides with zero slack, and a self-declaring
 * 25-byte `rep stosd`/`stosb` -- but the module's job is to write bytes into guest
 * memory, and the cost of being wrong is silent corruption rather than a failed test.
 * So the measured layout is COMPILED IN and available in one call, and the DEFAULT is
 * still unset, with every write refused until something adopts it. Adoption is a
 * decision a run records, in the same way `--ac97-ready` is.
 *
 * `xinput_hle_adopt_measured_layout()` installs the table above and announces it.
 * `xinput_hle_set_state_size()` and `xinput_hle_map_field()` remain, because a
 * re-derivation on another image must not have to edit this file to be testable.
 * =========================================================================== */

/** Sentinel for "no offset has been derived for this field". */
#define XINPUT_OFFSET_UNSET UINT32_MAX

/** Sentinel for "the guest state structure's size has not been derived". */
#define XINPUT_STATE_SIZE_UNSET 0u

/**
 * A field of the guest state structure.
 *
 * SEVEN, matching the seven measured fields exactly. There is deliberately no separate
 * LEFT_TRIGGER or RIGHT_TRIGGER: the measurement says the triggers are entries six and
 * seven of the eight-byte analog run at `0x06`, not fields of their own, and a field
 * list that disagreed with the bytes would be the whole bug this module exists to
 * avoid.
 */
typedef enum {
    /* A change counter the guest compares between polls to spot a new sample. Written
     * by XPP at 0x0046E3A5 and read by the game at 0x00190303. */
    XINPUT_FIELD_PACKET_NUMBER = 0,
    /* The digital set, as a bitmap. Read two bytes wide at 0x0018FFE2. */
    XINPUT_FIELD_DIGITAL_BUTTONS,
    /* The eight-byte analog run: six pressure-sensitive face buttons followed by the
     * two analog triggers. One field because the bytes are one contiguous array walked
     * by one loop, which is how the image presents it. */
    XINPUT_FIELD_ANALOG_RUN,
    /* Signed, per the `movsx` at every one of the four sites. */
    XINPUT_FIELD_THUMB_LEFT_X,
    XINPUT_FIELD_THUMB_LEFT_Y,
    XINPUT_FIELD_THUMB_RIGHT_X,
    XINPUT_FIELD_THUMB_RIGHT_Y,
    XINPUT_FIELD_COUNT,
} xinput_field;

/* The measured placements, available for adoption rather than applied by default. */
#define XINPUT_MEASURED_OFFSET_PACKET_NUMBER 0x00u
#define XINPUT_MEASURED_OFFSET_DIGITAL_BUTTONS 0x04u
#define XINPUT_MEASURED_OFFSET_ANALOG_RUN 0x06u
#define XINPUT_MEASURED_OFFSET_THUMB_LEFT_X 0x0Eu
#define XINPUT_MEASURED_OFFSET_THUMB_LEFT_Y 0x10u
#define XINPUT_MEASURED_OFFSET_THUMB_RIGHT_X 0x12u
#define XINPUT_MEASURED_OFFSET_THUMB_RIGHT_Y 0x14u

/**
 * The declared size the measured layout adopts: 22, the end of the last field.
 *
 * NOT 24, which is the frame the title allocates at 0x0018FF70. The two bytes at 0x16
 * and 0x17 are measured as untouched, and whether they are padding or an unused field
 * is not decidable from this image -- so they are outside the bound and the module
 * cannot write into them. A size of 24 would be a slightly better guess and still a
 * guess.
 */
#define XINPUT_MEASURED_STATE_SIZE 22u

/** The frame the title actually allocates for the structure, for the record. */
#define XINPUT_MEASURED_STATE_FRAME 24u

/**
 * Adopt the compiled-in measured layout, announcing it once.
 *
 * Replaces any existing size and mappings wholesale. Returns false only if the
 * compiled-in table somehow fails its own validation, which would be a bug in this
 * file rather than a caller error -- and is checked rather than assumed, because a
 * measured table that overruns its own declared size is exactly the failure being
 * guarded against and it would be absurd to let this module ship it unchecked.
 */
bool xinput_hle_adopt_measured_layout(void);

/** True once the measured layout has been adopted (as opposed to a hand-mapped one). */
bool xinput_hle_measured_layout_adopted(void);

/** A field's derived placement in the guest structure. */
typedef struct {
    uint32_t offset; /* XINPUT_OFFSET_UNSET until derived. */
    uint32_t width;  /* Bytes. 0 until derived. */
    bool mapped;
} xinput_field_placement;

/** The human name of a field, for diagnostics. NULL for an out-of-range value. */
const char *xinput_hle_field_name(xinput_field field);

/**
 * Declare the derived size of the guest state structure.
 *
 * This is the bound every field is checked against, so it is required before any
 * field can be mapped. `size` of XINPUT_STATE_SIZE_UNSET clears it, and clears every
 * field with it -- a field validated against a size that no longer applies is exactly
 * the `IO_STATUS_BLOCK` overrun with an extra step.
 */
void xinput_hle_set_state_size(uint32_t size);

/** The derived size, or XINPUT_STATE_SIZE_UNSET. */
uint32_t xinput_hle_state_size(void);

/**
 * Supply a field's derived offset and width.
 *
 * Refuses, and says why, for any of:
 *   - no state size declared yet, so there is nothing to bound the field against;
 *   - `offset + width` past the declared size, or a sum that wraps -- this is the
 *     `IO_STATUS_BLOCK` failure exactly, and it is rejected rather than clamped;
 *   - a width of 0, or one that is not 1, 2 or 4 bytes for a scalar field;
 *   - an overlap with an already-mapped field. Two fields claiming the same bytes
 *     means at least one derivation is wrong, and accepting both would write one over
 *     the other in an order nobody chose.
 *
 * `XINPUT_FIELD_ANALOG_BUTTONS` is the one field permitted a width that is not a
 * scalar size, because it is a run of per-button pressures rather than one number.
 */
bool xinput_hle_map_field(xinput_field field, uint32_t offset, uint32_t width);

/** A field's placement. Unmapped fields report offset UNSET and width 0. */
xinput_field_placement xinput_hle_field(xinput_field field);

/** How many fields have derived placements. Zero by default. */
unsigned xinput_hle_mapped_field_count(void);

/**
 * True when enough has been derived to write anything at all: a state size and at
 * least one mapped field.
 *
 * Deliberately NOT "all nine fields mapped". A partial layout derived from real call
 * sites is worth using for the fields it covers, and demanding completeness would
 * mean the first eight derivations buy nothing. What must never happen is writing a
 * field whose offset was assumed, and an unmapped field is simply not written.
 */
bool xinput_hle_layout_usable(void);

/* ===========================================================================
 * HOST-SIDE CONTROLLER STATE.
 *
 * Held in host types at host widths, because this is where the state lives when no
 * guest layout has been derived -- which is the current and default condition. Values
 * here are all zero unless something sets them, and nothing in this module sets them
 * from hardware.
 *
 * The widths below are HOST choices and are not claims about the guest. Signed 16-bit
 * sticks and unsigned 8-bit pressures are what the hardware's own resolution implies,
 * and the guest's widths come from `xinput_hle_map_field`, where they are measured.
 * =========================================================================== */

/**
 * Entries in the analog run: six pressure-sensitive face buttons, then two triggers.
 *
 * EIGHT, from the image, not six. The host array mirrors the guest run entry for entry
 * so that marshalling is a copy rather than a rearrangement -- a host struct with
 * separate trigger members would have to decide where they go in the run, and that
 * decision is precisely the thing the measurement already made.
 */
#define XINPUT_ANALOG_COUNT 8u

/** Index of the left trigger within the analog run. Measured: entry 6, at guest 0x0C. */
#define XINPUT_ANALOG_LEFT_TRIGGER 6u

/** Index of the right trigger within the analog run. Measured: entry 7, at guest 0x0D. */
#define XINPUT_ANALOG_RIGHT_TRIGGER 7u

typedef struct {
    uint32_t packet_number;
    uint16_t digital_buttons;
    /* Entries 0..5 are the face buttons, 6 and 7 the triggers. */
    uint8_t analog[XINPUT_ANALOG_COUNT];
    /* Signed, matching the `movsx` at all four measured read sites. */
    int16_t thumb_left_x;
    int16_t thumb_left_y;
    int16_t thumb_right_x;
    int16_t thumb_right_y;
} xinput_pad_state;

/** The host-side state for `port`, all zero by default. Zeroed for a bad port. */
xinput_pad_state xinput_hle_pad_state(unsigned port);

/**
 * Overwrite `port`'s host-side state with FABRICATED values, announcing it once.
 *
 * Requires a synthetic pad on `port`: fabricating button values for a port that
 * reports no device is incoherent, and returning false is how that mistake surfaces
 * instead of producing input from a controller the guest was told is absent.
 *
 * THIS IS THE LARGER OF THE TWO LIES THIS MODULE WILL TELL, and it is separated from
 * `xinput_hle_attach_synthetic_pad` for that reason. Presence is one bit. A stick
 * deflection is a value the guest will act on, and a run log that does not distinguish
 * "a pad is present but at rest" from "something is pressing buttons" cannot be used to
 * judge anything. Both announce; they announce differently.
 *
 * Nothing in this module ever calls this. It exists so a synthetic input source can be
 * driven from outside, under a flag, and so that the marshalling path can be tested
 * against values that are not all zero -- which is the only way its byte order and its
 * truncation are observable at all.
 */
bool xinput_hle_set_synthetic_pad_state(unsigned port, xinput_pad_state state);

/** T731: how many raw report changes (values differing from the previous ones, packet number ignored)
 * `xinput_hle_set_synthetic_pad_state` installed on `port`. The synthetic pad's packet number is 1 plus this. */
uint32_t xinput_hle_synthetic_report_changes(unsigned port);

/** How many times fabricated values were installed. Zero in an honest run. */
uint64_t xinput_hle_synthetic_value_count(void);

/**
 * How a byte reaches guest memory.
 *
 * A callback rather than a direct `guest_mem` call so this module links standalone:
 * the test suite points it at a local buffer and needs no guest address space, and
 * the host points it at the real one. It also means a write can be observed, which is
 * how the suite proves the un-derived case writes nothing at all.
 */
typedef void (*xinput_write_fn)(uint32_t guest_address, const void *bytes, uint32_t len,
                                void *user);

/** Install the writer guest state goes through. NULL disables writing. */
void xinput_hle_set_writer(xinput_write_fn writer, void *user);

/**
 * Marshal `port`'s host-side state into the guest structure at `guest_address`.
 *
 * Writes ONLY the fields with derived placements, each at its derived offset and
 * truncated to its derived width. An unmapped field is skipped, not guessed.
 *
 * Returns the number of fields written, and 0 having written NOTHING when the layout
 * is unusable or no writer is installed. The first refusal reports once, names the
 * `IO_STATUS_BLOCK` precedent, and says what has to be measured to get past it.
 */
unsigned xinput_hle_write_guest_state(unsigned port, uint32_t guest_address);

/** How many marshalling attempts were refused for want of a derived layout. */
uint64_t xinput_hle_write_refused_count(void);

/* ===================== CROSS-CHECK AGAINST THE GENERATOR ===================== */

/**
 * One row of an externally supplied surface table.
 *
 * Structurally identical to a row of the compiled-in table, and deliberately a
 * separate type: anything generated from the user's own executable is gitignored, so
 * a compile-time dependency on it would mean this module and its tests could not
 * build in a fresh clone.
 */
typedef struct {
    uint32_t address;
    const char *name;
    uint32_t sites;
} xinput_surface_ref;

/**
 * Diff an externally supplied input surface against the table compiled in here.
 *
 * Returns the number of disagreements and logs each one: a row we do not have, a row
 * we have that is missing from `refs`, a differing site count, a differing name. The
 * measured surface is duplicated between whatever derived it and this file, and the
 * failure mode of duplicated measured data is SILENT DRIFT -- so there is a function
 * whose whole job is to make the drift loud. Zero means the two agree exactly.
 */
unsigned xinput_hle_crosscheck(const xinput_surface_ref *refs, size_t count);

/* ===================== DIAGNOSTIC SINK ===================== */

/** A printf-style diagnostic sink. */
typedef int (*xinput_log_fn)(const char *format, ...);

/**
 * Redirect diagnostics. Defaults to stderr; tests use this to capture output.
 *
 * A sink of its own rather than a link against `kernel_hle_log()`, so that this
 * module builds and tests without the kernel layer. The host should point it AT
 * `kernel_hle_log()` during start-up, because two separate sinks means a reader who
 * captures one still misses the other, and a diagnostic nobody reads is the exact
 * failure this reporting discipline exists to prevent.
 */
void xinput_hle_set_log(xinput_log_fn printer);

/** The current sink. Never NULL. */
xinput_log_fn xinput_hle_log(void);

#endif /* TSFP_INPUT_XINPUT_HLE_H */
