#include "Pilot.h"

//The SDK's umbrella FFGLSDK.h pulls in every other scoped binding but leaves
//this one out (SDK b1afaf9), so it has to be reached for by hand. The symptom
//is an unknown-type error on ScopedFBOBinding and nothing else.
#include <ffglex/FFGLScopedFBOBinding.h>

#include "Controls.h"
#include "Diag.h"
#include "Font.h"
#include "Machines.h"
#include "Shaders.h"
#include "Spectrum.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace ffglex;
using namespace pilot;

//---------------------------------------------------------------------------
// The name is `SW Pilot`, with the prefix every released plugin in this fleet
// carries. The FFGL name field is char[16] and is NOT null-terminated, so a
// longer name is truncated by the host without a word; this is eight
// characters.
//---------------------------------------------------------------------------
static CFFGLPluginInfo PluginInfo(
	PluginFactory< Pilot >,// Create method
	"PT01",                // Plugin unique ID of maximum length 4.
	"SW Pilot",            // Plugin name
	2,                     // API major version number
	1,                     // API minor version number
	0,                     // Plugin major version number
	1,                     // Plugin minor version number
	FF_EFFECT,             // Plugin type
	"A tape loader. The clip arrives the way a ZX Spectrum loaded it: in screen-memory order, at the baud rate, with the border painted by the loading signal itself.\n\nThe Spectrum's display file is not linear, so the picture does not wipe down the screen - it arrives in three thirds, each filling in an eight-line interleave. Colour comes last, in one block of attributes, so the image lands in monochrome and colours in at the very end.\n\nA transition, in practice. Drive Progress by hand, by clip time, or off the beat.",// Plugin description
	"pilot FFGL effect"    // About
);

namespace
{
/// The loading-error report a Spectrum gives when a block will not read. The
/// text is the machine's own; the font is a 5x7 bitmap, so the message is an
/// exact arrangement of whole Spectrum pixels at every output size.
constexpr const char* kErrorMessage = "R Tape loading error, 0:1";

/// Where the message sits, in Spectrum pixels with y from the top. The bottom
/// character row, inset a few pixels from the left, which is roughly where a
/// Spectrum puts its reports.
constexpr int kMessageX = 4;
constexpr int kMessageY = 184;

int messageWidth()
{
	return static_cast< int >( std::strlen( kErrorMessage ) ) * pilot::font::kAdvance;
}

/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour. A logging call must never be the
/// thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

/// Four beats to the bar, which is what every host here means by a bar.
constexpr double kBeatsPerBar = 4.0;
} // namespace

Pilot::Pilot() :
	startTime( std::chrono::steady_clock::now() )
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The border is a signal and the sync modes are a transport, so the effect
	//needs the host's clock. Asking for it also means a re-render of the same
	//composition produces the same frame rather than whatever the wall clock
	//happened to say.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults: a tape already part way through, with the border running.
	//
	// An effect that does nothing until four sliders are moved is an effect
	// nobody finds out is any good, so the first frame is a recognisable
	// half-loaded Spectrum screen rather than the clip untouched. Error Rate
	// defaults to nothing, because a machine that is failing before anyone has
	// touched a slider is a machine nobody trusts.
	//---------------------------------------------------------------------
	params[ PT_TYPE ]         = 0.0f;//ZX 48
	params[ PT_BAUD ]         = 0.5f;//the machine's own rate
	params[ PT_INK ]          = 0.0f;//Black
	params[ PT_PAPER ]        = 7.0f;//White — a Spectrum powers up black on white
	params[ PT_BRIGHT ]       = 1.0f;//Auto

	params[ PT_PROGRESS ]     = 0.45f;
	params[ PT_SYNC ]         = 0.0f;//Manual
	params[ PT_ERROR_RATE ]   = 0.0f;
	params[ PT_MESSAGE ]      = 1.0f;

	params[ PT_BORDER_ON ]    = 1.0f;
	params[ PT_BORDER_WIDTH ] = 0.32f;//~8% of the picture off each edge
	params[ PT_PILOT_LENGTH ] = 0.16f;//~8% of the Progress range

	params[ PT_MIX ]          = 1.0f;
	params[ PT_BACKGROUND ]   = 0.0f;//Paper

	//---------------------------------------------------------------------
	// Declaration. Grouped the way Resolume shows them: what the machine is,
	// where the load has got to, what the border is doing, and how much of it
	// is in the programme.
	//---------------------------------------------------------------------
	SetOptionParamInfo( PT_TYPE, "Type", machineCount(), params[ PT_TYPE ] );
	for( int i = 0; i < machineCount(); ++i )
		SetParamElementInfo( PT_TYPE, i, machine( i ).name, static_cast< float >( i ) );

	SetParamInfof( PT_BAUD, "Baud", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_INK, "Ink", zx::kColourCount, params[ PT_INK ] );
	SetOptionParamInfo( PT_PAPER, "Paper", zx::kColourCount, params[ PT_PAPER ] );
	for( int i = 0; i < zx::kColourCount; ++i )
	{
		SetParamElementInfo( PT_INK, i, zx::ColourNames()[ i ], static_cast< float >( i ) );
		SetParamElementInfo( PT_PAPER, i, zx::ColourNames()[ i ], static_cast< float >( i ) );
	}

	//Off / Auto / On rather than a slider: BRIGHT is one bit of hardware, and
	//the only interesting third option is "let the picture decide".
	SetOptionParamInfo( PT_BRIGHT, "Bright", 3, params[ PT_BRIGHT ] );
	SetParamElementInfo( PT_BRIGHT, 0, "Off", 0.0f );
	SetParamElementInfo( PT_BRIGHT, 1, "Auto", 1.0f );
	SetParamElementInfo( PT_BRIGHT, 2, "On", 2.0f );

	SetParamInfof( PT_PROGRESS, "Progress", FF_TYPE_STANDARD );

	//Position is the meaning of this list, so it is not sorted: Manual, then
	//the three clocks in order of how long they take.
	SetOptionParamInfo( PT_SYNC, "Sync", 4, params[ PT_SYNC ] );
	SetParamElementInfo( PT_SYNC, kSyncManual, "Manual", float( kSyncManual ) );
	SetParamElementInfo( PT_SYNC, kSyncClip, "Clip time", float( kSyncClip ) );
	SetParamElementInfo( PT_SYNC, kSyncBeat, "Beat", float( kSyncBeat ) );
	SetParamElementInfo( PT_SYNC, kSyncBar, "Bar", float( kSyncBar ) );

	SetParamInfof( PT_ERROR_RATE, "Error Rate", FF_TYPE_STANDARD );
	SetParamInfo( PT_MESSAGE, "Message On", FF_TYPE_BOOLEAN, params[ PT_MESSAGE ] > 0.5f );

	SetParamInfo( PT_BORDER_ON, "Border On", FF_TYPE_BOOLEAN, params[ PT_BORDER_ON ] > 0.5f );
	SetParamInfof( PT_BORDER_WIDTH, "Border Width", FF_TYPE_STANDARD );
	SetParamInfof( PT_PILOT_LENGTH, "Pilot Length", FF_TYPE_STANDARD );

	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_BACKGROUND, "Background", 3, params[ PT_BACKGROUND ] );
	SetParamElementInfo( PT_BACKGROUND, kBackgroundPaper, "Paper", float( kBackgroundPaper ) );
	SetParamElementInfo( PT_BACKGROUND, kBackgroundBlack, "Black", float( kBackgroundBlack ) );
	SetParamElementInfo( PT_BACKGROUND, kBackgroundClip, "Clip", float( kBackgroundClip ) );

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}

	for( FFUInt32 i = PT_TYPE; i <= PT_BRIGHT; ++i )
		SetParamGroup( i, "Machine" );
	for( FFUInt32 i = PT_PROGRESS; i <= PT_MESSAGE; ++i )
		SetParamGroup( i, "Load" );
	for( FFUInt32 i = PT_BORDER_ON; i <= PT_PILOT_LENGTH; ++i )
		SetParamGroup( i, "Border" );
	for( FFUInt32 i = PT_MIX; i <= PT_BACKGROUND; ++i )
		SetParamGroup( i, "Output" );

	FFGLLog::LogToHost( "Created pilot effect" );

	diag::init();
}

//---------------------------------------------------------------------------
// The clock.
//---------------------------------------------------------------------------
/// Frames that must agree before the host's clock unit is settled.
static constexpr int kClockVotes = 4;

FFResult Pilot::SetTime( double time )
{
	hostTimeSeen = true;
	return CFFGLPlugin::SetTime( time );
}

void Pilot::ForceSecondsClock()
{
	clockScale = 1.0;
}

double Pilot::elapsedSeconds()
{
	// FFGL never says what unit SetTime arrives in, and hosts disagree:
	// Resolume sends MILLISECONDS (its own SDK's Particles sample divides by
	// 1000), while this repo's harness sends seconds. So measure rather than
	// assume: steady_clock says how much real time passed, the host says how
	// much host time passed, and the ratio names the unit outright. ~1 is a
	// seconds host, ~1000 a milliseconds one, and nothing plausible sits
	// between, so both bands are wide and a frame that fits neither does not
	// vote.
	const double wallNow =
	    std::chrono::duration< double >( std::chrono::steady_clock::now() - startTime ).count();

	if( !hostTimeSeen )
		return wallNow;

	const double raw = hostTime;

	if( clockScale == 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		//A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			//Several frames rather than one, so a single odd frame -- the first
			//after a seek, say -- cannot decide it on its own.
			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
			{
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
				diag::info( std::string( "host clock is " )
				            + ( clockScale == 0.001 ? "milliseconds" : "seconds" ) );
			}
		}
	}

	lastRawTime  = raw;
	lastWallTime = wallNow;

	//Until the unit is settled, run on the real clock rather than assume one:
	//wrong in origin but right in rate, where assuming seconds would be a
	//thousand times fast on Resolume.
	return clockScale != 0.0 ? raw * clockScale : wallNow;
}

float Pilot::effectiveProgress( double seconds ) const
{
	const float manual = std::clamp( params[ PT_PROGRESS ], 0.0f, 1.0f );
	const int mode     = static_cast< int >( std::lround( params[ PT_SYNC ] ) );

	if( mode == kSyncManual )
		return manual;

	//Progress stays live in every mode: it is the offset the clock is added to,
	//so an operator can still place the load where they want it inside the bar.
	double turns = 0.0;

	if( mode == kSyncClip )
	{
		const MachineSpec& spec = machine( static_cast< int >( std::lround( params[ PT_TYPE ] ) ) );
		const double baud       = controls::Baud( spec.nominalBaud, params[ PT_BAUD ] );
		const double period     = controls::TapeSeconds( baud, controls::PilotLength( params[ PT_PILOT_LENGTH ] ) );
		turns                   = seconds / std::max( period, 0.001 );
	}
	else
	{
		//The host gives a tempo and a position within the current bar, never
		//which bar it is. Recover a continuous count without keeping state: the
		//clock estimates how many bars have passed, barPhase is the exact
		//position inside this one, and the whole number reconciling them is
		//round( estimate - barPhase ). Continuous across the bar line, because
		//as barPhase wraps from 1 to 0 the rounded integer steps up at the same
		//instant. The same recovery the rest of the fleet uses.
		const double tempo      = bpm > 1.0f ? static_cast< double >( bpm ) : 120.0;
		const double barSeconds = ( 60.0 * kBeatsPerBar ) / tempo;
		const double estimate   = seconds / barSeconds;
		const double within     = std::clamp( static_cast< double >( barPhase ), 0.0, 1.0 );
		const double bars       = within + std::round( estimate - within );

		turns = mode == kSyncBeat ? bars * kBeatsPerBar : bars;
	}

	//Wrapped with floor, not a cast: a cast truncates toward zero, so a host
	//reporting a negative transport position -- which is a scrub backwards, and
	//operators do that constantly -- would run the load the wrong way.
	const double raw = static_cast< double >( manual ) + turns;
	return static_cast< float >( raw - std::floor( raw ) );
}

pilot::load::State Pilot::TapeStateForTest()
{
	load::Settings settings;
	settings.progress    = effectiveProgress( elapsedSeconds() );
	settings.pilotLength = controls::PilotLength( params[ PT_PILOT_LENGTH ] );
	settings.errorRate   = controls::ErrorRate( params[ PT_ERROR_RATE ] );
	return load::Evaluate( settings );
}

float Pilot::EffectiveProgressForTest()
{
	return effectiveProgress( elapsedSeconds() );
}

pilot::load::Border Pilot::BorderForTest()
{
	const MachineSpec& spec = machine( static_cast< int >( std::lround( params[ PT_TYPE ] ) ) );
	const double baud       = controls::Baud( spec.nominalBaud, params[ PT_BAUD ] );
	return load::BorderAt( elapsedSeconds(), baud, spec.frameHz );
}

void Pilot::MessageBoxForTest( int& x, int& y, int& w, int& h )
{
	x = kMessageX;
	y = kMessageY;
	w = messageWidth();
	h = font::kHeight;
}

//---------------------------------------------------------------------------
bool Pilot::compileShaders()
{
	struct Stage
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	};

	const Stage stages[] = {
		{ &rasterShader, shaders::kRasterFragment, "raster" },
		{ &attrShader, shaders::kAttrFragment, "attr" },
		{ &composeShader, shaders::kComposeFragment, "compose" },
	};

	for( const Stage& stage : stages )
	{
		if( !stage.shader->Compile( shaders::kVertex, stage.fragment ) )
		{
			//Returning FF_FAIL from InitGL is invisible to the operator: the
			//effect simply does nothing in Resolume, with no message anywhere.
			//This line is the only record of which stage it was.
			diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
			FFGLLog::LogToHost( "pilot: shader failed to compile" );
			return false;
		}
	}

	return true;
}

bool Pilot::buildMessageTexture()
{
	const int w = messageWidth();
	const int h = font::kHeight;

	//One byte per pixel, row 0 the TOP row of the glyphs. The compose pass
	//fetches it with the Spectrum's own top-down y, so uploading it this way up
	//is what keeps the flip count at the two already documented in the shader.
	std::vector< unsigned char > bitmap( static_cast< size_t >( w ) * h, 0 );
	for( int c = 0; kErrorMessage[ c ] != '\0'; ++c )
	{
		const int code = static_cast< unsigned char >( kErrorMessage[ c ] );
		for( int y = 0; y < font::kHeight; ++y )
			for( int x = 0; x < font::kWidth; ++x )
				if( font::Bit( code, x, y ) )
					bitmap[ static_cast< size_t >( y ) * w + c * font::kAdvance + x ] = 255;
	}

	glGenTextures( 1, &messageTexture );
	if( messageTexture == 0 )
		return false;

	glBindTexture( GL_TEXTURE_2D, messageTexture );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, bitmap.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return true;
}

FFResult Pilot::InitGL( const FFGLViewportStruct* vp )
{
	//The GL strings first, and unconditionally. When a shader will not compile
	//it is almost always the driver or the GL version, and knowing which machine
	//reported what is the whole diagnosis.
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	if( !compileShaders() )
	{
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !buildMessageTexture() )
	{
		diag::error( "could not build the message texture" );
		DeInitGL();
		return FF_FAIL;
	}

	diag::info( "initialised" );

	//Use the base class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

FFResult Pilot::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& input = *pGL->inputTextures[ 0 ];

	//The host's viewport, not the one InitGL was handed: Resolume changes
	//composition resolution without reinitialising the plugin. Captured up
	//front and restored before the composite, because ScopedFBOBinding restores
	//the framebuffer and ONLY the framebuffer -- every pass's ResizeViewPort()
	//otherwise leaks into the pass after it.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );
	const float outputWidth  = std::max( 1.0f, static_cast< float >( hostViewport[ 2 ] ) );
	const float outputHeight = std::max( 1.0f, static_cast< float >( hostViewport[ 3 ] ) );

	//Allocate BEFORE anything is bound. FFGLFBO::Initialise sizes its colour
	//texture under a scoped binding, and every ffglex::Scoped* clears its
	//binding to 0 on exit rather than restoring it -- so allocating a buffer
	//silently unbinds the input texture from the active unit, and the frame
	//that allocates is the only one that comes out wrong.
	if( !rasterBuffer.Ensure( zx::kScreenW, zx::kScreenH, GL_RGBA8 )
	    || !attrBuffer.Ensure( zx::kCellsX, zx::kCellsY, GL_RGBA16F ) )
	{
		diag::error( "could not allocate the pass buffers" );
		return FF_FAIL;
	}

	const int typeIndex     = static_cast< int >( std::lround( params[ PT_TYPE ] ) );
	const MachineSpec& spec = machine( typeIndex );
	const double baud       = controls::Baud( spec.nominalBaud, params[ PT_BAUD ] );
	const double seconds    = elapsedSeconds();

	load::Settings settings;
	settings.progress    = effectiveProgress( seconds );
	settings.pilotLength = controls::PilotLength( params[ PT_PILOT_LENGTH ] );
	settings.errorRate   = controls::ErrorRate( params[ PT_ERROR_RATE ] );
	const load::State tape = load::Evaluate( settings );

	//------------------------------------------------------------------
	// 1. The clip, down onto 256x192.
	//------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( rasterBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		rasterBuffer.ResizeViewPort();
		ScopedShaderBinding shader( rasterShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( input.Handle );

		const FFGLTexCoords maxCoords = GetMaxGLTexCoords( input );
		rasterShader.Set( "InputTexture", 0 );
		rasterShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		rasterShader.Set( "InputSize", static_cast< float >( input.Width ), static_cast< float >( input.Height ) );
		rasterShader.Set( "TargetSize", float( zx::kScreenW ), float( zx::kScreenH ) );
		quad.Draw();
	}

	//------------------------------------------------------------------
	// 2. One texel per character cell: its two colours and its threshold.
	//------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( attrBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		attrBuffer.ResizeViewPort();
		ScopedShaderBinding shader( attrShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( rasterBuffer.GetTextureInfo().Handle );

		attrShader.Set( "RasterTexture", 0 );
		attrShader.Set( "MaxUV", 1.0f, 1.0f );
		attrShader.Set( "BrightMode", static_cast< int >( std::lround( params[ PT_BRIGHT ] ) ) );
		quad.Draw();
	}

	//------------------------------------------------------------------
	// 3. The output.
	//------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( composeShader.GetGLID() );

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, rasterBuffer.GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, attrBuffer.GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE2 );
		glBindTexture( GL_TEXTURE_2D, input.Handle );
		glActiveTexture( GL_TEXTURE3 );
		glBindTexture( GL_TEXTURE_2D, messageTexture );
		glActiveTexture( GL_TEXTURE0 );

		const FFGLTexCoords maxCoords = GetMaxGLTexCoords( input );
		composeShader.Set( "RasterTexture", 0 );
		composeShader.Set( "AttrTexture", 1 );
		composeShader.Set( "InputTexture", 2 );
		composeShader.Set( "MessageTexture", 3 );
		composeShader.Set( "MaxUV", 1.0f, 1.0f );
		composeShader.Set( "InputMaxUV", maxCoords.s, maxCoords.t );

		//Border Off is not a colour, it is the absence of a border: the screen
		//fills the composition and there is no region left to paint.
		const float inset = params[ PT_BORDER_ON ] > 0.5f
		                        ? controls::BorderInset( params[ PT_BORDER_WIDTH ] )
		                        : 0.0f;
		composeShader.Set( "Inset", inset, inset );

		composeShader.Set( "BytesRevealed", tape.bytesRevealed );

		const bool defaultBright = static_cast< int >( std::lround( params[ PT_BRIGHT ] ) ) == 2;
		const zx::Rgb ink        = zx::Colour( static_cast< int >( std::lround( params[ PT_INK ] ) ), defaultBright );
		const zx::Rgb paper      = zx::Colour( static_cast< int >( std::lround( params[ PT_PAPER ] ) ), defaultBright );
		composeShader.Set( "DefaultInk", ink.r / 255.0f, ink.g / 255.0f, ink.b / 255.0f );
		composeShader.Set( "DefaultPaper", paper.r / 255.0f, paper.g / 255.0f, paper.b / 255.0f );

		//------------------------------------------------------------------
		// The border.
		//
		// The phase is reduced to [0, 2) HERE, in double, before anything
		// reaches the shader: the parity of a half-cycle count survives being
		// taken modulo two, and what the shader then sees is a small number
		// whatever the host's clock says. Handing it `seconds * baud * 2` would
		// hand it 1.5e9 after a day of uptime, where a float's step is 128.
		//------------------------------------------------------------------
		const load::Border border = load::BorderAt( seconds, baud, spec.frameHz );
		const double halfTop      = load::HalfCycleContinuous( border, 0.0 );
		double reduced            = std::fmod( halfTop, 2.0 );
		if( reduced < 0.0 )
			reduced += 2.0;

		composeShader.Set( "BorderStyle", spec.border == kBorderFlash ? 1 : 0 );
		composeShader.Set( "BorderHalfPhase", static_cast< float >( reduced ) );
		composeShader.Set( "BorderHalfSpan", static_cast< float >( border.bitsPerFrame * 2.0 ) );

		int colourA = tape.pilot ? spec.pilotA : spec.dataA;
		int colourB = tape.pilot ? spec.pilotB : spec.dataB;
		if( spec.border == kBorderFlash )
		{
			//One colour for the whole border, changed once per byte. There is
			//no row dependence at all, which is what makes it read as a flicker
			//rather than as stripes.
			colourA = load::FlashColour( load::ByteIndex( border ) );
			colourB = colourA;
		}
		const zx::Rgb a = zx::Colour( colourA, spec.bright );
		const zx::Rgb b = zx::Colour( colourB, spec.bright );
		composeShader.Set( "BorderA", a.r / 255.0f, a.g / 255.0f, a.b / 255.0f );
		composeShader.Set( "BorderB", b.r / 255.0f, b.g / 255.0f, b.b / 255.0f );

		const bool showMessage = params[ PT_MESSAGE ] > 0.5f && tape.message;
		composeShader.Set( "MessageOn", showMessage ? 1 : 0 );
		composeShader.Set( "MessageOrigin", float( kMessageX ), float( kMessageY ) );
		composeShader.Set( "MessageSize", float( messageWidth() ), float( font::kHeight ) );

		composeShader.Set( "Background", static_cast< int >( std::lround( params[ PT_BACKGROUND ] ) ) );
		composeShader.Set( "Mix", std::clamp( params[ PT_MIX ], 0.0f, 1.0f ) );

		quad.Draw();

		for( GLenum unit : { GL_TEXTURE3, GL_TEXTURE2, GL_TEXTURE1, GL_TEXTURE0 } )
		{
			glActiveTexture( unit );
			glBindTexture( GL_TEXTURE_2D, 0 );
		}
		glActiveTexture( GL_TEXTURE0 );
	}

	return FF_SUCCESS;
}

void Pilot::releaseBuffers()
{
	rasterBuffer.Destroy();
	attrBuffer.Destroy();
}

FFResult Pilot::DeInitGL()
{
	rasterShader.FreeGLResources();
	attrShader.FreeGLResources();
	composeShader.FreeGLResources();
	quad.Release();
	releaseBuffers();

	if( messageTexture != 0 )
	{
		glDeleteTextures( 1, &messageTexture );
		messageTexture = 0;
	}

	return FF_SUCCESS;
}

FFResult Pilot::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// An About button is a press, not a value to keep: it opens a browser and
	// nothing about the effect changes.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Pilot::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

FFResult Pilot::SetTextParameter( unsigned int index, const char* )
{
	// The About text line is display-only, but the SDK's FF_INSTANTIATE_GL
	// pushes every declared default into a fresh instance -- including this one,
	// through here -- and destroys the instance on the first FF_FAIL. The base
	// SetTextParameter returns FF_FAIL, so without this the plugin silently
	// fails to load in Resolume while every in-repo harness still passes.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, nullptr );
}

char* Pilot::GetTextParameter( unsigned int index )
{
	// The host is handed a bare pointer, so the string is kept as a member
	// rather than built on the stack here.
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}
