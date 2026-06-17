// adds output and speed (and removes silly comments)
// start main001.exe on win emu to see output
// start main001.prg on c64 emu to see output
//	g++ -O2 -DDEBUG -DSINGLE_INST -DRICOH2A03 -I.. -o main001.exe main001.cpp cpu6502.c
//	oscar64.exe -O2 -DDEBUG -DSINGLE_INST -DRICOH2A03 -i=.. -o=main001.prg main001.cpp cpu6502.c

#include "main.h"

#ifdef __OSCAR64C__
	#define ram ((u8*)0) // the only time in computing history a NULLPTR is a valid ptr ;-)
#else
	static u8 ram[65536] = {};
#endif

int main() {
	LOADROMS
	ram[53272] = 23;
	cpu6502_init(ram, NULL, NULL);
	cpu6502_reset();
	for (int i = 0; i < 20; i++) {
		cpu6502_dump();
		cpu6502_step();
	}
	return 0;
}