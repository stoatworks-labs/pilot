#include "../Shaders.h"

namespace pilot::shaders
{
/// The output: the address order, the reveal, the border and the message.
///
/// The one thing this pass owns that nothing else could is the address
/// arithmetic — every other decision has already been made on the CPU and
/// arrives as a uniform. `screenIndex()` here is the same expression as
/// `pilot::zx::ScreenIndex` in C++, and `pttest --reveal` renders through this
/// shader and compares the result against the independently written order
/// table, so the two cannot drift apart unnoticed.
///
/// Three coordinate systems meet here and the flips are all in one place:
///
///   uv          0..1 across the output, y = 0 at the BOTTOM (GL's own order).
///   inner       0..1 across the screen area, once the border is taken off.
///   (px, py)    Spectrum pixels, 0..255 and 0..191, y = 0 at the TOP, which
///               is the order the display file is addressed in.
///
/// The raster and attribute textures are stored bottom-up like every other GL
/// texture, so reading them from a top-down coordinate is `191 - py` and
/// `23 - cy`. Those two subtractions are the only flips in the plugin.
const char* const kComposeFragment = R"(#version 410 core
uniform sampler2D RasterTexture;
uniform sampler2D AttrTexture;
uniform sampler2D InputTexture;
uniform sampler2D MessageTexture;

uniform vec2 MaxUV;
uniform vec2 InputMaxUV;
uniform vec2 Inset;

uniform int BytesRevealed;
uniform vec3 DefaultInk;
uniform vec3 DefaultPaper;

uniform int BorderStyle;     //0 scanned stripes, 1 whole-border flash
uniform float BorderHalfPhase;//half-cycles at the top of the frame, reduced to [0, 2)
uniform float BorderHalfSpan; //half-cycles covered by one frame's scan
uniform vec3 BorderA;
uniform vec3 BorderB;

uniform int MessageOn;
uniform vec2 MessageOrigin;//Spectrum pixels, y from the top
uniform vec2 MessageSize;

uniform int Background;//0 paper, 1 black, 2 the clip
uniform float Mix;

in vec2 uv;

out vec4 fragColor;

const vec3 kLumaWeights = vec3( 0.299, 0.587, 0.114 );

/// Colour `c` (0..7) at the basic or bright level. Bit 2 green, bit 1 red,
/// bit 0 blue, which is the order the Spectrum's hardware wires the guns in.
vec3 zxColour( int c, bool bright )
{
	float level = bright ? 1.0 : ( 215.0 / 255.0 );
	return level * vec3( float( ( c >> 1 ) & 1 ), float( ( c >> 2 ) & 1 ), float( c & 1 ) );
}

/// The display file's address for a pixel. The bit layout, exactly as
/// pilot::zx::ScreenIndex states it.
int screenIndex( int px, int py )
{
	int xbyte   = px >> 3;
	int third   = py >> 6;
	int charrow = ( py >> 3 ) & 7;
	int line    = py & 7;
	return third * 2048 + line * 256 + charrow * 32 + xbyte;
}

vec3 borderColour( float rowFromTop )
{
	if( BorderStyle == 1 )
		return BorderA;//one colour for the whole border, changed once per byte

	// Both terms are non-negative -- BorderHalfPhase is reduced to [0, 2) on the
	// CPU and rowFromTop*BorderHalfSpan cannot be negative -- so the int
	// conversion and the & are well defined. GLSL leaves % and / undefined on
	// negative operands, which is why this is a mask and not a modulo.
	float halfCycle = floor( BorderHalfPhase + rowFromTop * BorderHalfSpan );
	return ( int( halfCycle ) & 1 ) != 0 ? BorderA : BorderB;
}

void main()
{
	vec2 p = uv;
	vec4 clip = texture( InputTexture, p * InputMaxUV );

	vec2 span = max( vec2( 1.0 ) - 2.0 * Inset, vec2( 1e-5 ) );
	vec2 inner = ( p - Inset ) / span;

	vec3 col;

	if( inner.x < 0.0 || inner.x >= 1.0 || inner.y < 0.0 || inner.y >= 1.0 )
	{
		col = borderColour( 1.0 - p.y );
	}
	else
	{
		int px = clamp( int( inner.x * 256.0 ), 0, 255 );
		int py = clamp( int( ( 1.0 - inner.y ) * 192.0 ), 0, 191 );
		int cx = px >> 3;
		int cy = py >> 3;

		vec4 attr = texelFetch( AttrTexture, ivec2( cx, 23 - cy ), 0 );
		vec3 src  = texelFetch( RasterTexture, ivec2( px, 191 - py ), 0 ).rgb;

		// Which of the cell's two colours this pixel takes. Decided from the
		// picture, NOT from whether the attribute has arrived: on a real tape
		// the bit pattern was settled when the screen was converted, and the
		// attribute byte only says what the two colours are.
		bool isInk = dot( src, kLumaWeights ) < attr.a;

		bool attrHere = ( 6144 + cy * 32 + cx ) < BytesRevealed;
		vec3 ink   = attrHere ? zxColour( int( attr.r + 0.5 ), attr.b > 0.5 ) : DefaultInk;
		vec3 paper = attrHere ? zxColour( int( attr.g + 0.5 ), attr.b > 0.5 ) : DefaultPaper;

		if( screenIndex( px, py ) < BytesRevealed )
		{
			col = isInk ? ink : paper;
		}
		else if( Background == 1 )
		{
			col = vec3( 0.0 );
		}
		else if( Background == 2 )
		{
			col = clip.rgb;
		}
		else
		{
			col = DefaultPaper;
		}

		if( MessageOn == 1 )
		{
			vec2 m = vec2( float( px ), float( py ) ) - MessageOrigin;
			if( m.x >= 0.0 && m.x < MessageSize.x && m.y >= 0.0 && m.y < MessageSize.y )
			{
				// The message texture is uploaded top row first, so no flip.
				float lit = texelFetch( MessageTexture, ivec2( int( m.x ), int( m.y ) ), 0 ).r;
				col = lit > 0.5 ? DefaultInk : DefaultPaper;
			}
		}
	}

	fragColor = vec4( mix( clip.rgb, col, Mix ), mix( clip.a, 1.0, Mix ) );
}
)";
} // namespace pilot::shaders
