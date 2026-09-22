// Export references to an address together with the containing function.
// @category AWL

import java.io.File;
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class ExportReferencesTo extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 2) {
            throw new IllegalArgumentException(
                "Usage: ExportReferencesTo.java <address> <output-path>");
        }

        Address target = toAddr(args[0]);
        try (PrintWriter out = new PrintWriter(new File(args[1]), "UTF-8")) {
            out.printf("References to %s%n", target);
            ReferenceIterator references = currentProgram.getReferenceManager()
                .getReferencesTo(target);
            while (references.hasNext() && !monitor.isCancelled()) {
                Reference reference = references.next();
                Address from = reference.getFromAddress();
                Function function = getFunctionContaining(from);
                String functionName = function == null ? "<no function>" : function.getName();
                out.printf("%s  %-24s  %s%n", from, functionName, reference.getReferenceType());
            }
        }
    }
}
