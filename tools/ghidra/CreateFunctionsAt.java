// SPDX-License-Identifier: GPL-3.0-or-later
//
// Create a function at each address in a CSV, without touching any function that
// already exists.
//
// The intended input is `tools/codediff/cli.py --candidates-csv`: entries the
// donor build's function table predicts and this build's own analysis has no
// function at. Unlike the .XTLID names ApplyNames.java applies, these are
// MACHINE-PROPOSED and unconfirmed, so every function created here is named
// `CODEDIFF_<hex>` and stays identifiable as such until a human agrees with it.
//
// Input format: the CSV written by --candidates-csv, whose first column is the
// address. Only that column is read, so any CSV or bare address list works. A
// leading `target_va` header row is skipped, as are blank lines and `#` comments.
//
//     target_va,call_sites,donor_va,donor_size,donor_name
//     0x000326e0,3,0x0002ea70,23,FUN_0002ea70
//
// NOTHING IS DELETED OR RENAMED. An address that already has a function is
// reported and skipped, never re-pointed or re-bodied, so the only possible
// effect on the program is new functions plus whatever Ghidra re-derives from
// them. That matters because the point of the exercise is to MEASURE the effect,
// and a script that also mutated existing functions would make the before/after
// comparison uninterpretable.
//
// Ghidra's createFunction returns null rather than throwing when it cannot place
// a function, so null is a FAILED outcome to count and move past, never a reason
// to abort the batch.
//
// MOST CANDIDATES LAND ON UNDEFINED BYTES, and that is the whole reason Ghidra
// has no function there: it never disassembled the region, so there is no code
// for a function to be made out of. createFunction does not fail on undefined
// bytes, which would at least be honest — it SUCCEEDS and hands back a function
// whose body is the single entry byte. Measured on the 106-candidate retail set,
// 49 of 106 came back that way. So unless `nodisassemble` is passed, the script
// disassembles at an address carrying no instruction before creating anything,
// and reports those separately as `created (after disassembly)`: they are a
// different and weaker claim than a function placed over code Ghidra had already
// found. A created function whose body is still <= 1 byte is counted as
// `degenerate`, because it is a failure wearing a success's clothes.
//
// Two outcomes are reported but never acted on:
//
//   * `inside <entry>+<delta>` — the address sits in the body of an existing
//     function. Either Ghidra merged two functions and this candidate splits
//     them correctly, or the candidate is a mid-function label and creating it
//     would be wrong. The script does not guess: it records the containing
//     function, before creating, so the split is measurable afterwards.
//   * `FAILED <reason>` — createFunction returned null. The reason is read off
//     what is actually at the address.
//
// Usage (headless):
//   analyzeHeadless <proj_dir> <proj> -process default.xbe -noanalysis \
//       -scriptPath tools/ghidra -postScript CreateFunctionsAt.java \
//       <candidates.csv> [nodisassemble]
//
//@category TSFP
//@author tsfp-decomp

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;

import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.app.script.GhidraScript;

public class CreateFunctionsAt extends GhidraScript {

    /** Prefix marking a function as proposed by tools/codediff, not confirmed. */
    private static final String NAME_PREFIX = "CODEDIFF_";

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            println("ERROR: expected a candidates-CSV argument");
            return;
        }
        Path source = Path.of(args[0]);
        if (!Files.isRegularFile(source)) {
            println("ERROR: no such file: " + source);
            return;
        }
        boolean allowDisassemble = !(args.length > 1 && args[1].equalsIgnoreCase("nodisassemble"));
        println("reading " + source + (allowDisassemble
                ? " (will disassemble an address carrying no instruction)"
                : " (nodisassemble: undefined bytes will yield degenerate bodies)"));

        List<String> lines = Files.readAllLines(source, StandardCharsets.UTF_8);
        int created = 0;
        int createdAfterDisassembly = 0;
        int degenerate = 0;
        int already = 0;
        int failed = 0;
        int outside = 0;
        int malformed = 0;

        for (String raw : lines) {
            String line = stripComment(raw).trim();
            if (line.isEmpty()) {
                continue;
            }
            String token = line.split(",", -1)[0].trim();
            if (token.equalsIgnoreCase("target_va") || token.equalsIgnoreCase("entry_va")) {
                continue;
            }
            Long value = parseHexAddress(token);
            if (value == null) {
                println("  " + token + "  MALFORMED");
                malformed++;
                continue;
            }

            Address address = currentProgram.getImageBase().getNewAddress(value);
            if (!currentProgram.getMemory().contains(address)) {
                println("  " + address + "  OUTSIDE-MEMORY");
                outside++;
                continue;
            }

            Function existing = getFunctionAt(address);
            if (existing != null) {
                println("  " + address + "  already-a-function  " + existing.getName()
                        + " size=" + existing.getBody().getNumAddresses());
                already++;
                continue;
            }

            // Record the containing function BEFORE creating anything: once a
            // function exists at the address, the question of what used to span
            // it can no longer be asked of the program.
            String context = describeContext(address);
            String name = NAME_PREFIX + hex(value);

            // Disassemble BEFORE creating, not as a retry: createFunction does not
            // fail on undefined bytes, it returns a 1-byte stub, so there is no
            // failure to retry on. See the header comment.
            boolean disassembled = false;
            if (allowDisassemble && getInstructionAt(address) == null) {
                disassemble(address);
                disassembled = getInstructionAt(address) != null;
            }

            Function made = create(address, name);
            if (made == null) {
                println("  " + address + "  FAILED  " + reason(address) + context);
                failed++;
                continue;
            }

            long size = made.getBody().getNumAddresses();
            String note = "";
            if (size <= 1) {
                degenerate++;
                note = "  DEGENERATE (" + reason(address) + ")";
            }
            if (disassembled) {
                createdAfterDisassembly++;
            } else {
                created++;
            }
            println("  " + address + "  created" + (disassembled ? " (after disassembly)" : "")
                    + "  " + name + " size=" + size + context + note);
        }

        println("created                   " + created);
        println("created after disassembly " + createdAfterDisassembly);
        println("degenerate of those two   " + degenerate + " (body <= 1 byte, not a real win)");
        println("already a function        " + already);
        println("FAILED                    " + failed);
        println("outside memory            " + outside);
        println("malformed                 " + malformed);
        println("total addresses           "
                + (created + createdAfterDisassembly + already + failed + outside + malformed));
    }

    /** createFunction, with a thrown exception folded into the null it documents. */
    private Function create(Address address, String name) {
        try {
            return createFunction(address, name);
        } catch (Exception exception) {
            println("    (exception at " + address + ": " + exception.getMessage() + ")");
            return null;
        }
    }

    /** `  inside <entry>+<delta> size=<n>` when another function spans the address. */
    private String describeContext(Address address) {
        Function containing = getFunctionContaining(address);
        if (containing == null) {
            return "";
        }
        Address entry = containing.getEntryPoint();
        long delta = address.getOffset() - entry.getOffset();
        return "  inside " + containing.getName() + " at " + entry + "+" + delta
                + " size=" + containing.getBody().getNumAddresses();
    }

    /** Why createFunction most likely refused, from what is at the address. */
    private String reason(Address address) {
        Instruction instruction = getInstructionAt(address);
        if (instruction == null) {
            return getUndefinedDataAt(address) != null ? "no-instruction (undefined bytes)"
                    : "no-instruction";
        }
        return "has-instruction (" + instruction.getMnemonicString() + ")";
    }

    private static String stripComment(String line) {
        int hash = line.indexOf('#');
        return hash >= 0 ? line.substring(0, hash) : line;
    }

    /** Lowercase hex, zero-padded to at least 8 digits, matching the CSV format. */
    private static String hex(long value) {
        return String.format("%08x", value);
    }

    /* Named parseHexAddress, not parseAddress: GhidraScript already defines
     * parseAddress(String) and overriding it with a different return type is a
     * compile error that surfaces only as "class could not be found". */
    private static Long parseHexAddress(String token) {
        String text = token.trim();
        if (text.startsWith("0x") || text.startsWith("0X")) {
            text = text.substring(2);
        }
        try {
            return Long.parseLong(text, 16);
        } catch (NumberFormatException exception) {
            return null;
        }
    }
}
