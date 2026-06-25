#ifdef BUMPY_PLAYABLE
#include <dos.h>       /* _dos_getvect, _dos_setvect */
#include <conio.h>     /* inp, outp */
#include "host.h"
#include "../input.h"  /* g_key_state_table, input_state, poll_input */

/* ============================================================================
 * host_input.c — INT9 keyboard ISR + input polls (Plan A, Task 6)
 * ============================================================================
 *
 * Implements the real INT9 (IRQ1 / keyboard) ISR for the playable host build.
 * Three public symbols replace or extend the game_stubs.c / input.c skeletons:
 *
 *   host_keyboard_isr    — real INT9 handler: fills g_key_state_table
 *   wait_keypress        — spin on poll_input() until input_state != 0
 *   fun_75a2_poll_action — action-poll primitive (calls read_input_action(0))
 *   restore_keyboard_isr — teardown: restore the old INT9 vector
 *
 * RECONSTRUCTION FIDELITY — INT9 ISR (HOST CHOSEN, DOCUMENTED DEVIATION)
 *
 * The original engine's keyboard ISR (installed by install_keyboard_isr 1000:798a
 * via INT 21h AH=0x25/AL=0x09) lives in the original binary as self-modifying
 * BGI-overlay code that does not cleanly decompile.  The faithful reconstruction
 * documents the ISR's EFFECT on g_key_state_table (the same table get_key_state /
 * read_input_action poll) rather than reproducing the BGI overlay machinery.
 *
 * The host ISR is the minimal correct implementation of that effect:
 *   1. Read the scancode from port 0x60.
 *   2. On make code (bit 7 clear): g_key_state_table[scancode & 0x7f] = 1.
 *   3. On break code (bit 7 set):  g_key_state_table[scancode & 0x7f] = 0.
 *   4. Send PIC EOI (OUT 0x20, 0x20).
 *
 * This is exactly what the original ISR did to the table — the per-game-tick
 * get_key_state / read_input_action path is UNCHANGED and fully faithful.
 *
 * e0 PREFIX HANDLING: AT keyboards send 0xE0 before extended scancodes (arrows,
 * Insert, Delete, etc.).  DOSBox and the BIOS keyboard handler strip the 0xE0
 * before delivering to INT 9, so the game sees the base codes 0x48/0x50/0x4b/0x4d
 * directly — exactly the codes the engine's decoded keyboard map expects.  If a
 * raw 0xE0 byte does arrive (some AT setups) it has bit 7 clear, so the ISR writes
 * g_key_state_table[0x60] = 1 and then the following scancode byte updates the
 * actual key.  This is harmless: slot 0x60 is unused in the engine's scancode map,
 * and the arrow slot (0x48/etc.) is still set correctly by the follow-on byte.
 * No special 0xE0 state machine is needed; the choice is documented here.
 *
 * Deviations from the original:
 *   - The original ISR body is in a BGI-overlay page; this host version is a plain
 *     C interrupt handler with the same observable effect on g_key_state_table.
 *   - The original used INT 21h AH=0x35/0x25 directly; the host uses
 *     _dos_getvect / _dos_setvect (same semantics, OW-idiomatic wrappers).
 *   - install_keyboard_isr (input.c) now installs the host ISR under
 *     #ifdef BUMPY_PLAYABLE; the default (non-playable) build is unchanged.
 * Recorded in docs/reconstruction-fidelity.md ("playable host: host_input INT9").
 *
 * RUNTIME-VERIFICATION DEFERRAL
 * The playable build cannot boot until Task 9 wires the BUMPY_PLAYABLE entry
 * point.  Verification here: (a) wmake play links BUMPYP.EXE with no -wx warnings;
 * (b) wmake BUMPY (default build) is byte-unchanged; (c) validate_input.sh and
 * validate_integration.sh both pass.  Runtime key-table fill + wait_keypress
 * return proof deferred to Tasks 9 and 11.
 * ============================================================================ */

/* ── Hardware constants ────────────────────────────────────────────────────── */
#define KBD_DATA_PORT   0x60u   /* keyboard controller data port (scancode) */
#define PIC_CMD_PORT    0x20u   /* 8259 PIC command port */
#define PIC_EOI         0x20u   /* non-specific End-Of-Interrupt command */
#define SCANCODE_MASK   0x7fu   /* strip break bit to get make scancode */
#define BREAK_BIT       0x80u   /* bit 7 set = break (key released) */

/* ── Saved old INT9 vector ─────────────────────────────────────────────────── */
static void (__interrupt __far *s_old_int9_handler)(void) = (void (__interrupt __far *)(void))0;

/* ── host_keyboard_isr ─────────────────────────────────────────────────────────
 * INT9 ISR: read scancode, update g_key_state_table, send EOI.
 *
 * Declared __interrupt __far per Open Watcom: the compiler auto-saves/restores
 * all registers and emits IRET.
 *
 * We do NOT chain the old INT9 vector because the engine fully owns the keyboard
 * state after install_keyboard_isr.  The old BIOS INT9 handler (which writes the
 * BIOS keyboard buffer) is no longer needed once the engine's table is live.
 * The BIOS keyboard buffer is flushed by install_keyboard_isr before we install,
 * so there is no stale input to drain.
 *
 * EOI is sent unconditionally after every IRQ1 (one EOI per ISR invocation). */
static void __interrupt __far host_keyboard_isr(void)
{
    unsigned char scancode;

    /* Read the raw scancode from the keyboard data port. */
    scancode = (unsigned char)inp(KBD_DATA_PORT);

    /* Make code (key pressed): bit 7 clear — set the table entry. */
    /* Break code (key released): bit 7 set  — clear the table entry. */
    if ((scancode & BREAK_BIT) != 0u) {
        g_key_state_table[scancode & SCANCODE_MASK] = 0u;
    } else {
        g_key_state_table[scancode & SCANCODE_MASK] = 1u;
    }

    /* Send non-specific EOI to the 8259 PIC so it can accept the next IRQ1. */
    outp(PIC_CMD_PORT, PIC_EOI);
}

/* ── host_keyboard_isr_install ─────────────────────────────────────────────────
 * Save the current INT9 vector and install host_keyboard_isr.
 * Called by install_keyboard_isr (input.c) under #ifdef BUMPY_PLAYABLE after
 * the key-state table is zeroed and the BIOS buffer is flushed.
 *
 * Idempotent: if called a second time it is a no-op (s_old_int9_handler != null). */
void host_keyboard_isr_install(void)
{
    if (s_old_int9_handler == (void (__interrupt __far *)(void))0) {
        s_old_int9_handler = _dos_getvect(0x09u);
        _dos_setvect(0x09u, host_keyboard_isr);
    }
}

/* ── restore_keyboard_isr ──────────────────────────────────────────────────────
 * Restore the old INT9 vector saved by host_keyboard_isr_install.
 * Called from the host exit/cleanup path (host_boot.c / atexit) to leave DOS
 * in a clean state.  Safe to call if never installed (null check). */
void restore_keyboard_isr(void)
{
    if (s_old_int9_handler != (void (__interrupt __far *)(void))0) {
        _dos_setvect(0x09u, s_old_int9_handler);
        s_old_int9_handler = (void (__interrupt __far *)(void))0;
    }
}

/* wait_keypress (1000:1de1) and fun_75a2_poll_action (1000:75a2) are engine input
 * logic, not host platform glue — relocated to src/input.c (next to poll_input /
 * read_input_action, which they call). */

#endif /* BUMPY_PLAYABLE */
