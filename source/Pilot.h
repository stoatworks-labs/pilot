#pragma once

#include <FFGLSDK.h>

#include <chrono>
#include <string>
#include <vector>

#include "Loader.h"
#include "PassBuffer.h"
#include "StoatworksAboutParams.h"

/**
	pilot — a tape loader, as an FFGL effect.

	The clip arrives the way a ZX Spectrum tape loads it: in screen-memory
	order, at the baud rate, with the border painted by the loading signal
	itself.

	The Spectrum's display file is not linear, and that is the whole visual
	idea. The picture does not wipe down the screen — it arrives in three
	thirds, each of which fills in an eight-line interleave, so the image
	assembles in the venetian-blind pattern. Colour does not arrive with it: the
	768-byte attribute file comes last, so the picture lands in monochrome and
	colours in at the very end.

	See `Spectrum.h` for the address order (written twice, and checked),
	`Loader.h` for the tape, `Shaders.h` for the three passes, and AGENTS.md for
	the traps.
*/
class Pilot : public CFFGLPlugin
{
public:
	Pilot();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	char* GetTextParameter( unsigned int index ) override;
	float GetFloatParameter( unsigned int index ) override;

	FFResult SetTime( double time ) override;

	/// Everything the operator can reach, in the order Resolume shows them.
	/// Public because the harness names them: it drives the real class, and
	/// referring to a control by its id is one fewer thing to get out of step
	/// than referring to it by a string.
	enum ParamID : FFUInt32
	{
		//Machine
		PT_TYPE,
		PT_BAUD,
		PT_INK,
		PT_PAPER,
		PT_BRIGHT,

		//Load
		PT_PROGRESS,
		PT_SYNC,
		PT_ERROR_RATE,
		PT_MESSAGE,

		//Border
		PT_BORDER_ON,
		PT_BORDER_WIDTH,
		PT_PILOT_LENGTH,

		//Output
		PT_MIX,
		PT_BACKGROUND,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. See StoatworksAboutParams.h.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	enum SyncMode
	{
		kSyncManual = 0,
		kSyncClip   = 1,
		kSyncBeat   = 2,
		kSyncBar    = 3,
	};

	enum BackgroundMode
	{
		kBackgroundPaper = 0,
		kBackgroundBlack = 1,
		kBackgroundClip  = 2,
	};

	//-----------------------------------------------------------------------
	// Test hooks.
	//
	// The harness DECLARES the unit its clock is in rather than leaving
	// elapsedSeconds() to infer one. An absolute time in a single frame is
	// genuinely ambiguous — Resolume sends milliseconds, this repo's harness
	// sends seconds — and an implicit unit is exactly how a thousand-times-fast
	// bug hides in a sibling repo for a month.
	//-----------------------------------------------------------------------
	void ForceSecondsClock();

	/// The tape state the plugin would render at this instant, without
	/// rendering. `pttest` steps this with no GL context at all, which is what
	/// keeps the order, border and error checks free of a rasteriser.
	pilot::load::State TapeStateForTest();

	/// The progress the Sync mode has arrived at, given the clock.
	float EffectiveProgressForTest();

	/// The border the plugin would paint at this instant.
	pilot::load::Border BorderForTest();

	/// Where the message bitmap lives in Spectrum pixels, and how big it is.
	static void MessageBoxForTest( int& x, int& y, int& w, int& h );

private:
	bool compileShaders();
	void releaseBuffers();
	bool buildMessageTexture();

	/// Seconds to drive the sync modes and the border with. Double throughout:
	/// Resolume's clock is milliseconds since the composition opened, which
	/// passes 499 million — where a float resolves only about 0.03 s — after
	/// about six days of uptime. Nothing here ever puts that number in a float.
	double elapsedSeconds();

	/// What the load has got to, with the Sync mode applied.
	float effectiveProgress( double seconds ) const;

	ffglex::FFGLShader rasterShader;
	ffglex::FFGLShader attrShader;
	ffglex::FFGLShader composeShader;
	ffglex::FFGLScreenQuad quad;

	pilot::PassBuffer rasterBuffer;//the clip on the Spectrum's 256x192 grid
	pilot::PassBuffer attrBuffer;  //one texel per 8x8 cell: ink, paper, bright, threshold

	GLuint messageTexture = 0;

	double clockScale   = 0.0;///< 0 until decided; then 1.0 or 0.001
	double lastRawTime  = -1.0;
	double lastWallTime = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	bool hostTimeSeen   = false;
	std::chrono::steady_clock::time_point startTime;

	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
