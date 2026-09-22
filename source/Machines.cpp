#include "Machines.h"

namespace pilot
{
namespace
{
/**
	The table.

	Sources, for the day somebody disputes a number:

	- **ZX 48** is the only faithful row. The 48K ROM's tape format is
	  documented: a pilot tone of 2168 T-states per half-cycle, a reset bit of
	  855 and a set bit of 1710, against a 3.5 MHz clock — which works out at
	  about 1500 bits per second averaged over typical data, and that is the
	  figure this row carries. The pilot's red/cyan and the data's yellow/blue
	  are the ROM loader's own border colours.
	- **ZX 128** is a turbo loader on Spectrum hardware, not the 128's ROM
	  (which is the 48's). 2250 baud and monochrome data stripes are typical of
	  the commercial fast loaders 128K releases shipped with.
	- **C64 turbo** is a turbo loader's whole-border flash rather than the
	  KERNAL's own routine. 2400 baud is in the range the common turbo loaders
	  ran at.
	- **Amstrad** is the CPC's 2000-baud "speedwrite" rate. The colours are
	  chosen to be distinct rather than measured.

	The frame rate is 50 Hz throughout: every one of these machines drew a 50 Hz
	picture, and it is the frame rate — not the composition's — that decides how
	many stripes fit down the screen.
*/
const MachineSpec kMachines[] = {
	//  name           baud   fps   style             pilot      data       bright
	{ "ZX 48",        1500.0f, 50.0f, kBorderScanned, 2, 5,      6, 1,      false },
	{ "ZX 128",       2250.0f, 50.0f, kBorderScanned, 3, 4,      7, 0,      false },
	{ "C64 turbo",    2400.0f, 50.0f, kBorderFlash,   0, 0,      0, 0,      false },
	{ "Amstrad",      2000.0f, 50.0f, kBorderScanned, 5, 1,      2, 6,      true },
};

constexpr int kMachineCount = int( sizeof( kMachines ) / sizeof( kMachines[ 0 ] ) );
} // namespace

int machineCount()
{
	return kMachineCount;
}

const MachineSpec& machine( int index )
{
	if( index < 0 || index >= kMachineCount )
		index = 0;
	return kMachines[ index ];
}

} // namespace pilot
