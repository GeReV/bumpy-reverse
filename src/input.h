#ifndef INPUT_H_INCLUDED
#define INPUT_H_INCLUDED

/* BIOS Data Area (segment 0x0040) keyboard-buffer fields flush_keyboard_buffer
 * resets, and the joystick/PIT ports read_joystick_axes/poll_joystick_state
 * poll.  Bare (unsuffixed, signed-int) literals — matches the original
 * exactly. */
#define BIOS_DATA_SEG     0x0040
#define BIOS_KBD_HEAD_OFF 0x001a
#define BIOS_KBD_TAIL_OFF 0x001c
#define BIOS_KBD_BUF_OFF  0x0080
#define GAME_PORT          0x201
#define PIT_COUNTER0_PORT  0x40
#define PIT_CMD_PORT       0x43

/*
 * input.h — keyboard input layer declarations.
 *
 * Phase-1 Task 5: a strictly structure-faithful port of the engine's pure
 * keyboard-input path (the "read a key action into input_state" plumbing),
 * mirroring the Ghidra decomp 1:1.  Functions ported here (engine addresses):
 *
 *   install_keyboard_isr   1000:798a   zero key table + flush BIOS buf + install INT9
 *   get_key_state          1000:7ab4   g_key_state_table[scancode & 0x7f]
 *   flush_keyboard_buffer   1000:7b01   clear BIOS kbd buffer
 *   input_state_clear      1000:65d2   input_state = 0
 *   poll_input             1000:1dde   c = read_input_action(); if (c) input_state = c
 *   read_input_action      1000:75a2   joystick/keyboard action-bytecode interpreter
 *   poll_joystick_state    (stub)      joystick phase — no joystick in this slice
 *
 * DEFERRED to Task 6 (player.c): handle_gameplay_input (1000:1d26).  Its body is
 * mostly PLAYER-SPINE dispatch (p1_read_tile_under, p1_movement_dispatch,
 * dispatch_move_step, begin_physics_freeze, run_physics_settle), so it lands with
 * those callees in Task 6, keeping input.c coherent and fully validatable now.
 *
 * See input.c for full RECONSTRUCTION FIDELITY notes.
 */

#include "bumpy.h"

/* ── DGROUP globals (segment 203b in the original) ──────────────────────────── */

/* Current resolved input action byte (DGROUP 203b:8244). */
extern u8 input_state;

/* Keyboard key-state table (0x80 bytes): nonzero == key currently down.
   In the engine this lives at 203b:4d42 and is referenced indirectly through a
   near pointer (the ISR writes it).  RECONSTRUCTION FIDELITY: the port uses a
   direct array rather than the near-ptr indirection (see input.c). */
extern u8 g_key_state_table[0x80];

/* 16-entry far-pointer joystick handler-script table (DGROUP 203b:4cf2). */
extern u8 __far *g_joystick_handler_table[16];

/* Tracks whether the INT 9 keyboard ISR has been installed (DGROUP 203b:4dc4). */
extern u8 g_keyboard_isr_installed;

/* ── Functions ──────────────────────────────────────────────────────────────── */

void install_keyboard_isr(void);
u8   get_key_state(u8 scancode);
u16  flush_keyboard_buffer(void);
void input_state_clear(void);
void poll_input(void);
u16  read_input_action(u16 handler_idx);
void poll_joystick_state(void);
u16 read_joystick_axes(u16 player_sel);   /* 1000:7861 — packed X/Y, 0xffff=none */

#endif /* INPUT_H_INCLUDED */
