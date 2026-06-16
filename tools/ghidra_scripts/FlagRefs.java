// Trace the protection pass/fail flag: list all references to a named global and
// decompile the functions that reference it (to find where it is set to -1/fail).
// @category Bumpy
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import java.util.LinkedHashMap;
import java.util.Map;

public class FlagRefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String name = getScriptArgs().length > 0 ? getScriptArgs()[0] : "DAT_203b_119a";
        FunctionManager fm = currentProgram.getFunctionManager();
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        Map<Address, Function> writers = new LinkedHashMap<>();
        SymbolIterator syms = currentProgram.getSymbolTable().getSymbols(name);
        int found = 0;
        while (syms.hasNext()) {
            Symbol s = syms.next();
            found++;
            println("symbol " + s.getName() + " @ " + s.getAddress());
            for (Reference r : getReferencesTo(s.getAddress())) {
                Function f = fm.getFunctionContaining(r.getFromAddress());
                println("  ref " + r.getFromAddress() + " " + r.getReferenceType()
                        + " func=" + (f == null ? "(none)" : f.getName()));
                if (f != null) writers.put(f.getEntryPoint(), f);
            }
        }
        if (found == 0) println("no symbol named " + name);

        for (Function f : writers.values()) {
            println("");
            println("===== " + f.getName() + " @ " + f.getEntryPoint() + " =====");
            DecompileResults res = decomp.decompileFunction(f, 120, monitor);
            if (res != null && res.getDecompiledFunction() != null)
                println(res.getDecompiledFunction().getC());
        }
    }
}
