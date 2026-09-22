#pragma once

#include <cstdint>
#include <vector>

/**
    A 5x7 bitmap font, for every piece of text the patterns carry.

    Cell labels, tile references, corner pixel counts, safe-area percentages,
    the burn-in and the frame counter are all drawn by the fragment shader from
    one small texture built out of this table. A bitmap font is the right tool
    for a test pattern: a glyph is an exact arrangement of whole pixels at an
    integer scale, so what the operator reads on the wall is the same pixels
    the file says, with no rasteriser, no hinting and no anti-aliasing between
    them. A blurred label on a pixel-mapping pattern would be evidence of a
    scaler in the chain -- unless the label was blurry to begin with.

    The glyphs are written as pictures in Font.cpp so they can be read, and
    `gttest --font` prints them back so they can be checked by eye. Printable
    ASCII only (32..126), one row per glyph line, top row first, and column 0 is
    the leftmost pixel.
*/
namespace pilot::font
{
constexpr int kFirst  = 32;///< first code covered
constexpr int kCount  = 96;///< 32..127
constexpr int kWidth  = 5;
constexpr int kHeight = 7;
/// Horizontal advance: the glyph plus one blank column.
constexpr int kAdvance = kWidth + 1;

/// The glyph for `code` as seven rows of five characters, '#' for a lit pixel.
/// Anything outside the covered range is a blank glyph.
const char* const* Glyph( int code );

/// Is pixel (x, y) of glyph `code` lit?
bool Bit( int code, int x, int y );

/// The whole table as an 8-bit single-channel image, kWidth*128 wide and
/// kHeight tall, indexed by ASCII code directly: glyph for code c starts at
/// column c*kWidth. Codes below kFirst are blank so the shader never has to
/// subtract anything.
std::vector< uint8_t > Texture();

constexpr int kTextureWidth  = kWidth * 128;
constexpr int kTextureHeight = kHeight;

} // namespace pilot::font
