#pragma once

#include "Loader.h"
#include "Machines.h"

/**
	One frame's worth of decisions, with no GL and no host SDK in sight.

	Everything the compose pass is told -- the byte count, the border's phase
	and colours, the inset, the power-on attribute, whether the message is up --
	is worked out here, from the host's controls and a time in seconds. The FFGL
	build turns the answer into uniforms; the OpenFX build hands it to the CPU
	copy of the three passes in `Render.cpp`. One copy of the arithmetic and two
	consumers, which is what stops a control meaning one thing in Resolume and
	another in Resolve.

	This file is also where the controls' defaults live, for the same reason:
	`Pilot.cpp` declares its parameters from `HostValues{}` and so does the
	OpenFX plugin.
*/
namespace pilot::frame
{
/**
	The controls, exactly as the FFGL build's `params[]` holds them: 0..1 for
	the ranged ones (see Controls.h for why), the element index as a float for
	the options, 0 or 1 for the booleans. Rounding happens in here, once, for
	both builds.

	The defaults are a tape already part way through, with the border running.
	An effect that does nothing until four sliders are moved is an effect nobody
	finds out is any good, so the first frame is a recognisable half-loaded
	Spectrum screen rather than the clip untouched. Error Rate defaults to
	nothing, because a machine that is failing before anyone has touched a
	slider is a machine nobody trusts.
*/
struct HostValues
{
	//Machine
	float type   = 0.0f;///< ZX 48
	float baud   = 0.5f;///< the machine's own rate
	float ink    = 0.0f;///< Black
	float paper  = 7.0f;///< White -- a Spectrum powers up black on white
	float bright = 1.0f;///< Auto

	//Load
	float progress  = 0.45f;
	float sync      = 0.0f;///< Manual
	float errorRate = 0.0f;
	float message   = 1.0f;

	//Border
	float borderOn    = 1.0f;
	float borderWidth = 0.32f;///< ~8% of the picture off each edge
	float pilotLength = 0.16f;///< ~8% of the Progress range

	//Output
	float mix        = 1.0f;
	float background = 0.0f;///< Paper
};

/// The Background options, in the order both builds list them.
enum BackgroundMode
{
	kBackgroundPaper = 0,
	kBackgroundBlack = 1,
	kBackgroundClip  = 2,
};

/// The machine row the Type control names.
const MachineSpec& Machine( const HostValues& host );

/// The baud the tape runs at: the machine's own rate, trimmed by Baud.
double Baud( const HostValues& host );

/// Wrap a progress into [0, 1) with floor, not a cast: a cast truncates toward
/// zero, so a host reporting a negative transport position -- which is a scrub
/// backwards, and operators do that constantly -- would run the load the wrong
/// way.
float Wrap( double raw );

/// Progress under Sync = Clip time: the tape runs at the baud rate, from the
/// Progress control as an offset, and wraps. The period is the whole tape,
/// pilot tone included.
float ClipTimeProgress( const HostValues& host, double seconds );

/// The tape at a given effective progress -- the Progress control after the
/// Sync mode has been applied, which the caller owns because only the FFGL
/// build has a beat clock.
load::State Tape( const HostValues& host, float progress );

/// The border at an instant.
load::Border Border( const HostValues& host, double seconds );

/**
	Every uniform the three passes are given, in the units they are given in.

	Floats where the shader has floats, because these are the values both
	builds actually compute with: the compose pass and its CPU copy must start
	from the same bits.
*/
struct Uniforms
{
	//Attr
	int brightMode = 1;///< 0 off, 1 auto, 2 on

	//Compose
	float inset       = 0.0f;
	int bytesRevealed = 0;
	float defaultInk[ 3 ]   = {};
	float defaultPaper[ 3 ] = {};

	int borderStyle       = 0;   ///< 0 scanned stripes, 1 whole-border flash
	float borderHalfPhase = 0.0f;///< half-cycles at the top of the frame, reduced to [0, 2)
	float borderHalfSpan  = 0.0f;///< half-cycles covered by one frame's scan
	float borderA[ 3 ]    = {};
	float borderB[ 3 ]    = {};

	bool messageOn = false;
	int background = kBackgroundPaper;
	float mix      = 1.0f;
};

/// The whole frame, from the controls, an effective progress and a time.
Uniforms Prepare( const HostValues& host, float progress, double seconds );

//---------------------------------------------------------------------------
// The loading-error report a Spectrum gives when a block will not read. The
// text is the machine's own; the font is a 5x7 bitmap, so the message is an
// exact arrangement of whole Spectrum pixels at every output size.
//
// Where it sits is in Spectrum pixels with y from the top: the bottom
// character row, inset a few pixels from the left, which is roughly where a
// Spectrum puts its reports. demo/tools/check_shaders.py reads these three
// lines by pattern and holds the browser demo to them -- keep the shape.
//---------------------------------------------------------------------------
constexpr const char* kErrorMessage = "R Tape loading error, 0:1";
constexpr int kMessageX = 4;
constexpr int kMessageY = 184;

/// The message's width in Spectrum pixels: one glyph advance per character.
int MessageWidth();

/// Is pixel (x, y) of the message lit? y from the top, both relative to the
/// message's own origin. Exactly the texture the FFGL build uploads.
bool MessageBit( int x, int y );

} // namespace pilot::frame
