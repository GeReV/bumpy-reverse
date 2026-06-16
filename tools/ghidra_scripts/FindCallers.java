// Decompile callers of the protection-challenge function FUN_1000_4015, to find
// the answer comparison and where the protection is invoked.
// @category Bumpy
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.symbol.Reference;
import java.util.LinkedHashMap;
import java.util.Map;

public class FindCallers extends GhidraScript {
    @Override
    public void run() throws Exception {
        String targetName = "FUN_1000_4015";
        if (getScriptArgs().length > 0) targetName = getScriptArgs()[0];
        FunctionManager fm = currentProgram.getFunctionManager();
        Function target = null;
        for (Function f : fm.getFunctions(true)) {
            if (f.getName().equals(targetName)) { target = f; break; }
        }
        if (target == null) { println("target not found: " + targetName); return; }
        Address ep = target.getEntryPoint();
        println("callers of " + targetName + " @ " + ep + ":");

        Map<Address, Function> callers = new LinkedHashMap<>();
        for (Reference r : getReferencesTo(ep)) {
            Function f = fm.getFunctionContaining(r.getFromAddress());
            println("  from " + r.getFromAddress() + " type=" + r.getReferenceType()
                    + " func=" + (f == null ? "(none)" : f.getName()));
            if (f != null) callers.put(f.getEntryPoint(), f);
        }

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        for (Function f : callers.values()) {
            println("");
            println("===== CALLER " + f.getName() + " @ " + f.getEntryPoint() + " =====");
            DecompileResults res = decomp.decompileFunction(f, 120, monitor);
            if (res != null && res.getDecompiledFunction() != null)
                println(res.getDecompiledFunction().getC());
            else
                println("   <decompile failed>");
        }
    }
}
