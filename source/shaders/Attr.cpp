#include "../Shaders.h"

namespace pilot::shaders
{
/// The attribute file: one texel per 8x8 character cell.
///
/// A Spectrum cell gets two colours and one bit per pixel saying which of them
/// that pixel takes. Choosing those two colours is the whole of "making a
/// Spectrum picture", and it is done here, once per cell, over the 64 pixels
/// the cell covers.
///
/// Two passes over the 64:
///
///   1. the darkest and lightest luminance in the cell, which give its
///      midpoint and its contrast;
///   2. the mean colour either side of the threshold, which are the cell's ink
///      and its paper.
///
/// The threshold is `mix( 0.5, midpoint, contrast )` and that blend is the one
/// decision here worth arguing about. Using the midpoint alone is right for a
/// cell with something in it and wrong for a flat one, where every pixel sits
/// exactly on the threshold and which side they fall depends on a comparison
/// operator. Sliding a flat cell's threshold back to 0.5 makes a flat dark cell
/// all ink and a flat bright cell all paper, so the monochrome phase of the
/// load reads as a 1-bit threshold of the picture — which is what a real
/// Spectrum conversion looks like — instead of as a field of whichever colour
/// the tie-break happened to pick.
///
/// Ink is the darker group and paper the lighter, so the default attribute
/// (black ink on white paper, which is what a Spectrum powers up with) lands
/// the right way round before any attribute byte has arrived.
///
/// Output: R = ink colour index 0..7, G = paper colour index, B = the BRIGHT
/// flag, A = the threshold. RGBA16F, not RGBA8: the threshold decides a
/// per-pixel comparison, and quantising it to 1/255 would move the bit pattern
/// of every low-contrast cell.
const char* const kAttrFragment = R"(#version 410 core
uniform sampler2D RasterTexture;
uniform vec2 MaxUV;
uniform int BrightMode;//0 off, 1 auto, 2 on

in vec2 uv;

out vec4 fragColor;

const vec3 kLumaWeights = vec3( 0.299, 0.587, 0.114 );

// The eight hues are the corners of the RGB cube scaled by the level, so the
// nearest one is a per-channel comparison against half the level and no search
// is needed. Bit 2 is green, bit 1 red, bit 0 blue -- the Spectrum's own order.
int nearestColour( vec3 c, float level )
{
	float mid = level * 0.5;
	return ( c.g > mid ? 4 : 0 ) | ( c.r > mid ? 2 : 0 ) | ( c.b > mid ? 1 : 0 );
}

void main()
{
	// uv.y runs 0 at the BOTTOM of the texture, which is also the bottom of the
	// picture, and the raster texture below is stored the same way. This pass
	// therefore never flips anything: the flip to screen-top order happens once,
	// in the compose pass, where the address arithmetic needs it.
	ivec2 cell = clamp( ivec2( floor( uv * vec2( 32.0, 24.0 ) ) ), ivec2( 0 ), ivec2( 31, 23 ) );
	ivec2 base = cell * 8;

	float lo = 2.0;
	float hi = -1.0;
	for( int y = 0; y < 8; ++y )
	{
		for( int x = 0; x < 8; ++x )
		{
			float l = dot( texelFetch( RasterTexture, base + ivec2( x, y ), 0 ).rgb, kLumaWeights );
			lo = min( lo, l );
			hi = max( hi, l );
		}
	}

	float midpoint = ( lo + hi ) * 0.5;
	float contrast = clamp( ( hi - lo ) * 8.0, 0.0, 1.0 );
	float threshold = mix( 0.5, midpoint, contrast );

	vec3 darkSum = vec3( 0.0 );
	vec3 lightSum = vec3( 0.0 );
	float darkN = 0.0;
	float lightN = 0.0;
	float peak = 0.0;
	for( int y = 0; y < 8; ++y )
	{
		for( int x = 0; x < 8; ++x )
		{
			vec3 c = texelFetch( RasterTexture, base + ivec2( x, y ), 0 ).rgb;
			peak = max( peak, max( c.r, max( c.g, c.b ) ) );
			if( dot( c, kLumaWeights ) < threshold )
			{
				darkSum += c;
				darkN += 1.0;
			}
			else
			{
				lightSum += c;
				lightN += 1.0;
			}
		}
	}

	// BRIGHT is one bit for the whole cell, so it is decided by the brightest
	// thing in the cell rather than by either colour on its own. The crossover
	// sits half way between the two levels the hardware had, 0xD7 and 0xFF.
	bool bright = BrightMode == 2;
	if( BrightMode == 1 )
		bright = peak > ( ( 215.0 / 255.0 ) + 1.0 ) * 0.5;
	float level = bright ? 1.0 : ( 215.0 / 255.0 );

	vec3 inkColour   = darkN  > 0.0 ? darkSum  / darkN  : vec3( 0.0 );
	vec3 paperColour = lightN > 0.0 ? lightSum / lightN : vec3( 1.0 );

	fragColor = vec4( float( nearestColour( inkColour, level ) ),
	                  float( nearestColour( paperColour, level ) ),
	                  bright ? 1.0 : 0.0,
	                  threshold );
}
)";
} // namespace pilot::shaders
