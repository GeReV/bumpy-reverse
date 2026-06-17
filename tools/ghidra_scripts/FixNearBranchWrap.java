// Fix 16-bit near-branch targets that Ghidra resolved without wrapping the
// offset to 16 bits. In this real-mode image a backward near CALL/JMP (E8/E9
// rel16) whose (next_ip + rel16) overflows 0xFFFF was resolved 0x10000 too high
// (into the DATA block / bad data), corrupting the call graph for the overlay
// code. We recompute the correct same-segment wrapped target, repoint the flow
// reference, and (re)create code/functions there. Then decompile a probe
// function to confirm the decompiler picks up the fix.
//
// Usage (headless): analyzeHeadless <proj> <name> -process <prog>
//                   -postScript FixNearBranchWrap.java [probeFunc]
// @category Bumpy
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.SegmentedAddress;
import ghidra.program.model.address.SegmentedAddressSpace;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.SourceType;
import java.util.ArrayList;
import java.util.List;

public class FixNearBranchWrap extends GhidraScript {
    @Override
    public void run() throws Exception {
        Listing listing = currentProgram.getListing();
        ReferenceManager rm = currentProgram.getReferenceManager();

        int scanned = 0, fixedCalls = 0, fixedJumps = 0;
        List<Address> callTargets = new ArrayList<>();
        List<Address> jumpTargets = new ArrayList<>();

        InstructionIterator it = listing.getInstructions(true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            if (ins.getLength() != 3) continue;          // 16-bit E8/E9 rel16 is 3 bytes
            byte[] b;
            try { b = ins.getBytes(); } catch (Exception e) { continue; }
            int op = b[0] & 0xFF;
            boolean isCall = (op == 0xE8);
            boolean isJmp = (op == 0xE9);
            if (!isCall && !isJmp) continue;
            Address a = ins.getAddress();
            if (!(a instanceof SegmentedAddress)) continue;
            SegmentedAddress sa = (SegmentedAddress) a;
            SegmentedAddressSpace space = (SegmentedAddressSpace) sa.getAddressSpace();
            int rel = (b[1] & 0xFF) | ((b[2] & 0xFF) << 8);
            int seg = sa.getSegment() & 0xFFFF;
            int off = (int) (sa.getSegmentOffset() & 0xFFFF);
            int tgtOff = (off + 3 + rel) & 0xFFFF;        // correct 16-bit-wrapped target
            Address correct;
            try { correct = space.getAddress(seg, tgtOff); } catch (Exception e) { continue; }
            scanned++;

            // Already correct? (the resolved flow includes the wrapped target)
            boolean ok = false;
            for (Address fa : ins.getFlows()) {
                if (fa.equals(correct)) { ok = true; break; }
            }
            if (ok) continue;

            // Repoint the flow reference to the wrapped in-segment target.
            for (Reference r : ins.getReferencesFrom()) {
                if (r.getReferenceType().isFlow()) rm.delete(r);
            }
            RefType rt = isCall ? RefType.UNCONDITIONAL_CALL : RefType.UNCONDITIONAL_JUMP;
            Reference nr = rm.addMemoryReference(a, correct, rt, SourceType.USER_DEFINED, 0);
            rm.setPrimary(nr, true);
            if (isCall) { fixedCalls++; callTargets.add(correct); }
            else { fixedJumps++; jumpTargets.add(correct); }
        }

        // Make sure call targets are code + functions so the decompiler names them.
        int madeCode = 0, madeFunc = 0;
        for (Address t : callTargets) {
            if (getInstructionAt(t) == null) {
                try { disassemble(t); madeCode++; } catch (Exception e) {}
            }
            if (getFunctionAt(t) == null) {
                try { if (createFunction(t, null) != null) madeFunc++; } catch (Exception e) {}
            }
        }
        for (Address t : jumpTargets) {
            if (getInstructionAt(t) == null) {
                try { disassemble(t); madeCode++; } catch (Exception e) {}
            }
        }

        println("FixNearBranchWrap: scanned=" + scanned
                + " fixedCalls=" + fixedCalls + " fixedJumps=" + fixedJumps
                + " disasm=" + madeCode + " newFuncs=" + madeFunc);

        // Verify: decompile a probe function (default blit_sprite_vga) and print it.
        String probe = getScriptArgs().length > 0 ? getScriptArgs()[0] : "blit_sprite_vga";
        Function pf = null;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (f.getName().equals(probe)) { pf = f; break; }
        }
        if (pf != null) {
            DecompInterface d = new DecompInterface();
            d.openProgram(currentProgram);
            DecompileResults r = d.decompileFunction(pf, 120, monitor);
            println("===== " + probe + " @ " + pf.getEntryPoint() + " =====");
            if (r != null && r.getDecompiledFunction() != null)
                println(r.getDecompiledFunction().getC());
            else
                println("(decompile failed)");
        } else {
            println("probe function not found: " + probe);
        }
    }
}
