// Decompile the copy-protection routine. The protection strings live in DGROUP;
// the code references them by DGROUP-relative offset (recovered from the unpack:
// DGROUP seg 0x204b, load base 0x1010). Scan instructions for those scalar
// offsets and decompile the containing functions.
// @category Bumpy
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;
import java.util.LinkedHashMap;
import java.util.Map;

public class FindProtection extends GhidraScript {
    @Override
    public void run() throws Exception {
        // DGROUP-relative offsets of the protection strings (from find_string_refs.py).
        long[] vals = {0x1331, 0x0606, 0x12e7, 0x12f5, 0x1309, 0x1318};
        String[] lbls = {"Enter the platform number", "INSERT THE OTHER DISK",
                         "YOUR PASSWORD", "ENTER YOUR PASSWORD", "PASSWORD OK", "PASSWORD ERROR"};
        Map<Long, String> want = new LinkedHashMap<>();
        for (int i = 0; i < vals.length; i++) want.put(vals[i], lbls[i]);

        FunctionManager fm = currentProgram.getFunctionManager();
        Map<Address, Function> funcs = new LinkedHashMap<>();

        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            for (int op = 0; op < ins.getNumOperands(); op++) {
                Scalar sc = ins.getScalar(op);
                if (sc == null) continue;
                long v = sc.getUnsignedValue() & 0xFFFFL;
                if (want.containsKey(v)) {
                    Function f = fm.getFunctionContaining(ins.getAddress());
                    println(String.format("REF %s : %-22s -> %#x (%s) func=%s",
                            ins.getAddress(), ins.toString(), v, want.get(v),
                            f == null ? "(none)" : f.getName()));
                    if (f != null) funcs.put(f.getEntryPoint(), f);
                }
            }
        }

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        for (Map.Entry<Address, Function> e : funcs.entrySet()) {
            Function f = e.getValue();
            println("");
            println("===== DECOMP " + f.getName() + " @ " + f.getEntryPoint() + " =====");
            DecompileResults res = decomp.decompileFunction(f, 120, monitor);
            if (res != null && res.getDecompiledFunction() != null)
                println(res.getDecompiledFunction().getC());
            else
                println("   <decompilation failed>");
        }
    }
}
