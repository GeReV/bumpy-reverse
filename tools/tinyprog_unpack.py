#!/usr/bin/env python3
"""
tinyprog_unpack.py — Unpack a TinyProg-protected DOS MZ executable by faithfully
emulating its self-extracting stub(s) with the Unicorn CPU engine, then dumping
the fully-decompressed, relocated program image as a clean MZ executable.

TinyProg (as used by Bumpy's Arcade Fantasy, 1992) wraps the program in TWO
layers, both of which run before the real entry point:

  Layer 1 — CRC16-CCITT-keyed XOR descrambler + anti-tamper.
      A small stub (image offset 0x54) builds a CRC16-CCITT table (poly 0x1021)
      and walks the loaded image backwards in 32 KB blocks, XOR-decrypting each
      word with a rolling key fed by the CRC of the emitted plaintext. The final
      key is compared against a stored checksum; any patched byte diverges the
      key and the stub jumps into garbage ("TINYPROG says, Patched program!").
      It then far-jumps to the layer-2 stub. This layer does NOT relocate.

  Layer 2 — LZSS decompressor + relocator.
      Revealed by layer 1. A bit-pumped LZSS decompressor (literals via movsb,
      matches via back-reference) inflates the actual program, applies its
      relocation fixups, and jumps to the real entry point.

Reproducing either layer's segment arithmetic by hand is error-prone, so we run
the real stub bytes under emulation. We stop at the first DOS call (INT 21h),
which the program's Turbo C startup issues only once both layers are done and
the program is fully in memory. Running the whole thing at TWO different load
segments lets us recover the relocation table by differencing the two outputs:
a word that moves with the load segment is a relocation entry.

Usage:
    python3 tinyprog_unpack.py INPUT.EXE OUTPUT.EXE
"""
import sys, struct
from unicorn import *
from unicorn.x86_const import *

MB = 0x100000
MEM_SIZE = 2 * MB


def load_mz(path: str) -> dict:
    data = bytearray(open(path, "rb").read())
    (magic, cblp, cp, crlc, cparhdr, minalloc, maxalloc,
     ss, sp, csum, ip, cs, lfarlc, ovno) = struct.unpack_from("<2sHHHHHHHHHHHHH", data, 0)
    assert magic == b"MZ", "not an MZ executable"
    hdr_bytes = cparhdr * 16
    img_len = (cp - 1) * 512 + (cblp if cblp else 512)
    return dict(minalloc=minalloc, ss=ss, sp=sp, ip=ip, cs=cs,
                hdr_bytes=hdr_bytes, load_module=data[hdr_bytes:img_len])


def run_full(mz: dict, psp_seg: int) -> tuple[bytearray, dict]:
    """Emulate both unpack layers until the program's first DOS call.
    Returns (mem, info) where info has the final entry regs and the set of
    physical byte addresses written during unpacking."""
    image_seg = psp_seg + 0x10
    lm = mz["load_module"]

    uc = Uc(UC_ARCH_X86, UC_MODE_16)
    uc.mem_map(0, MEM_SIZE)

    psp = bytearray(0x100)
    psp[0:2] = b"\xCD\x20"
    struct.pack_into("<H", psp, 0x02, 0x9FFF)
    uc.mem_write(psp_seg * 16, bytes(psp))
    uc.mem_write(image_seg * 16, bytes(lm))

    uc.reg_write(UC_X86_REG_DS, psp_seg)
    uc.reg_write(UC_X86_REG_ES, psp_seg)
    uc.reg_write(UC_X86_REG_SS, (image_seg + mz["ss"]) & 0xFFFF)
    uc.reg_write(UC_X86_REG_SP, mz["sp"])
    uc.reg_write(UC_X86_REG_CS, (image_seg + mz["cs"]) & 0xFFFF)
    uc.reg_write(UC_X86_REG_IP, mz["ip"])
    uc.reg_write(UC_X86_REG_AX, 0)
    uc.reg_write(UC_X86_REG_BX, 0)

    base_phys = image_seg * 16
    info = {"stop": None, "top": base_phys, "left_stub": False}

    def hook_mem_write(uc, access, address, size, value, user):
        # Track the program's contiguous forward-written extent from the load
        # base. The inner LZSS decompressor writes sequentially upward from
        # image_seg:0, so this converges on the true end of the program image
        # while ignoring the layer-1 scratch written higher up beforehand.
        if address <= info["top"] and address + size > info["top"]:
            info["top"] = address + size
        return True

    def hook_code(uc, address, size, user):
        # The unpack stubs run at the load segment (layer 1) and at a higher
        # scratch segment (layer 2). Once execution has LEFT the load segment
        # (entered layer 2) and returns to it, that is the original program's
        # entry point — snapshot BEFORE the program executes a single
        # instruction, so its image is pristine (no startup self-modification).
        cs = uc.reg_read(UC_X86_REG_CS)
        if cs != image_seg:
            info["left_stub"] = True
        elif info["left_stub"]:
            info.update(
                stop="ENTRY",
                cs=cs, ip=uc.reg_read(UC_X86_REG_IP),
                ss=uc.reg_read(UC_X86_REG_SS), sp=uc.reg_read(UC_X86_REG_SP),
                ds=uc.reg_read(UC_X86_REG_DS), es=uc.reg_read(UC_X86_REG_ES))
            uc.emu_stop()

    def hook_intr(uc, intno, user):
        # Stubs shouldn't fault to DOS; service benignly so emulation continues.
        fl = uc.reg_read(UC_X86_REG_EFLAGS) & ~0x1
        uc.reg_write(UC_X86_REG_EFLAGS, fl)

    uc.hook_add(UC_HOOK_MEM_WRITE, hook_mem_write)
    uc.hook_add(UC_HOOK_CODE, hook_code)
    uc.hook_add(UC_HOOK_INTR, hook_intr)
    try:
        uc.emu_start(image_seg * 16 + mz["ip"], MEM_SIZE, count=80_000_000)
    except UcError as e:
        if info["stop"] is None:
            info["stop"] = f"UcError:{e}"

    info["mem"] = bytearray(uc.mem_read(0, MEM_SIZE))
    return info["mem"], info


def write_mz(path: str, load_module: bytes, relocs: list[int], minalloc: int,
             e_cs: int, e_ip: int, e_ss: int, e_sp: int) -> None:
    nreloc = len(relocs)
    reloc_off = 0x1C
    hdr_paras = (reloc_off + nreloc * 4 + 15) // 16
    hdr_bytes = hdr_paras * 16
    total = hdr_bytes + len(load_module)
    pages = (total + 511) // 512
    cblp = total % 512
    hdr = bytearray(hdr_bytes)
    struct.pack_into("<2sHHHHHHHHHHHHH", hdr, 0,
                     b"MZ", cblp, pages, nreloc, hdr_paras,
                     minalloc, 0xFFFF, e_ss, e_sp, 0, e_ip, e_cs, reloc_off, 0)
    for i, off in enumerate(relocs):
        struct.pack_into("<HH", hdr, reloc_off + i * 4, off & 0xF, off >> 4)
    open(path, "wb").write(bytes(hdr) + bytes(load_module))


def main() -> None:
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(1)
    inp, outp = sys.argv[1], sys.argv[2]
    mz = load_mz(inp)
    print(f"[*] {inp}: load_module={len(mz['load_module'])} bytes "
          f"cs:ip={mz['cs']:#x}:{mz['ip']:#x} ss:sp={mz['ss']:#x}:{mz['sp']:#x}")

    DELTA = 0x1000
    mem1, i1 = run_full(mz, psp_seg=0x1000)
    print(f"[*] run1 stop={i1['stop']} entry={i1.get('cs',0):#06x}:{i1.get('ip',0):#06x} "
          f"ss:sp={i1.get('ss',0):#06x}:{i1.get('sp',0):#06x}")
    if i1["stop"] != "ENTRY":
        print("[!] did not reach program entry"); sys.exit(2)
    mem2, i2 = run_full(mz, psp_seg=0x1000 + DELTA)
    if i2["stop"] != "ENTRY":
        print("[!] run2 did not reach program entry"); sys.exit(2)
    shift = DELTA * 16

    # The unpacked program is based at the canonical EXE load segment (image_seg
    # = psp_seg + 0x10); the inner decompressor writes its output starting there.
    image_seg = 0x1000 + 0x10
    base_phys = image_seg * 16
    base_seg = image_seg
    hi = i1["top"]
    load_module = bytearray(mem1[base_phys:hi])
    entry_cs, entry_ip = i1["cs"], i1["ip"]

    def word(mem: bytes, p: int) -> int:
        return mem[p] | (mem[p + 1] << 8)

    # Scan EVERY byte offset (relocations may sit at odd offsets, e.g. the
    # segment immediate of `mov dx,SEG` whose opcode byte precedes it). A word
    # that moves by exactly the load-segment delta between the two runs is a
    # relocation; normalise it back to a zero load base.
    relocs = []
    for off in range(0, len(load_module) - 1):
        a = word(mem1, base_phys + off)
        b = word(mem2, base_phys + shift + off)
        if a != b and ((b - a) & 0xFFFF) == DELTA:
            relocs.append(off)
            struct.pack_into("<H", load_module, off, (a - base_seg) & 0xFFFF)

    # NB: do NOT trim trailing zeros — the decompressor emits initialised-zero
    # data as part of the load module (the original EXE keeps it in the image;
    # only true BSS beyond the image is covered by minalloc).
    e_cs = (entry_cs - base_seg) & 0xFFFF
    e_ip = entry_ip
    e_ss = (i1["ss"] - base_seg) & 0xFFFF
    e_sp = i1["sp"]
    print(f"[*] program: base_seg(emul)={base_seg:#06x} region=[{base_phys:#x},{hi:#x}) "
          f"load_module={len(load_module):#x} bytes relocs={len(relocs)}")
    print(f"[*] entry={e_cs:#06x}:{e_ip:#06x} stack={e_ss:#06x}:{e_sp:#06x}")

    write_mz(outp, load_module, relocs, mz["minalloc"], e_cs, e_ip, e_ss, e_sp)
    print(f"[*] wrote unpacked EXE -> {outp}")


if __name__ == "__main__":
    main()
