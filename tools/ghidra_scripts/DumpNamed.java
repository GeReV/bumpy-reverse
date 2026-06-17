// Decompile a list of functions by name (or seg:off), and follow their direct
// CALL targets one level deep. Args: name-or-addr ... -> stdout.
// @category Bumpy
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import java.util.LinkedHashSet;
import java.util.Set;

public class DumpNamed extends GhidraScript {
    DecompInterface d;
    Set<Address> seen = new LinkedHashSet<>();

    Function findFn(String nm) {
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (f.getName().equals(nm)) return f;
        }
        try {
            Address a = currentProgram.getAddressFactory().getAddress(nm);
            if (a != null) {
                Function f = getFunctionContaining(a);
                if (f == null) { disassemble(a); f = createFunction(a, null); }
                return f;
            }
        } catch (Exception e) { /* not an address */ }
        return null;
    }

    void dump(Function f, int depth) throws Exception {
        if (f == null || seen.contains(f.getEntryPoint())) return;
        seen.add(f.getEntryPoint());
        println("\n===== " + f.getName() + " @ " + f.getEntryPoint() + " =====");
        DecompileResults r = d.decompileFunction(f, 120, monitor);
        if (r != null && r.getDecompiledFunction() != null)
            println(r.getDecompiledFunction().getC());
        else
            println("(decompile failed)");
        if (depth <= 0) return;
        for (Instruction ins = getInstructionAt(f.getEntryPoint());
             ins != null && getFunctionContaining(ins.getAddress()) == f;
             ins = ins.getNext()) {
            if (!ins.getFlowType().isCall()) continue;
            for (Address t : ins.getFlows()) {
                Function g = getFunctionContaining(t);
                if (g == null) { disassemble(t); g = createFunction(t, null); }
                dump(g, depth - 1);
            }
        }
    }

    @Override
    public void run() throws Exception {
        d = new DecompInterface();
        d.openProgram(currentProgram);
        for (String nm : getScriptArgs()) {
            Function f = findFn(nm);
            if (f == null) { println("\n!! not found: " + nm); continue; }
            dump(f, 1);
        }
    }
}
