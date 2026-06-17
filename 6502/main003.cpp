// add (kind of) irqs

#include "cbm_petscii.h"
#include "main.h"

static u8 ram[65536] = {};
static int next_irq = 17000, irq_count = 0;

static u8 io_read(CPU6502*, u16 addr) {
	if (addr == 0xD011) return 0x1B;
	if (addr == 0xD012) return 0x33;
	if (addr == 0xD018) return 0x15;
	if (addr == 0xDC0D) return 0x81;
	if (addr >= 0xDC00 && addr <= 0xDC0F) return 0xFF;
	if (addr >= 0xDD00 && addr <= 0xDD0F) return 0x17;
	if (addr >= 0xD800) return 0x0E;
	return ram[addr];
}
static void io_write(CPU6502*, u16 addr, u8 val) { ram[addr] = val; }

int main() {
	LOADROMS
	ram[0x0000] = 0x2F;
	ram[0x0001] = 0x37;
	CPU6502 cpu;
	cpu6502_init(&cpu, ram, io_read, io_write);
	cpu6502_io_range(&cpu, 0xD0, 0xDF);
	cpu6502_reset(&cpu);
	if (cpu.PC != 0xFCE2) { printf("FAIL: wrong reset vector PC=$%04X\n", cpu.PC); return 1; }
	int cycles = 0;
	while (cycles < 10*1000*1000) {
		cycles += cpu6502_step(&cpu);
		if (cpu.PC == 0xE5CD) break;
		if (cycles >= next_irq) {
			cpu6502_irq(&cpu, 0);
			next_irq += 17000;
			irq_count++;
		}
		if (cpu.halted) { printf("HALTED\n"); break; }
	}
	printf("%d cycles, %d IRQs: PC=$%04X A=$%02X\n", cycles, irq_count, cpu.PC, cpu.A);
	printf("Jiffy: $%02X $%02X $%02X\n", ram[0xA2], ram[0xA1], ram[0xA0]);
	char line[128];
	for (int row = 0; row < 25; row++) {
		c64_screen_line(ram + 0x0400, row, line, sizeof(line));
		printf("%s\n", line);
		if (strstr(line, "READY")) break;
	}
}