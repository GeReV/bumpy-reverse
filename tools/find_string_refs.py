import struct, sys
x = open("local/originals/unpacked/BUMPY_unpacked.exe", "rb").read()
hdr = struct.unpack_from("<H", x, 8)[0] * 16
body = x[hdr:]
# DGROUP segment in emulation = 0x204b, image load base = 0x1010.
dgroup_base = (0x204b - 0x1010) * 16
print("DGROUP base in body = %#x" % dgroup_base)
names = ["ENTER YOUR PASSWORD", "PASSWORD ERROR", " PASSWORD OK  ",
         "Enter the platform number", "INSERT THE OTHER DISK", "YOUR PASSWORD"]
OPS = {0xBA: "mov dx", 0xB8: "mov ax", 0xB9: "mov cx", 0xBB: "mov bx",
       0xBE: "mov si", 0xBF: "mov di", 0x68: "push", 0x05: "add ax", 0x81: "alu"}
for nm in names:
    bo = body.find(nm.encode("latin1"))
    if bo < 0:
        print("%-26s NOT FOUND" % repr(nm))
        continue
    drel = (bo - dgroup_base) & 0xFFFF
    le = struct.pack("<H", drel)
    hits = []
    s = 0
    while True:
        i = body.find(le, s)
        if i < 0:
            break
        prev = body[i - 1] if i > 0 else None
        if prev in OPS:
            hits.append((hex(i - 1), OPS[prev]))
        s = i + 1
    print("%-26s body@%#06x dgroup_rel=%#06x  imm-hits=%s" % (repr(nm), bo, drel, hits[:8]))
