# SPDX-License-Identifier: GPL-3.0-or-later
"""Mutations for the T8a/T8c/T8f clock and floating-point groups: 151, 228, 142 and 139.

OWNED BY THE T258 TASK. One file per owner, so concurrent tasks cannot clobber each other's
mutation bytes -- see `tools/mutate/sets/_example.py` for the rule and for the traps it
records. Ported from the hand sweeps recorded in the T8a (12 clock mutants), T8c-1 (21
fpstate mutants) and T8f-2 commits, which existed only as "N of N killed" and could not be
rerun.

WHAT THESE MUTATIONS ARE CHOSEN TO CATCH. None of these ordinals can fail loudly. A stall
that rounds down loses a microsecond per call and the title still runs, a floating-point
save that is recorded under the wrong thread reports a plausible depth, and a wall-clock
rebase with the wrong sign puts the guest in a different year with no error anywhere. Every
entry is a rule whose violation produces a PLAUSIBLE answer.

Everything here is a plain-build target: the kill suites are `test_kernel_clock`,
`test_kernel_fpstate` and `test_kernel_system_time`, so no lifted tree is needed. The
`*_thunk` suites (hand ABI rows for 151/142/139/228) are NOT used because they need
`-DTSFP_LIFTED_DIR`, and are not part of this set.

EQUIVALENT MUTANTS REMOVED, and why, so nobody re-adds them:
  - `NtSetSystemTime` NULL NewTime check dropped (`args[0] == 0u ||`): survived, because
    `kernel_guest_read_u32(0)` refuses the unmapped page-0 read itself, so the explicit
    test is redundant in every ctest binary (no suite maps guest address 0).
  - `NtSetSystemTime`/`KeQuerySystemTime` monotonic clamp `<=` to `<`: an equal reading
    needs two reads inside one 100ns unit of the host CLOCK_REALTIME, which no test can
    force (the host clock is not injectable), so the kill would be a coin flip.
  - Binding only ONE of 142/139 to the other's handler: does not compile (`-Werror`, the
    now-unused static function), so it is NOT-A-MUTANT. The swap of both is kept.
  - `NtSetSystemTime` returning STATUS_UNSUCCESSFUL when `clock_gettime(CLOCK_REALTIME)`
    fails: the failure cannot be provoked on a working host, so any mutation of that arm is
    unreachable from a ctest binary.
"""

CLOCK = "src/xbox/kernel_clock.c"
FPSTATE = "src/xbox/kernel_fpstate.c"
EVENT = "src/xbox/kernel_event.c"

MUTATIONS: list[dict] = [
    # ------------------------------------------------------------------ 151, the stall
    {
        "id": "t8-clock-stall-bound-excludes-the-limit",
        "file": CLOCK,
        "old": "    if (microseconds > KERNEL_CLOCK_STALL_MAX_US) {",
        "new": "    if (microseconds >= KERNEL_CLOCK_STALL_MAX_US) {",
        "targets": ["test_kernel_clock"],
        "why": "the bound is an inclusive limit: exactly one second is accepted and one "
        "second plus a microsecond is refused. An off-by-one here refuses the limit "
        "itself, and a suite that only tried 10 ms and 2 s would never see it.",
    },
    {
        "id": "t8-clock-stall-bound-never-refuses",
        "file": CLOCK,
        "old": "    if (microseconds > KERNEL_CLOCK_STALL_MAX_US) {",
        "new": "    if (microseconds > KERNEL_CLOCK_STALL_MAX_US && false) {",
        "targets": ["test_kernel_clock"],
        "why": "an unbounded stall lets a corrupt argument (a pointer pushed where a "
        "count was expected) advance the guest clock by hours, and every later frame "
        "time computed from it is wrong with no diagnostic.",
    },
    {
        "id": "t8-clock-stall-refusal-still-advances",
        "file": CLOCK,
        "old": "                         microseconds, KERNEL_CLOCK_STALL_MAX_US);\n"
        "        return 0u;\n",
        "new": "                         microseconds, KERNEL_CLOCK_STALL_MAX_US);\n",
        "targets": ["test_kernel_clock"],
        "why": "the refusal log says 'clock not advanced'. A refusal that logs and then "
        "advances anyway is the worst kind of wrong trace: the log claims the safe "
        "outcome while the clock has moved by the refused amount.",
    },
    {
        "id": "t8-clock-stall-remainder-dropped",
        "file": CLOCK,
        "old": "    stall_carry = scaled % 1000000u;",
        "new": "    stall_carry = 0u;",
        "targets": ["test_kernel_clock"],
        "why": "1,000,000 stalls of 1 us must add exactly one second. Without the carried "
        "sub-tick remainder every stall truncates, and a title that stalls in small "
        "steps (the XNET sites) runs its clock slow by up to a tick per call.",
    },
    {
        "id": "t8-clock-stall-remainder-not-added",
        "file": CLOCK,
        "old": "    const uint64_t scaled = (uint64_t)microseconds * KERNEL_CLOCK_FREQUENCY_HZ + "
        "stall_carry;",
        "new": "    const uint64_t scaled = (uint64_t)microseconds * KERNEL_CLOCK_FREQUENCY_HZ;",
        "targets": ["test_kernel_clock"],
        "why": "the remainder is saved but never used: the carry variable is updated "
        "correctly and the sum is still wrong. Dropping this addition keeps every "
        "single-stall assertion true and only a long sequence of stalls notices.",
    },
    {
        "id": "t8-clock-stall-starts-below-the-floor",
        "file": CLOCK,
        "old": "     * by the next read's max(). */\n    if (now_ticks < floor_ticks) {",
        "new": "     * by the next read's max(). */\n    if (now_ticks < floor_ticks && false) {",
        "targets": ["test_kernel_clock"],
        "why": "a stall that starts from a counter below the frame floor is swallowed by "
        "the next read's max(): the stall is accepted, counted and returned, and the "
        "guest clock does not move by it. Plausible, and only visible after a frame.",
    },
    {
        "id": "t8-clock-stall-wrong-scale",
        "file": CLOCK,
        "old": "    const uint64_t ticks = scaled / 1000000u;",
        "new": "    const uint64_t ticks = scaled / 1000001u;",
        "targets": ["test_kernel_clock"],
        "why": "an off-by-one in the microsecond divisor is a rounding error of one part "
        "in a million: invisible for a 10 us stall, a tick short per second of "
        "stalling. Only an exact total over a known stall count can see it.",
    },
    {
        "id": "t8-clock-stall-adds-an-extra-tick",
        "file": CLOCK,
        "old": "    now_ticks += ticks;\n",
        "new": "    now_ticks += ticks + 1u;\n",
        "targets": ["test_kernel_clock"],
        "why": "one stray tick per stall is a drift that no single check on a large stall "
        "would notice, but exact equality on a stall of zero or one does.",
    },
    {
        "id": "t8-clock-stall-returns-the-wrong-tick-count",
        "file": CLOCK,
        "old": "    return ticks;\n",
        "new": "    return scaled;\n",
        "targets": ["test_kernel_clock"],
        "why": "the return value is the advance actually applied, which the hle wrapper "
        "discards, so a wrong value is reachable only through the C API. Returning the "
        "pre-division product reports an advance a million times too large.",
    },
    {
        "id": "t8-clock-stall-does-not-publish-the-tick-count",
        "file": CLOCK,
        "old": "    now_ticks += ticks;\n    (void)publish_tick_count();\n",
        "new": "    now_ticks += ticks;\n",
        "targets": ["test_kernel_clock"],
        "why": "GetTickCount reads the published dword directly, so a stall that moves the "
        "counter without publishing leaves a title polling GetTickCount in a delay "
        "loop spinning forever against a counter that only stalls were advancing.",
    },
    {
        "id": "t8-clock-read-ignores-the-frame-floor",
        "file": CLOCK,
        "old": "    now_ticks += KERNEL_CLOCK_READ_STEP_TICKS;\n    if (now_ticks < floor_ticks) {",
        "new": "    now_ticks += KERNEL_CLOCK_READ_STEP_TICKS;\n    if (now_ticks < floor_ticks "
        "&& false) {",
        "targets": ["test_kernel_clock"],
        "why": "the floor is what makes a frame advance the time a title can observe even "
        "when it reads rarely. Without the clamp a read after a frame returns a time "
        "from before the frame.",
    },
    {
        "id": "t8-clock-read-step-lost",
        "file": CLOCK,
        "old": "    now_ticks += KERNEL_CLOCK_READ_STEP_TICKS;\n",
        "new": "    now_ticks += KERNEL_CLOCK_READ_STEP_TICKS - 1u;\n",
        "targets": ["test_kernel_clock"],
        "why": "the read step is what makes two back-to-back reads differ. One tick fewer "
        "still differs, so only an assertion on the exact step catches it.",
    },
    {
        "id": "t8-clock-tick-count-scale-wrong",
        "file": CLOCK,
        "old": "    const uint32_t ms = (uint32_t)((ticks / KERNEL_CLOCK_FREQUENCY_HZ) * 1000u +",
        "new": "    const uint32_t ms = (uint32_t)((ticks / KERNEL_CLOCK_FREQUENCY_HZ) * 1001u +",
        "targets": ["test_kernel_clock"],
        "why": "the published millisecond count is what GetTickCount returns, and a 0.1 "
        "percent scale error shifts every title timeout while looking monotonic.",
    },
    {
        "id": "t8-clock-frame-rate-bound-off-by-one",
        "file": CLOCK,
        "old": "    if (refresh_hz == 0u || refresh_hz > MAX_REFRESH_HZ) {",
        "new": "    if (refresh_hz == 0u || refresh_hz >= MAX_REFRESH_HZ) {",
        "targets": ["test_kernel_clock"],
        "why": "the refresh bound is inclusive. Refusing the limit itself rejects a legal "
        "rate and leaves the floor unmoved, which the frame loop does not report.",
    },
    {
        "id": "t8-clock-frame-carry-boundary",
        "file": CLOCK,
        "old": "    if (frame_carry >= refresh_hz) {",
        "new": "    if (frame_carry > refresh_hz) {",
        "targets": ["test_kernel_clock"],
        "why": "the carry owes a whole tick exactly when it reaches the refresh rate. "
        "With `>` the extra tick arrives one frame late at every exact multiple, so "
        "a minute of frames is one tick short.",
    },
    {
        "id": "t8-clock-frame-carry-not-consumed",
        "file": CLOCK,
        "old": "        frame_carry -= refresh_hz;\n",
        "new": "",
        "targets": ["test_kernel_clock"],
        "why": "paying the extra tick without clearing the debt pays it every frame after "
        "the first, so the clock runs fast by one tick per frame.",
    },
    {
        "id": "t8-clock-frame-rate-change-keeps-the-old-remainder",
        "file": CLOCK,
        "old": "        carry_rate = refresh_hz;\n        frame_carry = 0u;\n",
        "new": "        carry_rate = refresh_hz;\n",
        "targets": ["test_kernel_clock"],
        "why": "the remainder is in units of 1/refresh, so one left over from 60 Hz is "
        "meaningless at 50 Hz. Keeping it mixes denominators and adds a spurious tick.",
    },
    {
        "id": "t8-clock-reset-keeps-the-stall-remainder",
        "file": CLOCK,
        "old": "    stall_carry = 0u;\n    (void)publish_tick_count();\n    "
        "pthread_mutex_unlock(&clock_lock);\n}\n",
        "new": "    (void)publish_tick_count();\n    pthread_mutex_unlock(&clock_lock);\n}\n",
        "targets": ["test_kernel_clock"],
        "why": "a reset is a fresh guest. A leftover sub-tick from the previous run makes "
        "the first stall of the next run one tick longer than the same stall in a "
        "fresh process, which breaks determinism between runs.",
    },
    {
        "id": "t8-clock-stall-reads-the-wrong-argument",
        "file": CLOCK,
        "old": "    if (frame == NULL || !kernel_frame_arg(frame, 0u, &microseconds)) {",
        "new": "    if (frame == NULL || !kernel_frame_arg(frame, 1u, &microseconds)) {",
        "targets": ["test_kernel_clock"],
        "why": "the one stdcall argument sits at slot 0. Reading slot 1 reads whatever "
        "the caller had above it, which is a plausible-looking stall length.",
    },
    {
        "id": "t8-clock-stall-handler-does-not-stall",
        "file": CLOCK,
        "old": "    (void)kernel_clock_stall_us(microseconds);\n    return 0u;\n}",
        "new": "    (void)microseconds;\n    return 0u;\n}",
        "targets": ["test_kernel_clock"],
        "why": "the registered handler can be bound correctly and do nothing: every call "
        "to the C function is right and the guest's stalls are silently free.",
    },
    {
        "id": "t8-clock-stall-bound-to-the-wrong-ordinal",
        "file": CLOCK,
        "old": "    if (kernel_hle_register(151u, hle_stall_execution_processor)) {",
        "new": "    if (kernel_hle_register(150u, hle_stall_execution_processor)) {",
        "targets": ["test_kernel_clock"],
        "why": "the handler can be correct and bound to the wrong number, which leaves 151 "
        "a stub returning 0 that every title call site accepts.",
    },
    # ------------------------------------------------------------------ 228, the wall clock
    {
        "id": "t8-systime-binding-wrong-ordinal",
        "file": EVENT,
        "old": "#define ORD_NtSetSystemTime 228u",
        "new": "#define ORD_NtSetSystemTime 229u",
        "targets": ["test_kernel_system_time"],
        "why": "a handler bound to the wrong ordinal leaves 228 an unimplemented stub "
        "returning 0, which reads as success to the one XONLINE caller.",
    },
    {
        "id": "t8-systime-nonnull-oldtime-accepted",
        "file": EVENT,
        "old": '    if (args[1] != 0u) {\n        kernel_hle_log()("kernel: NtSetSystemTime with '
        "a non-NULL OldTime",
        "new": '    if (args[1] != 0u && false) {\n        kernel_hle_log()("kernel: '
        "NtSetSystemTime with a non-NULL OldTime",
        "targets": ["test_kernel_system_time"],
        "why": "OldTime is unmeasured, and the call would succeed without writing the "
        "previous time the caller asked for, leaving its buffer uninitialised.",
    },
    {
        "id": "t8-systime-oldtime-refusal-wrong-status",
        "file": EVENT,
        "old": "                         (unsigned)args[1]);\n        return "
        "STATUS_NOT_IMPLEMENTED;",
        "new": "                         (unsigned)args[1]);\n        return "
        "STATUS_INVALID_PARAMETER;",
        "targets": ["test_kernel_system_time"],
        "why": "NOT_IMPLEMENTED says 'a mode this model lacks' and INVALID_PARAMETER says "
        "'the caller was wrong'. A guest that retries on one and gives up on the other "
        "behaves differently.",
    },
    {
        "id": "t8-systime-high-dword-read-from-the-low-address",
        "file": EVENT,
        "old": "        !kernel_guest_read_u32((kernel_guest_ptr)(args[0] + 4u), &high)) {",
        "new": "        !kernel_guest_read_u32((kernel_guest_ptr)(args[0] + 0u), &high)) {",
        "targets": ["test_kernel_system_time"],
        "why": "a LARGE_INTEGER is two dwords, and reading the low one twice builds a "
        "time in 1601 that is perfectly well formed.",
    },
    {
        "id": "t8-systime-high-dword-shift-wrong",
        "file": EVENT,
        "old": "    const uint64_t new_time = (uint64_t)low | ((uint64_t)high << 32);",
        "new": "    const uint64_t new_time = (uint64_t)low | ((uint64_t)high << 31);",
        "targets": ["test_kernel_system_time"],
        "why": "a one-bit shift error overlaps the halves and only differs when the high "
        "dword is nonzero and odd, which is every real date.",
    },
    {
        "id": "t8-systime-zero-time-accepted",
        "file": EVENT,
        "old": "    if (new_time == 0u || (new_time >> 63) != 0u) {",
        "new": "    if ((new_time >> 63) != 0u) {",
        "targets": ["test_kernel_system_time"],
        "why": "the XONLINE caller passes 0 when a reply carried no time. Accepting it "
        "rebases the guest wall clock to 1601 on every such reply.",
    },
    {
        "id": "t8-systime-negative-time-accepted",
        "file": EVENT,
        "old": "    if (new_time == 0u || (new_time >> 63) != 0u) {",
        "new": "    if (new_time == 0u || (new_time >> 62) != 0u) {",
        "targets": ["test_kernel_system_time"],
        "why": "a negative LARGE_INTEGER is invalid in NT. Testing one bit too low also "
        "refuses valid times above 2^62, and the sign test must be on bit 63 exactly.",
    },
    {
        "id": "t8-systime-sign-test-dropped",
        "file": EVENT,
        "old": "    if (new_time == 0u || (new_time >> 63) != 0u) {",
        "new": "    if (new_time == 0u) {",
        "targets": ["test_kernel_system_time"],
        "why": "without the sign test a negative time casts to a huge unsigned value and "
        "the offset computation wraps, giving a clock in an unrelated year.",
    },
    {
        "id": "t8-systime-offset-sign-flipped",
        "file": EVENT,
        "old": "        system_time_offset = (int64_t)(new_time - host_units);",
        "new": "        system_time_offset = (int64_t)(host_units - new_time);",
        "targets": ["test_kernel_system_time"],
        "why": "with the sign flipped the guest clock runs backwards from the set time "
        "relative to the host, so setting a later time moves the clock EARLIER.",
    },
    {
        "id": "t8-systime-monotonic-guard-not-restarted",
        "file": EVENT,
        "old": "         * by years would crawl at one unit per read for years. */\n        "
        "last_time = 0u;",
        "new": "         * by years would crawl at one unit per read for years. */",
        "targets": ["test_kernel_system_time"],
        "why": "a deliberate set backward must take effect at once. Without restarting the "
        "guard the clamp pins the clock at last+1 per read and a set backward by years "
        "crawls for years.",
    },
    {
        "id": "t8-systime-set-not-counted",
        "file": EVENT,
        "old": "        system_time_set_count++;\n",
        "new": "",
        "targets": ["test_kernel_system_time"],
        "why": "the set count is the only evidence a guest ever rebased its clock, which "
        "the run report relies on.",
    },
    {
        "id": "t8-systime-offset-not-applied-to-reads",
        "file": EVENT,
        "old": "        units = host_units + (uint64_t)system_time_offset;",
        "new": "        units = host_units;",
        "targets": ["test_kernel_system_time"],
        "why": "the set is accepted, logged and counted, and the clock a title reads "
        "never changes: a rebase that does nothing and says it worked.",
    },
    # ------------------------------------------------------------------ 142 / 139, the bracket
    {
        "id": "t8-fpstate-limit-off-by-one",
        "file": FPSTATE,
        "old": "    if (table_count >= KERNEL_FPSTATE_MAX_ACTIVE) {",
        "new": "    if (table_count > KERNEL_FPSTATE_MAX_ACTIVE) {",
        "targets": ["test_kernel_fpstate"],
        "why": "the table holds exactly MAX_ACTIVE entries. With `>` the 257th save "
        "writes one entry past the array instead of being refused.",
    },
    {
        "id": "t8-fpstate-area-size-too-small",
        "file": FPSTATE,
        "old": "    if (!kernel_guest_range_readable(area, KERNEL_FPSTATE_AREA_BYTES)) {",
        "new": "    if (!kernel_guest_range_readable(area, 1u)) {",
        "targets": ["test_kernel_fpstate"],
        "why": "the real kernel writes 0x20 bytes. Probing only the first byte accepts an "
        "area that straddles the end of a mapping and hides the fault the real kernel "
        "would take.",
    },
    {
        "id": "t8-fpstate-unreadable-area-not-counted",
        "file": FPSTATE,
        "old": "        pthread_mutex_lock(&table_mutex);\n        note_anomaly();\n"
        "        pthread_mutex_unlock(&table_mutex);\n"
        '        kernel_hle_log()("kernel: KeSaveFloatingPointState(%#x) REFUSED: the save area '
        'is not "',
        "new": "        pthread_mutex_lock(&table_mutex);\n"
        "        pthread_mutex_unlock(&table_mutex);\n"
        '        kernel_hle_log()("kernel: KeSaveFloatingPointState(%#x) REFUSED: the save area '
        'is not "',
        "targets": ["test_kernel_fpstate"],
        "why": "a refusal nobody counts is a refusal nobody notices: the anomaly count is "
        "what the run report shows for a title that saved into unmapped memory.",
    },
    {
        "id": "t8-fpstate-unreadable-area-wrong-status",
        "file": FPSTATE,
        "old": "        return STATUS_ACCESS_VIOLATION;",
        "new": "        return STATUS_INVALID_PARAMETER;",
        "targets": ["test_kernel_fpstate"],
        "why": "ACCESS_VIOLATION is what the real kernel raises writing the area. A guest "
        "handler that dispatches on the code behaves differently for the wrong one.",
    },
    {
        "id": "t8-fpstate-full-table-wrong-status",
        "file": FPSTATE,
        "old": "        return STATUS_INSUFFICIENT_RESOURCES;",
        "new": "        return STATUS_ACCESS_VIOLATION;",
        "targets": ["test_kernel_fpstate"],
        "why": "running out of table entries is a resource failure, not a fault, and the "
        "two are handled differently by any caller that checks.",
    },
    {
        "id": "t8-fpstate-full-table-not-counted",
        "file": FPSTATE,
        "old": "    if (table_count >= KERNEL_FPSTATE_MAX_ACTIVE) {\n        note_anomaly();\n",
        "new": "    if (table_count >= KERNEL_FPSTATE_MAX_ACTIVE) {\n",
        "targets": ["test_kernel_fpstate"],
        "why": "a title leaking saves until the table fills is exactly what the anomaly "
        "count exists to expose, and the refusal must register in it.",
    },
    {
        "id": "t8-fpstate-bad-argument-returns-success",
        "file": FPSTATE,
        "old": '    if (!read_area_argument(context, "KeSaveFloatingPointState", &area)) {\n'
        "        return STATUS_INVALID_PARAMETER;",
        "new": '    if (!read_area_argument(context, "KeSaveFloatingPointState", &area)) {\n'
        "        return STATUS_SUCCESS;",
        "targets": ["test_kernel_fpstate"],
        "why": "a save whose argument could not be read did not happen. Reporting success "
        "tells the guest the FPU state is saved when nothing was recorded.",
    },
    {
        "id": "t8-fpstate-argument-from-the-wrong-slot",
        "file": FPSTATE,
        "old": "    if (!kernel_frame_arg((const kernel_call_frame *)context, 0u, area)) {",
        "new": "    if (!kernel_frame_arg((const kernel_call_frame *)context, 1u, area)) {",
        "targets": ["test_kernel_fpstate"],
        "why": "both ordinals take the area at slot 0. Slot 1 reads the word above the "
        "argument, so every save and restore is recorded against the wrong address.",
    },
    {
        "id": "t8-fpstate-repeated-save-not-detected",
        "file": FPSTATE,
        "old": "    const bool repeated = find_latest(area) != table_count;",
        "new": "    const bool repeated = find_latest(area) != table_count && false;",
        "targets": ["test_kernel_fpstate"],
        "why": "saving into an area that already holds a live save overwrites state the "
        "matching restore still needs. Silence there hides a title bug.",
    },
    {
        "id": "t8-fpstate-save-recorded-under-no-thread",
        "file": FPSTATE,
        "old": "    table[table_count].owner = pthread_self();",
        "new": "    table[table_count].owner = (pthread_t)0;",
        "targets": ["test_kernel_fpstate"],
        "why": "the owner is the calling host thread. Recording nobody makes every "
        "per-thread depth read zero while the total depth looks right.",
    },
    {
        "id": "t8-fpstate-save-records-the-wrong-area",
        "file": FPSTATE,
        "old": "    table[table_count].area = area;",
        "new": "    table[table_count].area = area + 4u;",
        "targets": ["test_kernel_fpstate"],
        "why": "recording the wrong address means the matching restore finds no save: "
        "every bracket would report an anomaly, or if both sides share the bias, "
        "would pass.",
    },
    {
        "id": "t8-fpstate-lookup-ignores-the-owner",
        "file": FPSTATE,
        "old": "        if (pthread_equal(table[index - 1u].owner, self) && table[index - "
        "1u].area == area) {",
        "new": "        if ((pthread_equal(table[index - 1u].owner, self) || true) && table[index "
        "- 1u].area == area) {",
        "targets": ["test_kernel_fpstate"],
        "why": "two threads saving into the same address are two independent brackets. "
        "Matching across threads lets one thread restore another's save and hides "
        "the missing restore of its own.",
    },
    {
        "id": "t8-fpstate-latest-of-thread-ignores-the-owner",
        "file": FPSTATE,
        "old": "        if (pthread_equal(table[index - 1u].owner, self)) {\n            return "
        "index - 1u;",
        "new": "        if (pthread_equal(table[index - 1u].owner, self) || true) {\n            "
        "return index - 1u;",
        "targets": ["test_kernel_fpstate"],
        "why": "THE SURVIVOR OF THE FIRST HAND SWEEP (closed by test_order_is_judged_per_"
        "thread): the out-of-order judgement compares against the latest save of ANY "
        "thread, so one thread saving after another makes the first thread's correct "
        "restore look out of order.",
    },
    {
        "id": "t8-fpstate-out-of-order-restore-not-detected",
        "file": FPSTATE,
        "old": "    const bool out_of_order = found != find_latest_of_thread();",
        "new": "    const bool out_of_order = found != find_latest_of_thread() && false;",
        "targets": ["test_kernel_fpstate"],
        "why": "restoring an older save before a newer one corrupts the nesting. It is "
        "honoured but must be reported, or a mismatched bracket is silent.",
    },
    {
        "id": "t8-fpstate-unmatched-restore-not-counted",
        "file": FPSTATE,
        "old": "    if (found == table_count) {\n        note_anomaly();\n",
        "new": "    if (found == table_count) {\n",
        "targets": ["test_kernel_fpstate"],
        "why": "a restore with no save is the signature of a title that restores twice or "
        "on the wrong thread, and the count is how the run report shows it.",
    },
    {
        "id": "t8-fpstate-restore-leaves-the-entry",
        "file": FPSTATE,
        "old": "    table_count--;\n",
        "new": "",
        "targets": ["test_kernel_fpstate"],
        "why": "a restore that does not shrink the table leaks a live save forever, so "
        "every bracket raises the depth and the 257th save is refused.",
    },
    {
        "id": "t8-fpstate-restore-does-not-close-the-gap",
        "file": FPSTATE,
        "old": "    memmove(&table[found], &table[found + 1u], (table_count - found - 1u) * "
        "sizeof(table[0]));\n",
        "new": "",
        "targets": ["test_kernel_fpstate"],
        "why": "removing an out-of-order entry must close the gap. Without the move the "
        "last entry is dropped instead of the restored one, so the wrong save stays live.",
    },
    {
        "id": "t8-fpstate-depth-counts-every-thread",
        "file": FPSTATE,
        "old": "        if (pthread_equal(table[index].owner, self)) {\n            depth++;",
        "new": "        if (pthread_equal(table[index].owner, self) || true) {\n            "
        "depth++;",
        "targets": ["test_kernel_fpstate"],
        "why": "the per-thread depth is what a thread's own bracket accounting reads. "
        "Counting everyone's saves makes a thread with none look nested.",
    },
    {
        "id": "t8-fpstate-reset-keeps-the-anomaly-count",
        "file": FPSTATE,
        "old": "    anomalies = 0u;\n",
        "new": "",
        "targets": ["test_kernel_fpstate"],
        "why": "a reset is a fresh kernel. Anomalies from a previous guest carried into "
        "the next run make a clean run report problems it does not have.",
    },
    {
        "id": "t8-fpstate-register-keeps-stale-saves",
        "file": FPSTATE,
        "old": "    /* A fresh registration is a fresh kernel: the saves belong to a guest that "
        "is gone. */\n"
        "    kernel_fpstate_reset();",
        "new": "    /* A fresh registration is a fresh kernel: the saves belong to a guest that "
        "is gone. */",
        "targets": ["test_kernel_fpstate"],
        "why": "saves of a guest that no longer exists would count against the next "
        "guest's depth and table limit.",
    },
    {
        "id": "t8-fpstate-handlers-bound-to-each-others-ordinals",
        "file": FPSTATE,
        "old": "    if (kernel_hle_register(ORD_KeSaveFloatingPointState, hle_save)) {\n"
        "        registered++;\n"
        "    }\n"
        "    if (kernel_hle_register(ORD_KeRestoreFloatingPointState, hle_restore)) {",
        "new": "    if (kernel_hle_register(ORD_KeSaveFloatingPointState, hle_restore)) {\n"
        "        registered++;\n"
        "    }\n"
        "    if (kernel_hle_register(ORD_KeRestoreFloatingPointState, hle_save)) {",
        "targets": ["test_kernel_fpstate"],
        "why": "swapping the two bindings keeps both registered and counted (a one-sided "
        "swap does not compile under -Werror, the function goes unused) and turns "
        "every save into a restore: the table never fills and every restore is an anomaly.",
    },
]
