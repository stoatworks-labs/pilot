#include "Loader.h"

#include "Spectrum.h"

#include <algorithm>
#include <cmath>

namespace pilot::load
{
uint32_t HashInt( uint32_t x )
{
	// PCG output mix. Exact in 32 bits, and the same arithmetic the rest of the
	// fleet uses so an answer here is an answer anywhere.
	x          = x * 747796405u + 2891336453u;
	uint32_t w = ( ( x >> ( ( x >> 28u ) + 4u ) ) ^ x ) * 277803737u;
	return ( w >> 22u ) ^ w;
}

uint32_t Hash3( uint32_t a, uint32_t b, uint32_t c )
{
	// Three separate mixes rather than one over a packed word: packing would
	// cap the block index at whatever field it was given, and the error check
	// draws hundreds of thousands of blocks.
	return HashInt( a ^ HashInt( b * 2654435761u ^ HashInt( c * 40503u + 1u ) ) );
}

float Hash01( uint32_t x )
{
	return static_cast< float >( x ) * ( 1.0f / 4294967296.0f );
}

bool BlockFails( uint32_t seed, int attempt, int block, float rate )
{
	if( rate <= 0.0f )
		return false;
	if( rate >= 1.0f )
		return true;

	return Hash01( Hash3( seed, static_cast< uint32_t >( attempt ), static_cast< uint32_t >( block ) ) ) < rate;
}

State Evaluate( const Settings& settings )
{
	State out;

	const float p     = std::clamp( settings.progress, 0.0f, 1.0f );
	const float pilot = std::clamp( settings.pilotLength, 0.0f, 0.95f );

	// The pilot tone happens BEFORE any byte arrives, which is what it is: a
	// stretch of border with nothing on the screen yet. So the first `pilot` of
	// the Progress range is spent on it, and the rest maps onto the tape.
	out.pilot = p < pilot;
	out.tapeProgress = pilot >= 1.0f ? 0.0f : std::clamp( ( p - pilot ) / ( 1.0f - pilot ), 0.0f, 1.0f );

	// Tape position in blocks. This is what the head has PASSED; what is
	// actually held depends on how many times the load has restarted.
	const double q     = static_cast< double >( out.tapeProgress ) * zx::kBlocks;
	const int whole    = std::min( static_cast< int >( std::floor( q ) ), zx::kBlocks );
	const double frac  = q - std::floor( q );

	int attempt     = 0;
	int inAttempt   = 0;
	double lastFail = -1.0;
	int heldAtFail  = 0;

	// A block's fate is decided when it completes, which is at tape position
	// k+1. Twenty-seven iterations, once per frame; there is nothing to
	// optimise here and a loop reads as what it is.
	for( int k = 0; k < whole; ++k )
	{
		if( BlockFails( settings.seed, attempt, inAttempt, settings.errorRate ) )
		{
			heldAtFail = inAttempt;
			++attempt;
			inAttempt = 0;
			lastFail  = static_cast< double >( k ) + 1.0;
		}
		else
		{
			++inAttempt;
		}
	}

	out.attempt      = attempt;
	out.blocksLoaded = inAttempt;

	const double loaded = static_cast< double >( inAttempt ) + ( whole < zx::kBlocks ? frac : 0.0 );
	out.bytesRevealed   = std::clamp( static_cast< int >( std::floor( loaded * zx::kBlockBytes ) ), 0, zx::kTotalBytes );

	if( lastFail >= 0.0 )
	{
		out.sinceFailure = static_cast< float >( q - lastFail );
		out.message      = out.sinceFailure < kMessageHold;

		// A real Spectrum does not clear the screen when a load fails: it
		// prints the report over whatever had arrived and stops. So while the
		// message is up the byte count HOLDS at what the failed attempt had
		// reached, and only when the message goes does the retry's count take
		// over. Without this the picture vanishes the instant the error
		// appears, which is both wrong and -- since the message is then always
		// over a blank screen -- much less useful.
		if( out.message )
			out.bytesRevealed = std::clamp( heldAtFail * zx::kBlockBytes, 0, zx::kTotalBytes );
	}

	// Nothing has loaded and nothing has failed: still in the pilot tone, so
	// there is no picture to show yet.
	if( out.pilot )
	{
		out.bytesRevealed = 0;
		out.message       = false;
		out.sinceFailure  = -1.0f;
		out.attempt       = 0;
		out.blocksLoaded  = 0;
	}

	return out;
}

//---------------------------------------------------------------------------
Border BorderAt( double seconds, double baud, double frameHz )
{
	Border b;
	b.bitPosTop   = seconds * baud;
	b.bitsPerFrame = frameHz > 0.0 ? baud / frameHz : 0.0;
	return b;
}

double HalfCycleContinuous( const Border& b, double f )
{
	// Two half-cycles to a bit. `f` runs 0 at the top of the picture to 1 at
	// the bottom, because that is the order the beam paints it in.
	return ( b.bitPosTop + f * b.bitsPerFrame ) * 2.0;
}

long long HalfCycle( const Border& b, double f )
{
	return static_cast< long long >( std::floor( HalfCycleContinuous( b, f ) ) );
}

long long ByteIndex( const Border& b )
{
	return static_cast< long long >( std::floor( b.bitPosTop / 8.0 ) );
}

int FlashColour( long long byteIndex )
{
	// Never black: a border that goes black once every eight bytes reads as a
	// dropout rather than as a flash.
	return 1 + static_cast< int >( HashInt( static_cast< uint32_t >( byteIndex ) ) % 7u );
}

} // namespace pilot::load
