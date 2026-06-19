#include "input.h"

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
 * where poll_input -> read_input_action (FUN_1000_75a2, 1000:75a2) interprets a
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
 *    `if (stack_check_limit <= &stack0xfffe) FUN_1000_ab83();` — Borland C
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

/* dos_abort (FUN near the CRT exit path): read_input_action calls it on an
   out-of-range handler index or a null script entry.  It is a CRT terminate, not
   reachable on a valid script, and is provided elsewhere in the link (engine CRT)
   / by the host ctest.  Declared extern here so the faithful control flow stays
   intact without pulling in the CRT. */
extern int dos_abort(void);

/* ── DGROUP globals ─────────────────────────────────────────────────────────── */

u8           input_state;                    /* 203b:8244 */
u8           g_key_state_table[0x80];         /* 203b:4d42 (direct; see note) */
u8 __far    *g_joystick_handler_table[16];   /* 203b:4cf2 (zeroed by init) */
u8           g_keyboard_isr_installed;        /* 203b:4dc4 */

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
    u16 __far *kbd_head  = (u16 __far *)MK_FP(0x0040, 0x001a);
    u16 __far *kbd_tail  = (u16 __far *)MK_FP(0x0040, 0x001c);
    u16 __far *buf_start = (u16 __far *)MK_FP(0x0040, 0x0080);

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


/*
 * poll_joystick_state — joystick-phase accumulator (STUB for this slice).
 *
 * RECONSTRUCTION FIDELITY: the engine's joystick read produces an action byte
 * (returned in CH and OR'd into the accumulator's low byte by read_input_action's
 * phase-1 loop).  This slice is keyboard-only and has no joystick hardware
 * capture, so this faithful-signature stub contributes 0.  The synthesized
 * handler script (host test) starts its bytecode with a phase-1 delimiter, so the
 * joystick loop body never runs and the stub's value is never consulted.
 */
static u8 g_joystick_phase_out;   /* mirrors the engine's CH output register */

void poll_joystick_state(void)
{
    g_joystick_phase_out = 0;
}


/*
 * read_input_action — FUN_1000_75a2 @ 1000:75a2
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
        return (u16)dos_abort();
    }

    script_ptr = g_joystick_handler_table[handler_idx & 0xff];
    if (script_ptr == (u8 __far *)0) {
        return (u16)dos_abort();
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
