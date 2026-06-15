// no globals anymore

#include "main.h"

static u8 ram[65536] = {};

int main() {
	LOADROMS
	CPU6502 cpu;
	cpu6502_init(&cpu, ram, NULL, NULL);
	cpu6502_reset(&cpu);
	if (cpu.PC != 0xFCE2) { printf("FAIL: wrong reset vector\n"); return 1; }
	int cycles = 0, i = 0;
	for (; i < 20; i++) {
		cpu6502_dump(&cpu);
		cycles += cpu6502_step(&cpu);
	}
	printf("%d steps (%d cycles): PC=$%04X A=$%02X X=$%02X Y=$%02X SP=$%02X\n", i, cycles, cpu.PC, cpu.A, cpu.X, cpu.Y, cpu.SP);
	return 0;
}