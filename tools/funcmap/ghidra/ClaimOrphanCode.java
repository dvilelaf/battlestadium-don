// Makes Ghidra claim the code of the main text that no function owns.
// Usage (headless): -postScript ClaimOrphanCode.java <text start> <text end> [seeds file]
// Seeds file: lines "0x........ # comment"; only the lines whose comment starts with "new_code"
// are used. Disassembly never leaves [text start, text end). The program is changed.
// @category DON
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Listing;

import java.io.File;
import java.nio.file.Files;

public class ClaimOrphanCode extends GhidraScript {
    private static final int MAX_ROUNDS = 5;

    private Listing listing;
    private AddressSet range;

    private Address at(long value) {
        return currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(value);
    }

    // Disassembles from addr, restricted to the text range. Returns true when it ran.
    private boolean disassembleFrom(Address addr) {
        DisassembleCommand cmd = new DisassembleCommand(addr, range, true);
        return cmd.applyTo(currentProgram, monitor);
    }

    // Creates a function at addr if no function contains it. Returns 1 if created.
    private int ensureFunction(Address addr) throws Exception {
        if (listing.getFunctionContaining(addr) != null || listing.getInstructionAt(addr) == null) {
            return 0;
        }
        return createFunction(addr, null) != null ? 1 : 0;
    }

    private int countOrphanWords(long start, long end) throws Exception {
        int n = 0;
        for (long a = start; a < end; a += 4) {
            Address addr = at(a);
            if (listing.getInstructionContaining(addr) == null
                    && currentProgram.getMemory().getInt(addr) != 0) {
                n++;
            }
        }
        return n;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("ClaimOrphanCode: usage <text start> <text end> [seeds file]");
            return;
        }
        long start = Long.decode(args[0].trim());
        long end = Long.decode(args[1].trim());
        listing = currentProgram.getListing();
        range = new AddressSet(at(start), at(end - 1));

        long instrBefore = listing.getNumInstructions();
        int funcsBefore = currentProgram.getFunctionManager().getFunctionCount();
        int seedsUsed = 0;
        int created = 0;

        if (args.length >= 3 && new File(args[2]).isFile()) {
            for (String line : Files.readAllLines(new File(args[2]).toPath())) {
                int hash = line.indexOf('#');
                if (hash < 0 || !line.substring(hash + 1).trim().startsWith("new_code")) {
                    continue;
                }
                long value = Long.decode(line.substring(0, hash).trim());
                if (value < start || value >= end || (value & 3) != 0) {
                    continue;
                }
                Address addr = at(value);
                if (listing.getInstructionAt(addr) == null) {
                    disassembleFrom(addr);
                }
                int c = ensureFunction(addr);
                created += c;
                seedsUsed++;
            }
            println("seeds used: " + seedsUsed + ", functions created by seeds: " + created);
        }

        int rounds = 0;
        for (int round = 1; round <= MAX_ROUNDS; round++) {
            int createdInRound = 0;
            long instrRound = listing.getNumInstructions();
            for (long a = start; a < end; a += 4) {
                if (monitor.isCancelled()) {
                    return;
                }
                Address addr = at(a);
                if (listing.getInstructionContaining(addr) != null
                        || listing.getDefinedDataContaining(addr) != null
                        || currentProgram.getMemory().getInt(addr) == 0) {
                    continue;
                }
                disassembleFrom(addr);
                createdInRound += ensureFunction(addr);
            }
            rounds++;
            created += createdInRound;
            println("round " + round + ": functions created " + createdInRound
                    + ", instructions disassembled " + (listing.getNumInstructions() - instrRound));
            if (createdInRound == 0) {
                break;
            }
            analyzeChanges(currentProgram);
        }

        long remaining = countOrphanWords(start, end);
        println("claim summary: seeds used " + seedsUsed
                + ", instructions disassembled " + (listing.getNumInstructions() - instrBefore)
                + ", functions created " + created
                + " (function count " + funcsBefore + " -> "
                + currentProgram.getFunctionManager().getFunctionCount() + ")"
                + ", rounds " + rounds
                + ", non-zero words without instruction " + remaining);
    }
}
