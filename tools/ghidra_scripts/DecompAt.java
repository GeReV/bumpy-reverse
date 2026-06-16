// Create a function at a given (segmented) address if none exists, then
// decompile it. Arg: address like "1c28:0194".
// @category Bumpy
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        String a = getScriptArgs().length > 0 ? getScriptArgs()[0] : "1c28:0194";
        Address addr = currentProgram.getAddressFactory().getAddress(a);
        if (addr == null) { println("bad address: " + a); return; }
        Function f = getFunctionContaining(addr);
        if (f == null) {
            disassemble(addr);
            f = createFunction(addr, null);
        }
        if (f == null) { println("could not create function at " + a); return; }
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        DecompileResults r = d.decompileFunction(f, 180, monitor);
        println("===== " + f.getName() + " @ " + f.getEntryPoint() + " =====");
        if (r != null && r.getDecompiledFunction() != null)
            println(r.getDecompiledFunction().getC());
        else
            println("decompile failed");
    }
}
