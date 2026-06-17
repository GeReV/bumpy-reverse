// Annotate the sprite load-transform + blit chain functions discovered after
// FixNearBranchWrap made the overlay call graph legible. Defines each (they are
// reached only through palette_mode dispatch tables, so Ghidra did not auto-create
// them), renames it, and attaches a plate comment. Idempotent.
// Run in the GUI Script Manager (or headless) on the project.
// @category Bumpy
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.SegmentedAddressSpace;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class AnnotateSprites extends GhidraScript {
    // {1cec offset, name, plate comment}
    static final Object[][] FUNCS = {
        {0x0c34, "sprite_bank_relocate_frames",
            "BUMSPJEU load post-process (process_sprites -> sprite_proc_dispatch[pm]): walk the\n"
          + "frame-offset table at DI; byte-swap each big-endian offset and rewrite it as a far\n"
          + "ptr (ES:offset+0x800 into the bank); call sprite_frame_transform per frame until 0:0."},
        {0x0c77, "sprite_frame_transform",
            "Per-frame load transform (sets iRam00010ded=palette_mode). Byte-swaps the 12-byte\n"
          + "frame header (BE->LE: w,h,offsets at [-1..-6]); if pixel-flags&0xc000==0, reformats the\n"
          + "pixel words: palette_mode==0 (CGA) bit-reverses each byte, otherwise (EGA/VGA) byte-\n"
          + "swaps the plane words only. This is the on-disk -> in-memory bank transform."},
        {0x0e29, "sprite_blit_dispatch",
            "Sprite blit entry (dispatch_palette_mode_with_src_ptr[pm=2]): if the sprite-object far\n"
          + "ptr is non-null, fall through to sprite_blit_store_obj."},
        {0x0e34, "sprite_blit_store_obj",
            "Stash the sprite-object far ptr (BX:ES) into blit globals, then sprite_blit_object_list."},
        {0x0e48, "sprite_blit_object_list",
            "Iterate the sprite-object list; per object: test flags (0x80 visible, 0x20 mode, 0x01\n"
          + "align), compute screen cell x/y from object fields, clip-test against the view bounds,\n"
          + "and if visible call sprite_blit_setup."},
        {0x103d, "sprite_blit_setup",
            "Compute the source row offset/ptr and blit width (40 bytes/row); call sprite_blit_clip\n"
          + "then sprite_blit_planar_vga."},
        {0x0f50, "sprite_blit_clip",
            "Compute left/right/top/bottom clip margins and visible width/height for the sprite vs\n"
          + "the view rectangle; return (visible_height|1) when any column is visible, else 0."},
        {0x10e1, "sprite_blit_planar_vga",
            "The planar-VGA masked sprite blitter. Programs the VGA GC/sequencer (0x3ce/0x3cf set/\n"
          + "reset+function, 0x3c4/0x3c5 map-mask) then blits the prepared frame to 0xA000 column-\n"
          + "by-column via a jump-table of unrolled mask+pixel writes (per palette_mode)."},
    };

    @Override
    public void run() throws Exception {
        SegmentedAddressSpace sp = (SegmentedAddressSpace)
            currentProgram.getAddressFactory().getDefaultAddressSpace();
        int done = 0;
        for (Object[] e : FUNCS) {
            Address a = sp.getAddress(0x1cec, (Integer) e[0]);
            String name = (String) e[1];
            String cmt = (String) e[2];
            Function f = getFunctionAt(a);
            if (f == null) {
                if (getInstructionAt(a) == null) disassemble(a);
                f = createFunction(a, name);
            }
            if (f == null) { println("FAILED to define " + name + " @ " + a); continue; }
            f.setName(name, SourceType.USER_DEFINED);
            setPlateComment(a, cmt);
            println("annotated " + name + " @ " + a);
            done++;
        }
        println("AnnotateSprites: " + done + "/" + FUNCS.length + " functions annotated");
    }
}
