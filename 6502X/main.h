/**
 * main.h - Engine selector for 6502X multi-engine benchmark
 *
 * Unified header covering all engine backends:
 * ENGINE_T		-> cpu6502T (transistor-level)
 * ENGINE_CUDA	-> cpu6502CUDA (CUDA transistor)
 * default		-> cpu6502.c (instruction-level)
 */

#ifndef _MAIN_H_
#define _MAIN_H_

#if defined(ENGINE_T)
	#include "cpu6502T.h"
	#define CPU6502				CPU6502T
	#define cpu6502_init		cpu6502T_init
	#define cpu6502_reset		cpu6502T_reset
	#define cpu6502_step		cpu6502T_step
	#define cpu6502_irq			cpu6502T_irq
	#define cpu6502_irq_release	cpu6502T_irq_release
	#define cpu6502_dump		cpu6502T_dump
	#define cpu6502_set			cpu6502T_set
	#define cpu6502_flush		cpu6502T_flush
	#define cpu6502_soft_reset	cpu6502T_soft_reset
	#define cpu6502_log_start	cpu6502T_log_start
	#define cpu6502_log_stop	cpu6502T_log_stop
	#define cpu6502_log_count	cpu6502T_log_count
	#define cpu6502_log_entries	cpu6502T_log_entries
	#define CPU6502_Cycle		CPU6502T_Cycle
	#define _CPU6502_H_
	static void write_stubs(u8 *ram) {
		ram[0xD011] = 0x1B; ram[0xD012] = 0x33;
		ram[0xD018] = 0x15; ram[0xDC0D] = 0x81;
		for (int i = 0xDC00; i <= 0xDC0F; i++) ram[i] = 0xFF;
		for (int i = 0xDD00; i <= 0xDD0F; i++) ram[i] = 0x17;
		for (int i = 0xD800; i < 0xDBE8; i++) ram[i] = 0x0E;
	}
	#define WS write_stubs(ram);
	#define ENGINE "6502T"
#elif defined(ENGINE_CUDA)
	#include "sim.h"
	#include "cpu6502CUDA.h"
	static inline void cuda_init(cpu6502cuda::CPU* c, uint8_t* ram) { c->init(ram); }
	static inline void cuda_reset(cpu6502cuda::CPU* c) { c->reset(); }
	static inline uint8_t cuda_step(cpu6502cuda::CPU* c) { return c->stepGPU_fast(); }
	static inline void cuda_irq(cpu6502cuda::CPU* c, int force) { c->irq(force != 0); }
	static inline void cuda_irq_release(cpu6502cuda::CPU* c) { c->irq_release(); }
	static inline void cuda_dump(const cpu6502cuda::CPU* c) { printf("PC=%04X A=%02X X=%02X Y=%02X P=%02X SP=%02X\n", c->PC, c->A, c->X, c->Y, c->P, c->SP); }
	static inline void cuda_set(cpu6502cuda::CPU* c, uint16_t pc, uint8_t a, uint8_t x, uint8_t y, uint8_t sp, uint8_t p) { c->set(pc, a, x, y, sp, p); }
	static inline void cuda_flush(cpu6502cuda::CPU* c) { c->flush(); }
	static inline void cuda_soft_reset(cpu6502cuda::CPU* c) { c->softReset(); }
	static inline void cuda_log_start(cpu6502cuda::CPU* c) { c->logStart(); }
	static inline void cuda_log_stop(cpu6502cuda::CPU* c) { c->logStop(); }
	static inline int cuda_log_count(cpu6502cuda::CPU* c) { return c->logCount(); }
	static inline const cpu6502cuda::CPU::Cycle* cuda_log_entries(cpu6502cuda::CPU* c) { return c->logEntries(); }
	#define CPU6502				cpu6502cuda::CPU
	#define cpu6502_init		cuda_init
	#define cpu6502_reset		cuda_reset
	#define cpu6502_step		cuda_step
	#define cpu6502_irq			cuda_irq
	#define cpu6502_irq_release	cuda_irq_release
	#define cpu6502_dump		cuda_dump
	#define cpu6502_set			cuda_set
	#define cpu6502_flush		cuda_flush
	#define cpu6502_soft_reset	cuda_soft_reset
	#define cpu6502_log_start	cuda_log_start
	#define cpu6502_log_stop	cuda_log_stop
	#define cpu6502_log_count	cuda_log_count
	#define cpu6502_log_entries	cuda_log_entries
	#define CPU6502_Cycle		cpu6502cuda::CPU::Cycle
	#define _CPU6502_H_
	static void write_stubs(u8 *ram) {
		ram[0xD011] = 0x1B; ram[0xD012] = 0x33;
		ram[0xD018] = 0x15; ram[0xDC0D] = 0x81;
		for (int i = 0xDC00; i <= 0xDC0F; i++) ram[i] = 0xFF;
		for (int i = 0xDD00; i <= 0xDD0F; i++) ram[i] = 0x17;
		for (int i = 0xD800; i < 0xDBE8; i++) ram[i] = 0x0E;
	}
	#define WS write_stubs(ram);
	#define ENGINE "6502C"
#else
	#include "cpu6502.h"
	#define WS
	#define ENGINE "6502"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void load(u8 *ram, const char* path, u16 addr) {
	FILE* f = fopen(path, "rb");
	if (!f) { fprintf(stderr, "Can't open %s\n", path); exit(1); }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	fread(&ram[addr], 1, (size_t)n, f);
	fclose(f);
}
#define LOADROMS load(ram, "basic_a000.rom", 0xA000); load(ram, "kernal_e000.rom", 0xE000);

#endif /* _MAIN_H_ */
