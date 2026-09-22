// Decompiles a function at a given address and saves it to a file.
// @author awl-decomp
// @category Analysis

import java.io.File;
import java.io.PrintWriter;
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompileFunction extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String addrStr = getScriptArgs().length > 0 ? getScriptArgs()[0] : "80003154";
        String outPath = getScriptArgs().length > 1 ? getScriptArgs()[1] : "decompiled.c";
        
        Address addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(addrStr);
        Function func = currentProgram.getFunctionManager().getFunctionAt(addr);
        
        if (func == null) {
            System.err.println("No function found at " + addrStr);
            return;
        }

        DecompInterface decompInterface = new DecompInterface();
        decompInterface.openProgram(currentProgram);
        DecompileResults results = decompInterface.decompileFunction(func, 30, monitor);
        
        if (results != null && results.getDecompiledFunction() != null) {
            String code = results.getDecompiledFunction().getC();
            File file = new File(outPath);
            try (PrintWriter writer = new PrintWriter(file)) {
                writer.print(code);
            }
            println("Decompiled " + func.getName() + " to " + file.getAbsolutePath());
        } else {
            System.err.println("Decompilation failed for " + func.getName());
        }
    }
}
