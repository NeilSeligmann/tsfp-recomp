// SPDX-License-Identifier: GPL-3.0-or-later
//
// Export the function-boundary table of the current program as CSV. Addresses and
// sizes only, no decompilation, so this is fast enough to run on both builds.
//
// The point is cross-build comparison: run this on two XBEs and diff the tables
// to validate recovered boundaries independently of any decompiler output.
//
// Default columns: entry_va,size_bytes,name,is_thunk,body_max_va
// Optional --body-ranges appends body_min_va,body_ranges (inclusive lo-hi;lo-hi).
//
//   entry_va     function entry, hex, 0x-prefixed and zero-padded to 8 digits
//   size_bytes   decimal byte count of the body
//   name         Ghidra's symbol name, RFC 4180 quoted when it needs it
//   is_thunk     true/false
//   body_max_va  highest address in the body, same hex format as entry_va
//
// A function body is NOT guaranteed contiguous, so the size is
// getBody().getNumAddresses() and never maxAddress - entryPoint. body_max_va is
// emitted alongside precisely so a consumer can spot the fragmented cases:
// Entry can lie above body_min_va; use exact ranges, not entry+size, for membership.
//
// Usage (headless):
//   analyzeHeadless <proj_dir> <proj> -process default.xbe -noanalysis \
//       -scriptPath tools/ghidra -postScript ExportFunctionBounds.java <outfile> [--body-ranges]
//
//@category TSFP
//@author tsfp-decomp

import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;

public class ExportFunctionBounds extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1 || args.length > 2
                || (args.length == 2 && !args[1].equals("--body-ranges"))) {
            throw new IllegalArgumentException("expected output path and optional --body-ranges");
        }
        boolean exact = args.length == 2;
        Path out = Path.of(args[0]);
        if (out.getParent() != null) {
            Files.createDirectories(out.getParent());
        }

        FunctionManager functions = currentProgram.getFunctionManager();
        int total = functions.getFunctionCount();
        println("exporting " + total + " function bounds to " + out);

        int done = 0;
        int thunks = 0;
        int fragmented = 0;

        try (PrintWriter writer = new PrintWriter(
                Files.newBufferedWriter(out, StandardCharsets.UTF_8))) {
            writer.println("entry_va,size_bytes,name,is_thunk,body_max_va"
                    + (exact ? ",body_min_va,body_ranges" : ""));

            for (Function function : functions.getFunctions(true)) {
                if (monitor.isCancelled()) {
                    println("ERROR: cancelled after " + done + " of " + total);
                    break;
                }
                AddressSetView body = function.getBody();
                // An empty body would make getMaxAddress() null; record it rather
                // than crashing, because a 0-size function is itself a finding.
                boolean empty = body.isEmpty();
                if (body.getNumAddressRanges() > 1) {
                    fragmented++;
                }
                if (function.isThunk()) {
                    thunks++;
                }

                writer.println(hex(function.getEntryPoint().getOffset())
                        + "," + body.getNumAddresses()
                        + "," + csv(function.getName())
                        + "," + function.isThunk()
                        + "," + (empty ? "" : hex(body.getMaxAddress().getOffset()))
                        + (exact ? "," + (empty ? "" : hex(body.getMinAddress().getOffset()))
                                + "," + csv(ranges(body, function.getEntryPoint(),
                                        currentProgram.getAddressFactory().getDefaultAddressSpace())) : ""));
                done++;
            }
        }

        println("done: " + done + " functions, " + thunks + " thunks, "
                + fragmented + " with a non-contiguous body");
    }

    /** Ordered inclusive ranges; empty body has blank min/max/ranges and count 0. */
    private static String ranges(AddressSetView body, Address entry, AddressSpace defaultSpace) {
        StringBuilder result = new StringBuilder();
        if (!entry.getAddressSpace().equals(defaultSpace)
                || entry.getAddressSpace().getSize() != 32
                || entry.getOffset() < 0 || entry.getOffset() > 0xffffffffL) {
            throw new IllegalArgumentException("naming range export requires 32-bit entry");
        }
        for (AddressRange range : body.getAddressRanges(true)) {
            Address lo = range.getMinAddress();
            Address hi = range.getMaxAddress();
            if (!lo.getAddressSpace().equals(entry.getAddressSpace())
                    || !hi.getAddressSpace().equals(entry.getAddressSpace())
                    || lo.getOffset() < 0 || hi.getOffset() > 0xffffffffL) {
                throw new IllegalArgumentException("naming ranges require one 32-bit address space");
            }
            if (result.length() != 0) {
                result.append(';');
            }
            result.append(hex(lo.getOffset())).append('-').append(hex(hi.getOffset()));
        }
        return result.toString();
    }

    /** Lowercase hex, 0x-prefixed, zero-padded to at least 8 digits. */
    private static String hex(long value) {
        return String.format("0x%08x", value);
    }

    /** RFC 4180: quote and double-up embedded quotes only when necessary. */
    private static String csv(String value) {
        if (value == null) {
            return "";
        }
        if (value.indexOf(',') < 0 && value.indexOf('"') < 0
                && value.indexOf('\n') < 0 && value.indexOf('\r') < 0) {
            return value;
        }
        return "\"" + value.replace("\"", "\"\"").replace("\r", " ").replace("\n", " ") + "\"";
    }
}
