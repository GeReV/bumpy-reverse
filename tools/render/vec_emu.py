#!/usr/bin/env python3
"""Emulate Bumpy's in-EXE .VEC renderer (vec_run, overlay seg 1c28) on a real
.VEC file under Unicorn, tracing what it draws.

Approach:
  * Load the unpacked MZ at load-seg 0x1000 and apply its relocations -> this
    reproduces Ghidra's addresses exactly (DGROUP=0x203b, vec_run=0x1c28:0).
  * Map 1 MB. Put the .VEC bytes at 0x8000:0. Stack at 0x7000. VGA window
    0xA0000+ is captured via a write hook; VGA register ports via an I/O hook.
  * Set the registers vec_decode would (SI=stream off, DI=stream seg, AX/BX =
    initial point, CX/DX -> clip setup) and FAR-call vec_run at 0x1c28:0, with a
    far return address of 0x9000:0 so emu stops cleanly when it returns.

This first pass TRACES (instr count, BIOS/DOS/overlay int calls, VGA port writes,
and writes into the 0xA0000 window) so we can confirm the video mode and write
pattern before committing to a framebuffer reconstruction. Output is written
under build/render/ (gitignored).
"""
import sys, os, struct, collections, zlib
from unicorn import *
from unicorn.x86_const import *

FB_SEG = 0x4000   # redirect the renderer's screen-base global here (clean region)


def write_png_gray(path, w, h, data):
    """Minimal grayscale PNG (pure stdlib)."""
    def chunk(typ, body):
        c = struct.pack(">I", len(body)) + typ + body
        return c + struct.pack(">I", zlib.crc32(typ + body) & 0xFFFFFFFF)
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += data[y * w:(y + 1) * w]
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)

EXE = "local/originals/unpacked/BUMPY_unpacked.exe"
LOAD_SEG = 0x1000
VEC_SEG = 0x8000          # .VEC buffer segment (linear 0x80000)
STACK_SEG = 0x7000
RET_SEG = 0x9000          # far-return sentinel (emu 'until' stops here)
VGA = 0xA0000
OUT = "local/build/render"


def load_mz(path):
    x = open(path, "rb").read()
    e_cblp, e_cp = struct.unpack_from("<HH", x, 2)
    e_crlc, e_cparhdr = struct.unpack_from("<HH", x, 6)
    e_lfarlc = struct.unpack_from("<H", x, 0x18)[0]
    hdr = e_cparhdr * 16
    img = x[hdr:]
    relocs = []
    for i in range(e_crlc):
        off, seg = struct.unpack_from("<HH", x, e_lfarlc + i * 4)
        relocs.append((off, seg))
    return img, relocs


def main():
    vec_path = sys.argv[1] if len(sys.argv) > 1 else "local/originals/old-games/bumpy/TITRE.VEC"
    si_start = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    os.makedirs(OUT, exist_ok=True)
    img, relocs = load_mz(EXE)
    vec = open(vec_path, "rb").read()

    uc = Uc(UC_ARCH_X86, UC_MODE_16)
    uc.mem_map(0, 0x100000)
    uc.mem_write(LOAD_SEG * 16, img)
    for off, seg in relocs:
        lin = LOAD_SEG * 16 + ((seg * 16 + off) & 0xFFFFF)
        w = struct.unpack("<H", uc.mem_read(lin, 2))[0]
        uc.mem_write(lin, struct.pack("<H", (w + LOAD_SEG) & 0xFFFF))
    uc.mem_write(VEC_SEG * 16, vec)

    # --- trace state ---
    tr = dict(instr=0, vga_writes=0, vga_min=0xFFFFF, vga_max=0, ints=collections.Counter(),
              ports=collections.Counter(), farcalls=collections.Counter(), last_ip=0,
              wbucket=collections.Counter())
    vga_seen = bytearray(0x10000)
    vga_touched = bytearray(0x10000)
    # capture writes into a candidate back-buffer region too (everything above the
    # image+stack but below VGA, plus VGA itself)
    IMG_TOP = (LOAD_SEG * 16) + len(img)

    path = []
    visits = collections.Counter()
    KEY = {(LOAD_SEG + 0xc28) * 16 + 0x00: "vec_run",
           (LOAD_SEG + 0xc28) * 16 + 0x3c: "loop_top",
           (LOAD_SEG + 0xc28) * 16 + 0x5e: "dispatch_call",
           (LOAD_SEG + 0xc28) * 16 + 0xa09: "vec_read_record",
           (LOAD_SEG + 0xc28) * 16 + 0x194: "h_op4",
           (LOAD_SEG + 0xc28) * 16 + 0x4b0: "h_op12",
           (LOAD_SEG + 0xcda) * 16 + 0x00: "vec_xform"}

    DGROUP = (LOAD_SEG + 0x103b) * 16
    state = {"redirected": False}

    def hook_code(uc, addr, size, _):
        tr["instr"] += 1
        tr["last_ip"] = addr
        if addr in KEY:
            visits[KEY[addr]] += 1
        if len(path) < 240:
            path.append(addr)
        # one-shot: after vec_run's setup overwrote the screen-base globals
        # [0x4e0c]:[0x4e0a], redirect them to a clean framebuffer segment so the
        # plot writes land outside the loaded image (no code corruption/derail).
        if addr == (LOAD_SEG + 0xc28) * 16 + 0x3c and not state["redirected"]:
            uc.mem_write(DGROUP + 0x4e0c, struct.pack("<H", FB_SEG))
            uc.mem_write(DGROUP + 0x4e0a, struct.pack("<H", 0x0000))
            state["redirected"] = True

    fb = {}  # captured framebuffer writes: linear addr -> byte

    def hook_mem_write(uc, access, addr, size, value, _):
        # histogram of all writes by 8 KB region (to locate the draw target)
        tr["wbucket"][addr & ~0x1FFF] += 1
        # capture writes anywhere in the clean redirect window (0x20000..0x44000)
        if 0x20000 <= addr < 0x44000:
            for k in range(size):
                fb[addr + k] = (value >> (8 * k)) & 0xFF
        if VGA <= addr < VGA + 0x10000:
            tr["vga_writes"] += 1
            tr["vga_min"] = min(tr["vga_min"], addr)
            tr["vga_max"] = max(tr["vga_max"], addr + size)
            for k in range(size):
                o = (addr - VGA) + k
                if o < 0x10000:
                    vga_seen[o] = (value >> (8 * k)) & 0xFF
                    vga_touched[o] = 1

    def hook_intr(uc, intno, _):
        ah = (uc.reg_read(UC_X86_REG_AX) >> 8) & 0xFF
        tr["ints"][(intno, ah)] += 1
        tr["int_from"] = tr["last_ip"]
        # AH=0 int10 = set mode; just record AL (mode). Return from int.
        # Pop the int frame (IP, CS, FLAGS) so execution continues.
        sp = uc.reg_read(UC_X86_REG_SP)
        ss = uc.reg_read(UC_X86_REG_SS)
        # emulate IRET
        ip = struct.unpack("<H", uc.mem_read(ss * 16 + sp, 2))[0]
        cs = struct.unpack("<H", uc.mem_read(ss * 16 + sp + 2, 2))[0]
        fl = struct.unpack("<H", uc.mem_read(ss * 16 + sp + 4, 2))[0]
        uc.reg_write(UC_X86_REG_SP, (sp + 6) & 0xFFFF)
        uc.reg_write(UC_X86_REG_CS, cs)
        uc.reg_write(UC_X86_REG_IP, ip)

    def hook_io_out(uc, port, size, value, _):
        if 0x3B0 <= port <= 0x3DF:
            tr["ports"][port] += 1

    uc.hook_add(UC_HOOK_CODE, hook_code)
    uc.hook_add(UC_HOOK_MEM_WRITE, hook_mem_write)
    uc.hook_add(UC_HOOK_INTR, hook_intr)
    try:
        uc.hook_add(UC_HOOK_INSN, hook_io_out, None, 1, 0, UC_X86_INS_OUT)
    except Exception:
        pass

    # --- stack + far return sentinel ---
    uc.mem_write(RET_SEG * 16, b"\xf4")  # hlt (belt-and-suspenders; we also stop via 'until')
    SS, SP = STACK_SEG, 0xFFF0
    def push16(v):
        nonlocal SP
        SP = (SP - 2) & 0xFFFF
        uc.mem_write(SS * 16 + SP, struct.pack("<H", v))
    push16(RET_SEG)   # return CS
    push16(0x0000)    # return IP
    uc.reg_write(UC_X86_REG_SS, SS)
    uc.reg_write(UC_X86_REG_SP, SP)

    # registers as vec_decode sets them
    uc.reg_write(UC_X86_REG_SI, si_start)   # stream offset
    uc.reg_write(UC_X86_REG_DI, VEC_SEG)    # stream segment
    uc.reg_write(UC_X86_REG_AX, 0)          # initial x
    uc.reg_write(UC_X86_REG_BX, 0)          # initial y
    uc.reg_write(UC_X86_REG_CX, 320)        # clip setup param (guess)
    uc.reg_write(UC_X86_REG_DX, 200)        # clip setup param (guess)
    uc.reg_write(UC_X86_REG_DS, (LOAD_SEG + 0x103b) & 0xFFFF)
    uc.reg_write(UC_X86_REG_ES, VEC_SEG)
    uc.reg_write(UC_X86_REG_CS, LOAD_SEG + 0xc28)   # 0x1c28
    uc.reg_write(UC_X86_REG_IP, 0)

    begin = (LOAD_SEG + 0xc28) * 16   # vec_run @ 1c28:0
    err = None
    try:
        uc.emu_start(begin, RET_SEG * 16, count=20_000_000)
    except UcError as e:
        err = "%s @ CS:IP last=%#x" % (e, tr["last_ip"])

    # --- report ---
    name = os.path.basename(vec_path)
    touched = sum(vga_touched)
    rep = []
    rep.append("vec: %s (%d bytes), si_start=%d" % (name, len(vec), si_start))
    rep.append("exit: %s" % (err or "clean return"))
    rep.append("instructions: %d" % tr["instr"])
    rep.append("VGA writes: %d, touched bytes: %d, range: %#x..%#x" % (
        tr["vga_writes"], touched,
        tr["vga_min"] if tr["vga_writes"] else 0, tr["vga_max"]))
    rep.append("INT calls: " + ", ".join("int%02xh/ah%02x=%d" % (a, b, n) for (a, b), n in tr["ints"].most_common(10)))
    rep.append("VGA ports OUT: " + ", ".join("%#x=%d" % (p, n) for p, n in tr["ports"].most_common(12)))
    rep.append("top write regions (8KB buckets): " + ", ".join(
        "%#07x=%d" % (a, n) for a, n in tr["wbucket"].most_common(10)))
    rep.append("key-address visits: " + ", ".join("%s=%d" % (k, v) for k, v in visits.most_common()))
    rep.append("int origin (last_ip): %#x" % tr.get("int_from", 0))
    rep.append("first instruction path (linear):")
    rep.append("  " + " ".join("%05x" % a for a in path[:120]))
    print("\n".join(rep))
    open(os.path.join(OUT, name + ".trace.txt"), "w").write("\n".join(rep) + "\n")
    # --- reconstruct framebuffer from captured writes ---
    if fb:
        lo, hi = min(fb), max(fb)
        rep.append("captured FB writes: %d bytes, linear range %#x..%#x (span %d)" % (
            len(fb), lo, hi, hi - lo))
        # dense bitmap over the captured span
        span = hi - lo + 1
        buf = bytearray(span)
        for a, v in fb.items():
            buf[a - lo] = v
        open(os.path.join(OUT, name + ".fb.bin"), "wb").write(bytes(buf))
        # Render attempts: bytes look like packed 8-px columns; try several widths
        # as 8-bit grayscale so we can eyeball the structure.
        for wbytes in (40, 80, 160, 320):
            h = min(240, span // wbytes)
            if h < 8:
                continue
            # expand each byte's 8 bits to 8 gray pixels (1bpp view per plane)
            px = bytearray(wbytes * 8 * h)
            for y in range(h):
                for xb in range(wbytes):
                    b = buf[y * wbytes + xb]
                    for bit in range(8):
                        px[(y * wbytes + xb) * 8 + bit] = 0xFF if (b >> (7 - bit)) & 1 else 0
            write_png_gray(os.path.join(OUT, "%s.w%d.png" % (name, wbytes * 8)), wbytes * 8, h, px)
        print("wrote FB dump + PNG attempts -> %s/%s.fb.bin, %s.w*.png" % (OUT, name, name))
    print("\n".join(rep[-3:]))


if __name__ == "__main__":
    main()
