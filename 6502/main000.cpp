// no output, just to see minimal prg size for fun
// main000.prg is just 5591 bytes
// so trivial, it does not need comments... but i try...
//	g++ -O2 -DSINGLE_INST -DRICOH2A03 -I.. -o main000.exe main000.cpp cpu6502.c
//	oscar64.exe -Os -DSINGLE_INST -DRICOH2A03 -i=.. -o=main000.prg main000.cpp cpu6502.c

#include "main.h"

#ifdef __OSCAR64C__
	#define ram ((u8*)0)			// the only time in computing history a NULLPTR is a valid ptr ;-)
#else
	static u8 ram[65536] = {};		// easy in modern times
#endif

int main() {						// start
	LOADROMS						// load roms
	cpu6502_init(ram, NULL, NULL);	// init cpu
	cpu6502_reset();				// reset cpu
	for (int i = 0; i < 20; i++)
		cpu6502_step();				// step cpu
	return 0;						// end
}
