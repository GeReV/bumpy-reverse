/* ────────────────────────────────────────────────────────────────────────────
 *  anim.c — animation-channel FX module (Phase-5 reconstruction).
 *
 *  SKELETON (Phase-5 Task 2): this TU defines ONLY the anim-channel module's
 *  globals — NO function bodies.  The seven anim functions (apply_cell_animation
 *  1000:69aa, step/draw/erase_anim_channels_a/b) remain stubbed in game_stubs.c
 *  this task; their bodies port in Phase-5 T3 (allocator + steppers, validated on
 *  semantic state) and T4 (draw + erase, validated at descriptor level), at which
 *  point each is un-stubbed from game_stubs.c and reconstructed 1:1 here.  Because
 *  this TU contributes no function bodies, anim.obj links cleanly alongside the
 *  game_stubs.c bodies with ZERO duplicate symbols (the same pattern Phase-4 T2's
 *  player2.obj / Phase-3 T2's items.obj used for their globals-only skeletons).
 *
 *  ── OWNERSHIP / no-duplicate-symbols (grep-verified across the src tree) ───────
 *    DEFINED HERE (genuinely new — no other TU owns a symbol of this name):
 *      anim_channels_a_tbl / anim_channels_b_tbl  (the two far-ptr slot tables)
 *      anim_a_records / anim_b_records            (the 3 A + 4 B 12-byte records)
 *      anim_a_tiledef_tbl / anim_a_frame_tbl / anim_b_frame_tbl
 *      anim_a_grid_tbl / anim_b_grid_tbl / anim_posA_tbl / anim_posB_tbl
 *      the seven view descriptors (anim_a_*_view, anim_b_*_view/view0/view1)
 *      p1_sprite (the 0x8884 blit-descriptor far ptr — there is NO `p1_sprite`
 *        variable elsewhere; entity.c references "p1_sprite" only in comments and
 *        accesses that struct via the DG_P1_OBJ 0x792e offset, not a named global)
 *      the step-state scalars / working ptrs (g_anim_stream_ptr,
 *        g_anim_cur_cmd_byte, anim_b_loop_idx, anim_b_stream_ptr,
 *        anim_b_cur_frame_byte)
 *
 *    EXTERN (owned elsewhere — declared in anim.h, defined in the owning module;
 *    grep evidence beside each):
 *      anim_target_cell    — player.c:230  `u8 anim_target_cell;`        (0x856f)
 *      g_anim_channel_idx  — player.c:1664 `u8 g_anim_channel_idx;`      (0x856c)
 *      tilemap             — game.c:79     `u8 __far *tilemap;`          (0xa0d8)
 *      current_level       — level.c:106   `u8 current_level = 1u;`      (0x79b2)
 *
 *  STACK-CHECK PROLOGUE: every original anim fn opens with Turbo C's compiler-
 *  emitted stack-overflow probe (`if (stack_check_limit <= &stack0xfffe) …`); it is
 *  NOT game logic and is intentionally OMITTED from the future ports (the same
 *  convention player.c / items.c / player2.c document).
 *
 *  Source of truth: Ghidra BumpyDecomp + raw disassembly + tools/anim_oracle.py +
 *  local/build/anim_model.md (the Phase-5 T1 anim-channel capture).
 * ──────────────────────────────────────────────────────────────────────────── */
#include "anim.h"

/* ── the channel records (one fixed 12-byte record per slot) ─────────────────── */
anim_chan_rec anim_a_records[ANIM_A_SLOTS];   /* 3 channel-A slots */
anim_chan_rec anim_b_records[ANIM_B_SLOTS];   /* 4 channel-B slots */

/* The A slot-SCAN terminator record.  In the unpacked EXE the A far-ptr table at
   0x4c70 has FOUR entries: the 3 usable slot records (0x4c40/4c4c/4c58, active=0)
   followed by a 4th record at 0x4c64 whose active byte is 0xFF.  apply_cell_animation
   scans the A table until it hits this 0xFF terminator (which restarts the scan from
   slot 0); without it the unbounded scan would run off the table.  The 3 STEPPER
   loops only ever index slots 0..2 (bounded `< 3`), so this terminator is consumed
   solely by the allocator.  (Engine-verified vs BUMPY_unpacked.exe DGROUP 0x4c64.) */
anim_chan_rec anim_a_terminator = { 0xff, 0, 0, 0, 0, 0, 0, 0 };

/* ── slot tables (far ptrs into the records above) ──────────────────────────────
   DGROUP 0x4c70/0x4c72 (A: 3 slots + 0xFF terminator) and 0x4cbc/0x4cbe (B: 4 slots
   + spawn-data tail).  In the engine these are populated at load with far ptrs to the
   record storage; here they default to the module's own records so the steppers /
   allocator and the host harness see a consistent table.  Static initialisers cannot
   portably take the address of a far array element across compilers, so they are
   wired at runtime (by the harness / the engine spawn path); the records are the
   owned storage.  The A table has ANIM_A_SLOTS+1 entries so the allocator scan finds
   the 0xFF terminator at index ANIM_A_SLOTS. */
anim_chan_rec __far *anim_channels_a_tbl[ANIM_A_SLOTS + 1];
anim_chan_rec __far *anim_channels_b_tbl[ANIM_B_SLOTS];

/* ── per-action / per-frame far-ptr tables ─────────────────────────────────────
   In the engine these are DGROUP-resident tables of 4-byte FAR POINTERS indexed by
   action/cmd/frame: the off-half lives at base+N*4+0, the seg-half at base+N*4+2
   (the decomp reads `*(u16*)(N*4 + 0x2ede)` / `*(u16*)(N*4 + 0x2ee0)`, 0x2ede and
   0x2ee0 being 2 bytes apart = the off/seg halves of one 4-byte far-ptr slot).
   Mirrored here as raw byte blobs (the same representation player.c uses for
   mode_script_tbl @0x2252) so an entry's far ptr is rebuilt at the use site with
   MK_FP(seg, off); they are level/engine data populated at load time (zero until
   the engine spawn/load path or the host harness seeds them).  Sized to 256
   entries (256*4 = 1024 B) — action/cmd/frame bytes index the full 0..0xff range. */
#define ANIM_FARPTR_TBL_LEN  (256 * 4)
u8 anim_a_tiledef_tbl[ANIM_FARPTR_TBL_LEN];   /* DGROUP 0x2ede/0x2ee0 (action*4) */
u8 anim_a_frame_tbl[ANIM_FARPTR_TBL_LEN];     /* DGROUP 0x3d6a/0x3d6c (cmd*4)    */
u8 anim_b_frame_tbl[ANIM_FARPTR_TBL_LEN];     /* DGROUP 0x40a6/0x40a8 (frame*4)  */

/* ── grid-coord / pos tables (draw/erase A/B) ──────────────────────────────────*/
u8 __far *anim_a_grid_tbl;      /* DGROUP 0x32be/0x32c0 */
u8 __far *anim_b_grid_tbl;      /* DGROUP 0x343e/0x3440 */
u8 __far *anim_posA_tbl;        /* DGROUP 0xf4/0xf6     */
u8 __far *anim_posB_tbl;        /* DGROUP 0x3f4/0x3f6   */

/* ── view descriptors (the draw/erase BGI-overlay save-under / restore passes) ──*/
u8 __far *anim_a_erase_view;    /* DGROUP 0x8d4 */
u8 __far *anim_a_draw_view;     /* DGROUP 0x8e0 */
u8 __far *anim_a_clear_view;    /* DGROUP 0x8c0 */
u8 __far *anim_b_view0;         /* DGROUP 0x8c8 */
u8 __far *anim_b_view1;         /* DGROUP 0x8cc */
u8 __far *anim_b_draw_view;     /* DGROUP 0x8d0 */
u8 __far *anim_b_clear_view;    /* DGROUP 0x8bc */

/* ── blit descriptor far ptr (draw fns pass this to blit_sprite) ───────────────*/
u8 __far *p1_sprite;            /* DGROUP 0x8884 */

/* ── step-state scalars / working ptrs ─────────────────────────────────────────*/
u8 __far *g_anim_stream_ptr;    /* DGROUP 0xa0be/0xa0c0 — working stream ptr (A)   */
u8  g_anim_cur_cmd_byte;        /* DGROUP 0x8578 — cur cmd byte (step A)           */
u8  anim_b_loop_idx;            /* DGROUP 0x8566 — loop idx (step B)               */
u8 __far *anim_b_stream_ptr;    /* DGROUP 0xa0c2/0xa0c4 — working stream ptr (B)   */
u8  anim_b_cur_frame_byte;      /* DGROUP 0x8579 — cur frame byte (step B)         */

/* ════════════════════════════════════════════════════════════════════════════
 *  apply_cell_animation — 1000:69aa  (channel-A slot allocator)
 *  Ported 1:1 from the live Ghidra decomp + disassembly (verified fresh, 2026-06).
 *
 *  Claims an animation slot in channel A keyed by anim_target_cell (0x856f) for an
 *  action code, and stamps the action's tile into the base tilemap layer.  The slot
 *  SCAN is intentionally NOT cleaned up — the engine's control flow is two nested
 *  scans joined by a 0xFF "restart from slot 0" terminator and a cell-match path:
 *
 *    Scan 1 (do/while): advance slot_idx over the A table skipping ACTIVE slots
 *      (active=='\0' continues); on the FIRST non-zero slot, if it is the 0xFF
 *      terminator -> reset slot_idx=0 and fall into Scan 2 (LAB_6a06); otherwise
 *      if its [1]==anim_target_cell -> claim it (LAB_6a4d); else loop Scan 1 again.
 *    Scan 2 (LAB_6a06): advance from slot 0 looking for a FREE ('\0') slot, stopping
 *      on '\0' (claim) or 0xFF (give up -> return).  A non-'\0', non-0xFF byte keeps
 *      scanning.  On reaching '\0' -> claim (LAB_6a4d); if the loop exits non-'\0'
 *      (i.e. 0xFF) -> return without claiming.
 *
 *  On claim (LAB_6a4d): write [1]=anim_target_cell, stamp tilemap[anim_target_cell]
 *  = tile_def[0], copy the stream far ptr tile_def[2..5] -> slot[2..5], set [0]=1.
 *
 *  NOTE (entry capture): the decomp captures bVar5 = anim_target_cell at entry and
 *  uses it for the tilemap index; the disasm re-reads [0x856f] for both slot[1] and
 *  the tilemap index (anim_target_cell is not modified in the body, so the two are
 *  the same value).  Mirrored faithfully via the captured `target_cell`.
 *
 *  RECONSTRUCTION FIDELITY: the per-action tile-def table (0x2ede/0x2ee0) is a
 *  DGROUP table of 4-byte far ptrs; rebuilt here with MK_FP(seg, off) at the use
 *  site (the engine LES'es it).  No FX/sound/draw callee is invoked by this fn.
 * ════════════════════════════════════════════════════════════════════════════ */
void apply_cell_animation(u8 action_code)
{
    u8                target_cell;
    const u8 __far   *tile_def;       /* tile_def_ptr — far ptr at tiledef_tbl[a*4] */
    u16               tdef_off, tdef_seg;
    u8                slot_idx;
    u8                cmd;               /* the slot's active byte (decomp: char cVar1) */
    anim_chan_rec __far *slot;           /* slot_entry_ptr / slot_ptr                  */

    target_cell = anim_target_cell;
    /* tile_def far ptr = tiledef_tbl[action_code*4] (off @ +0, seg @ +2). */
    tdef_off = *(u16 *)(anim_a_tiledef_tbl + (u16)action_code * 4 + 0);
    tdef_seg = *(u16 *)(anim_a_tiledef_tbl + (u16)action_code * 4 + 2);
    tile_def = (const u8 __far *)MK_FP(tdef_seg, tdef_off);

    /* ── Scan 1: skip active slots; act on the first non-active slot. ──────────────
       The decomp reads the active byte through a `char *`; 0xFF == (char)-1 is the
       end-of-table terminator.  Mirrored as raw-byte (u8) comparisons against 0 /
       0xFF (semantically identical, avoids the signed-char fold warning). */
    slot_idx = 0;
    do {
        do {
            slot = anim_channels_a_tbl[slot_idx];
            slot_idx = slot_idx + 1;
        } while (slot->active == 0);
        if (slot->active == 0xff) {           /* 0xFF terminator -> restart at 0    */
            slot_idx = 0;
            goto LAB_6a06;
        }
    } while (slot->cell != anim_target_cell);  /* match this cell? else keep scanning */
    goto LAB_6a4d;

    /* ── Scan 2 (LAB_6a06): from slot 0, find a FREE (0) slot, stop on 0xFF. ────── */
    while (cmd != 0xff) {
LAB_6a06:
        slot = anim_channels_a_tbl[slot_idx];
        cmd = slot->active;
        slot_idx = slot_idx + 1;
        if (cmd == 0) {
            break;
        }
    }
    if (cmd != 0) {                             /* exited on 0xFF -> no free slot    */
        return;
    }

LAB_6a4d:
    slot->cell = anim_target_cell;
    *(u8 __far *)(tilemap + (u16)target_cell) = tile_def[0];
    /* stream far ptr: tile_def[2..5] -> slot[2..5]. */
    slot->stream_off = *(u16 __far *)(tile_def + 2);
    slot->stream_seg = *(u16 __far *)(tile_def + 4);
    slot->active = '\x01';
    return;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  step_anim_channels_a — 1000:14e4  (advance the 3 channel-A streams)
 *  Ported 1:1 from the live Ghidra decomp + disassembly (verified fresh, 2026-06).
 *
 *  Iterates the 3 channel-A slots via g_anim_channel_idx (0x856c, the engine's loop
 *  index).  For each ACTIVE slot: rebuild its stream far ptr from [+2..+5], read the
 *  next command byte, ADVANCE the slot's stream ptr (slot[+2] += 1), and STORE the
 *  byte into slot[+6] — the read/advance/store order matches the engine (the byte is
 *  the value at the OLD ptr; the store to [+6] precedes the 0xFF test).  0xFF ends
 *  the channel (active=0); any other non-zero byte indexes the channel-A frame table
 *  (0x3d6a/0x3d6c) for a far data ptr stored into slot[+8..+11]; byte 0 stores the
 *  frame byte only (no data-ptr load).
 *
 *  RECONSTRUCTION FIDELITY: working scalars g_anim_stream_ptr (0xa0be) and
 *  g_anim_cur_cmd_byte (0x8578) are the engine's; the frame table is rebuilt with
 *  MK_FP at the use site.  No FX/sound/draw callee is invoked.
 * ════════════════════════════════════════════════════════════════════════════ */
void step_anim_channels_a(void)
{
    u8                cmd_byte;
    u8                i;
    anim_chan_rec __far *slot;
    u16               fr_off, fr_seg;
    const u8 __far   *frame_data;

    g_anim_channel_idx = 0;
    for (i = 0; i < 3; i = i + 1) {
        slot = anim_channels_a_tbl[g_anim_channel_idx];
        if (slot->active != '\0') {
            g_anim_stream_ptr = (u8 __far *)MK_FP(slot->stream_seg, slot->stream_off);
            cmd_byte = *g_anim_stream_ptr;
            g_anim_cur_cmd_byte = cmd_byte;
            slot->stream_off = slot->stream_off + 1;
            slot->frame = cmd_byte;
            if (g_anim_cur_cmd_byte == 0xff) {
                slot->active = '\0';
            } else if (g_anim_cur_cmd_byte != 0) {
                fr_off = *(u16 *)(anim_a_frame_tbl + (u16)g_anim_cur_cmd_byte * 4 + 0);
                fr_seg = *(u16 *)(anim_a_frame_tbl + (u16)g_anim_cur_cmd_byte * 4 + 2);
                frame_data = (const u8 __far *)MK_FP(fr_seg, fr_off);
                slot->data_off = *(u16 __far *)(frame_data + 0);
                slot->data_seg = *(u16 __far *)(frame_data + 2);
            }
        }
        g_anim_channel_idx = g_anim_channel_idx + 1;
    }
    return;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  step_anim_channels_b — 1000:15a1  (advance the 4 channel-B streams)
 *  Ported 1:1 from the live Ghidra decomp + disassembly (verified fresh, 2026-06).
 *
 *  The 4-channel analogue of step_anim_channels_a.  The decomp aliases the B loop
 *  index as `last_contact_action`; the disasm shows it is byte [0x8566] =
 *  anim_b_loop_idx — used faithfully here as the B loop index (0..3).  Working
 *  scalars: anim_b_stream_ptr (0xa0c2), anim_b_cur_frame_byte (0x8579).  Same
 *  read/advance/store-[+6] order as the A stepper; 0xFF ends the channel; other
 *  non-zero bytes index the channel-B frame table (0x40a6/0x40a8).
 *
 *  RECONSTRUCTION FIDELITY: frame table rebuilt with MK_FP at the use site; no
 *  FX/sound/draw callee invoked.
 * ════════════════════════════════════════════════════════════════════════════ */
void step_anim_channels_b(void)
{
    u8                frame_byte;
    u8                i;
    anim_chan_rec __far *slot;
    u16               fr_off, fr_seg;
    const u8 __far   *frame_data;

    anim_b_loop_idx = 0;
    for (i = 0; i < 4; i = i + 1) {
        slot = anim_channels_b_tbl[anim_b_loop_idx];
        if (slot->active != '\0') {
            anim_b_stream_ptr = (u8 __far *)MK_FP(slot->stream_seg, slot->stream_off);
            frame_byte = *anim_b_stream_ptr;
            anim_b_cur_frame_byte = frame_byte;
            slot->stream_off = slot->stream_off + 1;
            slot->frame = frame_byte;
            if (anim_b_cur_frame_byte == 0xff) {
                slot->active = '\0';
            } else if (anim_b_cur_frame_byte != 0) {
                fr_off = *(u16 *)(anim_b_frame_tbl + (u16)anim_b_cur_frame_byte * 4 + 0);
                fr_seg = *(u16 *)(anim_b_frame_tbl + (u16)anim_b_cur_frame_byte * 4 + 2);
                frame_data = (const u8 __far *)MK_FP(fr_seg, fr_off);
                slot->data_off = *(u16 __far *)(frame_data + 0);
                slot->data_seg = *(u16 __far *)(frame_data + 2);
            }
        }
        anim_b_loop_idx = anim_b_loop_idx + 1;
    }
    return;
}
