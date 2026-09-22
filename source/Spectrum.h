#pragma once

#include <cstdint>
#include <vector>

/**
	The ZX Spectrum's display file, and the order a tape fills it in.

	The Spectrum's screen memory is not linear, and that is the entire visual
	idea of this plugin. 6144 bytes of bitmap are addressed by taking the pixel
	row apart into three fields -- which **third** of the screen, which **line**
	within a character cell, and which **character row** within the third -- and
	putting them back together in that order, most significant first. Walking
	the addresses in order therefore walks the screen in an eight-line
	interleave, three times, which is the venetian-blind pattern everybody who
	has watched a Spectrum load remembers. 768 bytes of attributes follow, in
	plain character order, which is why the picture lands in monochrome and
	colours in at the very end.

	Two derivations of that order live here on purpose:

	  ScreenIndex()  the bit layout, as an arithmetic expression.
	  ScreenOrder()  a plain nested loop over thirds, lines and character rows,
	                 which enumerates the addresses without doing arithmetic on
	                 the row number at all.

	`pttest --agree` asserts the two agree for all 6144 addresses. It is integer
	arithmetic on both sides, so the comparison is bitwise with no tolerance,
	and it is very much cheaper than discovering a transposition after the look
	has been tuned around it.
*/
namespace pilot::zx
{
constexpr int kScreenW       = 256;
constexpr int kScreenH       = 192;
constexpr int kCellsX        = kScreenW / 8;///< 32
constexpr int kCellsY        = kScreenH / 8;///< 24
constexpr int kDisplayBytes  = 6144;        ///< the bitmap
constexpr int kAttributeBytes = 768;        ///< one per character cell
constexpr int kTotalBytes    = kDisplayBytes + kAttributeBytes;///< 6912

/// The tape is blocked, and a block is what fails. 6912 divides by 256
/// exactly, which is the only reason this number is 256.
constexpr int kBlockBytes = 256;
constexpr int kBlocks     = kTotalBytes / kBlockBytes;///< 27

/**
	Deliberate defects, for the negative controls.

	A check that cannot fail is not a check. `pttest --negative` runs the real
	checks against a perturbed address order and asserts that each one reports
	a failure. The perturbation is applied to ScreenIndex() alone -- ScreenOrder()
	is always exact -- so `--agree` is testing what it claims to test.
*/
enum Perturbation
{
	kExact           = 0,
	kSwapLineCharrow = 1,///< line and character row exchanged: a transposition
	kOffByOneRow     = 2,///< charrow*33 instead of charrow*32: one byte of drift
	kLinearOrder     = 3,///< a plain top-down wipe, which is what this is not
};

/// The address of the byte holding pixel (px, py). 0..6143.
int ScreenIndex( int px, int py, int perturb = kExact );

/// The address of the attribute byte governing pixel (px, py). 6144..6911.
int AttributeIndex( int px, int py );

/// Has the byte holding this pixel arrived yet?
bool PixelRevealed( int px, int py, int bytesRevealed, int perturb = kExact );

/// Has this character cell's attribute byte arrived yet? Never before 6144.
bool AttributeRevealed( int cellX, int cellY, int bytesRevealed );

/// One byte of the display file: eight horizontal pixels of one row.
struct Address
{
	int xbyte;///< 0..31
	int py;   ///< 0..191
};

/**
	The display file in tape order, written the other way.

	Three thirds, each an eight-line interleave: for every line within a
	character cell, every character row of that third, every byte across. No
	arithmetic is done on the row number here -- the loop nesting *is* the
	address -- so agreeing with ScreenIndex() means two genuinely different
	statements of the layout agree.
*/
std::vector< Address > ScreenOrder();

//---------------------------------------------------------------------------
// The palette.
//
// Fifteen colours: eight hues at the basic level and seven at the bright one
// (bright black is the same black). The hue is three bits, one per gun, in
// GRB order -- bit 2 green, bit 1 red, bit 0 blue -- and BRIGHT swings the
// level of whichever guns are on.
//
// The two levels, 0xD7 and 0xFF, and the bit order are taken from nesolume's
// console table (`source/Consoles.cpp`, the ZX Spectrum rows), which is where
// this fleet already wrote them down. See ATTRIBUTIONS.md.
//---------------------------------------------------------------------------
constexpr int kBasicLevel  = 215;///< 0xD7
constexpr int kBrightLevel = 255;///< 0xFF

struct Rgb
{
	uint8_t r, g, b;
};

/// Colour `index` (0..7) at the basic or bright level.
Rgb Colour( int index, bool bright );

/// The eight hues, in the order the Spectrum's BASIC numbers them.
const char* const* ColourNames();
constexpr int kColourCount = 8;

} // namespace pilot::zx
