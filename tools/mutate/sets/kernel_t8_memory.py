# SPDX-License-Identifier: GPL-3.0-or-later
"""Mutations for the T8a/T8d/T94 memory ordinals: 175, 176 and 179 in `kernel_memory.c`.

OWNED BY THE T258 KERNEL-ORDINAL PORT. One file per owner, see `tools/mutate/sets/_example.py`
for the rule and for the traps it records. These re-create, as committed one-line
mutations, the hand sweeps that the T8a ("20/20 lock mutants killed"), T8d and T94 commits
record only as a count.

WHAT THESE ARE CHOSEN TO CATCH. Page locks are bookkeeping nothing else reads: a lock that
lands on the wrong page, an unlock that frees the wrong depth, or a refusal that is not
counted leaves every guest call returning STATUS_SUCCESS and the boot unchanged. So each
entry is a rule whose violation is INVISIBLE to the title and visible only to the suite's
per-page depth probe, its log capture, or its refusal counter. 179 answers a value the
title branches on, so its mutants are the wrong-but-plausible protection.

Neighbouring MmCreateKernelStack (169) and MmDeleteKernelStack (170) live in the same file
and the same suite, so their delete-side proof and return-the-top rules are covered too.

NOT COVERED, BY DESIGN. The table-exhaustion arm (`realloc` returning NULL, the
"ran out of host memory" log) cannot be reached without failing the host allocator, so no
mutation of it is listed. The mutex around the lock table is likewise only observable
under a race, so it carries no mutation here; `test_concurrent_locks_lose_no_count` is
its only check.

CLOSED BY T268 (were measured survivors, now in the set below): a range ending EXACTLY on 4 GB
(`end > ...` as `>=`), the lock-table growth step (`capacity * 2` as `* 1`, killed by locking
4096 pages so the overrun is reported as heap corruption when the table is freed) and the
page mask in the MmLockUnlockPhysicalPage "was not locked" log.
"""

SUITE = ["test_kernel_memory"]

MUTATIONS: list[dict] = [
    # ------------------------------------------------------------------ 175 argument rules
    {
        "id": "t8-memory-175-unlock-pages-accepts-two",
        "file": "src/xbox/kernel_memory.c",
        "old": "    if (unlock > 1u) {",
        "new": "    if (unlock > 2u) {",
        "targets": SUITE,
        "why": "only the literals 0 and 1 are measured for UnlockPages. An off-by-one in the "
        "refusal lets 2 through into the unlock arm (anything non-zero is an unlock), "
        "so a mis-read third argument silently RELEASES a lock instead of being reported.",
    },
    {
        "id": "t8-memory-175-zero-length-no-longer-a-no-op",
        "file": "src/xbox/kernel_memory.c",
        "old": """    if (bytes == 0u) {
        return STATUS_SUCCESS;
    }
    const uint64_t end = (uint64_t)base + bytes;""",
        "new": """    const uint64_t end = (uint64_t)base + bytes;""",
        "targets": SUITE,
        "why": "with zero bytes `end - 1` falls inside the previous page, so a zero-length "
        "call on an UNALIGNED base locks the page containing it. The first T8a sweep "
        "left this exact case alive: an aligned base hides it, because the loop then "
        "runs zero times.",
    },
    {
        "id": "t8-memory-175-past-4gb-not-refused",
        "file": "src/xbox/kernel_memory.c",
        "old": "    if (end > 0x100000000ULL) {",
        "new": "    if (end > 0x100000000ULL && false) {",
        "targets": SUITE,
        "why": "without the guard `(uint32_t)((end - 1) / page)` wraps to a LOW page, the "
        "loop never runs, and a wrapping range is accepted in silence: no lock, no "
        "refusal, no log, which is the plausible-but-wrong outcome.",
    },
    # ------------------------------------------------------------------ 175 page span
    {
        "id": "t8-memory-175-last-page-includes-the-end-boundary",
        "file": "src/xbox/kernel_memory.c",
        "old": "    const uint32_t last_page = (uint32_t)((end - 1u) / GUEST_PAGE_SIZE);",
        "new": "    const uint32_t last_page = (uint32_t)(end / GUEST_PAGE_SIZE);",
        "targets": SUITE,
        "why": "`end` is exclusive. Dividing it directly locks one extra page whenever the "
        "range ends on a page boundary, which is the common case (page-multiple "
        "buffers), and the neighbouring allocation's page then reads as locked.",
    },
    {
        "id": "t8-memory-175-first-page-rounds-up",
        "file": "src/xbox/kernel_memory.c",
        "old": "    const uint32_t first_page = base / GUEST_PAGE_SIZE;",
        "new": "    const uint32_t first_page = (base + GUEST_PAGE_SIZE - 1u) / GUEST_PAGE_SIZE;",
        "targets": SUITE,
        "why": "a range starting mid-page TOUCHES that page. Rounding the start up skips it, "
        "so a lock on an unaligned buffer leaves its first page unprotected while the "
        "range check and every aligned case still pass.",
    },
    # ------------------------------------------------------------------ 175 tracking report
    {
        "id": "t8-memory-175-region-end-check-dropped",
        "file": "src/xbox/kernel_memory.c",
        "old": "        if (region == NULL || end > (uint64_t)region->address + region->size) {",
        "new": "        if (region == NULL) {",
        "targets": SUITE,
        "why": "the report exists to flag a lock that starts in a region but runs off its "
        "end. Checking only the base finds the region and says nothing about the part "
        "of the buffer that lies outside it.",
    },
    {
        "id": "t8-memory-175-region-end-check-is-inclusive",
        "file": "src/xbox/kernel_memory.c",
        "old": "        if (region == NULL || end > (uint64_t)region->address + region->size) {",
        "new": "        if (region == NULL || end >= (uint64_t)region->address + region->size) {",
        "targets": SUITE,
        "why": "`end` is exclusive, so a lock covering the whole region is exactly in "
        "bounds. `>=` reports every full-region lock as outside tracked memory, a "
        "false alarm on the most ordinary buffer and noise that hides real ones.",
    },
    {
        "id": "t8-memory-175-reserve-only-lock-not-reported",
        "file": "src/xbox/kernel_memory.c",
        "old": "        } else if (region->state != MEM_COMMIT) {",
        "new": "        } else if (region->state != MEM_COMMIT && false) {",
        "targets": SUITE,
        "why": "locking reserved-but-uncommitted memory is a title bug the model is meant "
        "to surface. Dropping the arm records the lock as if the pages were backed.",
    },
    {
        "id": "t8-memory-175-unlock-also-checks-the-region",
        "file": "src/xbox/kernel_memory.c",
        "old": """    if (unlock == 0u) {
        /* Against the allocator's tracking""",
        "new": """    if (unlock <= 1u) {
        /* Against the allocator's tracking""",
        "targets": SUITE,
        "why": "the region report is for LOCKS. Running it on unlock too makes every "
        "unlock of memory the title already freed log 'locks a range outside any "
        "region', a wrong message on the legitimate free-then-unlock order.",
    },
    # ------------------------------------------------------------------ 175 depth table
    {
        "id": "t8-memory-175-second-lock-does-not-nest",
        "file": "src/xbox/kernel_memory.c",
        "old": "        lock_table[index].depth++;",
        "new": "        lock_table[index].depth = 1u;",
        "targets": SUITE,
        "why": "a page locked twice needs two unlocks. Resetting the depth makes the second "
        "unlock an underflow report and frees a page the title still holds locked.",
    },
    {
        "id": "t8-memory-175-first-lock-starts-at-depth-two",
        "file": "src/xbox/kernel_memory.c",
        "old": "    lock_table[index].depth = 1u;",
        "new": "    lock_table[index].depth = 2u;",
        "targets": SUITE,
        "why": "an inflated starting depth means one unlock never releases the page, so the "
        "locked-page total stays up after a balanced lock/unlock pair.",
    },
    {
        "id": "t8-memory-175-lower-bound-skips-the-exact-match",
        "file": "src/xbox/kernel_memory.c",
        "old": "        if (lock_table[middle].page < page) {",
        "new": "        if (lock_table[middle].page <= page) {",
        "targets": SUITE,
        "why": "the bisection must return the FIRST entry >= page. With `<=` it returns the "
        "first entry > page, so an existing page is never found and is inserted again "
        "as a duplicate with its own depth.",
    },
    {
        "id": "t8-memory-175-insert-shifts-too-few-entries",
        "file": "src/xbox/kernel_memory.c",
        "old": "            (lock_table_count - index) * sizeof(*lock_table));",
        "new": "            (lock_table_count - index) * sizeof(*lock_table) / 2u);",
        "targets": SUITE,
        "why": "inserting below existing entries must shift ALL of them. Shifting half "
        "overwrites the tail, so locks taken in descending page order lose pages. A "
        "suite that locked in ascending order only appends and never shifts anything.",
    },
    {
        "id": "t8-memory-175-unlock-removes-the-entry-at-depth-one",
        "file": "src/xbox/kernel_memory.c",
        "old": "    if (--lock_table[index].depth == 0u) {",
        "new": "    if (--lock_table[index].depth <= 1u) {",
        "targets": SUITE,
        "why": "an entry is dropped only at depth 0. Dropping at 1 loses one nesting level "
        "per page, so three locks need only two unlocks and the third reads as an "
        "underflow.",
    },
    {
        "id": "t8-memory-175-unlock-leaves-the-empty-entry",
        "file": "src/xbox/kernel_memory.c",
        "old": "        memmove(&lock_table[index], &lock_table[index + 1u],",
        "new": "        memmove(&lock_table[index], &lock_table[index],",
        "targets": SUITE,
        "why": "removing a page must close the gap. A self-move leaves the dead entry in "
        "place, so the next page takes its slot and ordering, and the bisection that "
        "depends on it, is wrong from then on.",
    },
    {
        "id": "t8-memory-175-unlock-never-shrinks-the-count",
        "file": "src/xbox/kernel_memory.c",
        "old": "        lock_table_count--;",
        "new": "        (void)lock_table_count;",
        "targets": SUITE,
        "why": "the locked-page total is the table length. If it only grows, a balanced "
        "lock and unlock leaves the total up forever and the drop to zero that every "
        "round-trip asserts never happens.",
    },
    {
        "id": "t8-memory-175-unlock-of-an-absent-page-hits-its-neighbour",
        "file": "src/xbox/kernel_memory.c",
        "old": "    if (index >= lock_table_count || lock_table[index].page != page) {",
        "new": "    if (index >= lock_table_count) {",
        "targets": SUITE,
        "why": "the lower bound lands on the NEXT page when the asked one is absent. Without "
        "the page-equality test an unlock of an unlocked page steals one level from "
        "an unrelated page and reports success.",
    },
    {
        "id": "t8-memory-175-underflow-not-counted",
        "file": "src/xbox/kernel_memory.c",
        "old": "            not_locked++;",
        "new": "            (void)not_locked;",
        "targets": SUITE,
        "why": "the underflow report is the model's only signal that the title unlocked "
        "memory it never locked. With the count stuck at zero the report never fires.",
    },
    {
        "id": "t8-memory-175-single-page-underflow-not-reported",
        "file": "src/xbox/kernel_memory.c",
        "old": "    if (not_locked != 0u) {",
        "new": "    if (not_locked > 1u) {",
        "targets": SUITE,
        "why": "an unlock of exactly one unlocked page is the usual shape of the bug, and "
        "the report is gated on the count. `> 1` hides the single-page case and "
        "reports only the rare multi-page one.",
    },
    {
        "id": "t8-memory-175-registration-keeps-the-previous-locks",
        "file": "src/xbox/kernel_memory.c",
        "old": "    lock_table_clear();\n    pthread_mutex_lock(&lock_table_mutex);\n    "
        "physical_refused_count = 0u;",
        "new": "    (void)lock_table_clear;\n    pthread_mutex_lock(&lock_table_mutex);\n    "
        "physical_refused_count = 0u;",
        "targets": SUITE,
        "why": "a fresh registration is a fresh kernel. Locks left over from the previous "
        "guest make the next run's first unlock balance against a lock it never took.",
    },
    # ------------------------------------------------------------------ 176
    {
        "id": "t8-memory-176-lock-mode-accepted",
        "file": "src/xbox/kernel_memory.c",
        "old": "    if (unlock != 1u) {\n        physical_refuse();",
        "new": "    if (unlock > 1u) {\n        physical_refuse();",
        "targets": SUITE,
        "why": "all four measured sites push 1, so lock mode (0) is unmeasured and must be "
        "refused loudly. Accepting it runs the UNLOCK path for a lock request and "
        "releases a page the title is asking to hold.",
    },
    {
        "id": "t8-memory-176-odd-modes-accepted",
        "file": "src/xbox/kernel_memory.c",
        "old": "    if (unlock != 1u) {\n        physical_refuse();",
        "new": "    if (unlock != 1u && unlock != 2u) {\n        physical_refuse();",
        "targets": SUITE,
        "why": "any value but the measured 1 is a mis-read argument. Letting 2 through "
        "treats garbage as an unlock request.",
    },
    {
        "id": "t8-memory-176-mode-refusal-not-counted",
        "file": "src/xbox/kernel_memory.c",
        "old": "        physical_refuse();\n"
        '        kernel_hle_log()("kernel: MmLockUnlockPhysicalPage(%#x, %#x) refused: '
        'UnlockPage is "',
        "new": '        kernel_hle_log()("kernel: MmLockUnlockPhysicalPage(%#x, %#x) refused: '
        'UnlockPage is "',
        "targets": SUITE,
        "why": "the refusal counter is how a harness learns that 176 was refused without "
        "scraping the log. A refusal that logs but does not count reads as success.",
    },
    {
        "id": "t8-memory-176-unmapped-refusal-not-counted",
        "file": "src/xbox/kernel_memory.c",
        "old": "        physical_refuse();\n"
        '        kernel_hle_log()("kernel: MmLockUnlockPhysicalPage(%#x, 1) refused: '
        'no tracked region "',
        "new": '        kernel_hle_log()("kernel: MmLockUnlockPhysicalPage(%#x, 1) refused: '
        'no tracked region "',
        "targets": SUITE,
        "why": "the other refusal arm. An unmapped physical address that is logged but not "
        "counted is indistinguishable, to a counting harness, from a page released.",
    },
    {
        "id": "t8-memory-176-releases-the-physical-page-number",
        "file": "src/xbox/kernel_memory.c",
        "old": "lock_table_unlock_page(address / GUEST_PAGE_SIZE);",
        "new": "lock_table_unlock_page(physical / GUEST_PAGE_SIZE);",
        "targets": SUITE,
        "why": "the lock table is keyed by VIRTUAL page. Using the physical page number "
        "releases a different (usually absent) page, so the virtual lock taken by 175 "
        "is never released through 176.",
    },
    {
        "id": "t8-memory-176-unlock-of-unlocked-page-not-reported",
        "file": "src/xbox/kernel_memory.c",
        "old": "    if (!was_locked) {",
        "new": "    if (!was_locked && false) {",
        "targets": SUITE,
        "why": "the report is the only trace of a title unlocking a page nothing locked.",
    },
    {
        "id": "t8-memory-176-registration-keeps-the-refusal-count",
        "file": "src/xbox/kernel_memory.c",
        "old": "    physical_refused_count = 0u;",
        "new": "    (void)physical_refused_count;",
        "targets": SUITE,
        "why": "the refusal count belongs to one guest. Surviving a re-registration makes "
        "a later run report refusals it never made.",
    },
    {
        "id": "t8-memory-176-first-physical-page-not-found",
        "file": "src/xbox/guest_mem.c",
        "old": "        if (physical >= info->physical &&",
        "new": "        if (physical > info->physical &&",
        "targets": SUITE,
        "why": "the physical-to-virtual walk behind 176. A strict lower bound makes the "
        "FIRST page of every region unresolvable, so exactly the page a title locks "
        "most (the buffer start) is refused as unmapped.",
    },
    {
        "id": "t8-memory-176-last-physical-byte-not-found",
        "file": "src/xbox/guest_mem.c",
        "old": "            (uint64_t)physical < (uint64_t)info->physical + info->size) {",
        "new": "            (uint64_t)physical < (uint64_t)info->physical + info->size - 1u) {",
        "targets": SUITE,
        "why": "an off-by-one at the top of the span drops the region's final byte, so the "
        "last page resolves as unmapped only for an address in its last byte.",
    },
    {
        "id": "t8-memory-176-physical-offset-dropped",
        "file": "src/xbox/guest_mem.c",
        "old": "            *out = info->address + (physical - info->physical);",
        "new": "            *out = info->address;",
        "targets": SUITE,
        "why": "without the offset every page of a region maps back to its first page, so "
        "176 on page N releases page 0's lock.",
    },
    # ------------------------------------------------------------------ 179
    {
        "id": "t8-memory-179-ignores-the-recorded-protection",
        "file": "src/xbox/kernel_memory.c",
        "old": "    return region->protect;\n}",
        "new": "    return PAGE_READWRITE;\n}",
        "targets": SUITE,
        "why": "READWRITE is the default for an allocation, so a hardcoded answer passes "
        "until MmSetAddressProtect changes it. The title branches on this value, so a "
        "stale READWRITE for a read-only page is a plausible wrong answer.",
    },
    {
        "id": "t8-memory-179-matches-only-the-region-base",
        "file": "src/xbox/kernel_memory.c",
        "old": """    const guest_region *region = guest_region_containing(args[0]);
    if (!region) {
        kernel_hle_log()("kernel: MmQueryAddressProtect(%#x) addresses no tracked \"""",
        "new": """    const guest_region *region = guest_region_at(args[0]);
    if (!region) {
        kernel_hle_log()("kernel: MmQueryAddressProtect(%#x) addresses no tracked \"""",
        "targets": SUITE,
        "why": "the title queries arbitrary addresses, not allocation bases. Resolving by "
        "base reports every interior address as untracked and answers 0, which the "
        "title reads as no access.",
    },
    {
        "id": "t8-memory-179-untracked-answers-a-protection",
        "file": "src/xbox/kernel_memory.c",
        "old": """                         args[0]);
        return 0u;
    }
    return region->protect;""",
        "new": """                         args[0]);
        return PAGE_READWRITE;
    }
    return region->protect;""",
        "targets": SUITE,
        "why": "the untracked value is INFERRED, not measured, and is kept at 0 so it "
        "cannot pass for a real protection. Answering READWRITE invents one.",
    },
    {
        "id": "t8-memory-179-untracked-not-logged",
        "file": "src/xbox/kernel_memory.c",
        "old": '        kernel_hle_log()("kernel: MmQueryAddressProtect(%#x) addresses no tracked '
        '"',
        "new": '        if (false) kernel_hle_log()("kernel: MmQueryAddressProtect(%#x) addresses '
        'no tracked "',
        "targets": SUITE,
        "why": "the log is what separates an inferred 0 from a measured one. Silence makes "
        "an unmapped answer indistinguishable from a real no-access page.",
    },
    {
        "id": "t8-memory-179-set-protect-records-nothing",
        "file": "src/xbox/guest_mem.c",
        "old": "    slot->info.protect = protect;",
        "new": "    slot->info.protect = (protect && false) ? protect : slot->info.protect;",
        "targets": SUITE,
        "why": "MmSetAddressProtect is what gives 179 something to report. A set that "
        "records nothing leaves the query answering the allocation default forever.",
    },
    {
        "id": "t8-memory-179-containing-lookup-excludes-the-last-byte",
        "file": "src/xbox/guest_mem.c",
        "old": "        if (addr >= info->address && (uint64_t)addr < (uint64_t)info->address + "
        "info->size) {",
        "new": "        if (addr >= info->address && (uint64_t)addr + 1u < "
        "(uint64_t)info->address + info->size) {",
        "targets": SUITE,
        "why": "the containing lookup behind 179. Excluding the last byte answers 0 "
        "(and logs an untracked region) for the final byte of a buffer.",
    },
    # ----------------------------------------------------------------- 169/170, same file and suite
    {
        "id": "t8-memory-169-returns-the-bottom-not-the-top",
        "file": "src/xbox/kernel_memory.c",
        "old": "    return (uint32_t)(address + bytes);",
        "new": "    return (uint32_t)address;",
        "targets": SUITE,
        "why": "measured: the return is the stack BASE, the TOP of the usable range, and "
        "sub_00388FAB switches esp to it and pushes DOWNWARD. Returning the bottom "
        "puts the first push below the allocation.",
    },
    {
        "id": "t8-memory-170-frees-without-proving-the-span",
        "file": "src/xbox/kernel_memory.c",
        "old": """    if (stack_base == 0u || stack_base < stack_limit || !region ||
        region->size != stack_base - stack_limit) {""",
        "new": """    if (stack_base == 0u || stack_base < stack_limit || !region) {""",
        "targets": SUITE,
        "why": "nothing may be freed unless the model can prove it allocated exactly that "
        "span. Dropping the size comparison lets a mismatched pair free someone "
        "else's region on the strength of its base alone.",
    },
    {
        "id": "t8-memory-175-176-handlers-swapped",
        "file": "src/xbox/kernel_memory.c",
        "old": "    {ORD_MmLockUnlockBufferPages, hle_mm_lock_unlock_buffer_pages},\n"
        "    {ORD_MmLockUnlockPhysicalPage, hle_mm_lock_unlock_physical_page},",
        "new": "    {ORD_MmLockUnlockBufferPages, hle_mm_lock_unlock_physical_page},\n"
        "    {ORD_MmLockUnlockPhysicalPage, hle_mm_lock_unlock_buffer_pages},",
        "targets": SUITE,
        "why": "correct handlers bound to each other's ordinals (a single swapped row does not "
        "compile, -Werror on the unused function). 175 would read its three arguments as "
        "(physical, unlock) and 176 would read two as a range, both returning "
        "STATUS_SUCCESS: registration still counts every ordinal and the boot reaches "
        "the same place.",
    },
    {
        "id": "t8-memory-179-and-query-allocation-size-handlers-swapped",
        "file": "src/xbox/kernel_memory.c",
        "old": "    {ORD_MmQueryAddressProtect, hle_mm_query_address_protect},\n"
        "    {ORD_MmQueryAllocationSize, hle_mm_query_allocation_size},",
        "new": "    {ORD_MmQueryAddressProtect, hle_mm_query_allocation_size},\n"
        "    {ORD_MmQueryAllocationSize, hle_mm_query_address_protect},",
        "targets": SUITE,
        "why": "179 bound to its one-argument sibling returns a plausible number that is a "
        "region SIZE, not a protection. A swapped pair is used because a lone rebind "
        "leaves a handler unused and does not compile.",
    },
    # ------------------------------------------------------------------ T268 gap closures
    {
        "id": "t8-memory-175-range-ending-on-4gb-refused",
        "file": "src/xbox/kernel_memory.c",
        "old": "    if (end > 0x100000000ULL) {",
        "new": "    if (end >= 0x100000000ULL) {",
        "targets": SUITE,
        "why": "`end` is exclusive, so a range whose last byte is 0xFFFFFFFF ends EXACTLY on "
        "4 GB and is legal. `>=` refuses the topmost page in silence from the guest's side "
        "(STATUS_SUCCESS, no lock recorded), so its later unlock reports a page that was "
        "never locked.",
    },
    {
        "id": "t8-memory-175-lock-table-never-grows",
        "file": "src/xbox/kernel_memory.c",
        "old": "lock_table_capacity == 0u ? 64u : lock_table_capacity * 2u;",
        "new": "lock_table_capacity == 0u ? 64u : lock_table_capacity * 1u;",
        "targets": SUITE,
        "why": "the 65th distinct locked page then writes past the table's heap block. Every "
        "guest call still returns STATUS_SUCCESS and the counts read back correctly until "
        "the heap metadata that was overwritten is next used, so only a test that locks "
        "far past 64 pages and frees the table sees it.",
    },
    {
        "id": "t8-memory-176-unlock-log-names-the-unmasked-address",
        "file": "src/xbox/kernel_memory.c",
        "old": "physical, address & ~(GUEST_PAGE_SIZE - 1u));",
        "new": "physical, address);",
        "targets": SUITE,
        "why": "the diagnostic for an unbalanced unlock is the only record of WHICH page was "
        "unlocked without being locked. Reporting the mid-page address the guest passed "
        "instead of the page it names makes two unbalanced unlocks of one page look like "
        "two different pages.",
    },
]
