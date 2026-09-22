#include "../Shaders.h"

namespace pilot::shaders
{
/// The clip, down onto 256x192.
///
/// A box filter over each destination pixel's footprint, not a point sample. A
/// Spectrum screen was *authored* at 256x192; a 1080p frame point-sampled onto
/// it keeps one row in five and a half and throws the rest away, so thin detail
/// crawls in and out as it drifts across the kept rows. Averaging is the
/// closest a downsampled photograph gets to looking authored.
///
/// The whole clip is squeezed into the grid, aspect and all. That is the right
/// call for a transition: an operator putting this on a layer wants all of the
/// clip to arrive, framed by the border, not a 4:3 window onto the middle of
/// it. On a 16:9 composition a Spectrum pixel is therefore wider than it is
/// tall, exactly as it was on a widescreen television in the 1980s.
const char* const kRasterFragment = R"(#version 410 core
uniform sampler2D InputTexture;
uniform vec2 MaxUV;
uniform vec2 InputSize;
uniform vec2 TargetSize;

in vec2 uv;

out vec4 fragColor;

void main()
{
	vec2 ratio = InputSize / max( TargetSize, vec2( 1.0 ) );

	// One tap per source texel covered, capped so a 4K or 8K composition costs
	// a bounded amount. The cap only bites past an 8:1 reduction, and 8:1 onto
	// 256x192 is already a 2048x1536 source.
	ivec2 taps = ivec2( clamp( ceil( ratio ), vec2( 1.0 ), vec2( 8.0 ) ) );
	vec2 texel = MaxUV / max( InputSize, vec2( 1.0 ) );

	vec4 sum = vec4( 0.0 );
	for( int y = 0; y < taps.y; ++y )
	{
		for( int x = 0; x < taps.x; ++x )
		{
			vec2 f = ( vec2( x, y ) + 0.5 ) / vec2( taps ) - 0.5;
			sum += texture( InputTexture, uv + f * ratio * texel );
		}
	}

	vec4 color = sum / float( taps.x * taps.y );

	// Straight colour from here on. The attribute pass compares luminances, and
	// a premultiplied pixel that is dark only because it is transparent would
	// otherwise be read as a legitimately dark one and pull the cell's
	// threshold with it.
	if( color.a > 0.0 )
		color.rgb /= color.a;

	fragColor = color;
}
)";
} // namespace pilot::shaders
