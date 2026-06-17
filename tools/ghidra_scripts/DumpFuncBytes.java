// Dump raw bytes + Ghidra listing for a segmented code range, for external
// (capstone) disassembly. The blitter sprite_blit_planar_vga (1cec:10e1) does
// not decompile (bad instruction data + jumptable), so we reconstruct it from
// the actual bytes. Args: <seg-hex> <off-hex> <len-hex> <out-file>.
// @category Bumpy
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.SegmentedAddressSpace;
import ghidra.program.model.listing.Instruction;
import java.io.FileOutputStream;

public class DumpFuncBytes extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        int seg = (a.length > 0) ? Integer.parseInt(a[0], 16) : 0x1cec;
        int off = (a.length > 1) ? Integer.parseInt(a[1], 16) : 0x10e1;
        int len = (a.length > 2) ? Integer.parseInt(a[2], 16) : 0x900;
        String out = (a.length > 3) ? a[3] : "blit_bytes.bin";

        SegmentedAddressSpace sp = (SegmentedAddressSpace)
            currentProgram.getAddressFactory().getDefaultAddressSpace();
        Address start = sp.getAddress(seg, off);
        println("range " + Integer.toHexString(seg) + ":" + Integer.toHexString(off)
                + " len=" + Integer.toHexString(len) + " -> " + out);

        byte[] buf = new byte[len];
        int got = 0;
        for (int i = 0; i < len; i++) {
            try {
                buf[i] = getByte(start.add(i));
                got++;
            } catch (Exception e) {
                println("read stopped at +" + Integer.toHexString(i) + ": " + e.getMessage());
                break;
            }
        }
        try (FileOutputStream fos = new FileOutputStream(out)) {
            fos.write(buf, 0, got);
        }
        println("wrote " + got + " bytes (base offset 0x" + Integer.toHexString(off) + ")");

        // Cross-check: Ghidra's own listing for whatever it managed to disassemble.
        println("\n===== Ghidra listing =====");
        Instruction ins = getInstructionAt(start);
        int count = 0;
        while (ins != null && count < 2000) {
            Address ia = ins.getAddress();
            long ioff = ia.getOffset() & 0xFFFF;
            if (ioff < off || ioff >= (off + len)) break;
            StringBuilder bytes = new StringBuilder();
            try {
                for (byte b : ins.getBytes()) bytes.append(String.format("%02x", b & 0xff));
            } catch (Exception e) { bytes.append("??"); }
            println(String.format("%04x: %-14s %s", ioff, bytes.toString(), ins.toString()));
            ins = ins.getNext();
            count++;
        }
        println("(listing " + count + " instructions)");
    }
}
