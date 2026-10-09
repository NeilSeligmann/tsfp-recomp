# SPDX-License-Identifier: GPL-3.0-or-later
"""Mutation set for the kernel arity oracle (src/xbox/kernel_arity_oracle.c and its suite).

WHY THIS SET NEEDS AN EXTERNAL ORACLE ARGUMENT, like the crypto set and unlike most.

The table under test is DERIVED, not measured, so `test_arity_oracle` could agree with
itself all day. What makes the kills below meaningful is that every pinned number also
appears in a second place with a separate derivation: `ABI_TABLE` in
`src/host/kernel_thunk.c` hand-counted it at this image's own call sites. Where the two
agree, a mutation that breaks one breaks the agreement, and `tests/test_arity_oracle.py`
says so by name. Where the suite pins a number the hand table does not carry, the kill is
weaker and is labelled as such below rather than dressed up.

TWO THINGS THIS SET CANNOT REACH, said plainly rather than faked.

  1. THE DERIVATION ITSELF. `tools/arity_oracle.py` turns `Name@32` into eight dwords.
     `tools/mutate/c_suites.py` runs `make <target>` and then executes `build/<target>`,
     so it can only reach code that links into a ctest binary. A mutation in the Python
     generator would be neither rebuilt nor retested and would score as a survivor for
     the wrong reason. The byte-versus-dword defect is therefore injected into the
     COMMITTED TABLE instead, which is a fair proxy because the committed table is the
     artefact that ships -- and the generator's own arithmetic is covered by
     `tests/test_arity_oracle.py::test_stdcall_decoration_is_bytes_divided_by_four` and
     by the parser's refusal of any byte count that is not a multiple of four.

  2. THE PYTHON CROSS-CHECK. The `ABI_TABLE`-versus-oracle comparison lives in pytest
     because `ABI_TABLE` is `static` and no C test can see it. The harness cannot run
     pytest, so "the cross-check made silent" has no entry below. It was verified by hand
     instead, twice, and the MEASURED results are recorded here because the first attempt
     disproved what had been assumed about it.

     Editing `{219u, ..., 8u, 0u}` to `6u` in the COMMITTED TABLE failed exactly ONE
     test, `test_the_committed_c_table_is_what_the_vendored_def_derives_to`, and NOT the
     cross-check. That is correct behaviour and worth understanding: the cross-check
     re-derives from the vendored `.def` rather than reading the committed table, so a
     hand-edited table is a STALENESS defect, not a disagreement. The two are guarded in
     a chain -- committed equals derived, and derived agrees with `ABI_TABLE` -- and only
     the two together imply the committed table agrees with the hand counts.

     Editing the SOURCE, `NtReadFile@32` to `@24` in the vendored `.def`, failed THREE:
     the recorded-hash check, the staleness check, and the cross-check, the last with
     `ORACLE/HAND DISAGREEMENT -- this is a FINDING: ordinal 219 (NtReadFile)
     stack_args: hand=8 oracle=6`. That is the loud failure the design calls for, and it
     names the ordinal.

  The nearest C-reachable analogue of a silenced cross-check IS covered, by
  `arity-oracle-data-refusal-made-silent` below: a refusal turned into a confident zero
  is the same failure shape one layer down.
"""

MUTATIONS: list[dict] = [
    {
        "id": "arity-oracle-bytes-where-dwords-belong",
        "file": "src/xbox/kernel_arity_oracle.c",
        "old": "{219u, KERNEL_ARITY_ORACLE_STDCALL, 8u, 0u}, /* NtReadFile */",
        "new": "{219u, KERNEL_ARITY_ORACLE_STDCALL, 32u, 0u}, /* NtReadFile */",
        "targets": ["test_arity_oracle"],
        "why": "THE OFF-BY-FOUR-TIMES ERROR, which is the defect this whole table exists "
        "to avoid introducing. The `.def` decoration `NtReadFile@32` carries BYTES; "
        "everything downstream wants dwords. A table that forgot the divide would "
        "still be a well-formed table of plausible-looking small integers, and for "
        "an export taking one argument the wrong answer (4) is an entirely ordinary "
        "arity, so nothing structural catches it. Because __stdcall is "
        "callee-cleanup, the consequence is not a crash: the thunk pops 128 bytes "
        "instead of 32, eats the caller's locals, and the run keeps going producing "
        "a plausible wrong trace. 219 is chosen because its correct value 8 is "
        "hand-verified at this image's own call sites, so the kill rests on "
        "something other than the table agreeing with itself.",
    },
    {
        "id": "arity-oracle-fastcall-misdetected-as-stdcall",
        "file": "src/xbox/kernel_arity_oracle.c",
        "old": "{161u, KERNEL_ARITY_ORACLE_FASTCALL, 0u, 1u}, /* KfLowerIrql */",
        "new": "{161u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* KfLowerIrql */",
        "targets": ["test_arity_oracle"],
        "why": "EXACTLY what a parser that ignored the leading `@` would produce: the one "
        "dword lands on the stack instead of in ecx. This is not hypothetical "
        "arithmetic -- `ABI_TABLE` records that KfLowerIrql is reached by "
        "`mov cl, al` with NOTHING pushed at its real call sites, so the mutated row "
        "would make the thunk read a bogus argument off the stack AND pop four bytes "
        "nobody pushed. The mutation is deliberately self-consistent (one argument "
        "either way), so only a check that looks at the CONVENTION rather than the "
        "count can see it.",
    },
    {
        "id": "arity-oracle-fastcall-stack-remainder-dropped",
        "file": "src/xbox/kernel_arity_oracle.c",
        "old": "{21u, KERNEL_ARITY_ORACLE_FASTCALL, 1u, 2u}, /* ExInterlockedCompareExchange64 */",
        "new": "{21u, KERNEL_ARITY_ORACLE_FASTCALL, 0u, 2u}, /* ExInterlockedCompareExchange64 */",
        "targets": ["test_arity_oracle"],
        "why": "The register/stack SPLIT, which is the half of fastcall that a spot check "
        "misses. Twenty-two of the twenty-four fastcall rows have two or fewer "
        "arguments and therefore no stack remainder at all, so a derivation that "
        "assumed fastcall never spills would be right 22 times out of 24 and wrong "
        "here and at ordinal 51. Those two rows are the ONLY evidence in the table "
        "that the split is computed rather than assumed, which makes a survivor here "
        "mean the suite is testing a coincidence.",
    },
    {
        "id": "arity-oracle-data-export-given-an-arity",
        "file": "src/xbox/kernel_arity_oracle.c",
        "old": "{16u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* ExEventObjectType */",
        "new": "{16u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* ExEventObjectType */",
        "targets": ["test_arity_oracle"],
        "why": "A DATA export is a VARIABLE. ExEventObjectType is an object-type pointer "
        "the guest dereferences, and this title imports it. Classifying it as a "
        "one-argument function would make the oracle answer with a pop count for "
        "something that is never called, and the single most likely way for that to "
        "happen is a parser that ignores the trailing DATA keyword -- which is "
        "exactly what this row simulates. The structural danger is that the table "
        "stores zero in `stack_args` for real DATA rows, so a check that read that "
        "field without consulting the convention would pass both before and after.",
    },
    {
        "id": "arity-oracle-data-refusal-made-silent",
        "file": "src/xbox/kernel_arity_oracle.c",
        "old": "if (entry->convention == KERNEL_ARITY_ORACLE_DATA) {",
        "new": "if (entry->convention == KERNEL_ARITY_ORACLE_DATA && false) {",
        "targets": ["test_arity_oracle"],
        "why": "A REFUSAL TURNED INTO A CONFIDENT ZERO, which is the C-reachable form of "
        "the cross-check-made-silent defect. The function keeps its signature, keeps "
        "returning true, and hands back the stored zero -- so every caller that only "
        "checks the return value is satisfied and the 34 DATA ordinals silently "
        "acquire an arity of zero. Zero is the single most dangerous wrong answer "
        "here because it is also the CORRECT answer for 19 genuine zero-argument "
        "stdcall exports and for all five cdecl rows, so it never looks anomalous. "
        "`&& false` rather than deleting the branch, because a deleted `entry` use "
        "would trip -Wunused and score NOT-A-MUTANT.",
    },
    {
        "id": "arity-oracle-cdecl-conflated-with-unknown",
        "file": "src/xbox/kernel_arity_oracle.c",
        "old": "{8u, KERNEL_ARITY_ORACLE_CDECL, 0u, 0u}, /* DbgPrint */",
        "new": "{8u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* DbgPrint */",
        "targets": ["test_arity_oracle"],
        "why": "The OPPOSITE error to the DATA mutation above, and it has to be tested "
        "separately because both rows store the same zero. An undecorated name in a "
        ".def is __cdecl, not a variable: the caller cleans up, so the callee pops "
        "nothing and that is a POSITIVE answer a thunk can act on. Collapsing it to "
        "DATA turns an answerable ordinal into a refusal, which costs a stopped run "
        "rather than a corrupted one -- less dangerous, but it silently removes the "
        "only coverage this title has for ordinal 8, which it imports and for which "
        "NEITHER the hand table NOR the measured table has any row at all. A "
        "survivor would mean the cdecl distinction is decorative.",
    },
    {
        "id": "arity-oracle-lookup-clamps-to-a-neighbour",
        "file": "src/xbox/kernel_arity_oracle.c",
        "old": """            high = mid;
        }
    }
    return NULL;
}""",
        "new": """            high = mid;
        }
    }
    return low < ORACLE_TABLE_COUNT ? &ORACLE_TABLE[low] : NULL;
}""",
        "targets": ["test_arity_oracle"],
        "why": "Ordinals are SPARSE -- 371 rows spanning 1..378 with a seven-wide hole at "
        "367..373 -- so a search that clamps instead of failing answers with the "
        "neighbouring export's arity. That is the worst possible shape of wrong "
        "answer: a confident number for an ordinal the oracle knows nothing about, "
        "where a refusal would have become a diagnostic naming the ordinal. The "
        "mutant is subtle because it is CORRECT for all 371 present ordinals and "
        "wrong only off the ends and in the hole, which is precisely the region a "
        "suite that only probed real exports would never visit.",
    },
    {
        "id": "arity-oracle-ordinal-49-drifts-to-the-fastcall-alternative",
        "file": "src/xbox/kernel_arity_oracle.c",
        "old": "{49u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* HalReturnToFirmware */",
        "new": "{49u, KERNEL_ARITY_ORACLE_FASTCALL, 0u, 1u}, /* HalReturnToFirmware */",
        "targets": ["test_arity_oracle"],
        "why": "ORDINAL 49 IS THE ONE CASE THIS PROJECT HAS SETTLED FROM THE TARGET IMAGE, "
        "and the oracle's agreement with that settlement is the single strongest "
        "piece of evidence we have against ordinal-number drift between nxdk's "
        "export list and XDK 5849. The mutation is the suspected alternative exactly "
        "as `tools/kernel_ordinals.py` describes it: HalRequestSoftwareInterrupt is "
        "__fastcall and pushes nothing, which is why four unanimous single-push call "
        "sites ruled it out. If this survives, the drift check is not actually "
        "checking anything and the 'oracle corroborates ordinal 49' claim in the "
        "report would be unfounded.",
    },
]
