"""Mutations for XBE section residency: ordinals 327 XeLoadSection and 328 XeUnloadSection.

OWNED BY THE XE-SECTION TASK. One file per owner, so concurrent tasks cannot clobber each
other's mutation bytes -- see `tools/mutate/sets/_example.py` for the rule and the two
traps it records.

WHY THIS MODULE NEEDS MUTATION COVERAGE MORE THAN MOST. A wrong implementation here is
INVISIBLE in a live run. `xbe_map()` already copies every XBE section into guest memory,
demand-paged or not, so the pages the title asks for are resident before it asks and a
handler that returned STATUS_SUCCESS and did nothing else would look perfect: the title
gets a working VirtualAddress either way, the boot advances by exactly as many kernel
calls, and the trace reads the same. The lie only surfaces at the matching unload, as an
underflow -- and only if the underflow is reported rather than clamped. So almost every
entry below injects a defect whose symptom is a PLAUSIBLE SUCCESS, not a crash.

TWO THINGS HAVE NO MUTATION COVERAGE AND ARE RECORDED RATHER THAN FAKED.

  1. THE ABI ROWS. The arity that matters most -- ONE stack argument for 327, where the
     measured table publishes TWO -- lives in `src/host/kernel_thunk.c`, and `tsfp_host`
     is not a ctest binary, so no mutation here can reach it. The handler-side proxies are
     `test_one_argument_is_enough` (a one-slot frame must succeed, which a handler reading
     argument 1 would refuse) and `test_one_argument_is_required` (an empty frame must be
     refused), which pin the count from both sides of the handler. The row itself is
     verified by the live boot: with it the run advances from 124 to 125 kernel calls and
     stops on ordinal 236; with a wrong row the guest's modelled esp would be off by four
     bytes from that point on.

  2. THE kernel_guest_write_u32 FAILURE ARMS. No ctest binary can construct a guest
     address that reads fine and then refuses a write, because `kernel_guest_at()` applies
     the same test to both. Same gap `rtl_object.py` records for ordinal 246.

ONE MUTATION IS DELIBERATELY ABSENT. Substituting the derived section INDEX for the guest
POINTER directly in the reference-count address (`index + 0x18` instead of
`section + 0x18`) would dereference guest address 0x18 and kill by SIGSEGV. The runner
scores a crash as a kill, so it would read as coverage, but it would prove only that low
memory is unmapped. The index/pointer confusion is injected in `xe-handle-treated-as-an-
index` below instead, where it is caught by an assertion that says what was wrong.
"""

MUTATIONS: list[dict] = [
    # ------------------------------------------------- the reference count must be real
    {
        "id": "xe-load-does-not-increment-the-reference-count",
        "file": "src/xbox/kernel_xe.c",
        "old": "    if (!kernel_guest_write_u32(section + SECTION_REFERENCE_COUNT, references + 1u)) {",  # noqa: E501 -- must match the C source exactly
        "new": "    if (!kernel_guest_write_u32(section + SECTION_REFERENCE_COUNT, references)) {",
        "targets": ["test_kernel_xe"],
        "why": "THE central defect this module exists to prevent, and the one the live "
        "boot cannot catch. The pages are already mapped, so a load that returns "
        "STATUS_SUCCESS without counting hands the title a working VirtualAddress "
        "and the run still reaches 125 kernel calls and still stops on ordinal 236. "
        "The only observable difference is the count in guest memory at Section+0x18 "
        "and the underflow on the matching unload. A survivor would mean the suite "
        "never reads that field back and the whole module is decoration.",
    },
    {
        "id": "xe-unload-does-not-decrement-the-reference-count",
        "file": "src/xbox/kernel_xe.c",
        "old": "    if (!kernel_guest_write_u32(section + SECTION_REFERENCE_COUNT, references - 1u)) {",  # noqa: E501 -- must match the C source exactly
        "new": "    if (!kernel_guest_write_u32(section + SECTION_REFERENCE_COUNT, references)) {",
        "targets": ["test_kernel_xe"],
        "why": "the mirror image, and it fails UPWARD rather than downward: counts only "
        "ever rise, no unload ever underflows, and the module's loudest diagnostic "
        "goes permanently silent. A section would stay 'referenced' forever, which on "
        "a real kernel means never decommitted. Nothing in the boot would notice, "
        "because the title's three load/unload pairs each return success either way.",
    },
    {
        "id": "xe-reference-count-written-one-field-high",
        "file": "src/xbox/kernel_xe.c",
        "old": "#define SECTION_REFERENCE_COUNT 0x18u",
        "new": "#define SECTION_REFERENCE_COUNT 0x1Cu",
        "targets": ["test_kernel_xe"],
        "why": "+0x18 is the one field in XBE_SECTION_HEADER that this image never reads, "
        "so it is the one whose offset cannot be confirmed from a guest instruction -- "
        "kernel_xe.h pins it by the shared-page counter chain at +0x1C and +0x20 "
        "instead. Shifting one slot high overwrites "
        "HeadSharedPageReferenceCountAddress, a LIVE pointer, with a small integer, "
        "and the count still reads back as whatever was written. A test that only "
        "round-trips the field it wrote cannot tell the difference, which is why the "
        "neighbours are asserted unchanged.",
    },
    # ------------------------------------------------- the underflow must be reported
    {
        "id": "xe-underflow-reported-to-the-guest-as-success",
        "file": "src/xbox/kernel_xe.c",
        "old": """                         (unsigned)section, where, name);
        return STATUS_INVALID_PARAMETER;
    }
    if (!kernel_guest_write_u32(section + SECTION_REFERENCE_COUNT, references - 1u)) {""",
        "new": """                         (unsigned)section, where, name);
        return STATUS_SUCCESS;
    }
    if (!kernel_guest_write_u32(section + SECTION_REFERENCE_COUNT, references - 1u)) {""",
        "targets": ["test_kernel_xe"],
        "why": "a clamp that tells the GUEST nothing. The host log still names the "
        "underflow, so a human reading a trace would see it, but the title's "
        "`test eax,eax; jge` at 0x0037C9A8 reads STATUS_SUCCESS and "
        "_XUnloadSectionByHandle@4 returns TRUE for an unload that did not happen. "
        "That is precisely 'clamped instead of reported' from the only point of view "
        "that affects execution, and a suite that checked the log text but not the "
        "returned status would miss it.",
    },
    {
        "id": "xe-underflow-not-counted",
        "file": "src/xbox/kernel_xe.c",
        "old": "        underflow_count++;",
        "new": "        (void)0;",
        "targets": ["test_kernel_xe"],
        "why": "the counter is the machine-readable half of the diagnostic, and it is what "
        "a later task or a host-side report would assert on rather than grepping log "
        "text. With it stuck at zero, every other mutation in this file that produces "
        "an underflow becomes harder to detect, so this one is load-bearing for the "
        "rest of the set.",
    },
    {
        "id": "xe-underflow-wraps-the-count-instead-of-leaving-it",
        "file": "src/xbox/kernel_xe.c",
        "old": "    if (references == 0u) {",
        "new": "    if (references == 0u && false) {",
        "targets": ["test_kernel_xe"],
        "why": "the worst available failure and the one a naive implementation falls into: "
        "with the guard gone, 0 - 1 writes 0xFFFFFFFF into guest memory and the next "
        "four billion unmatched unloads all succeed. The underflow becomes a "
        "permanent lie rather than a single reported event. Written as `&& false` "
        "rather than deleting the block, because deleting it would leave an unused "
        "label and score NOT-A-MUTANT.",
    },
    # ------------------------------------------------- the handle is a pointer, not an index
    {
        "id": "xe-handle-treated-as-an-index",
        "file": "src/xbox/kernel_xe.c",
        "old": """    if (section < table) {
        return false;
    }
    const uint32_t offset = section - table;""",
        "new": """    if (section < table && false) {
        return false;
    }
    const uint32_t offset = section * KERNEL_XE_SECTION_HEADER_BYTES;""",
        "targets": ["test_kernel_xe"],
        "why": "the index/handle confusion, injected where an assertion can name it. A "
        "'handle' here is a guest POINTER straight at an XBE_SECTION_HEADER -- "
        "MEASURED at 0x0037C977, `mov eax, edi`, where _XGetSectionHandleA@4 returns "
        "the table cursor itself -- and the word 'handle' invites reading it as a "
        "small index. With this mutation the real handle 0x00010648 scales to an "
        "offset far past the table and every load is refused as out of range, while a "
        "small bogus value would be ACCEPTED as a section. Both directions wrong, and "
        "both look like a guest problem rather than ours.",
    },
    {
        "id": "xe-stride-alignment-not-checked",
        "file": "src/xbox/kernel_xe.c",
        "old": "    if (offset % KERNEL_XE_SECTION_HEADER_BYTES != 0u) {",
        "new": "    if (offset % KERNEL_XE_SECTION_HEADER_BYTES != 0u && false) {",
        "targets": ["test_kernel_xe"],
        "why": "a pointer into the MIDDLE of a header is in range and still catastrophic: "
        "every field is read at the wrong offset, so a VirtualSize is read as a "
        "VirtualAddress and a FileAddress as a reference count, and the load reports "
        "success with plausible-looking numbers. Range alone is not the bounds check; "
        "the 0x38 stride measured at 0x0037C938 is half of it.",
    },
    # ------------------------------------------------- out of range must not succeed
    {
        "id": "xe-out-of-range-section-returns-success",
        "file": "src/xbox/kernel_xe.c",
        "old": "    if (!kernel_xe_section_index(section, &index)) {",
        "new": "    if (!kernel_xe_section_index(section, &index) && false) {",
        "targets": ["test_kernel_xe"],
        "why": "the bounds check made decorative while still being computed, which is how "
        "a check rots: the code reads as though it validates. Any readable guest "
        "pointer would then be incremented at +0x18 and reported as 'section 0', so "
        "the host would be writing into arbitrary guest memory and SAYING it "
        "validated. A survivor means no test ever passes a handle outside the table.",
    },
    {
        "id": "xe-not-found-sentinel-accepted-as-a-handle",
        "file": "src/xbox/kernel_xe.c",
        "old": "    if (section == 0xFFFFFFFFu) {",
        "new": "    if (section == 0xFFFFFFFFu && false) {",
        "targets": ["test_kernel_xe"],
        "why": "0xFFFFFFFF is what _XGetSectionHandleA@4 returns for a name it did not "
        "find (`or eax, 0xFFFFFFFF` at 0x0037C96D), and the title checks for it at "
        "0x00381096 before calling. Without the explicit case it still gets refused, "
        "but for the generic reason 'not a readable guest address' -- which sends the "
        "next reader looking for a memory-mapping bug instead of a missing "
        "not-found check on a path that forgot one. The mutation survives any test "
        "that only asserts the status.",
    },
    {
        "id": "xe-reference-count-at-the-maximum-wraps-to-zero",
        "file": "src/xbox/kernel_xe.c",
        "old": "    if (references == 0xFFFFFFFFu) {",
        "new": "    if (references == 0xFFFFFFFFu && false) {",
        "targets": ["test_kernel_xe"],
        "why": "an increment that wraps to zero turns the VERY NEXT unload into a reported "
        "underflow, so the module's own loudest diagnostic would fire and blame the "
        "guest for our arithmetic. Refusing the increment keeps the count true and "
        "the blame in the right place.",
    },
    # ------------------------------------------------- residency is the one real promise
    {
        "id": "xe-load-succeeds-for-pages-the-host-never-mapped",
        "file": "src/xbox/kernel_xe.c",
        "old": "    if (!pages_resident(virtual_addr, virtual_size, &probe_errno)) {",
        "new": "    if (!pages_resident(virtual_addr, virtual_size, &probe_errno) && false) {",
        "targets": ["test_kernel_xe"],
        "why": "XeLoadSection makes exactly ONE promise -- on success the body is readable "
        "-- and this is the only check in the module that verifies something instead "
        "of recording it. Without it the handler hands back a VirtualAddress for "
        "memory that is not there, and the SIGSEGV lands on the title's first read, "
        "arbitrarily far from this call and with nothing connecting the two. "
        "DEPENDS ON `test_a_section_whose_pages_are_not_mapped_is_refused` actually "
        "running: that test SKIPS, loudly, if no unmapped guest address can be found, "
        "and a skip here would show up as this mutation surviving.",
    },
    {
        "id": "xe-stale-residency-flagged-on-the-wrong-transition",
        "file": "src/xbox/kernel_xe.c",
        "old": "    const bool last_unload = (references == 1u);",
        "new": "    const bool last_unload = (references == 2u);",
        "targets": ["test_kernel_xe"],
        "why": "an off-by-one on the transition that matters. 1 -> 0 is the point where "
        "hardware decommits and we do not, so it is the point where a later read that "
        "should fault will quietly succeed. Reporting it at 2 -> 1 instead announces a "
        "section is gone while it still has a live reference, and stays silent when it "
        "really is. Both halves wrong, no crash, and the count still looks tidy.",
    },
    # ------------------------------------------------- announcements must stay honest
    {
        "id": "xe-missing-image-base-warned-on-every-call",
        "file": "src/xbox/kernel_xe.c",
        "old": "        const bool first = !warned_no_image_base;",
        "new": "        const bool first = true;",
        "targets": ["test_kernel_xe"],
        "why": "the gap must be announced ONCE. Repeating 'the bounds check is DISABLED' "
        "for every call buries whatever the next line says, and this module is "
        "reached on a path that also writes three files -- so a per-call warning would "
        "drown the part of the trace a reader is actually following. The inverse "
        "defect, never warning at all, is what makes a missing "
        "kernel_xe_set_image_base() in the host invisible, and that is the reason the "
        "warning exists.",
    },
    {
        "id": "xe-eager-mapping-announced-on-every-first-load",
        "file": "src/xbox/kernel_xe.c",
        "old": "    const bool announce_mapping = first_load && !warned_already_mapped;",
        "new": "    const bool announce_mapping = first_load;",
        "targets": ["test_kernel_xe"],
        "why": "same rule for the other announcement. The title loads three sections and "
        "each is a 0 -> 1 transition, so this repeats the same paragraph three times "
        "in a 125-call trace. The announcement is the thing that stops a reader "
        "inferring a paging event happened, and it only works if it is said once "
        "where it can be read.",
    },
    # ------------------------------------------------- the header must be an XBE header
    {
        "id": "xe-image-base-magic-not-checked",
        "file": "src/xbox/kernel_xe.c",
        "old": "    if (!kernel_guest_read_u32(base, &magic) || magic != XBE_MAGIC_XBEH) {",
        "new": "    if (!kernel_guest_read_u32(base, &magic)) {",
        "targets": ["test_kernel_xe"],
        "why": "without the 'XBEH' check, a base pointed anywhere readable yields a section "
        "table built from whatever two dwords happen to sit at +0x11C and +0x120. The "
        "bounds check then runs against a fabricated range and reports refusals and "
        "acceptances with equal confidence -- strictly worse than the announced "
        "no-bounds-check state, because it looks like validation.",
    },
    {
        "id": "xe-section-table-upper-bound-allowed-to-wrap",
        "file": "src/xbox/kernel_xe.c",
        "old": "    if (sections > (0xFFFFFFFFu - headers) / KERNEL_XE_SECTION_HEADER_BYTES) {",
        "new": "    if (sections > (0xFFFFFFFFu - headers) / KERNEL_XE_SECTION_HEADER_BYTES && false) {",  # noqa: E501 -- must match the C source exactly
        "targets": ["test_kernel_xe"],
        "why": "`table + count * 0x38` computed on values read out of guest memory can wrap "
        "past 2^32, and a wrapped upper bound is a LOW address, so the range test "
        "admits essentially any handle. This mutation survived the first version of "
        "`test_an_overflowing_section_count_is_refused`, which used a count so large "
        "that the flat SECTION_COUNT_MAX cap caught it first and the wrap arithmetic "
        "was never reached -- the test now also uses a small count against a table "
        "address high enough to wrap. Recorded because 'the test exercises an earlier "
        "guard than the one it names' is a failure mode that reads as coverage.",
    },
]
