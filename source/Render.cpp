#include "Render.h"

#include "Font.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pilot::render
{
namespace
{
constexpr float kLumaR = 0.299f;
constexpr float kLumaG = 0.587f;
constexpr float kLumaB = 0.114f;

/// `dot( c, vec3( 0.299, 0.587, 0.114 ) )`, in the order GLSL writes it.
inline float Luma( float r, float g, float b )
{
	return r * kLumaR + g * kLumaG + b * kLumaB;
}

/// What a GL_RGBA8 render target stores for a float: clamped, then rounded to
/// the nearest of 256 levels.
inline uint8_t Unorm8( float v )
{
	if( !( v > 0.0f ) )
		return 0;
	if( v >= 1.0f )
		return 255;
	return static_cast< uint8_t >( std::floor( v * 255.0f + 0.5f ) );
}

/// What texelFetch returns from that target.
inline float FromUnorm8( uint8_t v )
{
	return static_cast< float >( v ) / 255.0f;
}

/// GLSL mix( x, y, a ): x * ( 1 - a ) + y * a.
inline float Mix( float x, float y, float a )
{
	return x * ( 1.0f - a ) + y * a;
}
} // namespace

//---------------------------------------------------------------------------
// Reading a host image.
//---------------------------------------------------------------------------
void Texel( const View& image, int x, int y, float out[ 4 ] )
{
	x = std::clamp( x, 0, image.width - 1 );
	y = std::clamp( y, 0, image.height - 1 );

	const char* row = static_cast< const char* >( image.base ) + static_cast< std::ptrdiff_t >( y ) * image.rowBytes;
	const int n     = image.components;

	switch( image.depth )
	{
		case Depth::U8:
		{
			const uint8_t* p = reinterpret_cast< const uint8_t* >( row ) + static_cast< size_t >( x ) * n;
			out[ 0 ] = p[ 0 ] / 255.0f;
			out[ 1 ] = p[ 1 ] / 255.0f;
			out[ 2 ] = p[ 2 ] / 255.0f;
			out[ 3 ] = n == 4 ? p[ 3 ] / 255.0f : 1.0f;
			break;
		}
		case Depth::U16:
		{
			const uint16_t* p = reinterpret_cast< const uint16_t* >( row ) + static_cast< size_t >( x ) * n;
			out[ 0 ] = p[ 0 ] / 65535.0f;
			out[ 1 ] = p[ 1 ] / 65535.0f;
			out[ 2 ] = p[ 2 ] / 65535.0f;
			out[ 3 ] = n == 4 ? p[ 3 ] / 65535.0f : 1.0f;
			break;
		}
		case Depth::F32:
		{
			const float* p = reinterpret_cast< const float* >( row ) + static_cast< size_t >( x ) * n;
			out[ 0 ] = p[ 0 ];
			out[ 1 ] = p[ 1 ];
			out[ 2 ] = p[ 2 ];
			out[ 3 ] = n == 4 ? p[ 3 ] : 1.0f;
			break;
		}
	}

	//Premultiplied from here on, which is what the GPU is handed: the raster
	//pass averages premultiplied taps and only then divides alpha out.
	if( !image.premultiplied && n == 4 )
	{
		out[ 0 ] *= out[ 3 ];
		out[ 1 ] *= out[ 3 ];
		out[ 2 ] *= out[ 3 ];
	}
}

void Bilinear( const View& image, float s, float t, float out[ 4 ] )
{
	//Texel centres sit at k + 0.5, so a sample at u lies between texel
	//floor( u - 0.5 ) and the one after it.
	const float u  = s * static_cast< float >( image.width ) - 0.5f;
	const float v  = t * static_cast< float >( image.height ) - 0.5f;
	const float fu = std::floor( u );
	const float fv = std::floor( v );
	const int x0   = static_cast< int >( fu );
	const int y0   = static_cast< int >( fv );
	const float a  = u - fu;
	const float b  = v - fv;

	float t00[ 4 ];
	Texel( image, x0, y0, t00 );

	//A zero weight contributes nothing, so its texel is not fetched -- which is
	//also what keeps an inf or a NaN next door out of a float host's picture.
	float t10[ 4 ] = { 0.0f, 0.0f, 0.0f, 0.0f };
	float t01[ 4 ] = { 0.0f, 0.0f, 0.0f, 0.0f };
	float t11[ 4 ] = { 0.0f, 0.0f, 0.0f, 0.0f };
	if( a != 0.0f )
		Texel( image, x0 + 1, y0, t10 );
	if( b != 0.0f )
		Texel( image, x0, y0 + 1, t01 );
	if( a != 0.0f && b != 0.0f )
		Texel( image, x0 + 1, y0 + 1, t11 );

	const float w00 = ( 1.0f - a ) * ( 1.0f - b );
	const float w10 = a * ( 1.0f - b );
	const float w01 = ( 1.0f - a ) * b;
	const float w11 = a * b;
	for( int c = 0; c < 4; ++c )
		out[ c ] = w00 * t00[ c ] + w10 * t10[ c ] + w01 * t01[ c ] + w11 * t11[ c ];
}

float Half( float v )
{
	uint32_t bits = 0;
	std::memcpy( &bits, &v, sizeof( bits ) );

	const uint32_t sign = bits & 0x80000000u;
	const uint32_t mag  = bits & 0x7fffffffu;
	if( mag >= 0x7f800000u )
		return v;//inf and NaN pass through

	const float a = std::fabs( v );
	if( a >= 65520.0f )
		return std::copysign( 65504.0f, v );//toward zero never reaches infinity

	if( a < 6.103515625e-05f )
	{
		//A half subnormal: a fixed step of 2^-24.
		const float q = std::trunc( a * 16777216.0f ) / 16777216.0f;
		return std::copysign( q, v );
	}

	//A normal half keeps ten of the float's 23 mantissa bits, and the other
	//thirteen are simply dropped.
	const uint32_t outBits = sign | ( mag & ~0x1fffu );
	float out              = 0.0f;
	std::memcpy( &out, &outBits, sizeof( out ) );
	return out;
}

//---------------------------------------------------------------------------
//= mirrored: shaders/Raster.cpp kRasterFragment
//
// The clip, box-filtered down onto 256x192. One tap per source texel covered,
// capped at 8x8, each tap a GL_LINEAR sample; then straight colour, then the
// RGBA8 buffer's rounding.
//---------------------------------------------------------------------------
void RasterRows( const View& picture, Raster& out, int rowBegin, int rowEnd )
{
	const float targetW = static_cast< float >( zx::kScreenW );
	const float targetH = static_cast< float >( zx::kScreenH );
	const float inputW  = static_cast< float >( picture.width );
	const float inputH  = static_cast< float >( picture.height );

	const float ratioX = inputW / std::max( targetW, 1.0f );
	const float ratioY = inputH / std::max( targetH, 1.0f );
	const int tapsX    = static_cast< int >( std::clamp( std::ceil( ratioX ), 1.0f, 8.0f ) );
	const int tapsY    = static_cast< int >( std::clamp( std::ceil( ratioY ), 1.0f, 8.0f ) );
	const float texelX = 1.0f / std::max( inputW, 1.0f );
	const float texelY = 1.0f / std::max( inputH, 1.0f );
	const float taps   = static_cast< float >( tapsX * tapsY );

	rowBegin = std::max( rowBegin, 0 );
	rowEnd   = std::min( rowEnd, zx::kScreenH );

	for( int j = rowBegin; j < rowEnd; ++j )
	{
		const float uvY = ( static_cast< float >( j ) + 0.5f ) / targetH;

		for( int i = 0; i < zx::kScreenW; ++i )
		{
			const float uvX = ( static_cast< float >( i ) + 0.5f ) / targetW;

			float sum[ 4 ] = { 0.0f, 0.0f, 0.0f, 0.0f };
			for( int y = 0; y < tapsY; ++y )
			{
				const float fy = ( static_cast< float >( y ) + 0.5f ) / static_cast< float >( tapsY ) - 0.5f;
				for( int x = 0; x < tapsX; ++x )
				{
					const float fx = ( static_cast< float >( x ) + 0.5f ) / static_cast< float >( tapsX ) - 0.5f;

					float tap[ 4 ];
					Bilinear( picture, uvX + fx * ratioX * texelX, uvY + fy * ratioY * texelY, tap );
					sum[ 0 ] += tap[ 0 ];
					sum[ 1 ] += tap[ 1 ];
					sum[ 2 ] += tap[ 2 ];
					sum[ 3 ] += tap[ 3 ];
				}
			}

			float colour[ 4 ] = { sum[ 0 ] / taps, sum[ 1 ] / taps, sum[ 2 ] / taps, sum[ 3 ] / taps };

			//Straight colour from here on: the attribute pass compares
			//luminances, and a pixel dark only because it is transparent must
			//not pull its cell's threshold.
			if( colour[ 3 ] > 0.0f )
			{
				colour[ 0 ] /= colour[ 3 ];
				colour[ 1 ] /= colour[ 3 ];
				colour[ 2 ] /= colour[ 3 ];
			}

			uint8_t* px = out.rgba.data() + ( static_cast< size_t >( j ) * zx::kScreenW + i ) * 4;
			px[ 0 ]     = Unorm8( colour[ 0 ] );
			px[ 1 ]     = Unorm8( colour[ 1 ] );
			px[ 2 ]     = Unorm8( colour[ 2 ] );
			px[ 3 ]     = Unorm8( colour[ 3 ] );
		}
	}
}

//---------------------------------------------------------------------------
//= mirrored: shaders/Attr.cpp kAttrFragment
//
// One cell's two colours, its BRIGHT bit and its threshold, from the 64 raster
// pixels it covers. The threshold goes through Half() because the buffer it is
// written to is RGBA16F. Working() is the shader's two loops, Decide() its last
// eight lines; the one thing added is `nearest`, for the harness, which the
// shader has no use for and no output to put.
//---------------------------------------------------------------------------
namespace
{
//= mirrored: shaders/Attr.cpp nearestColour()
int NearestColour( const float c[ 3 ], float level )
{
	const float mid = level * 0.5f;
	return ( c[ 1 ] > mid ? 4 : 0 ) | ( c[ 0 ] > mid ? 2 : 0 ) | ( c[ 2 ] > mid ? 1 : 0 );
}
} // namespace

CellWorking Working( const Raster& raster, int cx, int cy )
{
	CellWorking w;

	const int baseX = cx * 8;
	const int baseY = cy * 8;

	auto fetch = [ & ]( int x, int y, float c[ 3 ] ) {
		const uint8_t* p = raster.rgba.data() + ( static_cast< size_t >( baseY + y ) * zx::kScreenW + baseX + x ) * 4;
		c[ 0 ]           = FromUnorm8( p[ 0 ] );
		c[ 1 ]           = FromUnorm8( p[ 1 ] );
		c[ 2 ]           = FromUnorm8( p[ 2 ] );
	};

	float lo = 2.0f;
	float hi = -1.0f;
	for( int y = 0; y < 8; ++y )
	{
		for( int x = 0; x < 8; ++x )
		{
			float c[ 3 ];
			fetch( x, y, c );
			const float l = Luma( c[ 0 ], c[ 1 ], c[ 2 ] );
			lo            = std::min( lo, l );
			hi            = std::max( hi, l );
		}
	}

	const float midpoint  = ( lo + hi ) * 0.5f;
	const float contrast  = std::clamp( ( hi - lo ) * 8.0f, 0.0f, 1.0f );
	const float threshold = Mix( 0.5f, midpoint, contrast );

	float darkSum[ 3 ]  = { 0.0f, 0.0f, 0.0f };
	float lightSum[ 3 ] = { 0.0f, 0.0f, 0.0f };
	float darkN         = 0.0f;
	float lightN        = 0.0f;
	float peak          = 0.0f;
	float nearest       = 2.0f;
	for( int y = 0; y < 8; ++y )
	{
		for( int x = 0; x < 8; ++x )
		{
			float c[ 3 ];
			fetch( x, y, c );
			peak          = std::max( peak, std::max( c[ 0 ], std::max( c[ 1 ], c[ 2 ] ) ) );
			const float l = Luma( c[ 0 ], c[ 1 ], c[ 2 ] );
			nearest       = std::min( nearest, std::fabs( l - threshold ) );
			if( l < threshold )
			{
				darkSum[ 0 ] += c[ 0 ];
				darkSum[ 1 ] += c[ 1 ];
				darkSum[ 2 ] += c[ 2 ];
				darkN += 1.0f;
			}
			else
			{
				lightSum[ 0 ] += c[ 0 ];
				lightSum[ 1 ] += c[ 1 ];
				lightSum[ 2 ] += c[ 2 ];
				lightN += 1.0f;
			}
		}
	}

	w.threshold = threshold;
	w.peak      = peak;
	w.nearest   = nearest;
	for( int c = 0; c < 3; ++c )
	{
		w.ink[ c ]   = darkN > 0.0f ? darkSum[ c ] / darkN : 0.0f;
		w.paper[ c ] = lightN > 0.0f ? lightSum[ c ] / lightN : 1.0f;
	}
	return w;
}

Cell Decide( const CellWorking& w, int brightMode )
{
	bool bright = brightMode == 2;
	if( brightMode == 1 )
		bright = w.peak > ( ( 215.0f / 255.0f ) + 1.0f ) * 0.5f;
	const float level = bright ? 1.0f : ( 215.0f / 255.0f );

	Cell cell;
	cell.ink       = static_cast< float >( NearestColour( w.ink, level ) );
	cell.paper     = static_cast< float >( NearestColour( w.paper, level ) );
	cell.bright    = bright ? 1.0f : 0.0f;
	cell.threshold = Half( w.threshold );
	return cell;
}

void Attribute( const Raster& raster, int brightMode, Attributes& out )
{
	for( int cy = 0; cy < zx::kCellsY; ++cy )
		for( int cx = 0; cx < zx::kCellsX; ++cx )
			out.cells[ cy * zx::kCellsX + cx ] = Decide( Working( raster, cx, cy ), brightMode );
}

//---------------------------------------------------------------------------
//= mirrored: shaders/Compose.cpp kComposeFragment
//
// The address order, the reveal, the border, the message and the mix. The
// address itself is zx::ScreenIndex -- the C++ statement of the layout that
// `pttest --agree` and `--reveal` already bind to the shader's copy -- so this
// file adds no fourth copy of it.
//---------------------------------------------------------------------------
namespace
{
//= mirrored: shaders/Compose.cpp zxColour()
void ZxColour( int c, bool bright, float out[ 3 ] )
{
	const float level = bright ? 1.0f : ( 215.0f / 255.0f );
	out[ 0 ]          = level * static_cast< float >( ( c >> 1 ) & 1 );
	out[ 1 ]          = level * static_cast< float >( ( c >> 2 ) & 1 );
	out[ 2 ]          = level * static_cast< float >( c & 1 );
}

void Copy3( const float in[ 3 ], float out[ 3 ] )
{
	out[ 0 ] = in[ 0 ];
	out[ 1 ] = in[ 1 ];
	out[ 2 ] = in[ 2 ];
}

//= mirrored: shaders/Compose.cpp borderColour()
const float* BorderColour( const frame::Uniforms& u, float rowFromTop )
{
	if( u.borderStyle == 1 )
		return u.borderA;//one colour for the whole border, changed once per byte

	//Non-negative by construction, as in the shader, so the parity is a mask.
	const float halfCycle = std::floor( u.borderHalfPhase + rowFromTop * u.borderHalfSpan );
	return ( static_cast< int >( halfCycle ) & 1 ) != 0 ? u.borderA : u.borderB;
}
} // namespace

void ComposeAt( const frame::Uniforms& u, const Raster& raster, const Attributes& attributes,
                const float clip[ 4 ], float pX, float pY, float out[ 4 ] )
{
	const float span   = std::max( 1.0f - 2.0f * u.inset, 1e-5f );
	const float innerX = ( pX - u.inset ) / span;
	const float innerY = ( pY - u.inset ) / span;

	float col[ 3 ];

	if( innerX < 0.0f || innerX >= 1.0f || innerY < 0.0f || innerY >= 1.0f )
	{
		Copy3( BorderColour( u, 1.0f - pY ), col );
	}
	else
	{
		const int px = std::clamp( static_cast< int >( innerX * 256.0f ), 0, 255 );
		const int py = std::clamp( static_cast< int >( ( 1.0f - innerY ) * 192.0f ), 0, 191 );
		const int cx = px >> 3;
		const int cy = py >> 3;

		//The only two flips: both buffers are stored bottom-up and the display
		//file is addressed from the top.
		const Cell& attr     = attributes.cells[ ( 23 - cy ) * zx::kCellsX + cx ];
		const uint8_t* s     = raster.rgba.data() + ( static_cast< size_t >( 191 - py ) * zx::kScreenW + px ) * 4;
		const float src[ 3 ] = { FromUnorm8( s[ 0 ] ), FromUnorm8( s[ 1 ] ), FromUnorm8( s[ 2 ] ) };

		//Which of the cell's two colours this pixel takes, decided from the
		//picture and not from whether the attribute has arrived.
		const bool isInk = Luma( src[ 0 ], src[ 1 ], src[ 2 ] ) < attr.threshold;

		const bool attrHere = ( zx::kDisplayBytes + cy * zx::kCellsX + cx ) < u.bytesRevealed;
		float ink[ 3 ];
		float paper[ 3 ];
		if( attrHere )
		{
			ZxColour( static_cast< int >( attr.ink + 0.5f ), attr.bright > 0.5f, ink );
			ZxColour( static_cast< int >( attr.paper + 0.5f ), attr.bright > 0.5f, paper );
		}
		else
		{
			Copy3( u.defaultInk, ink );
			Copy3( u.defaultPaper, paper );
		}

		if( zx::ScreenIndex( px, py ) < u.bytesRevealed )
		{
			Copy3( isInk ? ink : paper, col );
		}
		else if( u.background == frame::kBackgroundBlack )
		{
			col[ 0 ] = col[ 1 ] = col[ 2 ] = 0.0f;
		}
		else if( u.background == frame::kBackgroundClip )
		{
			col[ 0 ] = clip[ 0 ];
			col[ 1 ] = clip[ 1 ];
			col[ 2 ] = clip[ 2 ];
		}
		else
		{
			Copy3( u.defaultPaper, col );
		}

		if( u.messageOn )
		{
			const int mx = px - frame::kMessageX;
			const int my = py - frame::kMessageY;
			if( mx >= 0 && mx < frame::MessageWidth() && my >= 0 && my < font::kHeight )
				Copy3( frame::MessageBit( mx, my ) ? u.defaultInk : u.defaultPaper, col );
		}
	}

	out[ 0 ] = Mix( clip[ 0 ], col[ 0 ], u.mix );
	out[ 1 ] = Mix( clip[ 1 ], col[ 1 ], u.mix );
	out[ 2 ] = Mix( clip[ 2 ], col[ 2 ], u.mix );
	out[ 3 ] = Mix( clip[ 3 ], 1.0f, u.mix );
}

void ComposeRow( const frame::Uniforms& u, const Raster& raster, const Attributes& attributes,
                 const View& underlay, int outW, int outH, int y, int x0, int x1, float* out )
{
	const float w  = static_cast< float >( std::max( outW, 1 ) );
	const float h  = static_cast< float >( std::max( outH, 1 ) );
	const float pY = ( static_cast< float >( y ) + 0.5f ) / h;

	//At matching sizes every output pixel centre is an underlay texel centre,
	//where GL_LINEAR returns the texel itself. It is fetched directly there, so
	//a float host gets its own values back bit for bit.
	const bool sameSize = underlay.width == outW && underlay.height == outH;

	for( int x = x0; x < x1; ++x, out += 4 )
	{
		const float pX = ( static_cast< float >( x ) + 0.5f ) / w;

		float clip[ 4 ];
		if( sameSize )
			Texel( underlay, x, y, clip );
		else
			Bilinear( underlay, pX, pY, clip );

		ComposeAt( u, raster, attributes, clip, pX, pY, out );
	}
}

//---------------------------------------------------------------------------
void Frame( const frame::Uniforms& u, const View& picture, const View& underlay, int outW, int outH, float* out )
{
	Raster raster;
	RasterRows( picture, raster, 0, zx::kScreenH );

	Attributes attributes;
	Attribute( raster, u.brightMode, attributes );

	for( int y = 0; y < outH; ++y )
		ComposeRow( u, raster, attributes, underlay, outW, outH, y, 0, outW,
		            out + static_cast< size_t >( y ) * outW * 4 );
}

void Transition( const View& from, const View& to, float transition, double seconds,
                 const frame::HostValues& controls, int outW, int outH, float* out )
{
	const float progress     = std::clamp( transition, 0.0f, 1.0f );
	const frame::Uniforms u  = frame::Prepare( controls, progress, seconds );
	Frame( u, to, from, outW, outH, out );
}

} // namespace pilot::render
