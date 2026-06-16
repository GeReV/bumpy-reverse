"""Generate tiny synthetic fixtures for the ImHex patterns (no game data)."""
import os
import struct

OUT = os.path.join(os.path.dirname(__file__), "fixtures")
os.makedirs(OUT, exist_ok=True)


def write(name: str, data: bytes) -> None:
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(data)
    print("wrote", name, len(data), "bytes")


def car() -> bytes:
    # header: first_char=0x20, last=0xFF, dim_a=7, dim_b=8
    b = bytes([0x20, 0xFF, 0x07, 0x08])
    # BE16 offset table @0x04: table[0]=0x08 (phantom), table[1]=0x10 (glyph)
    b += struct.pack(">HH", 0x08, 0x10)
    # phantom blob @0x08 (8 bytes): width=0,height=3,pad,3 rows,2 trailer
    b += bytes([0x00, 0x03, 0x00, 0x11, 0x22, 0x33, 0xAA, 0xBB])
    # glyph @0x10 (8 bytes): width=5,height=3,pad=0,rows F8 88 F8,trailer 0000
    b += bytes([0x05, 0x03, 0x00, 0xF8, 0x88, 0xF8, 0x00, 0x00])
    return b


def bin_() -> bytes:
    DATA = 0x800
    b = bytearray()
    b += struct.pack(">I", 0x0C)          # frame 0 pixels rel offset
    b += struct.pack(">I", 0xFFFFFFFF)    # out-of-range terminator
    b += b"\x00" * (DATA - len(b))        # pad to data base
    b += struct.pack(">HHHHHH", 1, 0, 0, 0, 4, 2)   # header: count,ctrl,_,_,width_words=4,height=2
    b += struct.pack(">8H", *range(0xA0, 0xA8))     # 4*2 = 8 planar u16 pixels
    return bytes(b)


def container() -> bytes:
    w = [0, 0x7D63, 0x1111, 0x2222, 0x8004]   # magic, decoded_size, csumA, csumB, w4(flag+op4)
    w.append(w[0] ^ w[1] ^ w[2] ^ w[3] ^ w[4])
    b = struct.pack(">6H", *w)            # 12-byte record-0 header
    b += bytes([0x55])                    # escape byte @0x0C
    b += bytes([0x10, 0x20, 0x30])        # a few opaque body bytes
    return b


if __name__ == "__main__":
    write("car.bin", car())
    write("bin.bin", bin_())
    write("container.bin", container())
