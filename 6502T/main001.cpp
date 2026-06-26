#include "main.h"

static u8 ram[65536] = {};

int main() {
	LOADROMS
	CPU6502 cpu;
#if defined(ENGINE_T) || defined(ENGINE_CUDA)
	cpu6502_init(&cpu, ram);
	WS
#else
	cpu6502_init(&cpu, ram, NULL, NULL);
#endif
	cpu6502_reset(&cpu);
	for (int i = 0; i < 20; i++) {
		cpu6502_dump(&cpu);
		cpu6502_step(&cpu);
	}
#ifdef ENGINE_T
	cpu6502T_debug_nodes();
#endif
	return 0;
}
