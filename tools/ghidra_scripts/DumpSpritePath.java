// Map the live sprite render path. Resolves the palette_mode dispatch table at
// 1cec:2d9c, then recursively defines + decompiles the blit handler chain plus
// the prep/dispatch wrappers. Run AFTER FixNearBranchWrap so calls resolve.
// @category Bumpy
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.SegmentedAddressSpace;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import java.util.LinkedHashSet;
import java.util.Set;

public class DumpSpritePath extends GhidraScript {
    DecompInterface d;
    Set<Address> seen = new LinkedHashSet<>();

    Function ensureFunc(Address a) throws Exception {
        Function f = getFunctionContaining(a);
        if (f == null) {
            disassemble(a);
            f = createFunction(a, null);
        }
        return f;
    }

    void dumpRec(Address a, int depth) throws Exception {
        if (a == null || depth < 0 || seen.contains(a)) return;
        seen.add(a);
        Function f = ensureFunc(a);
        if (f == null) { println("\n-- no function @ " + a); return; }
        println("\n===== " + f.getName() + " @ " + f.getEntryPoint() + " =====");
        DecompileResults r = d.decompileFunction(f, 120, monitor);
        if (r != null && r.getDecompiledFunction() != null)
            println(r.getDecompiledFunction().getC());
        else
            println("(decompile failed)");
        // follow direct CALL targets within the same overlay (depth-limited)
        for (Instruction ins = getInstructionAt(f.getEntryPoint());
             ins != null && getFunctionContaining(ins.getAddress()) == f;
             ins = ins.getNext()) {
            if (!ins.getFlowType().isCall()) continue;
            for (Address t : ins.getFlows()) {
                dumpRec(t, depth - 1);
            }
        }
    }

    @Override
    public void run() throws Exception {
        d = new DecompInterface();
        d.openProgram(currentProgram);
        SegmentedAddressSpace sp = (SegmentedAddressSpace)
            currentProgram.getAddressFactory().getDefaultAddressSpace();
        // LOAD transform: process_sprites -> sprite_proc_dispatch table 1cec:2d09
        int toff = getShort(sp.getAddress(0x1cec, 0x2d09 + 2 * 2)) & 0xFFFF;
        println("LOAD-transform handler[pm=2] -> 1cec:" + Integer.toHexString(toff));
        dumpRec(sp.getAddress(0x1cec, toff), 4);
        // BLIT handler from dispatch table 1cec:2d9c
        Address ent = sp.getAddress(0x1cec, 0x2d9c + 2 * 2);
        int hoff = getShort(ent) & 0xFFFF;
        println("blit handler[pm=2] -> 1cec:" + Integer.toHexString(hoff));
        dumpRec(sp.getAddress(0x1cec, hoff), 4);
        // and the prep routine (the per-frame select/expand)
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (f.getName().equals("prepare_sprite_frames")) { dumpRec(f.getEntryPoint(), 1); break; }
        }
    }
}
