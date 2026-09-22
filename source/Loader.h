#pragma once

#include <cstdint>

/**
	The tape, as a pure function.

	Everything about *where the load has got to* is computed here, on the CPU,
	once per frame, and handed to the shader as a byte count and a phase. That
	is not an optimisation — it is what lets almost every check in `pttest` run
	with no GL context at all, so the claims about the address order, the border
	period and the error rate cannot depend on a rasteriser or on a raster.

	Nothing in here keeps state between frames. A given (progress, settings)
	always gives the same answer, so a re-render of the same composition is the
	same picture and a check can step the model without a plugin instance.
*/
namespace pilot::load
{
/// PCG output-mixed integer hash. Integer throughout on purpose: this is the
/// only randomness in the plugin, and `fract( sin( x ) )` would make the error
/// rate a property of somebody's libm.
uint32_t HashInt( uint32_t x );

/// Three values mixed into one hash, so a draw is identified by what it is
/// rather than by a counter somebody has to keep.
uint32_t Hash3( uint32_t a, uint32_t b, uint32_t c );

/// The draw itself, in [0, 1).
float Hash01( uint32_t x );

/**
	Does the block at `block` of attempt `attempt` fail?

	A Bernoulli draw at probability `rate`, seeded by (seed, attempt, block) and
	nothing else — so the same tape always fails in the same places, and the
	number of blocks that pass before the first failure is geometric with mean
	1/rate. `pttest --error` measures exactly that.
*/
bool BlockFails( uint32_t seed, int attempt, int block, float rate );

struct Settings
{
	float progress    = 0.0f;///< the Progress control, 0..1
	float pilotLength = 0.0f;///< fraction of the progress range spent on pilot tone
	float errorRate   = 0.0f;///< per-block failure probability
	uint32_t seed     = 1u;
};

struct State
{
	bool pilot          = true; ///< still in the pilot tone: no bytes have arrived
	float tapeProgress  = 0.0f; ///< 0..1 through the tape, after the pilot is taken out
	int bytesRevealed   = 0;    ///< 0..6912
	int attempt         = 0;    ///< how many times the load has restarted
	int blocksLoaded    = 0;    ///< whole blocks held in the current attempt
	float sinceFailure  = -1.0f;///< blocks since the last failure; negative if none
	bool message        = false;///< is the loading-error message showing
};

/// How long the error message holds, in blocks of tape.
constexpr float kMessageHold = 1.0f;

State Evaluate( const Settings& settings );

//---------------------------------------------------------------------------
// The border.
//
// The border is the loading signal itself. A byte on tape is eight bits, a bit
// is two half-cycles of the audio, and the ULA is told the current half-cycle's
// state — so the border colour toggles at twice the baud rate. The beam paints
// the border as it scans, which is why the toggles land as horizontal bands
// rather than as a whole-screen flicker: one frame of scan covers
// baud / frameHz bits, and that is how many stripe pairs fit down the picture.
//
// Time enters as a bit position and nothing else. Resolume's clock is
// milliseconds since the composition opened and overflows a float at about
// 499 million ms, where a float resolves only ~0.03 s; the arithmetic here is
// double throughout and what reaches the shader is reduced to [0, 2), so the
// shader never sees a large number at all.
//---------------------------------------------------------------------------
struct Border
{
	double bitPosTop = 0.0; ///< continuous bit position at the top of the frame
	double bitsPerFrame = 0.0;///< bits covered by one frame's scan
};

/// Where the tape head is, in bits, at the top of the frame.
Border BorderAt( double seconds, double baud, double frameHz );

/// The continuous half-cycle count at row fraction `f` (0 = top, 1 = bottom).
double HalfCycleContinuous( const Border& b, double f );

/// ...and the whole half-cycle the border is showing there.
long long HalfCycle( const Border& b, double f );

/// Which byte the tape head is on. Used by the whole-border flash style, which
/// changes once per byte and has no row dependence at all.
long long ByteIndex( const Border& b );

/// The flashing style's colour index, 0..7, for a given byte.
int FlashColour( long long byteIndex );

} // namespace pilot::load
