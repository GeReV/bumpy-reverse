#include <std/mem.pat>
#pragma endian big
#pragma description "Bumpy .BIN sprite bank (BUMSPJEU, FLECHE)"

// 12-byte per-frame header; sits at (frame_ptr - 0xC). 6 BE16 words.
struct FrameHeader {
    u16 count;
    u16 ctrl_word;    // ctrl byte = high byte of this word; ctrl & 0x40 => mask-RLE pixels
    u16 _w2;
    u16 _w3;
    u16 width_words;  // row width in BE16 words; sprite px wide = width_words/4 * 16
    u16 height;       // rows
};

struct Frame {
    FrameHeader hdr;
    // raw frames are 4-plane planar, interleaved in 16px blocks (ctrl & 0x40 == 0).
    u16 pixels[hdr.width_words * hdr.height];
};

// Flat BE32 frame-offset table at 0x00; each entry is relative to data base 0x800.
// A 12-byte header precedes the pixel pointer, so the header starts at 0x800+offset-0xC.
struct FrameEntry {
    u32 offset;
    Frame frame @ (0x800 + offset - 0xC);
};

struct Bin {
    // The frame-offset table occupies [0, 0x800); read BE32 entries until the cursor
    // reaches the data base (0x800/4 = 512 entries max) or an entry points past EOF.
    // (Mirrors tools/extract/sprite_container.py: range(0x800//4), break when out of range.)
    // NOTE: FLECHE.BIN is a degenerate single-frame bank the game loads via a dedicated
    // path (tools/extract/render_fleche.py). Its table is not densely packed, so this
    // generic walk over-counts it — only frame 0 (pixels @ 0x80c, the 16x16 arrow) is real.
    FrameEntry entries[while($ < 0x800 && 0x800 + std::mem::read_unsigned($, 4, std::mem::Endian::Big) < std::mem::size())];
};

Bin bin @ 0x00;
