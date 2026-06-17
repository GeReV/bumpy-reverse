// Experiment: prove that sprite_blit_planar_vga (1cec:10e1) fails to decompile
// for INTRINSIC reasons (computed `jmp ax` dispatch + self-modifying code), not
// the fixable near-branch-wrap bug. Steps: report current state, clear + force a
// clean re-disassembly, patch the 0x11c5 backward near-branch wrap (jmp 0xc24),
// re-decompile, and dump the C + diagnostics. Run on a COPY of the project.
// @category Bumpy
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.SegmentedAddressSpace;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;
import java.io.FileWriter;

public class DiagBlit10e1 extends GhidraScript {
    SegmentedAddressSpace sp;

    String decompile(Function f, DecompInterface d) {
        DecompileResults r = d.decompileFunction(f, 90, monitor);
        if (r == null) {
            return "(null results)";
        }
        if (r.getDecompiledFunction() == null) {
            return "(no decompiled function; error=" + r.getErrorMessage() + ")";
        }
        return r.getDecompiledFunction().getC();
    }

    void report(String tag, String c) throws Exception {
        // telltale signs of a broken decompile
        int pcodeErr = 0, badInstr = 0, jmptbl = 0, switchN = 0;
        for (String line : c.split("\n")) {
            String l = line.toLowerCase();
            if (l.contains("bad instruction data")) badInstr++;
            if (l.contains("jumptable")) jmptbl++;
            if (l.contains("switch(")) switchN++;
        }
        // bogus calls inside a leaf VGA blitter = mis-resolved control flow
        int bogus = 0;
        for (String s : new String[]{"draw_p1_sprite", "present_frame", "render_p1_view",
                "upload_vga_dac_palette", "swi(0x10)", "swi(0x21)", "FUN_1000_"}) {
            int idx = 0;
            while ((idx = c.indexOf(s, idx)) >= 0) { bogus++; idx += s.length(); }
        }
        println("[" + tag + "] lines=" + c.split("\n").length
                + " badInstrWarn=" + badInstr + " jumptableWarn=" + jmptbl
                + " switch()=" + switchN + " bogusCallRefs=" + bogus);
        String path = System.getProperty("user.dir") + "/local/build/diag_" + tag + ".c";
        try (FileWriter w = new FileWriter(path)) { w.write(c); }
        println("    -> " + path);
    }

    @Override
    public void run() throws Exception {
        sp = (SegmentedAddressSpace) currentProgram.getAddressFactory().getDefaultAddressSpace();
        Address ent = sp.getAddress(0x1cec, 0x10e1);
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);

        println("=== BEFORE (current DB state) ===");
        Instruction i0 = getInstructionAt(ent);
        println("instr @ 1cec:10e1 = " + (i0 == null ? "NULL (no instruction defined)" : i0.toString()));
        Function f0 = getFunctionContaining(ent);
        println("function containing = " + (f0 == null ? "NONE" : f0.getName() + " @ " + f0.getEntryPoint()));
        if (f0 != null) {
            report("before", decompile(f0, d));
        } else {
            // define it as-is and decompile
            disassemble(ent);
            Function f = createFunction(ent, "blit_before");
            if (f != null) report("before", decompile(f, d));
            else println("    (could not define function as-is)");
        }

        println("\n=== CLEAR + CLEAN RE-DISASSEMBLE ===");
        Address endA = sp.getAddress(0x1cec, 0x2700);
        clearListing(ent, endA);
        // remove any function bodies in the span so we start clean
        Function existing = getFunctionContaining(ent);
        if (existing != null) removeFunction(existing);
        disassemble(ent);
        Function f1 = createFunction(ent, "blit_clean");
        println("re-disassembled; function = " + (f1 == null ? "FAILED-to-define" : f1.getName()
                + " body=" + (f1.getBody().getNumAddresses()) + " bytes"));
        Instruction i1 = getInstructionAt(ent);
        println("first instr = " + (i1 == null ? "NULL" : i1.toString()));

        println("\n=== PATCH the 0x11c5 backward near-branch wrap (jmp -> 1cec:0c24) ===");
        Address site = sp.getAddress(0x1cec, 0x11c5);
        Address correct = sp.getAddress(0x1cec, 0x0c24);
        Instruction js = getInstructionAt(site);
        if (js == null) {
            println("  (0x11c5 not disassembled in this pass — fall-through never reached it)");
        } else {
            println("  instr @ 11c5 = " + js + "  flows=" + java.util.Arrays.toString(js.getFlows()));
            for (Reference r : js.getReferencesFrom()) {
                if (r.getReferenceType().isFlow()) {
                    currentProgram.getReferenceManager().delete(r);
                }
            }
            js.addOperandReference(0, correct, RefType.UNCONDITIONAL_JUMP, SourceType.USER_DEFINED);
            disassemble(correct);
            println("  patched flow ref -> 1cec:0c24");
        }

        println("\n=== DECOMPILE after clean re-disasm + wrap fix ===");
        if (f1 != null) {
            report("clean", decompile(f1, d));
        }
        // show the computed-jump + self-mod sites are present in the listing
        println("\n=== smoking guns in the listing ===");
        for (int off : new int[]{0x11d3, 0x1163, 0x117e, 0x25e1, 0x25e4}) {
            Address a = sp.getAddress(0x1cec, off);
            Instruction ins = getInstructionAt(a);
            println(String.format("  1cec:%04x  %s", off, ins == null ? "(not disassembled)" : ins.toString()));
        }
    }
}
