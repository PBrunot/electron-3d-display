// Glyph table + drawing for font.h's 5x7 bitmap font.
//
// No font asset exists anywhere in this repo to port from: the MicroPython overlays
// (device_render_common.py etc.) draw text via framebuf.FrameBuffer.text(), whose glyph
// bitmap lives inside the MicroPython firmware itself, not in this repo's source; the
// only other font-related code here (st7789py.py's text()/font-module system) is unused
// by every app in micropython/ and ships no font *data* either. So this table is authored
// from scratch, not ported.
//
// Authored as literal ASCII-art (one 5-char "#"/"." string per row) rather than hand-typed
// hex bytes, specifically so each glyph's shape is visible and checkable directly in this
// file -- there is no on-device rendering pass available to verify a hand-encoded byte
// table against before it reaches the board, so self-documenting source is the safety net.
// rowBits() takes the string literal's length as a template parameter, so a row with the
// wrong character count is a COMPILE ERROR, not a silently-corrupted glyph.
//
// Covers exactly the character set every current/planned overlay string needs (see the
// parity plan's M2/M3/M4): space, 0-9, A-Z, a-z, and '.', '-', '(', ')', ':', '=' for
// labels like "1s (n=1 l=0 m=0)", "Fe (Z=26)", "FPS: 42.7". Lowercase letters are drawn
// x-height (rows 0-1 blank, no true ascenders/descenders) to keep every glyph in the same
// 7-row cell -- a legibility simplification, not an attempt to reproduce any specific
// existing font. Anything outside this set draws as blank (see glyphFor()'s default) --
// add a case below if a future string needs one, rather than crashing.
#include "font.h"

#include <cstddef>

#include "display.h"

template <size_t N>
constexpr uint8_t rowBits(const char (&s)[N])
{
    static_assert(N == kFontGlyphWidth + 1, "font row must be exactly 5 chars ('#'/'.')");
    uint8_t bits = 0;
    for (int i = 0; i < kFontGlyphWidth; i++)
        if (s[i] == '#')
            bits |= uint8_t(1) << (kFontGlyphWidth - 1 - i);
    return bits;
}

/** bit (kFontGlyphWidth-1-c) of rows[r] set = column c lit, row 0 = top. */
struct Glyph
{
    uint8_t rows[kFontGlyphHeight];
};

static constexpr Glyph kGlyphBlank = {{
    rowBits("....."),
    rowBits("....."),
    rowBits("....."),
    rowBits("....."),
    rowBits("....."),
    rowBits("....."),
    rowBits("....."),
}};

static constexpr Glyph glyphFor(char c)
{
    switch (c)
    {
    case '0':
        return Glyph{{rowBits(".###."), rowBits("#...#"), rowBits("#..##"), rowBits("#.#.#"),
                      rowBits("##..#"), rowBits("#...#"), rowBits(".###.")}};
    case '1':
        return Glyph{{rowBits("..#.."), rowBits(".##.."), rowBits("..#.."), rowBits("..#.."),
                      rowBits("..#.."), rowBits("..#.."), rowBits(".###.")}};
    case '2':
        return Glyph{{rowBits(".###."), rowBits("#...#"), rowBits("....#"), rowBits("...#."),
                      rowBits("..#.."), rowBits(".#..."), rowBits("#####")}};
    case '3':
        return Glyph{{rowBits(".###."), rowBits("#...#"), rowBits("....#"), rowBits("..##."),
                      rowBits("....#"), rowBits("#...#"), rowBits(".###.")}};
    case '4':
        return Glyph{{rowBits("...#."), rowBits("..##."), rowBits(".#.#."), rowBits("#..#."),
                      rowBits("#####"), rowBits("...#."), rowBits("...#.")}};
    case '5':
        return Glyph{{rowBits("#####"), rowBits("#...."), rowBits("####."), rowBits("....#"),
                      rowBits("....#"), rowBits("#...#"), rowBits(".###.")}};
    case '6':
        return Glyph{{rowBits("..##."), rowBits(".#..."), rowBits("#...."), rowBits("####."),
                      rowBits("#...#"), rowBits("#...#"), rowBits(".###.")}};
    case '7':
        return Glyph{{rowBits("#####"), rowBits("....#"), rowBits("...#."), rowBits("..#.."),
                      rowBits(".#..."), rowBits(".#..."), rowBits(".#...")}};
    case '8':
        return Glyph{{rowBits(".###."), rowBits("#...#"), rowBits("#...#"), rowBits(".###."),
                      rowBits("#...#"), rowBits("#...#"), rowBits(".###.")}};
    case '9':
        return Glyph{{rowBits(".###."), rowBits("#...#"), rowBits("#...#"), rowBits(".####"),
                      rowBits("....#"), rowBits("...#."), rowBits(".##..")}};
    case 'A':
        return Glyph{{rowBits("..#.."), rowBits(".#.#."), rowBits("#...#"), rowBits("#...#"),
                      rowBits("#####"), rowBits("#...#"), rowBits("#...#")}};
    case 'B':
        return Glyph{{rowBits("####."), rowBits("#...#"), rowBits("#...#"), rowBits("####."),
                      rowBits("#...#"), rowBits("#...#"), rowBits("####.")}};
    case 'C':
        return Glyph{{rowBits(".####"), rowBits("#...."), rowBits("#...."), rowBits("#...."),
                      rowBits("#...."), rowBits("#...."), rowBits(".####")}};
    case 'D':
        return Glyph{{rowBits("####."), rowBits("#...#"), rowBits("#...#"), rowBits("#...#"),
                      rowBits("#...#"), rowBits("#...#"), rowBits("####.")}};
    case 'E':
        return Glyph{{rowBits("#####"), rowBits("#...."), rowBits("#...."), rowBits("####."),
                      rowBits("#...."), rowBits("#...."), rowBits("#####")}};
    case 'F':
        return Glyph{{rowBits("#####"), rowBits("#...."), rowBits("#...."), rowBits("####."),
                      rowBits("#...."), rowBits("#...."), rowBits("#....")}};
    case 'G':
        return Glyph{{rowBits(".####"), rowBits("#...."), rowBits("#...."), rowBits("#.###"),
                      rowBits("#...#"), rowBits("#...#"), rowBits(".###.")}};
    case 'H':
        return Glyph{{rowBits("#...#"), rowBits("#...#"), rowBits("#...#"), rowBits("#####"),
                      rowBits("#...#"), rowBits("#...#"), rowBits("#...#")}};
    case 'I':
        return Glyph{{rowBits("#####"), rowBits("..#.."), rowBits("..#.."), rowBits("..#.."),
                      rowBits("..#.."), rowBits("..#.."), rowBits("#####")}};
    case 'J':
        return Glyph{{rowBits("....#"), rowBits("....#"), rowBits("....#"), rowBits("....#"),
                      rowBits("#...#"), rowBits("#...#"), rowBits(".###.")}};
    case 'K':
        return Glyph{{rowBits("#...#"), rowBits("#..#."), rowBits("#.#.."), rowBits("##..."),
                      rowBits("#.#.."), rowBits("#..#."), rowBits("#...#")}};
    case 'L':
        return Glyph{{rowBits("#...."), rowBits("#...."), rowBits("#...."), rowBits("#...."),
                      rowBits("#...."), rowBits("#...."), rowBits("#####")}};
    case 'M':
        return Glyph{{rowBits("#...#"), rowBits("##.##"), rowBits("#.#.#"), rowBits("#.#.#"),
                      rowBits("#...#"), rowBits("#...#"), rowBits("#...#")}};
    case 'N':
        return Glyph{{rowBits("#...#"), rowBits("##..#"), rowBits("#.#.#"), rowBits("#.#.#"),
                      rowBits("#..##"), rowBits("#...#"), rowBits("#...#")}};
    case 'O':
        return Glyph{{rowBits(".###."), rowBits("#...#"), rowBits("#...#"), rowBits("#...#"),
                      rowBits("#...#"), rowBits("#...#"), rowBits(".###.")}};
    case 'P':
        return Glyph{{rowBits("####."), rowBits("#...#"), rowBits("#...#"), rowBits("####."),
                      rowBits("#...."), rowBits("#...."), rowBits("#....")}};
    case 'Q':
        return Glyph{{rowBits(".###."), rowBits("#...#"), rowBits("#...#"), rowBits("#...#"),
                      rowBits("#.#.#"), rowBits("#..#."), rowBits(".##.#")}};
    case 'R':
        return Glyph{{rowBits("####."), rowBits("#...#"), rowBits("#...#"), rowBits("####."),
                      rowBits("#.#.."), rowBits("#..#."), rowBits("#...#")}};
    case 'S':
        return Glyph{{rowBits(".####"), rowBits("#...."), rowBits("#...."), rowBits(".###."),
                      rowBits("....#"), rowBits("....#"), rowBits("####.")}};
    case 'T':
        return Glyph{{rowBits("#####"), rowBits("..#.."), rowBits("..#.."), rowBits("..#.."),
                      rowBits("..#.."), rowBits("..#.."), rowBits("..#..")}};
    case 'U':
        return Glyph{{rowBits("#...#"), rowBits("#...#"), rowBits("#...#"), rowBits("#...#"),
                      rowBits("#...#"), rowBits("#...#"), rowBits(".###.")}};
    case 'V':
        return Glyph{{rowBits("#...#"), rowBits("#...#"), rowBits("#...#"), rowBits("#...#"),
                      rowBits("#...#"), rowBits(".#.#."), rowBits("..#..")}};
    case 'W':
        return Glyph{{rowBits("#...#"), rowBits("#...#"), rowBits("#...#"), rowBits("#.#.#"),
                      rowBits("#.#.#"), rowBits("##.##"), rowBits("#...#")}};
    case 'X':
        return Glyph{{rowBits("#...#"), rowBits("#...#"), rowBits(".#.#."), rowBits("..#.."),
                      rowBits(".#.#."), rowBits("#...#"), rowBits("#...#")}};
    case 'Y':
        return Glyph{{rowBits("#...#"), rowBits("#...#"), rowBits(".#.#."), rowBits("..#.."),
                      rowBits("..#.."), rowBits("..#.."), rowBits("..#..")}};
    case 'Z':
        return Glyph{{rowBits("#####"), rowBits("....#"), rowBits("...#."), rowBits("..#.."),
                      rowBits(".#..."), rowBits("#...."), rowBits("#####")}};
    case 'a':
        return Glyph{{rowBits("....."), rowBits("....."), rowBits(".###."), rowBits("....#"),
                      rowBits(".####"), rowBits("#...#"), rowBits(".####")}};
    case 'b':
        return Glyph{{rowBits("#...."), rowBits("#...."), rowBits("####."), rowBits("#...#"),
                      rowBits("#...#"), rowBits("#...#"), rowBits("####.")}};
    case 'c':
        return Glyph{{rowBits("....."), rowBits("....."), rowBits(".####"), rowBits("#...."),
                      rowBits("#...."), rowBits("#...."), rowBits(".####")}};
    case 'd':
        return Glyph{{rowBits("....#"), rowBits("....#"), rowBits(".####"), rowBits("#...#"),
                      rowBits("#...#"), rowBits("#...#"), rowBits(".####")}};
    case 'e':
        return Glyph{{rowBits("....."), rowBits("....."), rowBits(".###."), rowBits("#...#"),
                      rowBits("#####"), rowBits("#...."), rowBits(".####")}};
    case 'f':
        return Glyph{{rowBits("..##."), rowBits(".#..."), rowBits("####."), rowBits(".#..."),
                      rowBits(".#..."), rowBits(".#..."), rowBits(".#...")}};
    case 'g':
        return Glyph{{rowBits("....."), rowBits(".####"), rowBits("#...#"), rowBits("#...#"),
                      rowBits(".####"), rowBits("....#"), rowBits(".###.")}};
    case 'h':
        return Glyph{{rowBits("#...."), rowBits("#...."), rowBits("####."), rowBits("#...#"),
                      rowBits("#...#"), rowBits("#...#"), rowBits("#...#")}};
    case 'i':
        return Glyph{{rowBits("..#.."), rowBits("....."), rowBits(".##.."), rowBits("..#.."),
                      rowBits("..#.."), rowBits("..#.."), rowBits(".###.")}};
    case 'j':
        return Glyph{{rowBits("...#."), rowBits("....."), rowBits("..##."), rowBits("...#."),
                      rowBits("...#."), rowBits("#..#."), rowBits(".##..")}};
    case 'k':
        return Glyph{{rowBits("#...."), rowBits("#...."), rowBits("#..#."), rowBits("#.#.."),
                      rowBits("##..."), rowBits("#.#.."), rowBits("#..#.")}};
    case 'l':
        return Glyph{{rowBits(".##.."), rowBits("..#.."), rowBits("..#.."), rowBits("..#.."),
                      rowBits("..#.."), rowBits("..#.."), rowBits(".###.")}};
    case 'm':
        return Glyph{{rowBits("....."), rowBits("....."), rowBits("##.#."), rowBits("#.#.#"),
                      rowBits("#.#.#"), rowBits("#...#"), rowBits("#...#")}};
    case 'n':
        return Glyph{{rowBits("....."), rowBits("....."), rowBits("####."), rowBits("#...#"),
                      rowBits("#...#"), rowBits("#...#"), rowBits("#...#")}};
    case 'o':
        return Glyph{{rowBits("....."), rowBits("....."), rowBits(".###."), rowBits("#...#"),
                      rowBits("#...#"), rowBits("#...#"), rowBits(".###.")}};
    case 'p':
        return Glyph{{rowBits("....."), rowBits("....."), rowBits("####."), rowBits("#...#"),
                      rowBits("#...#"), rowBits("####."), rowBits("#....")}};
    case 'q':
        return Glyph{{rowBits("....."), rowBits("....."), rowBits(".####"), rowBits("#...#"),
                      rowBits("#...#"), rowBits(".####"), rowBits("....#")}};
    case 'r':
        return Glyph{{rowBits("....."), rowBits("....."), rowBits("#.##."), rowBits("##..."),
                      rowBits("#...."), rowBits("#...."), rowBits("#....")}};
    case 's':
        return Glyph{{rowBits("....."), rowBits("....."), rowBits(".####"), rowBits("#...."),
                      rowBits(".###."), rowBits("....#"), rowBits("####.")}};
    case 't':
        return Glyph{{rowBits(".#..."), rowBits(".#..."), rowBits("####."), rowBits(".#..."),
                      rowBits(".#..."), rowBits(".#..."), rowBits("..##.")}};
    case 'u':
        return Glyph{{rowBits("....."), rowBits("....."), rowBits("#...#"), rowBits("#...#"),
                      rowBits("#...#"), rowBits("#...#"), rowBits(".####")}};
    case 'v':
        return Glyph{{rowBits("....."), rowBits("....."), rowBits("#...#"), rowBits("#...#"),
                      rowBits("#...#"), rowBits(".#.#."), rowBits("..#..")}};
    case 'w':
        return Glyph{{rowBits("....."), rowBits("....."), rowBits("#...#"), rowBits("#...#"),
                      rowBits("#.#.#"), rowBits("#.#.#"), rowBits(".#.#.")}};
    case 'x':
        return Glyph{{rowBits("....."), rowBits("....."), rowBits("#...#"), rowBits(".#.#."),
                      rowBits("..#.."), rowBits(".#.#."), rowBits("#...#")}};
    case 'y':
        return Glyph{{rowBits("....."), rowBits("....."), rowBits("#...#"), rowBits("#...#"),
                      rowBits(".####"), rowBits("....#"), rowBits(".###.")}};
    case 'z':
        return Glyph{{rowBits("....."), rowBits("....."), rowBits("#####"), rowBits("...#."),
                      rowBits("..#.."), rowBits(".#..."), rowBits("#####")}};
    case '.':
        return Glyph{{rowBits("....."), rowBits("....."), rowBits("....."), rowBits("....."),
                      rowBits("....."), rowBits(".##.."), rowBits(".##..")}};
    case '-':
        return Glyph{{rowBits("....."), rowBits("....."), rowBits("....."), rowBits("#####"),
                      rowBits("....."), rowBits("....."), rowBits(".....")}};
    case '(':
        return Glyph{{rowBits("...#."), rowBits("..#.."), rowBits(".#..."), rowBits(".#..."),
                      rowBits(".#..."), rowBits("..#.."), rowBits("...#.")}};
    case ')':
        return Glyph{{rowBits(".#..."), rowBits("..#.."), rowBits("...#."), rowBits("...#."),
                      rowBits("...#."), rowBits("..#.."), rowBits(".#...")}};
    case ':':
        return Glyph{{rowBits("....."), rowBits(".##.."), rowBits(".##.."), rowBits("....."),
                      rowBits(".##.."), rowBits(".##.."), rowBits(".....")}};
    case '=':
        return Glyph{{rowBits("....."), rowBits("....."), rowBits("#####"), rowBits("....."),
                      rowBits("#####"), rowBits("....."), rowBits(".....")}};
    default:
        return kGlyphBlank;
    }
}

void drawChar(uint16_t *frameBuf, int x, int y, char c, uint16_t color)
{
    Glyph g = glyphFor(c);
    for (int row = 0; row < kFontGlyphHeight; row++)
    {
        int py = y + row;
        if (py < 0 || py >= Display::kDisplayHeight)
            continue;
        uint8_t bits = g.rows[row];
        for (int col = 0; col < kFontGlyphWidth; col++)
        {
            if (!(bits & (uint8_t(1) << (kFontGlyphWidth - 1 - col))))
                continue;
            int px = x + col;
            if (px < 0 || px >= Display::kDisplayWidth)
                continue;
            frameBuf[py * Display::kDisplayWidth + px] = color;
        }
    }
}

int drawText(uint16_t *frameBuf, int x, int y, const char *text, uint16_t color)
{
    int cursorX = x;
    for (const char *p = text; *p; p++)
    {
        drawChar(frameBuf, cursorX, y, *p, color);
        cursorX += kFontAdvanceX;
    }
    return cursorX;
}

int textWidth(const char *text)
{
    int n = 0;
    for (const char *p = text; *p; p++)
        n++;
    return n * kFontAdvanceX;
}
