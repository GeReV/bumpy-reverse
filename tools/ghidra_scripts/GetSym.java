// Print the address (segment:offset + flat) of named symbols. Args: names...
// @category Bumpy
import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class GetSym extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        for (String name : a) {
            SymbolIterator it = currentProgram.getSymbolTable().getSymbols(name);
            boolean found = false;
            while (it.hasNext()) {
                Symbol s = it.next();
                println(name + " @ " + s.getAddress() + " (flat 0x"
                        + Long.toHexString(s.getAddress().getOffset()) + ")");
                found = true;
            }
            if (!found) {
                println(name + " : NOT FOUND");
            }
        }
    }
}
