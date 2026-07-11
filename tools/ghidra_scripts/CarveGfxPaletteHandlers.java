// Carve the Loriciel gfx-overlay (seg 1ab9) palette command handlers and the 1cec
// sprite-op dispatch slot handlers as functions, and type/label the per-palette_mode
// command-vector tables as word[3] data (slots = CGA, EGA, VGA). These handlers are
// reached ONLY through the [palette_mode*2 + base] vector tables, so Ghidra's auto-
// analysis never created them. Idempotent; safe to re-run.
//
// Grounded in the static vector-table contents of local/build/unpack/BUMPY_unpacked.exe
// (the cmdvec tables are static initialised data, not runtime-populated) plus a capstone
// disassembly of the 1ab9:0600..06d0 palette region. It VERIFIES that EGA vs VGA differ
// only in the palette stage (16-byte AC-index palette @+0x23 vs 48-byte RGB DAC @+0x33)
// and the palette upload (INT 10h 1002h AC program vs DAC 3c8/3c9), while the whole
// sprite/blit path has EGA slot == VGA slot in every gfx_spriteop table.
//
// Complements AnnotateSprites.java (which already defines the shared EGA/VGA sprite
// handlers 0e29 / 10e1 / sprite_frame_transform 0c77 / ...). Run in the GUI Script
// Manager (or headless) on the BumpyDecomp project.
// @category Bumpy
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.SegmentedAddressSpace;
import ghidra.program.model.data.ArrayDataType;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.WordDataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class CarveGfxPaletteHandlers extends GhidraScript {
    // {seg, offset, name, plate comment}
    static final Object[][] FUNCS = {
        // --- 1ab9 palette / graphics-overlay command handlers ---
        {0x1ab9, 0x0605, "gfx_stage_palette_cga",
            "CGA palette-stage cmdvec slot (bare RET / no-op)."},
        {0x1ab9, 0x0606, "gfx_stage_palette_ega",
            "EGA palette-stage (cmdvec_stage_palette_modes[1]): rep movsw 8 words = the 16-byte\n"
          + "AC-index palette from src+0x23 into draw-object[page]+0x23 (page slot via\n"
          + "gfx_page_slot_offset). This is the per-image AC map the palette-patch produced."},
        {0x1ab9, 0x0620, "gfx_stage_palette_vga",
            "VGA palette-stage (cmdvec_stage_palette_modes[2]): rep movsw 24 words = the 48-byte\n"
          + "RGB DAC palette from src+0x33 into +0x33 (+0x30 extra when palette_mode==5)."},
        {0x1ab9, 0x0661, "gfx_upload_palette_cga",
            "CGA palette-upload cmdvec slot (bare RET / no-op)."},
        {0x1ab9, 0x0662, "gfx_upload_palette_ega",
            "EGA palette-upload (cmdvec_upload_palette_modes[1]): INT 10h AX=1002h - program the 16\n"
          + "Attribute-Controller palette registers (+overscan) from draw-object[page]+0x23. The DAC\n"
          + "stays the BIOS mode-0Dh EGA ramp; the AC indices (0..15) select the fixed EGA colours."},
        {0x1ab9, 0x0677, "gfx_upload_palette_vga",
            "VGA palette-upload (cmdvec_upload_palette_modes[2]): DAC ports 0x3c8/0x3c9, slots 0..7\n"
          + "and 0x10..0x17, three bytes each from draw-object[page]+0x33."},
        {0x1ab9, 0x05b6, "gfx_page_slot_offset",
            "Helper called by the stage/upload handlers: returns the draw-object per-page byte\n"
          + "offset (page * 99) added before the +0x23 / +0x33 palette region."},
        {0x1ab9, 0x0379, "gfx_present_vga",
            "present cmdvec target -> gfx_present_crtc_flip (1ab9:06c1) (same slot for all modes)."},
        {0x1ab9, 0x06c1, "gfx_present_crtc_flip",
            "Frame present: CRTC start-address high XOR 0x20 (display page 0x0000 <-> 0x2000)."},
        {0x1ab9, 0x03d6, "gfx_setvisualpage_impl", "cmdvec_setvisualpage target (all modes)."},
        {0x1ab9, 0x020e, "gfx_cleardevice_impl",   "cmdvec_cleardevice target (all modes)."},
        {0x1ab9, 0x0268, "gfx_device_reset_impl",  "cmdvec_device_reset target (all modes)."},
        {0x1ab9, 0x01ae, "gfx_init_cga",
            "cmdvec_init CGA slot (EGA/VGA init slots are 0 -> secondary dispatch 1ab9:0000)."},
        // --- 1cec sprite-op dispatch slot handlers NOT already defined by AnnotateSprites ---
        // In every gfx_spriteop_* table the EGA slot == VGA slot; only the CGA slot differs.
        {0x1cec, 0x048e, "gfx_spriteop_2d15_cga",
            "gfx_spriteop_tbl_2d15[CGA] (EGA/VGA share sprite_blit_planar_vga 1cec:10e1)."},
        {0x1cec, 0x0c33, "gfx_spriteop_2d43_egavga",
            "gfx_spriteop_tbl_2d43[EGA] == [VGA] (the CGA slot is 0 / no-op)."},
        {0x1cec, 0x0262, "gfx_spriteop_with_src_cga",
            "gfx_spriteop_tbl_with_src[CGA] (EGA/VGA share sprite_blit_dispatch 1cec:0e29)."},
        {0x1cec, 0x0228, "gfx_spriteop_2da8_cga",
            "gfx_spriteop_tbl_2da8[CGA] (EGA/VGA share 1cec:0def)."},
        {0x1cec, 0x0def, "gfx_spriteop_2da8_egavga",
            "gfx_spriteop_tbl_2da8[EGA] == [VGA]."},
    };

    // {seg, offset, label, EOL comment} for the per-palette_mode vector tables (word[3]).
    static final Object[][] TABLES = {
        {0x203b, 0x4dda, "cmdvec_init_modes",         "[CGA,EGA,VGA] init: 01ae / 0 / 0 (0 -> 2nd dispatch 1ab9:0000)"},
        {0x203b, 0x5435, "cmdvec_stage_palette_modes", "[CGA,EGA,VGA] stage: 0605 / 0606(16B AC @+0x23) / 0620(48B RGB @+0x33)"},
        {0x203b, 0x5441, "cmdvec_upload_palette_modes", "[CGA,EGA,VGA] upload: 0661 / 0662(INT10 1002h AC) / 0677(DAC 3c8/3c9)"},
        {0x203b, 0x545d, "cmdvec_cleardevice_modes",   "[CGA,EGA,VGA] cleardevice: 020e / 020e / 020e"},
        {0x203b, 0x5469, "cmdvec_device_reset_modes",  "[CGA,EGA,VGA] device_reset: 0268 / 0268 / 0268"},
        {0x203b, 0x5475, "cmdvec_present_modes",       "[CGA,EGA,VGA] present: 0379 / 0379 / 0379 -> 06c1"},
        {0x203b, 0x5481, "cmdvec_setvisualpage_modes", "[CGA,EGA,VGA] setvisualpage: 03d6 / 03d6 / 03d6"},
        {0x1cec, 0x2d37, "gfx_spriteop_tbl_2d15",      "[CGA,EGA,VGA]: 048e / 10e1 / 10e1 (EGA==VGA)"},
        {0x1cec, 0x2d61, "gfx_spriteop_tbl_2d43",      "[CGA,EGA,VGA]: 0000 / 0c33 / 0c33 (EGA==VGA)"},
        {0x1cec, 0x2d9c, "gfx_spriteop_tbl_with_src",  "[CGA,EGA,VGA]: 0262 / 0e29 / 0e29 (EGA==VGA)"},
        {0x1cec, 0x2dc6, "gfx_spriteop_tbl_2da8",      "[CGA,EGA,VGA]: 0228 / 0def / 0def (EGA==VGA)"},
    };

    @Override
    public void run() throws Exception {
        SegmentedAddressSpace sp = (SegmentedAddressSpace)
            currentProgram.getAddressFactory().getDefaultAddressSpace();

        // 1) type + label the per-palette_mode command-vector tables as word[3].
        DataType w3 = new ArrayDataType(WordDataType.dataType, 3, 2);
        int tdone = 0;
        for (Object[] e : TABLES) {
            Address a = sp.getAddress((Integer) e[0], (Integer) e[1]);
            try {
                clearListing(a, a.add(5));
                createData(a, w3);
                createLabel(a, (String) e[2], true);
                setEOLComment(a, (String) e[3]);
                tdone++;
            } catch (Exception ex) {
                println("TABLE FAILED @ " + a + ": " + ex);
            }
        }

        // 2) carve + name the vector-reached handlers.
        int fdone = 0;
        for (Object[] e : FUNCS) {
            Address a = sp.getAddress((Integer) e[0], (Integer) e[1]);
            String name = (String) e[2];
            String cmt = (String) e[3];
            Function f = getFunctionAt(a);
            if (f == null) {
                if (getInstructionAt(a) == null) disassemble(a);
                f = createFunction(a, name);
            }
            if (f == null) {
                println("FUNC FAILED @ " + a + " (" + name + ")");
                continue;
            }
            f.setName(name, SourceType.USER_DEFINED);
            setPlateComment(a, cmt);
            fdone++;
            println("carved " + name + " @ " + a);
        }

        println("CarveGfxPaletteHandlers: " + fdone + "/" + FUNCS.length + " functions, "
              + tdone + "/" + TABLES.length + " tables");
    }
}
