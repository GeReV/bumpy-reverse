#!/usr/bin/env python3
"""Generate src/anim_data.c — the in-level entity placement tables, extracted
verbatim from BUMPY_unpacked.exe DGROUP and ground-truthed against the decomp + asm
(spawn_and_draw_level_entities 1000:2a78, draw_anim_channels_a 1000:165e,
draw_anim_channels_b 1000:17c7).

DGROUP file base = 0x11440 (DGROUP offset X -> file 0x11440+X), same as worldmap_data.c.

These DGROUP tables are bare zero-init arrays in the reconstructed C (no static
initializer in the original — they are runtime far-ptr / coord blobs).  init_anim_data()
fills the byte tables verbatim, REBUILDS the A/B frame far-ptr tables as runtime far
pointers into the local descriptor blobs (FP_OFF/FP_SEG — the original's link-seg 0x103b
is relocated by DOS at load; we cannot reproduce that fixup for a from-scratch Open
Watcom relink, so we rebuild the pointers against the runtime segment), and points the
coord-table pointers at their blobs.  Mirrors the worldmap_data.c relocation pattern.

Tables (all confirmed via mcp__ghidra disassemble 1000:2a78 + xxd):
  LAYER A (tilemap[+0x00 + cell]):
    spawn_a_type_tbl @0x3d3a, 0x30 bytes     cv -> type           (MOV [SI+0x3d3a])
    descriptors      @0x37be, 197*4 bytes    (yoff u16, frame u16); frame far-ptr base
    anim_a_frame_tbl @0x3d6a, 199*4 far-ptr  type -> &desc[IDX], link-seg 0x103b
                                             *** NOT LINEAR: types 1..95 map to
                                             desc[type-1] but types 96..197 are a
                                             scrambled permutation (88/197 deviate;
                                             first: type 96 -> 0x3942, not 0x393a),
                                             and entries 0 and 198 are NULL.  The real
                                             per-type desc index is extracted verbatim
                                             below (g_anim_a_frame_idx). ***
    anim_a_grid_tbl  @0x32be, 48*4 bytes      erase (X,Y) tile
    anim_posA_tbl    @0xf4,   48*4 bytes      draw  (X,Y) px
  LAYER B (tilemap[+0x30 + cell], col 7 skipped):
    spawn_b_type_tbl @0x4086, 0x20 bytes     cv -> type           (MOV [SI+0x4086])
    descriptors      @0x3ad2, 106*4 bytes    (yoff u16, frame u16); contiguous after A
    anim_b_frame_tbl @0x40a6, type*4 far-ptr  type -> &desc[type-1], link-seg 0x103b
                                             (verified genuinely linear, 0 deviations
                                             across indices 1..106; NULL at 0 and 107)
    anim_b_grid_tbl  @0x343e, 48*4 bytes      erase (X,Y) tile
    anim_posB_tbl    @0x3f4,  48*4 bytes      draw  (X,Y) px
  LAYER C (tilemap[+0x60 + cell]) — static sprites, NO type table:
    p2_cell_coord_tbl@0x274,  48*4 bytes      (X,Y) px; index = col*4 + row*32; frame=cv+0x179
  PLAYER 2:
    spawn_p2_frame_tbl@0x2546, 0x20 u16       header[+0x96]*2 -> p2_frame_base
                                             *** words 0 and 19 are loader-relocated
                                             far-ptr SEG halves (EXE reloc entries at
                                             DGROUP+0x2546 and +0x256c): at runtime the
                                             DOS loader patches them to the runtime
                                             DGROUP segment.  init_anim_data reproduces
                                             that fixup.  Words 20+ are neighbouring
                                             copy-protection string bytes (the engine's
                                             unbounded header-byte index can reach
                                             them; kept verbatim for fidelity). ***

NOTE: the layer-A descriptor region is exactly [0x37be, 0x3ad2) = 197 entries (the
real frame table references desc indices 0..196 only); layer-B desc[0] starts at
0x3ad2.  An earlier revision copied 198 layer-A descriptors on the (wrong) assumption
that type 198 mapped linearly past the end — the binary's type-198 entry is NULL.

Run from repo root:  uv run python tools/extract/gen_anim_data.py
"""
EXE = "local/originals/unpacked/BUMPY_unpacked.exe"
DGROUP_FILE_BASE = 0x11440
OUT = "src/anim_data.c"

# Byte blobs to emit.  (c_name, dgroup_off, length, far)
TABLES = [
    # layer A
    ("g_anim_a_desc",   0x37be, 197 * 4, True),
    ("g_anim_a_grid",   0x32be, 48 * 4,  True),
    ("g_anim_posA",     0xf4,   48 * 4,  True),
    ("g_spawn_a_type",  0x3d3a, 0x30,    False),
    # layer B
    ("g_anim_b_desc",   0x3ad2, 106 * 4, True),
    ("g_anim_b_grid",   0x343e, 48 * 4,  True),
    ("g_anim_posB",     0x3f4,  48 * 4,  True),
    ("g_spawn_b_type",  0x4086, 0x20,    False),
    # layer C
    ("g_posC",          0x274,  48 * 4,  True),
    # player 2 (u16 frame table; emitted as raw bytes, copied verbatim into the u16 tbl)
    ("g_spawn_p2_frame",0x2546, 0x20 * 2, False),
]
NDESC_A = 197        # layer-A desc entries [0x37be, 0x3ad2); indices 0..196 referenced
FRAME_A_TBL = 0x3d6a # layer-A frame far-ptr table, 199 entries (types 0..198)
FRAME_A_N   = 199
DESC_A_BASE = 0x37be
NDESC_B = 106        # layer-B type 1..106 -> desc[type-1] (verified linear)
P2_FRAME_RELOC_WORDS = (0, 19)  # loader-relocated seg-half words in spawn_p2_frame_tbl

# Layer-A tile-def table (apply_cell_animation, 1000:69aa): action_code -> far ptr to a
# tile_def {tile_word @+0, stream_far_ptr @+2/+4}.  The tile_def's stream is the per-tile
# animation cmd-byte sequence (0xff-terminated) that step_anim_channels_a walks.  Table is
# zero-init in anim.c, so animated tiles got a NULL tile_def -> garbage stream -> wrong
# frames.  Same blob+internal-far-ptr relocation as move_scripts.c.
TILEDEF_A_TBL  = 0x2ede   # anim_a_tiledef_tbl (action*4 far-ptrs, link-seg 0x103b)
TILEDEF_A_BASE = 0x2811   # blob[0] DGROUP offset (first stream; blob ends at the table)
LINK_SEG       = 0x103b

# Layer-B/contact tile-def table (apply_contact_action, 1000:6a89): action_code*4 ->
# far ptr at DGROUP 0x3256 to a tile_def {tile_word u16, stream_far_ptr} — the exact
# channel-B analogue of the layer-A tiledef machinery.  player.c's contact_tiledef_tbl
# carries the static link-time far-ptr image (seg 0x103b) but was never relocated in
# the playable build, and its blob was not dumped -> garbage stream on tile contacts.
# Blob region verified: [0x3062, 0x3256) (starts at the END of the layer-A tiledef
# table at 0x2ede+97*4 = 0x3062 — action 1's stream lives at 0x3062); all 23 tile_defs
# + streams fall inside it and every stream is 0xff-terminated in-region.
TILEDEF_C_TBL  = 0x3256   # contact_tiledef_tbl source (24 entries, actions 0..0x17)
TILEDEF_C_BASE = 0x3062   # blob[0] DGROUP offset
TILEDEF_C_NACT = 24

# RE-ENABLED 2026-07-02: the dead 64 KB flat host_framebuffer was reclaimed
# (host_render.c host_fb_init under HOST_FB_16K), clearing the conventional-memory
# cliff that gated this ~2.2 KB blob (it had been disabled 2026-06-30 because the
# level failed to load past the cliff).
EMIT_TILEDEF = True


def read(off: int, n: int) -> bytes:
    with open(EXE, "rb") as f:
        f.seek(DGROUP_FILE_BASE + off)
        return f.read(n)


def u16_at(buf: bytes, off: int) -> int:
    return buf[off] | (buf[off + 1] << 8)


def read_frame_a_idx() -> list[int]:
    """Extract the REAL layer-A frame table as per-type descriptor indices.

    Reads the 199 far-ptr entries at DGROUP 0x3d6a and translates each entry's
    offset half into a g_anim_a_desc index ((off - 0x37be) / 4).  NULL entries
    (types 0 and 198) become the sentinel 0xff.  Asserts every referenced offset
    is a valid, aligned descriptor slot with the link seg 0x103b — any violation
    means the table layout assumption broke and extraction must stop."""
    raw = read(FRAME_A_TBL, FRAME_A_N * 4)
    idx: list[int] = []
    for t in range(FRAME_A_N):
        off = u16_at(raw, t * 4 + 0)
        seg = u16_at(raw, t * 4 + 2)
        if off == 0 and seg == 0:
            idx.append(0xFF)
            continue
        assert seg == LINK_SEG, "type %d: seg 0x%04x != 0x103b" % (t, seg)
        assert DESC_A_BASE <= off < DESC_A_BASE + NDESC_A * 4, \
            "type %d: off 0x%04x outside desc region" % (t, off)
        assert (off - DESC_A_BASE) % 4 == 0, "type %d: unaligned off 0x%04x" % (t, off)
        d = (off - DESC_A_BASE) // 4
        assert d < 0xFF, "type %d: desc index %d overflows u8" % (t, d)
        idx.append(d)
    assert idx[0] == 0xFF and idx[198] == 0xFF, "expected NULL at types 0 and 198"
    return idx


def read_tiledef_a() -> tuple[bytes, list[int], list[int]]:
    """Returns (blob_bytes, action_offs[list], uniq_tiledef_offs[sorted list]).
       action_offs[a] = engine off of action a's tile_def, 0 for a null action."""
    full = read(0, TILEDEF_A_TBL + 0x400)   # table + slack
    action_offs = []
    a = 0
    while True:
        off = u16_at(full, TILEDEF_A_TBL + a * 4 + 0)
        seg = u16_at(full, TILEDEF_A_TBL + a * 4 + 2)
        if off == 0 and seg == 0:
            action_offs.append(0)
        elif seg == LINK_SEG and TILEDEF_A_BASE <= off < TILEDEF_A_TBL:
            sseg = u16_at(full, off + 4)
            soff = u16_at(full, off + 2)
            if sseg == LINK_SEG and TILEDEF_A_BASE <= soff < TILEDEF_A_TBL:
                action_offs.append(off)
            else:
                break
        else:
            break
        a += 1
        if a > 256:
            break
    blob = full[TILEDEF_A_BASE:TILEDEF_A_TBL]
    uniq = sorted(set(o for o in action_offs if o != 0))
    return blob, action_offs, uniq


def read_tiledef_c() -> tuple[bytes, list[int], list[int]]:
    """Contact (layer-B) tile-def blob + per-action tdef offsets (0 = null action).
       Same shape as read_tiledef_a but for the 24-entry table at 0x3256."""
    full = read(0, TILEDEF_C_TBL + TILEDEF_C_NACT * 4)
    action_offs: list[int] = []
    for a in range(TILEDEF_C_NACT):
        off = u16_at(full, TILEDEF_C_TBL + a * 4 + 0)
        seg = u16_at(full, TILEDEF_C_TBL + a * 4 + 2)
        if off == 0 and seg == 0:
            action_offs.append(0)
            continue
        assert seg == LINK_SEG and TILEDEF_C_BASE <= off < TILEDEF_C_TBL, \
            "contact action %d: bad tdef %04x:%04x" % (a, seg, off)
        soff = u16_at(full, off + 2)
        sseg = u16_at(full, off + 4)
        assert sseg == LINK_SEG and TILEDEF_C_BASE <= soff < TILEDEF_C_TBL, \
            "contact action %d: bad stream %04x:%04x" % (a, sseg, soff)
        action_offs.append(off)
    blob = full[TILEDEF_C_BASE:TILEDEF_C_TBL]
    uniq = sorted(set(o for o in action_offs if o != 0))
    return blob, action_offs, uniq


def emit_array(name: str, data: bytes, far: bool = True) -> str:
    q = "u8 __far " if far else "const u8 "
    lines = ["%s%s[%d] = {" % (q, name, len(data))]
    for i in range(0, len(data), 12):
        chunk = data[i:i + 12]
        lines.append("    " + " ".join("0x%02x," % b for b in chunk))
    lines.append("};")
    return "\n".join(lines)


def main() -> None:
    p = []
    p.append('/* anim_data.c — GENERATED by tools/extract/gen_anim_data.py from')
    p.append(' * BUMPY_unpacked.exe DGROUP. DO NOT EDIT BY HAND. In-level entity placement')
    p.append(' * tables: layer A (platform bars) / layer B / layer C (static sprites) / P2.')
    p.append(' * Ground-truthed vs the decomp + asm: spawn_and_draw_level_entities (1000:2a78),')
    p.append(' * draw_anim_channels_a (1000:165e), draw_anim_channels_b (1000:17c7).')
    p.append(' * init_anim_data() MUST run once at boot (beside init_worldmap_data) before')
    p.append(' * spawn_and_draw_level_entities. Mirrors the worldmap_data.c relocation pattern. */')
    p.append('#include "bumpy.h"')
    p.append('#include <i86.h>   /* FP_OFF / FP_SEG (MK_FP family) */')
    p.append("")
    blobs = {}
    for name, off, ln, far in TABLES:
        data = read(off, ln)
        blobs[name] = data
        p.append("/* DGROUP 0x%x, %d bytes */" % (off, ln))
        p.append("static " + emit_array(name, data, far=far))
        p.append("")
    # ── layer-A frame table: REAL per-type descriptor indices (NOT linear) ──────
    frame_a_idx = read_frame_a_idx()
    p.append("/* DGROUP 0x%x — layer-A frame table translated to per-type descriptor" % FRAME_A_TBL)
    p.append(" * indices ((entry_off - 0x37be)/4; 0xff = NULL entry).  The binary's table")
    p.append(" * is NOT linear: types 1..95 map to desc[type-1] but 96..197 are a scrambled")
    p.append(" * permutation (88/197 deviate) and types 0/198 are NULL. */")
    p.append("static " + emit_array("g_anim_a_frame_idx", bytes(frame_a_idx), far=True))
    p.append("")
    # ── layer-A tile-def blob + offset tables (gated: see EMIT_TILEDEF note) ────
    if EMIT_TILEDEF:
        td_blob, td_action_offs, td_uniq = read_tiledef_a()
        p.append("/* DGROUP 0x%x..0x%x — layer-A tile-def + anim-stream blob (%d bytes). */"
                 % (TILEDEF_A_BASE, TILEDEF_A_TBL, len(td_blob)))
        p.append("static " + emit_array("g_tiledef_a_blob", td_blob, far=True))
        p.append("")
        p.append("/* action_code -> tile_def engine offset (0 = null action, no anim). */")
        p.append("#define TILEDEF_A_NACT %d" % len(td_action_offs))
        p.append("#define TILEDEF_A_BASE 0x%xu" % TILEDEF_A_BASE)
        p.append("static const u16 g_tiledef_a_off[TILEDEF_A_NACT] = {")
        for i in range(0, len(td_action_offs), 8):
            chunk = td_action_offs[i:i + 8]
            p.append("    " + " ".join("0x%04x," % o for o in chunk))
        p.append("};")
        p.append("")
        p.append("/* Unique tile_def offsets (each tile_def's internal stream far ptr is")
        p.append("   relocated ONCE, even when shared by several actions). */")
        p.append("#define TILEDEF_A_NUNIQ %d" % len(td_uniq))
        p.append("static const u16 g_tiledef_a_uniq[TILEDEF_A_NUNIQ] = {")
        for i in range(0, len(td_uniq), 8):
            chunk = td_uniq[i:i + 8]
            p.append("    " + " ".join("0x%04x," % o for o in chunk))
        p.append("};")
        p.append("")
        # ── contact (layer-B) tile-def blob + tables — same machinery ─────────
        tc_blob, tc_action_offs, tc_uniq = read_tiledef_c()
        p.append("/* DGROUP 0x%x..0x%x — contact/layer-B tile-def + anim-stream blob (%d bytes)."
                 % (TILEDEF_C_BASE, TILEDEF_C_TBL, len(tc_blob)))
        p.append(" * apply_contact_action (player.c) reads tile_def far ptrs from")
        p.append(" * contact_tiledef_tbl (source DGROUP 0x3256); the tile_defs + their 0xff-")
        p.append(" * terminated cmd-byte streams live in this blob. */")
        p.append("static " + emit_array("g_tiledef_c_blob", tc_blob, far=True))
        p.append("")
        p.append("/* action_code -> contact tile_def engine offset (0 = null action). */")
        p.append("#define TILEDEF_C_NACT %d" % len(tc_action_offs))
        p.append("#define TILEDEF_C_BASE 0x%xu" % TILEDEF_C_BASE)
        p.append("static const u16 g_tiledef_c_off[TILEDEF_C_NACT] = {")
        for i in range(0, len(tc_action_offs), 8):
            chunk = tc_action_offs[i:i + 8]
            p.append("    " + " ".join("0x%04x," % o for o in chunk))
        p.append("};")
        p.append("")
        p.append("#define TILEDEF_C_NUNIQ %d" % len(tc_uniq))
        p.append("static const u16 g_tiledef_c_uniq[TILEDEF_C_NUNIQ] = {")
        for i in range(0, len(tc_uniq), 8):
            chunk = tc_uniq[i:i + 8]
            p.append("    " + " ".join("0x%04x," % o for o in chunk))
        p.append("};")
        p.append("")
    p.append("/* The bare tables the engine code reads (defined in spawn.c / anim.c / player2.c). */")
    p.append("extern u8        spawn_a_type_tbl[];    /* spawn.c   bare array  */")
    p.append("extern u8        anim_a_frame_tbl[];    /* anim.c    far-ptr table (off@+0, seg@+2) */")
    p.append("extern u8 __far *anim_a_grid_tbl;       /* anim.c    pointer     */")
    p.append("extern u8 __far *anim_posA_tbl;         /* anim.c    pointer     */")
    p.append("extern u8        spawn_b_type_tbl[];    /* spawn.c   bare array  */")
    p.append("extern u8        anim_b_frame_tbl[];    /* anim.c    far-ptr table (off@+0, seg@+2) */")
    p.append("extern u8 __far *anim_b_grid_tbl;       /* anim.c    pointer     */")
    p.append("extern u8 __far *anim_posB_tbl;         /* anim.c    pointer     */")
    p.append("extern u8 __far *p2_cell_coord_tbl;     /* player2.c pointer (layer-C posC) */")
    p.append("extern u16       spawn_p2_frame_tbl[];  /* spawn.c   u16 frame table */")
    if EMIT_TILEDEF:
        p.append("extern u8        anim_a_tiledef_tbl[];  /* anim.c    action*4 far-ptr table */")
        p.append("extern u8        contact_tiledef_tbl[]; /* player.c  action*4 far-ptr table */")
    p.append("")
    p.append("/* Layer B is verified LINEAR in the binary (type N -> desc[N-1], 0 deviations")
    p.append(" * across 1..106); layer A is NOT and uses the extracted index table below. */")
    p.append("static void reloc_frame_tbl(u8 *tbl, u8 __far *desc, u16 ndesc)")
    p.append("{")
    p.append("    /* type 0 = null; type N (1..ndesc) -> &desc[(N-1)*4] as a runtime far ptr. */")
    p.append("    u16 type;")
    p.append("    *(u16 *)(tbl + 0) = 0u;")
    p.append("    *(u16 *)(tbl + 2) = 0u;")
    p.append("    for (type = 1u; type <= ndesc; type++) {")
    p.append("        u8 __far *d = &desc[(type - 1u) * 4u];")
    p.append("        *(u16 *)(tbl + type * 4u + 0u) = FP_OFF(d);")
    p.append("        *(u16 *)(tbl + type * 4u + 2u) = FP_SEG(d);")
    p.append("    }")
    p.append("}")
    p.append("")
    p.append("static void reloc_frame_tbl_idx(u8 *tbl, u8 __far *desc,")
    p.append("                                const u8 __far *idx, u16 ntypes)")
    p.append("{")
    p.append("    /* type T -> &desc[idx[T]*4] (0xff = NULL entry), per the binary's real")
    p.append("       (non-linear) table at DGROUP 0x3d6a. */")
    p.append("    u16 type;")
    p.append("    for (type = 0u; type < ntypes; type++) {")
    p.append("        if (idx[type] == 0xffu) {")
    p.append("            *(u16 *)(tbl + type * 4u + 0u) = 0u;")
    p.append("            *(u16 *)(tbl + type * 4u + 2u) = 0u;")
    p.append("        } else {")
    p.append("            u8 __far *d = &desc[(u16)idx[type] * 4u];")
    p.append("            *(u16 *)(tbl + type * 4u + 0u) = FP_OFF(d);")
    p.append("            *(u16 *)(tbl + type * 4u + 2u) = FP_SEG(d);")
    p.append("        }")
    p.append("    }")
    p.append("}")
    p.append("")
    p.append("void init_anim_data(void)")
    p.append("{")
    p.append("    u16 i;")
    p.append("    /* layer A */")
    p.append("    for (i = 0u; i < %du; i++) { spawn_a_type_tbl[i] = g_spawn_a_type[i]; }" % len(blobs["g_spawn_a_type"]))
    p.append("    reloc_frame_tbl_idx(anim_a_frame_tbl, g_anim_a_desc, g_anim_a_frame_idx, %du);" % FRAME_A_N)
    p.append("    anim_a_grid_tbl = (u8 __far *)g_anim_a_grid;")
    p.append("    anim_posA_tbl   = (u8 __far *)g_anim_posA;")
    p.append("    /* layer B */")
    p.append("    for (i = 0u; i < %du; i++) { spawn_b_type_tbl[i] = g_spawn_b_type[i]; }" % len(blobs["g_spawn_b_type"]))
    p.append("    reloc_frame_tbl(anim_b_frame_tbl, g_anim_b_desc, %du);" % NDESC_B)
    p.append("    anim_b_grid_tbl = (u8 __far *)g_anim_b_grid;")
    p.append("    anim_posB_tbl   = (u8 __far *)g_anim_posB;")
    p.append("    /* layer C (also used by player2 movement) */")
    p.append("    p2_cell_coord_tbl = (u8 __far *)g_posC;")
    p.append("    /* player 2 frame table (verbatim u16 values, little-endian bytes copied). */")
    p.append("    for (i = 0u; i < %du; i++) {" % len(blobs["g_spawn_p2_frame"]))
    p.append("        ((u8 *)spawn_p2_frame_tbl)[i] = g_spawn_p2_frame[i];")
    p.append("    }")
    p.append("    /* Words 0 and 19 are loader-relocated far-ptr SEG halves in the original")
    p.append("       (EXE reloc entries at DGROUP+0x2546/+0x256c): at load the DOS loader")
    p.append("       patches the link seg 0x103b to the runtime DGROUP segment.  Reproduce")
    p.append("       that fixup with our runtime DGROUP (FP_SEG of a DGROUP object). */")
    for w in P2_FRAME_RELOC_WORDS:
        p.append("    spawn_p2_frame_tbl[%d] = FP_SEG((void __far *)spawn_p2_frame_tbl);" % w)
    if EMIT_TILEDEF:
      p.append("    /* layer-A tile-def table + blob-internal stream far ptrs (apply_cell_animation).")
      p.append("       Relocate each unique tile_def's stream ptr ONCE, then fill anim_a_tiledef_tbl. */")
      p.append("    {")
      p.append("        u16       tbase = FP_OFF((void __far *)g_tiledef_a_blob);")
      p.append("        u16       tseg  = FP_SEG((void __far *)g_tiledef_a_blob);")
      p.append("        u16       a, td, soff;")
      p.append("        for (a = 0u; a < TILEDEF_A_NUNIQ; a++) {")
      p.append("            td   = (u16)(g_tiledef_a_uniq[a] - TILEDEF_A_BASE);   /* blob index */")
      p.append("            soff = *(u16 __far *)(g_tiledef_a_blob + td + 2);     /* engine stream off */")
      p.append("            *(u16 __far *)(g_tiledef_a_blob + td + 2) = (u16)(tbase + (soff - TILEDEF_A_BASE));")
      p.append("            *(u16 __far *)(g_tiledef_a_blob + td + 4) = tseg;")
      p.append("        }")
      p.append("        for (a = 0u; a < TILEDEF_A_NACT; a++) {")
      p.append("            if (g_tiledef_a_off[a] == 0u) {")
      p.append("                *(u16 *)(anim_a_tiledef_tbl + a * 4 + 0) = 0u;")
      p.append("                *(u16 *)(anim_a_tiledef_tbl + a * 4 + 2) = 0u;")
      p.append("            } else {")
      p.append("                *(u16 *)(anim_a_tiledef_tbl + a * 4 + 0) =")
      p.append("                    (u16)(tbase + (g_tiledef_a_off[a] - TILEDEF_A_BASE));")
      p.append("                *(u16 *)(anim_a_tiledef_tbl + a * 4 + 2) = tseg;")
      p.append("            }")
      p.append("        }")
      p.append("    }")
      p.append("    /* contact/layer-B tile-def table + blob (apply_contact_action) — same")
      p.append("       relocate-once machinery; rewrites player.c's contact_tiledef_tbl static")
      p.append("       link-time far ptrs (seg 0x103b, never valid at runtime) to the blob. */")
      p.append("    {")
      p.append("        u16       tbase = FP_OFF((void __far *)g_tiledef_c_blob);")
      p.append("        u16       tseg  = FP_SEG((void __far *)g_tiledef_c_blob);")
      p.append("        u16       a, td, soff;")
      p.append("        for (a = 0u; a < TILEDEF_C_NUNIQ; a++) {")
      p.append("            td   = (u16)(g_tiledef_c_uniq[a] - TILEDEF_C_BASE);   /* blob index */")
      p.append("            soff = *(u16 __far *)(g_tiledef_c_blob + td + 2);     /* engine stream off */")
      p.append("            *(u16 __far *)(g_tiledef_c_blob + td + 2) = (u16)(tbase + (soff - TILEDEF_C_BASE));")
      p.append("            *(u16 __far *)(g_tiledef_c_blob + td + 4) = tseg;")
      p.append("        }")
      p.append("        for (a = 0u; a < TILEDEF_C_NACT; a++) {")
      p.append("            if (g_tiledef_c_off[a] == 0u) {")
      p.append("                *(u16 *)(contact_tiledef_tbl + a * 4 + 0) = 0u;")
      p.append("                *(u16 *)(contact_tiledef_tbl + a * 4 + 2) = 0u;")
      p.append("            } else {")
      p.append("                *(u16 *)(contact_tiledef_tbl + a * 4 + 0) =")
      p.append("                    (u16)(tbase + (g_tiledef_c_off[a] - TILEDEF_C_BASE));")
      p.append("                *(u16 *)(contact_tiledef_tbl + a * 4 + 2) = tseg;")
      p.append("            }")
      p.append("        }")
      p.append("    }")
    p.append("}")
    p.append("")
    with open(OUT, "w") as f:
        f.write("\n".join(p))
    print("wrote %s" % OUT)
    for name in blobs:
        print("  %-18s %d bytes" % (name, len(blobs[name])))


if __name__ == "__main__":
    main()
