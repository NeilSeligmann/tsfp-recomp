// SPDX-License-Identifier: GPL-3.0-or-later
//
// Apply an `address name` table to the current program.
//
// The intended input is the table tools/xtlid.py produces from the executable's
// own .XTLID section: exact XDK library names with zero false positives, so they
// may be applied without review. The format is deliberately plain so any other
// name source can feed the same script.
//
// Input format, one entry per line; blank lines and `#` comments are skipped:
//
//     0x0037c97b XLoadSectionByHandle  # XAPILIB
//
// Usage (headless):
//   analyzeHeadless <proj_dir> <proj> -process default.xbe -noanalysis \
//       -scriptPath tools/ghidra -postScript ApplyNames.java <names.txt>
//
//@category TSFP
//@author tsfp-decomp

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class ApplyNames extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            println("ERROR: expected a names-file argument");
            return;
        }
        Path source = Path.of(args[0]);
        if (!Files.isRegularFile(source)) {
            println("ERROR: no such file: " + source);
            return;
        }

        List<String> lines = Files.readAllLines(source, StandardCharsets.UTF_8);
        int applied = 0;
        int alreadyNamed = 0;
        int noFunction = 0;
        int malformed = 0;
        int outside = 0;

        for (String raw : lines) {
            String line = stripComment(raw).trim();
            if (line.isEmpty()) {
                continue;
            }
            String[] parts = line.split("\\s+", 2);
            if (parts.length < 2) {
                malformed++;
                continue;
            }
            Long value = parseHexAddress(parts[0]);
            if (value == null) {
                malformed++;
                continue;
            }
            String name = parts[1].trim();
            if (name.isEmpty()) {
                malformed++;
                continue;
            }

            Address address = currentProgram.getImageBase().getNewAddress(value);
            if (!currentProgram.getMemory().contains(address)) {
                outside++;
                continue;
            }

            Function function = getFunctionAt(address);
            if (function == null) {
                // No function here yet. Create one: an .XTLID entry is strong
                // evidence that this address IS a function entry point, which is
                // itself worth acting on rather than discarding.
                function = createFunction(address, name);
                if (function == null) {
                    noFunction++;
                    continue;
                }
                applied++;
                continue;
            }

            // Do not overwrite a name that is already meaningful. Ghidra's
            // auto-generated names are the ones safe to replace.
            String existing = function.getName();
            if (!existing.startsWith("FUN_") && !existing.startsWith("SUB_")
                    && !existing.equals(name)) {
                alreadyNamed++;
                continue;
            }
            function.setName(name, SourceType.IMPORTED);
            applied++;
        }

        println("applied        " + applied);
        println("already named  " + alreadyNamed);
        println("no function    " + noFunction);
        println("outside memory " + outside);
        println("malformed      " + malformed);
    }

    private static String stripComment(String line) {
        int hash = line.indexOf('#');
        return hash >= 0 ? line.substring(0, hash) : line;
    }

    /* Named parseHexAddress, not parseAddress: GhidraScript already defines
     * parseAddress(String) and overriding it with a different return type is a
     * compile error that surfaces only as "class could not be found". */
    private static Long parseHexAddress(String token) {
        String text = token.trim();
        int radix = 16;
        if (text.startsWith("0x") || text.startsWith("0X")) {
            text = text.substring(2);
        }
        try {
            return Long.parseLong(text, radix);
        } catch (NumberFormatException exception) {
            return null;
        }
    }
}
