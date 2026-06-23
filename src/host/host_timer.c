#ifdef BUMPY_PLAYABLE
#include <dos.h>       /* _dos_getvect, _dos_setvect, _chain_intr */
#include <conio.h>     /* outp */
#include "host.h"
#include "../game.h"     /* run_n_frames prototype (u8 n) */
#include "../screens.h"  /* timing_flag_accumulator (DGROUP 0x854f) */

/* ============================================================================
 * host_timer.c — INT8/PIT frame pacing (Plan A, Task 5)
 * ============================================================================
 *
 * Implements real INT8 (IRQ0 / PIT channel 0) frame pacing for the playable
 * host build.  Three public symbols replace the game_stubs.c carve-outs:
 *
 *   install_interrupt_handler   — install ISR + reprogram PIT to game rate
 *   rotate_timing_flags_and_wait— per-tick frame wait (1 or 2 host_tick advance)
 *   run_n_frames(n)             — wait n ticks (level-intro path)
 *
 * RECONSTRUCTION FIDELITY — INT8/PIT PACING (HOST CHOSEN, DOCUMENTED DEVIATION)
 *
 * The original engine installs its ISR (pit_timer_isr_multiplexer 1000:7c02)
 * directly from init_game_session_state via DOS INT 21h/AH=0x25/AL=0x08, then
 * programs PIT channel 0 to divisor 0x0951 (2385 decimal) via pit_set_counter0
 * (port 0x43 cmd 0x36, port 0x40 lo/hi).  The original ISR runs the 6-channel
 * sound timer-callback sweep, sends EOI (OUT 0x20,0x20), then chains the old
 * int-8 vector via a manufactured far-return frame (so the BIOS 18.2 Hz clock
 * still ticks).
 *
 * The host replaces that ISR with a minimal one that:
 *   (a) increments the volatile host_tick counter (the frame-pacing primitive);
 *   (b) accumulates a sub-tick counter and chains the old BIOS handler every
 *       CHAIN_EVERY ticks (~27 ISR fires ≈ 18.2 Hz) using _chain_intr so the
 *       DOS clock-of-day / Ctrl-Break / timer chain survives;
 *   (c) sends a PIC EOI (OUT 0x20,0x20) on non-chain ticks.
 *
 * Deviations from the original:
 *   - The original ISR ran the full 6-channel sound callback sweep; the host ISR
 *     does NOT (sound is a Tier-2 deliverable; the sound carve-outs remain
 *     no-ops).  Adding the sweep here would be premature and out of scope.
 *   - The original chained the old vector via a hand-rolled far frame; the host
 *     uses Open Watcom's _chain_intr() which produces the same observable effect
 *     (re-enters the old handler with the 8259 still unacknowledged — the old
 *     handler sends its own EOI).
 *   - Teardown restores the old INT 8 vector (DOS INT 21h/AH=0x25) and resets
 *     the PIT divisor to 0 (= 65536, the BIOS default 18.2 Hz) via the same
 *     port sequence.
 *
 * These are host-platform deviations; the game-logic state is not affected.
 * Recorded in docs/reconstruction-fidelity.md ("playable host: host_timer INT8").
 *
 * RUNTIME-VERIFICATION DEFERRAL
 * The playable build cannot boot until Task 9 wires the BUMPY_PLAYABLE entry
 * point.  Verification here is: (a) wmake play links BUMPYP.EXE with no -wx
 * warnings; (b) wmake BUMPY (default build) is byte-unchanged; (c)
 * validate_integration.sh passes.  Runtime cadence proof (tick-for-tick
 * INT8-synced replay) is deferred to Tasks 9 and 11.
 * ============================================================================ */

/* ── PIT constants ─────────────────────────────────────────────────────────────
 * PIT_CMD_PORT  = 0x43  (8253/8254 mode/command register)
 * PIT_DATA_PORT = 0x40  (channel 0 counter data)
 * PIT_CMD_CH0_MODE3 = 0x36  (channel 0 | access lo/hi | mode 3 square wave | binary)
 *
 * PIT_DIVISOR = 0x0951 (2385 decimal)
 *   Source: original engine's pit_set_counter0_wrap (1000:7db2) loads AX=0x0951
 *   then programs the PIT unconditionally.
 *   Rate = 1,193,182 / 2385 ≈ 500.3 Hz (the game's ISR fires at ~500 Hz).
 *
 * CHAIN_EVERY: how many host ISR ticks before we chain the old BIOS handler.
 *   1,193,182 / 65536 ≈ 18.2063 Hz (original BIOS rate).
 *   PIT_DIVISOR / (65536 / PIT_DIVISOR) == 2385 * 65536 / 65536 = 2385 ... wrong.
 *   Correct formula: fire old handler every (65536 / PIT_DIVISOR) of our ticks.
 *   65536 / 2385 ≈ 27.48 → 27 (rounds toward original cadence; slight overshoot
 *   means BIOS clock slightly fast, which is the safe direction vs. a drift that
 *   starves the clock).  27 × 2385 / 1,193,182 ≈ 0.05396 s → 18.53 Hz, ~2%
 *   above the BIOS 18.2 Hz — acceptable for a playable host.
 *
 * PIT_DIVISOR_BIOS = 0 (written to PIT on teardown; PIT interprets 0 as 65536,
 *   the BIOS default 18.2 Hz divisor). */
#define PIT_CMD_PORT        0x43u
#define PIT_DATA_PORT       0x40u
#define PIT_CMD_CH0_MODE3   0x36u
#define PIT_DIVISOR         0x0951u   /* original engine's divisor: ~500 Hz */
#define PIT_DIVISOR_BIOS    0x0000u   /* BIOS default (= 65536): 18.2 Hz */
#define CHAIN_EVERY         27u       /* chain old BIOS handler every N ticks */

/* ── PIC EOI constant ──────────────────────────────────────────────────────── */
#define PIC_PORT            0x20u
#define PIC_EOI             0x20u

/* ── Saved old INT8 vector ─────────────────────────────────────────────────── */
static void (__interrupt __far *s_old_int8_handler)(void) = (void (__interrupt __far *)(void))0;

/* ── Frame-tick counter (incremented by ISR, read by spin-waits) ───────────── */
volatile unsigned host_tick = 0u;

/* ── Sub-tick counter for BIOS chain cadence ───────────────────────────────── */
static unsigned s_chain_count = 0u;

/* ── ISR ───────────────────────────────────────────────────────────────────────
 * Declared __interrupt __far per Open Watcom: compiler auto-saves/restores
 * registers and emits IRET.  When we call _chain_intr the compiler-generated
 * epilogue is bypassed — _chain_intr never returns, it jumps to the old handler
 * (which sends its own EOI before IRET back to the interrupted code).
 *
 * EOI / chain model (correct for a shared IRQ0 chain):
 *   - Non-chain tick: we send EOI ourselves (OUT 0x20,0x20) so the 8259 can
 *     accept the next IRQ0 before we IRET.
 *   - Chain tick: we do NOT send EOI; instead we call _chain_intr which passes
 *     control to the old handler WITH the interrupt still pending on the 8259.
 *     The old BIOS handler sends its own EOI, then IRETs back to the
 *     interrupted code.  This is the standard DOS chaining model — sending EOI
 *     before chaining would cause a race where a second IRQ0 fires before the
 *     old handler has had a chance to update the BIOS clock-of-day tick. */
static void __interrupt __far host_int8_isr(void)
{
    host_tick++;

    s_chain_count++;
    if (s_chain_count >= CHAIN_EVERY) {
        s_chain_count = 0u;
        /* Chain to old handler: it sends EOI and IRETs to interrupted code. */
        _chain_intr(s_old_int8_handler);
        /* _chain_intr does not return. */
    }

    /* Non-chain tick: send EOI so 8259 accepts next IRQ0. */
    outp(PIC_PORT, PIC_EOI);
}

/* ── pit_program: write a 16-bit divisor to PIT channel 0 ─────────────────── */
static void pit_program(unsigned divisor)
{
    outp(PIT_CMD_PORT, PIT_CMD_CH0_MODE3);   /* cmd: ch0, lo/hi, mode 3 */
    outp(PIT_DATA_PORT, (unsigned char)( divisor        & 0xFFu));  /* lo byte */
    outp(PIT_DATA_PORT, (unsigned char)((divisor >> 8u) & 0xFFu));  /* hi byte */
}

/* ── install_interrupt_handler ─────────────────────────────────────────────────
 * Save old INT 8 vector (DOS INT 21h/AH=0x35/AL=0x08 via _dos_getvect), install
 * the host ISR (AH=0x25), then reprogram the PIT to PIT_DIVISOR (~500 Hz).
 *
 * RECONSTRUCTION FIDELITY: the original engine's install_interrupt_handler
 * (1000:7cde) also initialises the 6-channel sound-timer-callback table
 * (snd_timer_cb_table at DGROUP 0x5516) and the 16-entry tick_counter table
 * (0x54f6).  Those inits are NOT reproduced here — the sound subsystem is a
 * Tier-2 deliverable and the sound carve-outs remain no-ops; the tick counters
 * are irrelevant to the host_tick pacing model.  Deviation noted. */
void install_interrupt_handler(void)
{
    /* Save old INT 8 vector. */
    s_old_int8_handler = _dos_getvect(0x08u);

    /* Install host ISR. */
    _dos_setvect(0x08u, host_int8_isr);

    /* Reprogram PIT channel 0 to game rate (~500 Hz). */
    pit_program(PIT_DIVISOR);
}

/* ── host_timer_teardown ───────────────────────────────────────────────────────
 * Restore the old INT 8 vector and reset PIT channel 0 to the BIOS default
 * (divisor 0 = 65536 → 18.2 Hz).  Called from the host exit/cleanup path
 * (Task 9 / main-exit hook) to leave DOS in a clean state.
 *
 * NOTE: teardown is NOT called install_interrupt_handler's own name because the
 * engine symbol install_interrupt_handler has no paired "uninstall" entry point
 * in the reconstruction's public interface (the engine's timer_teardown is
 * FUN_1000_7fef, a carve-out in game_stubs.c).  The host exposes this as a
 * separate internal symbol used by host_boot.c's atexit/exit path. */
void host_timer_teardown(void)
{
    /* Restore old INT 8 vector. */
    if (s_old_int8_handler != (void (__interrupt __far *)(void))0) {
        _dos_setvect(0x08u, s_old_int8_handler);
        s_old_int8_handler = (void (__interrupt __far *)(void))0;
    }

    /* Reset PIT channel 0 to BIOS default 18.2 Hz (divisor 0 = 65536). */
    pit_program(PIT_DIVISOR_BIOS);
}

/* ── rotate_timing_flags_and_wait ──────────────────────────────────────────────
 * 1:1 port of the engine's rotate_timing_flags_and_wait (1000:1349):
 *   - Read timing_flag_accumulator bit 0 (the carry-out of the rotate).
 *   - Rotate the byte right by 1 (ROR 1) with that bit wrapping into bit 7.
 *   - Wait 2 host_ticks if the carry-out was 1, else wait 1 host_tick.
 *
 * Engine global timing_flag_accumulator is at DGROUP 0x854f.  The host honours
 * the same variable through the shared globals (declared in game.h as the
 * engine's DGROUP-mapped timing byte). */
void rotate_timing_flags_and_wait(void)
{
    unsigned char frames_to_wait;
    unsigned char carry_bit;
    unsigned char acc;

    frames_to_wait = 1u;
    acc = timing_flag_accumulator;

    if ((acc & 0x01u) != 0u) {
        carry_bit = 0x80u;    /* LSB was 1 — wraps to MSB */
        frames_to_wait = 2u;
    } else {
        carry_bit = 0x00u;
    }

    /* ROR 1: logical right-shift then OR in the carry. */
    timing_flag_accumulator = (unsigned char)((acc >> 1u) | carry_bit);

    /* Wait for frames_to_wait host_ticks. */
    run_n_frames((unsigned char)frames_to_wait);
}

/* ── run_n_frames ──────────────────────────────────────────────────────────────
 * Wait n host_tick advances (n frame-pacing ticks from the INT8 ISR).
 * Each iteration: snapshot the current tick value, spin until host_tick
 * advances (ISR fires), then count down.
 *
 * RECONSTRUCTION FIDELITY: the engine's run_n_frames (1000:05e7) dispatches
 * through the palette-mode overlay to a frame-tick wait that spins on
 * tick_counter_a (DGROUP 0x54f6, cleared by wait_tick_counter_a 1000:7efe).
 * The host uses host_tick (our own volatile counter) instead; the observable
 * effect is identical — each call waits exactly one ISR period per iteration.
 * Deviation noted. */
void run_n_frames(unsigned char n)
{
    unsigned snap;

    while (n != 0u) {
        snap = host_tick;
        /* Spin until the ISR increments host_tick past our snapshot. */
        while (host_tick == snap) {
            /* tight spin — ISR fires within 1/500 s = 2 ms */
        }
        n--;
    }
}

#endif /* BUMPY_PLAYABLE */
