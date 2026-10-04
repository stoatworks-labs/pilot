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
	2,                     // Plugin minor version number
	FF_EFFECT,             // Plugin type
	"A tape loader. The clip arrives the way a ZX Spectrum loaded it: in screen-memory order, at the baud rate, with the border painted by the loading signal itself.\n\nThe Spectrum's display file is not linear, so the picture does not wipe down the screen - it arrives in three thirds, each filling in an eight-line interleave. Colour comes last, in one block of attributes, so the image lands in monochrome and colours in at the very end.\n\nA transition, in practice. Drive Progress by hand, by clip time, or off the beat.",// Plugin description
	"pilot FFGL effect"    // About
);

namespace
{
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
	// They live in Frame.h, where the reasons are, because the OpenFX build
	// declares its parameters from the same struct.
	//---------------------------------------------------------------------
	const frame::HostValues defaults;
	params[ PT_TYPE ]         = defaults.type;
	params[ PT_BAUD ]         = defaults.baud;
	params[ PT_INK ]          = defaults.ink;
	params[ PT_PAPER ]        = defaults.paper;
	params[ PT_BRIGHT ]       = defaults.bright;

	params[ PT_PROGRESS ]     = defaults.progress;
	params[ PT_SYNC ]         = defaults.sync;
	params[ PT_ERROR_RATE ]   = defaults.errorRate;
	params[ PT_MESSAGE ]      = defaults.message;

	params[ PT_BORDER_ON ]    = defaults.borderOn;
	params[ PT_BORDER_WIDTH ] = defaults.borderWidth;
	params[ PT_PILOT_LENGTH ] = defaults.pilotLength;

	params[ PT_MIX ]          = defaults.mix;
	params[ PT_BACKGROUND ]   = defaults.background;

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

pilot::frame::HostValues Pilot::hostValues() const
{
	frame::HostValues host;
	host.type        = params[ PT_TYPE ];
	host.baud        = params[ PT_BAUD ];
	host.ink         = params[ PT_INK ];
	host.paper       = params[ PT_PAPER ];
	host.bright      = params[ PT_BRIGHT ];
	host.progress    = params[ PT_PROGRESS ];
	host.sync        = params[ PT_SYNC ];
	host.errorRate   = params[ PT_ERROR_RATE ];
	host.message     = params[ PT_MESSAGE ];
	host.borderOn    = params[ PT_BORDER_ON ];
	host.borderWidth = params[ PT_BORDER_WIDTH ];
	host.pilotLength = params[ PT_PILOT_LENGTH ];
	host.mix         = params[ PT_MIX ];
	host.background  = params[ PT_BACKGROUND ];
	return host;
}

float Pilot::effectiveProgress( double seconds ) const
{
	const float manual = std::clamp( params[ PT_PROGRESS ], 0.0f, 1.0f );
	const int mode     = static_cast< int >( std::lround( params[ PT_SYNC ] ) );

	if( mode == kSyncManual )
		return manual;

	//Progress stays live in every mode: it is the offset the clock is added to,
	//so an operator can still place the load where they want it inside the bar.
	//Clip time is shared with the OpenFX build, which has a timeline and no
	//beat, so it lives in Frame.cpp.
	if( mode == kSyncClip )
		return frame::ClipTimeProgress( hostValues(), seconds );

	//The host gives a tempo and a position within the current bar, never which
	//bar it is. Recover a continuous count without keeping state: the clock
	//estimates how many bars have passed, barPhase is the exact position inside
	//this one, and the whole number reconciling them is round( estimate -
	//barPhase ). Continuous across the bar line, because as barPhase wraps from
	//1 to 0 the rounded integer steps up at the same instant. The same recovery
	//the rest of the fleet uses.
	const double tempo      = bpm > 1.0f ? static_cast< double >( bpm ) : 120.0;
	const double barSeconds = ( 60.0 * kBeatsPerBar ) / tempo;
	const double estimate   = seconds / barSeconds;
	const double within     = std::clamp( static_cast< double >( barPhase ), 0.0, 1.0 );
	const double bars       = within + std::round( estimate - within );

	const double turns = mode == kSyncBeat ? bars * kBeatsPerBar : bars;
	return frame::Wrap( static_cast< double >( manual ) + turns );
}

pilot::load::State Pilot::TapeStateForTest()
{
	return frame::Tape( hostValues(), effectiveProgress( elapsedSeconds() ) );
}

float Pilot::EffectiveProgressForTest()
{
	return effectiveProgress( elapsedSeconds() );
}

pilot::load::Border Pilot::BorderForTest()
{
	return frame::Border( hostValues(), elapsedSeconds() );
}

void Pilot::MessageBoxForTest( int& x, int& y, int& w, int& h )
{
	x = frame::kMessageX;
	y = frame::kMessageY;
	w = frame::MessageWidth();
	h = font::kHeight;
}

bool Pilot::PassesForTest( std::vector< unsigned char >& raster, std::vector< float >& attributes )
{
	if( !rasterBuffer.IsValid() || !attrBuffer.IsValid() )
		return false;

	raster.assign( static_cast< size_t >( zx::kScreenW ) * zx::kScreenH * 4, 0 );
	attributes.assign( static_cast< size_t >( zx::kCellsX ) * zx::kCellsY * 4, 0.0f );

	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glBindTexture( GL_TEXTURE_2D, rasterBuffer.GetTextureInfo().Handle );
	glGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, raster.data() );
	glBindTexture( GL_TEXTURE_2D, attrBuffer.GetTextureInfo().Handle );
	glGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, attributes.data() );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return glGetError() == GL_NO_ERROR;
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
	const int w = frame::MessageWidth();
	const int h = font::kHeight;

	//One byte per pixel, row 0 the TOP row of the glyphs. The compose pass
	//fetches it with the Spectrum's own top-down y, so uploading it this way up
	//is what keeps the flip count at the two already documented in the shader.
	//frame::MessageBit is the same bitmap the OpenFX build reads directly.
	std::vector< unsigned char > bitmap( static_cast< size_t >( w ) * h, 0 );
	for( int y = 0; y < h; ++y )
		for( int x = 0; x < w; ++x )
			if( frame::MessageBit( x, y ) )
				bitmap[ static_cast< size_t >( y ) * w + x ] = 255;

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

	//Every decision this frame makes on the CPU, made once in Frame.cpp -- the
	//same function the OpenFX build calls -- and handed to the passes below.
	const double seconds     = elapsedSeconds();
	const frame::Uniforms u  = frame::Prepare( hostValues(), effectiveProgress( seconds ), seconds );

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
		attrShader.Set( "BrightMode", u.brightMode );
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

		composeShader.Set( "Inset", u.inset, u.inset );
		composeShader.Set( "BytesRevealed", u.bytesRevealed );
		composeShader.Set( "DefaultInk", u.defaultInk[ 0 ], u.defaultInk[ 1 ], u.defaultInk[ 2 ] );
		composeShader.Set( "DefaultPaper", u.defaultPaper[ 0 ], u.defaultPaper[ 1 ], u.defaultPaper[ 2 ] );

		//The border's phase arrives already reduced to [0, 2); see Frame.cpp
		//for why that has to happen in double, before the shader sees it.
		composeShader.Set( "BorderStyle", u.borderStyle );
		composeShader.Set( "BorderHalfPhase", u.borderHalfPhase );
		composeShader.Set( "BorderHalfSpan", u.borderHalfSpan );
		composeShader.Set( "BorderA", u.borderA[ 0 ], u.borderA[ 1 ], u.borderA[ 2 ] );
		composeShader.Set( "BorderB", u.borderB[ 0 ], u.borderB[ 1 ], u.borderB[ 2 ] );

		composeShader.Set( "MessageOn", u.messageOn ? 1 : 0 );
		composeShader.Set( "MessageOrigin", float( frame::kMessageX ), float( frame::kMessageY ) );
		composeShader.Set( "MessageSize", float( frame::MessageWidth() ), float( font::kHeight ) );

		composeShader.Set( "Background", u.background );
		composeShader.Set( "Mix", u.mix );

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
