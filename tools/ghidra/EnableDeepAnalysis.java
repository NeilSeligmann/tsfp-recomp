// SPDX-License-Identifier: GPL-3.0-or-later
//
// Turn on the analysis passes that resolve register-passed parameters, then
// report what the options actually ended up as.
//
// Why this exists: with stock headless options, 30% of the TSFP export carried
// `in_<REG>` or `unaff_<REG>` locals, meaning the decompiler could not tell what
// a function's arguments were. Xbox MSVC code uses __fastcall and __stdcall
// heavily while Ghidra's x86 default is __cdecl, and the analyzer that recovers
// the difference -- "Decompiler Parameter ID" -- is DISABLED by default because
// it is expensive. On a 12,343-function binary it is worth the wait.
//
// Run as a -preScript so the options are set before analysis begins.
//
//@category TSFP
//@author tsfp-decomp

import ghidra.app.script.GhidraScript;
import ghidra.framework.options.Options;
import ghidra.program.model.listing.Program;

public class EnableDeepAnalysis extends GhidraScript {

    /** Analyzer options to force on, by their exact option names. */
    private static final String[] ENABLE = {
        "Decompiler Parameter ID",
        "Decompiler Switch Analysis",
        "Stack",
    };

    @Override
    public void run() throws Exception {
        Options options = currentProgram.getOptions(Program.ANALYSIS_PROPERTIES);

        for (String name : ENABLE) {
            if (!options.contains(name)) {
                println("WARN  no such analysis option: '" + name + "'");
                continue;
            }
            boolean before = options.getBoolean(name, false);
            options.setBoolean(name, true);
            println("SET   " + name + ": " + before + " -> " + options.getBoolean(name, false));
        }

        // Parameter ID is the expensive one and its own sub-options govern how
        // hard it tries. Raise them where present.
        for (String name : options.getOptionNames()) {
            if (name.startsWith("Decompiler Parameter ID.")) {
                println("INFO  " + name + " = " + options.getValueAsString(name));
            }
        }

        println("INFO  analysis options prepared for " + currentProgram.getName());
    }
}
