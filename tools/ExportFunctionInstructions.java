// Exports the instruction listing for one function to a local evidence file.
// @author awl-decomp
// @category Analysis

import java.io.File;
import java.io.PrintWriter;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;

public class ExportFunctionInstructions extends GhidraScript {
    @Override
    protected void run() throws Exception {
        if (getScriptArgs().length < 2) {
            throw new IllegalArgumentException(
                "Usage: ExportFunctionInstructions.java <address> <output-path>");
        }

        String addressText = getScriptArgs()[0];
        String outputPath = getScriptArgs()[1];
        Address address = currentProgram.getAddressFactory()
            .getDefaultAddressSpace().getAddress(addressText);
        Function function = currentProgram.getFunctionManager().getFunctionAt(address);
        if (function == null) {
            throw new IllegalArgumentException("No function found at " + addressText);
        }

        File outputFile = new File(outputPath);
        InstructionIterator instructions = currentProgram.getListing()
            .getInstructions(function.getBody(), true);
        try (PrintWriter writer = new PrintWriter(outputFile)) {
            writer.printf("# Program: %s%n", currentProgram.getName());
            writer.printf("# Function: %s%n", function.getName());
            writer.printf("# Entry: %s%n", function.getEntryPoint());
            while (instructions.hasNext() && !monitor.isCancelled()) {
                Instruction instruction = instructions.next();
                writer.printf("%s  %s%n", instruction.getAddress(), instruction);
            }
        }
        println("Exported instructions for " + function.getName() + " to " +
                outputFile.getAbsolutePath());
    }
}
