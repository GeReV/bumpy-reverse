import sys, capstone
data = open(sys.argv[1],'rb').read()
import struct
# MZ header
(e_magic,e_cblp,e_cp,e_crlc,e_cparhdr,e_minalloc,e_maxalloc,
 e_ss,e_sp,e_csum,e_ip,e_cs,e_lfarlc,e_ovno) = struct.unpack('<2sHHHHHHHHHHHHH', data[:0x1c])
hdr = e_cparhdr*16
entry = hdr + (e_cs*16 + e_ip)
print(f"# pages={e_cp} lastpage={e_cblp} relocs={e_crlc} hdr_paras={e_cparhdr}(={hdr:#x}) "
      f"minalloc={e_minalloc:#x} ss:sp={e_ss:#x}:{e_sp:#x} cs:ip={e_cs:#x}:{e_ip:#x} "
      f"reloc_tbl@{e_lfarlc:#x}")
print(f"# code/entry file offset = {entry:#x}")
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
md.detail = False
n = int(sys.argv[2],0) if len(sys.argv)>2 else 0x140
for i in md.disasm(data[entry:entry+n], entry):
    print(f"{i.address:04x}: {i.bytes.hex():<14} {i.mnemonic} {i.op_str}")
