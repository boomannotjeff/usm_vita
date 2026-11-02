import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.block.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;

// Control-flow export for the static recompiler. args: <out file>
// Lines:  F <entry> <name>            function
//         R <start> <end>             body range (inclusive), one or more per F
//         B <addr>                    basic block start
//         J <from> <to>               computed jump target (switch tables) at instruction <from>
//         I <iat addr> <dll> <name>   import slot
public class ExportCFG extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        BasicBlockModel bbm = new BasicBlockModel(currentProgram);
        Listing listing = currentProgram.getListing();
        ReferenceManager refs = currentProgram.getReferenceManager();
        try (PrintWriter w = new PrintWriter(new BufferedWriter(new FileWriter(a[0])))) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (f.isExternal() || f.isThunk() && f.getBody().getNumAddresses() == 0) continue;
                w.println("F " + f.getEntryPoint() + " " + f.getName());
                for (AddressRange r : f.getBody()) w.println("R " + r.getMinAddress() + " " + r.getMaxAddress());
                CodeBlockIterator bi = bbm.getCodeBlocksContaining(f.getBody(), monitor);
                while (bi.hasNext()) w.println("B " + bi.next().getFirstStartAddress());
                for (Instruction in : listing.getInstructions(f.getBody(), true)) {
                    if (!in.getFlowType().isComputed() || !in.getFlowType().isJump()) continue;
                    for (Reference r : refs.getReferencesFrom(in.getAddress()))
                        if (r.getReferenceType().isFlow()) w.println("J " + in.getAddress() + " " + r.getToAddress());
                }
            }
            SymbolTable st = currentProgram.getSymbolTable();
            for (Symbol s : st.getExternalSymbols()) {
                for (Reference r : refs.getReferencesTo(s.getAddress())) {
                    if (r.getReferenceType() != RefType.DATA) continue;
                    w.println("I " + r.getFromAddress() + " " + s.getParentNamespace().getName() + " " + s.getName());
                }
            }
        }
    }
}
