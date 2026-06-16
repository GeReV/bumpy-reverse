#include <std/mem.pat>
#pragma endian big
#pragma description "Bumpy .CAR bitmap font (caractères)"

// 4-byte header
struct Header {
    u8 first_char;   // 0x20 (space) — code of first glyph
    u8 last_char;    // 0xFF
    u8 dim_a;        // nominal cell cols (e.g. 7)
    u8 dim_b;        // nominal cell rows (e.g. 8)
};

// One glyph blob: variable width (<=8 px) and height (proportional font)
struct Glyph {
    u8 width;        // pixels, <= 8
    u8 height;       // rows
    u8 pad;          // 0x00
    u8 rows[height]; // one byte per row, MSB = leftmost pixel
    u16 trailer;     // advance / kerning (not needed to render)
};

// Each BE16 table entry is a file offset; place its glyph there.
// entries[0] is the "phantom": a non-glyph blob in real files (~198 bytes), so the
// Glyph overlay on it is meaningless — real glyphs start at index 1.
// Char-to-index mapping: char C -> table index (C - first_char + 1).
struct GlyphEntry {
    u16 offset;
    Glyph glyph @ offset;
};

Header header @ 0x00;

// Offset table runs from 0x04 up to the first offset value.
u16 first_offset @ 0x04;
GlyphEntry entries[(first_offset - 4) / 2] @ 0x04;
