#pragma once

#include <algorithm>
#include <cmath>

/**
	Host parameters to physical units.

	Every ranged FFGL parameter this plugin declares is 0..1, because
	`SetParamInfo` clamps an `FF_TYPE_STANDARD` default into 0..1 *before*
	returning and `SetParamRange` can only be called afterwards — so a ranged
	parameter cannot have a ranged default, and the honest way out is to keep
	the host side normalised and put the conversion here, where it can be read
	and tested. Anything that is genuinely a whole number is declared
	`FF_TYPE_OPTION` or `FF_TYPE_INTEGER` instead, which is exempt.
*/
namespace pilot::controls
{
/// Baud is a trim around the machine's own rate, not an absolute: ×0.25 at 0,
/// ×1 at the centre, ×4 at the top. An absolute control would have to span
/// every machine's range and would put each machine's real rate somewhere
/// arbitrary on the slider.
inline float BaudMultiplier( float v )
{
	return std::exp2( ( std::clamp( v, 0.0f, 1.0f ) - 0.5f ) * 4.0f );
}

inline float Baud( float nominal, float v )
{
	return nominal * BaudMultiplier( v );
}

/// Per-block failure probability. Half is already absurd — 27 blocks at 0.5
/// almost never finish — so the top of the control is the top of the useful
/// range rather than the top of the arithmetic.
inline float ErrorRate( float v )
{
	return std::clamp( v, 0.0f, 1.0f ) * 0.5f;
}

/// Border width, as a fraction of the composition taken off each edge. A real
/// Spectrum's border was about an eighth of the picture; a quarter is as far
/// as this goes before the screen is a postage stamp.
inline float BorderInset( float v )
{
	return std::clamp( v, 0.0f, 1.0f ) * 0.25f;
}

/// How much of the Progress range is spent on the pilot tone before the first
/// byte lands.
inline float PilotLength( float v )
{
	return std::clamp( v, 0.0f, 1.0f ) * 0.5f;
}

/// How long the whole tape takes at a given rate, in seconds. This is what
/// drives Clip time sync: the picture really does arrive at the baud rate.
inline double TapeSeconds( double baud, double pilotFraction )
{
	const double dataSeconds = baud > 1.0 ? ( 6912.0 * 8.0 ) / baud : 36.0;
	const double f           = std::clamp( pilotFraction, 0.0, 0.95 );
	return dataSeconds / std::max( 1.0 - f, 0.05 );
}

} // namespace pilot::controls
