// WDC 65816 test — basic instruction decode and register verification
// g++ -O2 -DDEBUG -DCOUNT_CYCLES -I.. -o main001.exe main001.cpp cpu65816.c

#include "cpu65816.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define RAM_SIZE (16 * 1024 * 1024)

static u8 *ram; // 16MB flat address space (heap-allocated)

static int passed = 0, failed = 0;

static void check(const char* name, u32 expected, u32 actual) {
	if (expected == actual) { passed++; }
	else { failed++; printf("FAIL %s: expected $%X, got $%X\n", name, expected, actual); }
}

int main() {
	printf("65816 instruction tests\n"); fflush(stdout);
	ram = new u8[RAM_SIZE];
	CPU65816 cpu;
	cpu65816_init(&cpu, ram, NULL, NULL);

	// Test 1: LDA immediate in emulation mode
	memset(ram, 0, RAM_SIZE);
	ram[0xFFFC] = 0x00; ram[0xFFFD] = 0x02; // reset vector → $0200
	ram[0x0200] = 0xA9; ram[0x0201] = 0x42; // LDA #$42
	ram[0x0202] = 0xDB; // STP
	cpu65816_reset(&cpu);
	while (!cpu.halted) cpu65816_step(&cpu);
	check("LDA_imm_emu_A", 0x42, cpu.A & 0xFF);

	// Test 2: LDX immediate
	memset(ram, 0, RAM_SIZE);
	ram[0xFFFC] = 0x00; ram[0xFFFD] = 0x02;
	ram[0x0200] = 0xA2; ram[0x0201] = 0xFF; // LDX #$FF
	ram[0x0202] = 0xDB; // STP
	cpu65816_init(&cpu, ram, NULL, NULL);
	cpu65816_reset(&cpu);
	while (!cpu.halted) cpu65816_step(&cpu);
	check("LDX_imm_emu_X", 0xFF, cpu.X & 0xFFFF);
	check("LDX_imm_emu_N_flag", 1, (cpu.P & 0x80) ? 1 : 0);

	// Test 3: TAX
	memset(ram, 0, RAM_SIZE);
	ram[0xFFFC] = 0x00; ram[0xFFFD] = 0x02;
	ram[0x0200] = 0xA9; ram[0x0201] = 0x7F; // LDA #$7F
	ram[0x0202] = 0xAA; // TAX
	ram[0x0203] = 0xDB; // STP
	cpu65816_init(&cpu, ram, NULL, NULL);
	cpu65816_reset(&cpu);
	while (!cpu.halted) cpu65816_step(&cpu);
	check("TAX_emu_X", 0x7F, cpu.X & 0xFF);

	// Test 4: CLC + XCE → native mode
	memset(ram, 0, RAM_SIZE);
	ram[0xFFFC] = 0x00; ram[0xFFFD] = 0x02;
	ram[0x0200] = 0x18; // CLC
	ram[0x0201] = 0xFB; // XCE (swap carry & E → native mode)
	ram[0x0202] = 0xDB; // STP
	cpu65816_init(&cpu, ram, NULL, NULL);
	cpu65816_reset(&cpu);
	while (!cpu.halted) cpu65816_step(&cpu);
	check("XCE_native_E", 0, cpu.E);

	// Test 5: REP + 16-bit LDA
	memset(ram, 0, RAM_SIZE);
	ram[0xFFFC] = 0x00; ram[0xFFFD] = 0x02;
	ram[0x0200] = 0x18; // CLC
	ram[0x0201] = 0xFB; // XCE → native
	ram[0x0202] = 0xC2; ram[0x0203] = 0x30; // REP #$30 (clear M and X → 16-bit)
	ram[0x0204] = 0xA9; ram[0x0205] = 0x34; // LDA #$1234 (16-bit)
	ram[0x0206] = 0x12; // (high byte)
	ram[0x0207] = 0xDB; // STP
	cpu65816_init(&cpu, ram, NULL, NULL);
	cpu65816_reset(&cpu);
	while (!cpu.halted) cpu65816_step(&cpu);
	check("LDA_16bit_A", 0x1234, cpu.A & 0xFFFF);

	// Test 6: PHA/PLA
	memset(ram, 0, RAM_SIZE);
	ram[0xFFFC] = 0x00; ram[0xFFFD] = 0x02;
	ram[0x0200] = 0xA9; ram[0x0201] = 0x55; // LDA #$55
	ram[0x0202] = 0x48; // PHA
	ram[0x0203] = 0xA9; ram[0x0204] = 0xAA; // LDA #$AA
	ram[0x0205] = 0x68; // PLA
	ram[0x0206] = 0xDB; // STP
	cpu65816_init(&cpu, ram, NULL, NULL);
	cpu65816_reset(&cpu);
	while (!cpu.halted) cpu65816_step(&cpu);
	check("PLA_pull_A", 0x55, cpu.A & 0xFF);

	// Test 7: DEX
	memset(ram, 0, RAM_SIZE);
	ram[0xFFFC] = 0x00; ram[0xFFFD] = 0x02;
	ram[0x0200] = 0xA2; ram[0x0201] = 0x09; // LDX #$09
	ram[0x0202] = 0xCA; // DEX
	ram[0x0203] = 0xDB; // STP
	cpu65816_init(&cpu, ram, NULL, NULL);
	cpu65816_reset(&cpu);
	while (!cpu.halted) cpu65816_step(&cpu);
	check("DEX_result", 0x08, cpu.X & 0xFF);
	check("DEX_Z_flag", 0, (cpu.P & 0x02) ? 1 : 0);

	// Test 8: BEQ taken
	memset(ram, 0, RAM_SIZE);
	ram[0xFFFC] = 0x00; ram[0xFFFD] = 0x02;
	ram[0x0200] = 0xA9; ram[0x0201] = 0x00; // LDA #$00
	ram[0x0202] = 0xF0; ram[0x0203] = 0x02; // BEQ +2
	ram[0x0204] = 0xA9; ram[0x0205] = 0xFF; // LDA #$FF (skipped)
	ram[0x0206] = 0xDB; // STP
	cpu65816_init(&cpu, ram, NULL, NULL);
	cpu65816_reset(&cpu);
	while (!cpu.halted) cpu65816_step(&cpu);
	check("BEQ_taken_A", 0x00, cpu.A & 0xFF);

	// Test 9: JMP absolute
	memset(ram, 0, RAM_SIZE);
	ram[0xFFFC] = 0x00; ram[0xFFFD] = 0x02;
	ram[0x0200] = 0x4C; ram[0x0201] = 0x10; ram[0x0202] = 0x02; // JMP $0210
	ram[0x0210] = 0xA9; ram[0x0211] = 0x77; // LDA #$77
	ram[0x0212] = 0xDB; // STP
	cpu65816_init(&cpu, ram, NULL, NULL);
	cpu65816_reset(&cpu);
	while (!cpu.halted) cpu65816_step(&cpu);
	check("JMP_abs_A", 0x77, cpu.A & 0xFF);

	delete[] ram;
	printf("\n=== %d passed, %d failed ===\n", passed, failed);
	return failed ? 1 : 0;
}
