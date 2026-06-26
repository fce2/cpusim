#include "cpu6502M.h"
#include <stdio.h>
#include <stdlib.h>

static void load(uint8_t *ram, const char* path, uint16_t addr) {
	FILE* f = fopen(path, "rb");
	if (!f) { fprintf(stderr, "Can't open %s\n", path); exit(1); }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	fread(&ram[addr], 1, (size_t)n, f);
	fclose(f);
}

int main(int argc, char* argv[]) {
	CPU6502 cpu;
	uint8_t ram[65536] = {};
	load(ram, "basic_a000.rom",  0xA000);
	load(ram, "kernal_e000.rom", 0xE000);
	cpu.reset(ram);
	for (int a=0; a<20; a++)
	{
		printf("PC=%04X A=%02X X=%02X Y=%02X P=%02X SP=%02X\n", cpu.PC, cpu.A, cpu.X, cpu.Y, cpu.P, cpu.SP);
		cpu.step();
	}
    return 0;
}
