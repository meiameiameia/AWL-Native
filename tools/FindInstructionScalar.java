// Find instructions whose operands contain a requested scalar value.
// @category AWL

import java.io.File;
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;

public class FindInstructionScalar extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 2) {
            throw new IllegalArgumentException(
                "Usage: FindInstructionScalar.java <hex-value> <output-path>");
        }

        long target = Long.parseUnsignedLong(args[0].replaceFirst("^0[xX]", ""), 16);
        AddressSetView memory = currentProgram.getMemory().getExecuteSet();
        InstructionIterator instructions = currentProgram.getListing().getInstructions(memory, true);

        try (PrintWriter out = new PrintWriter(new File(args[1]), "UTF-8")) {
            out.printf("Instruction scalar matches for 0x%x%n", target);
            while (instructions.hasNext() && !monitor.isCancelled()) {
                Instruction instruction = instructions.next();
                boolean matched = false;
                for (int operand = 0; operand < instruction.getNumOperands() && !matched; ++operand) {
                    for (Object object : instruction.getOpObjects(operand)) {
                        if (object instanceof Scalar &&
                            ((Scalar)object).getUnsignedValue() == target) {
                            matched = true;
                            break;
                        }
                    }
                }
                if (matched) {
                    Function function = getFunctionContaining(instruction.getAddress());
                    String functionName = function == null ? "<no function>" : function.getName();
                    out.printf("%s  %-24s  %s%n", instruction.getAddress(), functionName,
                        instruction.toString());
                }
            }
        }
    }
}
