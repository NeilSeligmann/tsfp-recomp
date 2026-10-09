"""Mutations for the T505 fault report: the handler's evidence, its names and its wiring.

Judged by `tests/c/test_host_fault_report.c` (ctest `host_fault_report`, a deliberate fault in
fake lifted functions), `tests/c/test_host_report.c` (exact report bytes with fake names) and
`tests/test_host_fault_report.py` (the T485 sixteen instruction source through the real probe,
which needs the private XBE and the alias chunk, so those mutants report UNBUILT or skip on a
fresh clone). A fault report that names the wrong function, or silently names nothing, is worse
than the bare address it replaced, so each mutant below is a plausible slip of that kind.

Not mutated, equivalent or unreachable without a second fault: the host `rbp` chain's
`pair[0] <= rbp` climb check (a real chain ends at a zero return address first), the frame caps
(32 and 16, a larger loop bound only matters past a chain no test builds) and the UNSIZED_REACH
bound (hand written assembly has no symbol of size 0 in the test).
"""

RUNTIME = "src/host/host_runtime.c"
REPORT = "src/host/host_report.c"
SYMBOLS = "src/host/host_symbols.c"
PROBE_SOURCE = "tests/c/shader_compiler_probe.c"
FAULT = "test_host_fault_report"
PYTEST = "pytest:tests/test_host_fault_report.py"

MUTATIONS: list[dict] = [
    {
        "id": "hostfault-handler-skips-the-host-chain",
        "file": RUNTIME,
        "old": "    if (context != NULL) {\n        record_host_frames(",
        "new": "    if (context == NULL) {\n        record_host_frames(",
        "targets": [FAULT],
        "why": "without RIP and the rbp chain the report is the bare address again, and "
        "nothing else in the run would say so.",
    },
    {
        "id": "hostfault-handler-skips-the-guest-chain",
        "file": RUNTIME,
        "old": "    record_guest_frames(&t_stop);\n    errno = saved_errno;",
        "new": "    if (0) record_guest_frames(&t_stop);\n    errno = saved_errno;",
        "targets": [FAULT],
        "why": "the guest chain is what names the title's RtlFreeHeap callers.",
    },
    {
        "id": "hostfault-guest-chain-does-not-require-climbing",
        "file": RUNTIME,
        "old": "        if (pair[0] <= ebp) {",
        "new": "        if (pair[0] < ebp) {",
        "targets": [FAULT],
        "why": "a guest frame that points at itself would repeat its return address until "
        "the cap, burying the real chain in sixteen copies of one line.",
    },
    {
        "id": "hostfault-unchecked-read-of-a-wild-pointer",
        "file": RUNTIME,
        "old": "    return process_vm_readv(kernel_host_pid(), &local, 1, &remote, 1, 0) =="
        " (ssize_t)size;",
        "new": "    (void)local;\n    (void)remote;\n"
        "    memcpy(out, (const void *)address, size);\n    return true;",
        "targets": [FAULT],
        "why": "a fault usually leaves wild frame pointers, and a plain read of one would fault "
        "inside the handler and lose the report and the process.",
    },
    {
        "id": "hostfault-later-stop-inherits-the-evidence",
        "file": RUNTIME,
        "old": '    clear_fault_evidence(&t_stop);\n    t_stop.detail = detail ? detail : "";',
        "new": '    t_stop.detail = detail ? detail : "";',
        "targets": [FAULT],
        "why": "a run that faults, is caught by a scope and later stops for another reason "
        "would carry the old chain into a report that is not about a fault.",
    },
    {
        "id": "hostfault-stack-top-not-recorded",
        "file": RUNTIME,
        "old": "        stop->fault_stack_top = top;",
        "new": "        (void)top;",
        "targets": [FAULT],
        "why": "a frameless libc leaf hides its caller from the rbp chain, the stack top word "
        "is the only trace of it.",
    },
    {
        "id": "hostfault-rip-is-the-stack-pointer",
        "file": RUNTIME,
        "old": "    stop->fault_rip = (uintptr_t)context->uc_mcontext.gregs[REG_RIP];",
        "new": "    stop->fault_rip = (uintptr_t)context->uc_mcontext.gregs[REG_RSP];",
        "targets": [FAULT],
        "why": "a register mix up still prints a plausible address and names nothing right.",
    },
    {
        "id": "hostfault-chain-starts-from-the-stack-pointer",
        "file": RUNTIME,
        "old": "    uintptr_t rbp = (uintptr_t)context->uc_mcontext.gregs[REG_RBP];",
        "new": "    uintptr_t rbp = (uintptr_t)context->uc_mcontext.gregs[REG_RSP];",
        "targets": [FAULT],
        "why": "walking from rsp instead of rbp reads the wrong words as frames.",
    },
    {
        "id": "hostfault-guest-ebp-not-kept",
        "file": RUNTIME,
        "old": "    stop->fault_guest_ebp = ebp;",
        "new": "    (void)ebp;",
        "targets": [FAULT],
        "why": "the guest ebp at the fault is what lets a reader check the chain by hand.",
    },
    {
        "id": "hostfault-guest-reader-never-registered",
        "file": RUNTIME,
        "old": "    atomic_store_explicit(&guest_ebp_reader, reader, memory_order_release);",
        "new": "    (void)reader;",
        "targets": [FAULT],
        "why": "an unregistered reader degrades to a host-only report without any error.",
    },
    {
        "id": "hostfault-report-prints-for-every-stop",
        "file": REPORT,
        "old": "    if (stop->reason != HOST_STOP_FAULT) {\n        return;\n    }\n"
        "    char text[160];",
        "new": "    if (0) {\n        return;\n    }\n    char text[160];",
        "targets": ["test_host_report", FAULT],
        "why": "frames of an unrelated stop would be printed as if they were evidence.",
    },
    {
        "id": "hostfault-report-stack-top-always-offered",
        "file": REPORT,
        "old": '        (!have_rip_name || strncmp(text, "sub_", 4u) != 0) &&',
        "new": "        true &&",
        "targets": ["test_host_report"],
        "why": "offering the stack top as a caller when RIP is already in lifted code adds a "
        "misleading line to every ordinary fault.",
    },
    {
        "id": "hostfault-report-guest-frames-named-as-host-addresses",
        "file": REPORT,
        "old": "            const bool named = names->guest_function != NULL &&\n"
        "                               names->guest_function(stop->fault_guest_frames[i], text, "
        "sizeof(text));",
        "new": "            const bool named = names->host_symbol != NULL &&\n"
        "                               names->host_symbol(stop->fault_guest_frames[i], text, "
        "sizeof(text));",
        "targets": ["test_host_report"],
        "why": "a guest VA looked up as a host address names nothing or the wrong function.",
    },
    {
        "id": "hostfault-report-host-frames-named-as-guest-addresses",
        "file": REPORT,
        "old": "            const bool named = names->host_symbol != NULL &&\n"
        "                               names->host_symbol(stop->fault_host_frames[i], text, "
        "sizeof(text));",
        "new": "            const bool named = names->guest_function != NULL &&\n"
        "                               names->guest_function("
        "(uint32_t)stop->fault_host_frames[i], text, sizeof(text));",
        "targets": ["test_host_report"],
        "why": "the mirror slip: a host return address truncated to a guest VA.",
    },
    {
        "id": "hostfault-stop-report-omits-the-frames",
        "file": REPORT,
        "old": "    host_report_fault_frames(out, stop, names);",
        "new": "    (void)names;",
        "targets": ["test_host_report"],
        "why": "the calling thread's stop would print the bare address again.",
    },
    {
        "id": "hostfault-thread-stop-report-omits-the-frames",
        "file": REPORT,
        "old": "    host_report_fault_frames(out, &record->stop, names);",
        "new": "    (void)names;",
        "targets": ["test_host_report"],
        "why": "guest threads are where the shader compiler faults, so their record is the "
        "one that must carry the chain.",
    },
    {
        "id": "hostfault-symbols-ignore-the-load-bias",
        "file": SYMBOLS,
        "old": "                .address = (uintptr_t)symbol->st_value + load_bias,",
        "new": "                .address = (uintptr_t)symbol->st_value + (load_bias & 0u),",
        "targets": [FAULT],
        "why": "the host is a PIE, so an unbiased table names nothing at the addresses a "
        "signal context reports.",
    },
    {
        "id": "hostfault-symbols-host-lookup-skips-the-exact-start",
        "file": SYMBOLS,
        "old": "        if (table->functions[middle].address <= address) {",
        "new": "        if (table->functions[middle].address < address) {",
        "targets": [FAULT],
        "why": "an off by one that names the neighbouring function for an address at a start.",
    },
    {
        "id": "hostfault-symbols-guest-lookup-skips-the-exact-start",
        "file": SYMBOLS,
        "old": "        if (table->guests[middle].guest_va <= guest_va) {",
        "new": "        if (table->guests[middle].guest_va < guest_va) {",
        "targets": [FAULT],
        "why": "a return address at a function's first byte would be named after the previous "
        "function.",
    },
    {
        "id": "hostfault-symbols-name-anything-far-from-code",
        "file": SYMBOLS,
        "old": "    if (guest_va - function->guest_va >= GUEST_FUNCTION_REACH) {",
        "new": "    if (0) {",
        "targets": [FAULT, PYTEST],
        "why": "the guest stack sentinel 0xDEAD0000 and heap addresses would be named after "
        "the last lifted function, which reads as a real caller.",
    },
    {
        "id": "hostfault-symbols-prefix-match-counts-as-a-guest-name",
        "file": SYMBOLS,
        "old": '    if (strlen(name) != GUEST_NAME_LENGTH || strncmp(name, "sub_", 4u) != 0) {',
        "new": '    if (strlen(name) < GUEST_NAME_LENGTH || strncmp(name, "sub_", 4u) != 0) {',
        "targets": [FAULT],
        "why": "a probe wrap or helper named sub_XXXXXXXX_something would register as a guest "
        "function and steal the lookup.",
    },
    {
        "id": "hostfault-symbols-data-symbols-treated-as-functions",
        "file": SYMBOLS,
        "old": "            if (ELF64_ST_TYPE(symbol->st_info) != STT_FUNC || "
        "symbol->st_shndx == SHN_UNDEF ||",
        "new": "            if (ELF64_ST_TYPE(symbol->st_info) != STT_OBJECT || "
        "symbol->st_shndx == SHN_UNDEF ||",
        "targets": [FAULT],
        "why": "only function symbols may name a code address.",
    },
    {
        "id": "hostfault-main-never-registers-the-guest-ebp",
        "file": "src/host/main.c",
        "old": "    host_run_set_fault_guest_ebp(read_guest_ebp);",
        "new": "    (void)read_guest_ebp;",
        "targets": ["tsfp_shader_probe", PYTEST],
        "why": "the host would print a host-only report and no test of the pieces would notice.",
    },
    {
        "id": "hostfault-main-gives-the-report-no-symbols",
        "file": "src/host/main.c",
        "old": "    .host_symbol = host_symbols_describe,",
        "new": "    .host_symbol = NULL,",
        "targets": ["tsfp_shader_probe", PYTEST],
        "why": "raw addresses only: the report would look healthy and name nothing.",
    },
    {
        "id": "hostfault-probe-heap-free-not-logged",
        "file": PROBE_SOURCE,
        "old": '        fprintf(stderr, "heap free #%u ptr=%#x", tls.free_count, pointer);\n'
        "        log_heap_callers(g_esp);",
        "new": "        (void)pointer;",
        "targets": ["tsfp_shader_probe", PYTEST],
        "why": "the heap log's point is the free that faulted.",
    },
    {
        "id": "hostfault-probe-heap-log-on-by-default",
        "file": PROBE_SOURCE,
        "old": '    if (config.heap_log != 0u) {\n        fprintf(stderr, "heap alloc',
        "new": '    if (config.heap_log == 0u) {\n        fprintf(stderr, "heap alloc',
        "targets": ["tsfp_shader_probe", PYTEST],
        "why": "an opt-in log that is on by default floods every probe run.",
    },
    {
        "id": "hostfault-probe-alloc-caller-read-after-the-pop",
        "file": PROBE_SOURCE,
        "old": "        log_heap_callers(esp);",
        "new": "        log_heap_callers(g_esp);",
        "targets": ["tsfp_shader_probe", PYTEST],
        "why": "after RET 12 the stack pointer has moved past the return address, so the "
        "logged caller would be the wrong word.",
    },
]
