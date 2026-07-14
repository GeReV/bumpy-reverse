#ifndef BUMPY_H
#include <conio.h>   /* inp/outp — game-port 0x201 + PIT in read_joystick_axes */
#endif
#include "input.h"
#ifdef BUMPY_PLAYABLE
#include "host/host.h"   /* host_keyboard_isr_install */
#endif

/*
 * input.c — keyboard input layer.  Faithful structural port (Phase-1 Task 5).
 *
 * Ground truth: Ghidra decomp in local/build/slice_decomp.txt; flow narrative in
 * local/build/slice_model.md §3; symbols/addresses in local/build/slice_syms.txt.
 * One C function per original function, same control flow, same data layouts.
 *
 * The engine path this module documents (per tick, driven by the game loop):
 *     input_state_clear();              // 1000:65d2  input_state = 0
 *     ... poll_input();                 // 1000:1dde  read action -> input_state
 * where poll_input -> read_input_action (1000:75a2) interprets a
 * per-joystick action-bytecode script that ORs key-down output values together.
 *
 * ── RECONSTRUCTION FIDELITY (module-wide deviations) ────────────────────────────
 *  - g_key_state_table indirection: the engine references the 0x80-byte key table
 *    at 203b:4d42 INDIRECTLY through a near pointer (the INT 9 ISR writes it, and
 *    get_key_state / read_input_action read through `g_key_state_table + idx`).
 *    This port models it as a DIRECT `u8 g_key_state_table[0x80]` array.  All
 *    accesses are `g_key_state_table[scancode & 0x7f]`, matching the engine's
 *    effective semantics; only the near-ptr level of indirection is dropped.
 *  - stack_check_limit guard: several originals begin with
 *    `if (stack_check_limit <= &stack0xfffe) borland_stack_overflow();` — Borland C
 *    stack-overflow CRT noise with no game-state effect.  Omitted (documented
 *    here, per the brief), as in the rest of src/.
 *  - poll_joystick_state is a STUB for this keyboard-only slice (see below).
 *  - g_joystick_handler_table[] contents: init_joystick_handlers (1000:7532)
 *    ZEROES this table; the actual bytecode scripts are populated by a runtime
 *    path that is UNCERTAIN (Task-1 open item).  read_input_action aborts on a
 *    null entry, so the host differential test (tools/game_ctest.c) seeds a
 *    SYNTHESIZED script encoding the resolved mapping in this interpreter's exact
 *    opcode format.  The INTERPRETER is faithful; the script DATA is reconstructed
 *    from the resolved T2 scancode->input_state spec.
 */

/* dos_abort (1000:762a): read_input_action jumps here on an out-of-range handler
   index or a null script entry.  RECONSTRUCTION FIDELITY (verified 2026-07-14, raw
   disasm): DOS print-string (INT 0x21 AH=9) + an infinite `JMP $` — a genuine
   noreturn halt, not an int-returning function (see game_stubs.c).  Not reachable
   on a valid script; provided elsewhere in the link (game_stubs.c) / by the host
   ctest. */
extern void dos_abort(void);

/* ── DGROUP globals ─────────────────────────────────────────────────────────── */

u8           input_state;                    /* 203b:8244 */
u8           g_key_state_table[0x80];         /* 203b:4d42 (direct; see note) */
u8 __far    *g_joystick_handler_table[16];   /* 203b:4cf2 (zeroed by init) */
u8           g_keyboard_isr_installed;        /* 203b:4dc4 */

/* ── joystick-read DGROUP state (read_joystick_axes / poll_joystick_state) ─────── */
u8  g_p1_joystick_present;   /* 203b:4dca  0=untested,1=present,0xff=absent */
u8  g_p2_joystick_present;   /* 203b:4dcb */
s16 g_joy_pit_counter;       /* 203b:4d32  PIT ch0 decay tick count */
s16 g_joy_x_raw;             /* 203b:4dc6  raw X decay count (signed) */
s16 g_joy_y_raw;             /* 203b:4dc8  raw Y decay count (signed) */
u8  g_p1_joystick_state;     /* 203b:4d34  resolved P1 state byte */
u8  g_p2_joystick_state;     /* 203b:4d35  resolved P2 state byte */
u8  g_p1_joy_x_min_thresh;   /* 203b:4d36  calib thresholds (calibrate_joystick 1000:77d9; 0 in this slice) */
u8  g_p1_joy_y_min_thresh;   /* 203b:4d37 */
u8  g_p1_joy_x_max_thresh;   /* 203b:4d38 */
u8  g_p1_joy_y_max_thresh;   /* 203b:4d39 */
u8  g_p2_joy_x_min_thresh;   /* 203b:4d3a */
u8  g_p2_joy_y_min_thresh;   /* 203b:4d3b */
u8  g_p2_joy_x_max_thresh;   /* 203b:4d3c */
u8  g_p2_joy_y_max_thresh;   /* 203b:4d3d */

/* INT 9 keyboard ISR vector saved by install_keyboard_isr (1000:7a6d/7a6f). */
static u16   g_saved_kbd_isr_off;
static u16   g_saved_kbd_isr_seg;


/*
 * flush_keyboard_buffer — 1000:7b01
 * Flush the BIOS keyboard buffer: head (0040:001a) = tail (0040:001c) = buffer
 * start (0040:0080).  Returns AX unchanged in the engine (preserved here for the
 * faithful prototype; value is don't-care).
 */
u16 flush_keyboard_buffer(void)
{
#ifndef BUMPY_CTEST
    /* BIOS data area at segment 0x0040. */
    u16 __far *kbd_head  = (u16 __far *)MK_FP(BIOS_DATA_SEG, BIOS_KBD_HEAD_OFF);
    u16 __far *kbd_tail  = (u16 __far *)MK_FP(BIOS_DATA_SEG, BIOS_KBD_TAIL_OFF);
    u16 __far *buf_start = (u16 __far *)MK_FP(BIOS_DATA_SEG, BIOS_KBD_BUF_OFF);

    *kbd_head = *buf_start;
    *kbd_tail = *buf_start;
#endif
    return 0;
}


/*
 * install_keyboard_isr — 1000:798a
 * Zero the 0x80-byte key-state table, flush the BIOS keyboard buffer, then (once,
 * guarded by g_keyboard_isr_installed) save the old INT 9 vector and install the
 * engine's keyboard ISR.
 *
 * RECONSTRUCTION FIDELITY: the engine zeroes the table as 0x40 WORDS via a near
 * pointer and installs the real ISR through DOS INT 21h AH=0x35/0x25.  This port
 * zeroes the 0x80 BYTES directly and records the saved vector; the actual ISR
 * body is not part of this slice (the host test writes the key table directly,
 * exactly as the real ISR would).  Under the host ctest the DOS path is elided.
 */
void install_keyboard_isr(void)
{
    u16 i;

    for (i = 0; i < 0x80; i++) {
        g_key_state_table[i] = 0;
    }
    flush_keyboard_buffer();

    if (g_keyboard_isr_installed == 0) {
        g_keyboard_isr_installed = 1;
#ifndef BUMPY_CTEST
        {
            /* Save current INT 9 vector (INT 21h AH=0x35), install new (AH=0x25). */
            union REGS regs;
            struct SREGS sregs;

            regs.h.ah = 0x35;
            regs.h.al = 0x09;
            int86x(0x21, &regs, &regs, &sregs);
            g_saved_kbd_isr_off = regs.x.bx;
            g_saved_kbd_isr_seg = sregs.es;
            /* The real handler install (AH=0x25 with DS:DX -> ISR) is engine ISR
               code outside this slice; see fidelity note above. */
        }
#else
        (void)g_saved_kbd_isr_off;
        (void)g_saved_kbd_isr_seg;
#endif
#ifdef BUMPY_PLAYABLE
        /* Host platform layer: install the real INT9 keyboard ISR (host_input.c).
           Called after the table is zeroed + buffer flushed, exactly as the engine
           would install its own ISR at this point in install_keyboard_isr.
           BUMPY_PLAYABLE is always a real DOS binary (never BUMPY_CTEST), so this
           runs after the INT 21h/0x35 save above.  The host_input.c ISR uses its
           own s_old_int9_handler; the g_saved_kbd_isr_off/seg here are retained
           for structural fidelity to the engine's save/restore scheme. */
        host_keyboard_isr_install();
#endif
    }
}


/*
 * get_key_state — 1000:7ab4
 * Returns the key-down state for `scancode` from the key-state table.
 */
u8 get_key_state(u8 scancode)
{
    return g_key_state_table[scancode & 0x7f];
}


/*
 * input_state_clear — 1000:65d2
 * Clear all input_state bits.  (The per-tick CLEAR TIMING is owned by the game
 * loop / Task 7; this just performs the assignment.)
 */
void input_state_clear(void)
{
    input_state = 0;
}


/* ── joystick read + state — RECONSTRUCTED from local/decomp (audit 2026-06-28).
 * read_joystick_axes (1000:7861) + poll_joystick_state (1000:773c) replace the
 * keyboard-only poll stub.  DEAD in this slice: the synthesized handler script
 * has no phase-1 opcode so read_input_action never calls poll_joystick_state; and
 * with no joystick read_joystick_axes returns 0xffff (single cheap 0x201 probe).
 * CARVE-OUT: calibrate_joystick (1000:77d9) — which fills the 0x4d36..0x4d3d
 * thresholds — is not reconstructed, so the thresholds stay 0 (BSS); only relevant
 * if a real joystick + phase-1 script are present, which this slice never has. ── */

/*
 * read_joystick_axes — 1000:7861
 *
 * Low-level game-port (0x201) analog joystick read: times each axis one-shot's
 * capacitor decay against PIT channel-0 (port 0x40) for the selected player and
 * returns the packed X/Y position (high byte = X raw>>1, low byte = Y raw>>1),
 * or 0xffff on timeout / joystick-not-present.
 *
 * Player selected via the AL register selector (the engine's __cdecl16near reads
 * in_AL; modelled here as the forwarded AX from the joystick callers, only bit0 is
 * consumed): bit0=0 -> P1 (axis mask 0x102 = X=bit0, Y=bit1), bit0=1 -> P2 (0x408).
 *
 * Presence handshake: a present-flag of 0 means "untested" -> read port 0x201 once;
 * if the player's two axis bits are already low (a stick is wired) set the flag to 1
 * and return 0xffff (no reading this call).  A flag of 0xff (set by calibrate_joystick
 * on a failed read) means "no joystick" -> immediate 0xffff.  Only flag==1 proceeds.
 *
 * RECONSTRUCTION FIDELITY:
 *  - Interrupt mask: the engine brackets the whole body in PUSHF;CLI (1000:7861-7863)
 *    ... POPF (1000:7951), masking interrupts across the PIT capacitor-decay timing
 *    loop so a tick IRQ cannot corrupt the count.  The Ghidra C decomp (source of
 *    truth) does not surface this; it is documented here and omitted from the active
 *    code (as the project omits asm-level scaffolding such as the stack-check
 *    prologue).  A real-hardware build wanting tick-accurate readings must restore a
 *    disable()/enable() (flag-preserving) bracket; this matters only when the
 *    joystick path actually runs (poll_joystick_state is still a stub in this slice).
 *  - Dead recursive call: the newly-present path ends `return 0xffff`.  The binary
 *    reaches that return through a tautological CMP AX,0xffff;JZ, leaving a recursive
 *    self-CALL at 1000:7983 as unreachable dead code (Ghidra: "Removing unreachable
 *    block 0x00017983").  The decomp and this port both omit it.
 *  - Port I/O is the engine's IN/OUT, modelled with conio inp()/outp() exactly as
 *    sound.c does (input.c gains a `#ifndef BUMPY_H #include <conio.h> #endif`).
 *  - Variable roles mirror the decomp 1:1 (port_val and y_mask are deliberately
 *    reused for both a port value and a mask byte, as in the original);
 *    Ghidra's bVar1 is named high_mask here.  CONCAT11(hi,lo) packing is explicit.
 */
u16 read_joystick_axes(u16 player_sel)   /* in_AL: bit0 selects P1(0)/P2(1) */
{
    u8  port_val;
    u8  y_mask;
    u8  high_mask;          /* Ghidra bVar1 */
    u16 axis_mask;
    u16 y_val;
    s16 prev_timeout;
    s16 timeout;
    s16 retry_count;

    if ((player_sel & 1) == 0) {
        axis_mask = 0x102;
        if (g_p1_joystick_present != 1) {
            if (g_p1_joystick_present != 0) {
                return 0xffff;                    /* 0xff -> no joystick */
            }
            port_val = (u8)inp(GAME_PORT);            /* untested: probe once */
            if ((port_val & 3) != 0) {                /* P1 = game-port bits 0-1 (X,Y) */
                return 0xffff;
            }
            g_p1_joystick_present = 1;
            return 0xffff;
        }
    }
    else {
        axis_mask = 0x408;
        if (g_p2_joystick_present != 1) {
            if (g_p2_joystick_present != 0) {
                return 0xffff;
            }
            port_val = (u8)inp(GAME_PORT);
            if ((port_val & 0xc) != 0) {               /* P2 = game-port bits 2-3 (X,Y) */
                return 0xffff;
            }
            g_p2_joystick_present = 1;
            return 0xffff;
        }
    }

    /* Wait for the selected player's X then Y axis lines to settle low (their RC
       one-shots already discharged), guarded by a down-counting watchdog. */
    timeout = 0;
    do {
        do {
            prev_timeout = timeout;
            port_val = (u8)inp(GAME_PORT);
            timeout = prev_timeout + -1;
        } while (timeout != 0 && ((u8)axis_mask & port_val) != 0);
        y_mask = (u8)(axis_mask >> 8);
        retry_count = 0;
    } while ((timeout != 0) &&
            (retry_count = prev_timeout + -2, timeout = retry_count,
            retry_count != 0 && (y_mask & port_val) != 0));

    if (retry_count != 0) {
        outp(GAME_PORT, port_val);                /* strobe: re-trigger one-shots */
        port_val = (u8)inp(GAME_PORT);
        if (((y_mask & port_val) == 0) && (((u8)axis_mask & port_val) == 0)) {
            /* Program PIT ch0 (latch mode 0x06) and align to a fresh tick edge. */
            outp(PIT_CMD_PORT, 6);
            timeout = 100;
            do {
                timeout = timeout + -1;
            } while (timeout != 0);
            do {
                port_val = (u8)inp(PIT_COUNTER0_PORT);
                (void)inp(PIT_COUNTER0_PORT);
            } while ((port_val & 0x10) == 0);
            do {
                port_val = (u8)inp(PIT_COUNTER0_PORT);
                (void)inp(PIT_COUNTER0_PORT);
            } while ((port_val & 0x10) != 0);
        }
        g_joy_pit_counter = 0;
        timeout = 1000;
        do {
            do {
                port_val = (u8)inp(PIT_COUNTER0_PORT);
                (void)inp(PIT_COUNTER0_PORT);
            } while ((port_val & 0x10) == 0);
            g_joy_pit_counter = g_joy_pit_counter + 1;
            y_mask    = (u8)inp(GAME_PORT);
            port_val  = (u8)axis_mask;             /* low mask byte (BL=0x02 → port bit1 = Y line) */
            high_mask = (u8)(axis_mask >> 8);      /* high mask byte (BH=0x01 → port bit0 = X line) */
            if (port_val == 0) {
LAB_1000_78ff:                     /* port_val==0: the Y-line mask bit already went low
                                       on an earlier pass through this loop (or on entry
                                       via the goto below) — check whether the X-line
                                       (high_mask) has now gone low too. */
                if ((high_mask & y_mask) == 0) {
                    axis_mask = axis_mask & 0xff;
                    g_joy_x_raw = g_joy_pit_counter;
joined_r0x0001792a:                /* both this loop's line and the other line the code
                                       already latched have now gone low — port_val==0
                                       here means EVERY axis line is accounted for, so
                                       finish and return the packed X/Y timing counts. */
                    if (port_val == 0) {
                        axis_mask = (u16)g_joy_x_raw;
                        if (g_joy_x_raw < 0) {
                            axis_mask = 0;
                        }
                        y_val = (u16)g_joy_y_raw;
                        if (g_joy_y_raw < 0) {
                            y_val = 0;
                        }
                        /* CONCAT11(X raw>>1, Y raw>>1). */
                        return (u16)(((u16)(u8)(axis_mask >> 1) << 8) |
                                     (u8)(y_val >> 1));
                    }
                }
            }
            else {
                if ((port_val & y_mask) == 0) {
                    axis_mask = (u16)high_mask << 8;
                    g_joy_y_raw = g_joy_pit_counter;
                    port_val = high_mask;
                    goto joined_r0x0001792a;
                }
                if (high_mask != 0) {
                    goto LAB_1000_78ff;
                }
            }
            do {
                port_val = (u8)inp(PIT_COUNTER0_PORT);
                (void)inp(PIT_COUNTER0_PORT);
            } while ((port_val & 0x10) != 0);
            timeout = timeout + -1;
        } while (timeout != 0);
    }
    return 0xffff;
}

/*
 * poll_joystick_state — 1000:773c
 *
 * Read the joystick for the selected player (in_AX bit0: 0 = player 1, 1 = player 2):
 * fetch the packed axis position from read_joystick_axes() (HIGH byte = X raw>>1,
 * LOW byte = Y raw>>1 — see that function's header; 0xffff/-1 == no joystick / read
 * timed out), gate the axes against the per-player calibration thresholds to build
 * the direction bits, OR in the two fire-button bits read from game port 0x201, and
 * store the resulting state byte to g_p1_joystick_state / g_p2_joystick_state.
 *
 * State byte: 1=up, 2=down, 4=left, 8=right, 0x10=button1, 0x20=button2 (the
 * dispatch semantics player.c:3818 documents, validated by the int8 gate — the low
 * byte (Y axis) drives bits 1/2 and the high byte (X axis) drives bits 4/8).
 * NOTE (Ghidra-inherited misnomers, kept for symbol parity): the g_p*_joy_x_*
 * thresholds (4d36/4d38, 4d3a/4d3c) actually gate the LOW byte (hardware Y), and
 * the g_p*_joy_y_* thresholds gate the HIGH byte (hardware X); calibrate_joystick
 * fills them from the matching bytes, so the gating is self-consistent.
 *
 * RECONSTRUCTION FIDELITY (register I/O; consistent with the existing module note):
 *  - in_AX (player select, bit0) is a stale-register input in the engine — the caller
 *    read_input_action passes it implicitly; for the keyboard path it is effectively 0
 *    (= player 1).  Modelled by the file-static g_joy_poll_ax (default 0); a two-player
 *    joystick path would set it (and the matching read_joystick_axes AL) before calling.
 *  - the engine leaves the resulting state byte in CH for read_input_action's phase-1
 *    loop to OR into its accumulator.  That CH output is modelled by g_joystick_phase_out
 *    (the same static read_input_action already consumes); it mirrors the value stored to
 *    g_p{1,2}_joystick_state, and stays 0 when read_joystick_axes returns -1 (dir_bits=0).
 *  - port read uses inp(GAME_PORT) (see the conio.h guard at the top of the file).
 *
 * Stack-check prologue omitted (non-semantic Borland CRT guard, per the module note).
 * The threshold globals (203b:4d36..4d3d) are calibrated by calibrate_joystick
 * (1000:77d9); in this keyboard-only slice they stay 0 and this whole body is never
 * reached (read_input_action's synthesized script skips phase 1) — reconstructed
 * faithfully for structural fidelity.
 */
static u8 g_joystick_phase_out;   /* models the engine's CH output register (read by read_input_action) */
static u8 g_joy_poll_ax;          /* models the in_AX register (bit0 = player select); 0 = player 1 */

void poll_joystick_state(void)
{
    u8  x_or_buttons;
    s16 axes;
    u8  y_axis;
    u8  dir_bits;

    axes = read_joystick_axes(g_joy_poll_ax);
    dir_bits = 0;
    g_joystick_phase_out = 0;                 /* CH model: dir_bits == 0 when no read */
    if (axes != -1) {
        x_or_buttons = (u8)axes;              /* low byte  = Y axis (Ghidra-named var kept) */
        y_axis = (u8)((u16)axes >> 8);        /* high byte = X axis (Ghidra-named var kept) */
        if ((g_joy_poll_ax & 1) == 0) {
            /* ── Player 1 ── */
            if (y_axis < g_p1_joy_y_min_thresh) {
                dir_bits = 4;                 /* left  (X axis low)  */
            }
            else if (g_p1_joy_y_max_thresh <= y_axis) {
                dir_bits = 8;                 /* right (X axis high) */
            }
            if (x_or_buttons < g_p1_joy_x_min_thresh) {   /* 203b:4d36 (Ghidra: g_joy_calib_thresholds) */
                dir_bits = dir_bits | 1;      /* up   (Y axis low)   */
            }
            else if (g_p1_joy_x_max_thresh <= x_or_buttons) {
                dir_bits = dir_bits | 2;      /* down (Y axis high)  */
            }
            x_or_buttons = (u8)inp(GAME_PORT);    /* game-port: bit4=P1 btn1, bit5=P1 btn2 */
            if ((~x_or_buttons & 0x10) != 0) {
                dir_bits = dir_bits | 0x10;
            }
            g_p1_joystick_state = dir_bits;
            if ((~x_or_buttons & 0x20) != 0) {
                g_p1_joystick_state = dir_bits | 0x20;
            }
            g_joystick_phase_out = g_p1_joystick_state;   /* CH model */
        }
        else {
            /* ── Player 2 ── */
            if (y_axis < g_p2_joy_y_min_thresh) {
                dir_bits = 4;                 /* left  (X axis low)  */
            }
            else if (g_p2_joy_y_max_thresh <= y_axis) {
                dir_bits = 8;                 /* right (X axis high) */
            }
            if (x_or_buttons < g_p2_joy_x_min_thresh) {
                dir_bits = dir_bits | 1;      /* up   (Y axis low)   */
            }
            else if (g_p2_joy_x_max_thresh <= x_or_buttons) {
                dir_bits = dir_bits | 2;      /* down (Y axis high)  */
            }
            x_or_buttons = (u8)inp(GAME_PORT);    /* game-port: bit6=P2 btn1, bit7=P2 btn2 */
            if ((~x_or_buttons & 0x40) != 0) {
                dir_bits = dir_bits | 0x10;
            }
            g_p2_joystick_state = dir_bits;
            if ((~x_or_buttons & 0x80) != 0) {
                g_p2_joystick_state = dir_bits | 0x20;
            }
            g_joystick_phase_out = g_p2_joystick_state;   /* CH model */
        }
    }
}


/*
 * read_input_action — 1000:75a2
 *
 * Interprets the per-joystick action-bytecode script g_joystick_handler_table[idx]
 * (a far pointer).  Two phases:
 *   (1) joystick-accumulate: while the next opcode is <= 0xfc, call
 *       poll_joystick_state and OR its CH output into the accumulator low byte;
 *   (2) keyboard-group scan: each group is an output value followed by scancodes;
 *       opcodes >= 0xfd are delimiters (0xff ends, 0xfe ends the current group,
 *       others separate groups).  If any group scancode is down in
 *       g_key_state_table, OR the group's output value into the low accumulator.
 * Returns accum & 0xff.  idx > 0xf or a null table entry -> dos_abort.
 *
 * Reconstructed faithfully from the decomp (the CONCAT11 byte-pair packing is
 * modelled with explicit low/high bytes).  Named read_input_action per the brief.
 */
u16 read_input_action(u16 handler_idx)
{
    u16 accum;
    u8  opcode;
    u8 __far *script_ptr;

    if ((handler_idx & 0xff) > 0xf) {
        dos_abort();          /* real path: prints + hangs, never returns */
        return 0;
    }

    script_ptr = g_joystick_handler_table[handler_idx & 0xff];
    if (script_ptr == (u8 __far *)0) {
        dos_abort();          /* real path: prints + hangs, never returns */
        return 0;
    }

    accum = 0;

    /* Phase 1: joystick accumulate.  Runs while opcodes are <= 0xfc. */
    while (1) {
        opcode = *script_ptr;
        script_ptr++;
        if (opcode > 0xfc) {
            break;
        }
        poll_joystick_state();
        accum = (u16)((u8)accum | g_joystick_phase_out);   /* low byte |= CH */
    }

    /* Phase 2: keyboard-group scan. */
    do {
        if (opcode == 0xff) {
            break;
        }
        opcode = *script_ptr;          /* group output value */
        script_ptr++;
        if (opcode < 0xfd) {
            u8 out_val = opcode;       /* CONCAT11: stored in accum's high byte */
            accum = (u16)((u16)out_val << 8 | (u8)accum);
            while (1) {
                opcode = *script_ptr;  /* next scancode (or delimiter) */
                script_ptr++;
                if (opcode > 0xfc) {
                    break;
                }
                if (g_key_state_table[opcode & 0x7f] != 0) {
                    /* opcode = accum >> 8 (the out value); low |= out value. */
                    out_val = (u8)(accum >> 8);
                    accum = (u16)((u16)out_val << 8 | ((u8)accum | out_val));
                }
            }
        }
    } while (opcode != 0xfe);

    return accum & 0xff;
}


/*
 * poll_input — 1000:1dde
 * Read one input action (read_input_action); if nonzero, store it in input_state.
 *
 * RECONSTRUCTION FIDELITY: the engine passes a stale AH register as the high byte
 * of read_input_action's argument; only the low byte (handler index) is meaningful
 * and it is effectively 0 (handler 0) for the keyboard path.  The stack_check_limit
 * CRT guard is omitted (see module note).
 */
void poll_input(void)
{
    u8 input_code;

    input_code = (u8)read_input_action(0);
    if (input_code != 0) {
        input_state = input_code;
    }
}

#ifdef BUMPY_PLAYABLE
/* ── wait_keypress (1000:328f) ──────────────────────────────────────────────────
 * Engine logic (relocated from src/host/host_input.c — not host platform glue):
 * clear input_state, then spin calling poll_input() until input_state becomes
 * nonzero.  The game gates on the next resolved action key (menus, pause, title
 * sequence debounce) through this.  (Address corrected from the earlier "1de1 area"
 * citation, which pointed inside poll_input; the real wait_keypress is 1000:328f.)
 * Faithful to the engine body's structure (minus the Borland stack-check prologue,
 * a non-semantic compiler guard omitted throughout the reconstruction); the
 * tight spin is correct because IRQ1 is delivered asynchronously, so the INT9 ISR
 * sets the table entry and poll_input() eventually returns a nonzero action.
 * Built only into the playable image (game_stubs.c stubs it for the default). */
void wait_keypress(void)
{
    input_state = 0u;
    while (input_state == 0u) {
        poll_input();
    }
}

/* ── read_input_action_byte (1000:75a2) ─────────────────────────────────────────
 * u8/char-width adapter over read_input_action (same engine address) for the
 * narrower callers in screens.c / game_stubs.c.  Routes to the faithful
 * interpreter; for all existing call sites arg is effectively 0 (handler 0 = the
 * keyboard script). */
char read_input_action_byte(u8 arg)
{
    return (char)read_input_action((u16)arg);
}
#endif /* BUMPY_PLAYABLE */
