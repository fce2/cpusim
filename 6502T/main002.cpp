// "T" type

#include "cbm_petscii.h"
#include <chrono>
using clk = std::chrono::high_resolution_clock;
#include "main.h"

#ifndef BUDGET
	#if defined(ENGINE_CUDA)
		const int BUDGET = 100*1000;    /* CUDA: 100K cycles */
	#elif defined(ENGINE_T)
		const int BUDGET = 1000*1000;   /* T: 1M cycles (~3 min) */
	#else
		const int BUDGET = 10*1000*1000; /* plain: 10M cycles (~2 ms) */
	#endif
#endif

static u8 ram[65536] = {};
static int next_irq = 17000, irq_count = 0;

// not used by T and C variants
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

#if defined(ENGINE_T) || defined(ENGINE_CUDA)
static void progress_bar(int cycles, int budget, double secs) {
	int pct = cycles * 100 / budget;
	double mips = cycles / secs / 1e6;
	int bar_w = 30;
	int filled = pct * bar_w / 100;
	char bar[32];
	for (int i = 0; i < bar_w; i++) bar[i] = i < filled ? '#' : '-';
	bar[bar_w] = '\0';
	int eta = (int)((budget - cycles) / (cycles / secs));
	printf("\r  [%s] %3d%%  %7d/%d  %6.2f MIPS  ETA %3ds",
		bar, pct, cycles, budget, mips, eta);
	fflush(stdout);
}
#endif

int main() {
	LOADROMS
	ram[0x0000] = 0x2F;
	ram[0x0001] = 0x37;
	CPU6502 cpu;
#if defined(ENGINE_T) || defined(ENGINE_CUDA)
	cpu6502_init(&cpu, ram);
	WS
#else
	cpu6502_init(&cpu, ram, io_read, io_write);
	cpu6502_io_range(&cpu, 0xD0, 0xDF);
#endif
	auto t0 = clk::now();
	cpu6502_reset(&cpu);
	if (cpu.PC != 0xFCE2) { printf("FAIL: wrong reset vector PC=$%04X\n", cpu.PC); return 1; }
	int cycles = 0, insns = 0;
#if defined(ENGINE_T) || defined(ENGINE_CUDA)
	printf("[" ENGINE "] Running %d cycles...\n", BUDGET);
#endif
	while (cycles < BUDGET) {
		cycles += cpu6502_step(&cpu);
		insns++;
		if (cpu.PC == 0xE5CD) break;
		if (cycles >= next_irq) {
#if defined(ENGINE_T) || defined(ENGINE_CUDA)
			/* T/CUDA: IRQ is active-low level-triggered. Assert line low, step once to take the interrupt, then release the line high. */
			cpu6502_irq(&cpu, 0);
			cpu6502_step(&cpu);
			cpu6502_irq_release(&cpu);
#else
			/* Plain 6502: cpu6502_irq() fires the interrupt immediately, no line management needed. */
			cpu6502_irq(&cpu, 0);
#endif
			next_irq += 17000;
			irq_count++;
			WS
		}
		if (cpu.halted) { printf("HALTED\n"); break; }
#if defined(ENGINE_T) || defined(ENGINE_CUDA)
		if ((insns & 0xFFF) == 0) {
			double secs = std::chrono::duration<double>(clk::now() - t0).count();
			if (secs > 0.25) progress_bar(cycles, BUDGET, secs);
		}
#endif
	}
#if defined(ENGINE_T) || defined(ENGINE_CUDA)
	double secs_final = std::chrono::duration<double>(clk::now() - t0).count();
	progress_bar(cycles, BUDGET, secs_final);
	printf("\n");
#else
	double secs_final = std::chrono::duration<double>(clk::now() - t0).count();
#endif
	printf("took %.3lfms\n  %.8f MIPS\n  %.8f MCPS\n", secs_final*1000.0, insns/secs_final/1e6, cycles/secs_final/1e6);
	printf("%d cycles, %d IRQs\n", cycles, irq_count);
	cpu6502_dump(&cpu);
	printf("Jiffy: $%02X $%02X $%02X\n", ram[0xA2], ram[0xA1], ram[0xA0]);
	char line[128];
	for (int row = 0; row < 25; row++) {
		c64_screen_line(ram + 0x0400, row, line, sizeof(line));
		printf("%s\n", line);
		if (strstr(line, "READY")) break;
	}
}
