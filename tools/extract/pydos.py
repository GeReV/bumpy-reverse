#!/usr/bin/env python3
"""Pure-Python DOS host that boots Bumpy on our own 16-bit CPU (vec_cpu.py) — no
emulator dependency. It is a faithful port of tools/render/dosemu.py's environment
(MZ loader, INT 21h file I/O, INT 10h/16h, PIT/VGA ports, mode-0Dh planar VGA, the
timer-ISR IRET stubs, keyboard-matrix injection to drive title->menu->PLAY, and the
DOSEMU_LEVEL force) onto the pure-Python interpreter instead of Unicorn.

Goal: a fully standalone level extractor — boot the game, drive it to a level, read
the framebuffer the renderer fills — entirely in Python. See render_levels.py for the
hybrid (emulator-seeded) path this supersedes.
"""
from __future__ import annotations
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools/extract"))
import vec_cpu  # noqa: E402

GAME_DIR = os.path.join(ROOT, "local/originals/old-games/bumpy")
EXE = os.path.join(ROOT, "local/originals/unpacked/BUMPY_unpacked.exe")
PSP_SEG = 0x0100
RAM = 0x110000


def load_mz(path):
    x = open(path, "rb").read()
    e_crlc, e_cparhdr = struct.unpack_from("<HH", x, 6)
    e_ss, e_sp, _chk, e_ip, e_cs = struct.unpack_from("<HHHHH", x, 0x0E)
    e_lfarlc = struct.unpack_from("<H", x, 0x18)[0]
    img = x[e_cparhdr * 16:]
    relocs = [struct.unpack_from("<HH", x, e_lfarlc + i * 4) for i in range(e_crlc)]
    return img, relocs, dict(ss=e_ss, sp=e_sp, ip=e_ip, cs=e_cs)


class Files:
    def __init__(self):
        self.handles = {}
        self.next = 5
        self.bydir = {f.upper(): os.path.join(GAME_DIR, f) for f in os.listdir(GAME_DIR)}

    def open(self, name):
        key = name.upper().split("\\")[-1].split("/")[-1].strip()
        if "." in key:
            b, e = key.rsplit(".", 1)
            key = b.strip() + "." + e.strip()
        path = self.bydir.get(key)
        if not path:
            return -1
        h = self.next; self.next += 1
        self.handles[h] = open(path, "rb")
        return h


class Machine:
    def __init__(self, level=1):
        self.level = level
        self.pav = "D%d.PAV" % level
        self.bum = "D%d.BUM" % level
        self.files = Files()
        self.opened = set()
        self.exited = None
        self.mode = None
        self.io = 0
        # VGA state
        self.seq = bytearray(256); self.gc = bytearray(256)
        self.seq_i = 0; self.gc_i = 0
        self.latch = [0, 0, 0, 0]
        self.plane = [bytearray(0x10000) for _ in range(4)]
        self.dac = [[0, 0, 0] for _ in range(256)]
        self.dac_i = 0; self.dac_sub = 0; self.dac_written = set()
        ATTR = [0, 1, 2, 3, 4, 5, 6, 7, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17]
        self.attr = bytearray(32)
        for i, a in enumerate(ATTR):
            self.attr[i] = a
        self.attr_i = 0; self.attr_ff = 0
        ega = [(0, 0, 0), (0, 0, 42), (0, 42, 0), (0, 42, 42), (42, 0, 0), (42, 0, 42),
               (42, 21, 0), (42, 42, 42), (21, 21, 21), (21, 21, 63), (21, 63, 21),
               (21, 63, 63), (63, 21, 21), (63, 21, 63), (63, 63, 21), (63, 63, 63)]
        for i, c in enumerate(ega):
            self.dac[ATTR[i]] = list(c)
        self.cpu = None
        self.dg = 0

    # ---- boot ----
    def load(self):
        img, relocs, hdr = load_mz(EXE)
        base = PSP_SEG + 0x10
        mem = bytearray(RAM)
        mem[base * 16:base * 16 + len(img)] = img
        for off, seg in relocs:
            lin = base * 16 + ((seg * 16 + off) & 0xFFFFF)
            v = mem[lin] | (mem[lin + 1] << 8)
            v = (v + base) & 0xFFFF
            mem[lin] = v & 0xFF; mem[lin + 1] = v >> 8
        mem[PSP_SEG * 16] = 0xCD; mem[PSP_SEG * 16 + 1] = 0x20      # INT 20h at PSP:0
        mem[PSP_SEG * 16 + 2] = 0x00; mem[PSP_SEG * 16 + 3] = 0xA0  # top of memory
        cpu = vec_cpu.CPU(mem)
        cpu.s["ds"] = PSP_SEG; cpu.s["es"] = PSP_SEG
        cpu.s["ss"] = (hdr["ss"] + base) & 0xFFFF; cpu.r["sp"] = hdr["sp"]
        cpu.s["cs"] = (hdr["cs"] + base) & 0xFFFF; cpu.ip = hdr["ip"]
        cpu.flags = 0x0202
        cpu.entry_sp = 0x10000          # disable op12-isolation halt; run to program exit
        cpu.int_handler = self.on_int
        cpu.io_in = self.on_in
        cpu.io_out = self.on_out
        cpu.mmio_lo = 0xA0000
        cpu.mmio_read = self.vga_read
        cpu.mmio_write = self.vga_write
        self.cpu = cpu
        self.dg = (0x103b + base) & 0xFFFF
        self.base = base
        self.alloc = base + (len(img) + 15) // 16 + 0x100   # paragraph bump allocator
        if self.alloc < 0x1C00:
            self.alloc = 0x1C00
        self.alloc_end = 0x9000
        # default IRET stub (0050:0000) into every still-null IVT vector
        mem[0x500] = 0xCF
        for v in range(0x100):
            if (mem[v * 4] | (mem[v * 4 + 1] << 8) | (mem[v * 4 + 2] << 16) | (mem[v * 4 + 3] << 24)) == 0:
                mem[v * 4] = 0x00; mem[v * 4 + 1] = 0x00; mem[v * 4 + 2] = 0x50; mem[v * 4 + 3] = 0x00

    # ---- helpers ----
    def cf(self, cpu, on):
        cpu.flags = (cpu.flags | 1) if on else (cpu.flags & ~1)

    def rdstr(self, cpu, seg, off):
        out = b""
        while True:
            c = cpu.m[(seg << 4) + off]
            if c == 0:
                break
            out += bytes([c]); off += 1
        return out

    # ---- INT dispatch ----
    def on_int(self, cpu, n):
        ax = cpu.r["ax"]; ah = (ax >> 8) & 0xFF; al = ax & 0xFF
        if n == 0x21:
            return self.int21(cpu, ah, al)
        if n == 0x10:
            if ah == 0x00:
                self.mode = al
            return True
        if n == 0x16:
            if ah in (0x00, 0x10):
                cpu.r["ax"] = 0x0D            # always feed Enter if asked directly
            elif ah in (0x01, 0x11):
                cpu.flags |= 0x40             # ZF=1: no key in BIOS buffer
            return True
        if n == 0x33:                        # mouse: report not present
            cpu.r["ax"] = 0
            return True
        if n == 0x20:
            self.exited = 0; cpu.halted = True
            return True
        # unknown software int: just iret-noop
        return True

    def int21(self, cpu, ah, al):
        if ah == 0x4C:
            self.exited = al; cpu.halted = True
            return True
        if ah == 0x30:
            cpu.r["ax"] = 0x0005
            return True
        if ah == 0x25:                       # set vector AL -> DS:DX
            ds = cpu.s["ds"]; dx = cpu.r["dx"]
            cpu.w16(al * 4, dx); cpu.w16(al * 4 + 2, ds)
            return True
        if ah == 0x35:                       # get vector AL -> ES:BX
            cpu.r["bx"] = cpu.r16(al * 4); cpu.s["es"] = cpu.r16(al * 4 + 2)
            return True
        if ah in (0x1A, 0x2C, 0x2A, 0x44, 0x33, 0x19, 0x0E, 0x09, 0x40):
            return True                      # date/ioctl/console-write/etc — ignore
        if ah == 0x48:                       # allocate BX paragraphs
            bx = cpu.r["bx"]; avail = self.alloc_end - self.alloc
            if bx > avail:
                cpu.r["ax"] = 8; cpu.r["bx"] = avail; self.cf(cpu, True)
            else:
                cpu.r["ax"] = self.alloc; self.alloc += bx; self.cf(cpu, False)
            return True
        if ah in (0x49, 0x4A):
            self.cf(cpu, False)
            return True
        if ah == 0x3D:                       # open DS:DX
            name = self.rdstr(cpu, cpu.s["ds"], cpu.r["dx"]).decode("latin1")
            h = self.files.open(name)
            if h < 0:
                cpu.r["ax"] = 2; self.cf(cpu, True)
            else:
                self.opened.add(name.upper().split("\\")[-1].split("/")[-1].strip())
                cpu.r["ax"] = h; self.cf(cpu, False)
            return True
        if ah == 0x3F:                       # read BX -> DS:DX, CX bytes
            h = cpu.r["bx"]; cx = cpu.r["cx"]; dst = (cpu.s["ds"] << 4) + cpu.r["dx"]
            f = self.files.handles.get(h)
            data = f.read(cx) if f else b""
            cpu.m[dst:dst + len(data)] = data
            if len(data) < cx:               # zero the unread tail (op4 decodes in place)
                for i in range(len(data), cx):
                    cpu.m[dst + i] = 0
            cpu.r["ax"] = len(data)
            return True
        if ah == 0x3E:                       # close
            h = cpu.r["bx"]
            if h in self.files.handles:
                self.files.handles.pop(h).close()
            return True
        if ah == 0x42:                       # seek
            h = cpu.r["bx"]; f = self.files.handles.get(h)
            if f:
                off = (cpu.r["cx"] << 16) | cpu.r["dx"]
                f.seek(off, al & 0xFF if al in (0, 1, 2) else 0)
                p = f.tell()
                cpu.r["dx"] = (p >> 16) & 0xFFFF; cpu.r["ax"] = p & 0xFFFF
            return True
        if ah == 0x3C:                       # create — pretend success (game may write saves)
            cpu.r["ax"] = 0xFFFE; self.cf(cpu, False)
            return True
        return True                          # default: succeed/no-op

    # ---- ports ----
    def on_in(self, cpu, port, size):
        self.io += 1
        if port == 0x40:
            return (self.io * 0x11) & 0xFF
        if port == 0x201:
            return 0xF0
        if port == 0x3DA:
            self.attr_ff = 0
            return (self.io & 1) * 0x09
        if port == 0x60:
            return self.cur_scan
        if port == 0x61:
            return 0xFF
        return 0xFF

    def on_out(self, cpu, port, size, value):
        value &= 0xFFFF
        if port == 0x3C0:
            if self.attr_ff == 0:
                self.attr_i = value & 0x1F; self.attr_ff = 1
            else:
                if self.attr_i < 0x20:
                    self.attr[self.attr_i] = value & 0xFF
                self.attr_ff = 0
            return
        if port in (0x3C4, 0x3CE):
            reg = self.seq if port == 0x3C4 else self.gc
            if port == 0x3C4:
                self.seq_i = value & 0xFF
            else:
                self.gc_i = value & 0xFF
            if size == 2:
                reg[value & 0xFF] = (value >> 8) & 0xFF
            return
        if port in (0x3C5, 0x3CF):
            reg = self.seq if port == 0x3C5 else self.gc
            idx = self.seq_i if port == 0x3C5 else self.gc_i
            reg[idx] = value & 0xFF
            return
        if port == 0x3C8:
            self.dac_i = value & 0xFF; self.dac_sub = 0
            return
        if port == 0x3C9:
            self.dac[self.dac_i & 0xFF][self.dac_sub] = value & 0x3F
            self.dac_sub += 1
            if self.dac_sub == 3:
                self.dac_written.add(self.dac_i & 0xFF)
                self.dac_sub = 0; self.dac_i = (self.dac_i + 1) & 0xFF
            return

    # ---- VGA planar memory (0xA0000-0xAFFFF) ----
    def vga_write(self, lin, val):
        if lin >= 0xB0000:
            return
        off = (lin - 0xA0000) & 0xFFFF
        gc = self.gc; seq = self.seq; latch = self.latch
        wm = gc[5] & 3; mm = seq[2] & 0xF; bm = gc[8]
        sr = gc[0]; esr = gc[1]; fn = (gc[3] >> 3) & 3; rot = gc[3] & 7
        for p in range(4):
            if not (mm & (1 << p)):
                continue
            lat = latch[p]
            if wm == 1:
                res = lat
            elif wm == 2:
                v = 0xFF if (val & (1 << p)) else 0
                if fn == 1: v &= lat
                elif fn == 2: v |= lat
                elif fn == 3: v ^= lat
                res = (v & bm) | (lat & ~bm)
            elif wm == 3:
                rv = ((val >> rot) | (val << (8 - rot))) & 0xFF if rot else val
                m = bm & rv; sv = 0xFF if (sr & (1 << p)) else 0
                res = (sv & m) | (lat & ~m)
            else:
                v = ((val >> rot) | (val << (8 - rot))) & 0xFF if rot else val
                if esr & (1 << p):
                    v = 0xFF if (sr & (1 << p)) else 0
                if fn == 1: v &= lat
                elif fn == 2: v |= lat
                elif fn == 3: v ^= lat
                res = (v & bm) | (lat & ~bm)
            self.plane[p][off] = res & 0xFF

    def vga_read(self, lin):
        if lin >= 0xB0000:
            return 0
        off = (lin - 0xA0000) & 0xFFFF
        for p in range(4):
            self.latch[p] = self.plane[p][off]
        return self.plane[self.gc[4] & 3][off]

    # ---- driving the game ----
    def set_key(self, scancode, down):
        mbase = self.cpu.r16((self.dg << 4) + 0x4D42)
        self.cpu.m[(self.dg << 4) + mbase + (scancode & 0x7F)] = scancode if down else 0

    def force_level(self):
        self.cpu.m[(self.dg << 4) + 0x79B2] = self.level & 0xFF
        self.cpu.m[(self.dg << 4) + 0x119A] = 1

    def fire_int(self, n):
        cpu = self.cpu
        cpu.push(cpu.flags | 0xF002); cpu.push(cpu.s["cs"]); cpu.push(cpu.ip)
        vec = cpu.r16(n * 4) | (cpu.r16(n * 4 + 2) << 16)
        if vec == 0:
            cpu.ip = cpu.pop(); cpu.s["cs"] = cpu.pop(); cpu.flags = cpu.pop()
            return
        cpu.ip = vec & 0xFFFF; cpu.s["cs"] = (vec >> 16) & 0xFFFF

    cur_scan = 0

    def run(self, settle=60, max_instr=400_000_000):
        cpu = self.cpu
        CHUNK = 1_000_000
        total = 0; c = 0; countdown = None
        while total < max_instr and not cpu.halted:
            for _ in range(CHUNK):
                cpu.step()
                if cpu.halted:
                    break
            total += CHUNK; c += 1
            if cpu.halted:
                break
            for sc in (0x3D, 0x41, 0x39, 0x1C):
                self.set_key(sc, False)
            if self.pav not in self.opened:
                self.force_level()
            if self.pav not in self.opened:
                if c <= 14:
                    self.set_key(0x3D, True); self.set_key(0x41, True)
                elif c >= 16 and (c // 2) % 2 == 0:
                    self.set_key(0x39, True)
            if countdown is None and self.bum in self.opened:
                countdown = settle
                print("level loaded (%s) at chunk %d" % (self.bum, c), flush=True)
            if countdown is not None and countdown > 48 and (c // 2) % 2 == 0:
                self.set_key(0x39, True)
            if countdown is not None:
                countdown -= 1
                if countdown <= 0:
                    break
            self.fire_int(8)
        return total


def main():
    level = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    settle = int(os.environ.get("PYDOS_SETTLE", "60"))
    m = Machine(level)
    m.load()
    print("booting Bumpy (pure-Python CPU), level %d ..." % level, flush=True)
    total = m.run(settle=settle)
    print("ran %d Minstr, mode=%s, files=%s" % (
        total // 1_000_000, hex(self_mode(m)), sorted(m.opened)), flush=True)
    # dump memory for inspection / framebuffer extraction
    out = os.path.join(ROOT, "local/build/render/pydos_ram.bin")
    open(out, "wb").write(bytes(m.cpu.m[0x10000:0xA0000]))
    print("memory -> build/render/pydos_ram.bin", flush=True)


def self_mode(m):
    return m.mode if m.mode is not None else 0


if __name__ == "__main__":
    main()
