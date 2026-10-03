#include "Frame.h"

#include "Controls.h"
#include "Font.h"
#include "Spectrum.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pilot::frame
{
namespace
{
int Index( float v )
{
	return static_cast< int >( std::lround( v ) );
}

void Store( float out[ 3 ], const zx::Rgb& c )
{
	out[ 0 ] = c.r / 255.0f;
	out[ 1 ] = c.g / 255.0f;
	out[ 2 ] = c.b / 255.0f;
}
} // namespace

const MachineSpec& Machine( const HostValues& host )
{
	return machine( Index( host.type ) );
}

double Baud( const HostValues& host )
{
	return controls::Baud( Machine( host ).nominalBaud, host.baud );
}

float Wrap( double raw )
{
	return static_cast< float >( raw - std::floor( raw ) );
}

float ClipTimeProgress( const HostValues& host, double seconds )
{
	const float manual  = std::clamp( host.progress, 0.0f, 1.0f );
	const double period = controls::TapeSeconds( Baud( host ), controls::PilotLength( host.pilotLength ) );
	const double turns  = seconds / std::max( period, 0.001 );
	return Wrap( static_cast< double >( manual ) + turns );
}

load::State Tape( const HostValues& host, float progress )
{
	load::Settings settings;
	settings.progress    = progress;
	settings.pilotLength = controls::PilotLength( host.pilotLength );
	settings.errorRate   = controls::ErrorRate( host.errorRate );
	return load::Evaluate( settings );
}

load::Border Border( const HostValues& host, double seconds )
{
	return load::BorderAt( seconds, Baud( host ), Machine( host ).frameHz );
}

Uniforms Prepare( const HostValues& host, float progress, double seconds )
{
	Uniforms u;

	const MachineSpec& spec = Machine( host );
	const load::State tape  = Tape( host, progress );

	u.brightMode = Index( host.bright );

	//Border Off is not a colour, it is the absence of a border: the screen
	//fills the composition and there is no region left to paint.
	u.inset = host.borderOn > 0.5f ? controls::BorderInset( host.borderWidth ) : 0.0f;

	u.bytesRevealed = tape.bytesRevealed;

	const bool defaultBright = u.brightMode == 2;
	Store( u.defaultInk, zx::Colour( Index( host.ink ), defaultBright ) );
	Store( u.defaultPaper, zx::Colour( Index( host.paper ), defaultBright ) );

	//-----------------------------------------------------------------------
	// The border.
	//
	// The phase is reduced to [0, 2) HERE, in double, before anything reaches
	// the shader: the parity of a half-cycle count survives being taken modulo
	// two, and what the shader then sees is a small number whatever the host's
	// clock says. Handing it `seconds * baud * 2` would hand it 1.5e9 after a
	// day of uptime, where a float's step is 128.
	//-----------------------------------------------------------------------
	const load::Border border = Border( host, seconds );
	const double halfTop      = load::HalfCycleContinuous( border, 0.0 );
	double reduced            = std::fmod( halfTop, 2.0 );
	if( reduced < 0.0 )
		reduced += 2.0;

	u.borderStyle     = spec.border == kBorderFlash ? 1 : 0;
	u.borderHalfPhase = static_cast< float >( reduced );
	u.borderHalfSpan  = static_cast< float >( border.bitsPerFrame * 2.0 );

	int colourA = tape.pilot ? spec.pilotA : spec.dataA;
	int colourB = tape.pilot ? spec.pilotB : spec.dataB;
	if( spec.border == kBorderFlash )
	{
		//One colour for the whole border, changed once per byte. There is no
		//row dependence at all, which is what makes it read as a flicker rather
		//than as stripes.
		colourA = load::FlashColour( load::ByteIndex( border ) );
		colourB = colourA;
	}
	Store( u.borderA, zx::Colour( colourA, spec.bright ) );
	Store( u.borderB, zx::Colour( colourB, spec.bright ) );

	u.messageOn  = host.message > 0.5f && tape.message;
	u.background = Index( host.background );
	u.mix        = std::clamp( host.mix, 0.0f, 1.0f );

	return u;
}

int MessageWidth()
{
	return static_cast< int >( std::strlen( kErrorMessage ) ) * font::kAdvance;
}

bool MessageBit( int x, int y )
{
	if( x < 0 || y < 0 || x >= MessageWidth() || y >= font::kHeight )
		return false;

	//The blank column after each glyph is the advance, not part of the glyph.
	const int column = x % font::kAdvance;
	if( column >= font::kWidth )
		return false;

	const int code = static_cast< unsigned char >( kErrorMessage[ x / font::kAdvance ] );
	return font::Bit( code, column, y );
}

} // namespace pilot::frame
