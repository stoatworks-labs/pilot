#include "Spectrum.h"

#include <algorithm>

namespace pilot::zx
{
int ScreenIndex( int px, int py, int perturb )
{
	px = std::clamp( px, 0, kScreenW - 1 );
	py = std::clamp( py, 0, kScreenH - 1 );

	const int xbyte   = px >> 3;
	const int third   = py >> 6;      // 0, 1, 2
	const int charrow = ( py >> 3 ) & 7;
	const int line    = py & 7;

	switch( perturb )
	{
		case kSwapLineCharrow:
			return third * 2048 + charrow * 256 + line * 32 + xbyte;
		case kOffByOneRow:
			return std::min( third * 2048 + line * 256 + charrow * 33 + xbyte, kDisplayBytes - 1 );
		case kLinearOrder:
			return py * 32 + xbyte;
		case kExact:
		default:
			break;
	}

	return third * 2048 + line * 256 + charrow * 32 + xbyte;
}

int AttributeIndex( int px, int py )
{
	px = std::clamp( px, 0, kScreenW - 1 );
	py = std::clamp( py, 0, kScreenH - 1 );
	return kDisplayBytes + ( py >> 3 ) * kCellsX + ( px >> 3 );
}

bool PixelRevealed( int px, int py, int bytesRevealed, int perturb )
{
	return ScreenIndex( px, py, perturb ) < bytesRevealed;
}

bool AttributeRevealed( int cellX, int cellY, int bytesRevealed )
{
	if( cellX < 0 || cellX >= kCellsX || cellY < 0 || cellY >= kCellsY )
		return false;
	return kDisplayBytes + cellY * kCellsX + cellX < bytesRevealed;
}

std::vector< Address > ScreenOrder()
{
	std::vector< Address > out;
	out.reserve( kDisplayBytes );

	// No arithmetic on py: the loops walk the screen and the row is assembled
	// from where the walk currently is. ScreenIndex() takes a row apart; this
	// puts one together. That is the whole value of having both.
	for( int third = 0; third < 3; ++third )
		for( int line = 0; line < 8; ++line )
			for( int charrow = 0; charrow < 8; ++charrow )
				for( int xbyte = 0; xbyte < kCellsX; ++xbyte )
					out.push_back( Address{ xbyte, third * 64 + charrow * 8 + line } );

	return out;
}

Rgb Colour( int index, bool bright )
{
	const int level = bright ? kBrightLevel : kBasicLevel;
	const int i     = index & 7;
	return Rgb{
		static_cast< uint8_t >( ( ( i >> 1 ) & 1 ) * level ),
		static_cast< uint8_t >( ( ( i >> 2 ) & 1 ) * level ),
		static_cast< uint8_t >( ( i & 1 ) * level ),
	};
}

const char* const* ColourNames()
{
	static const char* const kNames[ kColourCount ] = {
		"Black", "Blue", "Red", "Magenta", "Green", "Cyan", "Yellow", "White"
	};
	return kNames;
}

} // namespace pilot::zx
