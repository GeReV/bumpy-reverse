#include <std/mem.pat>
#pragma endian big
#pragma description "Bumpy VEC/PAV/DEC/BUM container (op4-RLE body)"

// Documented 8-byte container header = the first 4 words of the 12-byte op4 record 0.
struct ContainerHeader {
    u16 magic;          // always 0x0000
    u16 decoded_size;   // op4 output size in bytes (record 0's w1); 0 in SCORE.VEC
    u16 checksum_a;
    u16 checksum_b;
};

// w4 of a record. Big-endian: declare the MSB (flag) field first.
bitfield Op {
    flag   : 1;         // 0x8000 per-record flag
    opcode : 15;        // dispatch uses opcode & 0x3f
};

// Reusable 12-byte record header (6 BE16 words).
struct VecRecord {
    u16 w0;
    u16 w1;
    u16 w2;
    u16 w3;
    Op  op;             // w4
    u16 xor_checksum;   // w5 = w0 ^ w1 ^ w2 ^ w3 ^ w4
};

ContainerHeader header @ 0x00;
VecRecord record0 @ 0x00;   // record 0 is always op4 (opcode == 4); w1 == decoded_size

// Body from 0x0D is op4-RLE-compressed; the escape byte is at 0x0C.
// The post-record-0 "records" a checksum walker finds are mostly coincidental hits
// inside the compressed stream — full walking lives in tools/extract/vec_records.py.
u8 escape_byte @ 0x0C;
u8 compressed_body[std::mem::size() - 0x0D] @ 0x0D;
