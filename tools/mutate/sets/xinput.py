# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for `src/input/`, the XAPI input boundary (facade and EMPTY adapter).

T351. The commit that landed this module records "4 of 4 mutations killed", but those four
were the kernel_io volume-geometry mutants that the harness's first run found, not input
mutants: no mutation set has ever covered `src/input/xinput_hle.c` (1,064 lines) or
`src/input/xinput_devices.c` (207 lines). This set closes the largest zero-coverage module
gap measured by the T351 inventory (audio was larger but is workspace-2c's active T156/T147
lane).

What these mutations are chosen to catch, by family:

    table       the measured surface rows (15 functions, 21 sites, kinds, the two
                zero-site rows) and the data symbols. The failure mode is the one the
                module's own comments warn about: a row one byte or one site off still
                produces a plausible table.
    dispatch    exact-match lookup (XInitDevices and XGetDevices are FIVE bytes apart),
                registration, the once-per-function stub report versus the every-time
                unknown-target report, and the three distinct unknown-target messages.
    ports       the honest EMPTY default, synthetic-pad presence, the refusal of values
                for an empty port, and the empty-query accounting.
    layout      the refuse-don't-guess state marshalling: size/width/bound/overlap
                validation (the IO_STATUS_BLOCK lesson), adoption of the measured
                layout through its own validation, little-endian field writes, and the
                field-order trap the kernel_io survivor taught (offset/width swapped).
    report      the ranking that makes the report a work queue (runtime calls before
                measured sites before address).
    crosscheck  both directions of the drift guard against the generated table.
    devices     the EMPTY adapter's preflight refusals (ready/empty/declaration
                checks, output alias and overlap recovery) and the enumeration mask
                semantics of get/peek/changes, which the original bytes pin via
                `tests/test_xinput_devices_oracle.py` but which also need a
                plain-build kill.

Kill suites are the two plain-build ctest binaries `test_xinput_hle` and
`test_xinput_devices`: no lifted tree, no XBE, no device. The XBE-gated oracle pytest
would also kill the mask-semantics mutants, but only on a machine with the user's
`default.xbe`, so it is deliberately NOT a target here.

EQUIVALENT MUTANTS CONSIDERED AND LEFT OUT, so nobody re-adds them:
  - `scalar_value` dropping the `(uint16_t)` narrowing on a thumb axis: every adopted
    and hand-derived mapping of a thumb field is 2 bytes wide, and a 2-byte write
    truncates to the same bytes with or without the narrowing. Observable only through
    a 4-byte thumb mapping, which nothing measured justifies.
  - `xinput_devices.c` `announced = false;` after the banner: re-announcing needs a
    second successful `init_empty` in one ready cycle, and `ready` refuses exactly
    that; `xinput_devices_reset` clears `announced` anyway. Unobservable.
  - `table_at` mapping 4 instead of 12 bytes: the six type tables are fixed addresses
    in the middle of one guest page, and `map_fixed` maps whole pages, so no test can
    make bytes 4..11 of a table unreadable while byte 0 is readable.
  - `adopt` dropping the size rollback after a failed measured field: the compiled-in
    table passes its own validation in this image, so the failure arm is unreachable
    from any test that does not first corrupt the table.

Every `old` here is checked to occur exactly once by `tests/test_mutation_anchors.py`.
"""

HLE = "src/input/xinput_hle.c"
DEV = "src/input/xinput_devices.c"
HLE_SUITE = ["test_xinput_hle"]
DEV_SUITE = ["test_xinput_devices"]
BOTH = ["test_xinput_hle", "test_xinput_devices"]


def _hle(mutation_id: str, old: str, new: str, why: str, targets: list[str] | None = None) -> dict:
    return {
        "id": f"xin-{mutation_id}",
        "file": HLE,
        "old": old,
        "new": new,
        "targets": list(targets or HLE_SUITE),
        "why": why,
    }


def _dev(mutation_id: str, old: str, new: str, why: str, targets: list[str] | None = None) -> dict:
    return {
        "id": f"xin-dev-{mutation_id}",
        "file": DEV,
        "old": old,
        "new": new,
        "targets": list(targets or DEV_SUITE),
        "why": why,
    }


MUTATIONS: list[dict] = [
    # ------------------------------------------------------------------ the table
    _hle(
        "tbl-getdevices-sites",
        '    {0x0046dbd2, "XGetDevices", 4, XINPUT_KIND_INPUT},',
        '    {0x0046dbd2, "XGetDevices", 3, XINPUT_KIND_INPUT},',
        "the site counts are the measurement and uniquely exact for this boundary; a "
        "row one site off still reads as a plausible table, and the backlog it feeds "
        "would rank XGetDevices below XGetDeviceChanges. The 21-site total must be "
        "summed from the rows, not restated from the macro.",
    ),
    _hle(
        "tbl-voice-misclassified",
        '    {0x004754fd, "XVoiceCreateMediaObjectEx", 2, XINPUT_KIND_VOICE},',
        '    {0x004754fd, "XVoiceCreateMediaObjectEx", 2, XINPUT_KIND_INPUT},',
        "the VOICE label exists so nobody spends a day on a microphone function "
        "thinking it is a controller function; a misclassified row defeats exactly "
        "that and nothing else in the table changes.",
    ),
    _hle(
        "tbl-initdevices-address-off-by-one",
        '    {0x0046dbcd, "XInitDevices", 1, XINPUT_KIND_INPUT},',
        '    {0x0046dbce, "XInitDevices", 1, XINPUT_KIND_INPUT},',
        "XInitDevices at 0x0046DBCD and XGetDevices at 0x0046DBD2 are five bytes "
        "apart; an address one byte off makes the real call site an unknown target "
        "and dispatches one-time initialisation as nothing at all.",
    ),
    _hle(
        "tbl-zero-site-row-invented",
        '    {0x0046da04, "XReadMUMetaData", 0, XINPUT_KIND_MEMORY_UNIT},',
        '    {0x0046da04, "XReadMUMetaData", 1, XINPUT_KIND_MEMORY_UNIT},',
        "the zero records that the generator counts .text origins only (the one real "
        "caller is in XONLINE); inventing a site both breaks the 21 total and "
        "promotes a function the title's .text never calls into the backlog.",
    ),
    _hle(
        "data-gamepad-table-address",
        '    {0x0046c75c, "XDEVICE_TYPE_GAMEPAD_TABLE"},',
        '    {0x0046c75d, "XDEVICE_TYPE_GAMEPAD_TABLE"},',
        "the data symbols exist to turn a call to a descriptor-table address into the "
        "most specific of the three unknown-target diagnoses; one byte off and the "
        "gamepad table call is reported as a generic in-section unknown instead.",
    ),
    # ------------------------------------------------------------------ init
    _hle(
        "init-rows-born-implemented",
        "        entries[i].state = XINPUT_ENTRY_STUB;",
        "        entries[i].state = XINPUT_ENTRY_IMPLEMENTED;",
        "a table born implemented empties the backlog, silences every stub report and "
        "makes implemented_count read 15 of 15 before any work exists.",
    ),
    _hle(
        "init-ports-born-synthetic",
        "        ports[i] = XINPUT_PORT_EMPTY;",
        "        ports[i] = XINPUT_PORT_SYNTHETIC;",
        "every port defaulting to EMPTY is the module's honest-default contract; a "
        "port born synthetic is a fabricated controller nobody attached and nothing "
        "announced.",
    ),
    _hle(
        "init-keeps-unknown-calls",
        "    unknown_calls = 0;",
        "    unknown_calls = 1;",
        "init must zero the accounting; a leftover unknown-call count makes a clean "
        "re-initialised run report a table/binary disagreement that never happened.",
    ),
    _hle(
        "init-keeps-a-state-size",
        "    state_size = XINPUT_STATE_SIZE_UNSET;",
        "    state_size = XINPUT_MEASURED_STATE_SIZE;",
        "the layout must be UNSET until someone derives or adopts one; a size that "
        "survives init is the first step toward writing guest memory with a layout "
        "nobody validated in this run.",
    ),
    # ------------------------------------------------------------------ dispatch
    _hle(
        "lookup-accepts-a-neighbour",
        "        if (entries[i].address == address) {",
        "        if (entries[i].address == address || entries[i].address + 1u == address) {",
        "the lookup comment promises it cannot return a neighbour; XInitDevices and "
        "XGetDevices are five bytes apart, so a one-byte tolerance dispatches device "
        "initialisation for a byte that is no function's entry.",
    ),
    _hle(
        "register-accepts-null-handler",
        "    if (!entry || !handler) {",
        "    if (!entry) {",
        "registering NULL marks the entry IMPLEMENTED with no handler, so every later "
        "call silently falls through to the default return while the report claims "
        "the function is done.",
    ),
    _hle(
        "register-leaves-stub",
        "    entry->handler = handler;\n    entry->state = XINPUT_ENTRY_IMPLEMENTED;",
        "    entry->handler = handler;\n    entry->state = XINPUT_ENTRY_STUB;",
        "a registered handler that stays STUB is never dispatched: the EMPTY adapter "
        "registers five handlers and every guest call would get the stub default "
        "instead of the adapter.",
        BOTH,
    ),
    _hle(
        "call-unknown-not-counted",
        "        unknown_calls++;",
        "        unknown_calls += 0u;",
        "an unknown target means the table and the binary disagree, which must reach "
        "the report's disagreement line; an uncounted one is diagnosed once in the "
        "log and then forgotten.",
    ),
    _hle(
        "call-data-symbol-diagnosed-as-unknown",
        "        if (data) {",
        "        if (data && false) {",
        "a call to a device-type table is an argument that reached a call slot; "
        "reporting it as a generic unknown target sends someone hunting for a missing "
        "function that never existed, which is the exact confusion the three-message "
        "split exists to prevent.",
    ),
    _hle(
        "call-inside-section-diagnosed-as-outside",
        "        } else if (address >= XINPUT_SECTION_VA_BEGIN && address < XINPUT_SECTION_VA_END) {",
        "        } else if (address >= XINPUT_SECTION_VA_END && address < XINPUT_SECTION_VA_BEGIN) {",
        "an in-section unknown means our surface table is incomplete, an out-of-"
        "section address means the caller is broken; collapsing the two messages "
        "points the first move at the wrong artefact.",
    ),
    _hle(
        "call-count-not-incremented",
        "    entry->call_count++;",
        "    entry->call_count += 0u;",
        "runtime call counts are what correct the site ranking (XInputGetState has "
        "one site and is the busiest function once anything runs); without them the "
        "report ranks by static sites forever and touched_count stays zero.",
    ),
    _hle(
        "call-implemented-not-dispatched",
        "    if (entry->state == XINPUT_ENTRY_IMPLEMENTED && entry->handler) {",
        "    if (entry->state == XINPUT_ENTRY_IMPLEMENTED && entry->handler && false) {",
        "an implemented function that is not dispatched returns the stub default and "
        "reports itself unimplemented; the EMPTY adapter's refusals would silently "
        "become success-shaped zeros.",
        BOTH,
    ),
    _hle(
        "call-stub-reports-every-time",
        "    if (!entry->reported) {\n        entry->reported = true;",
        "    if (!entry->reported) {\n        entry->reported = false;",
        "once-per-function is the report contract: XInputGetState is polled every "
        "frame, and per-call reporting buries the one-shot XInitDevices/XInputOpen "
        "lines that the enumeration order is learned from.",
    ),
    _hle(
        "call-default-return-dropped",
        "    return entry->default_return;",
        "    return 0u;",
        "the default return is the only stub knob a caller has (a status an observed "
        "call site insists on); dropping it makes set_default_return a silent no-op.",
    ),
    # ------------------------------------------------------------------ counts
    _hle(
        "implemented-count-counts-stubs",
        "        if (entries[i].state == XINPUT_ENTRY_IMPLEMENTED) {\n            done++;",
        "        if (entries[i].state != XINPUT_ENTRY_IMPLEMENTED) {\n            done++;",
        "the implemented count feeds coverage reporting; counting stubs inverts it to "
        "15 minus the truth, which reads most plausible exactly when the least work "
        "is done.",
    ),
    _hle(
        "touched-needs-two-calls",
        "        if (entries[i].call_count > 0) {\n            touched++;",
        "        if (entries[i].call_count > 1u) {\n            touched++;",
        "touched means observed at all; requiring two calls hides every one-shot "
        "function (XInitDevices, XInputOpen) from the touched count, which is "
        "precisely the set the count exists to surface.",
    ),
    # ------------------------------------------------------------------ ports
    _hle(
        "attach-accepts-port-4",
        "    if (port >= XINPUT_PORT_COUNT) {\n        return false;\n    }\n    if (ports[port] == XINPUT_PORT_SYNTHETIC) {\n        return true;\n    }\n    ports[port] = XINPUT_PORT_SYNTHETIC;",
        "    if (port > XINPUT_PORT_COUNT) {\n        return false;\n    }\n    if (ports[port] == XINPUT_PORT_SYNTHETIC) {\n        return true;\n    }\n    ports[port] = XINPUT_PORT_SYNTHETIC;",
        "the console has four ports; accepting port 4 writes one past the ports "
        "array, and the bound check is the only thing between a caller's typo and "
        "that overrun.",
    ),
    _hle(
        "detach-leaves-the-pad",
        "ports[port] = XINPUT_PORT_EMPTY;\n    memset(&pads[port], 0, sizeof(pads[port]));",
        "ports[port] = ports[port];\n    memset(&pads[port], 0, sizeof(pads[port]));",
        "a detach that logs EMPTY while leaving the synthetic pad attached is the "
        "worst shape of failure here: the log and the guest now disagree about "
        "whether a fabricated controller exists.",
    ),
    _hle(
        "connected-inverted",
        "    if (port < XINPUT_PORT_COUNT && ports[port] == XINPUT_PORT_SYNTHETIC) {\n        return true;\n    }",
        "    if (port < XINPUT_PORT_COUNT && ports[port] != XINPUT_PORT_SYNTHETIC) {\n        return true;\n    }",
        "presence is the first bit the title reads about input and it decides which "
        "branch the whole init takes; inverting it reports controllers on empty "
        "ports and none on the attached one.",
    ),
    _hle(
        "empty-queries-not-counted",
        "    empty_queries++;",
        "    empty_queries += 0u;",
        "the empty-query count is the evidence that the honest default is what "
        "stalled a boot ('the menu will not respond'); without it the report claims "
        "no enumeration was ever answered no.",
    ),
    _hle(
        "connected-count-counts-empty",
        "        if (ports[i] == XINPUT_PORT_SYNTHETIC) {\n            count++;",
        "        if (ports[i] != XINPUT_PORT_SYNTHETIC) {\n            count++;",
        "the connected count picks between the report's EMPTY banner and its "
        "SYNTHETIC banner, and the EMPTY adapter's require_empty gate refuses on it; "
        "inverted, the gate refuses exactly when it should pass.",
        BOTH,
    ),
    _hle(
        "values-for-an-empty-port-accepted",
        "    if (ports[port] != XINPUT_PORT_SYNTHETIC) {",
        "    if (ports[port] != XINPUT_PORT_SYNTHETIC && false) {",
        "values for a port the guest was told is empty are input from a controller "
        "that does not exist even by this module's own account; accepted silently, "
        "they surface later as a mysterious button press.",
    ),
    _hle(
        "synthetic-values-not-counted",
        "    pads[port] = state;\n    synthetic_values++;",
        "    pads[port] = state;\n    synthetic_values += 0u;",
        "the fabricated-values count is what lets a run log say this run's input did "
        "not come from a controller; at zero the report omits the fabrication "
        "banner entirely.",
    ),
    # ------------------------------------------------------------------ layout
    _hle(
        "size-change-keeps-mappings",
        "        placements[i].offset = XINPUT_OFFSET_UNSET;\n        placements[i].width = 0;\n        placements[i].mapped = false;\n    }\n    if (size == XINPUT_STATE_SIZE_UNSET) {",
        "        placements[i].offset = placements[i].offset;\n        placements[i].width = placements[i].width;\n        placements[i].mapped = placements[i].mapped;\n    }\n    if (size == XINPUT_STATE_SIZE_UNSET) {",
        "a field was accepted because it fitted the size in force at the time; a "
        "mapping kept across a size change is unvalidated against the new size, "
        "which is the IO_STATUS_BLOCK overrun with one extra step.",
    ),
    _hle(
        "map-width-zero-accepted",
        "    if (width == 0) {",
        "    if (width == 0 && false) {",
        "a zero-width field writes nothing and is not a derivation; accepted, it "
        "counts as a mapped field, flips layout_usable and lets writes proceed on "
        "the strength of a mapping that does nothing.",
    ),
    _hle(
        "map-scalar-width-three-accepted",
        "    if (field != XINPUT_FIELD_ANALOG_RUN && width != 1u && width != 2u &&\n        width != 4u) {",
        "    if (field != XINPUT_FIELD_ANALOG_RUN && width != 1u && width != 2u &&\n        width != 3u) {",
        "scalar fields must be scalar access widths derived from the call site; this "
        "both admits a 3-byte scalar no x86 access produces and refuses the 4-byte "
        "packet number, so adopting the measured layout itself must fail loudly.",
    ),
    _hle(
        "map-wrapping-sum-accepted",
        "    if (offset > UINT32_MAX - width || offset + width > state_size) {",
        "    if (offset + width > state_size) {",
        "a wrapping offset+width compares small and passes the end check, which is "
        "the exact IO_STATUS_BLOCK failure the module names: a field at 0xFFFFFFFF "
        "writes wherever the wrap lands.",
    ),
    _hle(
        "map-exact-fit-refused",
        '    if (offset > UINT32_MAX - width || offset + width > state_size) {\n        log_printer("xinput: map_field(%s) refused -- offset %#x + width %u exceeds the "',
        '    if (offset > UINT32_MAX - width || offset + width >= state_size) {\n        log_printer("xinput: map_field(%s) refused -- offset %#x + width %u exceeds the "',
        "the bound is half-open: a field ending exactly at the size fits. The "
        "measured thumb_right_y occupies bytes 20..22 of the 22-byte structure, so "
        "an off-by-one here makes the measured layout fail its own adoption.",
    ),
    _hle(
        "map-touching-fields-refused",
        "        if (offset < other_end && other_begin < offset + width) {",
        "        if (offset <= other_end && other_begin <= offset + width) {",
        "adjacent is not overlapping: the measured layout is fully packed (digital "
        "buttons end at 6 where the analog run begins), so refusing touching ranges "
        "refuses the measured layout itself.",
    ),
    _hle(
        "map-overlapping-fields-accepted",
        "        if (offset < other_end && other_begin < offset + width) {\n            /* Two fields claiming the same bytes means at least one derivation is",
        "        if (offset < other_end && other_begin < offset + width && false) {\n            /* Two fields claiming the same bytes means at least one derivation is",
        "two fields claiming the same bytes means at least one derivation is wrong; "
        "accepted, one writes over the other in an order nobody chose and the "
        "symptom is a field that is intermittently right.",
    ),
    _hle(
        "map-offset-width-swapped",
        "    placements[field].offset = offset;\n    placements[field].width = width;",
        "    placements[field].offset = width;\n    placements[field].width = offset;",
        "the kernel_io survivor's lesson verbatim: a swapped pair can keep every "
        "aggregate check happy while every consumer reads the wrong field. Here it "
        "writes 2-byte fields at offset 2 regardless of derivation.",
    ),
    _hle(
        "map-field-not-marked-mapped",
        "    placements[field].mapped = true;",
        "    placements[field].mapped = placements[field].width == 0u;",
        "an accepted field that is not marked mapped is silently dropped from every "
        "write: map_field reports success, the write loop skips it, and the guest "
        "field keeps whatever bytes were there.",
    ),
    _hle(
        "usable-without-any-field",
        "    return state_size != XINPUT_STATE_SIZE_UNSET && xinput_hle_mapped_field_count() > 0;",
        "    return state_size != XINPUT_STATE_SIZE_UNSET;",
        "a size with zero mapped fields is not a usable layout; calling it usable "
        "turns the loud layout-unset refusal into a silent write of nothing, and "
        "silent nothing is the failure mode this module refuses on principle.",
    ),
    # ------------------------------------------------------------------ adoption
    _hle(
        "adopt-uses-the-frame-size",
        "    xinput_hle_set_state_size(XINPUT_MEASURED_STATE_SIZE);",
        "    xinput_hle_set_state_size(XINPUT_MEASURED_STATE_FRAME);",
        "the title allocates a 24-byte frame but only 22 bytes are measured; "
        "declaring 24 claims the two undecided trailing bytes are writable state, "
        "which is exactly the write-bytes-whose-nature-is-undecided refusal.",
    ),
    _hle(
        "adopt-analog-run-shifted",
        "        {XINPUT_FIELD_ANALOG_RUN, XINPUT_MEASURED_OFFSET_ANALOG_RUN, XINPUT_ANALOG_COUNT},",
        "        {XINPUT_FIELD_ANALOG_RUN, XINPUT_MEASURED_OFFSET_ANALOG_RUN + 1u, XINPUT_ANALOG_COUNT},",
        "the analog run is EIGHT bytes at 0x06 from the 8-entry table at 0x0047DF40; "
        "shifted by one it collides with thumb_left_x at 0x0E, so the compiled-in "
        "table must fail its own validation rather than adopt a wrong row.",
    ),
    _hle(
        "adopt-not-flagged",
        "    measured_layout_adopted = true;",
        "    measured_layout_adopted = xinput_hle_mapped_field_count() == 0u;",
        "'derived but not adopted' and 'adopted' are different situations and only "
        "one is waiting on analysis work; the flag is what keeps the report from "
        "sending someone to re-derive a layout that is already in force.",
    ),
    # ------------------------------------------------------------------ writes
    _hle(
        "write-without-a-layout",
        "    if (!xinput_hle_layout_usable()) {\n        write_refusals++;",
        "    if (!xinput_hle_layout_usable() && false) {\n        write_refusals++;",
        "the single most important refusal in the module: with no derived layout the "
        "write must refuse and say so, not proceed to write zero fields while the "
        "refusal counter and the loud banner both stay silent.",
    ),
    _hle(
        "write-with-no-writer-crashes",
        "    if (!guest_writer) {\n        write_refusals++;",
        "    if (!guest_writer && false) {\n        write_refusals++;",
        "no installed writer must be a counted refusal; falling through dereferences "
        "a NULL function pointer on the first mapped field.",
    ),
    _hle(
        "write-accepts-port-4",
        "    if (port >= XINPUT_PORT_COUNT) {\n        write_refusals++;",
        "    if (port > XINPUT_PORT_COUNT) {\n        write_refusals++;",
        "writing guest state for port 4 reads one past the pads array and marshals "
        "whatever memory follows into the guest as controller input.",
    ),
    _hle(
        "write-unmapped-fields-guessed",
        "        if (!placements[i].mapped) {",
        "        if (!placements[i].mapped && false) {",
        "skipped, not guessed: a partial layout is used for the fields it covers and "
        "nothing else. Writing unmapped fields emits writes at XINPUT_OFFSET_UNSET "
        "and inflates the written count to all seven.",
    ),
    _hle(
        "write-analog-overread",
        "                bytes[b] = b < XINPUT_ANALOG_COUNT ? pad->analog[b] : 0u;",
        "                bytes[b] = pad->analog[b % XINPUT_ANALOG_COUNT];",
        "a derived analog width longer than the eight-entry host run must zero-fill "
        "rather than fabricate: repeating the run writes the first pressures again "
        "as bytes the measurement never assigned.",
    ),
    _hle(
        "write-big-endian",
        "                bytes[b] = (uint8_t)((value >> (8u * b)) & 0xFFu);",
        "                bytes[b] = (uint8_t)((value >> (8u * (width - 1u - b))) & 0xFFu);",
        "the guest is little-endian; host-order writes are right by accident on one "
        "build host and wrong on another, and a byte-swapped packet number still "
        "looks like a changing packet number to a weak test.",
    ),
    _hle(
        "write-ignores-field-offset",
        "        guest_writer(guest_address + placements[i].offset, bytes, width,\n                     guest_writer_user);",
        "        guest_writer(guest_address, bytes, width,\n                     guest_writer_user);",
        "every field written at the structure base overwrites the packet number with "
        "whichever field the loop visits last; the per-field offset is the entire "
        "point of deriving a layout.",
    ),
    _hle(
        "write-count-constant",
        "        written++;\n    }\n\n    return written;",
        "        written += 0u;\n    }\n\n    return written;",
        "the return value is how a caller distinguishes 'wrote the mapped fields' "
        "from 'refused'; a constant zero makes every successful write "
        "indistinguishable from a refusal.",
    ),
    # ------------------------------------------------------------------ report ranking
    _hle(
        "rank-calls-ascending",
        "    if (candidate->call_count != best->call_count) {\n        return candidate->call_count > best->call_count;",
        "    if (candidate->call_count != best->call_count) {\n        return candidate->call_count < best->call_count;",
        "the ordering IS the product: runtime calls outrank measured sites because "
        "XInputGetState (1 site, called every frame) must top the queue once "
        "anything runs. Ascending order points the work queue at the least "
        "important function first.",
    ),
    _hle(
        "rank-sites-ascending",
        "    if (candidate->sites != best->sites) {\n        return candidate->sites > best->sites;",
        "    if (candidate->sites != best->sites) {\n        return candidate->sites < best->sites;",
        "before any run the backlog is ordered by measured sites; ascending order "
        "puts the two zero-site rows first and XGetDevices last, inverting the "
        "static work queue.",
    ),
    _hle(
        "rank-address-tiebreak-flipped",
        "    return candidate->address < best->address;",
        "    return candidate->address > best->address;",
        "address ascending is the determinism tiebreak among the eight one-site "
        "rows; flipped, two runs of the same binary print the tied rows in a "
        "different order than the pinned one.",
    ),
    _hle(
        "report-missing-counts-implemented",
        "        if (entries[i].state != XINPUT_ENTRY_IMPLEMENTED) {\n            missing++;",
        "        if (entries[i].state == XINPUT_ENTRY_IMPLEMENTED) {\n            missing++;",
        "the missing count is the headline of the report; inverted it reads '0 of 15 "
        "still need implementations' on a fresh table, and the backlog below prints "
        "implemented rows as work.",
    ),
    # ------------------------------------------------------------------ crosscheck
    _hle(
        "crosscheck-ignores-site-drift",
        "        if (mine->sites != refs[i].sites) {\n            disagreements++;",
        "        if (mine->sites != refs[i].sites && false) {\n            disagreements++;",
        "the crosscheck is the guard the duplicated table buys its keep with; "
        "ignoring site drift lets the compiled-in counts and the generated table "
        "diverge silently, which is the exact drift duplication invites.",
    ),
    _hle(
        "crosscheck-skips-the-reverse-direction",
        "        bool present = false;",
        "        bool present = true;",
        "a check that only walks refs passes against a table of ours that grew an "
        "invented row, the drift most likely to happen by hand; the reverse "
        "direction exists to report exactly that (and the two zero-site rows "
        "against an unmodified generated table).",
    ),
    _hle(
        "crosscheck-null-refs-accepted",
        "    if (!refs && count > 0) {",
        "    if (!refs && count > 0 && false) {",
        "NULL rows with a nonzero count must be its own counted disagreement, not a "
        "dereference of refs[0] in the loop below.",
    ),
    # ------------------------------------------------------------------ devices: preflight
    _dev(
        "empty-gate-tolerates-one-pad",
        '    if (xinput_hle_connected_count() != 0u)\n        refuse(entry, "explicit synthetic pad requires the --synthetic-pad adapter");',
        '    if (xinput_hle_connected_count() > 1u)\n        refuse(entry, "explicit synthetic pad requires the --synthetic-pad adapter");',
        "the EMPTY adapter's answers are only proven for zero devices; serving them "
        "with a synthetic pad attached reports an empty bus to a guest that was "
        "told a controller exists.",
    ),
    _dev(
        "ready-gate-dropped",
        '    if (!ready) refuse(entry, "EMPTY backend is not initialized");',
        '    if (!ready && false) refuse(entry, "EMPTY backend is not initialized");',
        "enumeration before initialisation reads and rewrites the device-type tables "
        "before the cold-state validation ever ran, so the zero-mask and "
        "declaration checks are bypassed for good.",
    ),
    _dev(
        "repeat-init-accepted",
        '    if (ready) refuse(INIT_ENTRY, "repeated initialization is not recovered");',
        '    if (ready && false) refuse(INIT_ENTRY, "repeated initialization is not recovered");',
        "a second XInitDevices is a title behaviour this adapter has no evidence "
        "for; recovering it silently re-zeroes the status globals mid-run instead "
        "of surfacing the unmodelled call.",
    ),
    _dev(
        "overlap-everything",
        "    return (uint64_t)a < (uint64_t)b + size_b && (uint64_t)b < (uint64_t)a + size_a;",
        "    return (uint64_t)a < (uint64_t)b + size_b || (uint64_t)b < (uint64_t)a + size_a;",
        "with OR instead of AND nearly every pair of ranges reads as overlapping, so "
        "valid distinct outputs are refused and initialisation itself dies on the "
        "declarations-versus-status check.",
    ),
    _dev(
        "overlap-touching",
        "static bool overlaps(uint32_t a, uint32_t size_a, uint32_t b, uint32_t size_b)\n{\n    return (uint64_t)a < (uint64_t)b + size_b && (uint64_t)b < (uint64_t)a + size_a;",
        "static bool overlaps(uint32_t a, uint32_t size_a, uint32_t b, uint32_t size_b)\n{\n    return (uint64_t)a <= (uint64_t)b + size_b && (uint64_t)b <= (uint64_t)a + size_a;",
        "ranges are half-open: an output ending exactly where another begins shares "
        "no byte. Treating touching as overlap refuses adjacent previous/"
        "reconnected outputs a caller may legitimately pack.",
    ),
    _dev(
        "unknown-type-table-accepted",
        '    if (!known) refuse(entry, "unknown device-type table");',
        '    if (!known && false) refuse(entry, "unknown device-type table");',
        "only the six measured descriptor tables exist; an unknown type address "
        "reaching table_at reads and rewrites twelve bytes of whatever guest "
        "memory it names.",
    ),
    _dev(
        "optional-null-output-everywhere",
        "    if (address == 0u && optional) return NULL;",
        "    if (address == 0u && (optional || address == 0u)) return NULL;",
        "NULL outputs are optional for peek only; changes requires both, and "
        "treating its zero as optional turns the mandatory-output refusal into a "
        "write through a NULL pointer later. (The straight deletion of `optional` "
        "is NOT-A-MUTANT under -Werror=unused-parameter, so the flag is bypassed "
        "while staying referenced.)",
    ),
    _dev(
        "output-mapped-for-one-byte",
        "    void *output = kernel_guest_at(address, 4u);",
        "    void *output = kernel_guest_at(address, 1u);",
        "outputs receive four-byte masks; checking one byte accepts an output whose "
        "tail crosses into an unmapped page and the later memcpy writes off the "
        "mapping's end.",
    ),
    _dev(
        "type-table-alias-accepted",
        '        if (overlaps(address, 4u, types[i], 12u))\n            refuse(entry, "output alias with a device-type table is not recovered");',
        '        if (overlaps(address, 4u, types[i], 12u) && false)\n            refuse(entry, "output alias with a device-type table is not recovered");',
        "an output aliasing a descriptor table makes the enumeration write corrupt "
        "the very state it enumerates; the original's behaviour for that case was "
        "never measured, so it must refuse rather than guess.",
    ),
    _dev(
        "overlapping-outputs-accepted",
        '    if (*first_at != NULL && *second_at != NULL && overlaps(first, 4u, second, 4u))\n        refuse(entry, "overlapping outputs are not recovered");',
        '    if (*first_at != NULL && *second_at != NULL && overlaps(first, 4u, second, 4u) && false)\n        refuse(entry, "overlapping outputs are not recovered");',
        "two outputs sharing bytes means the second memcpy destroys part of the "
        "first answer; which one wins was never measured, so the case is refused "
        "rather than ordered arbitrarily.",
    ),
    # ------------------------------------------------------------------ devices: init
    _dev(
        "init-any-count-accepted",
        '    if (count != 4u || input == NULL) refuse(INIT_ENTRY, "expected four readable declarations");',
        '    if ((count != 4u && false) || input == NULL) refuse(INIT_ENTRY, "expected four readable declarations");',
        "the measured boot passes exactly four declarations; another count is a "
        "different title behaviour whose declaration block this validation has "
        "never seen, so it must refuse rather than read 32 bytes of it anyway.",
    ),
    _dev(
        "init-declarations-unchecked",
        '    if (memcmp(words, expected, sizeof(words)) != 0)\n        refuse(INIT_ENTRY, "only the measured declaration order/counts are recovered");',
        '    if (memcmp(words, expected, sizeof(words)) != 0 && false)\n        refuse(INIT_ENTRY, "only the measured declaration order/counts are recovered");',
        "the declaration table is the strongest evidence this is the measured boot "
        "path; accepting any content recovers boots the EMPTY model was never "
        "validated against, one word of drift at a time.",
    ),
    _dev(
        "init-nonzero-masks-accepted",
        '        if ((state[0] | state[1] | state[2]) != 0u)\n            refuse(INIT_ENTRY, "cold EMPTY initialization requires zero device masks");',
        '        if (((state[0] | state[1] | state[2]) != 0u) && false)\n            refuse(INIT_ENTRY, "cold EMPTY initialization requires zero device masks");',
        "nonzero masks at cold init mean someone wrote device state before the "
        "backend existed; initialising over it hides the conflicting writer "
        "instead of refusing while the evidence is still on the table.",
    ),
    _dev(
        "init-skips-normalising-status",
        "    const uint32_t zero = 0u;\n    memcpy(status_count, &zero, sizeof(zero));\n    memset(status_flag, 0, 1u);",
        "    const uint32_t zero = 0u;\n    memcpy(status_count, &zero, 0u);\n    memset(status_flag, 0, 0u);",
        "the original's empty-model final values are both zero; leaving stale guest "
        "bytes in the PUBLIC status globals makes XGetDeviceEnumerationStatus "
        "report an enumeration that never happened.",
    ),
    _dev(
        "init-never-ready",
        "    ready = true;\n    if (pad_mode()) {",
        "    ready = announced;\n    if (pad_mode()) {",
        "a successful init that does not latch ready makes every later call refuse "
        "as uninitialised, and a repeated init stops being refused because the "
        "repeat gate reads the same flag.",
    ),
    # ------------------------------------------------------------------ devices: enumeration semantics
    _dev(
        "get-keeps-the-pending-mask",
        "    state[1] = 0u;\n    state[2] = state[0];\n    memcpy(table, state, sizeof(state));\n    return state[0];",
        "    state[1] = state[1];\n    state[2] = state[0];\n    memcpy(table, state, sizeof(state));\n    return state[0];",
        "XGetDevices consumes the pending-change mask (state[1]); left set, the "
        "next XGetDeviceChanges reports phantom insertions for devices the title "
        "already enumerated.",
    ),
    _dev(
        "get-keeps-the-snapshot",
        "    state[1] = 0u;\n    state[2] = state[0];\n    memcpy(table, state, sizeof(state));",
        "    state[1] = 0u;\n    state[2] = state[2];\n    memcpy(table, state, sizeof(state));",
        "XGetDevices snapshots the current mask into state[2]; without the "
        "snapshot, a later changes call diffs against a stale baseline and "
        "reports devices that never moved.",
    ),
    _dev(
        "get-returns-the-cleared-mask",
        "    memcpy(table, state, sizeof(state));\n    return state[0];\n}\nstatic uint32_t locked_xinput_devices_peek(uint32_t type, uint32_t previous, uint32_t reconnected)",
        "    memcpy(table, state, sizeof(state));\n    return state[1];\n}\nstatic uint32_t locked_xinput_devices_peek(uint32_t type, uint32_t previous, uint32_t reconnected)",
        "XGetDevices returns the current device mask; returning the just-cleared "
        "pending mask reports zero devices to the caller that drives gamepad "
        "open decisions.",
    ),
    _dev(
        "peek-reconnect-union",
        "    memcpy(state, table, sizeof(state));\n    const uint32_t reconnect = state[1] & state[2] & state[0];",
        "    memcpy(state, table, sizeof(state));\n    const uint32_t reconnect = state[1] | state[2] | state[0];",
        "a reconnect is a device that was snapshotted, changed and is present again "
        "(the AND of all three masks); the union calls every known device a "
        "reconnect.",
    ),
    _dev(
        "peek-previous-is-current",
        "    if (previous_at != NULL) memcpy(previous_at, &state[2], 4u);",
        "    if (previous_at != NULL) memcpy(previous_at, &state[0], 4u);",
        "peek's previous output is the snapshot (state[2]), not the current mask; "
        "writing current makes previous==current always, so the caller's own diff "
        "logic can never see a change.",
    ),
    _dev(
        "peek-mutates-the-table",
        "    const uint32_t reconnect = state[1] & state[2] & state[0];\n    if (previous_at != NULL) memcpy(previous_at, &state[2], 4u);",
        "    const uint32_t reconnect = state[1] & state[2] & state[0];\n    state[1] = 0u;\n    state[2] = state[0];\n    memcpy((void *)(uintptr_t)(const void *)table, state, sizeof(state));\n    if (previous_at != NULL) memcpy(previous_at, &state[2], 4u);",
        "XPeekDevices is the read-only probe: it must not consume the pending mask "
        "or move the snapshot. A peek that commits turns every probe into a "
        "destructive XGetDevices.",
    ),
    _dev(
        "changes-without-pending-reports",
        "    if (state[1] != 0u) {",
        "    if (state[1] != 0u || state[0] != state[2]) {",
        "the pending mask gates change reporting: with no pending change the call "
        "reports nothing even when the masks differ. The wider gate invents a "
        "change event the pending mask never recorded.",
    ),
    _dev(
        "changes-added-is-removed",
        "        added = (~state[2] & state[0]) | reconnect;\n        removed = (~state[0] & state[2]) | reconnect;",
        "        added = (~state[0] & state[2]) | reconnect;\n        removed = (~state[2] & state[0]) | reconnect;",
        "insertions are devices present now and absent from the snapshot; swapped "
        "with removals, every plug event reads as an unplug, which a title answers "
        "by closing the handle it just opened.",
    ),
    _dev(
        "changes-returns-insertions-only",
        "    return (added | removed) != 0u ? 1u : 0u;",
        "    return added != 0u ? 1u : 0u;",
        "the boolean answer is 'did anything change'; reporting only insertions "
        "makes a pure removal invisible to a caller that polls the boolean before "
        "reading the masks.",
    ),
    _dev(
        "status-ignores-the-flag",
        "    return count != 0u || flag != 0u ? 1u : 0u;",
        "    return count != 0u ? 1u : 0u;",
        "enumeration is in progress when either global says so; ignoring the flag "
        "tells the title enumeration finished while the count is still zero and "
        "the flag says otherwise.",
    ),
    # ------------------------------------------------------------------ devices: dispatch
    _dev(
        "init-handler-reads-one-argument",
        "    const uint32_t count = argument(context, INIT_ENTRY, 0u);\n    const uint32_t declarations = argument(context, INIT_ENTRY, 1u);",
        "    const uint32_t count = argument(context, INIT_ENTRY, 1u);\n    const uint32_t declarations = argument(context, INIT_ENTRY, 1u);",
        "the wrapper takes (count, declarations) in stack order; reading the "
        "declarations pointer as the count makes the measured four-declaration "
        "boot refuse at dispatch while direct calls still pass.",
    ),
    _dev(
        "peek-handler-arguments-swapped",
        "    const uint32_t previous = argument(context, PEEK_ENTRY, 1u);\n    const uint32_t reconnected = argument(context, PEEK_ENTRY, 2u);",
        "    const uint32_t previous = argument(context, PEEK_ENTRY, 2u);\n    const uint32_t reconnected = argument(context, PEEK_ENTRY, 1u);",
        "the dispatch shim is the only place argument order can silently rot: "
        "swapped outputs write the snapshot into the caller's reconnect slot and "
        "the reconnect mask into its previous slot.",
    ),
    _dev(
        "register-miscounts",
        "    count += xinput_hle_register(PEEK_ENTRY, peek_handler) ? 1u : 0u;",
        "    count += xinput_hle_register(PEEK_ENTRY, peek_handler) ? 0u : 1u;",
        "the registration count is the caller's only signal that all five entries "
        "took; a miscount hides a failed registration behind a plausible total.",
    ),
]
