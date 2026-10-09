"""TSFP-LOCAL (patch 07): manual-override calls must not unbalance braces.

`_fixup_icall_esp_save` wraps every stack-safe indirect call in

    { uint32_t _icall_esp = g_esp;
      ... argument pushes ...
      PUSH32(esp, <retva>); RECOMP_ICALL_SAFE(<target>, _icall_esp);
    }

by scanning backwards from the call for the run of argument pushes. The scan
has to recognise a *completed* call and stop, because a completed call is
also emitted as a line beginning `PUSH32(esp, <retva>);`. It tested for the
literal `/* call 0x`, which an ordinary direct call carries -- but a call to
a function the project implements by hand carries `/* manual call 0x`, and
that spelling fell through to the generic "starts with PUSH32(esp," arm and
was absorbed as an argument push.

The consequence is not a subtle miscompile. The open brace is emitted from a
deduplicated set of insertion points while the close brace is emitted once
per call, so two manual calls in one function yield ONE `{` and TWO `}` --
`_icall_esp undeclared` and then `expected identifier or '(' before '}'`.

This matters disproportionately because `--manual-functions` /
`--exclude-manual` is the weak-symbol override mechanism: it is how a
hand-written C function replaces its lifted counterpart, which is the whole
of the decompilation phase. The bug fires exactly on the path we depend on,
and it fires as a build failure in a 2.5-million-line generated tree.

The existing upstream tests cannot catch it: test_manual_call_dispatch.py
exercises a single manual call, and a single one is balanced.
"""
import unittest

from .translator import _fixup_icall_esp_save

MANUAL_A = ("    PUSH32(esp, 0x00120005u); RECOMP_ICALL_SAFE(0x001E9100u,"
            " _icall_esp); /* manual call 0x001E9100 */")
MANUAL_B = ("    PUSH32(esp, 0x0012000Au); RECOMP_ICALL_SAFE(0x001E9200u,"
            " _icall_esp); /* manual call 0x001E9200 */")
DIRECT = ("    PUSH32(esp, 0x00120010u); RECOMP_ABI_CALL(0x00130000u,"
          " sub_00130000); /* call 0x00130000 */")
INDIRECT = ("    { uint32_t _icall_target = eax;"
            " PUSH32(esp, 0x00120020u);"
            " RECOMP_ICALL_SAFE_AT(_icall_target, _icall_esp, 0x0012001Eu); }"
            " /* indirect call */")
ARG = "    PUSH32(esp, eax);"
COMPUTE = "    eax = MEM32(esi + 4);"


def _braces(lines):
    out = _fixup_icall_esp_save(list(lines))
    text = "\n".join(out)
    return text.count("{ uint32_t _icall_esp = g_esp;"), sum(
        1 for ln in out if ln.strip() == "}"), out


class ManualCallBraceBalanceTest(unittest.TestCase):

    def assertBalanced(self, lines, msg):
        opens, closes, out = _braces(lines)
        self.assertEqual(opens, closes,
                         f"{msg}: {opens} open vs {closes} close\n"
                         + "\n".join(out))

    def test_two_manual_calls_in_a_row(self):
        # The minimal reproducer. Before the fix: 1 open, 2 close.
        self.assertBalanced([MANUAL_A, MANUAL_B], "two manual calls")

    def test_manual_calls_separated_by_an_argument_push(self):
        self.assertBalanced([MANUAL_A, ARG, MANUAL_B], "manual, arg, manual")

    def test_manual_calls_separated_by_interleaved_computation(self):
        self.assertBalanced([MANUAL_A, COMPUTE, MANUAL_B],
                            "manual, compute, manual")

    def test_manual_call_followed_by_an_ordinary_indirect_call(self):
        self.assertBalanced([MANUAL_A, ARG, INDIRECT], "manual then indirect")

    def test_direct_then_manual_then_indirect(self):
        self.assertBalanced([DIRECT, MANUAL_A, INDIRECT],
                            "direct, manual, indirect")

    def test_three_manual_calls(self):
        self.assertBalanced([MANUAL_A, MANUAL_B, MANUAL_A], "three manual")

    def test_a_manual_call_is_not_absorbed_as_an_argument_push(self):
        # The mechanism, not just the symptom: the save for the SECOND call
        # must sit between the two calls, not before the first. Putting it
        # before the first would rewind g_esp over a call that already ran.
        _, _, out = _braces([MANUAL_A, MANUAL_B])
        first_open = next(i for i, ln in enumerate(out) if "_icall_esp = g_esp" in ln)
        first_call = next(i for i, ln in enumerate(out) if "0x001E9100u" in ln)
        second_open = next(i for i, ln in enumerate(out)
                           if "_icall_esp = g_esp" in ln and i > first_call)
        self.assertLess(first_open, first_call)
        self.assertGreater(second_open, first_call)

    def test_the_single_manual_call_case_still_works(self):
        # Negative control: the shape upstream's own test covers was already
        # balanced, and must stay that way.
        self.assertBalanced([ARG, MANUAL_A], "one manual call")
        opens, _, out = _braces([ARG, MANUAL_A])
        self.assertEqual(opens, 1)
        # And the save still goes before the argument push, not after it.
        self.assertIn("_icall_esp = g_esp", out[0])


if __name__ == "__main__":
    unittest.main()
