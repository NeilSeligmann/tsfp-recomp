/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The callee stub contract, shared by the subject and (by mirroring) the oracle.
 *
 * Ordinary callees are replaced on both sides with one synthetic behaviour.
 * A separately proved leaf stack probe executes on both sides because its page
 * reads and frame allocation are part of the caller contract. Its proof lives
 * in stackprobe.py and stackprobe_contract.h; it has a separate PASS plan/count. These three constants and the esp rule below are the entire
 * contract; `tools/harness/callstub.py` mirrors them and
 * `tests/test_harness.py::test_the_two_sides_agree_on_the_stub_contract` parses
 * THIS FILE and fails if the two ever drift apart. A silent drift would make
 * every stubbed verdict meaningless while still reporting AGREE, so it is
 * checked mechanically rather than by convention.
 */
#ifndef TOOLS_HARNESS_CALL_STUB_H
#define TOOLS_HARNESS_CALL_STUB_H

#include <stdint.h>

/* eax is the return value. A distinctive constant, never 0: zero is what the
 * lifter's own unresolved-indirect-call path produces, and a stub that also
 * returned 0 would be indistinguishable from that failure mode. */
#define HARNESS_STUB_EAX 0xA5B6C7D8u

/* ecx and edx are caller-saved scratch under both cdecl and stdcall, so a real
 * callee is free to destroy them. The stub destroys them with fixed values,
 * identically on both sides, which keeps any caller that re-reads them
 * deterministic instead of accidentally agreeing because the stub happened to
 * preserve what was already there. */
#define HARNESS_STUB_ECX 0xC1C1C1C1u
#define HARNESS_STUB_EDX 0xD2D2D2D2u

void harness_stub_reset(void);
int harness_stub_add(uint32_t va, uint32_t pop_bytes);
int harness_passthrough_add(uint32_t va, uint32_t caller_va);
int harness_stackprobe_supported(void);
extern uint32_t g_passthrough_applied, g_passthrough_fault;
uint32_t harness_stub_count(void);
/** fn may be NULL only while g_stub_strict is set: strict calls consult the plan,
 * count a missing target, and execute only ordinary stubs or the separately
 * approved stack-probe entry. A passthrough requires a non-NULL matching fn. */
void harness_stub_call(uint32_t va, void (*fn)(void));
extern int g_stub_strict;
extern uint32_t g_stub_applied, g_stub_misses, g_stub_miss_va;

/* ebx, esi, edi, ebp are callee-saved: the stub leaves them completely alone.
 *
 * esp: the lifted caller has ALREADY pushed the guest return address (the
 * generated code emits `PUSH32(esp, retaddr); RECOMP_ABI_CALL(...)`), so the
 * stub must undo that push the way the callee's own `ret` would, plus pop
 * `pop_bytes` of stdcall arguments:
 *
 *     g_esp += 4 + pop_bytes;
 *
 * The 4-byte return-address store itself is left in guest memory, because the
 * hardware performs that store too and it must appear in both write-sets.
 */

#endif /* TOOLS_HARNESS_CALL_STUB_H */
