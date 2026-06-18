#!/usr/bin/env python3
"""replay_check.py — Per-tick comparison of two golden traces.

Loads a candidate trace and the golden trace, then compares them tick by tick:
  - VGA planes: plane-exact (4 × 0x4000 bytes per tick)
  - Named-state values: field by field (9 fields)

Reports the first-divergence tick + which field/plane diverged.
Exits with code 0 on all-match, 1 on any divergence.

Note: Task 7 (reconstructed-exe replay) imports TICK_SCRIPT from game_oracle.py;
keep that constant stable across changes to this file.

Trace format (slice_goldentrace.bin — see game_oracle.py for full spec):
  +0x00   8 B  magic "GTRACE01"
  +0x08   4 B  u32 n_ticks
  +0x0C   4 B  u32 tick_size
  per tick (tick_size bytes):
    4 × 0x4000 B  VGA planes (plane 0..3)
    12 B  named-state: p1_pixel_x(u16) p1_pixel_y(u16)
                       p1_move_anim(u8) p1_cell(u8)
                       game_mode(u8) input_state(u8)
                       move_locked(u8) current_level(u8)
                       copyprotect_flag(s8) _pad(u8)

Named-state field names (in order, matching the struct layout):
  p1_pixel_x, p1_pixel_y, p1_move_anim, p1_cell,
  game_mode, input_state, move_locked, current_level, copyprotect_flag

Usage:
  timeout 120 uv run python tools/replay_check.py <candidate.bin> <golden.bin>

Self-test (all-match expected):
  timeout 120 uv run python tools/replay_check.py \
      local/build/render/slice_goldentrace.bin \
      local/build/render/slice_goldentrace.bin
"""
from __future__ import annotations
import sys
import struct
import os
from typing import List, Tuple, Optional

# ---------------------------------------------------------------------------
# Trace format constants (must match game_oracle.py)
# ---------------------------------------------------------------------------
TRACE_MAGIC: bytes = b"GTRACE01"
PLANES_PER_TICK: int = 4 * 0x4000  # 65536 bytes
NAMED_STATE_SIZE: int = 12
TICK_SIZE: int = PLANES_PER_TICK + NAMED_STATE_SIZE
N_PLANES: int = 4
PLANE_BYTES: int = 0x4000

# Named-state field descriptors: (name, offset_in_named_state, byte_width, signed)
# signed=True only for copyprotect_flag (s8: -1 fail / 0 unchecked / 1 pass)
NAMED_STATE_FIELDS: List[Tuple[str, int, int, bool]] = [
    ("p1_pixel_x",        0, 2, False),
    ("p1_pixel_y",        2, 2, False),
    ("p1_move_anim",      4, 1, False),
    ("p1_cell",           5, 1, False),
    ("game_mode",         6, 1, False),
    ("input_state",       7, 1, False),
    ("move_locked",       8, 1, False),
    ("current_level",     9, 1, False),
    ("copyprotect_flag", 10, 1, True),
]


# ---------------------------------------------------------------------------
# Trace reader
# ---------------------------------------------------------------------------
class Trace:
    """Lazy-loaded trace: reads the header eagerly, tick data on demand."""

    def __init__(self, path: str) -> None:
        self.path = path
        with open(path, "rb") as fh:
            header = fh.read(16)
        if len(header) < 16:
            raise ValueError("Trace file too short: %s" % path)
        magic = header[:8]
        if magic != TRACE_MAGIC:
            raise ValueError("Bad magic in %s: got %r expected %r" % (
                path, magic, TRACE_MAGIC))
        self.n_ticks, self.tick_size = struct.unpack_from("<II", header, 8)
        expected_file_size = 16 + self.n_ticks * self.tick_size
        actual_size = os.path.getsize(path)
        if actual_size < expected_file_size:
            raise ValueError(
                "Trace file %s truncated: expected %d B, got %d B" % (
                    path, expected_file_size, actual_size))

    def read_tick(self, tick_idx: int, fh: object = None) -> bytes:
        """Return the raw tick_size bytes for tick tick_idx.

        If fh is provided (an open file object, positioned anywhere), it is used
        directly (seek + read) so the caller can keep the file open across ticks
        and avoid repeated open/close overhead.  If fh is None a temporary open
        is used (safe for single-tick access).
        """
        if tick_idx >= self.n_ticks:
            raise IndexError("Tick %d out of range (n_ticks=%d)" % (
                tick_idx, self.n_ticks))
        offset = 16 + tick_idx * self.tick_size
        if fh is not None:
            fh.seek(offset)  # type: ignore[union-attr]
            data = fh.read(self.tick_size)  # type: ignore[union-attr]
        else:
            with open(self.path, "rb") as _fh:
                _fh.seek(offset)
                data = _fh.read(self.tick_size)
        if len(data) < self.tick_size:
            raise IOError("Short read at tick %d in %s" % (tick_idx, self.path))
        return data

    def planes_at(self, tick_idx: int, fh: object = None) -> List[bytes]:
        """Return list of 4 plane snapshots (each PLANE_BYTES bytes) for one tick."""
        raw = self.read_tick(tick_idx, fh)
        return [raw[p * PLANE_BYTES:(p + 1) * PLANE_BYTES] for p in range(N_PLANES)]

    def named_state_at(self, tick_idx: int, fh: object = None) -> bytes:
        """Return the NAMED_STATE_SIZE bytes for tick tick_idx."""
        raw = self.read_tick(tick_idx, fh)
        return raw[PLANES_PER_TICK:PLANES_PER_TICK + NAMED_STATE_SIZE]


def decode_named_state(ns: bytes) -> dict:
    """Decode named-state bytes into a dict of field_name → value.

    u16 fields use unsigned <H; u8 fields use raw byte; s8 fields (signed=True)
    are sign-extended via struct unpack 'b'.
    """
    result = {}
    for name, off, width, signed in NAMED_STATE_FIELDS:
        chunk = ns[off:off + width]
        if width == 2:
            result[name] = struct.unpack_from("<H", chunk)[0]
        elif signed:
            result[name] = struct.unpack_from("b", chunk)[0]
        else:
            result[name] = chunk[0]
    return result


# ---------------------------------------------------------------------------
# Comparison logic
# ---------------------------------------------------------------------------
def compare_planes(planes_cand: List[bytes], planes_gold: List[bytes],
                   tick_idx: int) -> Optional[str]:
    """Compare planes from two traces at one tick.

    planes_cand is the candidate (reconstructed); planes_gold is the golden reference.
    Returns None on match, or a description string on first divergence.
    """
    for p in range(N_PLANES):
        if planes_cand[p] != planes_gold[p]:
            # Find first differing byte
            first_diff = next(
                (i for i, (bc, bg) in enumerate(zip(planes_cand[p], planes_gold[p]))
                 if bc != bg), None)
            if first_diff is None:
                # Length differs
                return "plane%d length mismatch: %d vs %d" % (
                    p, len(planes_cand[p]), len(planes_gold[p]))
            return ("plane%d first diff at byte 0x%04X: "
                    "candidate=0x%02X golden=0x%02X" % (
                        p, first_diff, planes_cand[p][first_diff],
                        planes_gold[p][first_diff]))
    return None


def compare_named_state(ns_a: bytes, ns_b: bytes,
                        tick_idx: int) -> Optional[str]:
    """Compare named-state bytes from two traces at one tick.

    Returns None on match, or a description string on first divergence.
    """
    decoded_a = decode_named_state(ns_a)
    decoded_b = decode_named_state(ns_b)
    for name, _, _, _ in NAMED_STATE_FIELDS:
        va = decoded_a.get(name, 0)
        vb = decoded_b.get(name, 0)
        if va != vb:
            return "%s: candidate=%d (0x%X)  golden=%d (0x%X)" % (
                name, va, va, vb, vb)
    return None


# ---------------------------------------------------------------------------
# Main comparison entry point
# ---------------------------------------------------------------------------
def compare(candidate_path: str, golden_path: str) -> bool:
    """Compare candidate trace to golden trace.

    Returns True if all ticks match, False otherwise.
    Prints a detailed report to stdout.
    """
    print("[replay_check] loading golden:    %s" % golden_path, flush=True)
    print("[replay_check] loading candidate: %s" % candidate_path, flush=True)

    golden = Trace(golden_path)
    candidate = Trace(candidate_path)

    print("[replay_check] golden:    n_ticks=%d  tick_size=%d" % (
        golden.n_ticks, golden.tick_size), flush=True)
    print("[replay_check] candidate: n_ticks=%d  tick_size=%d" % (
        candidate.n_ticks, candidate.tick_size), flush=True)

    if golden.tick_size != TICK_SIZE:
        print("[replay_check] WARN: golden tick_size=%d expected %d" % (
            golden.tick_size, TICK_SIZE), flush=True)
    if candidate.tick_size != TICK_SIZE:
        print("[replay_check] WARN: candidate tick_size=%d expected %d" % (
            candidate.tick_size, TICK_SIZE), flush=True)

    n_compare = min(golden.n_ticks, candidate.n_ticks)
    if golden.n_ticks != candidate.n_ticks:
        print("[replay_check] WARN: tick count mismatch — comparing first %d ticks" % (
            n_compare), flush=True)

    first_diverge: Optional[int] = None
    first_diverge_reason: str = ""
    plane_mismatches = 0
    state_mismatches = 0
    total_mismatches = 0

    # Keep both files open for the compare loop to avoid repeated open/close per tick.
    with open(golden_path, "rb") as fh_g, open(candidate_path, "rb") as fh_c:
        for tick_idx in range(n_compare):
            planes_g = golden.planes_at(tick_idx, fh_g)
            planes_c = candidate.planes_at(tick_idx, fh_c)
            ns_g = golden.named_state_at(tick_idx, fh_g)
            ns_c = candidate.named_state_at(tick_idx, fh_c)

            plane_err = compare_planes(planes_c, planes_g, tick_idx)
            state_err = compare_named_state(ns_c, ns_g, tick_idx)

            tick_ok = (plane_err is None and state_err is None)
            if not tick_ok:
                total_mismatches += 1
                if plane_err is not None:
                    plane_mismatches += 1
                if state_err is not None:
                    state_mismatches += 1
                if first_diverge is None:
                    first_diverge = tick_idx
                    parts = []
                    if plane_err is not None:
                        parts.append("planes: " + plane_err)
                    if state_err is not None:
                        parts.append("state: " + state_err)
                    first_diverge_reason = "; ".join(parts)

    # Summary
    print("", flush=True)
    if total_mismatches == 0:
        print("[replay_check] RESULT: ALL MATCH — %d/%d ticks identical "
              "(planes plane-exact + named-state field-exact)" % (n_compare, n_compare),
              flush=True)
        # Also print named-state for first and last tick for reference
        if n_compare > 0:
            _print_tick_summary(golden, 0, "tick 0")
            _print_tick_summary(golden, n_compare - 1, "tick %d" % (n_compare - 1))
        return True
    else:
        print("[replay_check] RESULT: DIVERGENCE — %d/%d ticks differ "
              "(%d plane mismatches, %d named-state mismatches)" % (
                  total_mismatches, n_compare, plane_mismatches, state_mismatches),
              flush=True)
        print("[replay_check] FIRST DIVERGENCE at tick %d: %s" % (
            first_diverge, first_diverge_reason), flush=True)
        # Print the named-state for the first diverging tick (both sides)
        if first_diverge is not None:
            ns_g = golden.named_state_at(first_diverge)
            ns_c = candidate.named_state_at(first_diverge)
            dg = decode_named_state(ns_g)
            dc = decode_named_state(ns_c)
            print("  [golden tick %d]    %s" % (first_diverge, _format_state(dg)),
                  flush=True)
            print("  [candidate tick %d] %s" % (first_diverge, _format_state(dc)),
                  flush=True)
        return False


def _format_state(d: dict) -> str:
    return "  ".join("%s=%d" % (k, v) for k, v in d.items())


def _print_tick_summary(trace: Trace, tick_idx: int, label: str) -> None:
    try:
        ns = trace.named_state_at(tick_idx)
        d = decode_named_state(ns)
        print("  [%s] %s" % (label, _format_state(d)), flush=True)
    except Exception as e:
        print("  [%s] error reading: %s" % (label, e), flush=True)


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------
def main() -> None:
    if len(sys.argv) != 3:
        print("Usage: replay_check.py <candidate_trace> <golden_trace>", flush=True)
        sys.exit(2)

    candidate_path = sys.argv[1]
    golden_path = sys.argv[2]

    for path in (candidate_path, golden_path):
        if not os.path.exists(path):
            print("[replay_check] ERROR: file not found: %s" % path, flush=True)
            sys.exit(2)

    ok = compare(candidate_path, golden_path)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
