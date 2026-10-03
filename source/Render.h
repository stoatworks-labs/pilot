#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Frame.h"
#include "Spectrum.h"

/**
	The three passes, on the CPU. The OpenFX build renders with this.

	Everything that is not per-pixel is already shared -- the address order
	(`Spectrum.cpp`), the tape (`Loader.cpp`), the machines and the controls,
	and every uniform the passes are given (`Frame.cpp`). What is left is the
	GLSL itself, and that is what this file is: `shaders/Raster.cpp`,
	`shaders/Attr.cpp` and `shaders/Compose.cpp`, statement for statement, in
	float where the shader has float. Every mirrored block is marked
	`//= mirrored:` here and beside its raw string in the shader file. **Change
	one, change the other**, then run `pttest --cpu`, which renders both and
	compares them pixel by pixel.

	It mirrors the GPU's two intermediate buffers as well as its arithmetic,
	because both of them change answers:

	  - the raster is RGBA8 on the GPU, so the clip is quantised to 1/255
	    before the attribute pass sees it, and
	  - the attribute buffer is RGBA16F, so the threshold every pixel is
	    compared against is a half float.

	Leave either out and a low-contrast cell's bit pattern moves.

	Coordinates are GL's throughout: row 0 at the BOTTOM, for the host images,
	the raster and the attribute cells alike. That is also OpenFX's own row
	order, so nothing flips on the way in or out, and the only two flips are the
	compose pass's `191 - py` and `23 - cy`, exactly as in the shader.
*/
namespace pilot::render
{
enum class Depth
{
	U8,
	U16,
	F32,
};

/// A host image, read in place. Nothing is copied: the raster pass reads
/// about one texel in every one of the source's, and the compose pass reads
/// each of the underlay's once.
struct View
{
	const void* base     = nullptr;///< pixel (0, 0), which is the BOTTOM-left
	std::ptrdiff_t rowBytes = 0;   ///< from one row to the one above it; may be negative
	int width            = 0;
	int height           = 0;
	int components       = 4;      ///< 4 for RGBA, 3 for RGB (alpha is then 1)
	Depth depth          = Depth::U8;
	bool premultiplied   = true;   ///< false: straight colour, multiplied up on the way in

	bool valid() const
	{
		return base != nullptr && width > 0 && height > 0;
	}
};

/// One texel as premultiplied float RGBA, clamped to the edge.
void Texel( const View& image, int x, int y, float out[ 4 ] );

/// What `texture()` returns from a GL_LINEAR, CLAMP_TO_EDGE texture at
/// (s, t), both 0..1 across the image.
void Bilinear( const View& image, float s, float t, float out[ 4 ] );

/**
	A float through a half float and back: what the GPU's RGBA16F attribute
	buffer does to the threshold written into it.

	Rounded toward zero, because that is what the GPU this was measured on
	does (Apple M4 Max under its Metal-backed GL 4.1: the threshold `pttest
	--cpu` reads back is, cell after cell, exactly the CPU's truncated and one
	half-float step below its round-to-nearest). OpenGL leaves the rounding of
	that conversion to the implementation, so another GPU may round to nearest
	and land one step higher -- a sliver of luma one half-float step wide
	(1/4096 for a threshold between 0.25 and 0.5), in which a pixel can take
	the other of its cell's two colours. The harness accepts either.
*/
float Half( float v );

/// The 256x192 raster as the GPU's RGBA8 buffer holds it: straight colour,
/// eight bits a channel, row 0 at the bottom.
struct Raster
{
	std::vector< uint8_t > rgba = std::vector< uint8_t >( static_cast< size_t >( zx::kScreenW ) * zx::kScreenH * 4, 0 );
};

/// One texel of the RGBA16F attribute buffer.
struct Cell
{
	float ink       = 0.0f;///< colour index 0..7
	float paper     = 0.0f;///< colour index 0..7
	float bright    = 0.0f;///< 0 or 1
	float threshold = 0.0f;///< luma; already through Half()
};

/// The 32x24 attribute buffer, row 0 at the bottom.
struct Attributes
{
	Cell cells[ zx::kCellsX * zx::kCellsY ];
};

/// Pass 1, for raster rows [rowBegin, rowEnd) counted from the bottom. Rows
/// are independent, so a host can split them across threads.
void RasterRows( const View& picture, Raster& out, int rowBegin, int rowEnd );

/**
	What one cell's decisions are made from, before they are made: the
	threshold before the RGBA16F rounding, the mean colour either side of it,
	the brightest channel, and how close the nearest pixel's luma came to the
	threshold. Split out of pass 2 so the harness can tell a real disagreement
	with the GPU from a decision that sat exactly on its edge -- a mean channel
	of exactly half the level, a peak of exactly 235/255 -- where the GPU's own
	rounding may fall the other way.
*/
struct CellWorking
{
	float threshold  = 0.0f;
	float ink[ 3 ]   = {};
	float paper[ 3 ] = {};
	float peak       = 0.0f;
	float nearest    = 0.0f;
};

/// Pass 2's arithmetic for cell (cx, cy), counted from the bottom left.
CellWorking Working( const Raster& raster, int cx, int cy );

/// Pass 2's decisions from it: the two colours, BRIGHT and the stored threshold.
Cell Decide( const CellWorking& working, int brightMode );

/// Pass 2, all 768 cells. Cheap enough that splitting it would cost more than
/// it saved.
void Attribute( const Raster& raster, int brightMode, Attributes& out );

/**
	Pass 3 at one sample point: (pX, pY) is the fragment's `uv`, 0..1 across
	the output with y up, and `clip` is what the shader's `texture(
	InputTexture, ... )` returned there (premultiplied). Writes premultiplied
	float RGBA. ComposeRow is this at every pixel centre; it is public so the
	harness can ask what a sample point a hair to either side would have shown.
*/
void ComposeAt( const frame::Uniforms& u, const Raster& raster, const Attributes& attributes,
                const float clip[ 4 ], float pX, float pY, float out[ 4 ] );

/**
	Pass 3, for output pixels [x0, x1) of row `y` (from the bottom) of an
	`outW` x `outH` frame. Writes premultiplied float RGBA, four floats a pixel,
	starting at `out` for pixel x0.

	`underlay` is what Background = Clip and Mix see. In the FFGL build -- and
	in the OpenFX filter -- that is the same picture that is loading. In the
	OpenFX transition it is the outgoing shot.
*/
void ComposeRow( const frame::Uniforms& u, const Raster& raster, const Attributes& attributes,
                 const View& underlay, int outW, int outH, int y, int x0, int x1, float* out );

/// All three passes, single-threaded, into `out` (outW x outH x 4 floats,
/// premultiplied, row 0 at the bottom). The reference the harness compares
/// against the GPU; the plugin itself splits the work.
void Frame( const frame::Uniforms& u, const View& picture, const View& underlay, int outW, int outH, float* out );

//---------------------------------------------------------------------------
// The transition's ends. OpenFX only: FFGL has no transition to give ends to.
//
// The tape alone does not start on the outgoing shot -- its first frame
// already has the pilot-tone border round it -- and does not finish on the
// incoming one: its last frame is the loaded Spectrum screen. On an NLE
// timeline both read as a glitch, a cut into the effect and a cut out of it.
// So the transition has Ends:
//
//   Fade  (the default) the tape runs over the middle of the transition, and
//         over the first and last End Length the picture crossfades -- a
//         smoothstep in premultiplied colour -- from exactly SourceFrom into
//         the effect at progress 0, and from the loaded screen into exactly
//         SourceTo. Transition 0 is SourceFrom and 1 is SourceTo.
//   Cut   the tape over the whole transition, nothing else: the raw load, as
//         the first OpenFX build had it, bit for bit.
//
// The same shape, the same names and the same defaults as lenticular's.
//---------------------------------------------------------------------------
enum class Ends
{
	Fade = 0,
	Cut  = 1,
};

constexpr float kEndLengthDefault = 0.15f;
constexpr float kEndLengthMax     = 0.5f;

/**
	Where the tape is at Transition value `transition`.

	Cut: the value itself, clamped. Fade: clamp( ( T - L ) / ( 1 - 2L ), 0, 1 ),
	so the load starts as the first ramp ends and is complete at T = 1 - L. At
	L = 0.5 the load takes no time at all and the screen goes from empty to
	loaded at T = 0.5. Arithmetic in double, from the host's double values.
*/
float TransitionProgress( double transition, Ends ends, double endLength );

/**
	How much of the effect is seen, against the plain clip, at `transition`:
	1 under Cut, and between Fade's ramps; over a ramp, a smoothstep of the
	distance to the nearer end, 0 at the end itself and 1 a whole End Length
	in, with zero slope at both. The plain clip is SourceFrom in the first half
	of the transition and SourceTo in the second.
*/
float EffectStrength( double transition, Ends ends, double endLength );

/// The plain clip for output pixels [x0, x1) of row `y`, as premultiplied
/// float RGBA -- the underlay's own fetch in ComposeRow, by itself.
void PlainRow( const View& image, int outW, int outH, int y, int x0, int x1, float* out );

/**
	The OpenFX transition, as one pure function of its inputs:

	    (from, to, transition, time, controls, ends) -> out

	`to` is the picture being loaded; `from` is what was on screen before it,
	which Background = Clip shows through every address that has not arrived
	and Mix fades against. The tape's position comes from `transition` by
	TransitionProgress -- clamped, never wrapped, so the load ends on a whole
	tape -- and under Fade the result is crossfaded with the plain clip by
	EffectStrength.
*/
void Transition( const View& from, const View& to, double transition, double seconds,
                 const frame::HostValues& controls, Ends ends, double endLength, int outW, int outH, float* out );

} // namespace pilot::render
