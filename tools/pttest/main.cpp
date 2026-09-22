/**
	pttest -- the offline harness.

	"It compiled" is not evidence that a tape loader loads a tape in the right
	order. So this drives the REAL `Pilot` class, through the real FFGL entry
	sequence, and checks what comes out.

	The shape of it is deliberate, and it is the shape the fleet arrived at
	after four plugins shipped checks that passed at one raster and failed at
	another:

	  * **Most checks open no GL context at all.** Nine of the twelve run
	    against `source/Spectrum.cpp` and `source/Loader.cpp` directly, before
	    any GL call, so they cannot depend on a rasteriser or on a raster. They
	    are integer arithmetic and they are compared bitwise.
	  * **The two that must rasterise run at two rasters on purpose**, one of
	    them small enough to resemble a CI runner. A thresholded position
	    measurement quantises to whole cells, and a check that only ever runs
	    at 1920x1080 cannot tell you that.
	  * **Every probe is validated, not assumed.** `--reveal` inverts the
	    shader's own mapping for each probe it places and fails if the probe
	    does not land on the Spectrum pixel it was aimed at -- and proves that
	    validator fires, by running it at a raster too small to resolve the grid.
	  * **`--negative` perturbs the model and asserts the checks fail.** A check
	    that cannot fail is not a check.

	AGENTS.md carries the table: every numeric check, its tolerance, and where
	that tolerance comes from.

		pttest --list
		pttest --out /tmp/pilot.png --size 1920x1080 --set "Progress=0.6"
		pttest --agree --order --thirds --attributes --border --error
		pttest --reveal --pixels --bench
*/

#include "Pilot.h"

#include "Controls.h"
#include "Font.h"
#include "Loader.h"
#include "Machines.h"
#include "Spectrum.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace pilot;

namespace
{
//---------------------------------------------------------------------------
// Reporting.
//---------------------------------------------------------------------------
int g_failures = 0;
int g_checks   = 0;

/// `--negative` runs the real check bodies against a broken model and asserts
/// they fail. What those runs print is noise, so they are silenced -- by a flag
/// rather than by reassigning stdout, which is a macro whose assignability is
/// nobody's business to rely on.
bool g_quiet = false;

void Check( bool ok, const std::string& what )
{
	++g_checks;
	if( !ok )
		++g_failures;
	if( !g_quiet )
		std::printf( "   %s  %s\n", ok ? "ok  " : "FAIL", what.c_str() );
}

/// printf, unless a negative-control run is in progress. Variadic rather than a
/// template so the compiler still checks the format string against the
/// arguments -- a template loses that, and -Wformat-security says so.
#if defined( __GNUC__ )
__attribute__( ( format( printf, 1, 2 ) ) )
#endif
void Say( const char* format, ... )
{
	if( g_quiet )
		return;
	va_list args;
	va_start( args, format );
	std::vprintf( format, args );
	va_end( args );
}

std::string F( double v, int places = 4 )
{
	char buf[ 64 ];
	std::snprintf( buf, sizeof( buf ), "%.*f", places, v );
	return buf;
}

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Test pictures. Written with y from the TOP, which is how they are described
// and how a PNG is read; the flip into GL's bottom-up order happens once, here.
//---------------------------------------------------------------------------
void setPixel( std::vector< unsigned char >& image, int w, int h, int x, int y, float r, float g, float b )
{
	const size_t i = ( static_cast< size_t >( h - 1 - y ) * w + x ) * 4;
	auto q         = []( float v ) { return static_cast< unsigned char >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) ); };
	image[ i + 0 ] = q( r );
	image[ i + 1 ] = q( g );
	image[ i + 2 ] = q( b );
	image[ i + 3 ] = 255;
}

/// A uniform field. The reveal check wants a picture whose 1-bit answer it
/// already knows: a black frame makes every cell flat and dark, so every
/// revealed pixel is ink and every unrevealed one is paper, and the frame
/// becomes a direct readout of the reveal mask.
std::vector< unsigned char > buildFlat( int w, int h, float level )
{
	std::vector< unsigned char > image( static_cast< size_t >( w ) * h * 4, 0 );
	for( int y = 0; y < h; ++y )
		for( int x = 0; x < w; ++x )
			setPixel( image, w, h, x, y, level, level, level );
	return image;
}

/// Four flat quadrants, in Spectrum order (y from the top): black, white, red,
/// blue. Flat on purpose -- a probe in the middle of a quadrant is a long way
/// from any boundary, so it means the same thing at 640x480 as at 1920x1080,
/// which is the entire point of running the pixel checks at two rasters.
std::vector< unsigned char > buildQuads( int w, int h )
{
	std::vector< unsigned char > image( static_cast< size_t >( w ) * h * 4, 0 );
	for( int y = 0; y < h; ++y )
	{
		for( int x = 0; x < w; ++x )
		{
			const bool left = x < w / 2;
			const bool top  = y < h / 2;
			if( top )
				setPixel( image, w, h, x, y, left ? 0.0f : 1.0f, left ? 0.0f : 1.0f, left ? 0.0f : 1.0f );
			else
				setPixel( image, w, h, x, y, left ? 1.0f : 0.0f, 0.0f, left ? 0.0f : 1.0f );
		}
	}
	return image;
}

void hsvToRgb( float hue, float s, float v, float& r, float& g, float& b )
{
	const float c  = v * s;
	const float hp = hue * 6.0f;
	const float x  = c * ( 1.0f - std::fabs( std::fmod( hp, 2.0f ) - 1.0f ) );
	const float m  = v - c;
	float rr = 0.0f, gg = 0.0f, bb = 0.0f;
	if( hp < 1.0f )      { rr = c; gg = x; }
	else if( hp < 2.0f ) { rr = x; gg = c; }
	else if( hp < 3.0f ) { gg = c; bb = x; }
	else if( hp < 4.0f ) { gg = x; bb = c; }
	else if( hp < 5.0f ) { rr = x; bb = c; }
	else                 { rr = c; bb = x; }
	r = rr + m;
	g = gg + m;
	b = bb + m;
}

/// The default card. Chosen to make wrong answers visible rather than to look
/// nice: a hue-by-brightness field shows what the attribute model does with
/// colours it cannot have, overlapping discs cross cell boundaries at every
/// phase, a grey ramp shows where the 1-bit threshold lands, and a fine
/// checkerboard shows whether the downsample averages or point-samples. The
/// bands are the ones nesolume's harness uses; see ATTRIBUTIONS.md.
std::vector< unsigned char > buildCard( int w, int h )
{
	std::vector< unsigned char > image( static_cast< size_t >( w ) * h * 4, 0 );

	const int hueEnd  = h * 45 / 100;
	const int discEnd = h * 70 / 100;
	const int rampEnd = h * 85 / 100;

	for( int y = 0; y < h; ++y )
	{
		for( int x = 0; x < w; ++x )
		{
			const float u = static_cast< float >( x ) / static_cast< float >( w );

			if( y < hueEnd )
			{
				const float v = 1.0f - 0.85f * static_cast< float >( y ) / static_cast< float >( hueEnd );
				float r, g, b;
				hsvToRgb( u, 0.9f, v, r, g, b );
				setPixel( image, w, h, x, y, r, g, b );
			}
			else if( y < discEnd )
			{
				const float bandY = ( static_cast< float >( y - hueEnd ) / static_cast< float >( discEnd - hueEnd ) - 0.5f ) * 2.0f;
				float r = 0.45f, g = 0.45f, b = 0.45f;
				const struct { float cx, cr, cg, cb; } discs[ 3 ] = {
					{ 0.35f, 0.9f, 0.1f, 0.1f },
					{ 0.50f, 0.1f, 0.8f, 0.15f },
					{ 0.65f, 0.15f, 0.2f, 0.9f },
				};
				for( const auto& d : discs )
				{
					const float dx = ( u - d.cx ) * ( static_cast< float >( w ) / static_cast< float >( discEnd - hueEnd ) );
					if( dx * dx + bandY * bandY < 0.8f )
					{
						r = d.cr;
						g = d.cg;
						b = d.cb;
					}
				}
				setPixel( image, w, h, x, y, r, g, b );
			}
			else if( y < rampEnd )
			{
				setPixel( image, w, h, x, y, u, u, u );
			}
			else
			{
				const bool on = ( ( x / 3 ) + ( y / 3 ) ) % 2 == 0;
				setPixel( image, w, h, x, y, on ? 0.95f : 0.05f, on ? 0.2f : 0.7f, 0.3f );
			}
		}
	}

	return image;
}

//---------------------------------------------------------------------------
// The headless context.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	//Accelerated first; fall back so the harness still runs somewhere without a
	//GPU, where it will at least prove the shaders compile.
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

/// One output size, as a framebuffer the plugin can treat as the host's.
struct Target
{
	int w = 0, h = 0;
	GLuint texture = 0, fbo = 0;

	bool Create( int width, int height )
	{
		w = width;
		h = height;
		glGenTextures( 1, &texture );
		glBindTexture( GL_TEXTURE_2D, texture );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glBindTexture( GL_TEXTURE_2D, 0 );

		glGenFramebuffers( 1, &fbo );
		glBindFramebuffer( GL_FRAMEBUFFER, fbo );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
		const bool ok = glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE;
		glBindFramebuffer( GL_FRAMEBUFFER, 0 );
		return ok;
	}

	void Destroy()
	{
		if( fbo )
			glDeleteFramebuffers( 1, &fbo );
		if( texture )
			glDeleteTextures( 1, &texture );
		fbo = texture = 0;
	}
};

/// An input texture holding one of the test pictures.
GLuint makeInput( const std::vector< unsigned char >& picture, int w, int h )
{
	GLuint id = 0;
	glGenTextures( 1, &id );
	glBindTexture( GL_TEXTURE_2D, id );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, picture.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return id;
}

/// A frame, top-down, RGBA8.
struct Image
{
	int w = 0, h = 0;
	std::vector< unsigned char > px;

	const unsigned char* at( int x, int y ) const
	{
		return px.data() + ( static_cast< size_t >( y ) * w + x ) * 4;
	}
	bool is( int x, int y, int r, int g, int b ) const
	{
		const unsigned char* p = at( x, y );
		return p[ 0 ] == r && p[ 1 ] == g && p[ 2 ] == b;
	}
	std::string str( int x, int y ) const
	{
		const unsigned char* p = at( x, y );
		char buf[ 48 ];
		std::snprintf( buf, sizeof( buf ), "(%d, %d, %d)", p[ 0 ], p[ 1 ], p[ 2 ] );
		return buf;
	}
};

Image readBack( const Target& t )
{
	std::vector< unsigned char > raw( static_cast< size_t >( t.w ) * t.h * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, t.fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, t.w, t.h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data() );
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );

	Image out;
	out.w = t.w;
	out.h = t.h;
	out.px.resize( raw.size() );
	const size_t stride = static_cast< size_t >( t.w ) * 4;
	for( int y = 0; y < t.h; ++y )
		std::memcpy( out.px.data() + static_cast< size_t >( y ) * stride,
		             raw.data() + static_cast< size_t >( t.h - 1 - y ) * stride, stride );
	return out;
}

/// A plugin instance ready to render at a given size.
struct Instance
{
	Pilot plugin;
	bool ok = false;

	Instance( int w, int h )
	{
		plugin.ForceSecondsClock();
		FFGLViewportStruct vp = { 0, 0, static_cast< FFUInt32 >( w ), static_cast< FFUInt32 >( h ) };
		ok                    = plugin.InitGL( &vp ) == FF_SUCCESS;
		if( !ok )
			std::printf( "   InitGL FAILED -- see the diagnostics log for which shader\n" );
	}
	~Instance()
	{
		plugin.DeInitGL();
	}

	void set( Pilot::ParamID id, float v )
	{
		plugin.SetFloatParameter( id, v );
	}

	/// The message is the only thing this plugin draws over its own raster, and
	/// a probe lands on whatever was drawn on top of it. Every check that reads
	/// a screen pixel turns it off first -- the same lesson graticule's burn-in
	/// plate taught the fleet twice.
	void quiet()
	{
		set( Pilot::PT_MESSAGE, 0.0f );
	}
};

Image render( Instance& i, const Target& t, GLuint input, int inputW, int inputH, double seconds )
{
	FFGLTextureStruct in = {};
	in.Width = in.HardwareWidth = static_cast< FFUInt32 >( inputW );
	in.Height = in.HardwareHeight = static_cast< FFUInt32 >( inputH );
	in.Handle                     = input;
	FFGLTextureStruct* inputs[ 1 ] = { &in };

	ProcessOpenGLStruct gl = {};
	gl.numInputTextures    = 1;
	gl.inputTextures       = inputs;
	gl.HostFBO             = t.fbo;

	glBindFramebuffer( GL_FRAMEBUFFER, t.fbo );
	glViewport( 0, 0, t.w, t.h );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );

	// A real host calls SetBeatInfo every frame, and the SDK's defaults are
	// 120 bpm with barPhase pinned at 0 -- which is a host saying it is
	// permanently at the start of a bar. Leave it there and the Beat and Bar
	// sync modes correctly never move, and tools/sweep.py correctly reports
	// Sync as a dead control. That is the harness's fault, not the plugin's,
	// and it is worth knowing before anyone "fixes" the plugin for it.
	constexpr double kBpm        = 120.0;
	constexpr double kBarSeconds = 240.0 / kBpm;
	const double bars            = seconds / kBarSeconds;
	i.plugin.SetBeatInfo( static_cast< float >( kBpm ), static_cast< float >( bars - std::floor( bars ) ) );

	i.plugin.SetTime( seconds );
	i.plugin.ProcessOpenGL( &gl );
	return readBack( t );
}

//---------------------------------------------------------------------------
// The mapping between an output pixel and a Spectrum pixel.
//
// This mirrors the compose shader, in float and in the same order, because it
// is used to PLACE probes and then to VALIDATE that they landed. A probe is not
// assumed to hit the Spectrum pixel it was aimed at; it is checked, and when
// the raster is too coarse to resolve the grid the check says so rather than
// quietly measuring the neighbour.
//---------------------------------------------------------------------------
struct Mapping
{
	int w, h;
	float inset;

	/// The Spectrum pixel the shader will read at output pixel (ox, oyTop).
	/// Returns false when the sample falls outside the screen area.
	bool toSpectrum( int ox, int oyTop, int& px, int& py ) const
	{
		const float uvx = ( static_cast< float >( ox ) + 0.5f ) / static_cast< float >( w );
		const float uvy = ( static_cast< float >( h - 1 - oyTop ) + 0.5f ) / static_cast< float >( h );
		const float span = std::max( 1.0f - 2.0f * inset, 1e-5f );
		const float ix  = ( uvx - inset ) / span;
		const float iy  = ( uvy - inset ) / span;
		if( ix < 0.0f || ix >= 1.0f || iy < 0.0f || iy >= 1.0f )
			return false;
		px = std::clamp( static_cast< int >( ix * 256.0f ), 0, 255 );
		py = std::clamp( static_cast< int >( ( 1.0f - iy ) * 192.0f ), 0, 191 );
		return true;
	}

	/// The output pixel nearest the centre of a Spectrum pixel.
	void probeFor( int px, int py, int& ox, int& oyTop ) const
	{
		const float span = 1.0f - 2.0f * inset;
		const float fx   = ( static_cast< float >( px ) + 0.5f ) / 256.0f;
		const float fy   = ( static_cast< float >( py ) + 0.5f ) / 192.0f;
		ox    = std::clamp( static_cast< int >( ( inset + fx * span ) * w ), 0, w - 1 );
		oyTop = std::clamp( static_cast< int >( ( inset + fy * span ) * h ), 0, h - 1 );
	}
};

//===========================================================================
// CPU checks. No GL context is created by any of these.
//===========================================================================

//---------------------------------------------------------------------------
/// --agree: the two address derivations agree, for every address.
//---------------------------------------------------------------------------
int runAgree( int perturb )
{
	Say( "agree: the bit layout against the nested loop\n\n" );

	const std::vector< zx::Address > order = zx::ScreenOrder();
	Check( static_cast< int >( order.size() ) == zx::kDisplayBytes,
	       "the loop enumerates " + std::to_string( zx::kDisplayBytes ) + " addresses, got " + std::to_string( order.size() ) );

	int mismatches   = 0;
	int firstBad     = -1;
	std::vector< char > seen( zx::kDisplayBytes, 0 );
	for( int k = 0; k < static_cast< int >( order.size() ); ++k )
	{
		const int index = zx::ScreenIndex( order[ k ].xbyte * 8, order[ k ].py, perturb );
		if( index != k )
		{
			if( firstBad < 0 )
				firstBad = k;
			++mismatches;
		}
		if( index >= 0 && index < zx::kDisplayBytes )
			seen[ index ] = 1;
	}

	// Bitwise, with no tolerance: both sides are integer arithmetic, so a
	// difference of one is a transposition and there is nothing to allow for.
	Check( mismatches == 0,
	       "all " + std::to_string( zx::kDisplayBytes ) + " addresses agree, bitwise (mismatches: "
	           + std::to_string( mismatches ) + ( firstBad >= 0 ? ", first at " + std::to_string( firstBad ) : "" ) + ")" );

	const int covered = static_cast< int >( std::count( seen.begin(), seen.end(), 1 ) );
	Check( covered == zx::kDisplayBytes,
	       "the order is a permutation: every address is hit exactly once (covered " + std::to_string( covered ) + ")" );

	// The other 768 bytes, which are the reason the picture lands in monochrome.
	bool attrOk = true;
	std::vector< char > attrSeen( zx::kAttributeBytes, 0 );
	for( int cy = 0; cy < zx::kCellsY; ++cy )
		for( int cx = 0; cx < zx::kCellsX; ++cx )
		{
			const int a = zx::AttributeIndex( cx * 8, cy * 8 );
			if( a < zx::kDisplayBytes || a >= zx::kTotalBytes )
				attrOk = false;
			else
				attrSeen[ a - zx::kDisplayBytes ] = 1;
		}
	Check( attrOk && std::count( attrSeen.begin(), attrSeen.end(), 1 ) == zx::kAttributeBytes,
	       "the 768 attribute addresses are 6144..6911, one per cell" );

	return g_failures;
}

//---------------------------------------------------------------------------
/// --order: at progress p the revealed set is exactly the first p*N addresses.
//---------------------------------------------------------------------------
int runOrder( int perturb )
{
	Say( "order: the revealed set against the independent table\n\n" );

	const std::vector< zx::Address > order = zx::ScreenOrder();

	// Every byte count worth trying, plus every boundary that matters: the
	// start, the last display byte, the first attribute byte and the end.
	std::vector< int > counts;
	for( int k = 0; k <= 64; ++k )
		counts.push_back( static_cast< int >( std::lround( k / 64.0 * zx::kTotalBytes ) ) );
	for( int extra : { 0, 1, 2047, 2048, 6143, 6144, 6145, 6911, 6912 } )
		counts.push_back( extra );

	int worst = 0;
	for( int bytes : counts )
	{
		// The table's answer: the pixels covered by the first `bytes` addresses.
		std::vector< char > expected( zx::kScreenW * zx::kScreenH, 0 );
		const int upTo = std::min( bytes, zx::kDisplayBytes );
		for( int k = 0; k < upTo; ++k )
			for( int b = 0; b < 8; ++b )
				expected[ order[ k ].py * zx::kScreenW + order[ k ].xbyte * 8 + b ] = 1;

		int wrong = 0;
		for( int py = 0; py < zx::kScreenH; ++py )
			for( int px = 0; px < zx::kScreenW; ++px )
				if( ( zx::PixelRevealed( px, py, bytes, perturb ) ? 1 : 0 ) != expected[ py * zx::kScreenW + px ] )
					++wrong;
		worst = std::max( worst, wrong );
	}

	Check( worst == 0,
	       "over " + std::to_string( counts.size() ) + " byte counts, the revealed set is exactly the first "
	           + "n addresses (worst disagreement: " + std::to_string( worst ) + " pixels of 49152)" );

	// A byte is eight horizontal pixels, revealed together. If that were ever
	// not true the interleave would still look right and the model would be
	// wrong, so it is asserted rather than assumed.
	int split = 0;
	for( int bytes = 1; bytes <= zx::kDisplayBytes; bytes += 37 )
		for( int py = 0; py < zx::kScreenH; ++py )
			for( int xb = 0; xb < zx::kCellsX; ++xb )
			{
				const bool first = zx::PixelRevealed( xb * 8, py, bytes, perturb );
				for( int b = 1; b < 8; ++b )
					if( zx::PixelRevealed( xb * 8 + b, py, bytes, perturb ) != first )
						++split;
			}
	Check( split == 0, "a byte's eight pixels are always revealed together (split bytes: " + std::to_string( split ) + ")" );

	return g_failures;
}

//---------------------------------------------------------------------------
/// --thirds: the interleave is present, and the thirds go in order.
//---------------------------------------------------------------------------
int runThirds( int perturb )
{
	Say( "thirds: the interleave, not a top-down wipe\n\n" );

	// When each pixel row first has a byte on it.
	std::vector< int > firstFor( zx::kScreenH, zx::kTotalBytes + 1 );
	for( int py = 0; py < zx::kScreenH; ++py )
	{
		int lowest = zx::kTotalBytes + 1;
		for( int xb = 0; xb < zx::kCellsX; ++xb )
			lowest = std::min( lowest, zx::ScreenIndex( xb * 8, py, perturb ) );
		firstFor[ py ] = lowest;
	}

	bool thirdsInOrder = true;
	for( int third = 0; third < 3; ++third )
	{
		int lo = zx::kTotalBytes + 1, hi = -1;
		for( int py = third * 64; py < ( third + 1 ) * 64; ++py )
		{
			lo = std::min( lo, firstFor[ py ] );
			hi = std::max( hi, firstFor[ py ] );
		}
		if( lo != third * 2048 || hi >= ( third + 1 ) * 2048 )
			thirdsInOrder = false;
	}
	Check( thirdsInOrder, "each third occupies its own 2048 addresses, in order" );

	// Within a third, row 0 precedes row 8 precedes row 16... -- the eight-line
	// interleave. A top-down wipe has row 0 before row 1, which is the thing
	// this is here to rule out.
	bool interleaved = true;
	bool notTopDown  = true;
	for( int third = 0; third < 3; ++third )
	{
		const int base = third * 64;
		for( int charrow = 0; charrow + 1 < 8; ++charrow )
			if( firstFor[ base + charrow * 8 ] >= firstFor[ base + ( charrow + 1 ) * 8 ] )
				interleaved = false;
		// Row 1 of the third is a whole 256-byte line later than row 0, not
		// 32 bytes later.
		if( firstFor[ base + 1 ] - firstFor[ base + 0 ] != 256 )
			notTopDown = false;
	}
	Check( interleaved, "within a third, row 0 precedes row 8 precedes row 16 ... precedes row 56" );
	Check( notTopDown, "row 1 arrives 256 bytes after row 0, not 32: this is an interleave, not a wipe" );

	return g_failures;
}

//---------------------------------------------------------------------------
/// --attributes: no colour before 6144.
//---------------------------------------------------------------------------
int runAttributes()
{
	Say( "attributes: colour arrives last, in one block\n\n" );

	int earliest = zx::kTotalBytes + 1;
	for( int cy = 0; cy < zx::kCellsY; ++cy )
		for( int cx = 0; cx < zx::kCellsX; ++cx )
			earliest = std::min( earliest, zx::AttributeIndex( cx * 8, cy * 8 ) );
	Check( earliest == zx::kDisplayBytes,
	       "the first attribute byte is at 6144, got " + std::to_string( earliest ) );

	int leaked = 0;
	for( int bytes = 0; bytes <= zx::kDisplayBytes; ++bytes )
		for( int cy = 0; cy < zx::kCellsY; ++cy )
			for( int cx = 0; cx < zx::kCellsX; ++cx )
				if( zx::AttributeRevealed( cx, cy, bytes ) )
					++leaked;
	Check( leaked == 0, "no cell has an attribute at or below 6144 bytes (leaks: " + std::to_string( leaked ) + ")" );

	// ...and every one of them has arrived by the end.
	int missing = 0;
	for( int cy = 0; cy < zx::kCellsY; ++cy )
		for( int cx = 0; cx < zx::kCellsX; ++cx )
			if( !zx::AttributeRevealed( cx, cy, zx::kTotalBytes ) )
				++missing;
	Check( missing == 0, "every cell has its attribute at 6912 bytes (missing: " + std::to_string( missing ) + ")" );

	// The threshold as a fraction of the load, which is the number the README
	// quotes: 6144/6912.
	Check( std::fabs( double( zx::kDisplayBytes ) / zx::kTotalBytes - 0.888888888 ) < 1e-6,
	       "colour starts at " + F( double( zx::kDisplayBytes ) / zx::kTotalBytes, 6 ) + " of the load" );

	return g_failures;
}

//---------------------------------------------------------------------------
/// --border: the stripe period is eight bit periods, at two rasters.
//---------------------------------------------------------------------------
/**
	`modelBaudScale` is the negative control's handle: the MODEL is driven at
	`nominal * scale` while every expectation is derived from `nominal`. At 1.0
	they are the same number and this is the real check; at 1.01 the model runs
	one per cent fast and the check must notice.
*/
int runBorder( double modelBaudScale )
{
	Say( "border: the stripe period is the byte period\n\n" );

	const double fps = 60.0;//the harness's synthetic clock

	//-----------------------------------------------------------------------
	// The sample times are deliberately NOT on the 60 fps grid, and finding
	// that out was the first thing this check did.
	//
	// The claim is that floor( phase + 16 ) - floor( phase ) is 16, which is
	// true for every real number. In double it is true unless `phase` sits
	// within the rounding error of an integer -- and on the 60 fps grid it
	// always does: at 1500 baud, k/60 seconds is exactly 25k bits, so every
	// sample landed on a half-cycle boundary and roughly one in nine came back
	// as 15. That is an artefact of the sampling, not of the plugin, which
	// takes one floor per row and never a difference of two.
	//
	// So the times are stepped by an interval that is not a dyadic rational and
	// does not divide any machine's bit period. A sample that still lands
	// within 1e-6 of a boundary -- t = 0 is one, being the phase origin -- is
	// counted and left out, and the count is asserted to be a vanishing
	// fraction, because a claim of "exactly 16" over an empty set would be
	// passing by luck rather than by arithmetic.
	//-----------------------------------------------------------------------
	const double step = 0.0173219;

	for( int m = 0; m < machineCount(); ++m )
	{
		const MachineSpec& spec = machine( m );
		const double baud       = spec.nominalBaud;           //what the control says
		const double modelBaud  = baud * modelBaudScale;      //what the model is given
		const double bytePeriod = 8.0 / baud;                 //eight bit periods at the stated baud

		Say( "  %s, %g baud, %g Hz\n", spec.name, baud, spec.frameHz );

		if( spec.border == kBorderFlash )
		{
			//---------------------------------------------------------------
			// The whole border, one colour, changed once per byte.
			//---------------------------------------------------------------
			int wrong = 0;
			int onBoundary = 0;
			std::vector< char > seen( 8, 0 );
			int black = 0;
			for( int k = 0; k < 2000; ++k )
			{
				const double t        = k * step;
				const load::Border b0 = load::BorderAt( t, modelBaud, spec.frameHz );
				const load::Border b1 = load::BorderAt( t + bytePeriod, modelBaud, spec.frameHz );

				const double bytes = b0.bitPosTop / 8.0;
				if( std::fabs( bytes - std::floor( bytes + 0.5 ) ) < 1e-6 )
					++onBoundary;
				else if( load::ByteIndex( b1 ) - load::ByteIndex( b0 ) != 1 )
					++wrong;

				const int colour = load::FlashColour( load::ByteIndex( b0 ) );
				if( colour < 0 || colour > 7 )
					++black;
				else
				{
					seen[ colour ] = 1;
					if( colour == 0 )
						++black;
				}
			}
			Check( onBoundary * 100 < 2000,
			       "  " + std::to_string( onBoundary ) + " of 2000 samples sat on a byte boundary and were left out, under 1%" );
			Check( wrong == 0, "  one byte of tape advances the flash by exactly one (wrong: " + std::to_string( wrong ) + " of 2000)" );
			Check( black == 0, "  the flash never lands on black -- that would read as a dropout" );
			Check( std::count( seen.begin(), seen.end(), 1 ) >= 5, "  the flash uses at least five of the seven colours" );
			Say( "     a flash lasts %s frames at %g fps\n", F( bytePeriod * fps, 3 ).c_str(), fps );
			continue;
		}

		//-------------------------------------------------------------------
		// The scanned border. Two half-cycles to a bit, so eight bit periods
		// is sixteen half-cycles, exactly. Both sides are a floor of a double,
		// so the whole-number assertion carries NO tolerance at all; the
		// continuous form is allowed 1e-6, which is a double-rounding budget
		// over a bit position that reaches 1e8 and nothing more.
		//-------------------------------------------------------------------
		int wrongWhole         = 0;
		int onBoundary         = 0;
		double worstContinuous = 0.0;
		for( int k = 0; k < 2000; ++k )
		{
			const double t = k * step;
			for( double f : { 0.0, 0.5, 1.0 } )
			{
				const load::Border b0 = load::BorderAt( t, modelBaud, spec.frameHz );
				const load::Border b1 = load::BorderAt( t + bytePeriod, modelBaud, spec.frameHz );

				const double phase = load::HalfCycleContinuous( b0, f );
				if( std::fabs( phase - std::floor( phase + 0.5 ) ) < 1e-6 )
					++onBoundary;
				else if( load::HalfCycle( b1, f ) - load::HalfCycle( b0, f ) != 16 )
					++wrongWhole;

				worstContinuous = std::max( worstContinuous,
				                            std::fabs( load::HalfCycleContinuous( b1, f ) - load::HalfCycleContinuous( b0, f ) - 16.0 ) );
			}
		}
		Check( onBoundary * 100 < 6000,
		       "  " + std::to_string( onBoundary ) + " of 6000 samples sat on a half-cycle boundary and were left out, under 1%" );
		Check( wrongWhole == 0,
		       "  eight bit periods advance the stripe by exactly 16 half-cycles (wrong: " + std::to_string( wrongWhole ) + " of 6000)" );
		Check( worstContinuous < 1e-6,
		       "  the continuous phase agrees to " + F( worstContinuous, 12 ) + " (allowance 1e-6: double rounding only)" );

		//-------------------------------------------------------------------
		// Raster independence. How many stripe pairs fit down the picture is
		// baud / frameHz and has nothing to do with the composition's height.
		// Measured at two heights, one of them small enough to be a CI runner,
		// because a count like this is exactly the kind that quantises to whole
		// cells and passes at one raster while failing at another.
		//
		// The expectation is not `baud / frameHz`: sampling row CENTRES can
		// only see the span between the first and last of them, which is
		// (h-1)/h of the frame. The allowance is half a pair -- one half-cycle
		// -- because the count is a difference of two floors and that is the
		// whole of what a floor can lose.
		//-------------------------------------------------------------------
		bool pairsOk = true;
		double measured[ 2 ] = { 0.0, 0.0 };
		int index = 0;
		for( int height : { 1080, 240 } )
		{
			const load::Border b = load::BorderAt( 3.0, modelBaud, spec.frameHz );
			int toggles          = 0;
			long long previous   = load::HalfCycle( b, 0.5 / height );
			for( int row = 1; row < height; ++row )
			{
				const long long now = load::HalfCycle( b, ( row + 0.5 ) / height );
				toggles += static_cast< int >( now - previous );
				previous = now;
			}
			const double pairs = toggles / 2.0;
			const double want  = ( baud / spec.frameHz ) * ( height - 1.0 ) / height;
			measured[ index++ ] = pairs;
			if( std::fabs( pairs - want ) > 0.5 )
				pairsOk = false;
			Say( "     %4d rows: %s stripe pairs between the first and last row centre (expected %s)\n",
			     height, F( pairs, 3 ).c_str(), F( want, 3 ).c_str() );
		}
		Check( pairsOk,
		       "  the stripe count matches baud/frameHz * (h-1)/h at BOTH 1080 and 240 rows, to half a pair" );
		Check( std::fabs( measured[ 0 ] - measured[ 1 ] ) <= 1.0 + baud / spec.frameHz / 240.0,
		       "  the two rasters agree to " + F( std::fabs( measured[ 0 ] - measured[ 1 ] ), 3 )
		           + " pairs (allowance: one half-cycle each, plus the 1/240 of a frame the coarser one cannot see)" );
	}

	return g_failures;
}

//---------------------------------------------------------------------------
/// --error: the mean number of blocks before a failure is 1/r.
//---------------------------------------------------------------------------
/**
	`drawScale` is the negative control's handle: the DRAW is made at
	`rate * scale` and the expectation stays `1 / rate`. At 1.0 they agree and
	this is the real check.
*/
int runError( double drawScale )
{
	Say( "error: blocks before a failure, against 1/r\n\n" );

	// A seeded, reproducible draw: the same tape always fails in the same
	// places, so a composition renders the same way twice.
	constexpr int kSamples = 20000;
	constexpr int kCap     = 200000;

	Say( "  %-8s %10s %10s %10s %10s\n", "rate", "mean", "1/r", "4 sigma", "verdict" );

	for( float rate : { 0.02f, 0.05f, 0.1f, 0.25f, 0.5f } )
	{
		const double r         = rate;
		const double drawRate  = r * drawScale;

		double total = 0.0;
		int capped   = 0;
		for( int s = 0; s < kSamples; ++s )
		{
			int trials = 0;
			while( trials < kCap && !load::BlockFails( static_cast< uint32_t >( s ) + 1u, 0, trials, static_cast< float >( drawRate ) ) )
				++trials;
			if( trials >= kCap )
				++capped;
			total += trials + 1;//count the failing trial itself
		}
		const double mean = total / kSamples;
		const double want = 1.0 / r;

		// Geometric with failure probability r: the number of trials up to and
		// including the first failure has mean 1/r and variance (1-r)/r^2, so
		// the standard error of the mean over N draws is
		// sqrt(1-r) / (r * sqrt(N)). Four of those is a 99.994% interval for a
		// normal, and the mean of 20000 draws is normal enough for that to mean
		// what it says. The band is DERIVED, not chosen: nothing here was
		// widened until the test passed.
		const double sigma = std::sqrt( 1.0 - r ) / ( r * std::sqrt( double( kSamples ) ) );
		const double band  = 4.0 * sigma;
		const bool ok      = std::fabs( mean - want ) <= band && capped == 0;

		Say( "  %-8s %10s %10s %10s %10s\n", F( r, 3 ).c_str(), F( mean, 3 ).c_str(),
		     F( want, 3 ).c_str(), F( band, 3 ).c_str(), ok ? "ok" : "FAIL" );
		++g_checks;
		if( !ok )
			++g_failures;
	}

	Say( "\n  %d samples per rate; the band is 4 * sqrt(1-r) / (r * sqrt(N))\n", kSamples );

	// The draw has to be reproducible, or nothing above means anything.
	bool stable = true;
	for( int k = 0; k < 500; ++k )
		if( load::BlockFails( 7u, 1, k, 0.3f ) != load::BlockFails( 7u, 1, k, 0.3f ) )
			stable = false;
	Check( stable, "the draw is a pure function of (seed, attempt, block)" );

	// Nothing fails at rate 0 and everything fails at rate 1. Both ends are
	// reachable from the control, so both are asserted rather than assumed.
	int atZero = 0, atOne = 0;
	for( int k = 0; k < 1000; ++k )
	{
		if( load::BlockFails( 3u, 0, k, 0.0f ) )
			++atZero;
		if( !load::BlockFails( 3u, 0, k, 1.0f ) )
			++atOne;
	}
	Check( atZero == 0 && atOne == 0, "rate 0 never fails and rate 1 always does" );

	// Successive attempts must be independent streams, or a tape that failed
	// once would fail in the same place for ever.
	int sameAsAttemptZero = 0;
	for( int k = 0; k < 2000; ++k )
		if( load::BlockFails( 11u, 0, k, 0.3f ) == load::BlockFails( 11u, 1, k, 0.3f ) )
			++sameAsAttemptZero;
	// Two independent Bernoulli(0.3) streams agree 0.3*0.3 + 0.7*0.7 = 58% of
	// the time. Anything near 100% means the attempt is not in the hash.
	Check( sameAsAttemptZero < 1500,
	       "attempt 1 is a different stream from attempt 0 (" + std::to_string( sameAsAttemptZero )
	           + "/2000 agree; two independent streams agree ~1160)" );

	return g_failures;
}
//---------------------------------------------------------------------------
/// --message: the sweep's context for Message On still shows a message.
//---------------------------------------------------------------------------
/// tools/sweep.py needs a (rate, progress) at which a block has just failed,
/// or Message On is correctly dead and the sweep reports a control that is
/// fine. The pair below was found by scanning the two controls and is pinned
/// here, so a change to the hash breaks this check loudly instead of quietly
/// hollowing the sweep out.
///
/// It is chosen to sit in the MIDDLE of the hold window -- half a block past
/// the failure -- rather than at either edge, so a small change to the model
/// moves it without falling off it.
constexpr float kSweepErrorRate = 0.30f;///< the Error Rate control's value
constexpr float kSweepProgress  = 0.54f;///< the Progress control's value

int runMessage()
{
	Say( "message: the pinned sweep context still fails a block\n\n" );

	Pilot plugin;
	plugin.ForceSecondsClock();
	plugin.SetFloatParameter( Pilot::PT_ERROR_RATE, kSweepErrorRate );
	plugin.SetFloatParameter( Pilot::PT_PROGRESS, kSweepProgress );
	plugin.SetTime( 0.0 );

	const load::State state = plugin.TapeStateForTest();
	Say( "  Error Rate=%g Progress=%g -> attempt %d, %d bytes, %s blocks since a failure\n",
	             kSweepErrorRate, kSweepProgress, state.attempt, state.bytesRevealed, F( state.sinceFailure, 3 ).c_str() );

	Check( state.attempt > 0, "a block has failed by this point" );
	Check( state.message, "the message is showing, so Message On has something to do" );

	// The hold is a whole block of tape and nothing longer, which is what keeps
	// the message from sitting on the picture for the rest of the load.
	Pilot later;
	later.ForceSecondsClock();
	later.SetFloatParameter( Pilot::PT_ERROR_RATE, kSweepErrorRate );
	later.SetFloatParameter( Pilot::PT_PROGRESS, 1.0f );
	later.SetTime( 0.0 );
	const load::State end = later.TapeStateForTest();
	Check( !end.message || end.sinceFailure < load::kMessageHold,
	       "the message only shows within " + F( load::kMessageHold, 1 ) + " block of a failure" );

	// With no errors there is no message at all, which is why the sweep needs
	// the context in the first place.
	Pilot clean;
	clean.ForceSecondsClock();
	clean.SetFloatParameter( Pilot::PT_PROGRESS, 0.6f );
	clean.SetTime( 0.0 );
	Check( !clean.TapeStateForTest().message, "at Error Rate 0 the message never shows" );

	return g_failures;
}

//---------------------------------------------------------------------------
/// --clock: a host clock six days in does not move the border.
//---------------------------------------------------------------------------
int runClock()
{
	Say( "clock: the border phase survives Resolume's clock\n\n" );

	const double baud = 1500.0;
	const double fps  = 50.0;

	// Resolume's SetTime is milliseconds since the composition opened. Six days
	// of uptime is 5.0e8 ms, which is where a float's step passes 0.03 s -- a
	// twentieth of the period of a 1500-baud stripe. The arithmetic here is
	// double throughout for that reason, and this asserts it.
	const double sixDays = 6.0 * 24.0 * 3600.0;

	int wrong = 0;
	for( int k = 0; k < 500; ++k )
	{
		const double t = sixDays + k / 60.0;
		const load::Border b0 = load::BorderAt( t, baud, fps );
		const load::Border b1 = load::BorderAt( t + 8.0 / baud, baud, fps );
		if( load::HalfCycle( b1, 0.25 ) - load::HalfCycle( b0, 0.25 ) != 16 )
			++wrong;
	}
	Check( wrong == 0, "six days in, eight bit periods still advance the stripe by 16 (wrong: " + std::to_string( wrong ) + ")" );

	// ...and the same arithmetic in a float does not, which is the trap stated
	// as a measurement rather than as a warning.
	int floatWrong = 0;
	for( int k = 0; k < 500; ++k )
	{
		const float t  = static_cast< float >( sixDays * 1000.0 + k / 60.0 );//milliseconds, as the host sends it
		const float t1 = t + static_cast< float >( 8.0 / baud * 1000.0 );
		const long long h0 = static_cast< long long >( std::floor( ( t * 0.001f ) * float( baud ) * 2.0f ) );
		const long long h1 = static_cast< long long >( std::floor( ( t1 * 0.001f ) * float( baud ) * 2.0f ) );
		if( h1 - h0 != 16 )
			++floatWrong;
	}
	Check( floatWrong > 0,
	       "the same sum in a float is wrong " + std::to_string( floatWrong ) + " times in 500 -- which is why it is not" );

	// What actually reaches the shader is the phase reduced to [0, 2), and the
	// reduction must preserve the parity or the stripes invert.
	int parityWrong = 0;
	for( int k = 0; k < 2000; ++k )
	{
		const double t   = sixDays + k * 0.001;
		const load::Border b = load::BorderAt( t, baud, fps );
		const double full    = load::HalfCycleContinuous( b, 0.0 );
		double reduced       = std::fmod( full, 2.0 );
		if( reduced < 0.0 )
			reduced += 2.0;
		const long long a = static_cast< long long >( std::floor( full ) ) & 1;
		const long long c = static_cast< long long >( std::floor( reduced ) ) & 1;
		if( a != c )
			++parityWrong;
	}
	Check( parityWrong == 0, "reducing the phase modulo two keeps the stripe's parity (wrong: " + std::to_string( parityWrong ) + ")" );

	return g_failures;
}

//---------------------------------------------------------------------------
/// --names: the host's limits on what a plugin may call things.
//---------------------------------------------------------------------------
int runNames()
{
	Say( "names: what the host will actually show\n\n" );

	Pilot plugin;

	// The FFGL name field is char[16] and is NOT null-terminated, so a longer
	// name is truncated by the host with no warning anywhere.
	const std::string display = "SW Pilot";
	Check( display.size() <= 16, "the plugin name '" + display + "' is " + std::to_string( display.size() ) + " of 16 characters" );

	bool unique = true;
	bool sized  = true;
	std::vector< std::string > names;
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const char* n = plugin.GetParamName( i );
		const std::string name = n ? n : "";
		if( name.size() > 16 )
		{
			sized = false;
			Say( "   parameter %u is '%s', %zu characters\n", i, name.c_str(), name.size() );
		}
		for( const std::string& seen : names )
			if( seen == name )
			{
				unique = false;
				Say( "   parameter %u repeats the name '%s'\n", i, name.c_str() );
			}
		names.push_back( name );
	}
	Check( sized, "no parameter name is over 16 characters" );
	Check( unique, "every parameter name is unique -- --set and the sweep find them by name" );

	Check( plugin.GetNumParams() == Pilot::PT_COUNT,
	       "the host is told about all " + std::to_string( static_cast< int >( Pilot::PT_COUNT ) ) + " parameters" );

	return g_failures;
}

//---------------------------------------------------------------------------
/// --negative: perturb the model and prove the checks notice.
//---------------------------------------------------------------------------
int runNegative()
{
	std::printf( "negative: the checks fail when the model is wrong\n\n" );

	int notCaught = 0;

	/// Run a check body against a broken model, silently, and report only
	/// whether it noticed. The failures it raises are the POINT, so they are
	/// rolled back afterwards and one check -- "did it notice?" -- is recorded
	/// in their place.
	auto expectFailure = [ & ]( const char* what, const std::function< void() >& body ) {
		const int beforeFailures = g_failures;
		const int beforeChecks   = g_checks;

		g_quiet = true;
		body();
		g_quiet = false;

		const int raised = g_failures - beforeFailures;
		g_failures       = beforeFailures;
		g_checks         = beforeChecks;

		++g_checks;
		if( raised == 0 )
		{
			++g_failures;
			++notCaught;
		}
		std::printf( "   %s  %s raises %d failure(s)\n", raised > 0 ? "ok  " : "FAIL", what, raised );
	};

	expectFailure( "--agree with line and character row swapped",
	               [] { runAgree( zx::kSwapLineCharrow ); } );
	expectFailure( "--agree with charrow*33 instead of *32",
	               [] { runAgree( zx::kOffByOneRow ); } );
	expectFailure( "--agree with a plain top-down order",
	               [] { runAgree( zx::kLinearOrder ); } );
	expectFailure( "--order with line and character row swapped",
	               [] { runOrder( zx::kSwapLineCharrow ); } );
	expectFailure( "--order with charrow*33 instead of *32",
	               [] { runOrder( zx::kOffByOneRow ); } );
	expectFailure( "--thirds with a plain top-down order",
	               [] { runThirds( zx::kLinearOrder ); } );
	expectFailure( "--border with the model driven 1% off the stated baud",
	               [] { runBorder( 1.01 ); } );
	expectFailure( "--border with the model driven 0.1% off the stated baud",
	               [] { runBorder( 1.001 ); } );
	expectFailure( "--error with the draw made 15% off the stated rate",
	               [] { runError( 1.15 ); } );
	expectFailure( "--error with the draw made 3% off the stated rate",
	               [] { runError( 1.03 ); } );

	std::printf( "\n  %s\n", notCaught == 0 ? "every check can fail" : "SOME CHECKS CANNOT FAIL" );
	return g_failures;
}

//===========================================================================
// GL checks.
//===========================================================================

//---------------------------------------------------------------------------
/// --reveal: the rendered frame IS the order table, at two rasters.
//---------------------------------------------------------------------------
/// A black input makes every cell flat and dark, so every revealed pixel is the
/// default ink and every unrevealed one is the default paper: the frame is a
/// direct binary readout of the reveal mask. Setting Ink to black and Paper to
/// white makes that readout unambiguous at eight bits.
struct RevealResult
{
	int probed = 0;
	int unresolved = 0;
	int wrong = 0;
};

RevealResult revealAt( int w, int h, float borderWidth, int bytesWanted, const std::vector< zx::Address >& order )
{
	RevealResult out;

	Target target;
	if( !target.Create( w, h ) )
		return out;

	const std::vector< unsigned char > picture = buildFlat( w, h, 0.0f );
	const GLuint input                         = makeInput( picture, w, h );

	Instance i( w, h );
	i.quiet();
	i.set( Pilot::PT_PILOT_LENGTH, 0.0f );//no pilot: Progress maps straight onto the tape
	i.set( Pilot::PT_ERROR_RATE, 0.0f );
	i.set( Pilot::PT_BORDER_WIDTH, borderWidth );
	i.set( Pilot::PT_BACKGROUND, float( Pilot::kBackgroundPaper ) );
	i.set( Pilot::PT_INK, 0.0f );  //Black
	i.set( Pilot::PT_PAPER, 7.0f );//White
	i.set( Pilot::PT_BRIGHT, 0.0f );//Off: the default attribute is not bright
	i.set( Pilot::PT_MIX, 1.0f );

	// Ask for a byte count by name rather than by arithmetic on Progress: the
	// plugin's own state is what the shader will use, so it is what the
	// expectation must be built from.
	const float progress = ( bytesWanted + 0.5f ) / float( zx::kTotalBytes );
	i.set( Pilot::PT_PROGRESS, progress );
	i.plugin.SetTime( 0.0 );
	const int bytes = i.plugin.TapeStateForTest().bytesRevealed;

	const Image img = render( i, target, input, w, h, 0.0 );

	const Mapping map{ w, h, controls::BorderInset( borderWidth ) };

	// What the table says, expanded to pixels.
	std::vector< char > expected( zx::kScreenW * zx::kScreenH, 0 );
	for( int k = 0; k < std::min( bytes, zx::kDisplayBytes ); ++k )
		for( int b = 0; b < 8; ++b )
			expected[ order[ k ].py * zx::kScreenW + order[ k ].xbyte * 8 + b ] = 1;

	const zx::Rgb ink   = zx::Colour( 0, false );
	const zx::Rgb paper = zx::Colour( 7, false );

	for( int py = 0; py < zx::kScreenH; ++py )
	{
		for( int px = 0; px < zx::kScreenW; ++px )
		{
			int ox = 0, oyTop = 0;
			map.probeFor( px, py, ox, oyTop );

			// The probe is VALIDATED, not assumed: invert the shader's own
			// mapping and check it lands on the pixel it was aimed at. At a
			// raster too coarse to resolve 256x192 some Spectrum pixels have no
			// output pixel of their own, and this is what says so.
			int gotX = 0, gotY = 0;
			if( !map.toSpectrum( ox, oyTop, gotX, gotY ) || gotX != px || gotY != py )
			{
				++out.unresolved;
				continue;
			}

			++out.probed;
			const bool want = expected[ py * zx::kScreenW + px ] != 0;
			const zx::Rgb c = want ? ink : paper;
			if( !img.is( ox, oyTop, c.r, c.g, c.b ) )
				++out.wrong;
		}
	}

	glDeleteTextures( 1, &input );
	target.Destroy();
	return out;
}

int runReveal()
{
	std::printf( "reveal: the rendered frame against the order table\n\n" );

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::printf( "   FAIL  could not create an OpenGL 4.1 core context\n" );
		return ++g_failures;
	}
	std::printf( "  GL %s / %s\n\n", glGetString( GL_VERSION ), glGetString( GL_RENDERER ) );

	const std::vector< zx::Address > order = zx::ScreenOrder();

	// Two rasters on purpose, one of them small enough to be a CI runner. A
	// thresholded position measurement quantises to whole cells and can pass at
	// one raster while failing at another by half a cell; running it once tells
	// you nothing about that.
	for( auto size : { std::pair< int, int >{ 1920, 1080 }, std::pair< int, int >{ 640, 480 } } )
	{
		for( int bytes : { 0, 1024, 3072, 6143, 6144, 6912 } )
		{
			const RevealResult r = revealAt( size.first, size.second, 0.32f, bytes, order );
			Check( r.unresolved == 0 && r.wrong == 0 && r.probed > 0,
			       std::to_string( size.first ) + "x" + std::to_string( size.second ) + " at " + std::to_string( bytes )
			           + " bytes: " + std::to_string( r.probed ) + " probed, " + std::to_string( r.unresolved )
			           + " unresolved, " + std::to_string( r.wrong ) + " wrong" );
		}
	}

	//---------------------------------------------------------------------
	// The probe validator itself, proved to fire. At 256x192 with a border
	// taken off each edge there are fewer output columns than Spectrum columns,
	// so some Spectrum pixels genuinely have no pixel of their own -- and a
	// check that quietly measured the neighbour instead would pass.
	//---------------------------------------------------------------------
	const RevealResult coarse = revealAt( 256, 192, 0.32f, 3072, order );
	Check( coarse.unresolved > 0,
	       "the probe validator fires at 256x192, where the grid cannot be resolved (unresolved: "
	           + std::to_string( coarse.unresolved ) + " of 49152)" );

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return g_failures;
}

//---------------------------------------------------------------------------
/// --pixels: the attribute model, the border and the message, at two rasters.
//---------------------------------------------------------------------------
int pixelsAt( int w, int h )
{
	std::printf( "\n  %dx%d\n", w, h );

	Target target;
	if( !target.Create( w, h ) )
	{
		Check( false, "  could not create a target" );
		return g_failures;
	}

	const std::vector< unsigned char > picture = buildQuads( w, h );
	const GLuint input                         = makeInput( picture, w, h );

	const float borderWidth = 0.32f;
	const Mapping map{ w, h, controls::BorderInset( borderWidth ) };

	auto probe = [ & ]( int px, int py, int& ox, int& oyTop ) {
		map.probeFor( px, py, ox, oyTop );
		int gx = 0, gy = 0;
		const bool landed = map.toSpectrum( ox, oyTop, gx, gy ) && gx == px && gy == py;
		Check( landed, "  a probe aimed at Spectrum (" + std::to_string( px ) + ", " + std::to_string( py ) + ") lands there" );
	};

	//-------------------------------------------------------------------
	// Colour arrives last. At 6143 bytes every pixel is on the screen and no
	// attribute has arrived, so the whole picture must be two colours.
	//-------------------------------------------------------------------
	{
		Instance i( w, h );
		i.quiet();
		i.set( Pilot::PT_PILOT_LENGTH, 0.0f );
		i.set( Pilot::PT_BORDER_WIDTH, borderWidth );
		i.set( Pilot::PT_INK, 0.0f );
		i.set( Pilot::PT_PAPER, 7.0f );
		i.set( Pilot::PT_BRIGHT, 0.0f );
		i.set( Pilot::PT_PROGRESS, ( zx::kDisplayBytes - 0.5f ) / zx::kTotalBytes );
		i.plugin.SetTime( 0.0 );

		const int bytes = i.plugin.TapeStateForTest().bytesRevealed;
		Check( bytes == zx::kDisplayBytes - 1, "  6143 bytes in, got " + std::to_string( bytes ) );

		const Image img = render( i, target, input, w, h, 0.0 );

		int offPalette = 0;
		for( int py = 0; py < zx::kScreenH; py += 3 )
			for( int px = 0; px < zx::kScreenW; px += 3 )
			{
				int ox = 0, oyTop = 0;
				map.probeFor( px, py, ox, oyTop );
				int gx = 0, gy = 0;
				if( !map.toSpectrum( ox, oyTop, gx, gy ) || gx != px || gy != py )
					continue;
				if( !img.is( ox, oyTop, 0, 0, 0 ) && !img.is( ox, oyTop, 215, 215, 215 ) )
					++offPalette;
			}
		Check( offPalette == 0,
		       "  before 6144 bytes every screen pixel is the default ink or paper (off: " + std::to_string( offPalette ) + ")" );
	}

	//-------------------------------------------------------------------
	// ...and then it does arrive. The bottom-left quadrant of the card is
	// saturated red, which at the bright level is the Spectrum's own red.
	//-------------------------------------------------------------------
	{
		Instance i( w, h );
		i.quiet();
		i.set( Pilot::PT_PILOT_LENGTH, 0.0f );
		i.set( Pilot::PT_BORDER_WIDTH, borderWidth );
		i.set( Pilot::PT_BRIGHT, 1.0f );//Auto
		i.set( Pilot::PT_PROGRESS, 1.0f );
		i.plugin.SetTime( 0.0 );

		Check( i.plugin.TapeStateForTest().bytesRevealed == zx::kTotalBytes, "  the whole tape is in" );

		const Image img = render( i, target, input, w, h, 0.0 );

		// The middle of each quadrant: a long way from any boundary, so the
		// probe means the same thing at both rasters.
		int ox = 0, oyTop = 0;
		probe( 64, 144, ox, oyTop );
		Check( img.is( ox, oyTop, 255, 0, 0 ), "  the red quadrant is bright red (255, 0, 0), got " + img.str( ox, oyTop ) );

		probe( 192, 144, ox, oyTop );
		Check( img.is( ox, oyTop, 0, 0, 255 ), "  the blue quadrant is bright blue (0, 0, 255), got " + img.str( ox, oyTop ) );

		probe( 64, 48, ox, oyTop );
		Check( img.is( ox, oyTop, 0, 0, 0 ), "  the black quadrant is black, got " + img.str( ox, oyTop ) );

		probe( 192, 48, ox, oyTop );
		Check( img.is( ox, oyTop, 255, 255, 255 ), "  the white quadrant is bright white, got " + img.str( ox, oyTop ) );
	}

	//-------------------------------------------------------------------
	// The border. What is checked is that the rendered stripe agrees with the
	// phase the model predicts for THAT ROW -- which is the raster-sensitive
	// part, and the reason this whole function runs twice.
	//-------------------------------------------------------------------
	{
		Instance i( w, h );
		i.quiet();
		i.set( Pilot::PT_PILOT_LENGTH, 0.0f );//data, not pilot: yellow and blue
		i.set( Pilot::PT_BORDER_WIDTH, borderWidth );
		i.set( Pilot::PT_PROGRESS, 0.5f );
		i.set( Pilot::PT_TYPE, 0.0f );//ZX 48

		const double seconds = 1.0;
		const Image img      = render( i, target, input, w, h, seconds );

		const MachineSpec& spec = machine( 0 );
		const double baud       = controls::Baud( spec.nominalBaud, 0.5f );
		const load::Border b    = load::BorderAt( seconds, baud, spec.frameHz );
		double reduced          = std::fmod( load::HalfCycleContinuous( b, 0.0 ), 2.0 );
		if( reduced < 0.0 )
			reduced += 2.0;
		const double span = b.bitsPerFrame * 2.0;

		const zx::Rgb a = zx::Colour( spec.dataA, spec.bright );
		const zx::Rgb c = zx::Colour( spec.dataB, spec.bright );

		//---------------------------------------------------------------
		// Every border row against the phase the model predicts for THAT row.
		//
		// A row whose phase sits on a half-cycle boundary is genuinely
		// undecidable: the CPU and the GPU evaluate the same sum in float from
		// different operand orders, and near an integer that can put the floor
		// either side. The allowance is 1e-5 half-cycles, which is about three
		// times the worst rounding of a sum whose largest term is `span`
		// (60 half-cycles times a float ulp of 6e-8 is 3.6e-6) and four orders
		// of magnitude tighter than the half-cycle it would take to actually
		// get a stripe wrong. Rows inside it are counted, not ignored.
		//---------------------------------------------------------------
		int wrong     = 0;
		int ambiguous = 0;
		int rows      = 0;
		const int ox  = 1;//the leftmost column, which is always border
		for( int oyTop = 0; oyTop < h; ++oyTop )
		{
			const double rowFromTop = 1.0 - ( static_cast< double >( h - 1 - oyTop ) + 0.5 ) / h;
			const double phase      = reduced + rowFromTop * span;
			if( std::fabs( phase - std::floor( phase + 0.5 ) ) < 1e-5 )
			{
				++ambiguous;
				continue;
			}
			const bool odd     = ( static_cast< long long >( std::floor( phase ) ) & 1 ) != 0;
			const zx::Rgb want = odd ? a : c;
			++rows;
			if( !img.is( ox, oyTop, want.r, want.g, want.b ) )
				++wrong;
		}
		Check( wrong == 0, "  every one of the " + std::to_string( rows ) + " decidable border rows is the stripe the model predicts (wrong: "
		                       + std::to_string( wrong ) + ", on a boundary: " + std::to_string( ambiguous ) + ")" );

		// ...and the stripes are actually there. A border that came out one
		// colour would pass the row-by-row check trivially if the phase never
		// crossed an integer.
		//
		// The expectation is not `2 * bitsPerFrame`: comparing neighbouring
		// rows can only see the span between the first and last row CENTRE,
		// which is (h-1)/h of the frame. The allowance is one edge, because
		// that is what a floor at each end of that span can lose.
		int toggles = 0;
		for( int oyTop = 1; oyTop < h; ++oyTop )
			if( std::memcmp( img.at( ox, oyTop ), img.at( ox, oyTop - 1 ), 3 ) != 0 )
				++toggles;
		const double expected = 2.0 * b.bitsPerFrame * ( h - 1.0 ) / h;
		Check( std::fabs( toggles - expected ) <= 1.0,
		       "  " + std::to_string( toggles ) + " stripe edges down the frame, expected " + F( expected, 2 )
		           + " = 2 * baud/frameHz * (h-1)/h (allowance 1: one floor at each end)" );
	}

	//-------------------------------------------------------------------
	// The message, as the difference between Message On and Message Off. That
	// comparison is exact, and it is the only way to tell the message's ink
	// from the picture's -- they are the same two colours.
	//-------------------------------------------------------------------
	{
		int mx = 0, my = 0, mw = 0, mh = 0;
		Pilot::MessageBoxForTest( mx, my, mw, mh );

		// Pilot Length is left at its DEFAULT here, unlike every other block in
		// this function: the pinned context is a (Error Rate, Progress) pair,
		// and Pilot Length is what maps Progress onto the tape. Overriding it
		// moves the failure out from under the probe, which is how this check
		// first failed.
		Image on, off;
		{
			Instance i( w, h );
			i.set( Pilot::PT_BORDER_WIDTH, borderWidth );
			i.set( Pilot::PT_ERROR_RATE, kSweepErrorRate );
			i.set( Pilot::PT_PROGRESS, kSweepProgress );
			i.set( Pilot::PT_MESSAGE, 1.0f );
			i.plugin.SetTime( 0.0 );
			Check( i.plugin.TapeStateForTest().message, "  the message is showing at the pinned context" );
			on = render( i, target, input, w, h, 0.0 );
		}
		{
			Instance i( w, h );
			i.set( Pilot::PT_BORDER_WIDTH, borderWidth );
			i.set( Pilot::PT_ERROR_RATE, kSweepErrorRate );
			i.set( Pilot::PT_PROGRESS, kSweepProgress );
			i.set( Pilot::PT_MESSAGE, 0.0f );
			i.plugin.SetTime( 0.0 );
			off = render( i, target, input, w, h, 0.0 );
		}

		int outside = 0;
		int litSeen = 0;
		for( int py = 0; py < zx::kScreenH; py += 2 )
			for( int px = 0; px < zx::kScreenW; px += 2 )
			{
				int ox = 0, oyTop = 0;
				map.probeFor( px, py, ox, oyTop );
				int gx = 0, gy = 0;
				if( !map.toSpectrum( ox, oyTop, gx, gy ) || gx != px || gy != py )
					continue;

				const bool inBox   = px >= mx && px < mx + mw && py >= my && py < my + mh;
				const bool changed = std::memcmp( on.at( ox, oyTop ), off.at( ox, oyTop ), 3 ) != 0;
				if( changed && !inBox )
					++outside;
				if( changed && inBox )
					++litSeen;
			}
		Check( outside == 0, "  Message On changes nothing outside the message box (changed: " + std::to_string( outside ) + ")" );
		Check( litSeen > 0, "  ...and does change something inside it (" + std::to_string( litSeen ) + " probes)" );
	}

	glDeleteTextures( 1, &input );
	target.Destroy();
	return g_failures;
}

int runPixels()
{
	std::printf( "pixels: the attribute model, the border and the message\n" );

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::printf( "   FAIL  could not create an OpenGL 4.1 core context\n" );
		return ++g_failures;
	}
	std::printf( "\n  GL %s / %s\n", glGetString( GL_VERSION ), glGetString( GL_RENDERER ) );

	pixelsAt( 1920, 1080 );
	pixelsAt( 640, 480 );

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return g_failures;
}

//---------------------------------------------------------------------------
/// --bench: the render cost, for the record.
//---------------------------------------------------------------------------
int runBench( int frames )
{
	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::printf( "bench: could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	std::printf( "bench: %s / %s\n\n", glGetString( GL_VERSION ), glGetString( GL_RENDERER ) );
	std::printf( "  %-12s %10s %10s\n", "size", "ms/frame", "%% of 60fps" );

	for( auto size : { std::pair< int, int >{ 1280, 720 },
	                   std::pair< int, int >{ 1920, 1080 },
	                   std::pair< int, int >{ 3840, 2160 } } )
	{
		Target target;
		target.Create( size.first, size.second );
		const std::vector< unsigned char > picture = buildCard( size.first, size.second );
		const GLuint input                         = makeInput( picture, size.first, size.second );

		Instance i( size.first, size.second );
		i.set( Pilot::PT_PROGRESS, 0.6f );

		// glFinish on both sides, or this times how fast the driver accepts
		// commands rather than how fast the GPU runs them.
		for( int f = 0; f < 20; ++f )
			render( i, target, input, size.first, size.second, f / 60.0 );
		glFinish();

		const auto start = std::chrono::steady_clock::now();
		for( int f = 0; f < frames; ++f )
			render( i, target, input, size.first, size.second, ( 20 + f ) / 60.0 );
		glFinish();
		const double ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() / frames;

		std::printf( "  %4dx%-7d %10s %9s%%\n", size.first, size.second, F( ms, 3 ).c_str(), F( ms / 16.667 * 100.0, 1 ).c_str() );

		glDeleteTextures( 1, &input );
		target.Destroy();
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return 0;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"pttest -- render the pilot chain, and check what it claims\n"
		"\n"
		"  --out PATH        write a PNG (default /tmp/pilot.png)\n"
		"  --size WxH        output size (default 1920x1080)\n"
		"  --frames N        frames to run before capturing (default 5)\n"
		"  --set \"Name=V\"    set a parameter by its display name\n"
		"  --flat V          a uniform field at level V instead of the card\n"
		"  --quads           the four-quadrant probe picture instead of the card\n"
		"  --list            print every parameter and its default, then exit\n"
		"\n"
		"  checks that need no GL context at all:\n"
		"  --agree           the two address derivations agree, bitwise\n"
		"  --order           the revealed set is the first n addresses\n"
		"  --thirds          the interleave is present, thirds in order\n"
		"  --attributes      no colour before 6144 bytes\n"
		"  --border          the stripe period is the byte period, at two rasters\n"
		"  --error           blocks before a failure, against 1/r\n"
		"  --message         the sweep's Message On context still fails a block\n"
		"  --clock           the border survives a six-day host clock\n"
		"  --names           nothing the host will silently truncate\n"
		"  --negative        perturb the model and prove the checks fail\n"
		"\n"
		"  checks that render:\n"
		"  --reveal          the rendered frame against the order table, two rasters\n"
		"  --pixels          attributes, border and message, two rasters\n"
		"  --bench           720p, 1080p and 4K\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outputPath = "/tmp/pilot.png";
	int width = 1920, height = 1080;
	int frames = 5;
	int benchFrames = 120;
	float flatLevel = -1.0f;
	bool quads = false;
	std::vector< std::pair< std::string, float > > overrides;
	std::vector< std::string > modes;

	for( int i = 1; i < argc; ++i )
	{
		const std::string arg = argv[ i ];
		auto next             = [ & ]() -> std::string { return i + 1 < argc ? argv[ ++i ] : std::string(); };

		if( arg == "--out" )
			outputPath = next();
		else if( arg == "--size" )
		{
			const std::string s = next();
			const size_t x      = s.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "pttest: --size wants WxH, got '%s'\n", s.c_str() );
				return 2;
			}
			width  = std::atoi( s.substr( 0, x ).c_str() );
			height = std::atoi( s.substr( x + 1 ).c_str() );
		}
		else if( arg == "--frames" )
			frames = std::atoi( next().c_str() );
		else if( arg == "--bench-frames" )
			benchFrames = std::atoi( next().c_str() );
		else if( arg == "--flat" )
			flatLevel = std::strtof( next().c_str(), nullptr );
		else if( arg == "--quads" )
			quads = true;
		else if( arg == "--set" )
		{
			const std::string assignment = next();
			const size_t equals          = assignment.rfind( '=' );
			if( equals == std::string::npos )
			{
				std::fprintf( stderr, "pttest: --set wants Name=Value, got '%s'\n", assignment.c_str() );
				return 2;
			}
			overrides.emplace_back( assignment.substr( 0, equals ),
			                        std::strtof( assignment.substr( equals + 1 ).c_str(), nullptr ) );
		}
		else if( arg == "--help" || arg == "-h" )
		{
			usage();
			return 0;
		}
		else if( arg.rfind( "--", 0 ) == 0 )
			modes.push_back( arg );
		else
		{
			std::fprintf( stderr, "pttest: unknown argument '%s'\n", arg.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 )
	{
		std::fprintf( stderr, "pttest: width, height and frames must all be positive\n" );
		return 2;
	}

	//-----------------------------------------------------------------------
	// The checks that need no context run FIRST and return before one is made,
	// so they still work on a machine that cannot create one at all.
	//-----------------------------------------------------------------------
	if( !modes.empty() )
	{
		bool ranSomething = false;
		bool needGL       = false;
		for( const std::string& mode : modes )
		{
			if( mode == "--agree" )       { runAgree( zx::kExact ); ranSomething = true; }
			else if( mode == "--order" )  { runOrder( zx::kExact ); ranSomething = true; }
			else if( mode == "--thirds" ) { runThirds( zx::kExact ); ranSomething = true; }
			else if( mode == "--attributes" ) { runAttributes(); ranSomething = true; }
			else if( mode == "--border" ) { runBorder( 1.0 ); ranSomething = true; }
			else if( mode == "--error" )  { runError( 1.0 ); ranSomething = true; }
			else if( mode == "--message" ){ runMessage(); ranSomething = true; }
			else if( mode == "--clock" )  { runClock(); ranSomething = true; }
			else if( mode == "--names" )  { runNames(); ranSomething = true; }
			else if( mode == "--negative" ) { runNegative(); ranSomething = true; }
			else if( mode == "--reveal" || mode == "--pixels" || mode == "--bench" || mode == "--list" )
				needGL = true;
			else
			{
				std::fprintf( stderr, "pttest: unknown mode '%s'\n", mode.c_str() );
				return 2;
			}
			if( ranSomething )
				std::printf( "\n" );
		}

		if( ranSomething && !needGL )
		{
			std::printf( "%d checks, %d failed\n", g_checks, g_failures );
			return g_failures == 0 ? 0 : 1;
		}

		for( const std::string& mode : modes )
		{
			if( mode == "--list" )
			{
				Pilot plugin;
				for( unsigned int p = 0; p < plugin.GetNumParams(); ++p )
					std::printf( "%2u  %-20s %.3f\n", p, plugin.GetParamName( p ), plugin.GetFloatParameter( p ) );
				return 0;
			}
		}

		for( const std::string& mode : modes )
		{
			if( mode == "--reveal" )
				runReveal();
			else if( mode == "--pixels" )
				runPixels();
			else if( mode == "--bench" )
				return runBench( benchFrames );
		}

		std::printf( "\n%d checks, %d failed\n", g_checks, g_failures );
		return g_failures == 0 ? 0 : 1;
	}

	//-----------------------------------------------------------------------
	// Render a frame.
	//-----------------------------------------------------------------------
	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "pttest: could not create an OpenGL 4.1 core context\n" );
		return 1;
	}
	std::printf( "GL %s / %s\n", glGetString( GL_VERSION ), glGetString( GL_RENDERER ) );

	Target target;
	if( !target.Create( width, height ) )
	{
		std::fprintf( stderr, "pttest: output framebuffer is incomplete\n" );
		return 1;
	}

	const std::vector< unsigned char > picture = flatLevel >= 0.0f ? buildFlat( width, height, flatLevel )
	                                            : quads            ? buildQuads( width, height )
	                                                               : buildCard( width, height );
	const GLuint input = makeInput( picture, width, height );

	Instance instance( width, height );
	if( !instance.ok )
		return 1;

	for( const auto& o : overrides )
	{
		int index = -1;
		for( unsigned int p = 0; p < instance.plugin.GetNumParams(); ++p )
		{
			const char* declared = instance.plugin.GetParamName( p );
			if( declared != nullptr && o.first == declared )
				index = static_cast< int >( p );
		}
		if( index < 0 )
		{
			std::fprintf( stderr, "pttest: no parameter named '%s' (try --list)\n", o.first.c_str() );
			return 2;
		}
		instance.plugin.SetFloatParameter( static_cast< unsigned int >( index ), o.second );
	}

	Image img;
	for( int f = 0; f < frames; ++f )
		img = render( instance, target, input, width, height, f / 60.0 );

	const GLenum error = glGetError();
	if( error != GL_NO_ERROR )
		std::fprintf( stderr, "pttest: GL error 0x%04x during render\n", error );

	//The plugin outputs premultiplied alpha, so the colour is already the
	//over-black composite. Flattening is a matter of forcing alpha opaque.
	for( size_t i = 3; i < img.px.size(); i += 4 )
		img.px[ i ] = 255;

	if( !writePng( outputPath, width, height, img.px ) )
	{
		std::fprintf( stderr, "pttest: could not write %s\n", outputPath.c_str() );
		return 1;
	}
	std::printf( "wrote %s (%dx%d, %d frames)\n", outputPath.c_str(), width, height, frames );

	glDeleteTextures( 1, &input );
	target.Destroy();
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return 0;
}
