// List references TO each named symbol / address (who reads/writes/calls it),
// reporting the containing function. Args: name-or-addr ...
// @category Bumpy
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class Xrefs extends GhidraScript {
    Address resolve(String nm) {
        SymbolIterator it = currentProgram.getSymbolTable().getSymbols(nm);
        if (it.hasNext()) return it.next().getAddress();
        try {
            return currentProgram.getAddressFactory().getAddress(nm);
        } catch (Exception e) { return null; }
    }

    @Override
    public void run() throws Exception {
        for (String nm : getScriptArgs()) {
            Address a = resolve(nm);
            if (a == null) { println("\n!! unresolved: " + nm); continue; }
            println("\n=== refs to " + nm + " @ " + a + " ===");
            ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(a);
            int n = 0;
            while (ri.hasNext()) {
                Reference r = ri.next();
                Address from = r.getFromAddress();
                Function f = getFunctionContaining(from);
                println("  " + from + "  " + r.getReferenceType()
                        + "  in " + (f == null ? "?" : f.getName() + " @ " + f.getEntryPoint()));
                if (++n > 60) { println("  ... (truncated)"); break; }
            }
            if (n == 0) println("  (none)");
        }
    }
}
