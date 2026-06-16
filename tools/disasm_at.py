import sys, capstone
data=open(sys.argv[1],'rb').read()
start=int(sys.argv[2],0); n=int(sys.argv[3],0) if len(sys.argv)>3 else 0x100
base=int(sys.argv[4],0) if len(sys.argv)>4 else start
md=capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
for i in md.disasm(data[start:start+n], base):
    print(f"{i.address:04x}: {i.bytes.hex():<14} {i.mnemonic} {i.op_str}")
