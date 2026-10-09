// SPDX-License-Identifier: GPL-3.0-or-later
//
// Parse a C header into the current program's data type manager, and report
// exactly which aggregates landed and which did not.
//
// The intended input is `tools/gen_donor_header.py`'s output: struct layouts
// recovered from the TimeSplitters 2 donor's `.mdebug` stabs. Types are the part
// of a decompilation that pays for itself immediately -- a named field read beats
// `*(int *)(param_1 + 0x2c)` for every reader -- and this is the only mechanism
// Ghidra offers for getting a few hundred of them in at once.
//
// THIS SCRIPT ONLY POPULATES THE TYPE DATABASE. It never attaches a type to a
// function signature, a parameter, or a variable, because nothing here is evidence
// about WHICH function takes WHICH struct. Parsing a layout in is safe and
// reversible; guessing an association writes a plausible-looking lie into the
// database that every later reader would inherit. The two are separate decisions
// and only the first one is supported by a donor header.
//
// Input format: any C that Ghidra's parser accepts. Include directories are
// searched for `#include` targets, and `tools/ghidra/cparser-include` exists
// precisely because Ghidra ships no <stdint.h> and an unresolved include fails the
// whole parse rather than one line of it.
//
// ONE PARSE, NOT MANY. CParserUtils runs the preprocessor over every file and then
// hands the combined result to the C parser in a single pass, so a syntax error is
// not isolated to its own declaration: the parser stops there and everything after
// it is lost. That is why the CSV this writes is the real result and the printed
// counters are a summary of it -- "the parse threw" and "the parse imported
// nothing" are different outcomes, and so are "imported all 391" and "imported the
// 120 that preceded the first error".
//
// Usage (headless):
//   analyzeHeadless <proj_dir> <proj> -process default.xbe -noanalysis \
//       -scriptPath tools/ghidra -postScript ParseCHeader.java \
//       <header.h> <types.csv> [includeDir ...]
//
// Columns of <types.csv>: path,name,kind,size_bytes,field_count
//
//@category TSFP
//@author tsfp-decomp

import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.app.util.cparser.C.CParserUtils;
import ghidra.app.util.cparser.C.CParserUtils.CParseResults;
import ghidra.program.model.data.Composite;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeManager;
import ghidra.program.model.data.Structure;
import ghidra.program.model.data.Union;

public class ParseCHeader extends GhidraScript {

    /** Longest a parse message may be before it is truncated in the log. */
    private static final int MESSAGE_LIMIT = 20000;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("ERROR: expected <header.h> <types.csv> [includeDir ...]");
            return;
        }
        Path header = Path.of(args[0]);
        if (!Files.isRegularFile(header)) {
            println("ERROR: no such file: " + header);
            return;
        }
        Path out = Path.of(args[1]);
        if (out.getParent() != null) {
            Files.createDirectories(out.getParent());
        }

        String[] includePaths = new String[args.length - 2];
        for (int index = 2; index < args.length; index++) {
            includePaths[index - 2] = args[index];
            println("include path   " + args[index]);
        }

        DataTypeManager manager = currentProgram.getDataTypeManager();
        int typesBefore = manager.getDataTypeCount(false);
        int compositesBefore = countComposites(manager);
        println("header         " + header);
        println("types before   " + typesBefore + " (" + compositesBefore + " composites)");

        // An empty openDTMgrs array keeps this self-contained: every name the
        // header needs is either defined in it or supplied by the include shim, so
        // there is no dependence on a .gdt archive that a later run might not have.
        CParseResults results = null;
        String failure = null;
        try {
            results = CParserUtils.parseHeaderFiles(
                    new DataTypeManager[0],
                    new String[] { header.toString() },
                    includePaths,
                    new String[0],
                    manager,
                    monitor);
        } catch (Exception exception) {
            // A thrown parse exception still leaves behind whatever was added
            // before the failure, so this is reported and then measured, never
            // treated as "nothing happened".
            failure = exception.getClass().getSimpleName() + ": " + exception.getMessage();
            println("ERROR: parse threw: " + failure);
        }

        if (results != null) {
            println("successful     " + results.successful());
            report("cpp", results.cppParseMessages());
            report("c", results.cParseMessages());
        }

        int typesAfter = manager.getDataTypeCount(false);
        List<Composite> composites = composites(manager);
        println("types after    " + typesAfter + " (" + composites.size() + " composites)");
        println("types added    " + (typesAfter - typesBefore));
        println("composites add " + (composites.size() - compositesBefore));

        write(out, composites);
        println("wrote          " + composites.size() + " composites to " + out);
        if (failure != null) {
            println("ERROR: parse did not complete; the CSV is a partial import");
        }
    }

    /** Print a parse-message block, truncated, with each line prefixed. */
    private void report(String label, String messages) {
        if (messages == null || messages.isBlank()) {
            println(label + " messages    (none)");
            return;
        }
        String text = messages.length() > MESSAGE_LIMIT
                ? messages.substring(0, MESSAGE_LIMIT) + "\n...(truncated)"
                : messages;
        println(label + " messages:");
        for (String line : text.split("\\R")) {
            if (!line.isBlank()) {
                println("  [" + label + "] " + line);
            }
        }
    }

    private static List<Composite> composites(DataTypeManager manager) {
        List<Composite> found = new ArrayList<>();
        Iterator<DataType> types = manager.getAllDataTypes();
        while (types.hasNext()) {
            DataType type = types.next();
            if (type instanceof Composite composite) {
                found.add(composite);
            }
        }
        return found;
    }

    private static int countComposites(DataTypeManager manager) {
        return composites(manager).size();
    }

    private void write(Path out, List<Composite> composites) throws Exception {
        try (PrintWriter writer = new PrintWriter(
                Files.newBufferedWriter(out, StandardCharsets.UTF_8))) {
            writer.println("path,name,kind,size_bytes,field_count");
            for (Composite composite : composites) {
                writer.println(csv(composite.getPathName())
                        + "," + csv(composite.getName())
                        + "," + kind(composite)
                        + "," + composite.getLength()
                        + "," + composite.getNumComponents());
            }
        }
    }

    private static String kind(Composite composite) {
        if (composite instanceof Union) {
            return "union";
        }
        return composite instanceof Structure ? "struct" : "composite";
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
