/**
 * main001.cpp - 6502 benchmark (CUDA 100K + single-engine)
 *
 * Build modes (see Makefile):
 *   -DBENCH_CUDA100K -DNUMCPUS=100000  ->  mass-parallel CUDA benchmark
 *   -DBENCH_CUDA100K -DMAX              ->  11M instances x 2KB (RTX 4090 max)
 *   -DENGINE_T       ->  transistor-level (cpu6502T)
 *   -DENGINE_CUDA     ->  single CUDA (cpu6502CUDA)
 *   default           ->  instruction-level (cpu6502.c)
 */

#include <chrono>
using clk = std::chrono::high_resolution_clock;
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#ifdef BENCH_CUDA100K
	#include "cpu6502CUDA100k.h"
#else
	#include "main.h"
	#if defined(ENGINE_T)
		#define BUDGET 10000
	#elif defined(ENGINE_CUDA)
		#define BUDGET 1000
	#else
		#define BUDGET 10000000
	#endif
#endif

#include "asm6502.h"

typedef uint8_t u8;
typedef uint16_t u16;

#ifdef BENCH_CUDA100K
	static u8 ram[RAM_SIZE] = {};
#else
	static u8 ram[65536] = {};
#endif

static u8* branch(u8* a, const char* mnemonic, u8* target) {
	char buf[32];
	snprintf(buf, sizeof(buf), "%s $%04X", mnemonic, (unsigned)(target - ram));
	return asm6502(ram, a, buf);
}

static u8* idle_jmp(u8* a) {
	char buf[32];
	snprintf(buf, sizeof(buf), "jmp $%04X", (unsigned)(a - ram));
	return asm6502(ram, a, buf);
}

static u16 t_start, t_end;

static void test_idle() {
	t_start = 0x0400;
	u8* a = asm6502(ram, &ram[t_start], "jmp $0400");
	t_end = (u16)(a - ram);
}

#ifdef ULTRA
/* ULTRA: RAM_SIZE=4, idle JMP-self lives at $0000 (4C 00 00). */
static void test_idle_ultra() {
	t_start = 0x0000;
	u8* a = asm6502(ram, &ram[t_start], "jmp $0000");
	t_end = (u16)(a - ram);
}
#endif

static void test_counter() {
	t_start = 0x0300;
	u8* a = &ram[t_start];
	a = asm6502(ram, a, "ldx #$FF");
	u8* loop = a;
	a = asm6502(ram, a, "dex");
	a = branch(a, "bne", loop);
	a = idle_jmp(a);
	t_end = (u16)(a - ram);
}

static void test_fib20() {
	t_start = 0x0200;
	u8* a = &ram[t_start];
	a = asm6502(ram, a, "lda #$01");
	a = asm6502(ram, a, "sta $10");
	a = asm6502(ram, a, "sta $11");
	a = asm6502(ram, a, "lda #$14");
	a = asm6502(ram, a, "sta $12");
	u8* loop = a;
	a = asm6502(ram, a, "lda $11");
	a = asm6502(ram, a, "sta $13");
	a = asm6502(ram, a, "clc");
	a = asm6502(ram, a, "adc $10");
	a = asm6502(ram, a, "sta $11");
	a = asm6502(ram, a, "lda $13");
	a = asm6502(ram, a, "sta $10");
	a = asm6502(ram, a, "dec $12");
	a = branch(a, "bne", loop);
	a = idle_jmp(a);
	t_end = (u16)(a - ram);
}

static void test_memfill() {
	t_start = 0x0500;
	u8* a = &ram[t_start];
	a = asm6502(ram, a, "ldx #$00");
	a = asm6502(ram, a, "lda #$AA");
	u8* loop = a;
	a = asm6502(ram, a, "sta $0600,X");  /* fits in 2KB for MAX mode */
	a = asm6502(ram, a, "inx");
	a = branch(a, "bne", loop);
	a = idle_jmp(a);
	t_end = (u16)(a - ram);
}

static void test_copy() {
	t_start = 0x0600;
	u8* a = &ram[t_start];
	a = asm6502(ram, a, "ldy #$FF");
	u8* loop = a;
	a = asm6502(ram, a, "lda ($10),Y");
	a = asm6502(ram, a, "sta ($20),Y");
	a = asm6502(ram, a, "dey");
	a = branch(a, "bne", loop);
	a = idle_jmp(a);
	ram[0x10] = 0x80; ram[0x11] = 0x06;  /* $0680, fits in 2KB for MAX mode */
	ram[0x20] = 0x00; ram[0x21] = 0x07;  /* $0700, fits in 2KB for MAX mode */
	t_end = (u16)(a - ram);
}

static void test_multiply() {
	t_start = 0x0700;
	u8* a = &ram[t_start];
	a = asm6502(ram, a, "lda #$00");
	a = asm6502(ram, a, "sta $12");
	a = asm6502(ram, a, "sta $13");
	a = asm6502(ram, a, "ldx #$08");
	u8* loop = a;
	a = asm6502(ram, a, "lsr $10");
	u8* bcc_pos = a;
	a = asm6502(ram, a, "bcc $0000"); /* placeholder */
	a = asm6502(ram, a, "clc");
	a = asm6502(ram, a, "lda $12");
	a = asm6502(ram, a, "adc $11");
	a = asm6502(ram, a, "sta $12");
	a = asm6502(ram, a, "lda $13");
	a = asm6502(ram, a, "adc #$00");
	a = asm6502(ram, a, "sta $13");
	/* patch BCC to skip to here */
	{ int rel = (int)((a - ram) - ((bcc_pos - ram) + 2)); bcc_pos[1] = (u8)rel; }
	a = asm6502(ram, a, "asl $11");
	a = asm6502(ram, a, "dex");
	a = branch(a, "bne", loop);
	a = idle_jmp(a);
	ram[0x10] = 0x07;
	ram[0x11] = 0x09;
	t_end = (u16)(a - ram);
}

/* ================================================================ */
#ifdef BENCH_CUDA100K
/* === CUDA 100K mass-parallel benchmark === */

int main(int argc, char** argv) {
	int num_instances = (argc > 1) ? atoi(argv[1]) : NUMCPUS;
	int warmup_steps  = (argc > 2) ? atoi(argv[2]) : 100;
	int measure_steps = (argc > 3) ? atoi(argv[3]) : 10000;

#ifdef ULTRA
	printf("=== CUDA ULTRA 6502 Benchmark (full RAM on-chip shared) ===\n");
	printf("Instances: %d  RAM: %dB  Warmup: %d  Measure: %d\n\n", num_instances, RAM_SIZE, warmup_steps, measure_steps);
#else
	printf("=== CUDA 100K 6502 Benchmark ===\n");
	printf("Instances: %d  RAM: %dKB  Warmup: %d  Measure: %d\n\n", num_instances, RAM_SIZE >> 10, warmup_steps, measure_steps);
#endif

	cpu100k::CPU100k gpu(num_instances);
	gpu.init();

	struct W { const char* name; uint16_t pc; void (*test)(); };
#ifdef ULTRA
	/* ULTRA: RAM_SIZE=4 — only the idle JMP-self at $0000 fits ("just enough RAM for 1 test"). The 6 standard tests land at $0200+ which is out of range. */
	W w[] = {
		{"Idle JMP-self",   0x0000, test_idle_ultra},
	};
#else
	W w[] = {
		{"Counter DEX+BNE", 0x0300, test_counter},
		{"Fibonacci(20)",   0x0200, test_fib20},
		{"Idle JMP-self",   0x0400, test_idle},
		{"MemFill 256B",   0x0500, test_memfill},
		{"Copy LDAiy+STAiy", 0x0600, test_copy},
		{"Multiply 8bit",  0x0700, test_multiply},
	};
#endif

	printf("%-20s %10s %10s %10s %10s\n", "Workload", "Insns/GPU", "GIPS", "MIPS/inst", "Time(ms)");
	printf("--------------------------------------------------------------------------\n");

	for (auto& wl : w) {
		memset(ram, 0, sizeof(ram));
		wl.test();
		gpu.load_image_all(ram);
		gpu.set_all(0, 0, 0, cpu100k::FU | cpu100k::FI, 0xFD, wl.pc);
		gpu.step_all(warmup_steps);

		auto t0 = clk::now();
		gpu.step_all(measure_steps);
		auto t1 = clk::now();
		double secs = std::chrono::duration<double>(t1 - t0).count();

		long long total_insns = (long long)measure_steps * num_instances;
		double gips = total_insns / secs / 1e9;
		double mips_per = gips * 1000.0 / num_instances;

		printf("%-20s %10lld %10.4f %10.4f %10.4f\n", wl.name, total_insns, gips, mips_per, secs * 1000.0);
		printf("  Instance 0: PC=$%04X  halted=%d\n", gpu.get_PC(0), gpu.get_halted(0));
	}

	printf("\n");
	return 0;
}

#else
/* === Single-engine benchmark (6502 / 6502T / 6502CUDA) === */

int main() {
	printf("=== %s ===\n", ENGINE);
	printf("%-20s %12s %16s %10s %s\n", "Workload", "Insns", "MIPS", "Time(ms)", "Result");
	printf("--------------------------------------------------------------------\n");
	fflush(stdout);

	struct W { const char* name; uint16_t pc; void (*test)(); };
	W w[] = {
		{"Counter DEX+BNE", 0x0300, test_counter},
		{"Fibonacci(20)",   0x0200, test_fib20},
		{"Idle JMP-self",   0x0400, test_idle},
		{"MemFill 256B",   0x0500, test_memfill},
		{"Copy LDAiy+STAiy", 0x0600, test_copy},
		{"Multiply 8bit",  0x0700, test_multiply},
	};

	for (auto& wl : w) {
		memset(ram, 0, sizeof(ram));
		wl.test();

#if defined(ENGINE_T)
		CPU6502 cpu;
		cpu6502_init(&cpu, ram);
		WS
		cpu6502_reset(&cpu);
		cpu6502_set(&cpu, wl.pc, 0, 0, 0, 0xFD, 0x24);
#elif defined(ENGINE_CUDA)
		CPU6502 cpu;
		cpu6502_init(&cpu, ram);
		WS
		cpu6502_reset(&cpu);
		cpu.PC = wl.pc; cpu.halted = false;
#else
		CPU6502 cpu;
		cpu6502_init(&cpu, ram, NULL, NULL);
		cpu6502_reset(&cpu);
		cpu.PC = wl.pc; cpu.halted = false;
#endif

		auto t0 = clk::now();
		int64_t insns = 0;
		while (insns < BUDGET && !cpu.halted) { cpu6502_step(&cpu); insns++; }
		auto t1 = clk::now();
		double secs = std::chrono::duration<double>(t1 - t0).count();

		double mips = insns / secs / 1e6;
		/* PASS: budget ran out (idle loop reached) or halted at idle loop */
		const char* result = (insns >= BUDGET || (cpu.halted && cpu.PC == wl.pc)) ? "PASS" : "FAIL";
		printf("%-20s %12lld %16.6f %10.4f %s PC=$%04X\n", wl.name, (long long)insns, mips, secs * 1000.0, result, (unsigned)cpu.PC);
		fflush(stdout);
	}

	printf("\n");
	return 0;
}
#endif
