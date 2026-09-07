// Clear a code range, force it to ARM or Thumb instruction-decode mode, and
// re-disassemble -- the three manual GUI steps (Clear Code Bytes / Set
// Register TMode / Disassemble) this project has been doing by hand for
// every ARM/Thumb disassembly-context bug, collapsed into one script run.
//
// Why this exists as a native Ghidra script rather than an MCP tool: the
// live "ghidra" MCP server in use on this project (themixednuts/GhidraMCP
// v0.8.0, see README.md) has no tool for setting processor context
// registers or forcing a disassembly mode, and a different MCP plugin that
// does have a general script-execution endpoint (bethington/ghidra-mcp) was
// tried on this project earlier and abandoned as unreliable -- not worth
// reviving just for this one feature. A script placed in Ghidra's own
// Script Manager needs no new plugin, no rebuild, and runs directly against
// the already-open program.
//
// TMode register confirmed directly against this Ghidra install's own ARM
// language definition (not guessed): see
// /opt/ghidra/Ghidra/Processors/ARM/data/languages/ARM.sinc line ~90 --
// "TMode = (0,0)  # 1 if in Thumb instruction decode mode". So TMode=1 is
// Thumb, TMode=0 is ARM. The script still verifies the register exists on
// the current program's language before touching anything, and lists what
// context registers ARE available if not, rather than assuming.
//
// Usage: Window -> Script Manager, find this script (add
// tools/ghidra_scripts as a script directory once via the Script
// Directories icon if it doesn't show up), run it, answer the three
// prompts (start address, byte length, ARM or Thumb). It only touches the
// exact range you give it -- it does not guess function boundaries and
// does not recreate a function afterward; do that separately if needed
// (see ghidra_scripts/ClearAndDisasm6f876960.java-style pattern for that,
// if wanted, as a follow-up, not folded in here to keep this script's
// blast radius small and predictable).
//
//@category ICOM.ARM-Thumb

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.ProgramContext;

import java.math.BigInteger;
import java.util.List;

public class FixArmThumbMode extends GhidraScript {
    @Override
    public void run() throws Exception {
        Address start = askAddress("Start address", "Clear/re-disassemble starting at:");
        int length = askInt("Length", "Number of bytes to clear/re-disassemble (from start):");
        if (length <= 0) {
            println("Length must be positive.");
            return;
        }
        Address end = start.add(length - 1); // inclusive, matches Ghidra's usual range convention

        String modeChoice = askChoice("Instruction set mode", "Force which mode over this range?",
                List.of("Thumb", "ARM"), "Thumb");
        int modeValue = modeChoice.equals("Thumb") ? 1 : 0;

        ProgramContext ctx = currentProgram.getProgramContext();
        Register modeReg = ctx.getRegister("TMode");
        if (modeReg == null) {
            println("No 'TMode' register on this program's language (" + currentProgram.getLanguageID()
                    + "). This script is ARM/Thumb-specific. Available context registers:");
            for (Register r : ctx.getContextRegisters()) {
                println("  " + r.getName());
            }
            return;
        }

        clearListing(start, end);
        println("Cleared " + start + " to " + end + " (" + length + " bytes)");

        ctx.setValue(modeReg, start, end, BigInteger.valueOf(modeValue));
        println("Set " + modeReg.getName() + "=" + modeValue + " (" + modeChoice + ") over " + start + "-" + end);

        disassemble(start);
        println("Disassembled from " + start);
    }
}
