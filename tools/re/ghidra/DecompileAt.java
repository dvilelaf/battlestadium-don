// Prints the decompiled C and the disassembly of the function that contains each address argument.
// Usage (headless): -postScript DecompileAt.java 0x001011D8 [0x...]
// Each function is printed between "=== BEGIN 0x<addr> <name> ===" and "=== END 0x<addr> ===".
// @category DON
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;

public class DecompileAt extends GhidraScript {
    private static final int TIMEOUT_SECONDS = 60;

    private void emit(String text) {
        for (String line : text.split("\n", -1)) {
            println(line);
        }
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length == 0) {
            println("DecompileAt: no addresses given");
            return;
        }
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        try {
            for (String arg : args) {
                long value;
                try {
                    value = Long.decode(arg.trim());
                } catch (NumberFormatException e) {
                    println("=== BEGIN " + arg + " <invalid address> ===");
                    println("invalid address: " + arg);
                    println("=== END " + arg + " ===");
                    continue;
                }
                Address addr = toAddr(value);
                String tag = String.format("0x%08x", value);
                Function f = getFunctionContaining(addr);
                if (f == null) {
                    println("=== BEGIN " + tag + " <none> ===");
                    println("no function contains " + tag);
                    println("=== END " + tag + " ===");
                    continue;
                }
                println("=== BEGIN " + tag + " " + f.getName() + " ===");
                println("// function " + f.getName() + " " + f.getBody().getMinAddress() + ".." + f.getBody().getMaxAddress());
                DecompileResults res = ifc.decompileFunction(f, TIMEOUT_SECONDS, monitor);
                if (res.decompileCompleted() && res.getDecompiledFunction() != null) {
                    emit(res.getDecompiledFunction().getC());
                } else {
                    println("// decompilation failed: " + res.getErrorMessage());
                }
                println("// ---- disassembly ----");
                InstructionIterator it = currentProgram.getListing().getInstructions(f.getBody(), true);
                while (it.hasNext()) {
                    Instruction ins = it.next();
                    println(ins.getAddress() + ": " + ins);
                }
                println("=== END " + tag + " ===");
            }
        } finally {
            ifc.dispose();
        }
    }
}
