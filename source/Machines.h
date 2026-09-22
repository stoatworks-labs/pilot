#pragma once

/**
	The machines, as four loaders rather than four computers.

	What a tape loader looked like is not really a property of the computer; it
	is a property of the *loader* — the routine that was reading the tape and
	poking the border. The 128's ROM loader is byte-identical to the 48's, so
	"ZX 128" here is what a 128K release actually shipped: a turbo loader on
	Spectrum hardware, faster and with its own colours. Only the ZX 48 row is a
	model of a specific documented routine; the other three are chosen to be
	honestly distinct and are described as such in AGENTS.md.

	A machine is a row here and nothing anywhere else.
*/
namespace pilot
{
enum BorderStyle
{
	/// The border is painted by the beam as it scans, so the signal's
	/// transitions land as horizontal bands. The Spectrum's ULA works this way
	/// and it is why the stripes are horizontal.
	kBorderScanned = 0,
	/// The whole border is one colour and changes once per byte. C64 turbo
	/// loaders did this, and it reads as a flicker rather than as stripes.
	kBorderFlash = 1,
};

struct MachineSpec
{
	const char* name;
	float nominalBaud;///< bits per second at the Baud control's centre
	float frameHz;    ///< display rate: how many bits one frame's scan covers
	BorderStyle border;
	int pilotA, pilotB;///< ZX colour indices for the pilot tone's two states
	int dataA, dataB;  ///< ...and for the data
	bool bright;       ///< are the border colours at the bright level
};

int machineCount();
const MachineSpec& machine( int index );

} // namespace pilot
