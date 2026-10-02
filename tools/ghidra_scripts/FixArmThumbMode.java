// Clear a code range, force it to ARM or Thumb instruction-decode mode, and
// re-disassemble -- the three manual GUI steps (Clear Code Bytes / Set
// Register TMode / Disassemble) this project has been doing by hand for
// every ARM/Thumb disassembly-context bug, collapsed into one script run.
//
// Reads its work from a fixed request file instead of interactive prompts,
// specifically so an assistant or another script can write the addresses
// directly and just ask you to click Run -- no copy-pasting addresses into
// dialogs. Request file (relative to the repo root, which is found from this
// script's own location, or $ICOM_REPO if set):
//   scratch/armthumb_fix_requests.txt
// One line per fix: "<address> <length> <arm|thumb>", optionally followed
// by a free-text note (kept only for the log, ignored otherwise). Blank
// lines and lines starting with # are skipped. Example:
//   0x20056fd4 16 thumb   known needs-Thumb spot, bookmark sweep 2026-08-29
//   0x2014b000 4096 thumb FreeType Thumb-2 region
// Length is bytes from the start address (inclusive end = start+length-1).
//
// After a run, every line's outcome (OK or the error message) is appended,
// timestamped, to:
//   scratch/armthumb_fix_results.txt
// and the request file is replaced with a single "processed at <time>"
// comment line, so a stray re-run with no new content is an obvious no-op
// instead of silently reapplying old fixes. Write fresh lines into the
// request file for the next run.
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
// Directories icon if it doesn't show up), run it. It only touches the
// exact ranges given in the request file -- it does not guess function
// boundaries and does not recreate a function afterward; do that
// separately if needed, to keep this script's blast radius small and
// predictable.
//
//@category ICOM.ARM-Thumb

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.ProgramContext;

import java.io.File;
import java.io.FileWriter;
import java.math.BigInteger;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.List;

public class FixArmThumbMode extends GhidraScript {

    private static final String REQUEST_REL_PATH = "scratch/armthumb_fix_requests.txt";
    private static final String RESULT_REL_PATH = "scratch/armthumb_fix_results.txt";

    /** The repo root: $ICOM_REPO if set, else three levels up from this file
     *  (<repo>/tools/ghidra_scripts/FixArmThumbMode.java). */
    private File repoRoot() {
        String env = System.getenv("ICOM_REPO");
        if (env != null && !env.isEmpty()) {
            return new File(env);
        }
        return new File(getSourceFile().getAbsolutePath()).getParentFile().getParentFile().getParentFile();
    }

    @Override
    public void run() throws Exception {
        File repo = repoRoot();
        File requestFile = new File(repo, REQUEST_REL_PATH);
        File resultFile = new File(repo, RESULT_REL_PATH);

        if (!requestFile.exists()) {
            println("No request file at " + requestFile + " -- nothing to do.");
            return;
        }

        List<String> lines = Files.readAllLines(requestFile.toPath());

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

        SimpleDateFormat ts = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss");
        StringBuilder results = new StringBuilder();
        int okCount = 0, failCount = 0;

        for (String raw : lines) {
            String line = raw.trim();
            if (line.isEmpty() || line.startsWith("#")) {
                continue;
            }

            String stamp = ts.format(new Date());
            try {
                String[] parts = line.split("\\s+", 4);
                if (parts.length < 3) {
                    throw new IllegalArgumentException(
                            "expected '<address> <length> <arm|thumb>', got: " + line);
                }
                Address start = toAddr(parts[0]);
                int length = Integer.decode(parts[1]);
                String modeStr = parts[2].toLowerCase();
                if (!modeStr.equals("arm") && !modeStr.equals("thumb")) {
                    throw new IllegalArgumentException("mode must be 'arm' or 'thumb', got: " + parts[2]);
                }
                if (length <= 0) {
                    throw new IllegalArgumentException("length must be positive: " + parts[1]);
                }

                Address end = start.add(length - 1); // inclusive, matches Ghidra's usual range convention
                int modeValue = modeStr.equals("thumb") ? 1 : 0;

                clearListing(start, end);
                ctx.setValue(modeReg, start, end, BigInteger.valueOf(modeValue));

                // disassemble(start) alone only follows CONTROL FLOW from start -- it stops at
                // any return/unconditional-branch with no traced successor, leaving later
                // independent functions in the same range undefined (hit for real 2026-09-07:
                // several small back-to-back functions connected by nothing but proximity, e.g.
                // a run of "mov r0,#N; bx lr" stubs). So sweep every mode-aligned address in the
                // range (4 bytes for ARM -- always instruction-aligned, so this never lands
                // mid-instruction; 2 bytes for Thumb, where it safely can land mid a 4-byte
                // Thumb-2 instruction, hence the getInstructionContaining check) and disassemble
                // from any address not already covered by an instruction.
                int step = modeStr.equals("thumb") ? 2 : 4;
                for (Address cur = start; cur.compareTo(end) <= 0; cur = cur.add(step)) {
                    if (getInstructionContaining(cur) == null) {
                        disassemble(cur);
                    }
                }

                println("Fixed " + start + "-" + end + " mode=" + modeStr);
                results.append(stamp).append("  ").append(line).append("  -> OK\n");
                okCount++;
            } catch (Exception e) {
                println("FAILED: " + line + " -- " + e.getMessage());
                results.append(stamp).append("  ").append(line).append("  -> ERROR: ")
                        .append(e.getMessage()).append("\n");
                failCount++;
            }
        }

        if (okCount == 0 && failCount == 0) {
            println("Request file had no actionable lines (all blank/comments).");
            return;
        }

        try (FileWriter fw = new FileWriter(resultFile, true)) {
            fw.write(results.toString());
        }

        String header = "# processed " + ts.format(new Date()) + " -- " + okCount + " ok, " + failCount
                + " failed. Write new '<address> <length> <arm|thumb>' lines here for the next run.\n";
        Files.writeString(requestFile.toPath(), header);

        println(okCount + " fixed, " + failCount + " failed. Results appended to " + resultFile);
    }
}
