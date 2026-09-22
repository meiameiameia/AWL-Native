// Exports all functions in the current program to a CSV file.
// @author awl-decomp
// @category Analysis
// @keybinding
// @menupath
// @toolbar

import java.io.File;
import java.io.PrintWriter;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

public class ExportFunctions extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String outputPath = getScriptArgs().length > 0 ? getScriptArgs()[0] : "functions.csv";
        File file = new File(outputPath);
        try (PrintWriter writer = new PrintWriter(file)) {
            writer.println("Address,Name,Size,CallingConvention");
            FunctionIterator iter = currentProgram.getFunctionManager().getFunctions(true);
            while (iter.hasNext()) {
                Function func = iter.next();
                writer.printf("%s,%s,%d,%s\n",
                    func.getEntryPoint().toString(),
                    func.getName(),
                    func.getBody().getNumAddresses(),
                    func.getCallingConventionName()
                );
            }
        }
        println("Exported functions to " + file.getAbsolutePath());
    }
}
