#pragma once

/**
	The GLSL, one stage per file under `source/shaders/`.

	Three passes:

	  Raster   the clip, box-filtered down onto the Spectrum's 256x192 grid.
	           Runs at 256x192 no matter what the composition is, so everything
	           after it costs the same at 4K as at 720p.
	  Attr     one texel per 8x8 character cell: which two colours that cell is
	           allowed, whether it is BRIGHT, and the luma threshold that
	           decides which of the two each pixel takes. 32x24 = 768 texels.
	  Compose  the output. The address order, the reveal, the border stripes,
	           the loading-error message and the mix.

	Almost nothing is decided here that could be decided on the CPU. The byte
	count, the border phase and the error state all arrive as uniforms from
	`Loader.cpp`, which is what lets `pttest` check them without a GL context.
	What the shader owns is the thing only the shader can own: the address
	arithmetic per pixel.

	GLSL 4.10 core. Note the reserved words the fleet has been bitten by —
	`layout`, `flat`, `active`, `filter`, `input`, `output`, `sample`, `common`,
	`half`, `patch` — none of which are used as identifiers here. In particular
	the half-cycle local is `halfCycle` and not `half`.
*/
namespace pilot::shaders
{
/// Shared by every pass: draws the screen quad and scales UVs by MaxUV so the
/// same program works against a host texture with padding and against our own
/// framebuffers, which have none.
extern const char* const kVertex;

extern const char* const kRasterFragment;
extern const char* const kAttrFragment;
extern const char* const kComposeFragment;

} // namespace pilot::shaders
